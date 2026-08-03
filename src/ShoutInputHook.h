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

        // Let a held shout through, from the graph side. Called when MCO opens its window, when
        // the attack ends, and from the cap.
        static void ReleaseHeldShout(std::string_view a_reason);

        // Is a press currently being held back? For the trace, and so the cap has something to
        // check cheaply on every graph event.
        [[nodiscard]] static bool IsHoldingShout();
        [[nodiscard]] static double HeldSinceMs();
    };
}
