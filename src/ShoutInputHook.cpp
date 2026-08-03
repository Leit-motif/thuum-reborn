#include "PCH.h"
#include "ShoutInputHook.h"

#include <cstring>

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
        struct HeldPress {
            bool   holding = false;      // we are swallowing shout input right now
            bool   buttonStillDown = false;  // the player has not released yet
            double heldSinceMs = 0.0;

            RE::ShoutHandler*       handler = nullptr;
            RE::PlayerControlsData* controls = nullptr;
            RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;
            std::uint32_t           idCode = 0;
            RE::BSFixedString       userEvent;

            void Clear() {
                holding = false;
                buttonStillDown = false;
                heldSinceMs = 0.0;
            }
        };

        HeldPress g_held;

        // One allocation for the life of the process, re-initialised per replay. `ButtonEvent`
        // objects are normally owned by the input dispatcher, so creating one per replay would
        // leak: nothing downstream frees it. Reusing a single event avoids the question, and
        // `ShoutHandler` has no reason to retain the pointer past the call.
        RE::ButtonEvent* ReplayEvent(float a_value, float a_heldSecs) {
            static RE::ButtonEvent* event = RE::ButtonEvent::Create(
                RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString{""}, 0, 0.0f, 0.0f);
            if (!event) return nullptr;
            event->Init(g_held.device, static_cast<std::int32_t>(g_held.idCode), a_value, a_heldSecs,
                        g_held.userEvent);
            return event;
        }

        // The decision, kept apart from the plumbing below it.
        bool ShouldSwallow(const RE::ButtonEvent& a_event) {
            const auto& settings = Settings::Get();
            if (!settings.enabled || !settings.shoutWaitsForSwing) return false;

            if (g_held.holding) {
                // Already holding one back. Keep eating everything until it is released, or the
                // game's handler would see a release with no press behind it.
                if (a_event.IsUp()) {
                    g_held.buttonStillDown = false;
                    SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT held press released while still waiting",
                          ShoutChainEngine::ElapsedMs());
                }
                return true;
            }

            // Only the down edge starts a hold. A press that is already through does not get
            // pulled back.
            if (!a_event.IsDown()) return false;

            const auto verdict = ShoutChainEngine::ShouldHoldShout();
            if (!verdict.hold) {
                if (verdict.attackLive) {
                    // The interesting non-hold: an attack IS live and its hit frame has passed,
                    // so the shout goes straight through. That is the rule working, not the rule
                    // being skipped.
                    SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT allowed immediately -- the swing has already landed",
                          ShoutChainEngine::ElapsedMs());
                }
                return false;
            }

            g_held.holding = true;
            g_held.buttonStillDown = true;
            g_held.heldSinceMs = ShoutChainEngine::ElapsedMs();
            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT held: an MCO attack is live and its swing has not landed",
                  ShoutChainEngine::ElapsedMs());
            return true;
        }

        void Hook_ProcessButton(RE::ShoutHandler* a_self, RE::ButtonEvent* a_event,
                                RE::PlayerControlsData* a_data) {
            if (IsShout(a_event)) {
                // Remembered on every real shout event, held or not, so a replay always has a
                // live handler and controls pointer to go with it.
                g_held.handler = a_self;
                g_held.controls = a_data;
                g_held.device = a_event->device.get();
                g_held.idCode = a_event->GetIDCode();
                g_held.userEvent = a_event->QUserEvent();

                if (ShouldSwallow(*a_event)) return;
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

    bool ShoutInputHook::IsHoldingShout() { return g_held.holding; }
    double ShoutInputHook::HeldSinceMs() { return g_held.heldSinceMs; }

    void ShoutInputHook::ReleaseHeldShout(std::string_view a_reason) {
        if (!g_held.holding) return;

        const bool  stillDown = g_held.buttonStillDown;
        const auto  waited = ShoutChainEngine::ElapsedMs() - g_held.heldSinceMs;
        auto* const handler = g_held.handler;
        auto* const controls = g_held.controls;

        // Cleared BEFORE the replay is posted, so the replayed event is not swallowed by the very
        // state that produced it.
        g_held.Clear();

        if (!handler || !g_originalProcessButton) {
            log::error("[ShoutMCO] shout held but no handler to give it back to -- dropping");
            return;
        }

        SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT released after {:.1f}ms ({}), player {} holding",
              ShoutChainEngine::ElapsedMs(), waited, a_reason,
              stillDown ? "still"sv : "no longer"sv);

        // On the main thread. The release is triggered from inside the animation-graph event
        // dispatch, which is not where the game's input handlers expect to be called from.
        auto* task = SKSE::GetTaskInterface();
        if (!task) return;

        task->AddTask([handler, controls, stillDown]() {
            // A fresh down edge, because `ShoutHandler` starts its charge on `IsDown` and would
            // never begin from a held event alone.
            if (auto* down = ReplayEvent(1.0f, 0.0f)) {
                g_originalProcessButton(handler, down, controls);
            }
            // If the player let go while we were holding the press, the shout has to be completed
            // for them -- a down with no up would leave them charging a shout they are not
            // touching. If they ARE still holding, nothing is synthesized: their own events now
            // flow through and the charge continues under their thumb, which is what makes a
            // multi-word shout still reachable through this path.
            if (!stillDown) {
                if (auto* up = ReplayEvent(0.0f, 0.01f)) {
                    g_originalProcessButton(handler, up, controls);
                }
            }
        });
    }
}
