#include "PCH.h"
#include "TuningMenu.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

#include "IniRewrite.h"
#include "Settings.h"

// Vendored verbatim from the menu host, and it does not compile clean under this project's /W4:
// one struct/class mismatch and a handful of ImGui's cross-enum flag ors. Pushed and popped rather
// than fixed, so the file stays byte-identical to upstream and re-vendoring is a copy.
#pragma warning(push)
#pragma warning(disable : 4099 5054)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

using namespace SKSE;
using namespace SKSE::log;

// THE MENU IS A FRONT END ONTO THE INI, NOT A SECOND PLACE SETTINGS LIVE.
//
// `Data/SKSE/Plugins/ShoutMCO.ini` stays the one source of truth. Every widget here rewrites the
// line it owns in that file and then calls `Settings::Load()`, which is the same call the engine
// already makes at the start of every shout -- so the menu buys the player the round trip, not a
// new mechanism, and a value set here is indistinguishable from one typed into the file.
//
// The alternative -- keeping the live values in the UI and writing the file on close -- was not
// taken. It puts a second copy of the configuration in memory that the per-shout reload would
// silently overwrite, and it makes the file and the game disagree for as long as the menu is open.
//
// WHAT IS DELIBERATELY NOT ON THIS PAGE. The engine's internal bounds -- the shout wait cap, the
// liveness cap, the press-ownership cap, the ready window, the window floor and ceiling -- are
// measurements, not preferences, and each fails silently when wrong. The wait cap sits above the
// slowest attack in the load order; lower it and a queued shout fires mid-swing, which is the exact
// bug that gate exists to fix. They are shown here as read-only numbers because they are worth
// seeing in a bug report, and they are read-only because a bound that a player can turn is a second
// tuning parameter wearing a safety bound's clothes.
namespace ShoutMCO::TuningMenu {
    namespace {
        constexpr auto kIniPath = "Data/SKSE/Plugins/ShoutMCO.ini";

        // The framework renders each section item from the D3D present hook, so this runs on a
        // thread that is not the engine's. Everything it touches is either its own state or
        // `Settings`, whose snapshot is published atomically and whose `Load()` already serves the
        // animation-event path and the input thread.
        struct UiState {
            bool  synced = false;
            bool  enabled = true;
            bool  trace = false;
            bool  shoutWaitsForSwing = true;
            int   chainWindowPct = 45;
            int   powerAdvanceWaitMs = 350;
            float wordTwoHoldSec = 0.4f;
            float wordThreeHoldSec = 0.0f;
            int   powerSource = 0;

            // The last write's outcome, shown under the buttons. A failed write is otherwise
            // completely silent: the slider stays where the player put it and the game keeps
            // playing the old value.
            std::string status;
        };

        UiState g_ui;

        const char* const kPowerSourceItems[] = {"auto", "ocpa", "hold", "off"};

        int PowerSourceIndex(Settings::PowerSource a_source) {
            switch (a_source) {
                case Settings::PowerSource::kOcpa:
                    return 1;
                case Settings::PowerSource::kHold:
                    return 2;
                case Settings::PowerSource::kOff:
                    return 3;
                default:
                    return 0;
            }
        }

        const char* PowerSourceWord(Settings::PowerSource a_source) {
            return kPowerSourceItems[PowerSourceIndex(a_source)];
        }

        // Pull the widgets back onto whatever the engine last parsed. `Load()` first, so an INI
        // hand-edited since the last shout -- or one never read at all, because no shout has
        // happened this session -- is what the player sees rather than the compiled defaults.
        void SyncFromFile() {
            Settings::Load();
            const auto settings = Settings::Snapshot();
            g_ui.enabled = settings->enabled;
            g_ui.trace = settings->trace;
            g_ui.shoutWaitsForSwing = settings->shoutWaitsForSwing;
            g_ui.chainWindowPct = settings->chainWindowPct;
            g_ui.powerAdvanceWaitMs = settings->powerAdvanceWaitMs;
            g_ui.wordTwoHoldSec = settings->wordTwoHoldSec;
            g_ui.wordThreeHoldSec = settings->wordThreeHoldSec;
            g_ui.powerSource = PowerSourceIndex(settings->powerSource);
            g_ui.synced = true;
        }

        // Rewrite one key and re-read the file. Returns false and leaves the file untouched when
        // either half of the round trip fails.
        bool Write(std::string_view a_section, std::string_view a_key, std::string_view a_value) {
            std::string text;
            {
                // A missing file is not an error: the player may have deleted it, and the engine
                // runs on the compiled defaults when they do. Writing then creates a small file
                // carrying just the keys they have chosen to move.
                std::ifstream in(kIniPath, std::ios::binary);
                if (in) {
                    std::ostringstream buffer;
                    buffer << in.rdbuf();
                    text = buffer.str();
                }
            }

            const auto updated = IniRewrite::SetValue(text, a_section, a_key, a_value);

            std::ofstream out(kIniPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                g_ui.status = std::format("could not write {} -- is it read-only?", kIniPath);
                log::warn("[ShoutMCO] tuning menu could not write {}", kIniPath);
                return false;
            }
            out << updated;
            out.close();
            if (!out) {
                g_ui.status = std::format("write to {} failed part way", kIniPath);
                log::warn("[ShoutMCO] tuning menu write to {} failed", kIniPath);
                return false;
            }

            // Take effect now rather than at the next shout. The engine reloads per shout anyway,
            // so this only removes a wait -- but the wait is the whole reason this page exists.
            SyncFromFile();
            g_ui.status = std::format("{} = {}", a_key, a_value);
            log::info("[ShoutMCO] tuning menu set {} = {}", a_key, a_value);
            return true;
        }

