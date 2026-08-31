#pragma once
#include "PCH.h"

namespace ShoutMCO {
    // `ShoutHandler::ProcessButton`, so a shout pressed partway through an MCO attack can be held
    // back until MCO's own window says the swing is done.
    //
    // This is the mirror of `AttackInputHook`, which swallows attack input during a shout. Same
    // vfunc trick, opposite direction, and the same rule: the original is always called when the
    // engine declines the event, so nothing else hooking this handler loses input.
    //
    // What it is NOT: a re-entry into the inhale state. That is still C3's job, and it is what a
    // shout flowing *out of* a swing without cutting it would need. This only moves *when* the
    // ordinary shout starts, which is enough to stop a mid-swing press reading as a cancelled
    // power attack.
    class ShoutInputHook {
    public:
        static void Install();

        // Let a held shout through, from the graph side. Called when an attack-queued hold
        // becomes shout-admissible (`IsAttacking` false) -- and from the wait cap.
        // Locks the engine internally, so it must be called from UNLOCKED contexts only: the
        // engine's phase-C emissions, never from inside a locked decision block (EngineLock.h
        // rule 1).
        static void ReleaseHeldShout(std::string_view a_reason);

        // Is a press currently being held back, and since when? For the engine's own locked
        // decisions -- the refresh, the cancel gate, and the cap. All require
        // `detail::g_engineLock` held (EngineLock.h).
        [[nodiscard]] static bool IsHoldingShoutLocked();
        // Attack-queued vs shout-queued holds release at different confirmed states.
        // These answer for a DRIVER's pending intent too, because the release gate must
        // treat both occupants of the one slot the same way.
        [[nodiscard]] static bool IsHoldingBehindAttackLocked();
        [[nodiscard]] static bool IsHoldingBehindShoutLocked();

        // The distinction is the whole of that bug. The VANILLA press only, for the
        // one decision that is about the press rather than about the intent: the wait cap, which
        // forces a release so a physical button is never stranded. `HeldSinceMsLocked` describes
        // that press and nothing else -- it reads 0.0 when a DRIVER owns the slot, so a cap
        // computed from it against `IsHoldingBehindAttackLocked` measured a driver intent's wait
        // from process start, blew the cap on the first graph event after the intent was taken, and
        // released it on `preHitFrame` while claiming `SHOUTMCO_CAUSE_READY`. A driver intent has
        // no button to strand and its bound is `CastIntentApi::CheckWatchdog`, which ABANDONS at
        // the same cap as ADR-0008 requires.
        [[nodiscard]] static bool IsVanillaHoldingBehindAttackLocked();
        [[nodiscard]] static double HeldSinceMsLocked();

        // Drop a held press at a game load: the attack it queued behind belongs to the previous
        // session, and the release paths that would hand it back were just reset with the rest of
        // the engine state -- so keeping it would strand the shout button until the wait cap.
        // Requires `detail::g_engineLock` held; called from the engine's load listener.
        static void ClearHeldOnLoadLocked();

        // THE VANILLA PRESS ONLY. Ends a held press without replaying it, and logs the reason.
        // Requires `detail::g_engineLock` held.
        //
        // IT MUST STAY VANILLA-ONLY, AND ONE CALLER IS WHY. `Api_Request` calls this
        // to hand back the press it has just displaced, one line after taking the slot for the
        // driver; a version that also retired the driver's intent would retire the intent that call
        // had only just created. This is its ONLY direct caller outside
        // `AbandonSlotLocked` below -- every engine site that means "this intent is finished" says
        // so with the slot-wide call instead.
        static void AbandonHeldLocked(std::string_view a_reason);

        // End WHICHEVER occupant the single slot holds -- the player's held press or a
        // driver's pending intent (ADR-0008: one slot, one release policy). The abandon-side mirror
        // of `ReleaseHeldShout`, which already makes both calls on the release side; only one of the
        // two can be pending, so the other call is a no-op.
        //
        // Use this wherever the state an intent was waiting for has ended in a way it can never
        // arrive from. The alternative is not a wrong cast -- `CastIntentApi::CheckWatchdog` still
        // retires the intent exactly once -- but a LATE one, up to `shoutWaitCapMs` after the fact
        // and blaming a watchdog for a cut. Raises `SHOUTMCO_CAUSE_CONTEXT_LOST`; the cause is
        // fixed rather than a parameter because all three sites mean the same thing.
        //
        // Requires `detail::g_engineLock` held. Safe there: the retirement is atomic under the
        // slot's own mutex and the driver's callback is queued to the main thread, never run inline.
        static void AbandonSlotLocked(std::string_view a_reason);

        // Abandon any synthetic release still in flight. The replayed tap is paced off a sleeping
        // thread, so for ~130ms there is an up the engine has decided on and cannot recall; a load
        // that lands inside that window would take it into a session whose shout charge is not the
        // one it belongs to. Called from the engine's load listener, next to the state reset there.
        static void InvalidateReplays();
    };
}
