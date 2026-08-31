#pragma once
#include "PCH.h"

#include <memory>
#include <string>

namespace ShoutMCO {
    // Runtime configuration.
    //
    // Re-read at the start of every shout. That is the established practice on this project --
    // four probe variants were retuned inside one 90-second session by editing the deployed MO2
    // copy between shouts (CONTEXT.md, harness notes) -- and it is what keeps the open acceptance
    // gates cheap to test.
    //
    // TWO CLASSES OF FIELD LIVE HERE, and the split is deliberate.
    //
    // Read from `Data/SKSE/Plugins/ShoutMCO.ini` -- EIGHT, and only these eight:
    //     bEnabled, bTrace, bShoutWaitsForSwing, iChainWindowPct, sPowerSource, iPowerAttackKeycode,
    //     fWordTwoHoldSec, fWordThreeHoldSec.
    //
    // The last two are the GMST hold-threshold overrides, and they are the first settings here
    // that configure the GAME rather than this engine -- see their fields and
    // `ShoutHoldThresholds.h`. They ship because a player feels a tap that charges to two words
    // without ever opening a log.
    //
    // TWO MORE KEYS PARSE AND ARE NOT SHIPPED: `bChainDriverCasts` and `iPowerAdvanceWaitMs`.
    // Neither is a counter-example to the rule above, both are the rule applied -- `Load()`
    // accepts them so a live session can move them between shouts, and `config/ShoutMCO.ini`
    // mentions neither, so no player is handed a knob for a feature still under proof. The list
    // above is what SHIPS; see each field for when that changes.
    // `tests/settings_ini_parity_tests.cpp` carries the same two names and fails on a third that
    // nobody decided to add.
    //
    // A setting ships if a player can FEEL it without a log -- and the chain window is the most
    // feelable in the file: it is how much of your shout you can cancel out of.
    //
    // The window setting was `iChainWindowMs` in 1.0.2 and is `iChainWindowPct` now. The count
    // is unchanged -- one key replaced another, it was not added alongside. `Load()` still
    // CONSUMES the retired key silently; see its parse case.
    //
    // `sShoutGate` is the same: `Load()` still CONSUMES the key silently -- see its parse case
    // for how long that lasts and why. A gate that lost on evidence does not ship as a knob a
    // player might turn. `bRootDuringChain` and `bStopMovementBeforeChain` got the same treatment
    // for the same reason: leaving a setting in a shipped INI whose own comment tells the player
    // which value to use is shipping the experiment.
    //
    // INTERNAL CONSTANTS -- everything else. The field and its default stay because the engine
    // reads them, but `Load()` has no parse case, so the INI cannot move them. They were measured
    // in game and a wrong value fails SILENTLY: a bad `attackEvent` produces no attack at all,
    // `sWindowSource = graph` needs a Nemesis patch that does not exist, `resumeMode` is a
    // decided A/B, and `shoutWaitCapMs` reintroduces the bug it guards if lowered. None of that
    // is a player's decision.
    //
    // Adding a setting back to the INI means adding its parse case in `Load()` AND its comment in
    // `config/ShoutMCO.ini`. Do it only when the behaviour is proven AND a player can feel
    // it, not by default.
    struct Settings {
        // Where the chain window comes from.
        //
        // `kGraph` is the shipping design: a `SHOUT_WinOpen` clip trigger at -0.4s
        // end-relative, so the window has the same *length* on a 1.03s and a 3.93s exhale
        // (ADR-0003). It needs the C3 Nemesis patch, which does not exist yet.
        //
        // `kSpellFire` is the interim source and the current default: the window opens at
        // `Voice_SpellFire_Event`, i.e. as soon as cutting the shout still delivers its magic.
        // It carries no duration heuristic of any kind -- inferring the window from exhale
        // length is forbidden outright, because some packs ship one clip for every word count.
        enum class WindowSource { kSpellFire, kGraph };

