#include "PCH.h"
#include "ShoutChainEngine.h"

#include <chrono>
#include <functional>
#include <string>

#include "AttackInputHook.h"
#include "Settings.h"
#include "ShoutInputHook.h"
#include "Trace.h"

using namespace SKSE;
using namespace SKSE::log;

namespace ShoutMCO {
    namespace {
        enum class AttackKind { kLight, kPower };

        // THIS COMMENT USED TO CLAIM THE STATE BELOW IS MAIN-THREAD-ONLY. IT IS NOT.
        //
        // A single chain trace carries six distinct thread ids on the `[%t]` field (measured
        // 2026-08-03, `T16-chain-installed-copy-2026-08-03.log`), so `Observe` is entered from
        // several threads. The deferred work still drains on the main thread via the SKSE task
        // interface, and that part of the original claim stands.
        //
        // Nothing in `ChainState` was made atomic by ticket 16: that is a pre-existing condition,
        // it is not what a shipping ticket should be widening into, and no misbehaviour has been
        // traced to it. What ticket 16 DID do is avoid adding to the problem -- the trace-liveness
        // flag and the "log this once" state it introduced are atomic, because a `std::string`
        // assigned from two threads is a heap race rather than a stale bool.
        //
        // Anyone hardening this properly should start here.
        struct ChainState {
            bool shoutActive = false;
            bool windowOpen = false;

            // The shout has committed -- `Voice_SpellFire_Event` has been seen, so the magic is
            // out and a cut still delivers it. Before that point an `inRdy` cannot mean the
            // shout ended, because the shout has not yet done anything to end.
            bool spellFired = false;
            // A shout that begins mid-combo owes us one ready pass: the graph tears the outgoing
            // MCO attack down first, and that teardown runs through `inRdy` (finding 12). Spent
            // once, so a genuine interruption after it still escapes.
            bool teardownReadyPending = false;

            // Three separate facts about one press, because they end at different moments.
            // Ownership lasts until the button comes up -- follow-ups to a down edge we ate
            // must keep being eaten, or the game's own handler sees a release with no press and
            // charges an attack of its own on top of the chained one. The pending press
            // outlives ownership: ADR-0003 buffers it "for the duration of the shout", so a
            // tap-and-release still fires when the window opens.
            bool       pressOwned = false;
            bool       pressPending = false;
            bool       pressResolved = false;
            double     pressedAtMs = 0.0;
            AttackKind pressKind = AttackKind::kLight;

            // Read at shout start, written back after the cut's ready pass has reset it.
            int resumeAttack = 0;
            int resumePowerAttack = 0;

            bool       awaitingReady = false;
            double     cutAtMs = 0.0;
            AttackKind pendingKind = AttackKind::kLight;

            // Movement taken away from the player for the length of a chained attack. Deliberately
            // NOT cleared by Reset(): a suppression outliving the state it was set from is exactly
            // the case that would strand the player, so only an explicit restore clears it.
            bool   movementSuppressed = false;
            double suppressedAtMs = 0.0;

            // Observation only. Armed when a chained attack goes out, so the trace carries a
            // velocity and displacement profile across the swing rather than one sample at the
            // start -- travel is what the bug is, and travel is a distance over time.
            bool         motionWatch = false;
            double       motionArmedAtMs = 0.0;
            RE::NiPoint3 motionOrigin{};
            float        motionPeakVelocity = 0.0f;

            void Reset() {
                const bool   suppressed = movementSuppressed;
                const double suppressedAt = suppressedAtMs;
                *this = ChainState{};
                // Carried across, so a reset cannot lose track of a suppression that is still in
                // force. The restore paths are the only thing allowed to clear it.
                movementSuppressed = suppressed;
                suppressedAtMs = suppressedAt;
            }
        };

        ChainState g_state;

        // MCO's attack state, kept apart from ChainState because it is not the shout's and must
        // survive `ChainState::Reset` -- which runs at shout start, exactly when a held press is
        // about to be decided on.
        bool g_mcoAttackLive = false;
        bool g_mcoSwingLanded = false;

        // Shout liveness FOR THE TRACE ONLY, read off the graph rather than off engine state.
        //
        // `g_state.shoutActive` is set in `BeginShout`, which does not run when `bEnabled = 0`. So
        // it reads false throughout a perfectly live shout in exactly the disabled case gate A12
        // exists to measure, and the forward-path marker printed "shout inactive" 385ms after
        // `Voice_SpellFire_Event` (observed 2026-08-03, trace T16-a12-marker-defect). A marker
        // whose own field contradicts the claim it supports is an assertion, not an instrument --
        // the error finding 16 was filed against, arrived at from a different direction.
        //
        // `BeginCastVoice` and `shoutStop` reach `Observe` whatever the engine is set to, so this
        // is correct with the engine off. It drives no decision; nothing reads it but the trace.
        //
        // Atomic because it is written from the graph path and read from the input path, which are
        // different threads -- see the note on `ChainState` above.
        std::atomic<bool> g_shoutLiveForTrace{false};

        std::chrono::steady_clock::time_point g_origin{};
        bool                                  g_haveOrigin = false;

