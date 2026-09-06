// The shipped INI and the compiled defaults have to be the same numbers.
//
// AN INSTALL WITH NO INI RUNS ON THE COMPILED DEFAULTS. A player who deletes the file, or whose
// mod manager hides it, or whose disk hands back a read error, gets whatever `Settings.h`
// initialises -- while every word of documentation describes what `config/ShoutMCO.ini` says. When
// those two disagree the mod plays differently from its own manual and nothing reports it. That is
// what this test exists to make impossible: the compiled chain window was 30 while the shipped
// file said 45, which is 1.22 s against 1.44 s between one-word shouts.
//
// WHY IT PARSES TEXT INSTEAD OF INCLUDING `Settings.h`. That header includes `PCH.h`, which pulls
// the whole CommonLibSSE surface in, and a host test that dragged the game SDK along to compare
// eight numbers could not run on a machine without it. The struct itself names no game type --
// only its include does -- so its defaults are readable straight from the source. Reading the
// source is also what buys the half a compiled test could not do: a value comparison cannot notice
// that a key exists on one side and not the other, and that is the drift that actually happens.
//
// THREE FILES, THREE FACTS, and all three have to line up:
//   config/ShoutMCO.ini -- what a player is handed.
//   src/Settings.cpp    -- which keys `Load()` assigns from, and to which field.
//   src/Settings.h      -- what that field holds when no INI is read at all.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_ok, const std::string& a_what, int a_line) {
        ++g_checks;
        if (a_ok) return;
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n", a_line, a_what.c_str());
    }