        // Which index the chain restores after the cut's ready pass resets the combo counter.
        //
        // `kRestoreNext` reads `MCO_nextattack` at shout start. MCO advances that variable to
        // the correct next index when each attack begins, so restoring it resumes
        // the combo including any wrap MCO applies -- and if the combo had already lapsed, the
        // value is 1 and restarting is the right answer anyway.
        //
        // `kIncrementCurrent` is the spec's literal wording: cached `MCO_currentattack` + 1.
        // Kept so the two can be compared in game without a rebuild.
        enum class ResumeMode { kRestoreNext, kIncrementCurrent, kOff };

        // There is one cancel gate and it is unconditional; the behaviour and the reasoning
        // live on `TryCancelRecoveryLocked` in `ShoutChainEngine.cpp`, which is where a reader
        // looking at the cancel will actually be.
        //
        // Waiting for MCO's combo advance is a real alternative that lost on measurement:
        // CONTEXT.md records that `MCO_nextpowerattack` advances ~+724 ms into an UNCOMBOED
        // power attack but ~+51 ms into a COMBOED one, against a hit frame at +559 ms. That is
        // the whole of what waiting for the advance bought, and what it cost was 667-1361 ms of
        // wait plus ~31% of presses on which its conditions never came true at all. Do not
        // rediscover the ready gate as a fix.

        bool enabled = true;

        // ADR-0006 -- CHAIN OUT OF A DRIVER'S CAST (Spell Hotbar 2).
        //
        // OFF, and it stays off until it is confirmed the spell still fires. With it off the
        // engine never arms on a shout-state entry that no `BeginCastVoice` vouched for, so
        // `Observe`'s new branch is one boolean test and the behaviour is byte-for-byte what
        // shipped -- which is what has to stay reachable.
        //
        // READ FROM THE INI BUT DELIBERATELY NOT IN `config/ShoutMCO.ini`, and the split is the
        // point. `Load()` has a parse case so a live session can flip it between shouts, which is
        // this project's established harness practice. It is absent from the SHIPPED file because
        // a setting ships when a player can feel it without a log, and an unconfirmed feature is
        // an experiment -- shipping it as a knob is exactly the mistake `sShoutGate` was. The INI
        // comment lands if and when the proof passes; until then a player who never edits the
        // file cannot reach this.
        //
        // FLIPPING IT MID-SESSION TAKES EFFECT AT THE NEXT `BeginCastVoice`, NOT THE NEXT CAST.
        // The per-shout reload is gated on `isBegin`, and a hotbar cast raises no `BeginCastVoice`
        // at all -- so a drive that edits the INI must take one ORDINARY shout to load the new
        // value before the cast it wants to measure. The reload is not widened to the state entry
        // on purpose: `Load()` reads a file from disk, `SBF_ShoutStart` fires on the jump bounce
        // too, and paying disk I/O twice a shout to save one keypress in a harness is the wrong
        // trade.
        bool chainDriverCasts = false;

        bool reloadPerShout = true;
        // OFF for a shipped mod. `Observe` runs on every animation event on the player and
        // `SCAR_UpdateDummy` alone fires every ~17ms with a weapon drawn, so this is a
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
        // order, not a new one.
        std::string cutEvent = "shoutStop";
        std::string attackEvent = "attackStart";
        // Only `attackStart` was ever exercised in game. A wrong name here produces no attack
        // rather than a wrong one -- which is precisely why this stopped being a
        // setting: a silent no-op is the worst possible thing to hand a player a text box for.
        // Where a power press comes from. Load orders differ on this and neither answer can be
        // assumed, so `kAuto` decides by looking: if One Click Power Attack's config is present
        // its key is used, otherwise the press is a held attack button, which is what a load
        // order without OCPA gives the player.
        //
        // Both paths are verified in game: the key, and the hold.
        enum class PowerSource { kAuto, kOcpa, kHold, kOff };

        PowerSource powerSource = PowerSource::kAuto;
        // What kAuto resolved to. Never read from the INI.
        PowerSource resolvedPowerSource = PowerSource::kOff;