        // Milliseconds since the first observed event. Relative time is what a trace is read
        // against -- wall clock says nothing about clip timing.
        double ElapsedMs() {
            const auto now = std::chrono::steady_clock::now();
            if (!g_haveOrigin) {
                g_origin = now;
                g_haveOrigin = true;
            }
            return std::chrono::duration<double, std::milli>(now - g_origin).count();
        }

        int GraphInt(RE::Actor* a_actor, const char* a_name) {
            std::int32_t out = -999;
            a_actor->GetGraphVariableInt(a_name, out);
            return static_cast<int>(out);
        }

        bool GraphBool(RE::Actor* a_actor, const char* a_name) {
            bool out = false;
            a_actor->GetGraphVariableBool(a_name, out);
            return out;
        }

        std::string GraphSummary(RE::Actor* a_actor) {
            return std::format(
                "MCO_currentattack={} MCO_nextattack={} MCO_currentpowerattack={} "
                "MCO_nextpowerattack={} MCO_IsInRecovery={} MCO_IsPowerAttacking={}",
                GraphInt(a_actor, "MCO_currentattack"), GraphInt(a_actor, "MCO_nextattack"),
                GraphInt(a_actor, "MCO_currentpowerattack"), GraphInt(a_actor, "MCO_nextpowerattack"),
                GraphBool(a_actor, "MCO_IsInRecovery"), GraphBool(a_actor, "MCO_IsPowerAttacking"));
        }

        std::string_view Describe(AttackKind a_kind) {
            return a_kind == AttackKind::kPower ? "power"sv : "light"sv;
        }

        // Never call into the graph from inside its own event dispatch. Everything the engine
        // emits is posted back through the task interface instead; the cost is one task drain,
        // measured at ~27ms end to end against ~10ms for firing in place (finding 7d).
        void Defer(RE::Actor* a_actor, std::function<void(RE::Actor*)> a_work) {
            auto* task = SKSE::GetTaskInterface();
            if (!task || !a_actor) return;

            const auto handle = a_actor->CreateRefHandle();
            task->AddTask([handle, work = std::move(a_work)]() {
                auto ref = handle.get();
                if (!ref) return;
                if (auto* actor = ref->As<RE::Actor>()) {
                    work(actor);
                }
            });
        }

        // Was there an attack for the shout to interrupt? Asked at shout start, because by the
        // time the teardown's `inRdy` arrives the graph has already reset every trace of it --
        // `MCO_nextattack` is back to 1 and the game's attack state is clear.
        //
        // Two independent readings, because the game's attack state and MCO's combo do not end
        // together: a combo still open past its first attack reads `MCO_nextattack > 1` long
        // after `meleeAttackState` has gone back to `kNone`, and leaving *that* state passes
        // through ready just the same.
        bool WasAttacking(RE::Actor* a_actor) {
            if (auto* state = a_actor->AsActorState();
                state && state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) {
                return true;
            }
            return GraphInt(a_actor, "MCO_currentattack") > 0 || GraphInt(a_actor, "MCO_nextattack") > 1 ||
                   GraphBool(a_actor, "MCO_IsInRecovery");
        }

        // What the ANIMATION is doing. These bits follow the locomotion state, so during a shout
        // -- which is not a locomotive state -- they read false even with a movement key held
        // down. That is why the first version of the moveStop stopgap never fired: by the time it
        // asked, the graph had left locomotion for the exhale (finding 13).
        bool IsMoving(RE::Actor* a_actor) {
            auto* state = a_actor->AsActorState();
            if (!state) return false;
            const auto& moving = state->actorState1;
            return moving.movingForward || moving.movingBack || moving.movingRight || moving.movingLeft;
        }

        // What the PLAYER is doing. Taken from the input watcher's own tracking of the movement
        // controls, NOT from `PlayerControls::data.moveInputVec`: that vector is live only during
        // the frame's input phase, and every chain reads it as (0.00,0.00) from inside the
        // deferred task, holding W or not (finding 14).
        bool HasMovementInput() {
            auto* controls = RE::PlayerControls::GetSingleton();
            return AttackInputHook::IsMovementInputHeld() || (controls && controls->data.autoMove);
        }

        std::string MovementSummary(RE::Actor* a_actor) {
            return std::format("{} animMoving={}", AttackInputHook::MovementInputSummary(),
                               IsMoving(a_actor));
        }

        // Animation-driven motion -- AMR-style root motion, which is what actually carries an MCO
        // attack forward. None of the movement readings above see it: it is not player input and it
        // is not the locomotion flags. `outVelocity` is the character controller's own answer, and
        // displacement from the armed origin is the only reading that measures the complaint
        // directly, since "runs forward" is a distance, not a state.
        float ControllerVelocity(RE::Actor* a_actor) {
            auto* controller = a_actor->GetCharController();
            return controller ? controller->outVelocity.Length3() : 0.0f;
        }