#define CHECK_MSG(expr, msg) Check((expr), (msg), __LINE__)

    std::string Trim(const std::string& a_text) {
        const std::size_t first = a_text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const std::size_t last = a_text.find_last_not_of(" \t\r\n");
        return a_text.substr(first, last - first + 1);
    }

    std::string Lower(std::string a_text) {
        for (auto& c : a_text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return a_text;
    }

    bool IsIdentifier(const std::string& a_text) {
        if (a_text.empty()) return false;
        if (!std::isalpha(static_cast<unsigned char>(a_text[0])) && a_text[0] != '_') return false;
        for (char c : a_text) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        }
        return true;
    }

    std::vector<std::string> ReadLines(const std::string& a_path) {
        std::ifstream file(a_path);
        std::vector<std::string> lines;
        if (!file) {
            std::printf("  FAIL: cannot open %s\n", a_path.c_str());
            ++g_failures;
            return lines;
        }
        std::string line;
        while (std::getline(file, line)) lines.push_back(line);
        return lines;
    }

    // --- the three files ------------------------------------------------------------------

    // `key = value`, minus `;` comments and `[sections]` -- the shipped file's own format.
    std::map<std::string, std::string> ParseIni(const std::string& a_path) {
        std::map<std::string, std::string> values;
        for (const auto& raw : ReadLines(a_path)) {
            const std::string line = Trim(raw);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq));
            if (!IsIdentifier(key)) continue;
            values[key] = Trim(line.substr(eq + 1));
        }
        return values;
    }

    struct Declaration {
        std::string type;
        std::string value;
    };

    // `<type> <name> = <value>;` inside the struct. Function declarations carry parentheses and are
    // skipped; the enums declare no initialisers, so they cannot be mistaken for fields.
    std::map<std::string, Declaration> ParseDefaults(const std::string& a_path) {
        std::map<std::string, Declaration> fields;
        for (const auto& raw : ReadLines(a_path)) {
            const std::string line = Trim(raw);
            if (line.rfind("//", 0) == 0 || line.rfind("*", 0) == 0 || line.rfind("#", 0) == 0) {
                continue;
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string left = Trim(line.substr(0, eq));
            if (left.find('(') != std::string::npos || left.find('{') != std::string::npos) continue;

            std::vector<std::string> tokens;
            std::size_t pos = 0;
            while (pos < left.size()) {
                const std::size_t next = left.find_first_of(" \t", pos);
                const std::string token = left.substr(pos, next - pos);
                if (!token.empty()) tokens.push_back(token);
                if (next == std::string::npos) break;
                pos = next + 1;
            }
            if (tokens.size() < 2) continue;
            const std::string name = tokens.back();
            if (!IsIdentifier(name)) continue;

            std::string value = Trim(line.substr(eq + 1));
            const std::size_t semi = value.find(';');
            if (semi == std::string::npos) continue;  // multi-line initialiser: not a plain default
            value = Trim(value.substr(0, semi));
            if (value.empty()) continue;

            std::string type = tokens[0];
            for (std::size_t i = 1; i + 1 < tokens.size(); ++i) type += " " + tokens[i];
            fields[name] = Declaration{type, value};
        }
        return fields;
    }

    struct ParseCase {
        std::string field;  // empty when the branch consumes the key and assigns nothing
    };

    // `Load()`'s if/else chain is the authority on which INI key reaches which field. A branch that
    // assigns nothing is a RETIRED key, kept only so an old file does not warn; this test holds
    // those to their own rule below.
    std::map<std::string, ParseCase> ParseLoadCases(const std::string& a_path) {
        std::map<std::string, ParseCase> cases;
        std::string current;
        for (const auto& raw : ReadLines(a_path)) {
            const std::string line = Trim(raw);
            const std::size_t marker = line.find("(key == \"");
            if (marker != std::string::npos) {
                const std::size_t start = marker + 9;
                const std::size_t end = line.find('"', start);
                if (end != std::string::npos) {
                    current = line.substr(start, end - start);
                    cases[current] = ParseCase{};
                    continue;
                }
            }
            if (current.empty() || line.rfind("//", 0) == 0) continue;
            if (!cases[current].field.empty()) continue;
            if (line.rfind("s.", 0) != 0) continue;

            std::size_t cursor = 2;
            while (cursor < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[cursor])) || line[cursor] == '_')) {
                ++cursor;
            }
            const std::string field = line.substr(2, cursor - 2);
            const std::string rest = Trim(line.substr(cursor));
            if (rest.rfind("=", 0) != 0 || rest.rfind("==", 0) == 0) continue;
            if (IsIdentifier(field)) cases[current].field = field;
        }
        return cases;
    }

    // --- comparing an INI string against a C++ initialiser ---------------------------------

    bool BoolFromIni(const std::string& a_value, bool& a_out) {
        const std::string v = Lower(a_value);
        if (v == "1" || v == "true" || v == "yes" || v == "on") {
            a_out = true;
            return true;
        }
        if (v == "0" || v == "false" || v == "no" || v == "off") {
            a_out = false;
            return true;
        }
        return false;
    }

    bool NumberFrom(std::string a_value, double& a_out) {
        if (!a_value.empty() && (a_value.back() == 'f' || a_value.back() == 'F')) a_value.pop_back();
        if (a_value.empty()) return false;
        char* end = nullptr;
        const double parsed = std::strtod(a_value.c_str(), &end);
        if (end == a_value.c_str() || *end != '\0') return false;
        a_out = parsed;
        return true;
    }

    // `PowerSource::kAuto` is what the field says; `auto` is what the file says. The INI's own
    // vocabulary is the enumerator minus its `k`, lowercased.
    std::string EnumeratorWord(const std::string& a_initialiser) {
        const std::size_t scope = a_initialiser.rfind("::");
        std::string word =
            scope == std::string::npos ? a_initialiser : a_initialiser.substr(scope + 2);
        if (!word.empty() && word[0] == 'k') word.erase(0, 1);
        return Lower(word);
    }

    bool Agrees(const Declaration& a_field, const std::string& a_iniValue, std::string& a_detail) {
        if (a_field.type == "bool") {
            bool fromIni = false;
            if (!BoolFromIni(a_iniValue, fromIni)) {
                a_detail = "the ini value is not a boolean";
                return false;
            }
            const bool compiled = a_field.value == "true";
            a_detail = std::string("compiled ") + (compiled ? "true" : "false") + ", ini " +
                       (fromIni ? "true" : "false");
            return compiled == fromIni;
        }
        if (a_field.type == "int" || a_field.type == "float" || a_field.type == "double") {
            double compiled = 0.0;
            double fromIni = 0.0;
            if (!NumberFrom(a_field.value, compiled) || !NumberFrom(a_iniValue, fromIni)) {
                a_detail = "one side is not a number";
                return false;
            }
            a_detail = "compiled " + a_field.value + ", ini " + a_iniValue;
            const double diff = compiled - fromIni;
            return (diff < 0 ? -diff : diff) < 1e-6;
        }
        if (a_field.type == "std::string") {
            std::string compiled = a_field.value;
            if (compiled.size() >= 2 && compiled.front() == '"' && compiled.back() == '"') {
                compiled = compiled.substr(1, compiled.size() - 2);
            }
            a_detail = "compiled '" + compiled + "', ini '" + a_iniValue + "'";
            return compiled == a_iniValue;
        }
        // Anything else is an enum: compare the words, not the spellings.
        const std::string compiled = EnumeratorWord(a_field.value);
        a_detail = "compiled '" + compiled + "', ini '" + Lower(a_iniValue) + "'";
        return compiled == Lower(a_iniValue);
    }
}

