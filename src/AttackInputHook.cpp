#include "PCH.h"
#include "AttackInputHook.h"

#include <atomic>
#include <cstring>

#include "Settings.h"
#include "ShoutChainEngine.h"
#include "Trace.h"

using namespace SKSE;
using namespace SKSE::log;

namespace ShoutMCO {
    namespace {
        bool IsRightAttack(const RE::ButtonEvent* a_event) {
            if (!a_event) return false;
            const char* userEvent = a_event->QUserEvent().c_str();
            // Right hand only. `Left Attack/Block` is also the shield-block control, and telling
            // a block from a left-hand attack needs the equipped gear, so swallowing it would eat
            // blocks to buy a chain direction whose event name (`attackStartLeftHand`) is
            // untested anyway.
            return userEvent && std::strcmp(userEvent, "Right Attack/Block") == 0;
        }

        using ProcessButton_t = void (*)(RE::AttackBlockHandler*, RE::ButtonEvent*, RE::PlayerControlsData*);
        ProcessButton_t g_originalProcessButton = nullptr;

        // The handler's own power-attack threshold, which already reflects the player's
        // `fInitialPowerAttackDelay`. Reading it beats inventing a constant that would disagree
        // with their game.
        float GameHoldThreshold(const RE::AttackBlockHandler* a_self) {
            return a_self ? a_self->initialPowerAttackDelay : 0.0f;
        }

        std::string_view DescribeEdge(const RE::ButtonEvent* a_event) {
            if (a_event->IsDown()) return "down"sv;
            // Matches what the engine now ACTS on: a zero-value event is a release whether or not
            // it carries a held duration, and `IsUp()` alone would label such a press "held" in the
            // trace while the code treated it as an up. An instrument that disagrees with the
            // decision it is recording is worse than no instrument.
            if (!a_event->IsPressed()) return "up"sv;
            return "held"sv;
        }

        void Hook_ProcessButton(RE::AttackBlockHandler* a_self, RE::ButtonEvent* a_event,
                                RE::PlayerControlsData* a_data) {
            if (IsRightAttack(a_event) &&
                ShoutChainEngine::OnAttackButton(*a_event, GameHoldThreshold(a_self))) {
                return;
            }

            // A FORWARDED PRESS CANNOT BE OBSERVED FROM OUTSIDE THIS DLL. During an exhale nothing
            // in the graph consumes an attack event (CONTEXT.md), so a press this hook forwarded, a
            // press it swallowed, and a press that was never delivered all produce the same nothing
            // -- in the game and in the trace alike. Telling them apart needs a marker on the path
            // actually taken, and this is that path: the branch where the engine declined the event
            // and the game's own handler receives it.
            //
            // `shout=` is on the line because the claim is specifically about a press made DURING
            // a live shout. A forward with no shout in flight evidences nothing, since the engine
            // declines that press switched on as readily as off -- and a marker that could not
            // tell the two apart would be an assertion rather than an instrument.
            //
            // It reads `IsShoutLive()` and NOT the engine's own `shoutActive`. `BeginShout` returns
            // early on an ordinary shout the engine holds nothing for -- no shout equipped, most
            // obviously -- and engine state then prints "shout inactive" hundreds of milliseconds
            // into a live exhale. A marker whose own field contradicts the thing it records is an
            // assertion rather than an instrument.
            //
            // Reported BEFORE the call, so a handler that crashes or never returns still leaves
            // the evidence that it was reached.
            if (IsRightAttack(a_event)) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> INPUT forwarded to the game ({} edge, shout {}, engine {}){}",
                      ShoutChainEngine::ElapsedMs(), DescribeEdge(a_event),
                      ShoutChainEngine::IsShoutLive() ? "LIVE"sv : "inactive"sv,
                      Settings::Snapshot()->enabled ? "enabled"sv : "disabled"sv,
                      g_originalProcessButton ? ""sv : " -- NO ORIGINAL HANDLER, PRESS DROPPED"sv);
            }

