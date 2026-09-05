#pragma once
#include "PCH.h"

namespace ShoutMCO {
    // Shout -> MCO attack chaining, direction one of three.
    //
    // The whole mechanism is two transitions that already exist in the vanilla graph, fired in
    // order: `shoutStop` returns the exhale to ready, and ready consumes `attackStart`
    // (CONTEXT.md). Between them, sequenced on the `inRdy` the cut produces, the combo index is
    // written back -- the ready path's own PIE reset lands just before `inRdy`, so anything after
    // that event is already past it.
    //
    // No graph patch is involved, and the DLL never inspects shout cooldown state.
    //
    // THE OTHER TWO DIRECTIONS SHIP AS WELL: MCO attack -> shout, and shout -> shout. Neither
    // waits on a new graph transition. The press is held DLL-side for one entry and replayed at a
    // confirmed legal state boundary, which is what lets them work without the re-entry into the
    // inhale that no vanilla transition provides. That direct re-entry is deferred rather than
    // owed: it would buy teardown-free presentation, not reachability, and both directions are
    // functional without it.
    class ShoutChainEngine {
    public:
        static void Install();

        // Called by the attack-input hook for `Right Attack/Block`. Returns true when the engine
        // has consumed the event and it must not reach the game's own handler. A queued shout
        // counts as engine-owned here even before `BeginCastVoice`: attack input is buffered
        // across that wait instead of forwarding it into an attack graph the queued shout will
        // tear down.
        //
        // Two entry points because `ProcessButton` only receives the down and up edges. The
        // ongoing hold arrives at `UpdateHeldStateActive` instead -- `AttackBlockHandler` is a
        // `HeldStateHandler` -- and without it a power press cannot be recognised until release,
        // which is often after the chain window has shut. Measured: a 0.49s hold
        // resolved 42ms too late and the chain was lost.
        // `a_gameHoldThreshold` is the handler's own `initialPowerAttackDelay`, so the hold that
        // counts as a power press is the one the player's game already uses.
        static bool OnAttackButton(const RE::ButtonEvent& a_event, float a_gameHoldThreshold);
        static bool OnAttackHold(const RE::ButtonEvent& a_event, float a_gameHoldThreshold);

        // A power press, seen DOWNSTREAM OF THE KEY as an outgoing `attackPowerStart*` graph
        // event (`AttackSeam.h`). Every power-attack source -- vanilla's hold, One Click Power
        // Attack's key, MCO's directional variants -- reaches this same seam, so nothing here
        // reads another mod's config or knows a key code.
        //
        // Returns true if the press was buffered for a queued shout or started a chain, in which
        // case the caller does NOT forward the event: the engine's own replay sends the real one
        // after the lock, and the source's attempt would otherwise land in the exhale state where
        // nothing consumes an attack event anyway.
        static bool OnPowerAttackEvent(std::string_view a_eventName);

        // Milliseconds since the first observed graph event. Shared so every line in the trace is
        // on one clock, whichever hook wrote it.
        [[nodiscard]] static double ElapsedMs();

        // The renderer's own frame number, from `BSGraphics::State::frameCount` -- the same counter
        // DevBench's health reading reports, so a trace line and a telemetry call can be compared
        // directly. Wall time cannot tell "the same frame" from "the next one": two events 0.2ms
        // apart are one frame at 60fps and two at 5000, and reading a trace turns on exactly
        // that distinction.
        //
        // FOR THE TRACE, not for decisions. Nothing waits on it or branches on it: it is written
        // from the render thread, and a plugin that gated behaviour on it would be reading a value
        // whose meaning depends on where in the frame it was sampled. Returns 0 before the graphics
        // state exists, which is indistinguishable from a genuine frame 0 -- acceptable in an
        // instrument that only ever prints, and the reason it must stay one.
        [[nodiscard]] static std::uint32_t FrameCount();

        // Is a shout in flight right now, READ OFF THE GRAPH? For the attack hook's forwarded-
        // press marker only -- no decision is taken on it.
        //
        // Deliberately not `g_state.shoutActive`: that is engine state, set in `BeginShout`, which
        // returns early on any shout the engine holds nothing for -- so it can read false through a
        // live shout. This is driven by `BeginCastVoice` / `shoutStop`, which arrive regardless.
        [[nodiscard]] static bool IsShoutLive();

