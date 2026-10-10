// MCO's teardown reset held one frame before it reaches the queued snapshot. Host tests, no Skyrim.
//
// The F1 case is `T25s2-A22.3-nordy-cap-and-F1-2026-08-04.log`: snapshot next=2 power=8, then in
// ONE graph update `@SGVI|MCO_nextattack|1`, `@SGVI|MCO_nextpowerattack|1`, then
// `JumpAttackStop` / `MCO_AttackExitNotify`. The engine resumed at 1; it should resume at 2 / 8.

#include <cstdio>

#include "ComboResetHold.h"

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
    using ShoutMCO::ComboReading;
    using ShoutMCO::ComboResetHold;
    using ShoutMCO::OnLiveComboReading;
    using ShoutMCO::SettleComboResetHold;

    std::printf("ShoutMCO combo reset hold\n\n");

    std::printf("F1: the reset and the end marker share a frame -> the snapshot keeps 2 / 8\n");
    {
        ComboResetHold     hold{};
        ComboReading       snap{.nextAttack = 2, .nextPowerAttack = 8};
        const ComboReading step1{.nextAttack = 1, .nextPowerAttack = 8};
        const ComboReading step2{.nextAttack = 1, .nextPowerAttack = 1};

        auto r = OnLiveComboReading(hold, snap, step1, 100);
        CHECK(!r.commit && r.heldNow);
        r = OnLiveComboReading(hold, snap, step2, 100);
        CHECK(!r.commit && r.heldNow);
        CHECK(hold.held && hold.reading == step2);
        const auto settled = SettleComboResetHold(hold, 100);
        CHECK(!settled);
        CHECK(!hold.held);
        CHECK(snap.nextAttack == 2 && snap.nextPowerAttack == 8);
    }

    std::printf("a wrap to 1 mid-attack is committed by the next frame's reading\n");
    {
        ComboResetHold     hold{};
        const ComboReading snap{.nextAttack = 4, .currentAttack = 3};
        const ComboReading wrap{.nextAttack = 1, .currentAttack = 4};

        auto r = OnLiveComboReading(hold, snap, wrap, 200);
        CHECK(!r.commit && r.heldNow);
        r = OnLiveComboReading(hold, snap, wrap, 207);
        CHECK(r.commit && *r.commit == wrap && r.releasedEarlier && !r.heldNow);
        CHECK(!hold.held);
    }

    std::printf("a wrap that is the last reading before a later-frame end marker is kept\n");
    {
        ComboResetHold     hold{};
        const ComboReading snap{.nextPowerAttack = 3};
        const ComboReading wrap{.nextPowerAttack = 1};

        (void)OnLiveComboReading(hold, snap, wrap, 300);
        const auto settled = SettleComboResetHold(hold, 312);
        CHECK(settled && *settled == wrap);
    }

    std::printf("an ordinary advance is committed at once and never held\n");
    {
        ComboResetHold     hold{};
        const ComboReading snap{.nextAttack = 1, .nextPowerAttack = 1};
        const ComboReading adv{.nextAttack = 1, .nextPowerAttack = 2};

        const auto r = OnLiveComboReading(hold, snap, adv, 400);
        CHECK(r.commit && *r.commit == adv && !r.heldNow);
        CHECK(!hold.held);
        CHECK(!SettleComboResetHold(hold, 400));
    }

    std::printf("1 -> 1 is not a move to ready, so a fresh combo is not held\n");
    {
        ComboResetHold     hold{};
        const ComboReading snap{.nextAttack = 1, .nextPowerAttack = 1};
        const ComboReading same{.nextAttack = 1, .nextPowerAttack = 1};

        const auto r = OnLiveComboReading(hold, snap, same, 500);
        CHECK(!r.commit && !r.heldNow && !hold.held);
    }

    std::printf("current counters falling to 0 are held like next falling to 1\n");
    {
        ComboResetHold     hold{};
        const ComboReading snap{.nextAttack = 3, .currentAttack = 2};
        const ComboReading reset{.nextAttack = 3, .currentAttack = 0};

        const auto r = OnLiveComboReading(hold, snap, reset, 600);
        CHECK(r.heldNow && !r.commit);
    }

    std::printf("a same-frame reading back at the committed value clears the hold\n");
    {
        ComboResetHold     hold{};
        const ComboReading snap{.nextAttack = 2};
        const ComboReading dip{.nextAttack = 1};

        (void)OnLiveComboReading(hold, snap, dip, 700);
        const auto r = OnLiveComboReading(hold, snap, snap, 700);
        CHECK(!r.commit && !hold.held);
        CHECK(!SettleComboResetHold(hold, 750));
    }

    std::printf("a held reading is committed by a later frame, then a new reset is held against it\n");
    {
        ComboResetHold     hold{};
        const ComboReading snap{.nextAttack = 4};
        const ComboReading wrap{.nextAttack = 1, .currentAttack = 4};
        const ComboReading reset{.nextAttack = 1, .currentAttack = 0};

        (void)OnLiveComboReading(hold, snap, wrap, 800);
        const auto r = OnLiveComboReading(hold, snap, reset, 820);
        CHECK(r.commit && *r.commit == wrap && r.releasedEarlier && r.heldNow);
        CHECK(hold.held && hold.frame == 820);
        CHECK(!SettleComboResetHold(hold, 820));
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
