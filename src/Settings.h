#pragma once
#include "PCH.h"

#include <string>

namespace ShoutMCO {
    // Runtime configuration.
    //
    // Re-read at the start of every shout. That is the established practice on this project --
    // four probe variants were retuned inside one 90-second session by editing the deployed MO2
    // copy between shouts (CONTEXT.md, harness notes) -- and it is what keeps the open acceptance
    // gates cheap to test.
    //
    // TWO CLASSES OF FIELD LIVE HERE, and the split is deliberate (ticket 17, 2026-08-03).
    //
    // Read from `Data/SKSE/Plugins/ShoutMCO.ini` -- FIVE, and only these five:
    //     bEnabled, bTrace, bShoutWaitsForSwing, sPowerSource, iPowerAttackKeycode.
    //
    // INTERNAL CONSTANTS -- everything else. The field and its default stay because the engine
    // reads them, but `Load()` has no parse case, so the INI cannot move them. They were measured
    // in game and a wrong value fails SILENTLY: a bad `attackEvent` produces no attack at all,
    // `sWindowSource = graph` needs a Nemesis patch that does not exist (ticket 06), `resumeMode`
    // is a decided A/B, and `shoutWaitCapMs` reintroduces the bug it guards if lowered. None of
    // that is a player's decision.
    //
    // Adding a setting back to the INI means adding its parse case in `Load()` AND its comment in
    // `config/ShoutMCO.ini`. Do it when a ticket needs it, not by default.
    struct Settings {
        // Where the chain window comes from.
        //
        // `kGraph` is the shipping design: a `SHOUT_WinOpen` clip trigger at -0.4s
        // end-relative, so the window has the same *length* on a 1.03s and a 3.93s exhale
        // (ADR-0003). It needs the C3 Nemesis patch, which does not exist yet.
        //
        // `kSpellFire` is the interim source and the current default: the window opens at
        // `Voice_SpellFire_Event`, i.e. as soon as cutting the shout still delivers its magic
        // (finding 1). It carries no duration heuristic of any kind -- inferring the window
        // from exhale length is forbidden outright, because some packs ship one clip for every
        // word count (finding 5).
        enum class WindowSource { kSpellFire, kGraph };

        // Which index the chain restores after the cut's ready pass resets the combo counter.
        //
        // `kRestoreNext` reads `MCO_nextattack` at shout start. MCO advances that variable to
        // the correct next index when each attack begins (finding 6), so restoring it resumes
        // the combo including any wrap MCO applies -- and if the combo had already lapsed, the
        // value is 1 and restarting is the right answer anyway.
        //
        // `kIncrementCurrent` is the spec's literal wording: cached `MCO_currentattack` + 1.
        // Kept so the two can be compared in game without a rebuild.
        enum class ResumeMode { kRestoreNext, kIncrementCurrent, kOff };

        bool enabled = true;
        bool reloadPerShout = true;
        // OFF for a shipped mod. `Observe` runs on every animation event on the player and
        // `SCAR_UpdateDummy` alone fires every ~17ms with a weapon drawn (finding 8), so this is a
        // multi-megabyte log and constant disk I/O through combat. See `Trace.h` for what survives
        // when it is off -- the startup lines and the warnings do, everything per-event does not.
        //
        // Defaults matter here beyond taste: `Load()` falls back to these when no INI is found at
        // all, so a user who deletes the file gets the shipping behaviour rather than the harness's.
        bool trace = false;

        WindowSource windowSource = WindowSource::kSpellFire;
        std::string  windowEvent = "SHOUT_WinOpen";
        // How long a press stays in the buffer, in ms. 0 buffers for the whole shout.
        int bufferMs = 0;

        // The cut. `shoutStop` is already consumed inside the exhale state, and `attackStart`
        // is already consumed in ready -- the bridge is those two existing transitions fired in
        // order, not a new one (finding 7c).
        std::string cutEvent = "shoutStop";
        std::string attackEvent = "attackStart";
        // Only `attackStart` was ever exercised in game. A wrong name here produces no attack
        // rather than a wrong one -- which is precisely why ticket 17 stopped this being a
        // setting: a silent no-op is the worst possible thing to hand a player a text box for.
        // Where a power press comes from. Load orders differ on this and neither answer can be
        // assumed, so `kAuto` decides by looking: if One Click Power Attack's config is present
        // its key is used, otherwise the press is a held attack button, which is what a load
        // order without OCPA gives the player.
        //
        // Both paths are verified in game (2026-07-29): the key at 18:41, the hold at 19:00.
        enum class PowerSource { kAuto, kOcpa, kHold, kOff };

