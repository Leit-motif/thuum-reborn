#pragma once

// ATTACK-QUEUED SHOUT RELEASE.
//
// A shout pressed during an MCO attack is held until the graph is shout-admissible.
// Releasing on `inRdy` is wrong: that tag arrives after cancel-at-hit with `IsAttacking`
// still true on both 1H and 2H; a synthetic tap there is silently refused, and the
// character hangs until sheathe. The event that actually precedes `BeginCastVoice` is
// the first later graph event whose sampled `IsAttacking` is false (often a second
// `attackStop`, measured on Iron Greatsword and Iron Sword).
//
// FREE OF `RE::` AND SKSE, for the same reason `ClipOwnedRelease.h` is: the rule has to
// be checkable without launching the game. Sampling `IsAttacking` is a graph read and
// lives in `ShoutChainEngine.cpp`. The arriving tag is the release *reason*, not a
// discriminator — 1H and 2H share this policy; there is no weapon family table.

namespace ShoutMCO {
    enum class AttackQueuedAction {
        kHold,
        kRelease,
        kCapRelease,
    };

    struct AttackQueuedView {
        bool holdingBehindAttack = false;
        bool isAttacking = false;
        bool capElapsed = false;
    };

    [[nodiscard]] inline AttackQueuedAction DecideAttackQueuedRelease(AttackQueuedView a_view) {
        if (!a_view.holdingBehindAttack) {
            return AttackQueuedAction::kHold;
        }
        // Shout-admissible: `IsAttacking` has fallen. The tag that carries this (often a
        // later `attackStop`, not cancel `inRdy`) is the caller's release reason.
        if (!a_view.isAttacking) {
            return AttackQueuedAction::kRelease;
        }
        if (a_view.capElapsed) {
            return AttackQueuedAction::kCapRelease;
        }
        return AttackQueuedAction::kHold;
    }
}
