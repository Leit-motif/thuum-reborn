// The tuning menu writes the player's INI. This is the part that can corrupt it.
//
// The shipped `ShoutMCO.ini` is mostly prose -- the comments are the only manual the mod has -- so
// a save that reformats the file is a save that deletes the documentation. These cases hold the
// rewriter to editing exactly one line and leaving every other byte alone, including the ones that
// are easy to lose by accident: the comment blocks, the section gaps, CRLF endings, and the
// duplicate key that `Settings::Load()` would resolve in the opposite direction from the naive fix.

#include "IniRewrite.h"

#include <cstdio>
#include <string>

namespace {
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_ok, const std::string& a_what, int a_line) {
        ++g_checks;
        if (a_ok) return;
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n", a_line, a_what.c_str());
    }

    void CheckEqual(const std::string& a_got, const std::string& a_want, const std::string& a_what,
                    int a_line) {
        ++g_checks;
        if (a_got == a_want) return;
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n    want: [%s]\n    got:  [%s]\n", a_line, a_what.c_str(),
                    a_want.c_str(), a_got.c_str());
    }

#define CHECK_MSG(expr, msg) Check((expr), (msg), __LINE__)
#define CHECK_EQ(got, want, msg) CheckEqual((got), (want), (msg), __LINE__)

    using ShoutMCO::IniRewrite::SetValue;
}

int main() {
    std::printf("ShoutMCO ini rewrite\n\n");

    std::printf("an existing value is replaced and its comments survive\n");
    {
        const std::string before =
            "; Shouts for MCO\n"
            "\n"
            "[Chain]\n"
            "\n"
            "; How much of the END of a shout you can cancel into an attack.\n"
            ";    45 = default.\n"
            "iChainWindowPct = 45\n";
        const std::string after =
            "; Shouts for MCO\n"
            "\n"
            "[Chain]\n"
            "\n"
            "; How much of the END of a shout you can cancel into an attack.\n"
            ";    45 = default.\n"
            "iChainWindowPct = 60\n";
        CHECK_EQ(SetValue(before, "Chain", "iChainWindowPct", "60"), after,
                 "only the value changed");
    }

    std::printf("the same key in another section is not touched\n");
    {
        const std::string before =
            "[Engine]\n"
            "bEnabled = 1\n"
            "\n"
            "[Chain]\n"
            "bEnabled = 0\n";
        const std::string after =
            "[Engine]\n"
            "bEnabled = 1\n"
            "\n"
            "[Chain]\n"
            "bEnabled = 1\n";
        CHECK_EQ(SetValue(before, "Chain", "bEnabled", "1"), after,
                 "the [Engine] copy is left alone");
    }

    std::printf("a duplicated key is rewritten everywhere, not just the first time\n");
    {
        // `Load()` assigns as it walks, so the LAST line wins. Rewriting only the first would
        // leave the player's new value silently overridden by the stale one below it.
        const std::string before =
            "[Chain]\n"
            "iChainWindowPct = 45\n"
            "; someone pasted it twice\n"
            "iChainWindowPct = 30\n";
        const std::string after =
            "[Chain]\n"
            "iChainWindowPct = 60\n"
            "; someone pasted it twice\n"
            "iChainWindowPct = 60\n";
        CHECK_EQ(SetValue(before, "Chain", "iChainWindowPct", "60"), after,
                 "both copies carry the new value");
    }

    std::printf("an absent key is appended inside its section, ahead of the next one\n");
    {
        const std::string before =
            "[Chain]\n"
            "iChainWindowPct = 45\n"
            "\n"
            "[Shout]\n"
            "fWordTwoHoldSec = 0.4\n";
        const std::string after =
            "[Chain]\n"
            "iChainWindowPct = 45\n"
            "iPowerAdvanceWaitMs = 350\n"
            "\n"
            "[Shout]\n"
            "fWordTwoHoldSec = 0.4\n";
        CHECK_EQ(SetValue(before, "Chain", "iPowerAdvanceWaitMs", "350"), after,
                 "appended after the last line of [Chain], keeping the blank line");
    }

    std::printf("an absent section is created at the end\n");
    {
        const std::string before =
            "[Engine]\n"
            "bEnabled = 1\n";
        const std::string after =
            "[Engine]\n"
            "bEnabled = 1\n"
            "\n"
            "[Chain]\n"
            "iChainWindowPct = 45\n";
        CHECK_EQ(SetValue(before, "Chain", "iChainWindowPct", "45"), after,
                 "a new section block, one blank line clear of the last one");
    }

    std::printf("CRLF in, CRLF out\n");
    {
        const std::string before = "[Engine]\r\nbEnabled = 1\r\n";
        const std::string after = "[Engine]\r\nbEnabled = 0\r\n";
        CHECK_EQ(SetValue(before, "Engine", "bEnabled", "0"), after, "no mixed endings");
        const std::string appended = SetValue(before, "Engine", "bTrace", "1");
        CHECK_MSG(appended == "[Engine]\r\nbEnabled = 1\r\nbTrace = 1\r\n",
                  "an appended line uses the file's own ending too");
    }

    std::printf("indentation and a missing trailing newline are preserved\n");
    {
        CHECK_EQ(SetValue("[Engine]\n    bEnabled = 1", "Engine", "bEnabled", "0"),
                 "[Engine]\n    bEnabled = 0", "leading whitespace kept, no newline invented");
    }

    std::printf("a commented-out key is not mistaken for the setting\n");
    {
        const std::string before =
            "[Engine]\n"
            "; bEnabled = 0\n"
            "bEnabled = 1\n";
        const std::string after =
            "[Engine]\n"
            "; bEnabled = 0\n"
            "bEnabled = 0\n";
        CHECK_EQ(SetValue(before, "Engine", "bEnabled", "0"), after,
                 "the comment line stays a comment");
    }

    std::printf("a key whose name contains ours is left alone\n");
    {
        // `sAttackEvent` inside `sPowerAttackEvent` is the shape that already bit `Load()` once.
        const std::string before =
            "[Chain]\n"
            "iChainWindowPctExtra = 1\n"
            "iChainWindowPct = 45\n";
        const std::string after =
            "[Chain]\n"
            "iChainWindowPctExtra = 1\n"
            "iChainWindowPct = 60\n";
        CHECK_EQ(SetValue(before, "Chain", "iChainWindowPct", "60"), after, "exact key match only");
    }

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
