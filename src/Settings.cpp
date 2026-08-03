#include "PCH.h"
#include "Settings.h"

#include <atomic>
#include <format>
#include <fstream>
#include <functional>
#include <string>

using namespace SKSE;
using namespace SKSE::log;

namespace ShoutMCO {
    namespace {
        std::string Trim(std::string_view a_s) {
            const auto b = a_s.find_first_not_of(" \t\r\n");
            if (b == std::string_view::npos) return {};
            const auto e = a_s.find_last_not_of(" \t\r\n");
            return std::string{a_s.substr(b, e - b + 1)};
        }

        bool ParseBool(const std::string& a_value, bool a_fallback) {
            try {
                return std::stoi(a_value) != 0;
            } catch (...) {
                return a_fallback;
            }
        }

        int ParseInt(const std::string& a_value, int a_fallback) {
            try {
                return std::stoi(a_value);
            } catch (...) {
                return a_fallback;
            }
        }

        // `ParseFloat`, `ParseWindowSource` and `ParseResumeMode` WERE HERE. They parsed
        // `fPowerHoldSeconds`, `sWindowSource` and `sResumeMode`, all three demoted to internal
        // constants by ticket 17 -- so the parsers went with their parse cases rather than sitting
        // unreferenced. The `Describe` overloads below stay: `Log()` still reports these values,
        // which is the point of demoting them rather than deleting them.

        // One Click Power Attack's own config, so its key is configured in one place rather than
        // copied into ours and left to drift. The player's MCM overrides come first; OCPA's
        // shipped defaults are the fallback, since a player who never opened the MCM has no
        // settings file at all.
        int ReadOcpaKeycode() {
            static constexpr const char* kPaths[]{
                "Data/MCM/Settings/OCPA.ini",
                "Data/MCM/Config/OCPA/settings.ini",
            };

            for (const auto* path : kPaths) {
                std::ifstream in(path);
                if (!in) continue;

                std::string line;
                while (std::getline(in, line)) {
                    const auto trimmed = Trim(line);
                    const auto eq = trimmed.find('=');
                    if (eq == std::string::npos) continue;
                    if (Trim(std::string_view{trimmed}.substr(0, eq)) != "iKeycode") continue;

                    // The first iKeycode is [General]'s; [DualAttack]'s comes later.
                    const auto code = ParseInt(Trim(std::string_view{trimmed}.substr(eq + 1)), 0);
                    // Once, then only on a change. This runs on every shout via the per-shout
                    // reload, and OCPA's key does not move between shouts -- see Settings::Log().
                    //
                    // Atomic because `Load()` is reached from `BeginShout`, i.e. from the
                    // animation-graph event path, and that path is NOT single-threaded: one chain
                    // trace carries six distinct thread ids (measured 2026-08-03). `exchange`
                    // rather than a read-then-write, so two threads cannot both decide to log.
                    static std::atomic<int> lastCode{-1};
                    if (lastCode.exchange(code) != code) {
                        log::info("[ShoutMCO] power key {} read from {}", code, path);
                    }
                    return code;
                }
            }

            static std::atomic<bool> warnedNoOcpa{false};
            if (!warnedNoOcpa.exchange(true)) {
                log::info("[ShoutMCO] no OCPA config found -- treating this as a load order without it");
            }
            return 0;
        }

        Settings::PowerSource ParsePowerSource(const std::string& a_value, Settings::PowerSource a_fallback) {
            if (a_value == "auto") return Settings::PowerSource::kAuto;
            if (a_value == "ocpa") return Settings::PowerSource::kOcpa;
            if (a_value == "hold") return Settings::PowerSource::kHold;
            if (a_value == "off") return Settings::PowerSource::kOff;
            log::warn("[ShoutMCO] unknown sPowerSource '{}'; keeping the previous value", a_value);
            return a_fallback;
        }

        std::string_view Describe(Settings::PowerSource a_source) {
            switch (a_source) {
                case Settings::PowerSource::kAuto:
                    return "auto"sv;
                case Settings::PowerSource::kOcpa:
                    return "ocpa"sv;
                case Settings::PowerSource::kHold:
                    return "hold"sv;
                default:
                    return "off"sv;
            }
        }

