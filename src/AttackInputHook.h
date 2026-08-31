#pragma once
#include "PCH.h"

namespace ShoutMCO {
    // `AttackBlockHandler::ProcessButton`, so attack input during a shout can be swallowed and
    // buffered rather than acted on. Modelled on MSCO's `attackhandler.cpp`, which locks the
    // same vfunc on a graph variable; ours locks on DLL state instead, because the shout lock
    // is owned by the DLL and needs no BDI-injected variable to exist.
    //
    // Only `Right Attack/Block` is ever offered to the engine. The original is always called
    // when the engine declines the event, so nothing else that hooks this vfunc loses input.
    class AttackInputHook {
    public:
        // Both `AttackBlockHandler` vfuncs: 0x4 `ProcessButton` for the press and release edges,
        // 0x5 `UpdateHeldStateActive` for the hold in between.
        static void Install();

        // The raw-input sink. Registered on the first shout rather than from an SKSE lifecycle
        // message: `BSInputDeviceManager` does not exist at plugin load, and a `kDataLoaded`
        // listener was observed not to run, so hanging the sink off a message is a
        // dependency with no payoff. Idempotent.
        //
        // Carries two jobs: One Click Power Attack's key, and movement input. It is registered
        // even with no OCPA key to watch, because the movement half is always needed.
        static void EnsureInputWatcher();

        // Is a movement control down RIGHT NOW? Tracked from the raw input stream rather than read
        // from `PlayerControls::data.moveInputVec`, which reads (0,0) from inside a deferred task
        // -- the vector is live only during the frame's input phase. Bindings come
        // from `UserEvents`, so a remapped key or a gamepad stick is covered without a key list of
        // our own.
        [[nodiscard]] static bool IsMovementInputHeld();

        // For the trace: which controls are down, and the vector that cannot be trusted.
        [[nodiscard]] static std::string MovementInputSummary();
    };
}