        std::string MotionSummary(RE::Actor* a_actor) {
            const float velocity = ControllerVelocity(a_actor);
            // Not `std::max`: `max` is a macro here, courtesy of the Windows headers.
            if (velocity > g_state.motionPeakVelocity) g_state.motionPeakVelocity = velocity;
            const float travelled =
                g_state.motionWatch ? a_actor->GetPosition().GetDistance(g_state.motionOrigin) : 0.0f;
            // The held reading rides along on every sample, because "a key held throughout" is a
            // claim about the whole swing and one reading at the edge cannot support it.
            return std::format(
                "animDriven={} moveAnimDriven={} bAnimationDriven={} vel={:.1f} travelled={:.1f} {}",
                a_actor->IsAnimationDriven(), a_actor->IsMovementAnimationDriven(),
                GraphBool(a_actor, "bAnimationDriven"), velocity, travelled,
                AttackInputHook::MovementInputSummary());
        }

        void ArmMotionWatch(RE::Actor* a_actor) {
            g_state.motionWatch = true;
            g_state.motionArmedAtMs = ElapsedMs();
            g_state.motionOrigin = a_actor->GetPosition();
            g_state.motionPeakVelocity = 0.0f;
        }

        void EndMotionWatch(RE::Actor* a_actor, std::string_view a_reason) {
            if (!g_state.motionWatch) return;
            const float travelled = a_actor->GetPosition().GetDistance(g_state.motionOrigin);
            SHOUTMCO_TRACE("[{:10.2f}] >>> MOTION end ({}): travelled {:.1f} units in {:.0f}ms, peak vel {:.1f}",
                  ElapsedMs(), a_reason, travelled, ElapsedMs() - g_state.motionArmedAtMs,
                  g_state.motionPeakVelocity);
            g_state.motionWatch = false;
        }

        // THE ROOTING SEAM, AND WHY IT IS A CONSTANT RATHER THAN A SETTING.
        //
        // These were `bRootDuringChain` and `iRootWatchdogMs` in the INI until ticket 17
        // (2026-08-03). Finding 14 records the result of actually testing the feature on
        // 2026-07-29 20:29: it suppressed and restored cleanly 13 times out of 13, and it rooted
        // NOTHING the player could see -- while costing the moveset its direction, so a forward
        // press produced the BACK power attack. The INI's own comment ended up telling the player
        // to leave it at 0, which is not a setting, it is a recorded regression with a knob on it.
        //
        // The knob is gone. The seam stays, off, because deferred ticket 07 (root the inhale) is
        // its named future consumer and the three-restores design below is the part worth keeping.
        // Flipping this to `true` re-enables a known-broken behaviour -- read finding 14 first.
        constexpr bool kRootDuringChain = false;
        constexpr int  kRootWatchdogMs = 3000;

        // `moveStop` is a one-shot: it tells the graph to leave locomotion, and `PlayerControls`
        // puts it straight back on the next update while the key is still down. Taking the input
        // away for the length of the swing is the only way to hold the root without the C3 patch.
        //
        // Everything here treats a stuck player as the failure to design against, since being
        // unable to move is worse than the bug this fixes. Three independent restores: the attack
        // ending, the watchdog below, and a load.
        void SuppressMovement(std::string_view a_reason) {
            if (g_state.movementSuppressed) return;
            auto* controls = RE::ControlMap::GetSingleton();
            if (!controls) return;

            controls->ToggleControls(RE::ControlMap::UEFlag::kMovement, false, false);
            g_state.movementSuppressed = true;
            g_state.suppressedAtMs = ElapsedMs();
            SHOUTMCO_TRACE("[{:10.2f}] >>> ROOT movement suppressed ({})", ElapsedMs(), a_reason);
        }

        void RestoreMovement(std::string_view a_reason) {
            if (!g_state.movementSuppressed) return;
            g_state.movementSuppressed = false;
            auto* controls = RE::ControlMap::GetSingleton();
            if (controls) {
                controls->ToggleControls(RE::ControlMap::UEFlag::kMovement, true, false);
            }
            SHOUTMCO_TRACE("[{:10.2f}] >>> ROOT movement restored after {:.1f}ms ({})", ElapsedMs(),
                  ElapsedMs() - g_state.suppressedAtMs, a_reason);
        }

        // Runs on every graph event, so a restore that never arrives cannot outlive the cap. The
        // graph is noisy even standing still, which is what makes this a usable backstop rather
        // than something needing a timer of its own.
        void WatchdogRestore() {
            if (!g_state.movementSuppressed) return;
            const auto held = ElapsedMs() - g_state.suppressedAtMs;
            if (held > static_cast<double>(kRootWatchdogMs)) {
                RestoreMovement("watchdog -- no attack-end event arrived"sv);
            }
        }

