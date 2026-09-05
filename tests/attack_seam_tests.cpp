// The power-attack seam's match rule and re-entrancy guard. Host tests, no Skyrim.
//
// The rule is a prefix over outgoing graph event names, which is the whole point: it has to hold
// for every power-attack source without naming any of them. The names below are the ones the
// engine and the graph actually trade -- vanilla's and MCO's directional power starts, this
// plugin's own `SH2_*` traffic, and the shout events the trace is full of.

#include <cstdio>
#include <thread>

#include "AttackSeam.h"

namespace {
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_ok, const char* a_what, int a_line) {
        ++g_checks;
        if (a_ok) return;
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n", a_line, a_what);
    }

#define CHECK(expr) Check((expr), #expr, __LINE__)
}

// Constant-evaluated, so the rule is proved at compile time as well as at run time -- a
// `constexpr` predicate that only ever ran at run time would not be one.
static_assert(ShoutMCO::IsPowerAttackStartEvent("attackPowerStartInPlace"));
static_assert(!ShoutMCO::IsPowerAttackStartEvent("attackStart"));

int main() {
    using ShoutMCO::IsPowerAttackStartEvent;
    using ShoutMCO::ScopedOwnEmit;

    std::printf("ShoutMCO power-attack seam\n\n");

    std::printf("every power-attack start matches, whatever the direction\n");
    CHECK(IsPowerAttackStartEvent("attackPowerStart"));
    CHECK(IsPowerAttackStartEvent("attackPowerStartInPlace"));
    CHECK(IsPowerAttackStartEvent("attackPowerStartForward"));
    CHECK(IsPowerAttackStartEvent("attackPowerStartBackward"));
    CHECK(IsPowerAttackStartEvent("attackPowerStartLeft"));
    CHECK(IsPowerAttackStartEvent("attackPowerStartRight"));
    CHECK(IsPowerAttackStartEvent("attackPowerStartDualWield"));

    std::printf("nothing else does -- a light attack, a bash, a stop, our own events\n");
    CHECK(!IsPowerAttackStartEvent("attackStart"));
    CHECK(!IsPowerAttackStartEvent("attackStartLeftHand"));
    CHECK(!IsPowerAttackStartEvent("attackStop"));
    CHECK(!IsPowerAttackStartEvent("attackPowerStop"));
    CHECK(!IsPowerAttackStartEvent("bashStart"));
    CHECK(!IsPowerAttackStartEvent("blockStart"));
    CHECK(!IsPowerAttackStartEvent("shoutStart"));
    CHECK(!IsPowerAttackStartEvent("shoutStop"));
    CHECK(!IsPowerAttackStartEvent("SH2_ArtStart"));
    CHECK(!IsPowerAttackStartEvent("SH2_CastExit"));
    CHECK(!IsPowerAttackStartEvent("MCO_AttackExitNotify"));
    CHECK(!IsPowerAttackStartEvent(""));

    std::printf("a name that only CONTAINS the prefix is not a match -- the rule is a prefix\n");
    CHECK(!IsPowerAttackStartEvent("MCO_attackPowerStartInPlace"));
    CHECK(!IsPowerAttackStartEvent("attackPowerStar"));

    std::printf("the re-entrancy guard is off, on inside its scope, and off again after\n");
    CHECK(!ScopedOwnEmit::Active());
    {
        ScopedOwnEmit guard;
        CHECK(ScopedOwnEmit::Active());
        {
            // Nested: the inner guard must not clear a flag the outer one still owns.
            ScopedOwnEmit inner;
            CHECK(ScopedOwnEmit::Active());
        }
        CHECK(ScopedOwnEmit::Active());
    }
    CHECK(!ScopedOwnEmit::Active());

    std::printf("and it is per thread, so our replay cannot mask another thread's press\n");
    {
        ScopedOwnEmit guard;
        bool          seenOnOtherThread = true;
        std::thread   other([&] { seenOnOtherThread = ScopedOwnEmit::Active(); });
        other.join();
        CHECK(!seenOnOtherThread);
        CHECK(ScopedOwnEmit::Active());
    }

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
