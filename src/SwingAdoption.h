#pragma once

// ADOPTING A SWING THE TRACKER NEVER SAW START.
//
// The attack tracker opens on `MCO_AttackInitiate` / `MCO_PowerAttackInitiate`. BFCO raises
// neither, so under BFCO a shout pressed mid-swing found the tracker shut and went straight to the
// game, which tore the swing down ~3 ms later. Every event AFTER the start -- `HitFrame`,
// `attackStop`, `inRdy` -- BFCO does raise, so the hit-frame cut and the `IsAttacking`-falls
// release already work there; only the start was missing.
//
// BFCO has no clean start event to key off instead. `BFCO_PlayerAttackStart` fires AFTER the hit,
// and `Bfco_AttackStartFX` / `preHitFrame` do not pair one-to-one with hits (613 and 824 against
// 627 HitFrames in one long BFCO play session). So the press asks the game directly, at the
// moment it happens: is the actor attacking, and how far into the swing is it?
//
// `attackState` answers the second half. Measured on BFCO in the same session: it reads 1 (draw) when
// a chained swing winds up and 3 (hit) from the `HitFrame` on, so a press can tell "still winding
// up -- wait for the hit" from "already landed -- cut now".
//
// MELEE STATES ONLY. Bash (6) and the bow and spell states (8+) also raise `IsAttacking`, and the
// MCO path never held a shout behind any of them; adopting them would put a wait in front of a
// bow draw that no player asked for.
//
// FREE OF `RE::` AND SKSE, like `AttackQueuedRelease.h`: the reads live in the caller, the rule
// lives here where a host test can reach it.

namespace ShoutMCO {
    // The two reads the press takes, before the engine lock (EngineLock.h rule 2).
    struct SwingReading {
        bool isAttacking = false;
        int  attackState = 0;  // RE::ATTACK_STATE_ENUM
    };

    enum class SwingAdoption {
        kNone,           // nothing to adopt: the tracker already has it, or no melee swing is up
        kBeforeHit,      // swing up, hit still to come -- the next `HitFrame` cuts it
        kAfterHit,       // swing already landed -- the next event cuts it
    };

    // RE::ATTACK_STATE_ENUM, mirrored so this header stays free of CommonLib.
    inline constexpr int kAttackStateDraw = 1;
    inline constexpr int kAttackStateSwing = 2;
    inline constexpr int kAttackStateHit = 3;
    inline constexpr int kAttackStateFollowThrough = 5;

    [[nodiscard]] inline SwingAdoption DecideSwingAdoption(bool a_trackerLive, SwingReading a_reading) {
        if (a_trackerLive || !a_reading.isAttacking) return SwingAdoption::kNone;
        const int s = a_reading.attackState;
        if (s == kAttackStateDraw || s == kAttackStateSwing) return SwingAdoption::kBeforeHit;
        if (s >= kAttackStateHit && s <= kAttackStateFollowThrough) return SwingAdoption::kAfterHit;
        return SwingAdoption::kNone;
    }
}
