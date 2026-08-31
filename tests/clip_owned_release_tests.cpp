// Clip-owned spellfire routing. Host tests, no Skyrim.
//
// THESE RUN ON THE HOST. The graph walk that samples the playing clip cannot live here; what
// can is the fail-open rule and the one-intercept-per-shout decision, which is the whole of
// ADR-0009's engine half. Runtime gates (A19, Fire Breath vs SYHO) stay on the log seam.

#include <cstdio>
#include <optional>

#include "ClipOwnedRelease.h"

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

int main() {
    using ShoutMCO::DecideClipOwnedSpellFire;
    using ShoutMCO::kClipOwnedSpellFireMinS;
    using ShoutMCO::SpellFireRoute;
    using std::optional;

    std::printf("ShoutMCO clip-owned spellfire routing\n\n");

    std::printf("fail open: missing or early annotation\n");
    CHECK(DecideClipOwnedSpellFire(std::nullopt, std::nullopt, false) ==
          SpellFireRoute::kDeliver);
    CHECK(DecideClipOwnedSpellFire(0.0f, 0.10f, false) == SpellFireRoute::kDeliver);
    CHECK(DecideClipOwnedSpellFire(0.10f, 0.10f, false) == SpellFireRoute::kDeliver);
    CHECK(DecideClipOwnedSpellFire(kClipOwnedSpellFireMinS, 0.10f, false) ==
          SpellFireRoute::kDeliver);

    std::printf("late annotation: intercept the generator, deliver the clip fire\n");
    CHECK(DecideClipOwnedSpellFire(0.38f, 0.10f, false) ==
          SpellFireRoute::kInterceptGenerator);
    CHECK(DecideClipOwnedSpellFire(0.38f, 0.10f, true) == SpellFireRoute::kDeliver);
    CHECK(DecideClipOwnedSpellFire(0.38f, 0.38f, false) == SpellFireRoute::kDeliver);
    CHECK(DecideClipOwnedSpellFire(0.38f, 0.38f, true) == SpellFireRoute::kDeliver);

    std::printf("late annotation, local time unread: first fire intercepts, second delivers\n");
    CHECK(DecideClipOwnedSpellFire(0.38f, std::nullopt, false) ==
          SpellFireRoute::kInterceptGenerator);
    CHECK(DecideClipOwnedSpellFire(0.38f, std::nullopt, true) == SpellFireRoute::kDeliver);

    std::printf("just later than the fail-open floor\n");
    CHECK(DecideClipOwnedSpellFire(0.151f, 0.10f, false) ==
          SpellFireRoute::kInterceptGenerator);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
