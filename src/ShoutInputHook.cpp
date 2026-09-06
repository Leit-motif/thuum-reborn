#include "PCH.h"
#include "ShoutInputHook.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "CastIntentApi.h"
#include "EngineLock.h"
#include "Settings.h"
#include "ShoutChainEngine.h"
#include "Trace.h"

using namespace SKSE;
using namespace SKSE::log;

namespace ShoutMCO {
    namespace {
        using ProcessButton_t = void (*)(RE::ShoutHandler*, RE::ButtonEvent*, RE::PlayerControlsData*);
        ProcessButton_t g_originalProcessButton = nullptr;

        bool IsShout(const RE::ButtonEvent* a_event) {
            if (!a_event) return false;
            auto* events = RE::UserEvents::GetSingleton();
            if (!events) return false;
            // Matched on the user event, so a remapped shout key or a gamepad binding is covered
            // without a key list of ours to drift.
            return a_event->QUserEvent() == events->shout;
        }

        // Everything needed to hand a held press to the game later. The handler and the controls
        // data are the game's own -- borrowed from the last real call rather than invented,
        // because a synthesized `PlayerControlsData` would be a guess about a struct we do not
        // own.
        //
        // GUARDED BY `detail::g_engineLock`. The identity fields are written from the
        // input path and snapshotted from the graph path; the five of them are ONE press and are
        // now taken as one, under the lock -- the `userEvent` string in particular is a refcounted
        // `BSFixedString`, and a copy racing a concurrent assignment was a heap hazard, not a
        // stale read.
        struct HeldPress {
            bool   holding = false;          // we are swallowing shout input right now
            bool   buttonStillDown = false;  // the player has not released yet
            double heldSinceMs = 0.0;
            ShoutChainEngine::ShoutHoldReason reason =
                ShoutChainEngine::ShoutHoldReason::kNone;

            RE::ShoutHandler*       handler = nullptr;
            RE::PlayerControlsData* controls = nullptr;
            RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;
            std::uint32_t           idCode = 0;
            RE::BSFixedString       userEvent;

            void Clear() {
                holding = false;
                buttonStillDown = false;
                heldSinceMs = 0.0;
                reason = ShoutChainEngine::ShoutHoldReason::kNone;
            }
        };

        HeldPress g_held;

        // ONE REPLAY, FROM ITS DOWN TO ITS UP. Everything the release needs, frozen when the replay
        // was decided.
        //
        // It travels as a struct because the two events are now ~130ms apart and every field has to
        // mean the same thing at both ends. The button's identity used to be read out of `g_held`
        // at each event; that was correct while both rode one task drain, and is not now -- a new
        // shout press landing inside the tap overwrites those fields, and the up would be built
        // from a different press than its own down.
        //
        // `handler` and `controls` are the game's own, borrowed from the last real call. They
        // outlive the tap: `PlayerControls` owns its data and `ShoutHandler` is one of its
        // long-lived handlers, both singletons for the session.
        struct Replay {
            RE::ShoutHandler*       handler = nullptr;
            RE::PlayerControlsData* controls = nullptr;
            RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;
            std::uint32_t           idCode = 0;
            RE::BSFixedString       userEvent;

            // Which replay this is. A release checks it is still the current one before delivering.
            std::uint64_t generation = 0;
            // Where the down actually went out, so the release can report a real interval and a
            // real frame separation rather than the numbers we asked for.
            double        downAtMs = 0.0;
            std::uint32_t downFrame = 0;
        };

        // One allocation for the life of the process, re-initialised per event. `ButtonEvent`
        // objects are normally owned by the input dispatcher, so creating one per replay would
        // leak: nothing downstream frees it. Reusing a single event avoids the question, and
        // `ShoutHandler` has no reason to retain the pointer past the call. Only ever touched from
        // inside a task drain, i.e. on the main thread.
        RE::ButtonEvent* ReplayEvent(const Replay& a_replay, float a_value, float a_heldSecs) {
            static RE::ButtonEvent* event = RE::ButtonEvent::Create(
                RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString{""}, 0, 0.0f, 0.0f);
            if (!event) return nullptr;
            event->Init(a_replay.device, static_cast<std::int32_t>(a_replay.idCode), a_value,
                        a_heldSecs, a_replay.userEvent);
            return event;
        }