        // MCO's attack, watched so a shout pressed mid-swing knows whether the swing has landed.
        //
        // Vanilla lets that shout start at once, and starting it tears the attack down ~3ms later
        // (finding 12) -- which mid-swing is the "power attack cancels and I run forward" report.
        //
        // THE GATE IS `HitFrame`, NOT MCO'S WINDOW. That was the first thing tried, on the
        // strength of finding 6's sequence, and measured 2026-08-02 it is simply wrong for this
        // attack: `MCO_PowerWinOpen` fires at +374ms and `weaponSwing` / `preHitFrame` /
        // `HitFrame` at ~+550ms, so releasing at the window still cut ~180ms before the swing.
        // Three trials produced a clean shout and no swing at all. The window says "you may queue
        // the next attack"; only the hit frame says "this one has landed", and the hit is what the
        // player is complaining about losing.
        void TrackMcoAttack(std::string_view a_tag) {
            if (a_tag == "MCO_AttackInitiate"sv || a_tag == "MCO_PowerAttackInitiate"sv) {
                g_mcoAttackLive = true;
                g_mcoSwingLanded = false;
                return;
            }

            // The swing has connected. Anything after this point cuts recovery, not the hit.
            if (a_tag == "HitFrame"sv) {
                g_mcoSwingLanded = true;
                ShoutInputHook::ReleaseHeldShout(a_tag);
                return;
            }

            // The attack ended without ever landing a hit -- interrupted, bashed, or a moveset
            // whose moving power attack has no swing in it at all (finding 16). The press is
            // still the player's, so it goes through here rather than being dropped.
            if (a_tag == "MCO_AttackExitNotify"sv || a_tag == "attackStop"sv || a_tag == "inRdy"sv) {
                g_mcoAttackLive = false;
                g_mcoSwingLanded = false;
                ShoutInputHook::ReleaseHeldShout(a_tag);
                return;
            }

            // Backstop, checked on every event: a press that is never released is a shout the
            // player pressed and did not get, which is worse than the bug this fixes.
            if (ShoutInputHook::IsHoldingShout()) {
                const auto waited = ElapsedMs() - ShoutInputHook::HeldSinceMs();
                if (waited > static_cast<double>(Settings::Get().shoutWaitCapMs)) {
                    ShoutInputHook::ReleaseHeldShout("cap -- no window and no attack end"sv);
                }
            }
        }

        // Step three: write the index back and fire the attack, in one task so nothing can slot
        // between them.
        void ResumeAndAttack(RE::Actor* a_actor, AttackKind a_kind) {
            const auto& settings = Settings::Get();

            const bool  power = a_kind == AttackKind::kPower;
            const char* var = power ? "MCO_nextpowerattack" : "MCO_nextattack";
            const int   index = power ? g_state.resumePowerAttack : g_state.resumeAttack;
            const std::string evt = power ? settings.powerAttackEvent : settings.attackEvent;
            // Movement is checked inside the task, not here: the actor can start or stop moving
            // in the ~7ms before it drains, and the graph's state at the moment the attack goes
            // out is what matters.
            const bool root = power && kRootDuringChain;
            const bool  resume = settings.resumeMode != Settings::ResumeMode::kOff && index > 0;

            Defer(a_actor, [var, index, evt, resume, root, a_kind](RE::Actor* actor) {
                if (resume) {
                    actor->SetGraphVariableInt(var, index);
                    SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN wrote {}={}  || {}", ElapsedMs(), var, index,
                          GraphSummary(actor));
                }

                // The player's INPUT, not the animation's flags: the shout is not a locomotive
                // state, so `actorState1.moving*` reads false with a key still held (finding 13).
                // Logged on every chain, moving or not, because it is the reading D6 was settled
                // on and the two it replaced were both silently blind.
                const bool moving = HasMovementInput() || IsMoving(actor);
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN movement at the attack edge: {}  || {}", ElapsedMs(),
                      moving ? "moving"sv : "still"sv, MovementSummary(actor));

                // No `moveStop` is sent here, deliberately. It used to be, as a stopgap for a
                // moving power attack that played a run cycle instead of a swing -- but that
                // belonged to one moveset, not to this engine: it reproduced with no shout in the
                // path at all and vanished when the moveset was disabled (finding 16). Sending
                // `moveStop` made our chain differ from the game's own moving power attack rather
                // than agree with it, which is the wrong direction for a chain whose whole job is
                // to hand control back to MCO.

                // moveStop alone loses to a key that is still down -- PlayerControls re-enters
                // locomotion on its next update. Taking the input away holds until the swing ends.
                //
                // Held off by `kRootDuringChain`; the reasoning lives on that constant and in
                // finding 14, rather than being restated here.
                if (root && moving) {
                    SuppressMovement(MovementSummary(actor));
                }

                const bool accepted = actor->NotifyAnimationGraph(evt);
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN fired '{}' ({}) accepted={}  || {}", ElapsedMs(), evt,
                      Describe(a_kind), accepted, GraphSummary(actor));

