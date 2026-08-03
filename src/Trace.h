#pragma once
#include "PCH.h"

#include <spdlog/spdlog.h>

#include "Settings.h"

namespace ShoutMCO {
    // The evidence channel, and the thing that must not ship on.
    //
    // Every acceptance gate in this project is evidenced from a trace, so the tracing is load
    // bearing during development and pure cost afterwards: `Observe` runs on EVERY animation event
    // on the player, and `SCAR_UpdateDummy` alone fires every ~17ms once a weapon is drawn
    // (CONTEXT.md finding 8). At `bTrace = 1` that is a multi-megabyte log and constant disk I/O
    // through combat.
    //
    // The split this enforces, decided in ticket 16:
    //
    //   Trace(...)   -- per-event and per-shout lines. Silent at `bTrace = 0`. Every line carrying
    //                   the `[{:10.2f}]` elapsed-time prefix is one of these, which is why the
    //                   distinction is legible at the call site rather than needing a comment.
    //   log::info    -- one-time startup lines only: the config dump and the hook installations.
    //                   These survive at `bTrace = 0` and are what makes a user's bug report
    //                   diagnosable at all.
    //   log::warn    -- kept unconditionally. A malformed INI or a missing one is not trace volume.
    //   log::error   -- kept unconditionally.
    //
    // Note the `>>> CHAIN` lines are NOT exempt. One line per shout sounds cheap right up until
    // somebody plays for six hours.
    //
    // Deliberately gated at the call site rather than by lowering the spdlog level: the level is
    // global, so it could not keep the startup lines while dropping the per-event ones, and a
    // reader of this code should be able to see which channel a line is on without checking what
    // the logger was set to at some earlier point.
    //
    // Mirrors `spdlog::info`'s own signature, so the emitted line is byte-identical to what
    // `log::info` produced before -- the archived traces under `.scratch/shout-mco-engine/` stay
    // comparable with new ones.
    template <class... Args>
    void TraceLine(spdlog::format_string_t<Args...> a_fmt, Args&&... a_args) {
        spdlog::log(spdlog::level::info, a_fmt, std::forward<Args>(a_args)...);
    }

    [[nodiscard]] inline bool TraceEnabled() { return Settings::Get().trace; }
}

// A MACRO, and deliberately so: the argument expressions must not be evaluated when tracing is off.
//
// The first version of this was a plain function that checked `Settings::trace` on entry. C++
// evaluates arguments BEFORE the call, so that check suppressed the write and nothing else -- every
// `GraphSummary(actor)` (six graph-variable reads) and every `MotionSummary(actor)` still ran and
// still formatted a string at `bTrace = 0`. The one place it did not was `Observe`, which carried a
// hand-written cost guard; the guard was the tell that the API itself was wrong, and the other
// sites simply did not have one. Caught in cold review 2026-08-03.
//
// A function cannot fix this without making every call site pass a lambda. The macro can, it fixes
// all ~30 sites at once, and it cannot be forgotten at a new one.
//
// `do { } while (0)` so it is a single statement and safe as the body of an unbraced `if`.
#define SHOUTMCO_TRACE(...)                            \
    do {                                               \
        if (::ShoutMCO::TraceEnabled()) {              \
            ::ShoutMCO::TraceLine(__VA_ARGS__);        \
        }                                              \
    } while (0)
