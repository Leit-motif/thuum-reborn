#pragma once
#include "PCH.h"

namespace ShoutMCO {
    // Shout -> MCO attack chaining, direction one of three.
    //
    // The whole mechanism is two transitions that already exist in the vanilla graph, fired in
    // order: `shoutStop` returns the exhale to ready, and ready consumes `attackStart`
    // (CONTEXT.md finding 7c). Between them, sequenced on the `inRdy` the cut produces, the
    // combo index is written back -- the ready path's own PIE reset lands just before `inRdy`,
    // so anything after that event is already past it (finding 7d).
    //
    // No graph patch is involved, and the DLL never inspects shout cooldown state.
    //
    // Not implemented here, and deliberately not half-implemented: MCO attack -> shout and
    // shout -> shout. Both need a re-entry into the inhale state that no vanilla transition
    // provides, so both wait on the C3 Nemesis patch.
    class ShoutChainEngine {
    public:
        static void Install();

        // Called by the attack-input hook for `Right Attack/Block`. Returns true when the engine
        // has consumed the event and it must not reach the game's own handler.
        //
        // Two entry points because `ProcessButton` only receives the down and up edges. The
        // ongoing hold arrives at `UpdateHeldStateActive` instead -- `AttackBlockHandler` is a
        // `HeldStateHandler` -- and without it a power press cannot be recognised until release,
        // which is often after the chain window has shut. Measured 2026-07-29: a 0.49s hold
        // resolved 42ms too late and the chain was lost.
        // `a_gameHoldThreshold` is the handler's own `initialPowerAttackDelay`, so the hold that
        // counts as a power press is the one the player's game already uses.
        static bool OnAttackButton(const RE::ButtonEvent& a_event, float a_gameHoldThreshold);
        static bool OnAttackHold(const RE::ButtonEvent& a_event, float a_gameHoldThreshold);

        // One Click Power Attack's key, seen on the raw input stream. Observed, never swallowed:
        // OCPA acts on the key itself, but its attempt lands in the exhale state where nothing
        // consumes an attack event (finding 7b), so it is a no-op and our chain is the only
        // effect. Returns true if the press started a chain.
        static bool OnPowerAttackKey(std::uint32_t a_keycode);

        // Milliseconds since the first observed graph event. Shared so every line in the trace is
        // on one clock, whichever hook wrote it.
        [[nodiscard]] static double ElapsedMs();

        // Is a shout in flight right now, READ OFF THE GRAPH? For the attack hook's A12 marker
        // only -- no decision is taken on it.
        //
        // Deliberately not `g_state.shoutActive`: that is engine state, set in `BeginShout`, which
        // does not run with `bEnabled = 0` -- so it reads false throughout a live shout in exactly
        // the disabled case A12 measures. This is driven by `BeginCastVoice` / `shoutStop`, which
        // arrive whatever the engine is set to.
        [[nodiscard]] static bool IsShoutLive();

        // Should a shout pressed right now be held back? Answered from the swing, not from MCO's
        // combo window: the window opens ~180ms BEFORE the hit lands, so gating on it still cut the
        // swing (measured 2026-08-02). `HitFrame` is what says the hit happened.
        struct ShoutVerdict {
            bool hold = false;        // an attack is live and its swing has not landed yet
            bool attackLive = false;  // for the trace -- tells "no attack" from "already swung"
        };
        [[nodiscard]] static ShoutVerdict ShouldHoldShout();

    private:
        using EventResult = RE::BSEventNotifyControl;

        static EventResult ProcessEvent_PC(
            RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink,
            RE::BSAnimationGraphEvent*                   a_event,
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource
        );

        static void Observe(RE::BSAnimationGraphEvent* a_event);

        // True for events worth dumping MCO graph state alongside. Only decides whether we pay
        // for the variable reads; everything is traced either way.
        static bool IsInteresting(std::string_view a_tag);

        // Per-frame spam that would bury a trace. `SCAR_UpdateDummy` alone fires every ~17ms
        // once a weapon is drawn.
        static bool IsNoise(std::string_view a_tag);

        static inline REL::Relocation<decltype(ProcessEvent_PC)> _originalPC;
    };
}
