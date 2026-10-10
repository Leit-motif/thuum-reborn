// Adopting a swing no MCO initiate opened -- every BFCO swing. Host tests, no Skyrim.
//
// The reads (`IsAttacking`, the actor's attack state) live in the engine; the rule lives in
// SwingAdoption.h. Measured shape it is written against (2026-09-09 BFCO sign-off log):
// a chained swing winds up at attackState=1 (`Bfco_AttackStartFX`), and from `HitFrame` on the
// state reads 3 (`BFCO_PlayerAttackStart` lands there, after the hit).

#include <cstdio>

#include "SwingAdoption.h"

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
    using ShoutMCO::DecideSwingAdoption;
    using ShoutMCO::SwingAdoption;

    std::printf("ShoutMCO swing adoption\n\n");

    std::printf("an MCO initiate already opened the tracker: nothing to adopt\n");
    CHECK(DecideSwingAdoption(true, {true, 1}) == SwingAdoption::kNone);
    CHECK(DecideSwingAdoption(true, {true, 3}) == SwingAdoption::kNone);

    std::printf("not attacking: nothing to adopt, whatever the state says\n");
    CHECK(DecideSwingAdoption(false, {false, 0}) == SwingAdoption::kNone);
    CHECK(DecideSwingAdoption(false, {false, 3}) == SwingAdoption::kNone);

    std::printf("winding up (draw, swing): adopt, wait for the hit\n");
    CHECK(DecideSwingAdoption(false, {true, 1}) == SwingAdoption::kBeforeHit);
    CHECK(DecideSwingAdoption(false, {true, 2}) == SwingAdoption::kBeforeHit);

    std::printf("landed (hit, next attack, follow-through): adopt, cut at once\n");
    CHECK(DecideSwingAdoption(false, {true, 3}) == SwingAdoption::kAfterHit);
    CHECK(DecideSwingAdoption(false, {true, 4}) == SwingAdoption::kAfterHit);
    CHECK(DecideSwingAdoption(false, {true, 5}) == SwingAdoption::kAfterHit);

    std::printf("IsAttacking with no melee state (teardown tail): not adopted\n");
    CHECK(DecideSwingAdoption(false, {true, 0}) == SwingAdoption::kNone);

    std::printf("bash, bow and spell states are not swings a shout waits behind\n");
    CHECK(DecideSwingAdoption(false, {true, 6}) == SwingAdoption::kNone);
    for (int s = 8; s <= 17; ++s) {
        CHECK(DecideSwingAdoption(false, {true, s}) == SwingAdoption::kNone);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