        void WriteBool(std::string_view a_section, std::string_view a_key, bool a_value) {
            Write(a_section, a_key, a_value ? "1" : "0");
        }

        void WriteInt(std::string_view a_section, std::string_view a_key, int a_value) {
            Write(a_section, a_key, std::to_string(a_value));
        }

        void WriteFloat(std::string_view a_section, std::string_view a_key, float a_value) {
            Write(a_section, a_key, std::format("{:.2f}", a_value));
        }

        // A slider reports a change on EVERY frame it is dragged, and the file must not be
        // rewritten at frame rate for the length of a drag. `IsItemDeactivatedAfterEdit` is the
        // whole test on its own -- it is true on the frame a slider the player actually moved is
        // released, and on the frame a typed-in value is committed. The slider's own return value
        // is deliberately not ANDed with it: the two are true on different frames, so the pair
        // would almost never fire together.
        //
        // Read it immediately after the widget, before anything else is submitted -- every
        // `IsItem*` query refers to the last item.
        bool CommittedNow() { return ImGuiMCP::IsItemDeactivatedAfterEdit(); }

        void __stdcall RenderTuning() {
            if (!g_ui.synced) SyncFromFile();

            ImGuiMCP::TextWrapped(
                "These are the values worth tuning by ear. Each one is written straight into "
                "SKSE/Plugins/ShoutMCO.ini and takes effect immediately -- no restart, no new save. "
                "The file keeps its comments, so anything set here can be read back and changed in "
                "a text editor later.");

            ImGuiMCP::SeparatorText("Chain");

            ImGuiMCP::SliderInt("Cancel window (% of shout)", &g_ui.chainWindowPct, 0, 100);
            bool commit = CommittedNow();
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "How much of the END of a shout you can cancel into an attack, as a percentage "
                    "of that shout.\n\n"
                    "Smaller = held through more of the shout, more committed. Larger = you can "
                    "break out earlier, and shout-to-shout chains faster because more of the exhale "
                    "is cut.\n\n"
                    "0 = no window: a buffered attack fires when the shout ends on its own.");
            }
            if (commit) WriteInt("Chain", "iChainWindowPct", g_ui.chainWindowPct);

