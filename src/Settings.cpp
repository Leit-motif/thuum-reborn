#include "PCH.h"
#include "Settings.h"

#include <atomic>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "ShoutHoldThresholds.h"

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

        // BACK, for `fWordTwoHoldSec` / `fWordThreeHoldSec`. It went with the demotions below and
        // returns with the first float setting that is a player's decision:
        // `SanitizeHoldSec` deals with the values `stof` can legitimately produce and this rule
        // cannot -- a negative, an absurd number, and `nan`.
        float ParseFloat(const std::string& a_value, float a_fallback) {
            try {
                return std::stof(a_value);
            } catch (...) {
                return a_fallback;
            }
        }

        // `fPowerHoldSeconds`, `sWindowSource` and `sResumeMode` are internal constants, so their
        // parsers went with the parse cases rather than sitting unreferenced. The `Describe`
        // overloads below stay: `Log()` still reports these values, which is the point of demoting
        // them rather than deleting them.

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
                    // trace carries six distinct thread ids. `exchange` rather than a
                    // read-then-write, so two threads cannot both decide to log.
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

    namespace {
        // The published snapshot and the hot-path trace mirror. The snapshot starts as the
        // built-in defaults so `Snapshot()` can never return null, whatever races `Load()`.
        std::atomic<std::shared_ptr<const Settings>> g_current{std::make_shared<const Settings>()};
        std::atomic<bool>                            g_traceLive{false};

        void Publish(const Settings& a_settings) {
            g_traceLive.store(a_settings.trace, std::memory_order_relaxed);
            g_current.store(std::make_shared<const Settings>(a_settings));
        }
    }

    std::shared_ptr<const Settings> Settings::Snapshot() { return g_current.load(); }

    bool Settings::TraceLive() { return g_traceLive.load(std::memory_order_relaxed); }

    void Settings::Load() {
        std::ifstream in("Data/SKSE/Plugins/ShoutMCO.ini");
        if (!in) {
            // Once, not per shout: `Load()` runs at every `BeginCastVoice`, and a per-shout warn
            // for a deliberately deleted INI is the same log-volume trap the unknown-keys warn
            // below already guards against. The defaults still apply -- they are the published
            // snapshot's starting state, and a missing file deliberately does not overwrite a
            // config the session already loaded.
            static std::atomic<bool> warnedMissing{false};
            if (!warnedMissing.exchange(true)) {
                log::warn("[ShoutMCO] no ShoutMCO.ini found; running on built-in defaults");
            }
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

            // EIGHT settings, and deliberately only eight -- see the class comment on `Settings`.
            // Everything else the engine reads is an internal constant with no parse case, so a
            // stale hand-edited INI falls through to the warning below and runs on the default
            // rather than silently moving a measured value.
            //
            // The seventh branch sets nothing: it swallows a retired key so an INI from the
            // previous release does not warn. Count the assignments, not the branches.
            if (key == "bEnabled") {
                s.enabled = ParseBool(value, s.enabled);
            } else if (key == "bChainDriverCasts") {
                // PARSED BUT NOT SHIPPED -- there is no case for it in
                // `config/ShoutMCO.ini`, deliberately, and the field's comment in `Settings.h`
                // says why. It is here so a live session can flip the feature between shouts; it
                // is out of the shipped file so an unconfirmed feature is not offered as a knob.
                //
                // So this branch does NOT make the shipped count seven. Count what the shipped
                // INI documents, not what `Load()` will accept.
                s.chainDriverCasts = ParseBool(value, s.chainDriverCasts);
            } else if (key == "bTrace") {
                s.trace = ParseBool(value, s.trace);
            } else if (key == "bShoutWaitsForSwing") {
                s.shoutWaitsForSwing = ParseBool(value, s.shoutWaitsForSwing);
            } else if (key == "iChainWindowPct") {
                // Clamped into 0..100 rather than rejected. A negative window has an obvious intent
                // -- "no window" -- and 0 already means exactly that, so honouring it beats warning
                // about it.
                //
                // ABOVE 100 THE CLAMP IS LOAD-BEARING, which is a correction: this said the engine
                // "already handles" it "by opening at spellfire", so clamping only "says the same
                // thing without relying on that path". That is true only while the resulting window
                // stays under `kWindowCeilingMs`. On Goetia's three-word tail -- unmeasured, but
                // several seconds -- 150% asks for a window longer than the tail, the ceiling holds
                // it at 2000 ms, and the engine then SCHEDULES an open partway in: a window in the
                // middle of the shout rather than the whole of it. Without this clamp an
                // out-of-range percentage would not degrade to the documented "opens at spellfire"
                // behaviour at all.
                //
                // Not `std::clamp`: `min`/`max` are macros here, courtesy of the Windows headers.
                const int parsed = ParseInt(value, s.chainWindowPct);
                s.chainWindowPct = parsed < 0 ? 0 : (parsed > 100 ? 100 : parsed);
            } else if (key == "fWordTwoHoldSec") {
                // NOT sanitized here, deliberately. The clamp and the 0-means-off rule live in
                // `SanitizeHoldSec`, next to the guard that compares the two thresholds against each
                // other -- splitting them across the parser and the planner is how the pair drifts
                // into disagreeing about what "off" means. `Log()` therefore reports what the file
                // says, and `ApplyHoldOverrides` reports what the game got.
                s.wordTwoHoldSec = ParseFloat(value, s.wordTwoHoldSec);
            } else if (key == "fWordThreeHoldSec") {
                s.wordThreeHoldSec = ParseFloat(value, s.wordThreeHoldSec);
            } else if (key == "iPowerAdvanceWaitMs") {
                // Negatives fold to 0, which is the documented "no wait" and restores
                // the immediate-cut-at-hit exactly -- same reasoning as `iChainWindowPct` above, where a
                // negative has one obvious intent and a value already means it.
                //
                // Capped at `shoutWaitCapMs` rather than left open. Above that the watchdog that
                // bounds a queued press would fire while this wait is still running, and the press
                // would be force-released out from under a cut that has not happened -- two caps
                // over one press with opposite outcomes.
                const int parsed = ParseInt(value, s.powerAdvanceWaitMs);
                s.powerAdvanceWaitMs =
                    parsed < 0 ? 0 : (parsed > s.shoutWaitCapMs ? s.shoutWaitCapMs : parsed);
            } else if (key == "iChainWindowMs") {
                // SILENTLY IGNORED, and deliberately NOT converted.
                //
                // Same upgrade path as `sShoutGate` below and for the same reason -- every 1.0.2
                // user has this key, and letting it fall through to the unknown-key warn would
                // scold them about a setting they never chose to set.
                //
                // NOT CONVERTED, which is the part worth arguing. Turning a stored 300 into a
                // percentage needs a tail length to divide by, and at parse time there is none --
                // `Load()` runs at `BeginCastVoice`, before this shout's tail is measured, and the
                // cache may be empty or hold another shout's. Any conversion would therefore have
                // to pick a reference clip, and picking one would silently give two users with the
                // same INI different windows depending on which pack they installed. The default
                // is a better answer than a guess dressed as a migration: 30 reproduces the shipped
                // 300 ms to within about a frame on the shout that ruling was taken on.
                //
                // Delete this a release after 1.0.3, on the same schedule as `sShoutGate`.
            } else if (key == "sShoutGate") {
                // SILENTLY IGNORED, and this is the whole of the upgrade path.
                //
                // Every 1.0.1 user has `sShoutGate = hit` in their INI, because that is what 1.0.1
                // shipped. Removing the setting without this case sends all of them through the
                // unknown-key warn below on their first shout after updating -- a warning about a
                // setting they never chose to set, and one that is deliberately NOT gated on
                // `bTrace`. The atomic-hash guard bounds the volume to one line rather
                // than one per shout, but the right number of lines here is zero.
                //
                // Two lines, and they buy a clean upgrade. Delete them a release after 1.0.2, once
                // an INI carrying this key is old enough to be worth telling its owner about.
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
        // a per-shout log-volume bug wearing a diagnostic's clothes -- and demoting fourteen
        // settings at once made it far more likely, so anyone whose INI predates 1.0.0
        // would have paid up to fourteen warnings on every shout. Warnings are deliberately NOT
        // gated on `bTrace`, which is exactly why this one has to hold itself to the same standard
        // the `>>> CHAIN` lines and the config dump were held to.
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

        Publish(s);

        // AFTER `Publish`, so the override that lands is the one the snapshot reports and a bug
        // report cannot show a config line disagreeing with the GMSTs the same shout ran on.
        ApplyHoldOverrides();
    }

    // The two GMSTs this mod moves, and everything the session remembers about having moved them.
    //
    // FILE-SCOPE SESSION STATE, GUARDED BY ITS OWN MUTEX. `Load()` is reached from `BeginShout` on
    // the animation-graph event path, which is not single-threaded -- one chain trace carries six
    // distinct thread ids -- and the shout-input hook calls this from the
    // input thread as well. Unlike the atomic-hash log guards elsewhere in this file, the state
    // here is a read-modify-write over three fields that must move together, so it takes a lock
    // rather than an atomic. It is uncontended in practice: the only callers are a shout press and
    // a shout start.
    //
    // Deliberately NOT part of the published `Settings` snapshot. The snapshot is immutable by
    // design and this is mutable per-session history, not configuration.
    namespace {
        std::mutex        g_holdOverrideMutex;
        HoldOverrideState g_wordTwoState;
        HoldOverrideState g_wordThreeState;

        // `nullptr` before data load, and on any runtime that does not carry the setting. Both are
        // ordinary states rather than errors here: `ApplyHoldOverrides` runs at every shout, so a
        // pass that finds nothing to write is simply a pass that changes nothing and tries again at
        // the next one.
        RE::Setting* FindGameSetting(const char* a_name) {
            auto* collection = RE::GameSettingCollection::GetSingleton();
            if (!collection) return nullptr;
            return collection->GetSetting(a_name);
        }

        // One `log::info` per actual change, and never one per shout. `ApplyHoldOverrides` returns
        // `kLeave` on every pass after the first, so this fires when the override lands, when the
        // player retunes the INI between shouts, when a data load clobbers it and it is re-asserted,
        // and when `bEnabled = 0` unwinds it -- which is the full list of things worth a line.
        //
        // STARTUP CLASS, NOT TRACE. It is a change this mod made to the player's game settings, so
        // it belongs in the log a bug report carries at `bTrace = 0`.
        void LogHoldOverride(const char* a_gmst, float a_vanilla,
                             const HoldOverrideDecision& a_decision) {
            switch (a_decision.action) {
                case HoldOverrideAction::kApply:
                    // The vanilla value is named alongside the old one because they are usually but
                    // NOT always the same number: a data-load clobber and a console `setgs` both
                    // arrive here as an ordinary re-apply, and "old" is then whatever they left.
                    log::info("[ShoutMCO] {} {:.3f} -> {:.3f} (vanilla {:.3f})", a_gmst,
                              a_decision.previous, a_decision.value, a_vanilla);
                    break;
                case HoldOverrideAction::kRestore:
                    log::info("[ShoutMCO] {} restored to {:.3f} from {:.3f} -- override withdrawn",
                              a_gmst, a_decision.value, a_decision.previous);
                    break;
                default:
                    break;
            }
        }
    }

    void Settings::ApplyHoldOverrides() {
        auto* wordTwo = FindGameSetting("fShoutTime1");
        auto* wordThree = FindGameSetting("fShoutTime2");
        // Before data load there is no collection and no setting. Nothing is captured and nothing
        // is remembered, so the next pass starts clean rather than from a half-read state.
        if (!wordTwo || !wordThree) return;

        const auto settings = Snapshot();

        HoldPlan plan{};
        {
            std::scoped_lock lock(g_holdOverrideMutex);

            plan = PlanHoldOverrides(settings->enabled, settings->wordTwoHoldSec,
                                     settings->wordThreeHoldSec, wordTwo->data.f, wordThree->data.f,
                                     g_wordTwoState, g_wordThreeState);

            // THE WRITE ITSELF IS UNDER THE LOCK, with the read it was decided from. Outside it,
            // two threads could each read the same pre-override value, both decide to write, and
            // the second would capture the FIRST's write as the player's original -- so a later
            // restore would put our own number back and call it vanilla.
            if (plan.wordTwo.action != HoldOverrideAction::kLeave) {
                wordTwo->data.f = plan.wordTwo.value;
            }
            if (plan.wordThree.action != HoldOverrideAction::kLeave) {
                wordThree->data.f = plan.wordThree.value;
            }
            g_wordTwoState = plan.wordTwo.state;
            g_wordThreeState = plan.wordThree.state;
        }

        LogHoldOverride("fShoutTime1", kVanillaWordTwoHoldSec, plan.wordTwo);
        LogHoldOverride("fShoutTime2", kVanillaWordThreeHoldSec, plan.wordThree);

        if (plan.wordThreeRefused) {
            // Same atomic-hash guard as the unknown-keys warn, and for the same reason: this is
            // reached at every shout, so a refusal the player has not fixed would otherwise be a
            // per-shout warning.
            auto warning = std::format(
                "[ShoutMCO] fWordThreeHoldSec = {:.3f} is at or below the word-two threshold "
                "({:.3f}), which would put word three no later than word two -- fShoutTime2 left "
                "alone. Set it above fWordTwoHoldSec, or to 0 to leave the game's {:.3f}.",
                settings->wordThreeHoldSec, plan.effectiveWordTwoSec, kVanillaWordThreeHoldSec);
            static std::atomic<std::size_t> lastWarned{0};
            const std::size_t               hash = std::hash<std::string>{}(warning);
            if (lastWarned.exchange(hash) != hash) {
                log::warn("{}", warning);
            }
        }
    }

    // Emitted once, then only when something actually changed.
    //
    // `Load()` runs at the start of EVERY shout. `ShoutChainEngine` still guards that call with
    // `if (settings.reloadPerShout)`, but `reloadPerShout` is an internal constant
    // and nothing can set it false, so treat the reload as unconditional. This line is
    // ~450 bytes. Logging it unconditionally put half a kilobyte per shout into a shipped mod's
    // log even at `bTrace = 0` -- a trace-off run found the reload
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
    // not single-threaded: one chain trace carries six distinct thread ids.
    // Concurrent assignment of a `std::string` is a heap race,
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
            // `chainDriverCasts` sits with the ini fields because a session CAN set it, even
            // though the shipped file does not mention it. A bug report from a
            // harness needs to show which side of that switch the run was on -- it is the
            // difference between "the engine did not arm" and "the engine was not allowed to".
            "[ShoutMCO] config (ini): enabled={} trace={} shoutWaitsForSwing={} chainWindowPct={} "
            "powerSource={}->{} "
            // As the FILE says them, not as the game got them -- `ApplyHoldOverrides` logs the
            // landed values separately, and a bug report wants both so a refused or clamped value
            // is visible as the difference between the two lines.
            "wordTwoHoldSec={:.3f} wordThreeHoldSec={:.3f} "
            "powerKey={} chainDriverCasts={} powerAdvanceWaitMs={} | (internal): reloadPerShout={} window={} ('{}') bufferMs={} cut='{}' "
            "attack='{}' power='{}' holdSecs={:.2f} resume={} readyWindowMs={} motionWatchMs={} "
            "waitCapMs={} pressCapMs={} shoutCapMs={}",
            enabled, trace, shoutWaitsForSwing, chainWindowPct, Describe(powerSource),
            Describe(resolvedPowerSource), wordTwoHoldSec, wordThreeHoldSec,
            powerAttackKeycode, chainDriverCasts, powerAdvanceWaitMs, reloadPerShout, Describe(windowSource),
            windowEvent, bufferMs,
            cutEvent, attackEvent, powerAttackEvent, powerHoldSeconds, Describe(resumeMode),
            readyWindowMs, motionWatchMs, shoutWaitCapMs, pressOwnershipCapMs, shoutLivenessCapMs);

        static std::atomic<std::size_t> lastLogged{0};
        const std::size_t hash = std::hash<std::string>{}(line);
        if (lastLogged.exchange(hash) == hash) return;
        log::info("{}", line);
    }
}
