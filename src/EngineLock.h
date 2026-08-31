#pragma once

#include <mutex>

namespace ShoutMCO::detail {
    // THE ENGINE LOCK -- one owner for every piece of cross-thread chain state.
    //
    // `Observe` is entered from at least six distinct threads, the input hooks run on whatever
    // thread carries
    // the input dispatch, and the deferred work drains on the main thread. Everything those paths
    // share -- `ChainState`, the queued combo snapshot, the MCO attack tracking, and the held
    // shout press -- is one machine, so it is guarded as one unit by this one mutex rather than
    // per-field atomics, which cannot make a multi-field snapshot tear-free.
    //
    // The discipline, stated once here and assumed everywhere (CONTEXT.md):
    //
    //   1. ONLY ENTRY POINTS LOCK. `Observe`, the input-hook bodies, `ReleaseHeldShout`, the
    //      load listener, and deferred-task bodies take the lock exactly once each. Internal
    //      helpers are named `...Locked` or sit inside a locked block, and never lock themselves
    //      -- so nesting is impossible by construction, and a plain (non-recursive) mutex will
    //      hang the first misuse in testing rather than mask it.
    //
    //   2. NO GAME CALLS UNDER THE LOCK. Graph-variable reads, `NotifyAnimationGraph`, the
    //      original input handlers, `ToggleControls`, position and velocity reads all happen
    //      OUTSIDE the locked region -- reads are sampled before it (phase A), decisions and
    //      state mutation happen inside it (phase B), and emissions run after it (phase C).
    //      The reason is deadlock, not politeness: the graph dispatch that calls `Observe` can
    //      hold engine-internal locks, so taking those same locks from another thread while
    //      holding this one is an ABBA deadlock waiting for the right two frames.
    //      `SKSE::GetTaskInterface()->AddTask` is known safe under the lock (its drain provably
    //      runs tasks outside the queue lock -- a same-pass requeue could not spin otherwise),
    //      but the rule stays blanket because a blanket rule needs no judgment call.
    //
    //   3. spdlog IS allowed under the lock. The sink is thread-safe and takes no game lock, and
    //      allowing it keeps the decision code readable enough to audit.
    //
    //   4. Atomics remain for genuinely independent single facts read from anywhere: the trace
    //      liveness flag, the replay generation, the trace-enabled mirror, the movement-held
    //      flags, and the sampling hints. None of them is part of a multi-field snapshot.
    inline std::mutex g_engineLock;
}