                // Armed after the event, so the origin is where the swing starts from.
                ArmMotionWatch(actor);
                SHOUTMCO_TRACE("[{:10.2f}] >>> MOTION armed  || {}", ElapsedMs(), MotionSummary(actor));
            });
        }

        // Step one: cut the shout. `Voice_SpellFire_Event` has already fired by the time any
        // window opens, so the shout still delivers its magic (finding 1).
        void FireChain(RE::Actor* a_actor, AttackKind a_kind) {
            const auto& settings = Settings::Get();

            g_state.shoutActive = false;  // the cut ends the shout; no natural shoutStop follows
            // ...and because no natural `shoutStop` follows, `Observe` never gets the event that
            // would clear the trace's liveness flag. Without this line it stays true after every
            // successful chain, and the next attack made nowhere near a shout logs `shout LIVE`.
            // Caught in cold review 2026-08-03; the comment directly above was the evidence.
            g_shoutLiveForTrace = false;
            g_state.windowOpen = false;
            g_state.pressPending = false;
            g_state.pressResolved = false;
            g_state.pendingKind = a_kind;
            g_state.awaitingReady = true;
            g_state.cutAtMs = ElapsedMs();

            const std::string cut = settings.cutEvent;
            Defer(a_actor, [cut](RE::Actor* actor) {
                const bool accepted = actor->NotifyAnimationGraph(cut);
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN cut '{}' accepted={}  || {}", ElapsedMs(), cut,
                      accepted, GraphSummary(actor));
            });
        }

        // The chain needs three things at once: a press, a decision about what kind of press it
        // was, and an open window. They can arrive in any order, so every one of them ends here
        // rather than each firing the chain itself.
        void TryFireChain(RE::Actor* a_actor) {
            if (!g_state.pressPending || !g_state.pressResolved) return;

            const auto& settings = Settings::Get();
            const auto  waited = ElapsedMs() - g_state.pressedAtMs;
            if (settings.bufferMs > 0 && waited > static_cast<double>(settings.bufferMs)) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> BUFFER expired after {:.1f}ms (cap {}ms)", ElapsedMs(), waited,
                      settings.bufferMs);
                g_state.pressPending = false;
                return;
            }

            if (g_state.shoutActive) {
                if (!g_state.windowOpen) return;  // hold it until the window opens
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN from a press {:.1f}ms old ({})", ElapsedMs(), waited,
                      Describe(g_state.pressKind));
                FireChain(a_actor, g_state.pressKind);
                return;
            }

            // The shout ended before this press resolved. We swallowed it, so it would otherwise
            // vanish and the player would have pressed attack for nothing. There is no shout
            // left to cut and the graph is already in ready -- past its own reset -- so the
            // attack goes out on its own.
            SHOUTMCO_TRACE("[{:10.2f}] >>> ATTACK without a cut, press {:.1f}ms old ({}) -- the shout had "
                  "already ended", ElapsedMs(), waited, Describe(g_state.pressKind));
            g_state.pressPending = false;
            ResumeAndAttack(a_actor, g_state.pressKind);
        }

        // Shared by the hold path (`UpdateHeldStateActive`) and any held event that does reach
        // `ProcessButton`.
        void ResolveAsPowerIfHeldEnough(RE::Actor* a_actor, float a_heldSeconds, float a_gameThreshold) {
            const auto& settings = Settings::Get();
            // `pressPending` as well as `pressResolved`: once the chain has fired, further hold
            // events are not a fresh decision to make.
            if (!g_state.pressPending || g_state.pressResolved || !settings.HoldToPower()) return;

            // The player's own threshold unless explicitly overridden. A constant of our own
            // would disagree with their game whenever they have changed it.
            const float threshold = settings.powerHoldSeconds >= 0.0f  ? settings.powerHoldSeconds
                                    : a_gameThreshold > 0.0f           ? a_gameThreshold
                                                                       : 0.3f;
            if (a_heldSeconds < threshold) return;

            g_state.pressKind = AttackKind::kPower;
            g_state.pressResolved = true;
            SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS held to power ({:.2f}s >= {:.2f}s, {})", ElapsedMs(),
                  a_heldSeconds, threshold,
                  settings.powerHoldSeconds >= 0.0f ? "INI override"sv : "the game's own"sv);
            TryFireChain(a_actor);
        }

        void OpenWindow(RE::Actor* a_actor, std::string_view a_reason) {
            if (!g_state.shoutActive || g_state.windowOpen) return;
            g_state.windowOpen = true;
            SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW open ({})", ElapsedMs(), a_reason);
            TryFireChain(a_actor);
        }

        void BeginShout(RE::Actor* a_actor) {
            auto& settings = Settings::Get();
            if (settings.reloadPerShout) {
                Settings::Load();
                settings.Log();
            }
            if (!settings.enabled) return;

            AttackInputHook::EnsureInputWatcher();

            const bool wasArmed = g_state.awaitingReady;
            const bool ownedPress = g_state.pressOwned;
            g_state.Reset();
            // Ownership of a button that is still physically down survives the reset, or its
            // release would reach the game's handler with no matching press.
            g_state.pressOwned = ownedPress;
            if (wasArmed) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN dropped: a new shout began before `inRdy`", ElapsedMs());
            }

            g_state.shoutActive = true;
            g_state.teardownReadyPending = WasAttacking(a_actor);
            g_state.resumeAttack = settings.resumeMode == Settings::ResumeMode::kIncrementCurrent
                                       ? GraphInt(a_actor, "MCO_currentattack") + 1
                                       : GraphInt(a_actor, "MCO_nextattack");
            g_state.resumePowerAttack = settings.resumeMode == Settings::ResumeMode::kIncrementCurrent
                                            ? GraphInt(a_actor, "MCO_currentpowerattack") + 1
                                            : GraphInt(a_actor, "MCO_nextpowerattack");

            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT begin, resume attack={} power={}{}", ElapsedMs(),
                  g_state.resumeAttack, g_state.resumePowerAttack,
                  g_state.teardownReadyPending ? " (out of an attack: one ready pass owed)"sv : ""sv);
        }

        void EndShout(std::string_view a_reason) {
            // Cleared before the early return, and unconditionally: an interruption that leaves the
            // engine with nothing to say is still the end of the shout as far as the trace is
            // concerned. The `shoutStop` path clears it in `Observe` too (ahead of the enabled
            // gate, so it is right with the engine off); this covers the ready-state interruption,
            // and `FireChain` covers the engine's own cut.
            g_shoutLiveForTrace = false;
            if (!g_state.shoutActive && !g_state.pressPending) return;
            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT end, no chain ({})", ElapsedMs(), a_reason);
            g_state.shoutActive = false;
            g_state.windowOpen = false;
            g_state.spellFired = false;
            g_state.teardownReadyPending = false;
            g_state.pressPending = false;
            g_state.pressResolved = false;
        }

        // Step two. The reset the cut provokes arrives as a PIE payload just *before* `inRdy`
        // (finding 7d), so ordering against this event -- not against a clock -- is what makes
        // the write survive.
        void OnReady(RE::Actor* a_actor) {
            if (!g_state.awaitingReady) {
                // `inRdy` DOES fire inside a shout, in exactly one case: the shout began while an
                // MCO attack was live, and the graph tears that attack down on its way into the
                // exhale -- MCO_AttackExitNotify, attackStop, tailcombatState, inRdy, all within
                // ~3ms of BeginCastVoice and all *before* Voice_SpellFire_Event (finding 12).
                // That pass is the outgoing attack's, not the shout's, so spend the debt and
                // leave the shout live. This is the whole combo -> shout -> chain case, so
                // treating it as an interruption killed the feature the mod exists for.
                if (g_state.shoutActive && !g_state.spellFired && g_state.teardownReadyPending) {
                    g_state.teardownReadyPending = false;
                    SHOUTMCO_TRACE("[{:10.2f}] >>> READY pass belongs to the attack the shout cut short; "
                          "the shout is still live", ElapsedMs());
                    return;
                }
                // Otherwise ready means nothing is shouting, so anything still live here was
                // interrupted by something that never raised `shoutStop` -- a killmove, a
                // ragdoll, a graph reset. This is the escape that stops input being swallowed
                // forever.
                EndShout("graph returned to ready"sv);
                return;
            }

            const auto& settings = Settings::Get();
            const auto  since = ElapsedMs() - g_state.cutAtMs;
            g_state.awaitingReady = false;

            if (since > static_cast<double>(settings.readyWindowMs)) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN dropped: `inRdy` came {:.1f}ms after the cut (cap {}ms)",
                      ElapsedMs(), since, settings.readyWindowMs);
                return;
            }

            SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN ready {:.1f}ms after the cut", ElapsedMs(), since);
            ResumeAndAttack(a_actor, g_state.pendingKind);
        }

    }

    void ShoutChainEngine::Install() {
        Settings::Load();
        Settings::Get().Log();

        // Player only for MVP -- see CONTEXT.md, surface decision. Nothing below is
        // player-specific, so adding RE::VTABLE_Character[2] later is a registration change
        // rather than a rewrite.
        REL::Relocation<std::uintptr_t> vtblPC{RE::VTABLE_PlayerCharacter[2]};
        _originalPC = vtblPC.write_vfunc(0x1, ProcessEvent_PC);

        // A load carries no graph events across, so state left over from the session before it
        // -- an armed chain, an owned button -- would never be cleared by anything else.
        if (auto* messaging = SKSE::GetMessagingInterface()) {
            messaging->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
                if (!a_message) return;
                switch (a_message->type) {
                    case SKSE::MessagingInterface::kPreLoadGame:
                    case SKSE::MessagingInterface::kPostLoadGame:
                    case SKSE::MessagingInterface::kNewGame:
                        // Before the reset, which deliberately carries a suppression across: a
                        // load is the one moment where dropping it on the floor would leave the
                        // player unable to move with nothing left to restore it.
                        RestoreMovement("game load"sv);
                        g_state.Reset();
                        break;
                    default:
                        break;
                }
            });
        }

        log::info("[ShoutMCO] anim-event hook installed");
    }

    ShoutChainEngine::EventResult ShoutChainEngine::ProcessEvent_PC(
        RE::BSTEventSink<RE::BSAnimationGraphEvent>*   a_sink,
        RE::BSAnimationGraphEvent*                     a_event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource) {
        Observe(a_event);
        // Always forward. The engine acts by emitting events of its own and by swallowing
        // *input*; it never swallows a graph event, so no other mod's sink loses anything.
        return _originalPC(a_sink, a_event, a_eventSource);
    }

    bool ShoutChainEngine::IsNoise(std::string_view a_tag) {
        return a_tag == "SCAR_UpdateDummy"sv || a_tag == "RdyDummy"sv;
    }

    bool ShoutChainEngine::IsInteresting(std::string_view a_tag) {
        static constexpr std::string_view kNeedles[]{
            "hout"sv,    // shoutRelease, shoutStop, ShoutSprint*, shoutReleaseSlowTime
            "SHOUT_"sv,  // our own vocabulary, once the C3 patch declares it
            "Breath"sv,  // MT_BreathExhale*, CombatReady_BreathExhale*
            "Exhale"sv,  // CombatReady_ShoutExhaleMedium
            "Voice"sv,   // Voice_SpellFire_Event, BeginCastVoice
            "MCO_"sv,    // every MCO window / recovery / transition event
            "attack"sv,  // attackStart, attackStop
            "Attack"sv,  // AttackWinStart, MCO_AttackInitiate
            "CastOK"sv,  // CastOKStart / CastOKStop
            "Pie"sv,     // Payload Interpreter -- the live tag is "Pie", not "PIE"
            "ocomotion"sv,  // tailCombatLocomotion -- where the running bug lives (finding 13)
            "move"sv,       // moveStart / moveStop
            "Move"sv,
        };
        for (const auto needle : kNeedles) {
            if (a_tag.find(needle) != std::string_view::npos) return true;
        }
        return false;
    }

    void ShoutChainEngine::Observe(RE::BSAnimationGraphEvent* a_event) {
        if (!a_event || !a_event->holder) return;

        const char* rawTag = a_event->tag.c_str();
        if (!rawTag || !rawTag[0]) return;
        const std::string_view tag{rawTag};
        if (IsNoise(tag)) return;

        auto* holder = const_cast<RE::TESObjectREFR*>(a_event->holder);
        auto* actor = holder ? holder->As<RE::Actor>() : nullptr;
        if (!actor) return;

        // Registered here rather than only in `BeginShout`, because the control that says whether a
        // symptom belongs to this engine is a run with NO shout in it -- and a watcher that first
        // registers on a shout leaves exactly that run with no movement reading at all. The lazy
        // registration itself stays (a `kDataLoaded` listener did not run in this setup); it is only
        // the trigger that widens, from the first shout to the first player animation event. The
        // call is a single already-registered branch after the first.
        AttackInputHook::EnsureInputWatcher();

        const auto& settings = Settings::Get();

        // The hand-written cost guard that used to wrap this block is gone: `SHOUTMCO_TRACE` is a
        // macro and does not evaluate its arguments when tracing is off, so `GraphSummary`'s six
        // graph-variable reads no longer happen at `bTrace = 0`. This site was the ONLY one that
        // ever carried such a guard, which is exactly why the others were paying for summaries they
        // never logged -- one mechanism now, applied everywhere, rather than a rule and an exception.
        {
            const char*            rawPayload = a_event->payload.c_str();
            const std::string_view payload{rawPayload ? rawPayload : ""};
            if (IsInteresting(tag)) {
                SHOUTMCO_TRACE("[{:10.2f}] {}{}{}  || {}", ElapsedMs(), tag,
                               payload.empty() ? ""sv : " | "sv, payload, GraphSummary(actor));
            } else {
                SHOUTMCO_TRACE("[{:10.2f}] {}{}{}", ElapsedMs(), tag,
                               payload.empty() ? ""sv : " | "sv, payload);
            }
        }

        // Before anything else, and on every event: a suppression must not outlive the swing it
        // was taken out for, whatever else the engine decides about this event.
        WatchdogRestore();
        TrackMcoAttack(tag);
        if (tag == "MCO_AttackExitNotify"sv || tag == "attackStop"sv || tag == "inRdy"sv) {
            RestoreMovement(tag);
        }

        // The motion profile across a chained swing. Every event carries a sample, because the
        // travel accumulates between them and one reading at the start would show nothing.
        if (g_state.motionWatch) {
            if (ElapsedMs() - g_state.motionArmedAtMs > static_cast<double>(settings.motionWatchMs)) {
                EndMotionWatch(actor, "watch window elapsed"sv);
            } else {
                SHOUTMCO_TRACE("[{:10.2f}] >>> MOTION {}  || {}", ElapsedMs(), tag, MotionSummary(actor));
                if (tag == "MCO_AttackExitNotify"sv || tag == "attackStop"sv || tag == "inRdy"sv) {
                    EndMotionWatch(actor, tag);
                }
            }
        }

        // Maintained BEFORE the `enabled` gate below, so the trace's shout reading stays correct
        // with the engine switched off. See `g_shoutLiveForTrace`.
        if (tag == "BeginCastVoice"sv) {
            g_shoutLiveForTrace = true;
        } else if (tag == "shoutStop"sv) {
            g_shoutLiveForTrace = false;
        }

        if (tag == "BeginCastVoice"sv) {
            BeginShout(actor);
            return;
        }

        // `inRdy` is handled whatever the enabled state, so a toggle flipped mid-shout cannot
        // strand an armed chain.
        if (tag == "inRdy"sv) {
            OnReady(actor);
            return;
        }

        if (!settings.enabled) return;

        if (tag == "Voice_SpellFire_Event"sv) {
            // The magic has fired, so from here a cut still delivers the shout. In the interim
            // window source that is also when the window opens.
            g_state.spellFired = true;
            if (settings.windowSource == Settings::WindowSource::kSpellFire) {
                OpenWindow(actor, "Voice_SpellFire_Event"sv);
            }
        } else if (settings.windowSource == Settings::WindowSource::kGraph &&
                   tag == settings.windowEvent) {
            OpenWindow(actor, "graph trigger"sv);
        } else if (tag == "shoutStop"sv) {
            EndShout("shoutStop"sv);
        }
    }

    bool ShoutChainEngine::OnAttackButton(const RE::ButtonEvent& a_event, float a_gameHoldThreshold) {
        const auto& settings = Settings::Get();
        if (!settings.enabled) return false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        if (a_event.IsDown()) {
            if (!g_state.shoutActive) return false;

            g_state.pressOwned = true;
            g_state.pressPending = true;
            g_state.pressedAtMs = ElapsedMs();
            g_state.pressKind = AttackKind::kLight;
            // While the power press is a hold, this press is not yet a light attack: the game
            // itself only decides at the threshold or on release, and deciding earlier here is
            // what would make a power chain unreachable. With a dedicated power key there is
            // nothing to wait for, so a light attack stays crisp.
            g_state.pressResolved = !settings.HoldToPower();

            SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS taken (window {})", ElapsedMs(),
                  g_state.windowOpen ? "open"sv : "shut"sv);
            TryFireChain(player);
            return true;
        }

        // Follow-ups belong to us only if we took the down edge. A press that began before the
        // shout -- including a block being held -- is never touched.
        if (!g_state.pressOwned) return false;

        if (a_event.IsUp()) {
            g_state.pressOwned = false;
            // `pressPending` as well: a press whose chain has already fired is not still waiting
            // to be called light or heavy. Without it the release of a chained press logs
            // "released as light" *after* the attack went out, which reads in a trace as a second
            // press that never happened.
            if (g_state.pressPending && !g_state.pressResolved) {
                g_state.pressResolved = true;  // released short of the threshold: a light attack
                SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS released as light ({:.2f}s held)", ElapsedMs(),
                      a_event.HeldDuration());
                TryFireChain(player);
            }
            return true;
        }

        ResolveAsPowerIfHeldEnough(player, a_event.HeldDuration(), a_gameHoldThreshold);
        return true;
    }

    bool ShoutChainEngine::OnAttackHold(const RE::ButtonEvent& a_event, float a_gameHoldThreshold) {
        const auto& settings = Settings::Get();
        if (!settings.enabled || !g_state.pressOwned) return false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        ResolveAsPowerIfHeldEnough(player, a_event.HeldDuration(), a_gameHoldThreshold);
        // We own this press, so the game must not charge a power attack of its own from it.
        return true;
    }

    double ShoutChainEngine::ElapsedMs() { return ShoutMCO::ElapsedMs(); }

    bool ShoutChainEngine::IsShoutLive() { return g_shoutLiveForTrace; }

    ShoutChainEngine::ShoutVerdict ShoutChainEngine::ShouldHoldShout() {
        // Never asks about shout cooldown, and never asks whether the player *may* shout. Only
        // whether MCO is midway through a swing (ADR-0002: issue the attempt, let the game
        // refuse it).
        return ShoutVerdict{.hold = g_mcoAttackLive && !g_mcoSwingLanded, .attackLive = g_mcoAttackLive};
    }

    bool ShoutChainEngine::OnPowerAttackKey(std::uint32_t a_keycode) {
        const auto& settings = Settings::Get();
        if (!settings.KeyToPower() || settings.powerAttackKeycode <= 0) return false;
        if (a_keycode != static_cast<std::uint32_t>(settings.powerAttackKeycode)) return false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        // Logged on EVERY power press, including one with no shout anywhere near it. A power
        // attack taken outside a shout is the control that says whether a symptom belongs to this
        // engine at all, and a control carrying no movement reading of its own cannot evidence
        // "a movement key was held" -- it can only assert it in a header. Ticket 02 leaned on
        // exactly that control, so the instrument has to reach it.
        SHOUTMCO_TRACE("[{:10.2f}] >>> POWER KEY {} seen, shout {}  || {}", ElapsedMs(), a_keycode,
              g_state.shoutActive ? "active"sv : "inactive"sv, MovementSummary(player));

        if (!settings.enabled || !g_state.shoutActive) return false;

        // A dedicated power key is unambiguous the moment it goes down -- there is no hold to
        // wait out, which is why this direction can fire immediately where the hold path cannot.
        g_state.pressPending = true;
        g_state.pressResolved = true;
        g_state.pressedAtMs = ElapsedMs();
        g_state.pressKind = AttackKind::kPower;

        SHOUTMCO_TRACE("[{:10.2f}] >>> POWER KEY {} (window {})", ElapsedMs(), a_keycode,
              g_state.windowOpen ? "open"sv : "shut"sv);
        TryFireChain(player);
        return true;
    }
}