        // Should a shout pressed right now be queued behind a running attack? Answered from
        // whether an MCO attack is live at all -- not from MCO's combo window, which opens ~180ms
        // BEFORE the hit, and no longer from `HitFrame`, which banks the hit but leaves the attack
        // owning the character for ~900ms after it. "Live" runs through the teardown until `inRdy`,
        // so a press
        // made while the graph is still resetting queues rather than being offered to a graph
        // that would silently refuse it.
        //
        // `...Locked`: requires `detail::g_engineLock` held (see EngineLock.h). The one caller is
        // the shout-input hook's own locked region, so press-taking and queue-noting are one
        // atomic decision rather than two racing ones.
        // Why a shout press is held. Attack and shout→shout share one held slot (one-entry cast
        // intent); the reason decides which confirmed state releases it (`IsAttacking` falling vs
        // `shoutStop`).
        enum class ShoutHoldReason {
            kNone = 0,
            kBehindAttack,  // MCO attack live -- release when IsAttacking falls
            kBehindShout,   // legal chain window open -- arm at `shoutStop`, paced release
        };
        struct ShoutVerdict {
            bool            hold = false;
            ShoutHoldReason reason = ShoutHoldReason::kNone;
            bool            attackLive = false;   // for the trace -- tells "no attack" from "attack running"
            bool            swingLanded = false;  // for the trace only; it decides nothing
        };
        [[nodiscard]] static ShoutVerdict ShouldHoldShoutLocked();

        // MCO's combo counters, read off the graph in one place. A GAME read -- call it OUTSIDE
        // the engine lock (EngineLock.h rule 2) and hand the result to the `Locked` functions.
        struct ComboSnapshot {
            int nextAttack = 0;
            int nextPowerAttack = 0;
            int currentAttack = 0;
            int currentPowerAttack = 0;
        };
        [[nodiscard]] static ComboSnapshot SampleCombo(RE::Actor* a_actor);

        // The shout key's DOWN edge, from `ShoutInputHook`. This is the authoritative sprint
        // reading: the earliest point the engine sees a shout at all, ahead of `ShoutHandler`,
        // ahead of any teardown, and the same edge the `fShoutTime1` override already uses for
        // exactly that reason.
        //
        // Stamping the sprint from animation events and giving the arm a 500 ms grace closed the
        // teardown-ordering gap and was still not enough: real input reads `last sprint 1146ms`
        // through `2757ms` at every arm, so the sprint is long gone by the time the engine looks.
        // A window wide enough to cover that would also arm a sprint attack for someone who
        // stopped sprinting two seconds ago and then shouted standing still.
        //
        // So the reading moves to the press instead of the window getting wider. A GAME read --
        // call it OUTSIDE the engine lock (EngineLock.h rule 2).
        static void NoteShoutKeyDown(RE::Actor* a_actor);

        // Called the moment a shout press is queued behind a live attack. Records the combo
        // position while MCO's counter still holds it -- the attack ends before the shout starts
        // now, and MCO resets that counter when it does. Without this the engine resumes at 1 and
        // the combo is lost, which is the one thing this mod exists to prevent.
        // Requires `detail::g_engineLock` held; the sample is taken by the caller outside it.
        static void NoteQueuedShoutLocked(const ComboSnapshot& a_combo);

        // Cut a live shout's exhale for a shout press queued in its window, so
        // shout→shout chains like the other two directions instead of waiting the clip out.
        // MUST be called with the engine lock DOWN -- it takes the lock itself and then emits.
        static void CutShoutForQueuedShoutChain();

        // Drop a queued combo snapshot whose press will never become a shout: the
        // replay was superseded before its down went out, a real press took the button over, or
        // the game was loaded. The `Locked` form is for callers already inside the engine lock;
        // the plain form takes it.
        static void AbandonQueuedResume(std::string_view a_reason);
        // A replayed shout press did not become a shout. Abandons the queued token and hands back
        // any press parked behind it, the same hand-back the press watchdog makes -- but now,
        // not four seconds later. No-op when nothing is queued or parked.
        static void QueuedShoutDidNotStart();
        static void AbandonQueuedResumeLocked(std::string_view a_reason);

    private:
        using EventResult = RE::BSEventNotifyControl;

        static EventResult ProcessEvent_PC(
            RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink,
            RE::BSAnimationGraphEvent*                   a_event,
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource
        );

        // Returns false when this event must not reach TESObjectREFR's shout handler
        // (player clip-owned generator intercept, ADR-0009). True forwards as before.
        static bool Observe(RE::BSAnimationGraphEvent* a_event);

        // True for events worth dumping MCO graph state alongside. Only decides whether we pay
        // for the variable reads; everything is traced either way.
        static bool IsInteresting(std::string_view a_tag);

        // Per-frame spam that would bury a trace. `SCAR_UpdateDummy` alone fires every ~17ms
        // once a weapon is drawn.
        static bool IsNoise(std::string_view a_tag);

        static inline REL::Relocation<decltype(ProcessEvent_PC)> _originalPC;
    };
}