        // The power attack event. Always the in-place one: MCO consumes it, and the directional
        // variants are a dead end here -- `attackPowerStartForward` is accepted by the graph but
        // consumed by nothing, so it cancels the shout and produces no attack.
        std::string powerAttackEvent = "attackPowerStartInPlace";

        // `rootDuringChain` and `rootWatchdogMs` are deliberately gone. They took movement input
        // away for the length of a chained power attack, and that does NOT work: it suppressed
        // and restored cleanly 13/13 but rooted nothing the player could see, and it cost the
        // moveset its direction -- a forward press produced the BACK power attack. Shipping a
        // knob whose own comment said "leave at 0" is what this removed.
        //
        // Same treatment as `bStopMovementBeforeChain`. The seam itself is gone too --
        // `kRootDuringChain` and both toggle functions were deleted from `ShoutChainEngine.cpp`
        // once rooting was ruled behaviour-only. Do not re-add it as a setting, and do not rebuild
        // the seam: the root lives in the `shmco` behaviour patch.

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
        // 250ms before the shout's own `Voice_SpellFire_Event`. Pressed mid-swing
        // that reads as the power attack cancelling and the character dropping out of MCO.
        //
        // With this on, a shout pressed while an attack is live is QUEUED and handed back when the
        // attack ENDS -- `IsAttacking` falling, which is later than the graph's own `inRdy`: a cut
        // attack raises `inRdy` while it is still swinging, and releasing there restores the exact
        // failure this gate exists to fix. The engine never looks at shout cooldown (ADR-0002) and
        // never decides whether the player *may* shout, only whether the attack is finished with
        // the character.
        //
        // `MCO_WinOpen` was tried as the gate and is genuinely wrong: it fires ~180 ms BEFORE the
        // hit on a power attack, so gating on it cut the swing 3 times out of 3. That measurement
        // has never been in doubt, and it is why the surviving gate keys off the hit and never off
        // the window.
        //
        // This flag chooses WHETHER the press is held, not how long for. False means it is never
        // held at all; true means it is held until the swing connects, and there is exactly one way
        // to wait now that the alternative is gone. Nothing selects between gates any
        // more, so this is the whole of the switch.
        bool shoutWaitsForSwing = true;