        // THE REPLAYED TAP MUST HAVE A LENGTH, AND THE UP MUST NOT RIDE THE DOWN'S TASK.
        //
        // Measured against the deployed build. A real tapped
        // shout runs `BeginCastVoice` -> `Voice_SpellFire_Event` at +205ms -> `shoutStop` at
        // +1145ms. The replay, which delivered its down and its up in the SAME task drain,
        // produced `BeginCastVoice` and then nothing at all: no spell fire, no exhale, no
        // `shoutStop`, forever. `ShoutHandler` charges from the down and works out the shout from
        // how long the button was held, so a zero-length charge in one frame is not a tap it can
        // resolve -- it simply stays charging.
        //
        // That is the whole of the bug. The shout never fires, and because no `shoutStop`
        // ever arrives the engine's `shoutActive` never clears, so every later attack press is
        // swallowed with the window shut and the attack button appears dead.
        //
        // 120ms is a tap the handler resolves, comfortably inside the 205ms the real one took to
        // reach its spell fire, and it is wall time rather than a frame count so it does not move
        // with the player's framerate.
        constexpr double kReplayTapMs = 120.0;

        // A RELEASE THAT NO LONGER OWNS THE BUTTON MUST NOT DELIVER.
        //
        // The tap is paced off a sleeping thread, so for ~130ms there is an up in flight that the
        // engine has already decided on and cannot recall. Three things can happen in that window,
        // and all three end with the same wrong outcome -- an up landing on a charge it did not
        // start:
        //
        //   - another queued shout is released, so a second replay's down goes out;
        //   - the PLAYER presses shout themselves, and their own down reaches the handler;
        //   - the game is loaded, and the handler's charge belongs to a different session.
        //
        // Every one of them bumps this counter, and a release delivers only if the number it
        // claimed at its own down is still the current one. It is the only thing shared between a
        // replay's two halves -- the button's identity travels in the `Replay` struct instead.
        std::atomic<std::uint64_t> g_replayGeneration{0};

        // Bumped from the input path, the graph path and the load listener, i.e. from several
        // threads. Atomic for that reason (EngineLock.h rule 4) -- an independent counter, not
        // part of any multi-field snapshot.
        std::uint64_t ClaimReplayGeneration() { return ++g_replayGeneration; }