            if (g_originalProcessButton) {
                g_originalProcessButton(a_self, a_event, a_data);
            }
        }

        // `ProcessButton` only sees the down and up edges; the ongoing hold comes through here,
        // because `AttackBlockHandler` is a `HeldStateHandler`. Without this vfunc a power press
        // cannot be recognised until release, which is usually after the window has shut.
        using UpdateHeld_t = void (*)(RE::AttackBlockHandler*, const RE::ButtonEvent*);
        UpdateHeld_t g_originalUpdateHeld = nullptr;

        void Hook_UpdateHeldStateActive(RE::AttackBlockHandler* a_self, const RE::ButtonEvent* a_event) {
            if (IsRightAttack(a_event) &&
                ShoutChainEngine::OnAttackHold(*a_event, GameHoldThreshold(a_self))) {
                return;
            }

            if (g_originalUpdateHeld) {
                g_originalUpdateHeld(a_self, a_event);
            }
        }

        // Which movement controls are physically down. Kept here, updated from the raw stream,
        // because the engine needs the answer from inside a deferred task -- a moment at which
        // `PlayerControls::data.moveInputVec` has already been zeroed for the frame.
        //
        // ATOMICS, not the engine lock (EngineLock.h rule 4): written on the input
        // stream, read from deferred tasks and trace formatting on other threads -- but each flag
        // is an independent single fact about one physical control, never part of a multi-field
        // snapshot, so a lock would buy consistency nothing here reads for. Relaxed everywhere:
        // a reading one event stale is the same reading the old plain bools gave, minus the UB.
        struct MovementHeld {
            std::atomic<bool> forward{false};
            std::atomic<bool> back{false};
            std::atomic<bool> left{false};
            std::atomic<bool> right{false};
            std::atomic<bool> stick{false};  // gamepad "Move", analog, no up/down edge

            [[nodiscard]] bool Any() const {
                return forward.load(std::memory_order_relaxed) || back.load(std::memory_order_relaxed) ||
                       left.load(std::memory_order_relaxed) || right.load(std::memory_order_relaxed) ||
                       stick.load(std::memory_order_relaxed);
            }
        };

        MovementHeld g_movementHeld;

        // Matched on the user event, not on a key code, so a remapped binding is covered without
        // a key list of ours to drift out of date.
        bool TrackMovement(const RE::ButtonEvent* a_button) {
            auto* events = RE::UserEvents::GetSingleton();
            if (!events) return false;

            const auto  name = a_button->QUserEvent();
            const bool  down = a_button->IsPressed();  // down edge or still held
            MovementHeld& held = g_movementHeld;

            if (name == events->forward) {
                held.forward.store(down, std::memory_order_relaxed);
            } else if (name == events->back) {
                held.back.store(down, std::memory_order_relaxed);
            } else if (name == events->strafeLeft) {
                held.left.store(down, std::memory_order_relaxed);
            } else if (name == events->strafeRight) {
                held.right.store(down, std::memory_order_relaxed);
            } else {
                return false;
            }
            return true;
        }

        // One Click Power Attack owns its own key and never routes it through
        // `AttackBlockHandler`, so the only way to see it is the raw input stream. This sink
        // observes and never consumes -- an input sink cannot swallow anyway, and it does not
        // need to: OCPA's own attempt lands in the exhale state, where nothing consumes an attack
        // event.
        class InputWatcher : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            static InputWatcher* GetSingleton() {
                static InputWatcher singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const*              a_event,
                                                  RE::BSTEventSource<RE::InputEvent*>*) override {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;

                for (auto* event = *a_event; event; event = event->next) {
                    // The gamepad's movement is a thumbstick, which has no press or release to
                    // track -- only a magnitude, reported every frame it is off centre.
                    if (auto* thumb = event->AsThumbstickEvent()) {
                        // Only the left stick moves the character; the right one is the camera,
                        // and it reports through the same event type.
                        if (thumb->IsLeft()) {
                            g_movementHeld.stick.store(thumb->xValue != 0.0f || thumb->yValue != 0.0f,
                                                       std::memory_order_relaxed);
                        }
                        continue;
                    }

                    auto* button = event->AsButtonEvent();
                    if (!button) continue;

                    if (TrackMovement(button)) continue;
                    if (!button->IsDown()) continue;

                    auto code = static_cast<std::uint32_t>(button->GetIDCode());
                    // SKSE's numbering, which is also OCPA's: mouse buttons sit above the
                    // keyboard scan codes.
                    if (button->device.get() == RE::INPUT_DEVICE::kMouse) code += 256;

                    ShoutChainEngine::OnPowerAttackKey(code);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void AttackInputHook::Install() {
        REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_AttackBlockHandler[0]};

        g_originalProcessButton =
            reinterpret_cast<ProcessButton_t>(vtbl.write_vfunc(0x4, &Hook_ProcessButton));
        g_originalUpdateHeld =
            reinterpret_cast<UpdateHeld_t>(vtbl.write_vfunc(0x5, &Hook_UpdateHeldStateActive));

        log::info("[ShoutMCO] attack-input hooks installed");
    }

    void AttackInputHook::EnsureInputWatcher() {
        // Atomic, because the caller is `Observe` and `Observe` runs on many threads.
        // `exchange` first so exactly one thread proceeds to `AddEventSink` -- the plain bool this
        // replaces allowed two first-callers to both register, and a doubly-registered sink sees
        // every event twice. Rolled back if the manager does not exist yet, so a later event
        // retries.
        static std::atomic<bool> registered{false};
        if (registered.exchange(true, std::memory_order_acq_rel)) return;

        auto* manager = RE::BSInputDeviceManager::GetSingleton();
        if (!manager) {
            registered.store(false, std::memory_order_release);
            // Warned once, not once per attempt. The caller is every player animation event, so
            // a per-attempt line would bury the trace this plugin exists to produce.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                log::error("[ShoutMCO] no input device manager yet; retrying on later events");
            }
            return;
        }

        // Registered whatever the power source: the movement half of this sink is needed either
        // way, and it is the only reading of movement that holds up inside a deferred task.
        manager->AddEventSink(InputWatcher::GetSingleton());

        const auto  snapshot = Settings::Snapshot();
        const auto& settings = *snapshot;
        if (settings.KeyToPower() && settings.powerAttackKeycode > 0) {
            log::info("[ShoutMCO] watching input: movement, and power-attack key {}",
                      settings.powerAttackKeycode);
        } else {
            log::info("[ShoutMCO] watching input: movement only -- power presses come from the held "
                      "attack button, not a key");
        }
    }

    bool AttackInputHook::IsMovementInputHeld() { return g_movementHeld.Any(); }

    std::string AttackInputHook::MovementInputSummary() {
        auto*       controls = RE::PlayerControls::GetSingleton();
        const float x = controls ? controls->data.moveInputVec.x : 0.0f;
        const float y = controls ? controls->data.moveInputVec.y : 0.0f;
        const auto& h = g_movementHeld;
        return std::format("held=[{}{}{}{}{}] vec=({:.2f},{:.2f}) autoMove={}", h.forward ? "W" : "",
                           h.back ? "S" : "", h.left ? "A" : "", h.right ? "D" : "",
                           h.stick ? "stick" : "", x, y, controls && controls->data.autoMove);
    }
}
