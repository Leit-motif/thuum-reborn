// Tap-responsive shouts: the GMST hold-threshold override decision. Host tests, no Skyrim.
//
// THESE RUN ON THE HOST. What cannot live here is the two lines that read and write
// `RE::GameSettingCollection`; what can is everything that decides whether to write, what to write,
// what to put back, and when a value is refused -- which is the whole of the feature's logic.
//
// The in-game gate is separate and stays open until driven: a hold at 300 ms must read one word
// with the default 0.4, and 500 ms must read two, off `ShoutMCO.log`'s own
// `WINDOW tail measured ... N word(s)` lines.

#include <cstdio>

#include "ShoutHoldThresholds.h"

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
    using ShoutMCO::DecideHoldOverride;
    using ShoutMCO::HoldOverrideAction;
    using ShoutMCO::HoldOverrideState;
    using ShoutMCO::kHoldOverrideMaxSec;
    using ShoutMCO::kVanillaWordThreeHoldSec;
    using ShoutMCO::kVanillaWordTwoHoldSec;
    using ShoutMCO::NearHoldSec;
    using ShoutMCO::PlanHoldOverrides;
    using ShoutMCO::SanitizeHoldSec;

    std::printf("ShoutMCO shout hold thresholds\n\n");

    std::printf("sanitizing: 0 and negatives disable, absurd values clamp, nan disables\n");
    CHECK(SanitizeHoldSec(0.0f) == 0.0f);
    CHECK(SanitizeHoldSec(-0.4f) == 0.0f);
    CHECK(SanitizeHoldSec(-1e9f) == 0.0f);
    CHECK(SanitizeHoldSec(0.4f) == 0.4f);
    CHECK(SanitizeHoldSec(kHoldOverrideMaxSec) == kHoldOverrideMaxSec);
    CHECK(SanitizeHoldSec(9999.0f) == kHoldOverrideMaxSec);
    CHECK(SanitizeHoldSec(std::nanf("")) == 0.0f);

    std::printf("first pass: capture the game's value and write the override\n");
    {
        const auto first = DecideHoldOverride(true, 0.4f, kVanillaWordTwoHoldSec, {});
        CHECK(first.action == HoldOverrideAction::kApply);
        CHECK(NearHoldSec(first.value, 0.4f));
        CHECK(NearHoldSec(first.previous, kVanillaWordTwoHoldSec));
        CHECK(first.state.applied);
        CHECK(NearHoldSec(first.state.capturedOriginal, kVanillaWordTwoHoldSec));
        CHECK(NearHoldSec(first.state.lastWritten, 0.4f));

        std::printf("second pass with nothing changed: idempotent, no write\n");
        const auto second = DecideHoldOverride(true, 0.4f, first.value, first.state);
        CHECK(second.action == HoldOverrideAction::kLeave);
        CHECK(second.state.applied);
        CHECK(NearHoldSec(second.state.capturedOriginal, kVanillaWordTwoHoldSec));

        std::printf("retuned between shouts: re-write, original still the game's\n");
        const auto retuned = DecideHoldOverride(true, 0.6f, second.value, second.state);
        CHECK(retuned.action == HoldOverrideAction::kApply);
        CHECK(NearHoldSec(retuned.value, 0.6f));
        CHECK(NearHoldSec(retuned.state.capturedOriginal, kVanillaWordTwoHoldSec));

        std::printf("bEnabled flipped off: restore the captured value, then stay quiet\n");
        const auto off = DecideHoldOverride(false, 0.6f, retuned.value, retuned.state);
        CHECK(off.action == HoldOverrideAction::kRestore);
        CHECK(NearHoldSec(off.value, kVanillaWordTwoHoldSec));
        CHECK(!off.state.applied);

        const auto stillOff = DecideHoldOverride(false, 0.6f, off.value, off.state);
        CHECK(stillOff.action == HoldOverrideAction::kLeave);
        CHECK(!stillOff.state.applied);

        std::printf("back on after a restore: capture afresh and apply again\n");
        const auto backOn = DecideHoldOverride(true, 0.4f, stillOff.value, stillOff.state);
        CHECK(backOn.action == HoldOverrideAction::kApply);
        CHECK(NearHoldSec(backOn.state.capturedOriginal, kVanillaWordTwoHoldSec));
    }

    std::printf("a data load (or a console setgs) clobbered our write: re-apply, re-baseline\n");
    {
        HoldOverrideState state{.applied = true, .capturedOriginal = 0.15f, .lastWritten = 0.4f};
        // The load put Skyrim.esm's own record back. That is the new baseline, NOT the 0.15 we
        // captured before it -- restoring 0.15 later would be putting back a value the player's
        // game no longer has.
        const auto reapplied = DecideHoldOverride(true, 0.4f, kVanillaWordTwoHoldSec, state);
        CHECK(reapplied.action == HoldOverrideAction::kApply);
        CHECK(NearHoldSec(reapplied.value, 0.4f));
        CHECK(NearHoldSec(reapplied.state.capturedOriginal, kVanillaWordTwoHoldSec));

        // And with the engine off, a clobbered override is not un-clobbered: there is nothing of
        // ours standing to withdraw.
        const auto offAfterClobber =
            DecideHoldOverride(false, 0.4f, kVanillaWordTwoHoldSec, state);
        CHECK(offAfterClobber.action == HoldOverrideAction::kLeave);
        CHECK(!offAfterClobber.state.applied);
    }

    std::printf("configured to the value already there: no write, but the state is claimed\n");
    {
        const auto same = DecideHoldOverride(true, kVanillaWordTwoHoldSec, kVanillaWordTwoHoldSec,
                                             HoldOverrideState{});
        CHECK(same.action == HoldOverrideAction::kLeave);
        CHECK(same.state.applied);
        CHECK(NearHoldSec(same.state.capturedOriginal, kVanillaWordTwoHoldSec));
    }

    std::printf("plan: shipped defaults move word two and leave word three alone\n");
    {
        const auto plan = PlanHoldOverrides(true, 0.4f, 0.0f, kVanillaWordTwoHoldSec,
                                            kVanillaWordThreeHoldSec, {}, {});
        CHECK(plan.wordTwo.action == HoldOverrideAction::kApply);
        CHECK(NearHoldSec(plan.wordTwo.value, 0.4f));
        CHECK(plan.wordThree.action == HoldOverrideAction::kLeave);
        CHECK(!plan.wordThreeRefused);
        CHECK(NearHoldSec(plan.effectiveWordTwoSec, 0.4f));
    }

    std::printf("plan: word three above word two is applied\n");
    {
        const auto plan = PlanHoldOverrides(true, 0.4f, 1.2f, kVanillaWordTwoHoldSec,
                                            kVanillaWordThreeHoldSec, {}, {});
        CHECK(plan.wordThree.action == HoldOverrideAction::kApply);
        CHECK(NearHoldSec(plan.wordThree.value, 1.2f));
        CHECK(!plan.wordThreeRefused);
    }

    std::printf("plan: word three at or below the EFFECTIVE word two is refused, not clamped\n");
    {
        // Against the value being written this pass (0.4), not against the 0.2 the game still had
        // when the pass started -- 0.3 would be legal against vanilla and is not against 0.4.
        const auto refused = PlanHoldOverrides(true, 0.4f, 0.3f, kVanillaWordTwoHoldSec,
                                               kVanillaWordThreeHoldSec, {}, {});
        CHECK(refused.wordThreeRefused);
        CHECK(refused.wordThree.action == HoldOverrideAction::kLeave);
        CHECK(!refused.wordThree.state.applied);

        const auto equal = PlanHoldOverrides(true, 0.4f, 0.4f, kVanillaWordTwoHoldSec,
                                             kVanillaWordThreeHoldSec, {}, {});
        CHECK(equal.wordThreeRefused);
        CHECK(equal.wordThree.action == HoldOverrideAction::kLeave);
    }

    std::printf("plan: a refusal WITHDRAWS an override that had already landed\n");
    {
        HoldOverrideState three{.applied = true, .capturedOriginal = kVanillaWordThreeHoldSec,
                                .lastWritten = 1.2f};
        const auto plan = PlanHoldOverrides(true, 0.4f, 0.3f, 0.4f, 1.2f,
                                            HoldOverrideState{.applied = true,
                                                              .capturedOriginal =
                                                                  kVanillaWordTwoHoldSec,
                                                              .lastWritten = 0.4f},
                                            three);
        CHECK(plan.wordThreeRefused);
        CHECK(plan.wordThree.action == HoldOverrideAction::kRestore);
        CHECK(NearHoldSec(plan.wordThree.value, kVanillaWordThreeHoldSec));
    }

    std::printf("plan: word three is judged against the RESTORED word two when the engine is off\n");
    {
        // bEnabled = 0 unwinds both, and the guard must not fire on the way out -- there is no
        // override to refuse.
        const auto plan = PlanHoldOverrides(
            false, 0.4f, 0.3f, 0.4f, 1.2f,
            HoldOverrideState{
                .applied = true, .capturedOriginal = kVanillaWordTwoHoldSec, .lastWritten = 0.4f},
            HoldOverrideState{
                .applied = true, .capturedOriginal = kVanillaWordThreeHoldSec, .lastWritten = 1.2f});
        CHECK(plan.wordTwo.action == HoldOverrideAction::kRestore);
        CHECK(NearHoldSec(plan.wordTwo.value, kVanillaWordTwoHoldSec));
        CHECK(NearHoldSec(plan.effectiveWordTwoSec, kVanillaWordTwoHoldSec));
        CHECK(plan.wordThree.action == HoldOverrideAction::kRestore);
        CHECK(NearHoldSec(plan.wordThree.value, kVanillaWordThreeHoldSec));
        CHECK(!plan.wordThreeRefused);
    }

    std::printf("plan: a negative word two disables the override without disabling word three\n");
    {
        const auto plan = PlanHoldOverrides(true, -1.0f, 1.2f, kVanillaWordTwoHoldSec,
                                            kVanillaWordThreeHoldSec, {}, {});
        CHECK(plan.wordTwo.action == HoldOverrideAction::kLeave);
        CHECK(NearHoldSec(plan.effectiveWordTwoSec, kVanillaWordTwoHoldSec));
        CHECK(plan.wordThree.action == HoldOverrideAction::kApply);
    }

    std::printf("plan: an absurd word two clamps, and word three is judged against the CLAMP\n");
    {
        const auto plan = PlanHoldOverrides(true, 9999.0f, 4.0f, kVanillaWordTwoHoldSec,
                                            kVanillaWordThreeHoldSec, {}, {});
        CHECK(plan.wordTwo.action == HoldOverrideAction::kApply);
        CHECK(NearHoldSec(plan.wordTwo.value, kHoldOverrideMaxSec));
        CHECK(plan.wordThreeRefused);
        CHECK(plan.wordThree.action == HoldOverrideAction::kLeave);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
