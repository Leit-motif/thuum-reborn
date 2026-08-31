#pragma once

#include <string>
#include <string_view>
#include <vector>

// EDIT ONE VALUE IN AN INI WITHOUT REWRITING THE FILE.
//
// `config/ShoutMCO.ini` is about 90% comment by line count, and those comments are the whole of
// what a player has to go on -- there is no manual anywhere else. A settings UI that serialised the
// live struct back over the file would deliver a correct set of numbers and delete every word
// explaining them, on the first click. So the tuning menu edits the file the way a person would:
// find the line, change the number after the `=`, leave everything else exactly as it was.
//
// TEXT IN, TEXT OUT, AND NO GAME TYPES. The whole of the risk here is the string handling, which
// is why it lives in its own header behind a pure function -- `tests/ini_rewrite_tests.cpp`
// exercises it on a host with no Skyrim SDK present.
//
// EVERY OCCURRENCE OF THE KEY IS REWRITTEN, not the first. `Settings::Load()` walks the file top to
// bottom and assigns as it goes, so on a file that somehow carries a key twice the LAST line wins.
// Rewriting only the first would leave a stale duplicate below it silently overriding what the
// player just chose.
namespace ShoutMCO::IniRewrite {
    namespace detail {
        inline std::string_view TrimView(std::string_view a_text) {
            const auto first = a_text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) return {};
            const auto last = a_text.find_last_not_of(" \t\r\n");
            return a_text.substr(first, last - first + 1);
        }

        // The file's own line ending, so an edit does not leave one LF in a CRLF file. Windows
        // Notepad renders that as a joined line, and this file is meant to be hand-editable.
        inline std::string_view LineEnding(std::string_view a_text) {
            return a_text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        }

        inline std::vector<std::string> SplitLines(std::string_view a_text, bool& a_trailingNewline) {
            std::vector<std::string> lines;
            std::size_t              start = 0;
            while (start <= a_text.size()) {
                const auto  nl = a_text.find('\n', start);
                std::string line{a_text.substr(start, nl == std::string_view::npos ? nl : nl - start)};
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
                if (nl == std::string_view::npos) break;
                start = nl + 1;
            }
            // A file ending in a newline splits to a final empty element. Record that and drop it,
            // so a round trip through here does not grow a blank line on every save.
            a_trailingNewline = !lines.empty() && lines.back().empty();
            if (a_trailingNewline) lines.pop_back();
            return lines;
        }

        // `[Section]` -> `Section`, or empty for any other line.
        inline std::string_view SectionName(std::string_view a_line) {
            const auto trimmed = TrimView(a_line);
            if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') return {};
            return trimmed.substr(1, trimmed.size() - 2);
        }

        // The key of a `key = value` line, or empty for a comment, a blank or a section header.
        // Exact-match semantics, matching `Settings::Load()`: no case folding, no substrings.
        inline std::string_view KeyOf(std::string_view a_line) {
            const auto trimmed = TrimView(a_line);
            if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' ||
                trimmed.front() == '[') {
                return {};
            }
            const auto eq = trimmed.find('=');
            if (eq == std::string_view::npos) return {};
            return TrimView(trimmed.substr(0, eq));
        }

        inline std::string Indentation(std::string_view a_line) {
            const auto first = a_line.find_first_not_of(" \t");
            return std::string{a_line.substr(0, first == std::string_view::npos ? 0 : first)};
        }
    }

    // Return `a_text` with `a_key` set to `a_value`.
    //
    // The key is looked for inside `a_section` only, because a key name is only unique within one.
    // Missing key: appended at the end of that section, before its trailing blank lines so the gap
    // between sections survives. Missing section: a new `[section]` block at the end of the file.
    [[nodiscard]] inline std::string SetValue(std::string_view a_text, std::string_view a_section,
                                              std::string_view a_key, std::string_view a_value) {
        const auto  eol = detail::LineEnding(a_text);
        bool        trailingNewline = false;
        auto        lines = detail::SplitLines(a_text, trailingNewline);
        std::string current;
        bool        wrote = false;

        // The line after the section's last content line -- where an absent key is appended.
        std::size_t insertAt = lines.size();
        bool        sectionSeen = false;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto section = detail::SectionName(lines[i]);
            if (!section.empty()) {
                current = std::string{section};
                if (current == a_section) {
                    sectionSeen = true;
                    insertAt = i + 1;
                }
                continue;
            }
            if (current != a_section) continue;
            if (!detail::TrimView(lines[i]).empty()) insertAt = i + 1;
            if (detail::KeyOf(lines[i]) != a_key) continue;

            lines[i] = detail::Indentation(lines[i]) + std::string{a_key} + " = " + std::string{a_value};
            wrote = true;
        }

        if (!wrote) {
            if (!sectionSeen) {
                if (!lines.empty() && !detail::TrimView(lines.back()).empty()) lines.emplace_back();
                lines.emplace_back("[" + std::string{a_section} + "]");
                insertAt = lines.size();
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt),
                         std::string{a_key} + " = " + std::string{a_value});
        }

        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            out += lines[i];
            if (i + 1 < lines.size() || trailingNewline) out += eol;
        }
        return out;
    }
}
