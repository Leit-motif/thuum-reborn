#pragma once
#include "PCH.h"

#include "CastIntentSlot.h"

namespace ShoutMCO {
    // The engine-facing half of the cast-intent driver API (ADR-0008). The public C
    // contract is `include/ShoutMCO_CastIntent.h`; the slot's rules are `CastIntentSlot.h`; this
    // is the part that knows about SKSE, the engine lock and the main thread.
    //
    // A DRIVER INTENT RIDES THE VANILLA PRESS'S SEAMS. It is not a second buffer with a second
    // release policy -- that is precisely what ADR-0008 rejected. `ShoutInputHook` already calls
    // exactly the four places an intent can end, and each one now retires the driver's too:
    // release at the confirmed state, abandon, game load, and replacement by a newer press.
    class CastIntentApi {
    public:
        // Emits the single startup status line. Called once from `plugin.cpp`.
        static void Install();

        // Is a DRIVER intent the current occupant of the one-entry slot?
        [[nodiscard]] static bool DriverPending();

        // ...and is it waiting on THIS confirmed state? The engine's release gate asks before it
        // lets a confirmed state through, exactly as it asks the input hook whether the held
        // vanilla press is behind an attack or behind a shout. Without these, any confirmed state
        // released any pending intent -- an intent deferred behind a live shout could be released
        // early by an unrelated MCO attack's `inRdy`.
        [[nodiscard]] static bool DriverPendingBehindAttack();
        [[nodiscard]] static bool DriverPendingBehindShout();

        // The confirmed graph state arrived. Retires a pending driver intent with RELEASE/READY
        // and dispatches its one callback on the main thread. No-op when the slot is empty or
        // holds a vanilla press.
        static void ReleaseDriverIntent();

        // Retire a pending driver intent without releasing it. `a_cause` reaches the driver
        // verbatim for its own logging.
        static void AbandonDriverIntent(ShoutMCO_CastCause a_cause);

        // The player's own shout press is taking the slot. Displaces a pending driver intent with
        // ABANDON/REPLACED -- newest valid intent wins, whoever owns it.
        static void VanillaPressTookSlot();

        // ...and the player's press has ended, so the slot is free again. Clears ONLY a vanilla
        // occupant, so a driver intent that displaced the press is not wiped by the press's own
        // departure. Every path that ends a held press calls this, or the slot keeps reporting
        // `kVanilla` with nothing actually held.
        static void VanillaPressLeftSlot();

        // Bounded watchdog. Called from the engine's existing watchdog pass; abandons a driver
        // intent that has waited past the cap. It never forces execution (ADR-0008).
        static void CheckWatchdog(std::uint32_t a_capMs);

        // Spend one notice. Public because the exported `Request` has to dispatch the notice it
        // displaced, and that call sits outside this class.
        //
        // Notices are always delivered on the main thread, via the task interface, and never
        // inline. A driver's callback may call `Request` again to chain, so calling one while
        // holding the slot's mutex or the engine lock would deadlock -- see the re-entrancy test
        // in `tests/cast_intent_tests.cpp`.
        static void Dispatch(const CastIntentNotice& a_notice);
    };
}

extern "C" __declspec(dllexport) const ShoutMCO_CastIntentApi* ShoutMCO_GetCastIntentApi(
    std::uint32_t a_requestedMajor);
