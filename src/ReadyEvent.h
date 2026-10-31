#pragma once

#include <string_view>

// THE GRAPH'S "BACK IN READY" EVENT, NAMED ONCE.
//
// The engine sequenced every return to ready on `inRdy` -- the chained attack, the attack
// tracker's reset, the attack-queued release. `inRdy` is not a vanilla, MCO, BFCO or SBF event.
// It is raised by **Jumping Attack**, which appends it to the third-person ready state's
// enter-notify array in `1hm_behavior` (`#4873`). Every trace this project took had Jumping
// Attack installed, so the dependency was invisible. Without it the cut lands, the attack is
// owed, and the chain is dropped at `readyWindowMs` -- what a player on BFCO without Jumping
// Attack reported.
//
// `SBF_ReadyStart` is State Behavior Framework's entry annotation on that same state, in that
// same array, one slot after `inRdy`, and SBF is already a requirement (the shout ends at
// `SBF_ShoutStop`). Both frameworks' combo resets ride the array AHEAD of it -- MCO's PIE
// payloads at the front, BFCO's `@SGVI|MCO_nextattack|1` on the transition just before -- so the
// ordering the chain's index write depends on holds exactly as it did on `inRdy`.
namespace ShoutMCO {
    inline constexpr std::string_view kReadyEvent = "SBF_ReadyStart";
}