        // MEASURED, AND IT SETTLES THE PREMISE: SKSE DRAINS TASKS ADDED DURING
        // A DRAIN IN THE SAME PASS.
        //
        // This used to be a task that re-queued itself until 120ms of wall time had elapsed,
        // written believing a task queued from inside a drain ran on the NEXT one. The instrumented
        // build reported the truth in one line:
        //
        //   >>> SHOUT replayed press (frame 11725)
        //   >>> SHOUT replayed release after a 120.0ms tap (1457275 task passes, frame 11725 -> 11726)
        //
        // 1.46 MILLION passes inside 120ms, and one frame. The loop was a busy-wait holding the
        // main thread for the whole tap -- on a game that had been rendering ~58fps a second
        // earlier, that is ~7 frames of the player's game spent spinning, and the trace shows a
        // 120ms hole with no graph events in it at all. See CONTEXT.md.
        //
        // No task-based scheme can fix it: while the main thread is inside the drain the frame
        // cannot advance, so "re-queue until the frame changes" would spin until the cap instead
        // of hanging up. The wait has to happen somewhere that is not the main thread, and the
        // delivery still has to happen on it -- which is exactly one sleeping thread that ends in
        // an `AddTask`.
        //
        // Detached, and one per replay rather than a standing worker: a replay happens at most
        // once per shout, the thread lives ~120ms, and a worker with a queue would be more shared
        // state than the thing it schedules. What that costs is a thread sleeping across a quit
        // issued inside the tap's 120ms; it touches nothing of the game's until it is back on the
        // main thread, and the alternative -- a standing thread -- is asleep across every quit
        // rather than that one.
        //
        // WALL TIME AND NOT A FRAME COUNT, which is a deviation from "a one-shot task armed from
        // a frame counter". The requirement is a duration: `ShoutHandler`
        // resolves the shout from how long the button was held, and the real tap it was measured
        // against took 205ms to reach its spell fire. A frame-counted wait would be ~33ms at 30fps
        // and ~250ms at 240, i.e. a different shout on different hardware. Landing on a later frame
        // is the OBSERVABLE CONSEQUENCE of waiting real time, not the thing being asked for -- which
        // is why the frame numbers are in the trace and not in the condition.
        void PostReplayUp(const Replay& a_replay) {
            const auto delay = std::chrono::milliseconds(static_cast<long long>(kReplayTapMs));

            std::thread([replay = a_replay, delay]() {
                std::this_thread::sleep_for(delay);

                auto* task = SKSE::GetTaskInterface();
                if (!task) {
                    // A down with no up leaves the player charging a shout they are not touching,
                    // which is the failure this whole path exists to prevent -- so it is an error
                    // and not a silent return, exactly like the missing-handler case above.
                    log::error("[ShoutMCO] no task interface for the replayed release -- the "
                               "synthesized press has no up behind it");
                    return;
                }
                task->AddTask([replay]() {
                    if (g_replayGeneration.load() != replay.generation) {
                        SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT replayed release dropped -- the button "
                              "has moved on (newer replay, a real press, or a load)",
                              ShoutChainEngine::ElapsedMs());
                        return;
                    }
                    // The held duration is the REAL elapsed time since the down went out, not the
                    // nominal tap length: it is what the handler reads to decide the shout, and a
                    // value that disagrees with the clock is the same class of lie as the
                    // same-frame up this replaces. Sleep granularity and the wait for the next
                    // drain both land here.
                    const auto now = ShoutChainEngine::ElapsedMs();
                    const auto heldMs = now - replay.downAtMs;
                    if (auto* up = ReplayEvent(replay, 0.0f, static_cast<float>(heldMs / 1000.0))) {
                        g_originalProcessButton(replay.handler, up, replay.controls);
                    }
                    SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT replayed release after a {:.1f}ms tap "
                          "(frame {} -> {})", now, heldMs, replay.downFrame,
                          ShoutChainEngine::FrameCount());
                });
            }).detach();
        }

        // The decision, kept apart from the plumbing below it. REQUIRES the engine lock held --
        // it reads and writes the held press and, on a hold-start, takes the combo snapshot in
        // the same locked breath (press-taking and queue-noting are one decision).
        bool ShouldSwallowLocked(const RE::ButtonEvent& a_event, const Settings& a_settings,
                                 const ShoutChainEngine::ComboSnapshot& a_combo, bool a_haveCombo) {
            if (!a_settings.enabled || !a_settings.shoutWaitsForSwing) return false;

            if (g_held.holding) {
                // Already holding one back. Keep eating everything until it is released, or the
                // game's handler would see a release with no press behind it.
                //
                // `!IsPressed()` and NOT `IsUp()`. CommonLib defines `IsUp()` as
                // `value == 0 && heldDownSecs > 0`, so a release carrying a held duration of zero
                // is neither down nor up by that test -- it would be swallowed here and never
                // clear `buttonStillDown`, and the replay below would then hand the game a down
                // with no up behind it. A real key release always carries a duration; a
                // SYNTHESIZED one need not, and Spell Hotbar 2 emits exactly that when it drives a
                // shout from a hotbar key. Any zero-value event is the button coming up.
                //
                // A fresh DOWN while holding is replacement (one-entry cast intent):
                // newest valid input owns the pending slot. Refresh ownership so a tap-tap does
                // not leave `buttonStillDown` false from the prior release and synthesize an up
                // the player is no longer asking for.
                if (a_event.IsDown()) {
                    g_held.buttonStillDown = true;
                    g_held.heldSinceMs = ShoutChainEngine::ElapsedMs();
                    SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT held press replaced (newest cast intent)",
                          ShoutChainEngine::ElapsedMs());
                } else if (!a_event.IsPressed()) {
                    g_held.buttonStillDown = false;
                    SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT held press released while still waiting",
                          ShoutChainEngine::ElapsedMs());
                }
                return true;
            }

            // Only the down edge starts a hold. A press that is already through does not get
            // pulled back.
            if (!a_event.IsDown()) return false;

            const auto verdict = ShoutChainEngine::ShouldHoldShoutLocked();
            if (!verdict.hold) {
                // Nothing to queue behind -- no live MCO attack and no open shout chain window --
                // so the press is not ours to take.
                //
                // TRACED, BECAUSE THIS BRANCH USED TO SAY NOTHING AND THAT BROKE THIS FILE'S OWN
                // RULE: a press must not vanish with nothing in the trace. It left `>>> SHOUT KEY
                // down` as the last word on a press, which reads identically whether the engine
                // swallowed it and lost it or handed it straight to the game. A shout pressed 63 ms
                // into a stagger produced exactly that shape -- the game refused it, correctly, and
                // the log could not say so, which is a whole investigation to rule out a defect
                // that was never here.
                //
                // What this line means for a reader: from here the press belongs to the GAME. Any
                // reason it does not become a shout -- staggered, mid-killmove, on cooldown, out of
                // voice -- is vanilla's decision, and this engine never makes it (ADR-0002).
                SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT press forwarded -- nothing to wait behind "
                               "(attack live={}); from here the shout is the game's to allow",
                               ShoutChainEngine::ElapsedMs(), verdict.attackLive);
                return false;
            }

            // The player's press is an intent like any other, and it takes the ONE
            // slot -- displacing a driver's, which gets its single ABANDON/REPLACED. Done before
            // the fields below are written so the slot's owner and this hold are set together.
            CastIntentApi::VanillaPressTookSlot();

            g_held.holding = true;
            g_held.buttonStillDown = true;
            g_held.heldSinceMs = ShoutChainEngine::ElapsedMs();
            g_held.reason = verdict.reason;
            // Combo snapshot only when queuing behind an MCO attack. A shout→shout hold has no
            // attack counter to preserve; noting one would poison a later resume.
            if (a_haveCombo &&
                verdict.reason == ShoutChainEngine::ShoutHoldReason::kBehindAttack) {
                ShoutChainEngine::NoteQueuedShoutLocked(a_combo);
            }
            if (verdict.reason == ShoutChainEngine::ShoutHoldReason::kBehindShout) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT queued behind live shout (window open)",
                      ShoutChainEngine::ElapsedMs());
            } else {
                SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT queued behind a live MCO attack (hit {})",
                      ShoutChainEngine::ElapsedMs(),
                      verdict.swingLanded ? "already landed"sv : "not yet landed"sv);
            }
            return true;
        }

        void Hook_ProcessButton(RE::ShoutHandler* a_self, RE::ButtonEvent* a_event,
                                RE::PlayerControlsData* a_data) {
            if (IsShout(a_event)) {
                const auto settings = Settings::Snapshot();

                // THE EARLIEST PROVEN-TO-RUN POINT FOR THE HOLD-THRESHOLD OVERRIDE, and the reason
                // it is here as well as at the end of `Settings::Load()`.
                //
                // `fShoutTime1` decides word count from the DOWN edge onwards, and this hook is on
                // that edge -- ahead of `ShoutHandler`, ahead of `BeginCastVoice`, and ahead of any
                // comparison the game makes against the threshold. `Load()` at `BeginCastVoice`
                // would very probably be early enough on its own (press to `BeginCastVoice` is
                // ~0 ms), but "very probably" is not a sequencing argument and
                // this call costs two hash lookups and two float compares on the pass where the
                // GMSTs already agree.
                //
                // NOT a `Load()`: this must not read the INI from disk on the input thread. It
                // pushes the CURRENT snapshot, so an INI edit still takes effect at the next shout
                // start as the file documents -- this only guarantees the values already loaded are
                // in the game before the charge starts.
                //
                // Runs on every down edge, not only on ones the engine will act on: withdrawing an
                // override the player has since configured to 0 is part of what it does.
                if (a_event->IsDown()) {
                    Settings::ApplyHoldOverrides();

                    // The sprint reading, taken here for the same reason
                    // the override above is taken here: this edge runs ahead of `ShoutHandler`
                    // and ahead of `BeginCastVoice`, so it is the last moment the player's sprint
                    // is still the player's rather than something the cast has already undone.
                    //
                    // Runs on every down edge, like the override above, because the trace it writes
                    // is how a press the engine never acts on gets a sprint reading at all.
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        ShoutChainEngine::NoteShoutKeyDown(player);
                    }
                }

                // Phase A: the combo counters are graph reads and must happen before the lock.
                // Sampled on the down edge only -- that is the only edge that can start a hold --
                // and only when the engine could actually queue it, so a disabled engine pays
                // nothing per press.
                ShoutChainEngine::ComboSnapshot combo{};
                bool                            haveCombo = false;
                if (a_event->IsDown() && settings->enabled && settings->shoutWaitsForSwing) {
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        combo = ShoutChainEngine::SampleCombo(player);
                        haveCombo = true;
                    }
                }

                bool swallowed = false;
                {
                    std::scoped_lock lock(detail::g_engineLock);

                    // Remembered on every real shout event, held or not, so a replay always has a
                    // live handler and controls pointer to go with it.
                    g_held.handler = a_self;
                    g_held.controls = a_data;
                    g_held.device = a_event->device.get();
                    g_held.idCode = a_event->GetIDCode();
                    g_held.userEvent = a_event->QUserEvent();

                    swallowed = ShouldSwallowLocked(*a_event, *settings, combo, haveCombo);

                    // AFTER the swallow decision, and that ordering is the whole point.
                    //
                    // A down edge that reaches the game starts the player's own charge, so any
                    // release still sleeping belongs to a press that is now history and must not
                    // land on top of it. A SWALLOWED down starts nothing: we took it, the handler
                    // never saw it, and invalidating here would drop the in-flight up while
                    // delivering no down to replace it -- leaving the previous replay's charge
                    // open with nothing to end it, which is the original bug rebuilt from
                    // the other side.
                    //
                    // Our own replayed down cannot reach here either way; it goes straight to
                    // `g_originalProcessButton`.
                    if (!swallowed && a_event->IsDown()) {
                        ClaimReplayGeneration();
                        // And any queued snapshot is now an orphan: the shout about
                        // to start is the PLAYER's own press, not the queued one -- whose replay,
                        // if still in flight, just lost the button. Restoring the orphan's combo
                        // into this shout would be `fromQueue` with no queued press behind it.
                        ShoutChainEngine::AbandonQueuedResumeLocked(
                            "a real shout press took the button over"sv);
                    }
                }
                // Deliberately OUTSIDE the block above. A shout queued in a live
                // shout's window now cuts that shout's exhale instead of waiting it out, and the
                // cut notifies the graph -- which must not happen under the engine lock or
                // inside the graph's own dispatch. The engine rechecks every precondition, so an
                // unconditional call on the down edge is correct and cheap.
                if (swallowed && a_event->IsDown()) {
                    ShoutChainEngine::CutShoutForQueuedShoutChain();
                }
                if (swallowed) return;
            }

            if (g_originalProcessButton) {
                g_originalProcessButton(a_self, a_event, a_data);
            }
        }
    }

    void ShoutInputHook::Install() {
        REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_ShoutHandler[0]};
        g_originalProcessButton =
            reinterpret_cast<ProcessButton_t>(vtbl.write_vfunc(0x4, &Hook_ProcessButton));
        log::info("[ShoutMCO] shout-input hook installed");
    }

    bool ShoutInputHook::IsHoldingShoutLocked() { return g_held.holding; }
    // A DRIVER'S INTENT ANSWERS THESE TOO, and it must: the engine gates every release
    // on them, so a predicate that only knew about the vanilla press would let ANY confirmed state
    // release ANY pending intent. An intent deferred behind a live shout would then be released
    // early by an unrelated MCO attack's `inRdy` -- the reason each intent records what it is
    // actually waiting for.
    bool ShoutInputHook::IsHoldingBehindAttackLocked() {
        return IsVanillaHoldingBehindAttackLocked() ||
               CastIntentApi::DriverPendingBehindAttack();
    }
    bool ShoutInputHook::IsHoldingBehindShoutLocked() {
        return (g_held.holding &&
                g_held.reason == ShoutChainEngine::ShoutHoldReason::kBehindShout) ||
               CastIntentApi::DriverPendingBehindShout();
    }
    // The vanilla half on its own, for the cap that forces a release -- see the header
    // for why a driver intent must never reach it. Deliberately the SAME expression the predicate
    // above ORs, so the two cannot drift into disagreeing about what "behind an attack" means.
    bool ShoutInputHook::IsVanillaHoldingBehindAttackLocked() {
        return g_held.holding &&
               g_held.reason == ShoutChainEngine::ShoutHoldReason::kBehindAttack;
    }
    // Describes the VANILLA press only, and reads 0.0 when there is none. Every caller must have
    // established `IsVanillaHoldingBehindAttackLocked` first.
    double ShoutInputHook::HeldSinceMsLocked() { return g_held.heldSinceMs; }

    void ShoutInputHook::ClearHeldOnLoadLocked() {
        // BEFORE the early return below, because a driver intent and a held vanilla
        // press are alternatives: when a driver owns the slot there is no `g_held.holding`, and a
        // return taken first would carry its intent into a session it does not belong to.
        CastIntentApi::AbandonDriverIntent(SHOUTMCO_CAUSE_CONTEXT_LOST);
        CastIntentApi::VanillaPressLeftSlot();

        if (!g_held.holding) return;
        SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT held press dropped by game load",
              ShoutChainEngine::ElapsedMs());
        g_held.Clear();
    }

    void ShoutInputHook::AbandonHeldLocked(std::string_view a_reason) {
        // BEFORE the early return: the slot must stop reporting `kVanilla` the
        // moment the press it named ends, whichever of this function's several callers ended it.
        // Clears a vanilla occupant only, so the driver intent that displaced a press is never
        // wiped by that press's own abandonment -- which is how `Api_Request` can call this.
        CastIntentApi::VanillaPressLeftSlot();

        if (!g_held.holding) return;
        SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT held press abandoned ({})",
              ShoutChainEngine::ElapsedMs(), a_reason);
        g_held.Clear();
        // Any in-flight synthetic up belongs to this intent and must not land later.
        ClaimReplayGeneration();
        ShoutChainEngine::AbandonQueuedResumeLocked(a_reason);
    }

    void ShoutInputHook::AbandonSlotLocked(std::string_view a_reason) {
        // Both calls run unconditionally and the order is NOT load-bearing -- they are
        // two statements, not a guarded pair like `ClearHeldOnLoadLocked`'s, and only one of the two
        // occupants can be pending. The driver half is written first to match that function's shape
        // and to read in the order ADR-0008 describes the slot.
        CastIntentApi::AbandonDriverIntent(SHOUTMCO_CAUSE_CONTEXT_LOST);
        AbandonHeldLocked(a_reason);
    }

    void ShoutInputHook::ReleaseHeldShout(std::string_view a_reason) {
        // The decision and the snapshot, under the lock; the delivery, outside it. This is the
        // graph path reading and clearing state the input path writes, which is exactly the
        // cross-thread seam the engine lock exists for -- the whole press is frozen into `replay` in
        // one locked breath, and `g_held` no longer describes it from the moment the lock drops.
        // The confirmed state that releases a vanilla press releases a driver's intent
        // too -- one slot, one release policy (ADR-0008). Only one of the two can be pending, so
        // this is a no-op whenever the press below is the real occupant. Outside the lock, because
        // the engine lock must not be held across a dispatch.
        CastIntentApi::ReleaseDriverIntent();

        Replay replay{};
        bool   stillDown = false;
        double waited = 0.0;
        {
            std::scoped_lock lock(detail::g_engineLock);
            if (!g_held.holding) return;
            // The press is being handed back, so the slot is no longer the player's.
            CastIntentApi::VanillaPressLeftSlot();

            stillDown = g_held.buttonStillDown;
            waited = ShoutChainEngine::ElapsedMs() - g_held.heldSinceMs;

            replay = Replay{.handler = g_held.handler,
                            .controls = g_held.controls,
                            .device = g_held.device,
                            .idCode = g_held.idCode,
                            .userEvent = g_held.userEvent};

            // Cleared BEFORE the replay is posted, so the replayed event is not swallowed by the
            // very state that produced it.
            g_held.Clear();

            // This replay now owns the button. Any release still sleeping from an earlier one is
            // stale from here, and drops itself when it wakes. Claimed where the replay is
            // decided and carried in the struct -- reading the counter inside the release would
            // give two replays issued in one drain the same number, and then both ups would
            // deliver.
            //
            // Claimed INSIDE the lock, and that placement is load-bearing: a real press and this
            // release both claim under the lock, so
            // lock order IS claim order. Claimed outside it, this thread could be preempted
            // between clearing `g_held` and claiming -- a real press (or a load's invalidation)
            // claims in that gap, and this stale replay then claims a NEWER number, passes its
            // own generation check, and delivers a synthetic tap on top of the player's live
            // charge.
            replay.generation = ClaimReplayGeneration();
        }

        if (!replay.handler || !g_originalProcessButton) {
            log::error("[ShoutMCO] shout held but no handler to give it back to -- dropping");
            return;
        }

        SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT released after {:.1f}ms ({}), player {} holding",
              ShoutChainEngine::ElapsedMs(), waited, a_reason,
              stillDown ? "still"sv : "no longer"sv);

        // On the main thread. The release is triggered from inside the animation-graph event
        // dispatch, which is not where the game's input handlers expect to be called from.
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            log::error("[ShoutMCO] no task interface to replay the held shout on -- dropping");
            return;
        }

        task->AddTask([replay, stillDown]() mutable {
            // The DOWN is checked too, not just the up. A load or a real press can land between
            // this task being queued and the drain that runs it, and a down that goes out after
            // that is a charge nothing will close: its own release would wake up stale and drop
            // itself, which is a swallowed press never handed back -- the thing this file exists
            // to prevent.
            if (g_replayGeneration.load() != replay.generation) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT replay abandoned before its press -- the button "
                      "moved on first", ShoutChainEngine::ElapsedMs());
                // The queued combo snapshot belonged to this press, and this press will never
                // become a shout -- whoever claimed the button (a real press, a load) has its own
                // path to a fresh snapshot or a live read.
                ShoutChainEngine::AbandonQueuedResume("replay abandoned before its press"sv);
                return;
            }

            // Stamped BEFORE the call, not after: the down goes out at this instant, and timing it
            // from the far side of the dispatch would make every reported tap short by however long
            // the handler took.
            replay.downAtMs = ShoutChainEngine::ElapsedMs();
            replay.downFrame = TraceEnabled() ? ShoutChainEngine::FrameCount() : 0u;

            // A fresh down edge, because `ShoutHandler` starts its charge on `IsDown` and would
            // never begin from a held event alone.
            if (auto* down = ReplayEvent(replay, 1.0f, 0.0f)) {
                g_originalProcessButton(replay.handler, down, replay.controls);
            }
            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT replayed press (frame {})", replay.downAtMs,
                  replay.downFrame);

            // If the player let go while we were holding the press, the shout has to be completed
            // for them -- a down with no up would leave them charging a shout they are not
            // touching. If they ARE still holding, nothing is synthesized: their own events now
            // flow through and the charge continues under their thumb, which is what makes a
            // multi-word shout still reachable through this path.
            if (!stillDown) {
                PostReplayUp(replay);
            }
        });
    }

    void ShoutInputHook::InvalidateReplays() { ClaimReplayGeneration(); }
}