        std::string_view Describe(Settings::WindowSource a_source) {
            return a_source == Settings::WindowSource::kGraph ? "graph"sv : "spellfire"sv;
        }

        std::string_view Describe(Settings::ResumeMode a_mode) {
            switch (a_mode) {
                case Settings::ResumeMode::kRestoreNext:
                    return "next"sv;
                case Settings::ResumeMode::kIncrementCurrent:
                    return "increment"sv;
                default:
                    return "off"sv;
            }
        }
    }

    Settings& Settings::Get() {
        static Settings instance;
        return instance;
    }

    void Settings::Load() {
        std::ifstream in("Data/SKSE/Plugins/ShoutMCO.ini");
        if (!in) {
            log::warn("[ShoutMCO] no ShoutMCO.ini found; running on built-in defaults");
            return;
        }

        // Parse into a fresh copy, not over the live one. A per-shout reload has to be able to
        // *revert* a setting -- deleting a line has to mean the default, not the value the last
        // reload happened to leave behind.
        Settings s{};

        // Unknown keys are COLLECTED, not warned inline -- see the emit below for why.
        std::string unknownKeys;

        std::string line;
        while (std::getline(in, line)) {
            const auto trimmed = Trim(line);
            if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '[') continue;

            const auto eq = trimmed.find('=');
            if (eq == std::string::npos) continue;

            // Exact keys, not substring matches. The case that established this is now internal:
            // `sAttackEvent` is a SUBSTRING of `sPowerAttackEvent`, so a substring reader resolved
            // the pair by declaration order. No surviving key contains another, so the hazard is
            // currently latent rather than live -- kept because the fix is free and the next
            // setting added could reintroduce it.
            const auto key = Trim(std::string_view{trimmed}.substr(0, eq));
            const auto value = Trim(std::string_view{trimmed}.substr(eq + 1));
            if (key.empty()) continue;

            // FIVE cases, and deliberately only five -- see the class comment on `Settings`.
            // Everything else the engine reads is an internal constant with no parse case, so a
            // stale hand-edited INI falls through to the warning below and runs on the default
            // rather than silently moving a measured value.
            if (key == "bEnabled") {
                s.enabled = ParseBool(value, s.enabled);
            } else if (key == "bTrace") {
                s.trace = ParseBool(value, s.trace);
            } else if (key == "bShoutWaitsForSwing") {
                s.shoutWaitsForSwing = ParseBool(value, s.shoutWaitsForSwing);
            } else if (key == "sPowerSource") {
                s.powerSource = ParsePowerSource(value, s.powerSource);
            } else if (key == "iPowerAttackKeycode") {
                s.powerAttackKeycode = ParseInt(value, s.powerAttackKeycode);
            } else {
                if (!unknownKeys.empty()) unknownKeys += ", ";
                unknownKeys += key;
            }
        }

        // ONE line for the whole file, and then only when the set of unknown keys CHANGES.
        //
        // `Load()` runs at the start of every shout, so an inline `log::warn` per unknown key was
        // a per-shout log-volume bug wearing a diagnostic's clothes -- and ticket 17 made it far
        // more likely by demoting fourteen settings at once, so anyone whose INI predates 1.0.0
        // would have paid up to fourteen warnings on every shout. Warnings are deliberately NOT
        // gated on `bTrace`, which is exactly why this one has to hold itself to the same standard
        // the `>>> CHAIN` lines and the config dump were held to (ticket 16).
        //
        // Same atomic-hash guard as `Log()`, and for the same reason: this path is reached from
        // `BeginShout` on the animation-graph event path, which carries six distinct thread ids.
        // `exchange` so two threads cannot both decide to log.
        if (!unknownKeys.empty()) {
            auto              warning = std::format("[ShoutMCO] unknown settings ignored: {}", unknownKeys);
            static std::atomic<std::size_t> lastWarned{0};
            const std::size_t hash = std::hash<std::string>{}(warning);
            if (lastWarned.exchange(hash) != hash) {
                log::warn("{} -- these are not settings this version reads; it is running on the "
                          "built-in default for each", warning);
            }
        }

