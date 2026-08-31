// The attack-queued shout release. Host tests, no Skyrim.
//
// THESE RUN ON THE HOST. Sampling `IsAttacking` is a graph read and lives in Observe; what can
// live here is the hold / release / cap-release decision. Runtime cells (1H, 2H, one power) stay
// on the log seam.
//
// Traces (Nolvus Awakening, Save14 CS-Test, bTrace=1, 2026-08-22):
//   D3 2H Iron Greatsword 0x0001359D — cancel `inRdy` at 147155 ms, IsAttacking=true,
//     replay, no BeginCastVoice; later `attackStop` at 147345, IsAttacking=false.
//   D5 1H Iron Sword 0x00012EB7 — same shape: cancel `inRdy` IsAttacking=true, replay,
//     no BeginCastVoice; later `attackStop` IsAttacking=false.
//   D2 2H drawn ready — idle shout admitted with IsAttacking=false attackState=0 (buffered
//     path can start from 2H ready; C3 item 3 stays deferred).

#include <cstdio>

#include "AttackQueuedRelease.h"

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
    using ShoutMCO::AttackQueuedAction;
    using ShoutMCO::AttackQueuedView;
    using ShoutMCO::DecideAttackQueuedRelease;

    std::printf("ShoutMCO attack-queued release\n\n");

    std::printf("not holding: never release, even at an idle-ready sample\n");
    CHECK(DecideAttackQueuedRelease({false, false, false}) == AttackQueuedAction::kHold);
    CHECK(DecideAttackQueuedRelease({false, true, true}) == AttackQueuedAction::kHold);

    std::printf("today's bug: cancel inRdy with IsAttacking still true must not release\n");
    CHECK(DecideAttackQueuedRelease({true, true, false}) == AttackQueuedAction::kHold);

    std::printf("named gate: first event where IsAttacking is false releases (1H and 2H)\n");
    CHECK(DecideAttackQueuedRelease({true, false, false}) == AttackQueuedAction::kRelease);

    std::printf("cap: ready never comes, still attacking\n");
    CHECK(DecideAttackQueuedRelease({true, true, true}) == AttackQueuedAction::kCapRelease);

    std::printf("admissible state wins over a simultaneous cap\n");
    CHECK(DecideAttackQueuedRelease({true, false, true}) == AttackQueuedAction::kRelease);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
