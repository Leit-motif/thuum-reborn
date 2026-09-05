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
            bool  trace = false;
            float wordTwoHoldSec = 0.4f;
            int   powerSource = 0;

            // The last write's outcome, shown under the buttons. A failed write is otherwise
            // completely silent: the slider stays where the player put it and the game keeps
            // playing the old value.
            std::string status;
        };

        UiState g_ui;

        const char* const kPowerSourceItems[] = {"hold", "off"};

        int PowerSourceIndex(Settings::PowerSource a_source) {
            return a_source == Settings::PowerSource::kOff ? 1 : 0;
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
            g_ui.trace = settings->trace;
            g_ui.wordTwoHoldSec = settings->wordTwoHoldSec;
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
                "Changes save to SKSE/Plugins/ShoutMCO.ini and apply on your next shout. "
                "No restart, no new save.\n\n"
                "Everything else the engine does is fixed.");

            ImGuiMCP::SliderFloat("Hold for word two (s)", &g_ui.wordTwoHoldSec, 0.0f, 1.0f, "%.2f");
            const bool commit = CommittedNow();
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "How long the shout key must be held for the second word.\n\n"
                    "0.40 = default. A tap is reliably one word.\n"
                    "0.20 = the game's own value, where a deliberate tap often charges to two.\n"
                    "0 = leave the game's value alone.\n\n"
                    "Raising this also means every two-word shout needs a longer hold.");
            }
            if (commit) WriteFloat("Shout", "fWordTwoHoldSec", g_ui.wordTwoHoldSec);

            if (ImGuiMCP::Combo("Held attack is a power attack", &g_ui.powerSource, kPowerSourceItems, 2)) {
                Write("Chain", "sPowerSource", kPowerSourceItems[g_ui.powerSource]);
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Whether a HELD attack button counts as a power press. A power attack mod\'s "
                    "own key chains either way.\n\n"
                    "hold = yes, as in vanilla and MCO alone.\n"
                    "off = a held button is a light attack. Shouts still chain into it.\n\n"
                    "Set this to off if you use One Click Power Attack or Elden Power Attack, "
                    "where HOLDING attack is not a power attack.");
            }

            if (ImGuiMCP::Checkbox("Diagnostic log (takes effect on restart)", &g_ui.trace)) {
                WriteBool("Engine", "bTrace", g_ui.trace);
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Takes effect after a restart. Ticking it now writes the file, but this "
                    "session keeps logging as it was.\n\n"
                    "On, it records every animation event on your character, roughly one every "
                    "17 ms with a weapon drawn. Turn it on to capture a bug report, then turn it "
                    "back off.");
            }

            const auto settings = Settings::Snapshot();

            ImGuiMCP::SeparatorText("Status");
            ImGuiMCP::Text("Held attack is a power attack: %s", PowerSourceWord(settings->powerSource));
            ImGuiMCP::Text("Diagnostic log this session: %s", Settings::TraceLive() ? "on" : "off");
            ImGuiMCP::TextWrapped("Fixed limits. Not settings -- listed so you can quote them in a "
                                  "bug report.");
            ImGuiMCP::BulletText("Held shout fires anyway after: %d ms", settings->shoutWaitCapMs);
            ImGuiMCP::BulletText("Shout treated as stuck after: %d ms", settings->shoutLivenessCapMs);
            ImGuiMCP::BulletText("Held attack press released after: %d ms",
                                 settings->pressOwnershipCapMs);
            ImGuiMCP::BulletText("Ready-state grace period: %d ms", settings->readyWindowMs);

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
                // ONLY THE KEYS THE FILE DOCUMENTS. The pruned settings still parse, so a
                // hand-added line goes on working -- but writing them here would put them back into
                // an INI that deliberately no longer offers them, and one press of this button
                // would undo the pruning in front of the player.
                const Settings defaults{};
                WriteBool("Engine", "bTrace", defaults.trace);
                Write("Chain", "sPowerSource", PowerSourceWord(defaults.powerSource));
                WriteFloat("Shout", "fWordTwoHoldSec", defaults.wordTwoHoldSec);
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

        // THE PUBLIC NAME, and the player reads this one directly. See README.md's Naming table,
        // which is canonical: `ShoutMCO` stays the internal identifier forever, and the
        // public name settled as `Thu'um Reborn`. This said "Shouts for MCO", which is two
        // names superseded.
        SKSEMenuFramework::SetSection("Thu'um Reborn");
        SKSEMenuFramework::AddSectionItem("Tuning", RenderTuning);
        log::info("[ShoutMCO] tuning menu registered with SKSE Menu Framework");
    }
}