        // Resolve the power source by looking at what is actually installed, so neither answer
        // has to be assumed and no key is hardcoded.
        if (s.powerSource == Settings::PowerSource::kOcpa || s.powerSource == Settings::PowerSource::kAuto) {
            if (s.powerAttackKeycode < 0) {
                s.powerAttackKeycode = ReadOcpaKeycode();
            }
        } else {
            s.powerAttackKeycode = 0;
        }

        switch (s.powerSource) {
            case Settings::PowerSource::kAuto:
                s.resolvedPowerSource = s.powerAttackKeycode > 0 ? Settings::PowerSource::kOcpa
                                                                 : Settings::PowerSource::kHold;
                break;
            case Settings::PowerSource::kOcpa:
                // Asked for explicitly but no key found: fall back rather than silently drop the
                // whole direction.
                s.resolvedPowerSource = s.powerAttackKeycode > 0 ? Settings::PowerSource::kOcpa
                                                                 : Settings::PowerSource::kHold;
                break;
            default:
                s.resolvedPowerSource = s.powerSource;
                break;
        }

        Get() = s;
    }

    // Emitted once, then only when something actually changed.
    //
    // `Load()` runs at the start of EVERY shout. `ShoutChainEngine` still guards that call with
    // `if (settings.reloadPerShout)`, but `reloadPerShout` is an internal constant since ticket 17
    // and nothing can set it false, so treat the reload as unconditional. This line is
    // ~450 bytes. Logging it unconditionally put half a kilobyte per shout into a shipped mod's
    // log even at `bTrace = 0` -- measured 2026-08-03, when a trace-off gate run found the reload
    // pair to be the only thing in the file. That is the same "one line per shout sounds cheap
    // until somebody plays for six hours" argument the `>>> CHAIN` lines were held to, so it is
    // held to it too.
    //
    // Comparing the rendered line rather than tracking a dirty flag keeps the guard honest: any
    // field that reaches the log is a field the comparison covers, so the two cannot drift.
    // A user retuning the INI mid-session still sees the change, which is the whole point of the
    // per-shout reload -- what they no longer get is the identical line repeated.
    //
    // The "have I already logged this" state is an ATOMIC HASH, not a `static std::string`.
    // `Load()` is reached from `BeginShout`, on the animation-graph event path, and that path is
    // not single-threaded: one chain trace carries six distinct thread ids (measured 2026-08-03,
    // and note the comment at the top of ShoutChainEngine.cpp asserting main-thread-only is
    // therefore wrong -- see the ticket). Concurrent assignment of a `std::string` is a heap race,
    // not a benign one. A 64-bit hash compared with `exchange` has no allocation and no torn read,
    // and two threads cannot both decide to log the same line.
    //
    // A hash collision would silently drop one config line. At 64 bits across the handful of
    // distinct configs a session ever sees, that is not a risk worth a lock.
    void Settings::Log() const {
        // Split into the two classes of field, so a bug report says at a glance which values the
        // user could actually have set and which are stock. The internals are still reported in
        // full: "this user is on stock internals" is worth more in a report than a shorter line,
        // and the guard below means it is emitted once rather than per shout.
        auto line = std::format(
            "[ShoutMCO] config (ini): enabled={} trace={} shoutWaitsForSwing={} powerSource={}->{} "
            "powerKey={} | (internal): reloadPerShout={} window={} ('{}') bufferMs={} cut='{}' "
            "attack='{}' power='{}' holdSecs={:.2f} resume={} readyWindowMs={} motionWatchMs={} "
            "waitCapMs={}",
            enabled, trace, shoutWaitsForSwing, Describe(powerSource), Describe(resolvedPowerSource),
            powerAttackKeycode, reloadPerShout, Describe(windowSource), windowEvent, bufferMs,
            cutEvent, attackEvent, powerAttackEvent, powerHoldSeconds, Describe(resumeMode),
            readyWindowMs, motionWatchMs, shoutWaitCapMs);

        static std::atomic<std::size_t> lastLogged{0};
        const std::size_t hash = std::hash<std::string>{}(line);
        if (lastLogged.exchange(hash) == hash) return;
        log::info("{}", line);
    }
}
