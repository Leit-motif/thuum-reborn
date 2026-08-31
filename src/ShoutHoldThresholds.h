#pragma once

// THE HOLD THRESHOLDS THAT DECIDE HOW MANY WORDS A SHOUT IS.
//
// Vanilla `fShoutTime1` = 0.2 s and `fShoutTime2` = 0.9 s, both defined only by `Skyrim.esm`
// (`01402F`), override depth 1, no plugin winner in this load order. They are the ONLY thing that
// decides word count: a live A/B (Fire Breath, weapon drawn, DevBench-injected holds, words read
// from `ShoutMCO.log`'s own `WINDOW tail measured ... N word(s)` lines) moved `fShoutTime1` to
// 0.5 with `setgs` and the word-2 boundary moved with it, one for one:
//
//     0.2 (vanilla): 30->1  150->1  200->2  250->2  300->2  400->2  1200->3
//     0.5 (setgs):   150->1 300->1  400->1  550->2  700->2
//
// The bracket contains the GMST value and nothing else at both settings. That is what makes a
// deliberate human tap read as a two-word charge: 200 ms is inside an ordinary thumb-button press,
// and this pack's fully animated clips make a threshold nobody notices in vanilla legible.
//
// FREE OF `RE::` AND SKSE, for the same reason `ClipOwnedRelease.h` and `CastIntentSlot.h` are:
// the decision of whether to write, what to write, and when to put the player's own value back has
// to be checkable without launching the game. The two lines that actually touch
// `RE::GameSettingCollection` live in `Settings.cpp`.

#include <cmath>

namespace ShoutMCO {
    // Vanilla, for the log line and for the INI's own documentation. Read off `Skyrim.esm`;
    // they are NOT used as fallbacks -- the game's live value is always read.
    inline constexpr float kVanillaWordTwoHoldSec = 0.2f;
    inline constexpr float kVanillaWordThreeHoldSec = 0.9f;

    // Refusal ceiling. Above this the setting stops being a feel adjustment and becomes a shout
    // the player cannot finish charging: five seconds of held button before word two, against a
    // vanilla 0.2. Clamped rather than rejected, on `iChainWindowPct`'s precedent -- an absurd
    // value has no second reading, so honouring the nearest sane one beats dropping the line.
    inline constexpr float kHoldOverrideMaxSec = 5.0f;

    // Float equality against a value the engine round-trips through a `float` union member. A
    // millisecond is two orders of magnitude below anything a player can feel, and three below the
    // 0.2 this exists to move, so it cannot mask a real difference.
    inline constexpr float kHoldOverrideEpsilonS = 0.001f;

    [[nodiscard]] inline bool NearHoldSec(float a_lhs, float a_rhs) {
        return std::fabs(a_lhs - a_rhs) < kHoldOverrideEpsilonS;
    }

    // 0 or negative means "leave the game's value alone", which is `fWordThreeHoldSec`'s shipped
    // default and the documented way to switch either override off. NaN is treated the same way:
    // `std::stof` cannot produce one from an ordinary typo, but `nan` is a literal it accepts, and
    // writing one into a GMST would make every hold comparison false.
    [[nodiscard]] inline float SanitizeHoldSec(float a_configured) {
        if (!(a_configured > 0.0f)) return 0.0f;  // false for NaN, which is the point
        return a_configured > kHoldOverrideMaxSec ? kHoldOverrideMaxSec : a_configured;
    }

    enum class HoldOverrideAction {
        kLeave,    // the GMST already says what it should; write nothing
        kApply,    // write the configured value
        kRestore,  // write the value captured before our first write, and forget it
    };

    // WHAT THIS SESSION HAS DONE TO ONE GMST. Carried by the caller rather than owned here, so the
    // decision function stays pure and the tests can drive a whole session by hand.
    struct HoldOverrideState {
        bool  applied = false;          // an override of ours is currently standing
        float capturedOriginal = 0.0f;  // the value read immediately before our first write
        float lastWritten = 0.0f;       // what we wrote, so a foreign write is detectable
    };

    struct HoldOverrideDecision {
        HoldOverrideAction action = HoldOverrideAction::kLeave;
        float              value = 0.0f;   // what to write, for kApply and kRestore
        float              previous = 0.0f;  // what the GMST said before, for the log line
        HoldOverrideState  state{};        // store this back before the next call
    };