int main() {
    const std::string root = SHOUTMCO_REPO_ROOT;
    const auto ini = ParseIni(root + "/config/ShoutMCO.ini");
    const auto defaults = ParseDefaults(root + "/src/Settings.h");
    const auto cases = ParseLoadCases(root + "/src/Settings.cpp");

    std::printf("ShoutMCO settings / ini parity\n\n");

    if (ini.empty() || defaults.empty() || cases.empty()) {
        std::printf("  FAIL: one of the three files parsed to nothing "
                    "(ini %zu, defaults %zu, parse cases %zu)\n",
                    ini.size(), defaults.size(), cases.size());
        std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures + 1);
        return 1;
    }

    // Keys `Load()` accepts on purpose and the shipped file deliberately does not document. A
    // setting ships when a player can feel it without opening a log; these two are harness switches
    // for features whose acceptance is still open, and they are named here so that adding to this
    // list is a decision somebody makes rather than one that happens.
    //
    // FOUR MORE JOINED THEM WHEN THE MENU WAS PRUNED. Owner ruling: a setting the author has to
    // reload the concept of is not a setting. `iChainWindowPct` trades responsiveness against how
    // much of the shout animation you see, which is one number pulling against both things the mod
    // is for; `bShoutWaitsForSwing` off is not a preference but a different mod; the word-three
    // hold only means anything after moving word two a long way; and the power keycode is the
    // escape hatch for `sPowerSource`, not a choice. All four still parse, so development and a
    // support case can still reach them. None is offered to a player.
    const std::set<std::string> unshippedOnPurpose{
        "bChainDriverCasts",   "iPowerAdvanceWaitMs", "iChainWindowPct",
        "bShoutWaitsForSwing", "fWordThreeHoldSec",   "iPowerAttackKeycode"};

    std::printf("every shipped key parses, and its default matches what the file says\n");
    for (const auto& [key, value] : ini) {
        const auto parsed = cases.find(key);
        if (parsed == cases.end()) {
            CHECK_MSG(false, "the ini ships '" + key + "' and Load() has no case for it");
            continue;
        }
        if (parsed->second.field.empty()) {
            CHECK_MSG(false,
                      "the ini ships '" + key + "' and Load() consumes it without assigning");
            continue;
        }
        const auto field = defaults.find(parsed->second.field);
        if (field == defaults.end()) {
            CHECK_MSG(false, "'" + key + "' assigns Settings::" + parsed->second.field +
                                 ", which has no default in Settings.h");
            continue;
        }
        // Sequenced deliberately: `Agrees` fills `detail`, and both would otherwise be arguments
        // to one call, whose evaluation order is unspecified -- which prints an empty diagnostic
        // exactly on the runs that need one.
        std::string detail;
        const bool agrees = Agrees(field->second, value, detail);
        CHECK_MSG(agrees, "'" + key + "' -> " + parsed->second.field + ": " + detail);
    }

    std::printf("every assigning key is either shipped or listed as deliberately unshipped\n");
    for (const auto& [key, parsed] : cases) {
        if (parsed.field.empty() || ini.count(key) != 0) continue;
        CHECK_MSG(unshippedOnPurpose.count(key) != 0,
                  "Load() assigns from '" + key + "', the shipped ini never mentions it, and it is "
                  "not on the deliberately-unshipped list");
    }

    std::printf("the deliberately-unshipped list names only keys that still parse\n");
    for (const auto& key : unshippedOnPurpose) {
        const auto parsed = cases.find(key);
        CHECK_MSG(parsed != cases.end() && !parsed->second.field.empty(),
                  "'" + key + "' is listed as deliberately unshipped and Load() no longer assigns "
                  "from it");
        CHECK_MSG(ini.count(key) == 0,
                  "'" + key + "' is listed as deliberately unshipped and the shipped ini documents "
                  "it");
    }

    std::printf("a retired key is consumed silently and never shipped\n");
    for (const auto& [key, parsed] : cases) {
        if (!parsed.field.empty()) continue;
        CHECK_MSG(ini.count(key) == 0,
                  "'" + key + "' is consumed without assigning and the shipped ini still documents "
                  "it");
    }

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