        // HOW MUCH OF THE END OF A SHOUT IS CANCELLABLE, as a PERCENTAGE of it.
        //
        // The chain window is the tail of the exhale, measured BACKWARDS from where that exhale
        // ends. Smaller means the shout plays longer before the player can leave it, which is more
        // committed; larger means they can break out earlier. 0 means no window at all -- the
        // buffered press fires when the shout ends on its own.
        //
        // The end is not assumed, it is MEASURED: the engine already sees
        // `Voice_SpellFire_Event` -> `shoutStop` on every shout, caches the interval per shout,
        // stance, draw and word count, and opens the window at `measured_end - pct% of measured_end`
        // on the next shout with the same key.
        //
        // WHY A PERCENTAGE, WHEN THIS WAS A MILLISECOND COUNT. The criterion is what makes this
        // measurable rather than a taste: commitment means the player is held through the gesture
        // -- inhale, exhale, effect -- and released for the RECOVERY, the part afterwards that
        // reads as hanging. MCO's own light attack hands the player exactly that, and it is a
        // fraction: the window is the last 23.4% and 24.9% of the animation on two measured
        // attacks.
        //
        // A fixed count cannot track a fraction across clips that differ by 4x. At the shipped 300
        // ms, against Goetia's own three exhales, the player was released for 29.0% of a one-word
        // shout, 15.3% of a two-word and 7.6% of a three-word -- so the longer and heavier the
        // animation, the LESS of its recovery they got back, which is backwards. Held through 92.4%
        // of a 3.93 s clip is the defect this replaces.
        //
        // 45 IS THE SHIPPED VALUE, AND THIS DEFAULT'S JOB IS TO REPRODUCE A SHIPPED INSTALL. It was
        // tuned live across 30/60/45/40 once the same value also governed the shout-to-shout
        // exhale cut, with 60 rejected as too liberal. Do not re-tune it as an optimisation, and
        // do not let this number drift from `config/ShoutMCO.ini` again: an install whose INI is
        // missing, deleted or unreadable runs on THIS line while every word of documentation
        // describes that file, and the two disagreed for a release without anything reporting it.
        // `tests/settings_ini_parity_tests.cpp` now fails when they do.
        //
        // HOW THE FRACTION WAS FIRST ANCHORED, kept because it is what makes the number
        // re-derivable rather than a taste. Spellfire lands ~0.1 s into the exhale (ADR-0003) so the
        // measured tail is ~91% of the clip, and 30% of that tail is ~27% of the animation -- within
        // a couple of points of MCO's measured 23.4-24.9%. That derivation also moved a ~940 ms
        // tail -- SYHO's, measured at 940.43 ms -- from 300 ms to 282 ms, about one frame, so the
        // ruling survived the change to a fraction rather than being quietly reversed by it. The
        // pack is named because the same figure was for a while attached to GOETIA's one-word tail
        // by mistake, and Goetia's is 615 ms, not 940 -- see the correction in
        // `ShoutChainEngine.cpp`'s `WindowKey` comment before reusing either. The ear then landed
        // above the derivation.
        //
        // ADR-0004 records the supersession of ADR-0003's "end-relative, not fractional" decision,
        // including why both of its premises are spent. The floor and ceiling that bound this are
        // internal constants in `ShoutChainEngine.cpp` and are set not to bind on real shouts.
        //
        // Direction of travel, because "extend the window" is ambiguous in this project's own
        // vocabulary: the window IS the cancellable tail, so a smaller number is MORE commitment.
        int chainWindowPct = 45;

        // Backstop, in ms: a queued shout press is let through regardless once this elapses.
        //
        // This is NOT the delay a player feels. That is however long the attack has left to run,
        // and it is an event rather than a duration -- the queue releases when `IsAttacking`
        // falls, so it scales with the animation on its own. `inRdy` after cancel
        // is not shout-admissible on 1H or 2H; a later event (often a second `attackStop`) is.
        //
        // It only fires when an attack never reaches ready at all. It must therefore sit
        // ABOVE the slowest complete ATTACK in the load order, and that is a different and much
        // larger number than it was when the gate was the hit frame: the measured
        // rapier light attack hits at +481ms but does not finish until ~+1400ms, so the old 1500
        // left barely 100ms of headroom and any two-hander would have blown through it -- firing
        // the queue mid-swing and restoring the exact failure the gate was moved to fix.
        //
        // 4000 is a deliberate over-estimate on one measured weapon, not a tuned value. Narrowing
        // it wants a greatsword and a slow power attack measured first, and lowering it below the
        // slowest attack is how this bug comes back.
        int shoutWaitCapMs = 4000;

