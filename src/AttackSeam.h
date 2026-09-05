#pragma once

#include <string_view>

// THE POWER-ATTACK SEAM, downstream of the key.
//
// Every power-attack source -- vanilla's held button, One Click Power Attack's dedicated key,
// MCO's directional variants -- differs only in how it decides "that press was a power attack".
// They all then START the attack the same way: an outgoing graph event on the player whose name
// begins `attackPowerStart` (`attackPowerStartInPlace`, `attackPowerStartForward`, ...). Matching
// the event instead of the key is what makes this engine source-agnostic; reading another mod's
// ini was the thing that made it brittle.
//
// GAME-FREE ON PURPOSE, so the match rule and the re-entrancy guard are host-testable
// (`tests/attack_seam_tests.cpp`). Nothing here includes a Skyrim header.

namespace ShoutMCO {
    // The prefix, not a list of names. A per-mod list is precisely the coupling this replaces,
    // and the directional variants are all `attackPowerStart` + a direction.
    //
    // `attackStart` (a light attack), `attackPowerStop`, `bashStart`, an empty name, and this
    // plugin's own `SH2_*`/`shout*` traffic are all false: only the START of a POWER attack is a
    // press this engine buffers.
    [[nodiscard]] constexpr bool IsPowerAttackStartEvent(std::string_view a_name) noexcept {
        return a_name.starts_with("attackPowerStart");
    }

    // RE-ENTRANCY. The chain's own replay sends `attackPowerStartInPlace` through
    // `NotifyAnimationGraph`, which is the very vfunc the seam hooks -- so without a marker the
    // replay would arrive back at the engine as a fresh power press and re-buffer itself.
    //
    // A thread-local flag rather than a member or an atomic: the guard and the call it guards are
    // one synchronous span on one thread (the deferred task), and a process-wide flag would let a
    // replay on the main thread mask a genuine press seen on another.
    class ScopedOwnEmit {
    public:
        ScopedOwnEmit() noexcept : _previous(Flag()) { Flag() = true; }
        // The PREVIOUS value is restored, not `false`. Nothing nests these today, but a guard that
        // clears a flag it did not set is a bug that only shows up once someone does.
        ~ScopedOwnEmit() noexcept { Flag() = _previous; }

        ScopedOwnEmit(const ScopedOwnEmit&) = delete;
        ScopedOwnEmit(ScopedOwnEmit&&) = delete;
        ScopedOwnEmit& operator=(const ScopedOwnEmit&) = delete;
        ScopedOwnEmit& operator=(ScopedOwnEmit&&) = delete;

        // True while this thread is inside one of the engine's own graph emissions.
        [[nodiscard]] static bool Active() noexcept { return Flag(); }

    private:
        const bool _previous;

        static bool& Flag() noexcept {
            thread_local bool active = false;
            return active;
        }
    };
}