            if (ImGuiMCP::Checkbox("Shout waits for the swing to land", &g_ui.shoutWaitsForSwing)) {
                WriteBool("Chain", "bShoutWaitsForSwing", g_ui.shoutWaitsForSwing);
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "On: a shout pressed mid-attack is held until your swing connects, then fires.\n"
                    "Off: vanilla timing -- the shout starts at once and the swing is lost before "
                    "it lands.");
            }

            ImGuiMCP::SliderInt("Power combo wait (ms)", &g_ui.powerAdvanceWaitMs, 0, 1000);
            commit = CommittedNow();
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "How long the cut waits for MCO's own power-combo advance before going ahead "
                    "anyway.\n\n"
                    "0 = do not wait, which is the behaviour before this existed. It only ever arms "
                    "on the first power attack of a chain; a comboed one has already advanced by "
                    "the time the hit lands and pays nothing.\n\n"
                    "The interval it is waiting for measured 102 ms on one clip and 267 ms on "
                    "another, so a pack with slow power attacks wants the higher end.");
            }
            if (commit) WriteInt("Chain", "iPowerAdvanceWaitMs", g_ui.powerAdvanceWaitMs);

            if (ImGuiMCP::Combo("Power attack source", &g_ui.powerSource, kPowerSourceItems, 4)) {
                Write("Chain", "sPowerSource", kPowerSourceItems[g_ui.powerSource]);
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Where a power attack press comes from.\n\n"
                    "auto = look: One Click Power Attack's key if it is installed, otherwise a held "
                    "attack button.\n"
                    "off = no power chaining. Shouts still chain into normal attacks.\n\n"
                    "Set off if you run a power attack mod this engine does not recognise -- Elden "
                    "Power Attack is the known case.");
            }

            ImGuiMCP::SeparatorText("Shout");

            ImGuiMCP::SliderFloat("Hold for word two (s)", &g_ui.wordTwoHoldSec, 0.0f, 1.0f, "%.2f");
            commit = CommittedNow();
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "How long the shout key must be held for the second word.\n\n"
                    "0.40 = default: a tap is reliably ONE word.\n"
                    "0.20 = vanilla, where a deliberate tap often charges to two.\n"
                    "0 = leave the game's own value alone.\n\n"
                    "The same number in both directions: raising it also means every two-word shout "
                    "needs a longer hold.");
            }
            if (commit) WriteFloat("Shout", "fWordTwoHoldSec", g_ui.wordTwoHoldSec);

            ImGuiMCP::SliderFloat("Hold for word three (s)", &g_ui.wordThreeHoldSec, 0.0f, 2.0f, "%.2f");
            commit = CommittedNow();
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "0 = default: leave the game's own 0.9 s alone.\n\n"
                    "Only worth moving if you pushed word two a long way up and want the gap to "
                    "word three widened to match. It must be LARGER than the word-two hold; a value "
                    "at or below it is refused and a line in the log says so.");
            }
            if (commit) WriteFloat("Shout", "fWordThreeHoldSec", g_ui.wordThreeHoldSec);

            ImGuiMCP::SeparatorText("Engine");

            if (ImGuiMCP::Checkbox("Enabled", &g_ui.enabled)) {
                WriteBool("Engine", "bEnabled", g_ui.enabled);
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Off restores stock shouting completely: nothing is intercepted, no chain is "
                    "started, and the hold thresholds above are put back to whatever your game had.\n\n"
                    "Turn this off first when tracking down a mod conflict. If the problem is still "
                    "there, it is not this mod.");
            }

            if (ImGuiMCP::Checkbox("Diagnostic log (takes effect on restart)", &g_ui.trace)) {
                WriteBool("Engine", "bTrace", g_ui.trace);
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "RESTART ONLY. This one is latched when the plugin loads, so the box moves and "
                    "the file changes but this session keeps logging exactly as it was.\n\n"
                    "On, it records every animation event on your character -- one about every 17 ms "
                    "with a weapon drawn. Turn it on to capture a bug report, then straight back off.");
            }

            const auto settings = Settings::Snapshot();

            ImGuiMCP::SeparatorText("What the engine is running on");
            ImGuiMCP::Text("Power source resolved to: %s", PowerSourceWord(settings->resolvedPowerSource));
            ImGuiMCP::Text("Power attack key: %d", settings->powerAttackKeycode);
            ImGuiMCP::Text("Diagnostic log this session: %s", Settings::TraceLive() ? "on" : "off");
            ImGuiMCP::TextWrapped(
                "Safety bounds, shown because a bug report wants them and not because they are "
                "knobs. Each was measured in game, and each fails quietly when wrong -- the wait cap "
                "sits above the slowest attack in your load order, and lowering it fires a queued "
                "shout mid-swing.");
            ImGuiMCP::BulletText("Queued shout released regardless after: %d ms", settings->shoutWaitCapMs);
            ImGuiMCP::BulletText("Shout abandoned as hung after: %d ms", settings->shoutLivenessCapMs);
            ImGuiMCP::BulletText("Swallowed attack press released after: %d ms",
                                 settings->pressOwnershipCapMs);
            ImGuiMCP::BulletText("A ready pass still counts as ours for: %d ms", settings->readyWindowMs);

            ImGuiMCP::SeparatorText("File");
            if (ImGuiMCP::Button("Reload from file")) {
                SyncFromFile();
                g_ui.status = "reloaded from ShoutMCO.ini";
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Restore defaults")) {
                // The compiled defaults, read off a default-constructed `Settings` rather than
                // spelled out again here. A second copy of these numbers is exactly the drift the
                // settings/INI parity test exists to catch.
                const Settings defaults{};
                WriteBool("Engine", "bEnabled", defaults.enabled);
                WriteBool("Engine", "bTrace", defaults.trace);
                WriteBool("Chain", "bShoutWaitsForSwing", defaults.shoutWaitsForSwing);
                WriteInt("Chain", "iChainWindowPct", defaults.chainWindowPct);
                WriteInt("Chain", "iPowerAdvanceWaitMs", defaults.powerAdvanceWaitMs);
                Write("Chain", "sPowerSource", PowerSourceWord(defaults.powerSource));
                WriteFloat("Shout", "fWordTwoHoldSec", defaults.wordTwoHoldSec);
                WriteFloat("Shout", "fWordThreeHoldSec", defaults.wordThreeHoldSec);
                g_ui.status = "defaults written to ShoutMCO.ini";
            }

            if (!g_ui.status.empty()) ImGuiMCP::Text("%s", g_ui.status.c_str());
        }
    }

    void Install() {
        if (!SKSEMenuFramework::IsInstalled()) {
            // An ordinary state, not a failure. The framework is a convenience dependency: without
            // it the INI is still the whole of the configuration surface, which is what every
            // release before this one shipped.
            log::info("[ShoutMCO] SKSE Menu Framework not installed -- tuning menu not registered");
            return;
        }

        SKSEMenuFramework::SetSection("Shouts for MCO");
        SKSEMenuFramework::AddSectionItem("Tuning", RenderTuning);
        log::info("[ShoutMCO] tuning menu registered with SKSE Menu Framework");
    }
}