        // HOW LONG THE CUT WAITS FOR MCO'S OWN POWER-COMBO ADVANCE. 0 disables the wait
        // entirely and restores the immediate-cut-at-hit behaviour exactly.
        //
        // WHY THIS IS NOT THE DELETED `kReady` GATE, stated here because the comment above
        // `TryCancelRecoveryLocked` lists "waiting for MCO's combo advance" as a DISPROVED
        // alternative and a reader arriving at this field deserves the difference up front:
        //
        //   - `kReady` required THREE conditions -- hit landed, `MCO_WinOpen` open, and the combo
        //     advanced. 31% of presses never saw one of the three come true, and the median cancel
        //     sat on `MCO_WinOpen` at +1174 ms. The window is what made it expensive and what made
        //     it intermittent; the window annotation is missing from 18.2% of power clips outright.
        //   - This waits on ONE condition, the advance, and never on the window. It is bounded, so
        //     it cannot ride an attack to its natural end the way `kReady` did on that 31%.
        //   - It arms ONLY when the advance has not already landed by the hit. A COMBOED power
        //     attack advances at ~+51 ms, well before its +559 ms hit, so the common case pays
        //     nothing at all -- the wait is armed for the first power attack of a chain and for
        //     nothing else.
        //
        // THE NUMBER. Two independent measurements agree: the uncomboed advance sits at ~+724 ms
        // against a +559 ms hit (+165 ms), and the clip annotation puts it 266.7 ms after the hit
        // in CLIP time, which is 166.7 ms at that clip's own `MCO_AttackSpeed = 1.6`. 350 covers
        // the clip-time figure at 1.0x speed with headroom, because a pack whose power clip does
        // not set a speed multiplier pays the full 266.7 ms.
        //
        // It is a BOUND, not a trigger time: the cut normally rides MCO's own event and this only
        // decides how long to keep waiting for one. Do not read 350 as a measurement of anything.
        //
        // PARSED BUT NOT SHIPPED, on the `bChainDriverCasts` precedent rather than by oversight:
        // a setting ships when a player can feel it, and this one is feelable, but it is an
        // experiment until proven in game. `config/ShoutMCO.ini` gains its comment when that
        // happens, not before. Defaulted ON where `bChainDriverCasts` defaults off, and the
        // difference is real -- that one opens a new surface for an external driver, this one IS
        // the behaviour the wait exists to get.
        int powerAdvanceWaitMs = 350;

        // Backstop, in ms, on an attack press this engine has swallowed during a shout — the mirror
        // of `shoutWaitCapMs` for the other direction. A missed release left the attack button
        // dead for a whole session; this bounds that failure.
        //
        // Ownership normally ends when the player lets go. This only fires when that release never
        // reached the hook at all, so it sits far above any real press: a held power attack is well
        // under a second, and nothing legitimate holds the button for four. It is not a tuning
        // knob — lowering it would start cutting genuine holds, and raising it lengthens the only
        // failure it exists to bound.
        int pressOwnershipCapMs = 4000;

        // How long a shout may stay "live" with neither `shoutStop` nor `SBF_ShoutStop` before the
        // engine abandons it and stops swallowing attack input, in ms.
        //
        // A ready pass no longer ends a shout at all, so naming one here would describe a
        // condition that gates nothing. Deleting the `inRdy` inference left this as the ONLY
        // backstop behind `SBF_ShoutStop`, where before it sat behind two signals.
        //
        // Nothing legitimate comes near it: the longest observed shout -- a three-word charge held
        // under the player's own thumb -- runs about a second of exhale after an inhale the player
        // controls. This only fires when a shout hangs.
        int shoutLivenessCapMs = 8000;

        // HOW LONG THE SHOUT BUTTON MUST BE HELD FOR THE SECOND AND THIRD WORD, in seconds.
        //
        // These do not configure this engine at all -- they OVERRIDE the game's own `fShoutTime1`
        // and `fShoutTime2` GMSTs at runtime, which is where word count is actually decided (see
        // `ShoutHoldThresholds.h` for the A/B that proved it, and `Settings.cpp`'s
        // `ApplyHoldOverrides` for the write). A runtime write is the whole reason they are here:
        // the packaging constraint is no Papyrus and no ESP, and moving a GMST any other way needs
        // one of the two.
        //
        // 0.4 AGAINST A VANILLA 0.2, and the number is a doubling rather than a measurement. A
        // live A/B put the real word-2 boundary at ~200 ms at stock settings, which is inside
        // an ordinary deliberate press -- an MMO-mouse thumb button especially -- so a tap the
        // player means as one word charges to two. 0.4 puts the boundary clear of a tap while
        // staying well under the ~0.55 s that 0.5 measured as its first two-word hold, i.e. it
        // costs a two-word shout about a fifth of a second of extra hold. Retune it in the INI
        // between shouts; it re-reads with everything else.
        //
        // SHIPPED -- a player feels this one without a log, and it is the
        // most feelable thing in the file after the chain window: it is whether their tap does what
        // they asked.
        float wordTwoHoldSec = 0.4f;