    // ONE GMST, ONE PASS. Idempotent by construction: called again with the same inputs it returns
    // `kLeave`, which is what lets this run at every shout without logging or writing per shout.
    //
    // `a_wanted` is "the engine is on AND a positive value is configured". `a_current` is what the
    // GMST says right now, read fresh every pass -- never cached, because the two things that can
    // move it out from under us both matter:
    //
    //   - DATA LOAD. `Skyrim.esm` defines these as GMST records, so anything written before the
    //     load is clobbered by it, and CONTEXT.md records that an SKSE `kDataLoaded`
    //     listener did NOT run in this setup -- so there is no lifecycle hook to sequence against.
    //     A clobber is detected here instead, as "the value is no longer what we wrote", and the
    //     override is simply re-applied with the post-load value captured as the new original.
    //   - THE PLAYER'S CONSOLE. A `setgs` mid-session is the same shape and gets the same
    //     treatment, which is correct: the last write wins, and ours is re-asserted at the next
    //     shout rather than fighting theirs on a timer.
    [[nodiscard]] inline HoldOverrideDecision DecideHoldOverride(bool a_wanted, float a_configured,
                                                                 float             a_current,
                                                                 HoldOverrideState a_prior) {
        HoldOverrideDecision out{};
        out.previous = a_current;
        out.state = a_prior;

        // Our write is gone -- a data load or a console `setgs` replaced it. Whatever stands there
        // now is the new baseline, so the old captured original is stale and must not be restored
        // over it later.
        if (out.state.applied && !NearHoldSec(a_current, out.state.lastWritten)) {
            out.state.applied = false;
        }

        if (a_wanted) {
            const bool needsWrite = !NearHoldSec(a_current, a_configured);
            if (!out.state.applied) {
                out.state.capturedOriginal = a_current;
            }
            out.state.applied = true;
            out.state.lastWritten = a_configured;
            out.action = needsWrite ? HoldOverrideAction::kApply : HoldOverrideAction::kLeave;
            out.value = a_configured;
            return out;
        }

        // Not wanted. `bEnabled = 0` promises the INI's own "nothing is intercepted", and a GMST
        // this mod moved is exactly the kind of thing a player ruling it out of a conflict needs
        // gone -- so an override already standing is UNWOUND here rather than merely not renewed.
        if (out.state.applied) {
            out.state.applied = false;
            out.action = HoldOverrideAction::kRestore;
            out.value = out.state.capturedOriginal;
            return out;
        }

        out.action = HoldOverrideAction::kLeave;
        out.value = a_current;
        return out;
    }

    struct HoldPlan {
        HoldOverrideDecision wordTwo{};
        HoldOverrideDecision wordThree{};
        // `fWordThreeHoldSec` was set to something at or below the word-two threshold that will be
        // in force after this pass. The caller warns; the override is dropped, not clamped.
        bool wordThreeRefused = false;
        // What the word-two threshold reads after this pass. The guard above divides on it, and
        // the warning names it.
        float effectiveWordTwoSec = 0.0f;
    };

    // BOTH GMSTS IN ONE PASS, BECAUSE THEY ARE NOT INDEPENDENT. The engine promotes to word 2 at
    // `fShoutTime1` and to word 3 at `fShoutTime2`, so a `fShoutTime2` at or below the effective
    // `fShoutTime1` asks for word 3 no later than word 2 -- an ordering the game has no answer for
    // and which would make a two-word shout unreachable. It is REFUSED rather than clamped: there
    // is no nearby value that is obviously what the player meant (0.41 against a 0.4 word two is a
    // 10 ms window nobody can hit), so the honest outcome is to leave `fShoutTime2` alone and say
    // so. That is the difference from `SanitizeHoldSec`'s ceiling, where the intent IS obvious.
    [[nodiscard]] inline HoldPlan PlanHoldOverrides(bool a_engineEnabled, float a_wordTwoSec,
                                                    float a_wordThreeSec, float a_currentWordTwo,
                                                    float             a_currentWordThree,
                                                    HoldOverrideState a_priorWordTwo,
                                                    HoldOverrideState a_priorWordThree) {
        HoldPlan plan{};

        const float two = SanitizeHoldSec(a_wordTwoSec);
        const float three = SanitizeHoldSec(a_wordThreeSec);

        plan.wordTwo =
            DecideHoldOverride(a_engineEnabled && two > 0.0f, two, a_currentWordTwo, a_priorWordTwo);
        // What word two reads once this pass has run -- the configured value on an apply, the
        // captured original on a restore, and whatever is already there when nothing is written.
        plan.effectiveWordTwoSec = plan.wordTwo.action == HoldOverrideAction::kLeave
                                       ? a_currentWordTwo
                                       : plan.wordTwo.value;

        const bool threeAsked = a_engineEnabled && three > 0.0f;
        plan.wordThreeRefused = threeAsked && three <= plan.effectiveWordTwoSec;

        plan.wordThree = DecideHoldOverride(threeAsked && !plan.wordThreeRefused, three,
                                            a_currentWordThree, a_priorWordThree);
        return plan;
    }
}