        PowerSource powerSource = PowerSource::kAuto;
        // What kAuto resolved to. Never read from the INI.
        PowerSource resolvedPowerSource = PowerSource::kOff;

        // The power attack event. Always the in-place one: MCO consumes it, and the directional
        // variants are a dead end here -- `attackPowerStartForward` is accepted by the graph but
        // consumed by nothing, so it cancels the shout and produces no attack (measured
        // 2026-07-29).
        std::string powerAttackEvent = "attackPowerStartInPlace";

        // `rootDuringChain` and `rootWatchdogMs` WERE HERE, and are deliberately gone -- ticket 17,
        // 2026-08-03. They took movement input away for the length of a chained power attack, and
        // finding 14 records that this does NOT work: it suppressed and restored cleanly 13/13 but
        // rooted nothing the player could see, and it cost the moveset its direction -- a forward
        // press produced the BACK power attack. Shipping a knob whose own comment said "leave at 0"
        // is what this ticket removed.
        //
        // Same treatment as D6's `bStopMovementBeforeChain` (finding 16). The seam survives as
        // `kRootDuringChain` in `ShoutChainEngine.cpp`, because deferred ticket 07 (root the
        // inhale) is its named future consumer. Do not re-add it as a setting.

        // How long after a chained attack to keep sampling animation-driven motion into the trace.
        // Observation only -- it changes nothing, it just bounds the log volume. The travel being
        // investigated happens over the swing, so a profile is needed rather than one sample.
        int motionWatchMs = 3000;

        // How long the attack button must be held to count as a power press, in hold mode.
        // Negative means "use the game's own threshold" -- `AttackBlockHandler` carries
        // `initialPowerAttackDelay`, which already reflects the player's `fInitialPowerAttackDelay`.
        // Inventing a constant here would silently disagree with their game, so this is an
        // override for odd setups, not a value anyone should have to find and tune.
        float powerHoldSeconds = -1.0f;

        // OCPA's key. -1 means "read OCPA's own config", so the key lives in exactly one place
        // and follows the player's own binding; 0 means no key. Mouse buttons are 256 + button
        // index, as SKSE and OCPA both number them.
        int powerAttackKeycode = -1;

        [[nodiscard]] bool HoldToPower() const { return resolvedPowerSource == PowerSource::kHold; }
        [[nodiscard]] bool KeyToPower() const { return resolvedPowerSource == PowerSource::kOcpa; }

        // THE OTHER DIRECTION: a shout pressed partway through an MCO attack.
        //
        // Vanilla lets the shout start immediately, and starting one tears the outgoing attack
        // down about 3ms later -- through `MCO_AttackExitNotify` / `attackStop` / `inRdy`, some
        // 250ms before the shout's own `Voice_SpellFire_Event` (finding 12). Pressed mid-swing
        // that reads as the power attack cancelling and the character dropping out of MCO.
        //
        // With this on, a shout pressed while an attack is live and its swing has NOT LANDED is
        // held until `HitFrame`, then handed to the game. The teardown then cuts recovery rather
        // than the hit. The engine never looks at shout cooldown (ADR-0002) and never decides
        // whether the player *may* shout, only whether the swing is finished.
        //
        // The gate is `HitFrame` and NOT `MCO_WinOpen`, which was tried first and measured wrong
        // on 2026-08-02: the window fires ~180ms BEFORE the hit on this attack, so gating on it
        // still cut the swing 3/3 (finding 17).
        bool shoutWaitsForSwing = true;

        // Backstop, in ms: a held press is let through regardless once this elapses.
        //
        // This is NOT the delay a player feels -- that is however long the animation takes to
        // reach its hit frame, ~509ms from the start of the measured rapier power attack and less
        // for a press made later into it. Nothing here scales that.
        //
        // It only fires when an attack raises no `HitFrame` at all AND has not ended: a moveset
        // whose moving power attack has no swing in it (finding 16) is exactly that case. It must
        // therefore sit ABOVE the slowest hit frame in the load order, or it would pre-empt a slow
        // weapon's swing and reintroduce the bug it exists to guard. Only the rapier has been
        // measured, so 1500 is a deliberate over-estimate rather than a tuned value -- narrowing
        // it wants a two-hander measured first.
        int shoutWaitCapMs = 1500;

        ResumeMode resumeMode = ResumeMode::kRestoreNext;
        // How long after the cut an `inRdy` still counts as ours. Guards against a stale arm
        // firing an attack into an unrelated ready pass. The measured gap is ~9ms, so this is
        // an order of magnitude of slack, not a licence to fire late.
        int readyWindowMs = 100;

        static Settings& Get();
        static void      Load();

        void Log() const;
    };
}