        // 0 = LEAVE `fShoutTime2` ALONE, which is the shipped default and is deliberate rather than
        // an omission. Vanilla's 0.9 s already sits far outside any tap, so the misread this
        // feature exists to fix does not reach word three -- and moving a threshold nobody is
        // tripping would only make three-word shouts harder to reach. It ships as a knob because a
        // player who raises `fWordTwoHoldSec` a long way may want the gap between the two words
        // widened to match.
        //
        // A value at or below the effective `fWordTwoHoldSec` is REFUSED, not clamped: it would
        // ask for word three no later than word two. `PlanHoldOverrides` decides that and warns.
        float wordThreeHoldSec = 0.0f;

        ResumeMode resumeMode = ResumeMode::kRestoreNext;
        // How long after the cut an `inRdy` still counts as ours. Guards against a stale arm
        // firing an attack into an unrelated ready pass. The measured gap is ~9ms, so this is
        // an order of magnitude of slack, not a licence to fire late.
        //
        // A SECOND READER AND NOT A SECOND MEANING. `WatchdogDropChainLocked`
        // drops an armed chain once it is older than this, for the case where no `inRdy` arrives
        // AT ALL -- a sheathing character never raises one. The question is the same question
        // ("is the pass our cut provoked still coming?"), so it takes the same number rather than
        // a second one whose relation to this one nobody could state.
        int readyWindowMs = 100;

        // AN IMMUTABLE SNAPSHOT, NOT A MUTABLE GLOBAL.
        //
        // `Load()` runs at the start of every shout on the animation-graph event path, while the
        // input hooks read settings from their own threads. The old `static Settings& Get()`
        // handed both sides one mutable object, so a reload could reassign the `std::string`
        // fields under a concurrent reader -- a heap race, same class as the one the atomic-hash
        // log guards were built against. `Load()` now parses into a fresh object and publishes it
        // atomically; readers take a snapshot once per operation and see one consistent config
        // for the whole of it. A reader holding last shout's snapshot for the few microseconds of
        // an event is correct behaviour, not staleness: settings changes take effect at the next
        // shout, which is exactly what the per-shout reload always promised.
        [[nodiscard]] static std::shared_ptr<const Settings> Snapshot();
        static void                                          Load();

        // PUSH `fWordTwoHoldSec` / `fWordThreeHoldSec` INTO THE GAME'S OWN GMSTS. Idempotent: it
        // re-reads `fShoutTime1` / `fShoutTime2` and writes only when they disagree, so it is two
        // hash lookups and two float compares on the pass where nothing has changed.
        //
        // WHEN, which is the load-bearing part. `Load()` calls this at its end, so every one of
        // its three call sites is covered -- and the important one is `BeginCastVoice`, which fires
        // on the shout key's DOWN edge, before any threshold comparison, so even the first shout of
        // a session is overridden in time. The shout-input hook calls it too, one edge earlier
        // still. Neither is a lifecycle listener on purpose: `Skyrim.esm` defines both as GMST
        // records so a write before data load is clobbered, and CONTEXT.md records that
        // an SKSE `kDataLoaded` listener did not run in this setup at all. A clobber is handled by
        // detecting it rather than by sequencing against it -- see `DecideHoldOverride`.
        //
        // Re-applying per shout is a feature, not a cost: it makes the two thresholds live-tunable
        // between shouts, which is this project's established harness practice.
        static void ApplyHoldOverrides();

        // Lock-free mirror of `Snapshot()->trace`, for the SHOUTMCO_TRACE macro -- which runs on
        // every animation event on every thread and must not pay a shared_ptr refcount for a
        // usually-false answer.
        [[nodiscard]] static bool TraceLive();

        void Log() const;
    };
}
