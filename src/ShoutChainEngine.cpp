#include "PCH.h"
#include "ShoutChainEngine.h"

#include <array>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "AttackInputHook.h"
#include "AttackQueuedRelease.h"
#include "AttackSeam.h"
#include "CastIntentApi.h"
#include "ClipOwnedRelease.h"
#include "EngineLock.h"
#include "Settings.h"
#include "ShoutInputHook.h"
#include "Trace.h"

using namespace SKSE;
using namespace SKSE::log;

namespace ShoutMCO {
    namespace {
        enum class AttackKind { kLight, kPower };

        // WHICH EXHALE'S LENGTH A MEASUREMENT DESCRIBES.
        //
        // Deliberately coarse to start, and each half earns its place:
        //
        //   - STANCE, because the graph has separate `1HM_` and `Sneak1HM_` exhale slots
        //     and they are different clips. The sheathed `MT_` branch is not keyed
        //     because there is no MCO attack to chain into from it -- it is tolerated,
        //     not distinguished.
        //   - SHOUT IDENTITY, because OAR selects animations per shout via `IsEquippedShout`,
        //     which is exactly what Thu'um does across its 18 shouts.
        //
        //   - WEAPON DRAWN, and this one was learned the hard way. An earlier design argued
        //     the sheathed `MT_` branch "need not be keyed, only tolerated" because there is
        //     no MCO attack to chain into from it. That reasoning covers the sheathed shout's OWN window and nothing else,
        //     and MIN-BIAS INVERTS IT: a shorter sheathed measurement permanently overwrites the
        //     drawn entry, and no later drawn measurement can raise it back for the session.
        //     Measured on one key in one session -- sheathed 894 ms, drawn 1740 ms -- after which
        //     every drawn shout scheduled from 894 and opened its window 1143 ms before the end
        //     against a 300 ms setting. One sheathe is enough to switch the feature off for the
        //     rest of the session, silently.
        //
        //   - WORD COUNT. A pack shipping different clip lengths per word count cannot share
        //     one entry covering all of them. Min-bias makes that fail in the worst available
        //     direction, and the numbers are static facts about an installed pack rather than a
        //     hypothetical: Goetia's exhale is `mt_shout_exhale` 1.033 s, `_medium` 1.967 s,
        //     `_long` 3.933 s. One one-word shout caches a ~615 ms tail, and every three-word
        //     shout after it then schedules from 615 against a three-word tail several times
        //     longer -- opening the window seconds before the shout ends and cutting a
        //     four-second animation a small fraction of the way in. That is the same class of
        //     silent session-wide revert the `drawn` field above was added for, one order of
        //     magnitude larger. The DIRECTION and the ORDER OF MAGNITUDE are what this argument
        //     needs, and both survive whatever the long tail measures at.
        //
        //     THE ~615 MS IS A CORRECTION, so nobody re-derives the number it replaces. This
        //     passage once said ~939 ms, which is 1.033 s minus the ~0.1 s to spellfire --
        //     arithmetic off Goetia's CLIP LENGTH, not a reading. ~939 is SYHO's MEASURED tail,
        //     and the two packs ship the same 1.033 s one-word exhale yet measure **940.43 ms
        //     and 615.49 / 615.69 / 615.79 ms**, because spellfire does not land at the same
        //     point inside the two clips. Goetia's tail cannot be derived from Goetia's clip
        //     length.
        //
        //     WHICH IS EXACTLY WHY THE THREE-WORD FIGURE IS NO LONGER QUOTED HERE. It was
        //     "~3.82 s", and that is 3.933 s minus SYHO's ~0.1 s offset -- the identical
        //     derivation this paragraph just rejected. **Goetia's three-word tail has never
        //     been measured.** Applying Goetia's own measured offset (1033 - 615 = ~418 ms)
        //     instead gives ~3.5 s, and whether that offset even holds across clip lengths is
        //     itself unknown. A three-word Goetia shout will report the real number in
        //     `>>> WINDOW tail measured`; until then, treat every three-word figure in this
        //     repo as derived.
        //
        //     SYHO hides it completely, which is why it survived this long: SYHO ships one
        //     1.033 s clip for every word count, so on a fixture that only uses SYHO all three
        //     variations measure the same tail and the key never needed the field. It is Goetia
        //     that exposes it.
        //
        // `kNone` IS ITS OWN BUCKET AND NEVER FOLDS INTO `kOne`. Spell Hotbar 2 casts with no
        // shout equipped, so the variation reads stale or absent. Defaulting an unknown reading
        // to one word would be a silent lie exactly where the engine already knows it is
        // guessing, so an unknown reading gets its own cache entry and measures itself.
        struct WindowKey {
            RE::FormID shout = 0;
            bool       sneaking = false;
            bool       drawn = false;
            // `TESShout::VariationID` widened to its underlying type so `kNone` (-1 as u32) stays a
            // distinct, comparable bucket without the enum's arithmetic caveats.
            std::uint32_t variation = static_cast<std::uint32_t>(RE::TESShout::VariationID::kNone);

            bool operator==(const WindowKey&) const = default;
        };

        // How a variation prints in a trace line. `kNone` is spelled out rather than shown as
        // 4294967295, because the whole point of giving it its own bucket is that a reader can see
        // when the engine did not know the word count.
        [[nodiscard]] std::string_view VariationName(std::uint32_t a_variation) {
            using V = RE::TESShout::VariationID;
            switch (a_variation) {
                case static_cast<std::uint32_t>(V::kOne):   return "1 word"sv;
                case static_cast<std::uint32_t>(V::kTwo):   return "2 words"sv;
                case static_cast<std::uint32_t>(V::kThree): return "3 words"sv;
                default:                                    return "word count unknown"sv;
            }
        }

        // GUARDED BY `detail::g_engineLock` -- see EngineLock.h for the discipline.
        //
        // A single chain trace carries six distinct thread ids on `Observe`, and nothing in
        // `ChainState` is atomic. Every read and write of this struct, the queued snapshot, the
        // MCO tracking globals and the held press now happens inside the engine lock, so a press
        // cannot be half-read or cleared out from under the thread deciding on it.
        struct ChainState {
            bool shoutActive = false;
            bool windowOpen = false;
            // When the shout began. A shout is only ever ended by an event -- `shoutStop`, or
            // `SBF_ShoutStop` -- so a shout that raises
            // neither leaves this state latched and every attack press swallowed behind it. See
            // `WatchdogEndShoutLocked`.
            double shoutStartedAtMs = 0.0;

            // The shout has committed -- `Voice_SpellFire_Event` has been seen, so the magic is
            // out and a cut still delivers it. Before that point an `inRdy` cannot mean the
            // shout ended, because the shout has not yet done anything to end.
            bool spellFired = false;

            // The generator's 0.100 s fire was swallowed for this shout because the
            // playing clip owns a later `Voice_SpellFire_Event`. One intercept; the clip's event
            // delivers. Cleared by `Reset()` with the rest of the shout.
            bool generatorIntercepted = false;

            // The three fields the measured window needs.
            //
            // `windowKey` is read ONCE, in phase A at `BeginCastVoice`, and stored here. It is
            // deliberately not re-read at spellfire: the selected power can change under the
            // engine mid-shout, and Spell Hotbar 2 is known to swap `selectedPower` and sometimes
            // leave it empty. An
            // empty slot is not an error, it falls in the `0` bucket, which is the right bucket
            // for a one-clip pack like SYHO anyway.
            //
            // `spellFiredAtMs` is the start of the interval being measured, and doubles as the
            // "is there a measurement in flight" flag: cleared by `Reset()`, by `EndShoutLocked`
            // (so a shout ended by the liveness cap, a state exit or a load can never be
            // recorded), and by the recording itself (so one `shoutStop` records once).
            //
            // `cutSent` is the poisoning guard, and it is the single most important correctness
            // property here. A shout WE cut has a `shoutStop` we sent ourselves, so its interval
            // is "however long the player waited" and not the clip's length. Record one and the
            // cache eats itself -- cut at 540 ms records 540, the next window opens at 140, the
            // player cuts at 140, and within a few shouts the window is back at spellfire with
            // nothing in the log saying the feature switched itself off.
            WindowKey windowKey{};
            double    spellFiredAtMs = 0.0;
            bool      cutSent = false;

            // Whether the live shout is a DRIVER'S CAST -- armed at `SBF_ShoutStart`
            // with no `BeginCastVoice` behind it -- rather than the player's own shout.
            //
            // It exists for one mechanical reason and one diagnostic one.
            //
            // MECHANICAL, AND IT IS BELT-AND-BRACES OVER AN ASSERTION NOBODY HAS MEASURED. It keeps
            // the driver's bucket at `(0, kNone, sneaking, drawn)`, which is the key this
            // flag was designed to hold.
            //
            // `ReadShoutVariation` is SUPPOSED to return `kNone` here on its own: a
            // `currentShout` null test exists for exactly this case, because
            // `currentShoutVariation` is not cleared when no shout is cast and would otherwise
            // return the PREVIOUS shout's word count. If that test holds on a hotbar cast, this
            // flag changes nothing.
            //
            // WHETHER IT HOLDS HAS NEVER BEEN OBSERVED. The first hotbar cast in
            // this project did not read `currentShout`. The claim is an inference from
            // `selectedPower` -- a
            // different field -- and Spell Hotbar 2 reaches the shout graph by notifying it
            // directly, which is not a path that obviously clears anything.
            //
            // So the two outcomes are: this flag is redundant, or it is the only thing keeping one
            // bucket from silently splitting three ways by an unrelated shout's word count and
            // teaching the window cache wrong. One bool and one condition is the right price for
            // not having to know which. Delete it once a drive reads `currentShout` on a cast.
            //
            // DIAGNOSTIC: `shout 00000000` alone does not say which of the two things happened, and
            // the engine's own guard exists because the record disagreed with itself about what
            // Spell Hotbar 2 leaves in `selectedPower`. A trace that says which is a trace that can
            // settle that argument.
            bool driverCast = false;

            // A shout that begins mid-combo owes us one ready pass: the graph tears the outgoing
            // MCO attack down first, and that teardown runs through `inRdy`. Spent
            // once, so a genuine interruption after it still escapes.
            bool teardownReadyPending = false;

            // The player was SPRINTING when this shout began -- captured at the arm,
            // because the shout itself tears sprint down and a read at the fire edge answers for
            // the wrong moment: a hands-on drive chained real sprint shouts
            // into plain in-place attacks, while an auto-move fixture -- whose sprint re-engages
            // the instant the graph returns to locomotion -- happened to sprint-route the same
            // chain. Same lesson: ask about the player, at the moment the fact is
            // true, not about the animation after the graph has moved on.
            bool sprintingAtBegin = false;

            // A WHIRLWIND SPRINT IS ITSELF A SPRINT, and it
            // chains into the sprint attacks whether or not the player was sprinting beforehand.
            //
            // The prior-sprint requirement was a design choice in a first pass, never a ruling,
            // and it does not survive the request it was built for. After whirlwind sprint the
            // graph offers sprint attacks, which says nothing about already sprinting.
            // Whirlwind Sprint always chains to sprint attacks. It is also the reading the
            // motion supports: the propel is root-motion
            // locomotion (`WhirlwindSprintIsActiveModifer` binds `bAnimationDriven`, `#0070`), so
            // the player IS sprinting during it by every measure except the actor bit.
            //
            // DETECTED FROM THE GRAPH'S OWN VOCABULARY, NOT A FORMID LIST. Whirlwind Sprint is its
            // own branch out of the inhale, entered by the selector events
            // `ShoutSprint{Short,Medium,Long,Longest}Start`.
            // Those reach `Observe` already -- the "hout" needle matches them -- so the shout
            // announces itself. A FormID list would need four vanilla entries
            // (`02F7BA`/`07A4C8`/`10F731`/`03547B`) and would still miss any mod that routes a new
            // shout through the same states; the graph reading covers all of them for free.
            bool whirlwindThisShout = false;

            // Three separate facts about one press, because they end at different moments.
            // Ownership lasts until the button comes up -- follow-ups to a down edge we ate
            // must keep being eaten, or the game's own handler sees a release with no press and
            // charges an attack of its own on top of the chained one. The pending press
            // outlives ownership: ADR-0003 buffers it "for the duration of the shout", so a
            // tap-and-release still fires when the window opens.
            bool       pressOwned = false;
            bool       pressPending = false;
            bool       pressResolved = false;
            double     pressedAtMs = 0.0;
            AttackKind pressKind = AttackKind::kLight;
            // The press was made after a shout was queued but before that shout
            // reached `BeginCastVoice`. It is ours, but it is not yet inside a shout: resolving
            // it must wait for `BeginShoutLocked` rather than taking `TryFireChainLocked`'s
            // ordinary post-shout hand-back path. `Reset()` deliberately clears this when the
            // queued shout begins; the press fields themselves are carried across below.
            bool waitingForShoutStart = false;
            // When ownership was taken. Carried across `Reset()` with `pressOwned` itself, because
            // a preserved latch with a reset clock reads as instantly expired.
            double pressOwnedAtMs = 0.0;

            // Read at shout start, written back after the cut's ready pass has reset it.
            int resumeAttack = 0;
            int resumePowerAttack = 0;

            bool       awaitingReady = false;
            double     cutAtMs = 0.0;
            AttackKind pendingKind = AttackKind::kLight;

            // Whether the engine believes `SHOUT_lock` is set in the graph --
            // the C3 root, which is a graph gate and not a control-map toggle (deviation D7).
            //
            // CARRIED ACROSS `Reset()`, and it is now the only field that is. A rooting that
            // fails to lift is worse than no rooting: a reset that dropped this flag would leave
            // the graph variable at 1 with nothing left that knows to clear
            // it, and the player steers nothing for the rest of the session. Only an explicit
            // clear path may clear it.
            bool rootLocked = false;

            // Observation only. Armed when a chained attack goes out, so the trace carries a
            // velocity and displacement profile across the swing rather than one sample at the
            // start -- travel is what the bug is, and travel is a distance over time.
            bool         motionWatch = false;
            double       motionArmedAtMs = 0.0;
            RE::NiPoint3 motionOrigin{};
            float        motionPeakVelocity = 0.0f;

            void Reset() {
                const bool rooted = rootLocked;
                *this = ChainState{};
                // Carried across, so a reset cannot lose track of a root that is still in force --
                // see the field.
                rootLocked = rooted;
            }
        };

        ChainState g_state;

        // ADR-0006 -- THE WHOLE DISCRIMINATOR, AND IT IS ONE BOOL.
        // GUARDED BY `detail::g_engineLock`.
        //
        // *A driver cast is a shout-state entry with no `BeginCastVoice` behind it.* `BeginCastVoice`
        // sets this; `SBF_ShoutStart` consumes it. An entry that consumed a flag was vouched for by
        // a real shout -- armed OR declined -- and is never a driver's cast. An entry that found
        // none is one, and arms.
        //
        // IT DOES NOT LIVE IN `ChainState`, and that is not tidiness. `BeginShoutLocked` calls
        // `g_state.Reset()`, so a flag stored there would be wiped by the very shout it has to
        // vouch for -- the flag is set BEFORE that call precisely so a shout the engine DECLINES
        // still vouches for its own state entry, and a field the reset clears cannot do that.
        //
        // THE THREE CASES IT SEPARATES, all measured:
        //   - an ordinary shout   -- `BeginCastVoice` precedes the entry by 11.3/11.4/11.9 ms
        //                            and 31 ms. Vouched.
        //   - a hotbar cast       -- no `BeginCastVoice` at all, entry 1342 ms before spellfire.
        //                            Arms.
        //   - the JUMP BOUNCE     -- the graph RE-ENTERS the shout state 0.06 ms after `JumpUp`,
        //                            3 of 3 trials, no `BeginCastVoice` behind it. **THIS FLAG DOES
        //                            NOT SEPARATE IT**, and the second flag below is why.
        //
        // A STALE FLAG FAILS SAFE, WHICH IS WHY IT IS NOT BOUNDED. A `BeginCastVoice` that never
        // reaches a state entry leaves this set, and the next entry -- possibly a genuine cast --
        // is then wrongly vouched and does not arm. That costs ONE missed chain and heals itself,
        // because that entry consumes the flag. The opposite failure (a false arm) is the one worth
        // spending on, and it cannot be reached this way.
        bool g_beginSeenPending = false;

        // THE SECOND HALF OF THE DISCRIMINATOR, AND IT IS A CORRECTION TO ADR-0006.
        // GUARDED BY `detail::g_engineLock`.
        //
        // **ADR-0006 SAID THE BEGIN FLAG ALONE WAS ENOUGH, AND THE LOG IT CITES FALSIFIES THAT.**
        // The ADR's sketch arms on `!vouched && !shoutActive`, reasoning that the jump bounce is
        // covered because the shout it interrupts is still live. It is not live. In all three
        // trials the state exit ends the shout
        // ~60 MICROSECONDS BEFORE the re-entry, so `shoutActive` is already false when the bounce
        // arrives:
        //
        //     47540.33  SBF_ShoutStop
        //     47540.34  >>> SHOUT end, no press waiting (SBF_ShoutStop)   <- shoutActive = false
        //     47540.35  JumpUp
        //     47540.40  SBF_ShoutStart      <- unvouched AND unheld: the ADR's condition is TRUE
        //
        // So the single-token design arms on the jump bounce, which is the one hazard ADR-0006 was
        // written to prevent.
        //
        // WHAT SEPARATES THEM IS THE READY EXIT, AND THE ADR ALREADY MEASURED IT -- 4 of 4 genuine
        // shout-state entries are preceded by `SBF_ReadyStop` ~0.06 ms before, and 3 of 3 jump
        // bounces have none:
        //
        //     ordinary shout   149873.42 -> 149873.47
        //     hotbar cast      145242.17 -> 145242.24
        //     jump bounce      JumpUp -> entry, no token
        //
        // ADR-0006 CONSIDERED REQUIRING BOTH TOKENS AND REJECTED IT, on the grounds that "the second
        // token pays a per-event cost to guard a hypothesis". **It is not a hypothesis.** The cost
        // argument was sound and its premise was not: it compared a cheap correct design against a
        // dear one, and the cheap design is measurably wrong. One `tag == "SBF_ReadyStop"sv` per
        // graph event is what correctness costs here.
        //
        // NEITHER TOKEN WORKS ALONE, WHICH IS WHY THIS IS AND, NOT OR:
        //
        //     case                      ready token   begin vouched   arms
        //     ordinary shout                yes            yes         no
        //     shout, engine off             yes            yes         no   <- ready token alone fails
        //     shout, no power selected      yes            yes         no   <- ready token alone fails
        //     HOTBAR CAST                   yes            no          YES
        //     jump bounce                   no             no          no   <- begin flag alone fails
        //
        // The ADR's own reason for preferring the begin flag survives intact and is the reason both
        // are kept: `BeginShoutLocked` returns before `shoutActive` on two paths, so a GENUINE
        // ordinary shout can enter the shout state with the engine holding nothing, and a ready-exit
        // test cannot see that because a declined shout comes out of ready exactly like a cast does.
        //
        // A STALE READY TOKEN FAILS SAFE IN THE SAME DIRECTION as the begin flag: a ready exit that
        // never reaches a shout-state entry leaves this set, and the next entry consumes it. It can
        // only ever cause an arm that the begin flag has ALSO declined to veto -- which is a genuine
        // unvouched entry out of ready, i.e. the thing this arms on anyway.
        bool g_readyExitPending = false;

        // MCO's attack state, kept apart from ChainState because it is not the shout's and must
        // survive `ChainState::Reset` -- which runs at shout start, exactly when a held press is
        // about to be decided on.
        // WHERE THE COMBO POSITION IS CAPTURED, AND WHY IT IS NOT AT `BeginCastVoice`.
        //
        // Resuming the combo is what this engine exists for: attack 1, attack 2, shout, and the
        // next attack must be 3. The index used to be read in `BeginShout`, and that worked only
        // while the shout began INSIDE the outgoing attack -- the trace read `resume attack=2`
        // because MCO's counter was still live.
        //
        // Both gates that end the attack BEFORE the shout starts do it
        // deliberately. MCO resets `MCO_nextattack` to 1 when an attack ends, so by `BeginCastVoice`
        // the position is already gone and the engine restored 1 because it honestly read 1 --
        // the combo starts back over at 1, which is the feature
        // rather than a detail of it.
        //
        // So the snapshot is taken when the press is QUEUED, which is the last moment the counter
        // still means anything, and `BeginShoutLocked` prefers it over a live read.
        //
        // `takenAtMs` is why the snapshot expires: it used to be cleared in exactly one place --
        // `BeginShout` -- so a replay whose shout never started left it valid indefinitely, and
        // the next ordinary shout, minutes later, resumed a combo from an unrelated attack. Now
        // it expires by age and is abandoned explicitly on every path that orphans it.
        struct QueuedResume {
            bool   valid = false;
            double takenAtMs = 0.0;
            int    nextAttack = 0;
            int    nextPowerAttack = 0;
            int    currentAttack = 0;
            int    currentPowerAttack = 0;
        };
        QueuedResume g_queuedResume;

        // How much older than the wait cap a snapshot may be and still describe its own shout.
        // The longest legitimate path from queue to `BeginCastVoice` is the full wait
        // (`shoutWaitCapMs`) plus the release-to-replay hop (~7ms) plus the replayed tap (~137ms)
        // plus the handler's charge-to-cast (~3ms measured); 1500ms buries all of that with an
        // order of magnitude to spare while staying far below "a different fight".
        constexpr double kQueuedResumeSlackMs = 1500.0;

        // THE LAST COMBO POSITION THE PLAYER WAS ACTUALLY IN.
        // GUARDED BY `detail::g_engineLock`.
        //
        // `g_queuedResume` above is filled by a *shout-key press*. A Spell Hotbar 2 cast never
        // presses the shout key, so nothing fills it, and by the time the engine can see the cast
        // at all the position is gone: a hotbar cast raises nothing before `Voice_SpellFire_Event`,
        // and the interrupting teardown's `inRdy` has already reset
        // `MCO_nextattack` to 1 before that instant. A live read there returns the reset value, and
        // restoring it would "preserve" a combo the player was never in.
        //
        // So the reading has to be taken BEFORE the cast tears it down, and this is it.
        //
        // WHY THE CAPTURE SITE IS THE DISCRIMINATOR AND THE VALUE IS NOT. `MCO_nextattack == 1` is
        // ambiguous on its own -- it is both the teardown's reset value and the legitimate value at
        // the start of a combo. Nothing can tell those apart by looking at the number. What CAN be
        // told apart is *when* the reading was taken: this is written only at attack-time events
        // (`MCO_AttackInitiate`, `MCO_PowerAttackInitiate`, `HitFrame`), never during a teardown, so
        // whatever stands then is the player's real position. MCO advances `MCO_nextattack` to the
        // correct next index when each attack *begins*, wrap included, so the value is already the
        // one a resume wants.
        //
        // ADR-0005 IS WHY THIS EXISTS RATHER THAN AN INCREMENT. Shouts and hotbar casts are
        // orthogonal to the combo: the engine preserves the index it read and never derives one.
        // Preserving still needs something to preserve, and on a late-arming path that is this.
        //
        // The trace line is what a late-arming resume needs -- "the captured index is provably the
        // pre-cast one" cannot be shown by a resume value alone, because a correct one and a lucky
        // one look identical after the fact.
        struct RollingCombo {
            bool   valid = false;
            double takenAtMs = 0.0;
            int    nextAttack = 0;
            int    nextPowerAttack = 0;
            int    currentAttack = 0;
            int    currentPowerAttack = 0;
        };
        RollingCombo g_rollingCombo;

        // How stale a rolling reading may be and still describe the fight the player is in.
        //
        // UNVALIDATED. There is no measurement behind this number yet -- it is chosen to be
        // obviously longer than a combo's own pacing and obviously shorter than "a different
        // fight". It is a named constant rather than a literal
        // so the drive that settles it has one place to change.
        //
        // The failure directions are not symmetric, and the cap is biased accordingly: too SHORT
        // and a legitimate carry is refused, which falls back to today's behaviour and costs the
        // player a combo position they would have liked. Too LONG and an index from an unrelated
        // fight is restored with confidence, which is a guess wearing a memory's clothes -- the
        // same failure `g_queuedResume`'s own age cap exists to prevent.
        constexpr double kRollingComboMaxAgeMs = 5000.0;

        bool g_mcoAttackLive = false;
        // The teardown, tracked apart from liveness. `MCO_AttackExitNotify` and
        // `attackStop` used to be treated as equivalent to `inRdy` and released the queued shout
        // on whichever arrived first -- which offered the press to a graph still resetting, and
        // such an offer is refused without a word. Now the early
        // markers only mark the teardown; `inRdy` -- the graph's own statement that it is ready
        // -- is the sole release point, and `g_mcoAttackLive` stays true through the teardown so
        // a press made during it still queues instead of walking into the same silent refusal.
        bool g_mcoAttackEnding = false;
        bool g_mcoSwingLanded = false;
        // Whether we have already spent this attack's one cancel. Reset when an attack starts.
        bool g_cancelSent = false;

        // A shout→shout intent is ARMED at confirmed `shoutStop` and DELIVERED
        // after a paced delay. Same-update `inRdy` / `IdleStop` ride the exit bundle and a
        // synthetic tap there completes with no `BeginCastVoice`. ADR-0007 allows the delay.
        bool   g_releaseShoutChainOnReady = false;
        double g_releaseShoutChainArmedAtMs = 0.0;

        // Window tracking and the light-attack initiate capture went with the `kReady` gate.
        //
        // They existed to answer that gate's two extra conditions -- "is MCO's own window
        // open" and "has MCO advanced the counter belonging to THIS attack kind" -- and nothing
        // else ever read them. The window flag had exactly one read and the three combo fields
        // one each, every one of them inside the deleted block. What was left was six writes
        // feeding no reader, which is a dead variable rather than merely a dead store.
        //
        // Nothing measurable is lost with them. `MCO_WinOpen`, `MCO_PowerWinOpen`, `MCO_WinClose`
        // and `MCO_PowerWinClose` all still reach the trace through `IsInteresting`, so MCO's own
        // window can still be measured off a log without the engine tracking it.
        //
        // TWO OF THE FOUR CAME BACK -- and the two that did not are the
        // point. `g_mcoPowerAttack` and the power half of the initiate capture are below;
        // `g_mcoWindowOpen` is NOT, and neither is the light-attack capture. The deleted gate
        // needed the window, and the window is the condition that made it cost 667-1361 ms and
        // fail on 31% of presses; the annotation behind it is
        // missing from 18.2% of power clips. The surviving wait is on the advance alone, bounded, and a
        // future reader deleting these two again should check that the window has not crept back
        // in with them.

        // What kind of attack is live, and what `MCO_nextpowerattack` read when it
        // started.
        //
        // The pair answers one question at the hit frame -- "has MCO already advanced the counter
        // for THIS attack?" -- and it is a comparison against the initiate reading rather than a
        // test against 1, because a combo wrap legitimately writes 1 and a moveset can
        // branch to 8. There is no next index to predict here and nothing tries to:
        // the engine only asks whether the value MOVED.
        //
        // `g_powerAtInitiate` is 0 when the initiate carried no sample, which reads as "cannot
        // tell" and is handled by not arming the wait at all.
        bool g_mcoPowerAttack = false;
        int  g_powerAtInitiate = 0;

        // A cut deferred at the hit frame while MCO's own advance is still coming.
        //
        // `g_advanceWaitArmed` is the whole of the state a graph event has to clear; the paced
        // poller below it is stateless apart from the generation it carries. Cleared by every
        // attack boundary -- a new initiate, the teardown markers, `inRdy`, and a game load -- so a
        // late poll can never act on an attack that is over.
        bool   g_advanceWaitArmed = false;
        double g_advanceWaitArmedAtMs = 0.0;

        // Which attack a deferred cut belongs to.
        //
        // Bumped on every initiate and on game load, and carried by value into each paced poll, so
        // a poll that drains after the player has started a DIFFERENT attack drops instead of
        // cutting it. `g_shoutGeneration` cannot do this job: it counts shouts, and the hazard here
        // is a second attack inside one shout's queue.
        std::atomic<std::uint32_t> g_attackGeneration{0};

        // Shout liveness FOR THE TRACE ONLY, read off the graph rather than off engine state.
        //
        // `g_state.shoutActive` is set in `BeginShoutLocked`, which returns early on any shout the
        // engine holds nothing for. So it can read false throughout a perfectly live shout, and the
        // forward-path marker printed "shout inactive" 385ms after `Voice_SpellFire_Event`. A marker
        // whose own field contradicts the claim it supports is an assertion, not an instrument.
        //
        // `BeginCastVoice` and `shoutStop` reach `Observe` whatever the engine is set to, so this
        // is correct with the engine off. It drives no decision; nothing reads it but the trace.
        //
        // Atomic, not lock-guarded: an independent single fact (EngineLock.h rule 4), readable
        // from the attack hook's trace line without taking the engine lock on every event.
        std::atomic<bool> g_shoutLiveForTrace{false};

        // WHETHER STATE BEHAVIOR FRAMEWORK HAS EVER SPOKEN THIS SESSION.
        //
        // The engine learns that an interrupted shout ended ONLY from SBF's
        // `SBF_ShoutStop`, with no fallback -- so on a load order WITHOUT the
        // patch, every shout cut short holds the attack button for the full liveness cap, and the
        // failure looks like this mod is broken. An SKSE plugin cannot ask "is this behaviour
        // patch installed" -- the events simply never arrive. What it can know is whether any
        // `SBF_` annotation has ever arrived. SBF is noisy: `SBF_Ready*` and `SBF_Default*` fire
        // on ordinary posture changes, and `SBF_ShoutStart` annotates the shout state's own entry,
        // so a session that reaches a LIVE shout with this still false is one where the patch is
        // genuinely absent, not one that has merely been quiet.
        //
        // Process-lifetime on purpose, and NOT reset on game load: the presence of a behaviour
        // patch is a property of the load order, which cannot change under a running game.
        //
        // Atomic, not lock-guarded: an independent single fact (EngineLock.h rule 4), written on
        // the event path before the lock is taken.
        std::atomic<bool> g_sbfEverSeen{false};

        // When the player was last observed SPRINTING, in `ElapsedMs()`
        // terms. Sampled on the per-event path, not read once at the arm.
        //
        // WHY A LATCH REPLACED THE POINT READ. The first pass read `actorState1.sprinting` at
        // `BeginCastVoice` on the argument that the bit is "true of the player at that instant".
        // It is not, on real input: the game clears sprint when the voice cast starts, and
        // `BeginCastVoice` is an ANIMATION event that arrives after it. A sprinting-Whirlwind
        // drive shows three sprinting Whirlwind
        // Sprints where the `>>> SHOUT began from a SPRINT` line never printed at all and the
        // chain fired plain `attackStart` -- the bit was already down. The auto-move fixture that
        // passed the original gates re-engages sprint the instant the graph returns to
        // locomotion, which is what put the bit back up in time to be read. Same fixture-versus-
        // real divergence named one state over; it just also swallowed the
        // reading the fix depended on.
        //
        // The latch answers "was sprinting recently", which is the question the chain actually
        // asks, and it cannot be beaten by teardown ordering because it is fed from every
        // animation event the player raises -- a sprint produces a continuous stream of them.
        //
        // NOT the sprint KEY's held state: a toggle-sprint mod, a rebind, or a hold with no
        // stamina all lie about intent, while the game's own bit is the fact.
        //
        // Reset on load with `g_rootVerified`: a stale timestamp across a load would arm the
        // sprint entry for the first shout of the next session.
        constexpr double  kSprintRecencyMs = 500.0;
        std::atomic<double> g_lastSprintingMs{-1.0e9};

        // SAMPLING HINTS -- cost switches, not state (EngineLock.h rule 4).
        //
        // Phase A of `Observe` must know whether to pay for graph reads BEFORE it holds the lock
        // that owns the state which answers that. These relaxed atomics mirror "a queued snapshot
        // wants refreshing" and "the motion watch is armed"; a stale read costs one event's worth
        // of sampling (a handful of graph-variable reads) or one skipped sample that the next
        // event makes up, never a wrong decision -- every decision re-checks the real state under
        // the lock.
        std::atomic<bool> g_comboSampleWanted{false};
        std::atomic<bool> g_motionWatchLive{false};

        // THE MEASURED EXHALE TAIL, PER KEY. GUARDED BY `detail::g_engineLock`.
        //
        // `tailMs` is `shoutStop` - `Voice_SpellFire_Event`: the part of the exhale that remains
        // AFTER the magic has fired. Not the clip length, and deliberately so -- spellfire is the
        // earliest point at which cutting still delivers the shout, so the window can
        // never open before it whatever the arithmetic says. Reference values already in hand:
        // SYHO 940.43 ms and Goetia short 615.49/615.69/615.79 ms, both MEASURED.
        // "Goetia long ~3.83 s" used to sit in this list as though it were a third reading; it is
        // not, it is 3.933 s of clip minus SYHO's spellfire offset, and Goetia's own offset is
        // ~418 ms rather than ~100 ms. The three-word tail is UNMEASURED -- a three-word Goetia
        // shout reports it.
        //
        // THE MINIMUM is stored rather than the latest or a mean, because the two failure
        // directions are not symmetric. Cached too SHORT and the window opens earlier than asked
        // for -- less commitment, toward today's behaviour, and it still chains. Cached too LONG
        // and the scheduled open never arrives before `shoutStop`, so the press falls through to
        // the shout's natural end and fires late. Min-bias keeps the engine on the mild side.
        //
        // FIXED CAPACITY, because this is reached from the animation-event path and must not
        // allocate there. A player has a handful of shouts; oldest-wins eviction costs at most one
        // permissive shout while the entry is measured again.
        //
        // This is a per-session cache and is deliberately not persisted. It costs one permissive
        // shout per key per session to rebuild, and a saved value would outlive the animation pack
        // it was measured from -- which is the one thing that would make it a lie.
        struct WindowCacheEntry {
            WindowKey key{};
            double    minTailMs = 0.0;  // 0 means the slot is empty
        };

        std::array<WindowCacheEntry, 16> g_windowCache{};
        std::size_t                      g_windowCacheNext = 0;

        // WHICH SHOUT A PACED WINDOW OPEN BELONGS TO.
        //
        // Bumped in `BeginShoutLocked` and on game load. A scheduled open claims the current value
        // and the task checks it before touching anything, so an open belonging to a shout that
        // has already finished is dropped rather than opening a window on the next one. Same shape
        // and same reason as `g_replayGeneration` in `ShoutInputHook`.
        //
        // Atomic because it is claimed on the graph path and read on the pacer's task
        // (EngineLock.h rule 4): an independent counter, not part of any multi-field snapshot.
        std::atomic<std::uint32_t> g_shoutGeneration{0};

        // The one name the C3 patch declares that this DLL depends on.
        //
        // An INT32 graph variable, default 0, verified round-tripping from SKSE by name:
        // wrote 1 read 1, wrote 7 read 7, against a negative control where an
        // undeclared name silently reads back 0. Route 1's `SHOUT_lock == 0` condition on the
        // `moveStart` transition is the only consumer in the graph.
        constexpr const char* kRootLockVariable = "SHOUT_lock";

        // ROUTE 2'S TRIGGER, AND IT IS A VANILLA EVENT ON PURPOSE.
        //
        // NAMED FOR WHAT IT DOES, NOT FOR THE FEATURE IT SERVES. It was `kRootEvent` while a first
        // candidate raised a private `SHOUT_root`, where the name was literal. It is not the root now --
        // the LOCK is the root, held for the whole shout -- and this is the one-shot that takes the
        // graph out of locomotion so the lock has something to hold.
        //
        // It was `SHOUT_root`, an event this mod's own Nemesis patch used to declare -- the
        // declaration and the wildcard transition that consumed it were dropped once
        // this route shipped, taking the patch from 9 files to 6. That candidate was measured
        // undeliverable -- `NotifyAnimationGraph` returned false six times for six,
        // in the same second that `shoutStop` and `attackStart` returned true from this same call,
        // and with `iSyncIdleLocomotion` reading 1 so a real state change was available. See
        // CONTEXT.md: a mod-declared VARIABLE is reachable by name, a mod-declared EVENT
        // is not.
        //
        // `moveStop` needs no new graph node at all. Vanilla `shout_behavior` already carries the
        // edge -- `#0321`, `ShoutLocomotion -> ShoutStanding`, `FLAG_DISABLE_CONDITION`, so it is
        // ungated and fires whenever the event arrives. Route 2 is therefore "send the event the
        // graph already listens for" rather than "build a private one and hope it arrives".
        //
        // THE REASON THIS WORKS NOW AND WOULD NOT HAVE BEFORE THE LOCK. `moveStop` is a one-shot: it
        // tells the graph to leave locomotion and `PlayerControls` puts it straight back on the
        // next update while the key is still held -- the same fact that defeated an earlier
        // control-map attempt. Route 1's gate is what makes the difference:
        // the re-raised `moveStart` now has to pass `SHOUT_lock == 0`, and while the lock is set it
        // cannot. One event to leave, one condition to stay. Neither half roots a moving start
        // alone, which is exactly why they were built together.
        //
        // The cost: `moveStop` is a vanilla event with unknown listeners across
        // the project, so its side effects are measured rather than assumed.
        constexpr const char* kLeaveLocomotionEvent = "moveStop";

        // HAS THE ROOT'S PRESENCE BEEN CHECKED THIS SESSION (the soft degrade).
        //
        // "Checked", not "present": it latches on the first check whether that check passed or
        // failed, which is what makes the warn below fire exactly once per session rather than on
        // every shout for a user who has not run Nemesis.
        //
        // The patch is a separate install step -- the user has to tick `shmco` and re-run Nemesis
        // -- so "the DLL is installed" does not imply "the graph can be locked". A write to an
        // UNDECLARED graph variable silently goes nowhere and reads back 0, which the
        // negative control established, so the write's own return value cannot tell the two apart
        // and the readback is what does.
        //
        // Latched: verified on the first shout of a session and then never paid for again. The
        // graph cannot lose a variable mid-session -- only a load can change the behaviour graph,
        // and the load listener clears this latch so the next session re-verifies.
        std::atomic<bool> g_rootVerified{false};

        // Milliseconds since the first observed event. Relative time is what a trace is read
        // against -- wall clock says nothing about clip timing. The origin is a magic static so
        // its one-time initialisation is thread-safe (the old two-variable latch could be set
        // twice by two first events racing).
        double ElapsedMs() {
            static const auto origin = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin)
                .count();
        }

        // The renderer's frame number, or 0 before the graphics state exists. Read fresh every
        // time: caching it is exactly the mistake this instrument exists to catch.
        std::uint32_t FrameCount() {
            auto* state = RE::BSGraphics::State::GetSingleton();
            return state ? state->frameCount : 0u;
        }

        // GAME READS from here down to `MovementSummary` -- call them OUTSIDE the engine lock
        // (EngineLock.h rule 2), and hand the results to the `...Locked` deciders.

        int GraphInt(RE::Actor* a_actor, const char* a_name) {
            std::int32_t out = -999;
            a_actor->GetGraphVariableInt(a_name, out);
            return static_cast<int>(out);
        }

        bool GraphBool(RE::Actor* a_actor, const char* a_name) {
            bool out = false;
            a_actor->GetGraphVariableBool(a_name, out);
            return out;
        }

        // `IsShouting` IS HERE FOR THE CHEAPEST DECIDING CELL IN THE DRIVER-CAST ARM.
        // The proposed arming for a Spell Hotbar 2 cast has two stages, and whether stage
        // one is possible at all turns on one unobserved fact: is `IsShouting` true through the
        // cast's release lead, or only once the exhale is playing?
        //
        // It matters because the leads are long. The driver notifies its exhale 250 ms before a
        // ritual cast fires and 1.0-1.5 s before a ritual concentration one, and one
        // attack trial pressed **775 ms before** `Voice_SpellFire_Event`. If `IsShouting` is true
        // across that lead, the press can be buffered and the cast can chain; if it goes true only
        // at the exhale, it cannot, and the design is one stage rather than two.
        //
        // This is the same graph bool Spell Hotbar 2 itself uses as its liveness check,
        // so it is the driver's own signal being read rather than an inference about it. Animation
        // state, not cooldown state -- ADR-0002's scope note covers exactly this distinction.
        //
        // Costs nothing when tracing is off: `SHOUTMCO_TRACE` is a macro and does not evaluate its
        // arguments, so this whole function is unreached at `bTrace = 0`.
        std::string GraphSummary(RE::Actor* a_actor) {
            // `IsAttacking` at `inRdy` after cancel is still true on 1H and 2H;
            // the later event where it falls is shout-admissible. `inRdy` itself was not
            // interesting, so the hang log had the tag and no graph vars beside it.
            int attackState = -1;
            if (auto* state = a_actor->AsActorState()) {
                attackState = static_cast<int>(state->GetAttackState());
            }
            return std::format(
                "MCO_currentattack={} MCO_nextattack={} MCO_currentpowerattack={} "
                "MCO_nextpowerattack={} MCO_IsInRecovery={} MCO_IsPowerAttacking={} "
                "IsShouting={} IsAttacking={} attackState={}",
                GraphInt(a_actor, "MCO_currentattack"), GraphInt(a_actor, "MCO_nextattack"),
                GraphInt(a_actor, "MCO_currentpowerattack"), GraphInt(a_actor, "MCO_nextpowerattack"),
                GraphBool(a_actor, "MCO_IsInRecovery"), GraphBool(a_actor, "MCO_IsPowerAttacking"),
                GraphBool(a_actor, "IsShouting"), GraphBool(a_actor, "IsAttacking"), attackState);
        }

        // THE PLAYING CLIP, NOT AN OAR SUBMOD NAME (ADR-0009).
        //
        // Walks active clip generators and reads `Voice_SpellFire_Event` off the HKX annotation
        // tracks. Unannotated packs and a failed walk both return empty, which
        // `DecideClipOwnedSpellFire` treats as fail-open. Called from `Observe` during graph
        // dispatch: do not take the manager's `updateLock` here -- the dispatcher already holds
        // the graph stable for this event, and taking it is an ABBA with that path.
        struct ClipSpellFireSample {
            std::optional<float> annotationS;
            std::optional<float> localTimeS;
        };

        [[nodiscard]] std::optional<float> SpellFireAnnotationTime(const RE::hkaAnimation* a_anim) {
            if (!a_anim) return std::nullopt;
            std::optional<float> latest;
            const auto&          tracks = a_anim->annotationTracks;
            for (auto t = 0; t < tracks.size(); ++t) {
                const auto& track = tracks[t];
                for (auto i = 0; i < track.annotations.size(); ++i) {
                    const auto& anno = track.annotations[i];
                    const char* text = anno.text.c_str();
                    if (!text || !text[0]) continue;
                    if (std::string_view{text} != "Voice_SpellFire_Event"sv) continue;
                    if (!latest || anno.time > *latest) {
                        latest = anno.time;
                    }
                }
            }
            return latest;
        }

        [[nodiscard]] RE::hkaAnimation* AnimationOf(RE::hkbClipGenerator* a_clip) {
            if (!a_clip) return nullptr;
            if (a_clip->binding && a_clip->binding->animation) {
                return a_clip->binding->animation.get();
            }
            if (a_clip->animationControl) {
                auto* control = a_clip->animationControl.get();
                if (control && control->binding && control->binding->animation) {
                    return control->binding->animation.get();
                }
            }
            return nullptr;
        }

        void ConsiderClip(RE::hkbClipGenerator* a_clip, ClipSpellFireSample& a_out) {
            if (!a_clip) return;
            const auto time = SpellFireAnnotationTime(AnimationOf(a_clip));
            if (!time) return;
            if (!a_out.annotationS || *time > *a_out.annotationS) {
                a_out.annotationS = time;
                a_out.localTimeS = a_clip->localTime;
            }
        }

        [[nodiscard]] ClipSpellFireSample ReadPlayingClipSpellFire(RE::Actor* a_actor) {
            ClipSpellFireSample out{};
            if (!a_actor) return out;

            RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
            if (!a_actor->GetAnimationGraphManager(manager) || !manager) {
                return out;
            }

            for (const auto& graph : manager->graphs) {
                if (!graph || !graph->behaviorGraph) continue;
                auto* nodes = graph->behaviorGraph->activeNodes;
                if (!nodes) continue;
                const auto count = nodes->size();
                if (count <= 0) continue;
                constexpr auto kCap = static_cast<RE::hkArrayBase<RE::hkbNodeInfo>::size_type>(256);
                const auto     n = count < kCap ? count : kCap;
                for (RE::hkArrayBase<RE::hkbNodeInfo>::size_type i = 0; i < n; ++i) {
                    auto* clone = (*nodes)[i].nodeClone;
                    if (!clone) continue;
                    if (auto* clip = skyrim_cast<RE::hkbClipGenerator*>(clone)) {
                        ConsiderClip(clip, out);
                        continue;
                    }
                    if (auto* sync = skyrim_cast<RE::BSSynchronizedClipGenerator*>(clone)) {
                        ConsiderClip(sync->clipGenerator, out);
                    }
                }
            }
            return out;
        }

        std::string_view Describe(AttackKind a_kind) {
            return a_kind == AttackKind::kPower ? "power"sv : "light"sv;
        }

        // Never call into the graph from inside its own event dispatch. Everything the engine
        // emits is posted back through the task interface instead; the cost is one task drain,
        // measured at ~27ms end to end against ~10ms for firing in place.
        void Defer(RE::Actor* a_actor, std::function<void(RE::Actor*)> a_work) {
            auto* task = SKSE::GetTaskInterface();
            if (!task || !a_actor) return;

            const auto handle = a_actor->CreateRefHandle();
            task->AddTask([handle, work = std::move(a_work)]() {
                auto ref = handle.get();
                if (!ref) return;
                if (auto* actor = ref->As<RE::Actor>()) {
                    work(actor);
                }
            });
        }

        // Which exhale is about to play, as far as this engine can tell. A GAME READ,
        // so it happens in phase A and the answer is handed to `BeginShoutLocked` -- the same
        // shape `WasAttacking` below uses, and for the same EngineLock.h rule 2 reason.
        //
        // A null `selectedPower` yields shout 0, which is a real bucket and not a failure: Spell
        // Hotbar 2 empties the slot for its own casts, and a pack that ships one clip
        // for everything wants one bucket anyway.
        WindowKey ReadWindowKey(RE::Actor* a_actor) {
            WindowKey key{};
            if (const auto* power = a_actor->GetActorRuntimeData().selectedPower) {
                key.shout = power->GetFormID();
            }
            if (const auto* state = a_actor->AsActorState()) {
                key.sneaking = state->IsSneaking();
                key.drawn = state->IsWeaponDrawn();
            }
            return key;
        }

        // WORD COUNT, READ AT SPELLFIRE AND NOWHERE ELSE.
        //
        // The rest of `WindowKey` is read at `BeginCastVoice` because that is the one moment the
        // selected power is still trustworthy. This field is the exact opposite and the two must
        // not be merged into one read:
        //
        //   - TOO EARLY IS WRONG. At `BeginCastVoice` the player has not released the key, so no
        //     variation has been chosen yet -- the value is `kNone` or, worse, the PREVIOUS shout's,
        //     which would key this shout's measurement to the last one's word count.
        //   - TOO LATE IS WRONG. `shoutStop` may already have cleared it, so a read taken when the
        //     tail is recorded returns `kNone`. Spellfire is after the release and
        //     before the teardown, which is the only interval where the answer is both decided and
        //     still present.
        //
        // ADR-0002 IS THE REASON THIS FUNCTION TOUCHES ONE FIELD AND RETURNS. `voiceTimeElapsed`
        // (0x014) and `voiceRecoveryTime` (0x018) sit four and eight bytes past the field being
        // read, and reading either would be cooldown inspection. The proximity is what makes an
        // accidental widening easy, so the narrowness here is deliberate -- do not "helpfully"
        // return more of this struct.
        //
        // A null high process yields `kNone`, which is a real bucket rather than a failure: the
        // player is always in high process, so this only answers at all for the actor-agnostic path
        // that is not switched on.
        //
        // `currentShout` IS CHECKED, AND IT IS NOT BELT-AND-BRACES. The field this reads is not
        // cleared when no shout is cast, so on a Spell Hotbar 2 driver cast -- which fires with
        // no shout equipped -- it holds THE PREVIOUS SHOUT'S word count. That is not a missing
        // reading the `kNone` bucket catches; it is a confident wrong one, and it lands the
        // driver's measurement in the bucket belonging to whatever the player last shouted. A
        // null shout or `kNone` must map to 'unknown', never to 'one word'. The shipped code
        // covered `kNone`; this covers the null shout.
        //
        // `currentShout` sits at `0x008`, EARLIER than the field being read, so the ADR-0002
        // caveat below is untouched -- this widens the read away from the cooldown fields, not
        // toward them.
        [[nodiscard]] std::uint32_t ReadShoutVariation(RE::Actor* a_actor) {
            if (const auto* high = a_actor->GetHighProcess(); high && high->currentShout) {
                return static_cast<std::uint32_t>(high->currentShoutVariation);
            }
            return static_cast<std::uint32_t>(RE::TESShout::VariationID::kNone);
        }

        // Was there an attack for the shout to interrupt? Asked at shout start, because by the
        // time the teardown's `inRdy` arrives the graph has already reset every trace of it --
        // `MCO_nextattack` is back to 1 and the game's attack state is clear.
        //
        // Two independent readings, because the game's attack state and MCO's combo do not end
        // together: a combo still open past its first attack reads `MCO_nextattack > 1` long
        // after `meleeAttackState` has gone back to `kNone`, and leaving *that* state passes
        // through ready just the same.
        bool WasAttacking(RE::Actor* a_actor) {
            if (auto* state = a_actor->AsActorState();
                state && state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) {
                return true;
            }
            return GraphInt(a_actor, "MCO_currentattack") > 0 || GraphInt(a_actor, "MCO_nextattack") > 1 ||
                   GraphBool(a_actor, "MCO_IsInRecovery");
        }

        // What the ANIMATION is doing. These bits follow the locomotion state, so during a shout
        // -- which is not a locomotive state -- they read false even with a movement key held
        // down. That is why the first version of the moveStop stopgap never fired: by the time it
        // asked, the graph had left locomotion for the exhale.
        bool IsMoving(RE::Actor* a_actor) {
            auto* state = a_actor->AsActorState();
            if (!state) return false;
            const auto& moving = state->actorState1;
            return moving.movingForward || moving.movingBack || moving.movingRight || moving.movingLeft;
        }

        // What the PLAYER is doing. Taken from the input watcher's own tracking of the movement
        // controls, NOT from `PlayerControls::data.moveInputVec`: that vector is live only during
        // the frame's input phase, and every chain reads it as (0.00,0.00) from inside the
        // deferred task, holding W or not.
        bool HasMovementInput() {
            auto* controls = RE::PlayerControls::GetSingleton();
            return AttackInputHook::IsMovementInputHeld() || (controls && controls->data.autoMove);
        }

        std::string MovementSummary(RE::Actor* a_actor) {
            return std::format("{} animMoving={}", AttackInputHook::MovementInputSummary(),
                               IsMoving(a_actor));
        }

        // Is this shout a Whirlwind Sprint?
        //
        // THE FORMID, BECAUSE THE GRAPH DOES NOT ANSWER. The first attempt at this read the
        // branch's own selector events (`ShoutSprint{Short,Medium,Long,Longest}Start`)
        // and would have been dead code: those are transition events the shout
        // system sends INTO the graph, never annotations a clip raises, so they never reach the
        // anim-event hook. A real Whirlwind Sprint raises `BeginCastVoice`,
        // `SBF_ShoutStart`, `Voice_SpellFire_Event`, `SBF_ShoutStop` -- every one of them generic,
        // nothing that says "whirlwind". The near miss was `ShoutSprintFire`, which appears 28
        // times across the archived logs and is neither an event nor a shout: the TAG is
        // `SoundPlay` and `MAGShoutSprintFire` is its sound descriptor, raised by an MCO attack.
        //
        // So it is the three Skyrim.esm shouts, which is every one the PLAYER can hold:
        // `WhirlwindSprintShout` is the one a player learns, `MQ105WhirlwindSprintShout` is the
        // Ustengrav quest copy, and `GreybeardWhirlwindSprintShout` is Arngeir's. Miraak's
        // `DLC2MiraakWhirlwindSprintShout` (`03547B:Dragonborn.esm`) is deliberately absent -- it
        // is an NPC shout, its runtime FormID depends on the load index, and no player chain can
        // reach it.
        //
        // A mod-added whirlwind clone is not covered here and does not need to be: it still takes
        // the sprint entry events through `sprintingAtBegin` when the player was actually
        // sprinting, which is the general rule this one is an exception to.
        bool IsWhirlwindSprintShout(RE::FormID a_shout) {
            return a_shout == 0x0002F7BA || a_shout == 0x0007A4C8 || a_shout == 0x0010F731;
        }

        // The game's own sprint bit, read live. True only while locomotion is actually
        // sprinting -- which is NOT the whole of a shout's arm (see `g_lastSprintingMs`), so no
        // decision reads this alone.
        bool IsSprintingNow(RE::Actor* a_actor) {
            auto* state = a_actor->AsActorState();
            return state && state->actorState1.sprinting;
        }

        // Feeds the recency latch. Called from `Observe` on every player
        // animation event: one pointer deref and a bit test, which is the same order of cost as
        // the tag comparisons already on that path, and it is the only sampling rate that
        // survives the shout tearing sprint down before `BeginCastVoice` arrives.
        void SampleSprint(RE::Actor* a_actor) {
            if (IsSprintingNow(a_actor)) {
                g_lastSprintingMs.store(ElapsedMs(), std::memory_order_relaxed);
            }
        }

        // Was the player sprinting at, or just before, this moment? The live bit OR the latch,
        // so a read taken after the cast has already dropped sprint still answers correctly.
        bool WasSprintingRecently(RE::Actor* a_actor) {
            if (IsSprintingNow(a_actor)) return true;
            const double last = g_lastSprintingMs.load(std::memory_order_relaxed);
            return (ElapsedMs() - last) <= kSprintRecencyMs;
        }

        // The sprint entry events, chosen at the fire edge. The light event is one
        // name for every family; the POWER event is per weapon family -- unlike
        // `attackPowerStartInPlace`, which the graph resolves itself -- so the family is read off
        // the equipped weapon. Greatsword is 2HM; battleaxe and warhammer are 2HW; everything
        // else (1H, fists, anything unreadable) takes the 1H event, which is also the value that
        // fails toward today's behaviour if a moveset lacks the 2H variants.
        const char* SprintAttackEvent(RE::Actor* a_actor, AttackKind a_kind) {
            if (a_kind != AttackKind::kPower) return "attackStartSprint";
            auto weaponType = RE::WEAPON_TYPE::kHandToHandMelee;
            if (auto* obj = a_actor->GetEquippedObject(false)) {
                if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    weaponType = weap->GetWeaponType();
                }
            }
            if (weaponType == RE::WEAPON_TYPE::kTwoHandSword) return "attackPowerStart_2HMSprint";
            if (weaponType == RE::WEAPON_TYPE::kTwoHandAxe) return "attackPowerStart_2HWSprint";
            return "attackPowerStart_Sprint";
        }

        // Animation-driven motion -- AMR-style root motion, which is what actually carries an MCO
        // attack forward. None of the movement readings above see it: it is not player input and it
        // is not the locomotion flags. `outVelocity` is the character controller's own answer, and
        // displacement from the armed origin is the only reading that measures the complaint
        // directly, since "runs forward" is a distance, not a state.
        float ControllerVelocity(RE::Actor* a_actor) {
            auto* controller = a_actor->GetCharController();
            return controller ? controller->outVelocity.Length3() : 0.0f;
        }

        // One motion sample, taken as GAME reads outside the lock and formatted as a pure
        // function of the values -- the old `MotionSummary` read the graph AND mutated the peak
        // in one call, which the lock discipline cannot host.
        struct MotionReading {
            float        velocity = 0.0f;
            RE::NiPoint3 pos{};
            bool         animDriven = false;
            bool         moveAnimDriven = false;
            bool         graphAnimDriven = false;
            std::string  held;
        };

        MotionReading ReadMotion(RE::Actor* a_actor) {
            return MotionReading{
                .velocity = ControllerVelocity(a_actor),
                .pos = a_actor->GetPosition(),
                .animDriven = a_actor->IsAnimationDriven(),
                .moveAnimDriven = a_actor->IsMovementAnimationDriven(),
                .graphAnimDriven = GraphBool(a_actor, "bAnimationDriven"),
                // The held reading rides along on every sample, because "a key held throughout"
                // is a claim about the whole swing and one reading at the edge cannot support it.
                .held = AttackInputHook::MovementInputSummary(),
            };
        }

        std::string FormatMotion(const MotionReading& a_reading, float a_travelled) {
            return std::format(
                "animDriven={} moveAnimDriven={} bAnimationDriven={} vel={:.1f} travelled={:.1f} {}",
                a_reading.animDriven, a_reading.moveAnimDriven, a_reading.graphAnimDriven,
                a_reading.velocity, a_travelled, a_reading.held);
        }

        // THE ROOTING SEAM IS GONE, AND THIS NOTE IS WHAT REPLACES IT.
        //
        // `SuppressMovement` / `RestoreMovement` toggled `ControlMap::UEFlag::kMovement`, behind
        // `constexpr bool kRootDuringChain = false` and a 3 s watchdog, with three independent
        // restores. All of it is deleted. Two reasons, and the second is the one that generalises:
        //
        //   1. IT NEVER WORKED. Measured: 13 suppressions, 13 clean
        //      restores, and it "rooted NOTHING the player could see" -- `ToggleControls` does not
        //      stop an actor already in locomotion -- while the zeroed movement state cost the
        //      moveset its direction, so a forward press produced the BACK power attack.
        //   2. ROOTING IS BEHAVIOUR-ONLY, STACK-WIDE. No DLL toggles
        //      controls, and no DLL writes movement state on its own clock. Spell Hotbar 2 retired
        //      its own movement capture the same day (ADR-0015).
        //
        // What roots the player now is a `BSIsActiveModifier` on the `ShoutStanding` state,
        // authored in the `shmco` patch (`#shmco$3`..`#shmco$5`), binding `bAnimationDriven` and
        // `bAllowRotation` and gated on the vanilla `IsPlayer` variable so no NPC is touched. It is
        // vanilla's own mechanism: `#0070 WhirlwindSprintIsActiveModifer` binds `bIsActive0` to
        // `bAnimationDriven` on the Whirlwind Sprint state in this same file.
        //
        // The `SHOUT_lock` write and the `moveStop` raise below STAY. They are not the root -- they
        // are what puts the player into `ShoutStanding` in the first place, which is the state the
        // modifier acts on. With route 2 inert (no `SBF_ShoutStart`
        // in the whole session) the shout ran in `ShoutLocomotion` with
        // footfalls throughout, so nothing would have been rooted at all.

        // WHAT ONE EVENT DECIDED TO EMIT, executed only after the lock is released -- phase C of
        // the EngineLock.h discipline. Decisions are made under the lock; everything here is a
        // game call or leads to one, so it must not be.
        struct Emit {
            bool             releaseHeld = false;
            std::string_view releaseReason{};
            bool             cancelRecovery = false;

            // FireChain's cut.
            bool        cut = false;
            std::string cutEvent;

            // ResumeAndAttack, planned under the lock from the resume state.
            bool        fire = false;
            bool        fireResume = false;
            const char* fireVar = nullptr;
            int         fireIndex = 0;
            std::string fireEvent;
            AttackKind  fireKind = AttackKind::kLight;
            // the shout this press chains out of began from a sprint, so the fire task
            // may substitute the sprint entry event -- decided there, where the player's movement
            // at the attack edge can be read.
            bool        fireFromSprint = false;

            // one paced poll for MCO's power-combo advance. Same shape as the window
            // open below, and re-emitted by the poll itself until the advance lands or the bound
            // expires -- so this is one tick, not the whole wait.
            bool          scheduleAdvancePoll = false;
            double        scheduleAdvancePollInMs = 0.0;
            std::uint32_t scheduleAdvanceGeneration = 0;

            // a paced window open, decided under the lock and slept for outside it.
            bool          scheduleWindowOpen = false;
            double        scheduleWindowInMs = 0.0;
            std::uint32_t scheduleGeneration = 0;

            // sheathed shout ends never raise `inRdy`. IdleStop arrives in the same
            // graph update as `shoutStop`, and an immediate replay there is silently refused
            // (tap completes, no BeginCastVoice). Sleep briefly, then release on the main thread.
            bool          scheduleShoutChainRelease = false;
            double        scheduleShoutChainReleaseInMs = 0.0;
            std::string_view scheduleShoutChainReleaseReason{};

            // the C3 root. `setLock` writes `SHOUT_lock`; `raiseRoot` also
            // notifies `kLeaveLocomotionEvent`, which drives the vanilla `ShoutLocomotion -> ShoutStanding`
            // edge and closes the moving-start route. `verifyLock` asks the task to read the value
            // back once.
            bool setLock = false;
            int  lockValue = 0;
            bool raiseRoot = false;
            bool verifyLock = false;

            // Motion instrumentation, composed under the lock and printed after it.
            bool             motionSample = false;
            float            motionTravelled = 0.0f;
            bool             motionEnd = false;
            std::string_view motionEndReason{};
            float            motionEndPeak = 0.0f;
            double           motionEndHeldMs = 0.0;
        };

        void ResumeAndAttackLocked(const Settings& a_settings, AttackKind a_kind, Emit& a_emit);
        void AttackWithoutResumeLocked(const Settings& a_settings, AttackKind a_kind, Emit& a_emit);
        void AbandonQueuedResumeLockedImpl(std::string_view a_reason);

        // LIFT THE ROOT. Called from every path that ends a shout, and the release
        // paths are the review's first target because a root that fails to lift is worse than no
        // root at all.
        //
        // Guarded on `rootLocked` rather than emitting unconditionally, so an end arriving for a
        // shout the engine never rooted -- a shout begun with the engine disabled, or one declined
        // for having no shout equipped -- costs no task and writes nothing. The flag survives
        // `Reset()` precisely so this guard cannot be the thing that loses a live root.
        //
        // A SPELL HOTBAR 2 CAST WAS IN THAT LIST AND IS NOT ANY MORE. A driver's cast now roots
        // on the same rule as a shout, so it reaches here
        // with `rootLocked` set and lifts through this function like everything else. That is why
        // the lift needed no new call site: `EndShoutLocked` already covers every ending including
        // `SBF_ShoutStop`, which is measured firing on a cast at `147513.87`,
        // and `FireChainLocked` covers the cut.
        void ClearRootLockLocked(Emit& a_emit) {
            if (!g_state.rootLocked) return;
            g_state.rootLocked = false;
            a_emit.setLock = true;
            a_emit.lockValue = 0;
            // Cleared rather than left as found. No path today hands one `Emit` to a begin and then
            // to a clear, so these are already false whenever this runs -- but "already false"
            // is a fact about the callers, and an unroot that also raised `kLeaveLocomotionEvent` would tell
            // the graph the player stopped moving at the exact moment it is handing steering back.
            // Stating it here costs two lines and stops the next caller from having to be careful.
            a_emit.raiseRoot = false;
            a_emit.verifyLock = false;
        }

        // THE ATTACK BUTTON MUST COME BACK.
        //
        // `OnAttackButton` takes ownership of an attack press made during a shout and then swallows
        // EVERY right-attack event until it is released. Until this watchdog, that release had one
        // exit: an `IsUp()` event reaching that one hook. `BeginShoutLocked` carries ownership
        // across `Reset()`, `EndShoutLocked` does not clear it, and the chain firing does not
        // clear it -- so a single missed release left the player unable to attack for the rest of
        // the session. That is strictly worse than the bug the swallowing exists to fix, which is
        // the same argument the shout side was built on and which the older attack
        // side never got.
        //
        // Rides the graph, like the restore watchdog, so it needs no timer of its own.
        //
        // The cap sits far above any real press. Letting go is what normally ends ownership, and a
        // deliberate power-attack hold is well under a second; anything past the cap means the
        // release was already lost. Clearing early costs at worst one attack the game charges from
        // a release it did see -- which is a swing, not a dead button.
        void WatchdogReleasePressLocked(const Settings& a_settings, Emit& a_emit) {
            // A press buffered before `BeginCastVoice` cannot use the ordinary release
            // path: there is no live shout yet, so `TryFireChainLocked` would immediately treat it
            // as a post-shout hand-back. Hold the resolved press for the same four-second bound as
            // ownership and the queued shout itself. If no shout begins inside that bound, give
            // the attack back explicitly instead of leaving a tap parked forever.
            if (g_state.waitingForShoutStart && g_state.pressPending) {
                const auto waited = ElapsedMs() - g_state.pressedAtMs;
                if (waited > static_cast<double>(a_settings.pressOwnershipCapMs)) {
                    g_state.waitingForShoutStart = false;
                    // The snapshot is the queued-shout token used by both attack input paths. If
                    // it survived this decision, the next unrelated attack would be swallowed as
                    // though the refused shout were still pending. Retire the token at the same
                    // boundary as the press it justified.
                    AbandonQueuedResumeLockedImpl("queued shout never started before press watchdog"sv);
                    if (g_state.pressResolved) {
                        const auto kind = g_state.pressKind;
                        g_state.pressPending = false;
                        SHOUTMCO_TRACE("[{:10.2f}] >>> ATTACK without a cut, press {:.1f}ms old ({}) -- "
                              "the queued shout never started, watchdog handed the press back",
                              ElapsedMs(), waited, Describe(kind));
                        // No `BeginShoutLocked` means no current resume index exists in
                        // `ChainState`; those fields may still describe an older shout. This is a
                        // plain hand-back, so fire without writing either combo variable.
                        AttackWithoutResumeLocked(a_settings, kind, a_emit);
                    }
                }
            }

            if (!g_state.pressOwned) return;
            const auto held = ElapsedMs() - g_state.pressOwnedAtMs;
            if (held <= static_cast<double>(a_settings.pressOwnershipCapMs)) return;

            g_state.pressOwned = false;
            g_state.pressPending = false;
            g_state.waitingForShoutStart = false;
            SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS ownership released by the watchdog after {:.0f}ms -- "
                  "no release ever arrived", ElapsedMs(), held);
        }

        // THE ARMED CHAIN'S BOUNDED ESCAPE, AND IT IS THE ONE ESCAPE THIS ENGINE NEVER HAD.
        //
        // `FireChainLocked` is only half of firing a chain. It clears `shoutActive`, emits the cut
        // and sets `awaitingReady`; the ATTACK goes out later, from `OnReadyLocked`, once the graph
        // says it is back in ready. `awaitingReady` was cleared in three places -- an `inRdy`
        // arriving, a new shout beginning, and a game LOAD (the load listener's own `g_state.Reset()`
        // further down) -- and the first two need an event that may never come, while the third is
        // not a chain being honoured but a session ending.
        //
        // A SHEATHING CHARACTER NEVER RAISES `inRdy`. It is recovering from nothing. So the armed
        // chain waited, in silence, for a pass that was not coming: the cut `accepted=true`
        // 310 ms after `BeginWeaponSheathe` and then NOTHING -- no `CHAIN wrote`, no `CHAIN fired`,
        // no `CHAIN dropped`. The loss was finally reported against the NEXT shout, minutes later,
        // blaming a shout that did nothing wrong.
        //
        // Everything else this engine swallows already has a bound -- `pressOwnershipCapMs` and its
        // watchdog, `bufferMs`, `shoutWaitCapMs`, `kQueuedResumeSlackMs`. This was the gap.
        //
        // **THIS IS A BACKSTOP. THE SHEATHE NO LONGER REACHES IT, AND NO TRIAL HAS EVER FIRED THIS
        // LINE.** A sheathe during the exhale raises `SBF_ShoutStop` 122 ms after
        // spellfire, so the shout ends BEFORE the window it scheduled for spellfire+649 ms
        // opens; there is no cut, so nothing arms, and the press is handed back instead
        // (a jump is the same at 252 ms).
        // The earlier sheathe trace is not contradicted -- it predates SBF entirely.
        //
        // KEPT ANYWAY, FOR TWO REASONS THAT ARE NOT SENTIMENT. A gap closed by an adjacent change's
        // side effect is closed by luck, not by design, and every other swallowed-input path here
        // carries its own bound. And **A KILLMOVE HAS NEVER BEEN TRACED** -- it is precisely
        // the ending most likely to take the graph somewhere that does not return to ready, after a
        // cut that has already fired, which is this defect with a different cause.
        //
        // THE BOUND IS `readyWindowMs`, REUSED RATHER THAN INVENTED. `OnReadyLocked` already
        // refuses an `inRdy` that arrives later than that cap, on the grounds that it is no longer
        // the pass our cut provoked. An `inRdy` that has NOT arrived by the cap is dead for exactly
        // the same reason. One rule, two ways of noticing it.
        //
        // IT DROPS RATHER THAN FIRING, so the reasoning is here
        // rather than in a commit message. The question is whether the attack should come out OR
        // the press be handed back, and whether firing the attack anyway is even useful
        // mid-sheathe. It is not, and the answer was already in the record before this was built:
        //
        //   1. MEASURED, AND THE SIGNATURE IS WORSE THAN "REFUSED". On this fixture a
        //      weapon sheathe suppresses the attack whichever side owns the press -- its feature-OFF
        //      control produced no swing on the identical trial. **The graph ACCEPTS the event and
        //      then plays nothing**: `>>> CHAIN fired 'attackStart' (light)
        //      accepted=true` twice with no `MCO_AttackInitiate` behind either. So a fire here
        //      would not fail loudly -- it would write `accepted=true` into the trace and produce
        //      no attack, which is this file's own "a marker whose field contradicts the claim it
        //      supports is an assertion, not an instrument".
        //   2. THE WRITE WOULD RACE THE RESET. `OnReadyLocked` exists because the reset the cut
        //      provokes arrives just BEFORE `inRdy` -- ordering against that event,
        //      not against a clock, is what makes the index write survive. Firing on a clock is
        //      firing into the race that ordering was built to win.
        //   3. THE WORST CASES ARE NOT SYMMETRIC. Dropping costs the player one attack, inside a
        //      shout they themselves interrupted, and says so. Firing costs them a swing they did
        //      not ask for at an instant they do not control -- a stray attack -- with the
        //      combo index possibly clobbered behind it.
        //
        // WHAT IS FORBIDDEN IS THE SILENCE: a press that vanishes with nothing in the
        // trace. Nothing vanishes silently now.
        //
        // CALLED AFTER THE DECISION CHAIN, NOT ALONGSIDE THE OTHER TWO WATCHDOGS. Run first, this
        // would drop an armed chain on the very `inRdy` that `OnReadyLocked` was about to judge,
        // and that function's "`inRdy` came Xms after the cut" line would be unreachable on the one
        // event it exists for. Run last, each event gets its own chance first.
        //
        // **IT ONLY NARROWS THE RACE, IT DOES NOT WIN IT.** `IsNoise` filters two tags, so this
        // runs on nearly every animation event the player raises -- nine of them land inside the
        // 4.4 ms between the cut and `inRdy`. A genuinely LATE `inRdy` is therefore
        // almost always preceded by some other event, which trips this first; `OnReadyLocked`'s
        // late line stays mostly unreachable either way, and the late pass then arrives with
        // `awaitingReady` already false and says nothing at all.
        //
        // That costs a diagnostic and nothing else -- **the chain is dropped either way, at the
        // same cap, with a line in the trace** -- which is why the ordering is kept rather than
        // rebuilt around a flag. The trace wording below is written to survive it: it says no
        // ready pass has arrived WITHIN THE CAP, which is what this function actually knows, and
        // not that none is ever coming, which it cannot know.
        //
        // RIDING GRAPH EVENTS IS ENOUGH HERE, WHICH IS NOT TRUE OF EVERY WATCHDOG -- `PostWindowOpen`
        // is a thread precisely because a still character stops producing events. An
        // armed chain can only be ACTED on by an event as well: `inRdy` fires it, a new shout drops
        // it. So a character still enough to starve this check is one in whose game nothing can
        // consume the arm either, and the first event that could reaches this line first.
        void WatchdogDropChainLocked(const Settings& a_settings) {
            if (!g_state.awaitingReady) return;
            const auto since = ElapsedMs() - g_state.cutAtMs;
            if (since <= static_cast<double>(a_settings.readyWindowMs)) return;

            g_state.awaitingReady = false;
            SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN dropped: no `inRdy` within the cap -- {:.1f}ms since the "
                  "cut (cap {}ms). The graph has not returned to ready, so the pass this chain armed "
                  "against is too late to be ours whether or not one arrives later. The shout was cut "
                  "and the attack is owed, so this press is LOST -- said out loud rather than waited "
                  "on forever",
                  ElapsedMs(), since, a_settings.readyWindowMs);
        }

        // WHY A SHOUT ENDED, and why this is an enum rather than the reason string.
        //
        // `EndShoutLocked` took only a string, and the string was for the trace. It now has to
        // DECIDE with the reason -- whether the player's buffered press is handed back or dropped
        // -- and a decision keyed to a string is one that changes meaning silently the next time
        // someone improves the wording. The caller states which of the three this is; the string
        // survives alongside it and stays a label.
        enum class ShoutEnd {
            // The exhale ran out on its own -- the natural `shoutStop`. The player finished their
            // shout, whatever else was going on.
            kFinished,
            // THE GRAPH LEFT THE SHOUT STATE AND SAID SO ITSELF -- State Behavior Framework's
            // `SBF_ShoutStop`.
            //
            // It fires on every ending TRACED SO FAR -- a jump, a sheathe, a ragdoll, a hotbar
            // cast, and the natural end too. **A KILLMOVE HAS NEVER BEEN TRACED**
            // (the load order has never seen one fire), and it is precisely the case
            // where a state exit is most likely to be skipped, because a paired animation takes the
            // graph over. Since this engine no longer ends a shout any other way, an unannotated
            // killmove means the 8 s liveness cap. Do not promote that gap to a fact here: an
            // earlier version of this comment listed a killmove among the endings it "fires on",
            // which is the assertion-not-an-instrument error this file warns about twice.
            //
            // So it is not an "interruption" signal to be told apart from a normal one -- it is a
            // single statement that the shout is over, whatever ended it. The natural end still
            // reaches `kFinished` instead, because vanilla's own `shoutStop` precedes it by ~0.1 ms
            // and gets there first; this reason is what is left over.
            //
            // TWO DIFFERENT THINGS STILL ARRIVE HERE -- a shout cut short before its magic fired and
            // one cut short after -- and `EndShoutLocked` no longer separates them.
            // Both are the player's shout ending, so both hand the press back; only the trace still
            // tells them apart. This is the exit that used to be inferred from `inRdy`.
            // The engine stopped INFERRING the end from `inRdy` and started reading the event that
            // states it.
            kStateExit,
            // Nothing ended the shout; the engine gave up on it. The liveness cap, and the one
            // reason that is not the player finishing anything.
            kAbandoned,
        };

        void EndShoutLocked(const Settings& a_settings, ShoutEnd a_why, std::string_view a_reason,
                            Emit& a_emit);

        // A SHOUT THAT NEVER ENDS EATS EVERY ATTACK PRESS BEHIND IT.
        //
        // `shoutActive` is cleared only by an event: `shoutStop`, or `SBF_ShoutStop`. Both come from
        // the graph, and a shout that hangs in its inhale raises neither --
        // a replayed press produced `BeginCastVoice` and nothing after it. From that moment
        // `OnAttackButton` takes every press, the chain holds each one for a window that never
        // opens, and the player's attack button is dead until they reload.
        //
        // The replay bug that caused it is fixed in `ShoutInputHook`. This is here anyway, because
        // "the engine swallowed input and never gave it back" must not depend on one bug being
        // absent -- any hung shout, from any source, ends here instead.
        //
        // The pending press is DROPPED rather than fired, and that is a stated
        // classification rather than a side effect of `EndShoutLocked` clearing everything. It is
        // seconds old by now -- the cap is 8000 ms -- and an attack the player pressed for a chain
        // that never happened is not one they still want. This is the one exit that keeps
        // the old behaviour, kept on purpose -- and it is the ONLY exit that drops a
        // press at all. See the classification in `EndShoutLocked`.
        //
        // `a_emit` IS STRUCTURALLY DEAD HERE and that is a property worth stating rather than a
        // parameter worth removing: `kAbandoned` can never be classified player-facing, so this
        // path can never reach `TryFireChainLocked` and can never emit. It is carried only because
        // `EndShoutLocked` takes one. If a future reason ever makes an abandonment path fire, the
        // plumbing is already correct.
        void WatchdogEndShoutLocked(const Settings& a_settings, Emit& a_emit) {
            if (!g_state.shoutActive) return;
            const auto live = ElapsedMs() - g_state.shoutStartedAtMs;
            if (live <= static_cast<double>(a_settings.shoutLivenessCapMs)) return;

            // THIS LINE USED TO SAY "no `shoutStop` and no ready pass ever arrived", AND THAT
            // COULD BE FALSE. A ready pass may well have arrived -- it is now
            // deliberately ignored and traced as ignored, three lines above this one in the same
            // log. A diagnostic that contradicts the trace above it is an assertion rather than an
            // instrument, which is the error `g_shoutLiveForTrace` and `RecordNaturalTailLocked`
            // both already carry warnings about. It names the two events that DO end a shout.
            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT abandoned after {:.0f}ms -- neither `shoutStop` nor "
                  "`SBF_ShoutStop` ever arrived; handing input back", ElapsedMs(), live);
            // THE DIAGNOSIS, NOT A FALLBACK. On a load order with SBF the cap
            // firing is a rare unannotated ending on a session that has long seen `SBF_` traffic;
            // without SBF it is EVERY shout cut short, and this line is the only thing telling the
            // user why their attack button keeps going dead. `log::warn` rather than trace, so it
            // survives at `bTrace = 0` -- the one diagnostic a user who has not turned
            // tracing on still needs. Repeated on every occurrence deliberately: each firing is a
            // real dead-button incident, and the line belongs next to it in the log.
            if (!g_sbfEverSeen.load(std::memory_order_relaxed)) {
                log::warn(
                    "[ShoutMCO] a shout hung for {:.0f}ms and NO `SBF_` event has arrived since "
                    "the game started -- State Behavior Framework (Nexus 164546) looks ABSENT. It is a "
                    "REQUIREMENT: without it this mod cannot see an interrupted shout end, and "
                    "the attack button goes dead for {}ms after every shout cut short by a jump, "
                    "sheathe or knockdown. Install it and re-run Nemesis with `sbeef` ticked.",
                    live, a_settings.shoutLivenessCapMs);
            }
            EndShoutLocked(a_settings, ShoutEnd::kAbandoned, "liveness cap"sv, a_emit);
        }

        // The pre-lock combo sample, and whether phase A took one this event. `have == false`
        // means the sampling hint lagged a state change by one event; every consumer skips and
        // the next event makes it up -- the same tolerance the old racy reads had, without the
        // torn state.
        struct SampledCombo {
            ShoutChainEngine::ComboSnapshot v{};
            bool                            have = false;
        };

        // A SNAPSHOT IS NOT A MOMENT, IT IS THE LAST GOOD READING.
        //
        // `NoteQueuedShoutLocked` captures the combo position when the press is queued. That is
        // necessary (MCO resets the counter when the attack ends) and NOT sufficient: MCO also
        // ADVANCES the counter partway through the attack, so a press queued before that advance
        // captures the index of the attack currently playing instead of the next one.
        //
        // Measured from play, four consecutive power attacks: snapshots
        // at +418ms and +520ms after `MCO_PowerAttackInitiate` read 1, and snapshots at +537ms and
        // +931ms read the advanced value. The power attack's hit frame is +565ms and a light
        // attack's advance lands before +300ms, which is why this showed up on power attacks first
        // and why it read as intermittent: it depended entirely on how early the shout was pressed.
        //
        // So the reading is refreshed on every graph event for as long as the attack is live and
        // the press is still queued. Logged only when the value CHANGES -- this runs on every
        // event, and a line per event is not an instrument, it is noise.
        void RefreshQueuedResumeLocked(const SampledCombo& a_sample) {
            if (!g_queuedResume.valid || !g_mcoAttackLive) return;
            if (!ShoutInputHook::IsHoldingBehindAttackLocked()) return;
            // FROZEN THE MOMENT WE CANCEL. Everything after that point is MCO tearing the attack
            // down, and its reset to 1 arrives BEFORE the attack-end events -- so without this the
            // refresh captures the reset and destroys the advanced value it exists to preserve.
            // Observed across five trials: "power 1->2" (correct) followed 18ms later
            // by "power 2->1" (the reset), and the engine then resumed the combo at 1 every time.
            //
            // Frozen through a NATURAL teardown for the same reason: the end markers
            // used to return before this function was reached; now that they only mark state, the
            // freeze has to be explicit. CAVEAT: this freeze assumes MCO's reset lands AT OR
            // AFTER the end markers on a
            // natural end, and the only measured teardown -- the cancel's -- had the reset
            // BEFORE its markers. If a natural end resets first and any sampled event lands in
            // between, the refresh captures the reset and this freeze then preserves the wrong
            // value. Unmeasurable statically; the trace signature to check
            // is `snapshot advanced: ... N->1` before the end markers on a non-cancelled attack,
            // and any fix must not blanket-reject writes of 1, which a legitimate combo wrap
            // also produces.
            if (g_cancelSent || g_mcoAttackEnding) return;
            if (!a_sample.have) return;

            const auto& s = a_sample.v;
            if (s.nextAttack == g_queuedResume.nextAttack && s.nextPowerAttack == g_queuedResume.nextPowerAttack &&
                s.currentAttack == g_queuedResume.currentAttack &&
                s.currentPowerAttack == g_queuedResume.currentPowerAttack) {
                return;
            }

            SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO snapshot advanced: attack {}->{} power {}->{}", ElapsedMs(),
                  g_queuedResume.nextAttack, s.nextAttack, g_queuedResume.nextPowerAttack, s.nextPowerAttack);
            g_queuedResume.nextAttack = s.nextAttack;
            g_queuedResume.nextPowerAttack = s.nextPowerAttack;
            g_queuedResume.currentAttack = s.currentAttack;
            g_queuedResume.currentPowerAttack = s.currentPowerAttack;
        }

        // SKIP THE RECOVERY RATHER THAN WAIT IT OUT.
        //
        // A queued shout waits for the attack to be finished with the character. Most of that wait
        // is recovery -- animation the player has already got everything out of. Measured
        // on a rapier light attack: hits at +481ms and +925ms, `MCO_WinOpen` at +974ms,
        // and the attack not actually over until +1607ms. Everything after the window is tail.
        //
        // `attackStop`, notified INTO the graph, ends it early: sent at +520ms the attack was over
        // at +615ms against a natural +1610ms, straight through `MCO_AttackExitNotify` /
        // `attackStop` / `inRdy`. `MCO_AttackExitNotify` notified the same way does nothing.
        //
        // THE CANCEL POINT IS THE HIT, AND ONLY THE HIT. One gate. Both were driven on one
        // binary in one session, 10/10
        // each, and `hit` won on responsiveness (39-448 ms press-to-shout against 408-743) once the
        // combo criterion stopped discriminating between them.
        //
        // THREE ALTERNATIVES ARE DISPROVED. They are kept here because they are what stops the next
        // agent retrying one of them:
        //
        //   - A BARE RELEASE at the hit, with no cancel at all. This is v1.0.0's literal behaviour
        //     and it does not work; re-driven on the fixed plumbing, with the shout
        //     cooldown zeroed and the main-thread spin gone, so neither confound explains it. The
        //     shout STARTS -- `BeginCastVoice` ~11 ms after the release -- and then nothing. No
        //     `Voice_SpellFire_Event`, no `shoutStop`, until the liveness cap hands input back
        //     8.9 s later. The attack is still playing and the graph will not carry the inhale
        //     through to an exhale while it is. The hit frame is the right MOMENT; a bare release
        //     is the wrong ACTION.
        //   - `MCO_WinOpen` INSTEAD OF the hit. It fires BEFORE the hit -- +374 ms against a
        //     ~+550 ms hit frame on the power attack -- so cancelling there
        //     cut the swing 3 trials out of 3. It means "you may queue the next attack", never
        //     "this one landed".
        //   - WAITING FOR MCO'S COMBO ADVANCE on top of the hit -- the deleted `kReady` gate. It
        //     bought a more reliable resume index and cost 667-1361 ms of wait, plus ~31% of
        //     presses on which its conditions never came true at all and the shout rode the attack
        //     to its natural end. Intermittent behaviour is not acceptable.
        //     The measurement that made it look necessary survives in CONTEXT.md.
        //
        //     PARTLY REVIVED, AND THE DIFFERENCE IS THE WINDOW. This
        //     paragraph is left standing because everything in it is true of `kReady`, and a reader
        //     who finds `ArmAdvanceWaitLocked` above and this line below deserves to be told which
        //     one the shipped code is. `kReady` wanted THREE conditions and the expensive one was
        //     `MCO_WinOpen` -- the median cancel sat on it at +1174 ms, and
        //     that annotation is missing from 18.2% of power clips. The surviving wait is on the
        //     advance ALONE, never on the window; only when the advance has not already landed by
        //     the hit, which is the first power attack of a chain and nothing else;
        //     and under a hard bound, so it cannot ride an attack to its natural end the way the
        //     31% did. Set `iPowerAdvanceWaitMs = 0` and this paragraph describes the binary again.
        //
        // A KNOWN COST OF CANCELLING AT THE FIRST HIT, recorded rather than argued away: an attack
        // that lands TWICE loses its second hit. The measured rapier light attack hits at +481 ms
        // and again at +925 ms, and the cancel fires on the first. Waiting for the second is not
        // available -- nothing in the graph says how many a moveset's attack has, and waiting for a
        // hit that never comes is the hang above. This was on the table when the owner ruled and it
        // is the price of the responsiveness they ruled for.
        //
        // WHY CANCEL AT ALL RATHER THAN WAIT THE RECOVERY OUT. Most of the wait is recovery the
        // player has already got everything out of: measured on the rapier light attack,
        // hits at +481 ms and +925 ms, `MCO_WinOpen` at +974 ms, and the attack not actually over
        // until +1607 ms. `attackStop` notified INTO the graph ends it early -- sent at +520 ms the
        // attack was over at +615 ms against a natural +1610 ms, straight through
        // `MCO_AttackExitNotify` / `attackStop` / `inRdy`. (`MCO_AttackExitNotify` notified the
        // same way does nothing.)
        // Takes neither the settings nor the combo sample any more: the settings only ever chose
        // between the gates, and the sample only ever answered the deleted gate's advance check.
        void TryCancelRecoveryLocked(Emit& a_emit) {
            if (g_cancelSent || !g_mcoAttackLive || g_mcoAttackEnding) return;
            // THIS GUARD IS WHAT MAKES THE WAIT REAL. `TrackMcoAttackLocked` calls
            // this from its fall-through as well as from the hit frame, on EVERY event that is not
            // an initiate, a hit, a teardown marker or `inRdy` -- and by then `g_mcoSwingLanded` is
            // already true. Without this, the first ordinary event after the hit (a `Pie` payload,
            // a `SCAR_UpdateDummy`) would cut inside the deferral and the wait would buy nothing at
            // all. The poll clears the flag before it calls here, so the cut it asks for still goes
            // out; nothing else's does.
            if (g_advanceWaitArmed) return;
            // Non-negotiable, and the whole of the gate: the hit is what the player is complaining
            // about losing. Nothing here may key off MCO's window, which fires ~180 ms
            // BEFORE the hit on a power attack.
            if (!g_mcoSwingLanded) return;
            if (!ShoutInputHook::IsHoldingBehindAttackLocked()) return;

            g_cancelSent = true;
            // Combo is frozen from here (see the refresh above). Do not clear `g_comboSampleWanted`
            // — `IsAttacking` still needs sampling through teardown until the shout
            // actually releases.
            SHOUTMCO_TRACE("[{:10.2f}] >>> CANCEL recovery for a queued shout -- hit landed", ElapsedMs());
            a_emit.cancelRecovery = true;
            // Nothing is released here. The cancel produces teardown; the shout waits until
            // `IsAttacking` falls, not until the cancel's own `inRdy`.
        }

        // MCO's attack, watched so a shout pressed during one knows when the attack is finished
        // with the character.
        //
        // Vanilla lets that shout start at once, and starting it tears the attack down ~3ms later
        // later -- which mid-swing is the "power attack cancels and I run forward" report.
        //
        // THE GATE IS THE ATTACK'S END, NOT `HitFrame` AND NOT MCO'S WINDOW. Both earlier gates
        // were built and both were measured wrong, in opposite directions:
        //
        //   - `MCO_WinOpen` (tried first) fires BEFORE the hit -- +374ms against a
        //     ~+550ms `HitFrame` on the measured power attack. Three trials produced a clean shout
        //     and no swing at all. It means "you may queue the next attack", never "this one
        //     landed".
        //   - `HitFrame` banks the hit but is not the end of anything. Measured
        //     on a rapier light attack: `HitFrame` at +481ms, a SECOND `HitFrame` at
        //     +914ms, and the attack not actually done until ~+1400ms. A shout released at the hit
        //     starts its inhale on top of a still-running attack, and the attack's own teardown
        //     takes the graph back before the exhale -- so the shout never fires at all.
        //
        // So the queue holds until the graph confirms READY. The end is an EVENT, not
        // a duration: the wait is exactly as long as the animation has left, on any weapon, in any
        // pack, with nothing timed and nothing assumed. What CANCELLING adds is not a second
        // release point but an earlier `inRdy` -- the release still rides the graph's own
        // confirmation, the recovery in front of it is just skipped. Cancelling at the hit is the
        // default and the only behaviour, because queuing out the full recovery cost 667-1361 ms
        // and read as sluggish.
        // Remember where the player is, so a producer that arms too late to read it can
        // still preserve it. See `RollingCombo` for why the call site is the discriminator.
        //
        // Called only from attack-time events. Adding a call from a teardown event would poison it
        // with the reset value and there would be nothing in the reading to reveal that, which is
        // the whole hazard -- so if a new call site is ever added, justify it here.
        void RecordRollingComboLocked(std::string_view a_site, const SampledCombo& a_sample) {
            if (!a_sample.have) return;

            g_rollingCombo.valid = true;
            g_rollingCombo.takenAtMs = ElapsedMs();
            g_rollingCombo.nextAttack = a_sample.v.nextAttack;
            g_rollingCombo.nextPowerAttack = a_sample.v.nextPowerAttack;
            g_rollingCombo.currentAttack = a_sample.v.currentAttack;
            g_rollingCombo.currentPowerAttack = a_sample.v.currentPowerAttack;

            // The line names its reader, because a capture that looks unread next to the
            // `COMBO carried` line that consumes it is a trace that contradicts itself.
            SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO position remembered at {} -- next={} power={} "
                  "(read by a driver cast's arm)",
                  ElapsedMs(), a_site, g_rollingCombo.nextAttack, g_rollingCombo.nextPowerAttack);
        }

        // Is there a remembered position young enough to describe this fight?
        //
        // Called from exactly one place: the driver-cast arm in `Observe`'s
        // `isShoutStateEntry` branch. Still kept beside the writer, so the age rule lives with the
        // thing it governs rather than at the call site.
        //
        // `false` COVERS TWO DIFFERENT SITUATIONS and the caller must tell them apart in its trace:
        // nothing was ever captured, and what was captured is too old. Both fall back to the live
        // read, which is correct for both.
        [[nodiscard]] bool RollingComboUsableLocked(double& a_ageMsOut) {
            if (!g_rollingCombo.valid) return false;
            a_ageMsOut = ElapsedMs() - g_rollingCombo.takenAtMs;
            return a_ageMsOut <= kRollingComboMaxAgeMs;
        }

        // How often the deferred cut looks for MCO's advance.
        //
        // Not a tuning knob and deliberately not a setting: it is a sampling rate, and the only
        // thing it trades is how much of the ~167 ms wait is spent past the advance. One frame at
        // 60 fps costs at most a frame of lateness and posts at most `powerAdvanceWaitMs / 16`
        // tasks -- about 22 at the shipped bound, and only on a power attack with a shout already
        // queued behind it.
        //
        // POLLED RATHER THAN RIDDEN ON GRAPH EVENTS, for the reason `PostWindowOpen` documents at
        // length: a still character stops producing graph events, and
        // the deferral must end on its own whatever the graph does. Polling a variable also side-
        // steps the PIE payload entirely -- the advance arrives as `@SGVI|MCO_nextpowerattack|N`,
        // the engine only ever LOGGED that string, and matching on it would mean parsing a payload
        // that carries a light-attack advance at the same timestamp (the clip annotation
        // shows both at 1.166667).
        constexpr double kAdvancePollIntervalMs = 16.0;

        // Should the cut wait at the hit frame for MCO's own power-combo advance?
        //
        // Every refusal below is a case where waiting buys nothing, and the order is cheapest
        // first. The last two mirror `TryCancelRecoveryLocked`'s own preconditions deliberately: a
        // deferral must never outlive a cut that was never going to happen.
        [[nodiscard]] bool ArmAdvanceWaitLocked(const Settings& a_settings, const SampledCombo& a_sample,
                                                Emit& a_emit) {
            // ALREADY WAITING, so the poll owns the cut and this must not hand it back. A rapier
            // light attack raises a second `HitFrame` at +914 ms and a power clip can do the same;
            // returning false here would let that second hit fall through and cut in the middle of
            // the wait, which is the exact bug the wait exists to fix.
            if (g_advanceWaitArmed) return true;

            if (a_settings.powerAdvanceWaitMs <= 0) return false;

            // Light attacks advance ~+51 ms into the swing against a hit at +481 ms,
            // so their advance is already banked by the time this is asked and waiting would be
            // pure latency.
            if (!g_mcoPowerAttack) return false;

            // No baseline means no comparison. Refusing here is what keeps the engine from
            // guessing: without a reading at the initiate there is no honest way to tell an
            // advance from the value that was always there.
            if (g_powerAtInitiate <= 0 || !a_sample.have) return false;

            // THE COMBOED CASE, AND IT IS THE COMMON ONE. A comboed power
            // attack advancing at ~+51 ms, long before its hit -- so by here the value has already
            // moved and the cut goes out at the hit exactly as it does today. This is why
            // `pattack2 -> shout -> pattack3` keeps its current timing to the
            // millisecond.
            if (a_sample.v.nextPowerAttack != g_powerAtInitiate) return false;

            if (g_cancelSent || !g_mcoAttackLive || g_mcoAttackEnding) return false;
            if (!ShoutInputHook::IsHoldingBehindAttackLocked()) return false;

            g_advanceWaitArmed = true;
            g_advanceWaitArmedAtMs = ElapsedMs();
            a_emit.scheduleAdvancePoll = true;
            a_emit.scheduleAdvancePollInMs = kAdvancePollIntervalMs;
            a_emit.scheduleAdvanceGeneration = g_attackGeneration.load(std::memory_order_relaxed);

            SHOUTMCO_TRACE("[{:10.2f}] >>> ADVANCE wait armed at the hit -- MCO_nextpowerattack still {} "
                  "after {}ms, holding the cut up to {}ms", ElapsedMs(), g_powerAtInitiate,
                  a_settings.powerAdvanceWaitMs, a_settings.powerAdvanceWaitMs);
            return true;
        }

        // One tick of the deferred cut: has the advance landed, has the bound expired,
        // or is it worth another look?
        //
        // The live reading is taken by the caller BEFORE the lock, because it is a game call and
        // phase A is where those belong (EngineLock.h). Everything decided here is decided on that
        // one reading.
        void OnAdvancePollLocked(const SampledCombo& a_live, const Settings& a_settings, Emit& a_emit) {
            // Cleared by every attack boundary, so this covers the teardown, `inRdy`, a new
            // initiate and a game load in one test. A poll that finds it false has been outlived
            // by its attack and must do nothing at all -- least of all cut.
            if (!g_advanceWaitArmed) return;

            const auto waited = ElapsedMs() - g_advanceWaitArmedAtMs;
            const bool advanced = a_live.have && a_live.v.nextPowerAttack != g_powerAtInitiate;
            const bool expired = waited >= static_cast<double>(a_settings.powerAdvanceWaitMs);

            if (!advanced && !expired) {
                a_emit.scheduleAdvancePoll = true;
                a_emit.scheduleAdvancePollInMs = kAdvancePollIntervalMs;
                a_emit.scheduleAdvanceGeneration = g_attackGeneration.load(std::memory_order_relaxed);
                return;
            }

            g_advanceWaitArmed = false;

            if (advanced) {
                // Fold it into the snapshot the shout is about to consume, on the same path every
                // other reading takes. This is the entire point of the wait: the refresh is frozen
                // by `g_cancelSent`, so it has to happen BEFORE the cut or not at all.
                RefreshQueuedResumeLocked(a_live);
                SHOUTMCO_TRACE("[{:10.2f}] >>> ADVANCE landed {}->{} after {:.1f}ms -- cutting now",
                      ElapsedMs(), g_powerAtInitiate, a_live.v.nextPowerAttack, waited);
            } else {
                // THE BOUND, AND IT IS A REAL OUTCOME RATHER THAN AN ERROR. A clip whose power
                // attack never advances is cut exactly where the hit-gate cuts it, one bound later.
                // The player keeps the index they had, which is the behaviour this wait exists to
                // fix -- so a run that logs this line has NOT been fixed and the log is where
                // that shows.
                SHOUTMCO_TRACE("[{:10.2f}] >>> ADVANCE never came within {}ms -- cutting anyway, combo "
                      "stays at {}", ElapsedMs(), a_settings.powerAdvanceWaitMs, g_powerAtInitiate);
            }

            // Rechecked in full, under this lock, on this reading -- the press may have been
            // released, replaced or abandoned during the wait, and `TryCancelRecoveryLocked` is
            // the one place that decides whether a cut is still owed.
            TryCancelRecoveryLocked(a_emit);
        }

        // Hold until `IsAttacking` falls. `inRdy` after cancel-at-hit still has
        // `IsAttacking` true on 1H and 2H; releasing there replays into a silent refusal.
        // The arriving tag is the reason string, not a family discriminator.
        void ApplyAttackQueuedReleaseLocked(std::string_view a_tag, bool a_isAttacking,
                                            const Settings& a_settings, Emit& a_emit) {
            const bool vanillaHold = ShoutInputHook::IsVanillaHoldingBehindAttackLocked();
            const bool capElapsed =
                vanillaHold && (ElapsedMs() - ShoutInputHook::HeldSinceMsLocked() >
                                static_cast<double>(a_settings.shoutWaitCapMs));
            const auto action = DecideAttackQueuedRelease({
                .holdingBehindAttack = ShoutInputHook::IsHoldingBehindAttackLocked(),
                .isAttacking = a_isAttacking,
                .capElapsed = capElapsed,
            });
            if (action == AttackQueuedAction::kHold) {
                return;
            }
            g_mcoAttackLive = false;
            g_mcoAttackEnding = false;
            g_mcoSwingLanded = false;
            g_comboSampleWanted.store(false, std::memory_order_relaxed);
            g_advanceWaitArmed = false;
            a_emit.releaseHeld = true;
            a_emit.releaseReason = action == AttackQueuedAction::kCapRelease
                                       ? "cap -- no shout-admissible ready"sv
                                       : a_tag;
        }

        void TrackMcoAttackLocked(std::string_view a_tag, const SampledCombo& a_sample,
                                  const Settings& a_settings, Emit& a_emit, bool a_isAttacking) {
            if (a_tag == "MCO_AttackInitiate"sv || a_tag == "MCO_PowerAttackInitiate"sv) {
                g_mcoAttackLive = true;
                g_mcoAttackEnding = false;
                g_mcoSwingLanded = false;
                g_cancelSent = false;
                // The kind, the baseline, and a fresh generation, all before the early
                // return. The baseline is deliberately read from the SAMPLE rather than live: it is
                // the same reading every other decision on this event uses, and a second live read
                // here could catch an advance that landed between the two.
                //
                // ARMED FROM THE POWER TAG, NOT FROM `MCO_IsPowerAttacking`. The graph flag was
                // observed false on an `MCO_AttackInitiate` that a power key had produced, so
                // the tag is the honest discriminator and the
                // flag is not.
                g_mcoPowerAttack = a_tag == "MCO_PowerAttackInitiate"sv;
                g_powerAtInitiate = a_sample.have ? a_sample.v.nextPowerAttack : 0;
                g_advanceWaitArmed = false;
                g_attackGeneration.fetch_add(1, std::memory_order_relaxed);
                // The attack is BEGINNING, which is the moment
                // `MCO_nextattack` already holds the correct next index, wrap included. Captured
                // before the early return, because everything below this line belongs to attacks
                // that are ending.
                RecordRollingComboLocked(a_tag, a_sample);
                return;
            }

            // The hit is banked, and it is also the cancel point -- there is no
            // longer a later gate that might want it held.
            //
            // The refresh runs on this tag too: a power
            // attack's combo advance lands around the hit frame, and the old early return meant
            // an advance arriving exactly here was only captured one event later -- while the
            // comment below promised the opposite.
            if (a_tag == "HitFrame"sv) {
                g_mcoSwingLanded = true;
                // Refreshed BEFORE the cancel, so the snapshot the shout is about to consume is
                // the freshest reading available -- a power attack's combo advance lands around
                // here.
                RefreshQueuedResumeLocked(a_sample);
                // and for the same reason: a power attack's advance lands around the
                // hit frame, so an initiate-only capture would carry a stale index for the whole of
                // a power swing. Still an attack-time site, so the discriminator holds.
                RecordRollingComboLocked(a_tag, a_sample);

                // THE CANCEL POINT. Reached from inside this branch because the hit frame returns
                // early; before the hit-only gate the other gate got its chance on later events instead,
                // which is why the call below this branch also exists and still has to.
                //
                // Nothing is released here. The cancel produces teardown events; the release waits
                // for `IsAttacking` to fall. A bare release at this exact moment is
                // what hung the shout when it was tried (see `TryCancelRecoveryLocked`).
                //
                // The second `HitFrame` a rapier light attack raises at +914 ms cannot double-fire
                // it either: `g_cancelSent` is already set by then.
                //
                // ONE THING IN FRONT OF THE CUT, and only for the first power attack
                // of a chain. MCO's advance for THAT attack lands after this hit -- ~+165 ms by
                // CONTEXT.md, 166.7 ms by the clip annotation -- so cutting here ends the attack
                // before the payload that sets the next index ever plays, and the chain replays the
                // same power attack. `ArmAdvanceWaitLocked` returns true only when waiting can
                // actually change that; everything else falls straight through to the cut below,
                // unchanged and untimed.
                //
                // The second `HitFrame` cannot re-arm it either: `g_advanceWaitArmed` is still set
                // on the first one's behalf and `g_cancelSent` on the other side of it.
                if (ArmAdvanceWaitLocked(a_settings, a_sample, a_emit)) return;

                TryCancelRecoveryLocked(a_emit);
                return;
            }

            // `MCO_WinOpen` / `MCO_PowerWinOpen` / `MCO_WinClose` / `MCO_PowerWinClose` were
            // tracked here into `g_mcoWindowOpen` for the deleted gate. They fall
            // through to the cancel attempt below now, exactly as they did before -- this block
            // never returned -- so removing it changes no control flow. The tags still reach the
            // trace through `IsInteresting`.

            // the teardown began -- STATE ONLY, no release. These used to be treated
            // as equivalent to `inRdy` and released the queued press on whichever arrived first;
            // the trace showed the release riding `MCO_AttackExitNotify`, the EARLIEST of them,
            // while MCO was still resetting the graph -- and a shout offered to a graph that is
            // not ready is silently refused. `g_mcoAttackLive` deliberately
            // stays true so a press made mid-teardown still queues.
            if (a_tag == "MCO_AttackExitNotify"sv || a_tag == "attackStop"sv) {
                g_mcoAttackEnding = true;
                g_mcoSwingLanded = false;
                // The attack this deferral belongs to is ending, whether we asked for it
                // or not -- our own cut reaches here too, and so does a stagger, an interrupt or a
                // killmove that takes the attack away mid-wait. There is nothing left to cut and
                // nothing left to preserve: MCO's reset to 1 arrives with this teardown, so a poll
                // draining after it would see the reset and read it as an advance.
                //
                // Traced only when a wait was actually up, because this branch runs on every
                // attack that ever ends.
                if (g_advanceWaitArmed) {
                    g_advanceWaitArmed = false;
                    SHOUTMCO_TRACE("[{:10.2f}] >>> ADVANCE wait dropped -- the attack is ending ('{}')",
                          ElapsedMs(), a_tag);
                }
                // The *later* `attackStop` is the 1H/2H admit event -- `IsAttacking`
                // is false there. The cancel-produced `attackStop` still has it true, so this
                // holds. Do not swallow that later stop before the policy sees it.
                ApplyAttackQueuedReleaseLocked(a_tag, a_isAttacking, a_settings, a_emit);
                return;
            }

            // MCO ready. Shout→shout holds are armed at `shoutStop` and paced out separately;
            // same-frame `inRdy` here is the shout's exit bundle and must not deliver the next
            // cast. Attack-queued holds do NOT release here: cancel `inRdy` still has
            // `IsAttacking` true.
            if (a_tag == "inRdy"sv) {
                g_mcoAttackLive = false;
                g_mcoAttackEnding = false;
                g_mcoSwingLanded = false;
                // Dropping the wait here rather than letting it expire keeps a
                // late cut from racing the queued shout. `g_comboSampleWanted` stays set until
                // the shout actually releases, so phase A keeps sampling `IsAttacking`.
                g_advanceWaitArmed = false;
                ApplyAttackQueuedReleaseLocked(a_tag, a_isAttacking, a_settings, a_emit);
                return;
            }

            // Before the cancel, so the reading is refreshed one last time on the very event that
            // triggers it -- the cancel point is at or after the hit frame, which is where MCO's
            // advance lands on a power attack.
            RefreshQueuedResumeLocked(a_sample);
            TryCancelRecoveryLocked(a_emit);

            // Backstop for attack-queued holds: force a release rather than
            // strand the button. For a shout→shout hold armed at `shoutStop`, the same cap
            // ABANDONS -- the watchdog must not force a cast. Age from the ARM, not
            // from the last replacement press, or a tap-tap resets the clock forever.
            //
            // THE PREDICATE IS THE FIX. This asked
            // `IsHoldingBehindAttackLocked()`, which also answers for a DRIVER's
            // pending intent -- while the age below reads `HeldSinceMsLocked()`, which describes
            // the vanilla press and nothing else. With a driver in the slot there is no press, so
            // that reads 0.0, `waited` became the whole session's elapsed time, and the cap blew on
            // the FIRST graph event after the intent was taken. Observed from Spell
            // Hotbar 2's side: released on `preHitFrame` 88 ms after the defer, reported as
            // `SHOUTMCO_CAUSE_READY`, and refused by the driver's graph because `IsAttacking` was
            // still 1 -- the cast lost, roughly a second before the `inRdy` the cause names.
            //
            // A driver intent must not reach a cap that FORCES a release in any case: it has no
            // button to strand, and ADR-0008 gives it a bound that abandons instead --
            // `CastIntentApi::CheckWatchdog`, run at the top of every `Observe` pass on this same
            // `shoutWaitCapMs`. Two caps over one intent with opposite outcomes is the duplicated
            // timing policy the boundary exists to prevent.
            // Only a vanilla press is forced
            // through. Admit (`IsAttacking` falling) is NOT decided on window tags -- a trial
            // released
            // on `MCO_PowerWinClose` 74 ms after queue because cancel had already dropped the
            // sample hint. Cap only.
            if (ShoutInputHook::IsVanillaHoldingBehindAttackLocked()) {
                const auto waited = ElapsedMs() - ShoutInputHook::HeldSinceMsLocked();
                if (waited > static_cast<double>(a_settings.shoutWaitCapMs)) {
                    ApplyAttackQueuedReleaseLocked(a_tag, a_isAttacking, a_settings, a_emit);
                }
            }
            if (g_releaseShoutChainOnReady &&
                       ShoutInputHook::IsHoldingBehindShoutLocked()) {
                const auto waited = ElapsedMs() - g_releaseShoutChainArmedAtMs;
                if (waited > static_cast<double>(a_settings.shoutWaitCapMs)) {
                    g_releaseShoutChainOnReady = false;
                    // Slot-wide, though this is the one of its three sites a driver
                    // intent is not expected to reach: `g_takenAtMs` is stamped when the intent is
                    // taken and `g_releaseShoutChainArmedAtMs` strictly later, so `CheckWatchdog`
                    // running on the same `shoutWaitCapMs` always expires first. That argument
                    // rests on the two caps being ONE number. Correct here costs a call that does
                    // nothing today.
                    ShoutInputHook::AbandonSlotLocked(
                        "cap -- no ready after shoutStop arm"sv);
                }
            }
        }

        // Step three: write the index back and fire the attack, planned under the lock and
        // executed as one deferred task so nothing can slot between the write and the event.
        void ResumeAndAttackLocked(const Settings& a_settings, AttackKind a_kind, Emit& a_emit) {
            const bool power = a_kind == AttackKind::kPower;
            a_emit.fire = true;
            a_emit.fireKind = a_kind;
            a_emit.fireVar = power ? "MCO_nextpowerattack" : "MCO_nextattack";
            a_emit.fireIndex = power ? g_state.resumePowerAttack : g_state.resumeAttack;
            a_emit.fireEvent = power ? a_settings.powerAttackEvent : a_settings.attackEvent;
            // EITHER fact arms the sprint entry: the shout was a Whirlwind Sprint
            // (a Whirlwind IS a sprint, no prior sprint required), or
            // the player was sprinting when the key went down (any shout, kept because it costs
            // nothing and the press-edge capture now makes it work). The still-moving gate at the
            // fire edge applies to both, so neither can produce a lunge from a standstill.
            a_emit.fireFromSprint = g_state.whirlwindThisShout || g_state.sprintingAtBegin;
            a_emit.fireResume =
                a_settings.resumeMode != Settings::ResumeMode::kOff && a_emit.fireIndex > 0;
        }

        // A press handed back before its queued shout ever began has no shout-owned resume state.
        // Reuse the normal deferred attack emission, but deliberately omit the graph-variable
        // write so a prior shout's combo index cannot leak into this independent attack.
        void AttackWithoutResumeLocked(const Settings& a_settings, AttackKind a_kind, Emit& a_emit) {
            a_emit.fire = true;
            a_emit.fireKind = a_kind;
            a_emit.fireEvent =
                a_kind == AttackKind::kPower ? a_settings.powerAttackEvent : a_settings.attackEvent;
            // Deliberately NOT `sprintingAtBegin`: this path fires for a press whose queued shout
            // never began, so there is no shout-owned sprint fact to consult.
            a_emit.fireResume = false;
        }

        // Step one: cut the shout. `Voice_SpellFire_Event` has already fired by the time any
        // window opens, so the shout still delivers its magic.
        void FireChainLocked(const Settings& a_settings, AttackKind a_kind, Emit& a_emit) {
            g_state.shoutActive = false;  // the cut ends the shout; no natural shoutStop follows
            // ...and because no natural `shoutStop` follows, `Observe` never gets the event that
            // would clear the trace's liveness flag. Without this line it stays true after every
            // successful chain, and the next attack made nowhere near a shout logs `shout LIVE`.
            g_shoutLiveForTrace = false;
            g_state.windowOpen = false;
            g_state.pressPending = false;
            g_state.pressResolved = false;
            g_state.pendingKind = a_kind;
            g_state.awaitingReady = true;
            g_state.cutAtMs = ElapsedMs();
            // Poisoning guard, set at the one place that cuts. The `shoutStop` this
            // provokes is OURS, so the interval it would close is "however long the player waited"
            // rather than the clip's length, and recording it would make the cache eat itself.
            g_state.cutSent = true;

            // THE CUT IS AN END, AND IT IS THE ONE THAT DOES NOT GO THROUGH
            // `EndShoutLocked`. This function clears `shoutActive` itself and returns, so the
            // clear above never runs for a chained shout; without this line the root would survive
            // into the MCO attack the chain just fired and hold through the whole swing. The order
            // in `ExecuteEmits` puts the unlock ahead of the cut for the same reason -- the player
            // gets steering back no later than the attack that replaces the shout.
            ClearRootLockLocked(a_emit);

            // A shout→shout cast intent queued during this shout's window cannot ride
            // the chain's later `inRdy` -- that would fire a shout after an MCO attack the player
            // chose instead. Abandon; do not force execution.
            //
            // THE PREDICATE AND THE ACTION NOW AGREE. The test has answered for a
            // DRIVER's pending intent; the action was vanilla-only, so a driver
            // whose cast was deferred behind this shout kept waiting for a `shoutStop` this cut has
            // just made impossible, until the watchdog retired it up to `shoutWaitCapMs` later and
            // named itself as the reason. This site is event-driven and has no ordering that puts
            // the watchdog first -- unlike the cap above, it can fire well inside the cap.
            if (ShoutInputHook::IsHoldingBehindShoutLocked()) {
                g_releaseShoutChainOnReady = false;
                ShoutInputHook::AbandonSlotLocked("shout cut for MCO chain"sv);
            }

            a_emit.cut = true;
            a_emit.cutEvent = a_settings.cutEvent;
        }

        // THE SAME CUT, FOR A QUEUED SHOUT INSTEAD OF A QUEUED ATTACK.
        //
        // shout→shout was the only one of the three chain directions that cut nothing. attack→shout
        // cuts the attack at `HitFrame`; shout→attack cuts the shout in `FireChainLocked` directly
        // above. shout→shout waited out the whole exhale and then added the 300 ms floor on top,
        // measured at 310 ms of dead air and a 1.44 s cadence against a 950 ms clip.
        //
        // WHY THE 300 ms STAYS. It is not the fat. `EndShoutLocked` records the measurement: a real
        // second shout is accepted ~266 ms after `shoutStop`, and 150 ms synthetic taps completed
        // with no `BeginCastVoice` at all. The recoverable time is the un-cut exhale AHEAD of the
        // floor, which is what this takes. Cutting the floor instead reintroduces a silently
        // refused chain.
        //
        // WHY THIS ARMS THE RELEASE ITSELF rather than letting the provoked `shoutStop` do it.
        // `EndShoutLocked` returns early on `!g_state.shoutActive`, and the cut clears that flag
        // here -- so the `shoutStop` we provoke never reaches the arm at its natural site. Arming
        // in the same locked breath as the cut is the only ordering that cannot drop the intent.
        //
        // WHY IT WILL NOT FIRE BEFORE THE MAGIC IS OUT. `spellFiredAtMs` gates it:
        // `Voice_SpellFire_Event` has already fired by the time any window opens, so the shout
        // still delivers its effect. The gate makes that a checked precondition rather than an
        // assumption inherited from the window's timing.
        void TryCutForShoutChainLocked(const Settings& a_settings, Emit& a_emit) {
            if (!g_state.shoutActive || !g_state.windowOpen || g_state.cutSent) return;
            if (g_state.spellFiredAtMs <= 0.0) return;
            if (!ShoutInputHook::IsHoldingBehindShoutLocked()) return;

            // Same teardown as `FireChainLocked`, and for the same reasons recorded there: the cut
            // ends the shout so no natural `shoutStop` follows, the trace liveness flag has to be
            // cleared by hand, and the poisoning guard must mark this `shoutStop` as OURS so the
            // window cache does not record "however long the player waited" as a clip length.
            g_state.shoutActive = false;
            g_shoutLiveForTrace = false;
            g_state.windowOpen = false;
            g_state.cutAtMs = ElapsedMs();
            g_state.cutSent = true;

            // The player gets steering back no later than the
            // shout that replaces this one.
            ClearRootLockLocked(a_emit);

            g_releaseShoutChainOnReady = true;
            g_releaseShoutChainArmedAtMs = ElapsedMs();
            a_emit.scheduleShoutChainRelease = true;
            a_emit.scheduleShoutChainReleaseInMs = 300.0;
            a_emit.scheduleShoutChainReleaseReason = "chain cut+delay"sv;

            a_emit.cut = true;
            a_emit.cutEvent = a_settings.cutEvent;
            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT→SHOUT cut the exhale -- release in 300ms",
                  ElapsedMs());
        }

        // The chain needs three things at once: a press, a decision about what kind of press it
        // was, and an open window. They can arrive in any order, so every one of them ends here
        // rather than each firing the chain itself.
        void TryFireChainLocked(const Settings& a_settings, Emit& a_emit) {
            if (!g_state.pressPending || !g_state.pressResolved) return;
            // A resolved press made during the queued-shout wait belongs to the shout
            // that has not started yet. `BeginShoutLocked` clears this flag while carrying the
            // press across its reset; only then may the window or post-shout logic decide it.
            if (g_state.waitingForShoutStart) return;

            const auto waited = ElapsedMs() - g_state.pressedAtMs;
            if (a_settings.bufferMs > 0 && waited > static_cast<double>(a_settings.bufferMs)) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> BUFFER expired after {:.1f}ms (cap {}ms)", ElapsedMs(), waited,
                      a_settings.bufferMs);
                g_state.pressPending = false;
                return;
            }

            if (g_state.shoutActive) {
                if (!g_state.windowOpen) return;  // hold it until the window opens
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN from a press {:.1f}ms old ({})", ElapsedMs(), waited,
                      Describe(g_state.pressKind));
                FireChainLocked(a_settings, g_state.pressKind, a_emit);
                return;
            }

            // The shout ended before this press resolved. We swallowed it, so it would otherwise
            // vanish and the player would have pressed attack for nothing. There is no shout
            // left to cut and the graph is already in ready -- past its own reset -- so the
            // attack goes out on its own.
            SHOUTMCO_TRACE("[{:10.2f}] >>> ATTACK without a cut, press {:.1f}ms old ({}) -- the shout had "
                  "already ended", ElapsedMs(), waited, Describe(g_state.pressKind));
            g_state.pressPending = false;
            ResumeAndAttackLocked(a_settings, g_state.pressKind, a_emit);
        }

        // Shared by the hold path (`UpdateHeldStateActive`) and any held event that does reach
        // `ProcessButton`.
        void ResolveAsPowerIfHeldEnoughLocked(const Settings& a_settings, float a_heldSeconds,
                                              float a_gameThreshold, Emit& a_emit) {
            // `pressPending` as well as `pressResolved`: once the chain has fired, further hold
            // events are not a fresh decision to make.
            if (!g_state.pressPending || g_state.pressResolved || !a_settings.HoldToPower()) return;

            // The player's own threshold unless explicitly overridden. A constant of our own
            // would disagree with their game whenever they have changed it.
            const float threshold = a_settings.powerHoldSeconds >= 0.0f  ? a_settings.powerHoldSeconds
                                    : a_gameThreshold > 0.0f             ? a_gameThreshold
                                                                         : 0.3f;
            if (a_heldSeconds < threshold) return;

            g_state.pressKind = AttackKind::kPower;
            g_state.pressResolved = true;
            SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS held to power ({:.2f}s >= {:.2f}s, {})", ElapsedMs(),
                  a_heldSeconds, threshold,
                  a_settings.powerHoldSeconds >= 0.0f ? "INI override"sv : "the game's own"sv);
            TryFireChainLocked(a_settings, a_emit);
        }

        WindowCacheEntry* FindWindowLocked(const WindowKey& a_key) {
            for (auto& entry : g_windowCache) {
                if (entry.minTailMs > 0.0 && entry.key == a_key) return &entry;
            }
            return nullptr;
        }

        // A TAIL THIS SHORT IS NOT A MEASUREMENT OF A CLIP, IT IS THE ABSENCE OF ONE.
        //
        // The tail is `shoutStop` minus `Voice_SpellFire_Event`. Below a frame or two those two
        // events landed in the same graph update: the state ended on the event that fired the
        // spell and no exhale played between them, so there is nothing for a percentage to be a
        // fraction OF.
        //
        // 33 ms is two frames at 60 Hz, which is also one at 30 Hz -- the same argument either
        // way, and wide enough for the pair to straddle a frame boundary. It is a CAUSAL bound
        // rather than a round number: the question it answers is "did an animation play", and the
        // answer is no when the two events cannot be told apart in time.
        //
        // IT MUST NOT BIND IN NORMAL PLAY, which is this file's own rule for a bound (see
        // `kWindowFloorMs` below and the correction attached to it). The shortest tail this
        // project has ever READ is 890 ms -- a factor of 27 -- and the shortest
        // it has an ESTIMATE for is Goetia's one-word exhale at ~615 ms, a factor of 18. Every
        // reading this bound rejects has been sub-millisecond, so nothing observed sits anywhere
        // near it from either side and the gap between the two populations is three orders of
        // magnitude rather than a judgement call.
        //
        // 890 IS A CORRECTION: 922 ms is the minimum of one KEY
        // (`00013E07`, standing, drawn, one word) rather than of the archive.
        // The 0.20 ms figure is DERIVED from adjacent timestamps
        // (spellfire `631229.29`, record
        // `631229.50`), not printed as such -- traces render it `0ms` under `{:.0f}`, which is
        // exactly why the refusal line below prints `{:.2f}`.
        constexpr double kMinClipTailMs = 33.0;

        // ONLY EVER CALLED FROM THE NATURAL `shoutStop` BRANCH. Every other way a shout can end --
        // the liveness cap, a state exit, a game load, our own cut -- must not reach here, and
        // three separate things make sure of it: `cutSent` for the cut, `EndShoutLocked` clearing
        // `spellFiredAtMs` for the rest, and this function clearing it again so one `shoutStop`
        // records at most once.
        void RecordNaturalTailLocked() {
            // `shoutActive` as well as the two obvious guards. `spellFiredAtMs` is set whenever
            // spellfire is seen, but `BeginShoutLocked` returns early on a shout the engine holds
            // nothing for -- so a shout that started outside the engine and ended inside it would
            // otherwise record a real interval against a default key, poisoning the bucket that a
            // null `selectedPower` legitimately uses.
            if (!g_state.shoutActive || g_state.cutSent || g_state.spellFiredAtMs <= 0.0) return;

            const double tailMs = ElapsedMs() - g_state.spellFiredAtMs;
            g_state.spellFiredAtMs = 0.0;

            // REFUSE A TAIL THAT IS NOT A CLIP, AND SAY SO OUT LOUD.
            //
            // This replaces a silent `if (tailMs <= 0.0) return;`, and the silence was half the
            // defect. `0.0` is not the degenerate value -- "shorter than an animation" is -- so
            // every sub-millisecond reading walked straight past that test and into the cache. The
            // driver bucket went from a real 940 ms to 0.20 ms in one cast and stayed there for the
            // rest of the session, after which `iChainWindowPct` meant nothing for casts: 30% of
            // 0 ms is 0 ms, held at the 50 ms floor, window open at spellfire, every time.
            //
            // SAID OUT LOUD BECAUSE A BUCKET THAT QUIETLY DECLINES TO LEARN LOOKS EXACTLY LIKE A
            // BUCKET NOBODY HAS MEASURED YET. Without this line a reader sees "first shout for this
            // key, measuring its length" on every cast forever and has no way to tell a cache that
            // is filling up from one that is refusing to.
            if (tailMs < kMinClipTailMs) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW tail {:.2f}ms NOT cached -- it is under {:.0f}ms, so "
                      "`shoutStop` landed in the same graph update as spellfire and no exhale played "
                      "between them. That is the absence of a clip, not a measurement of one; "
                      "the bucket keeps whatever it already had",
                      ElapsedMs(), tailMs, kMinClipTailMs);
                return;
            }

            const auto& key = g_state.windowKey;
            if (auto* hit = FindWindowLocked(key)) {
                const double cached = hit->minTailMs;
                // TWO CLIPS ARE SHARING ONE KEY. Report that and stop there.
                //
                // Word count is IN the key, so it is no longer a candidate cause
                // and is deliberately not named as one. What remains after shout, stance, draw and
                // word count is genuinely unknown, and the line says so rather than guessing.
                if (tailMs > cached * 1.15 || tailMs < cached * 0.85) {
                    SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW tail {:.0f}ms differs from the cached {:.0f}ms by "
                          "more than 15% -- two clip lengths are sharing one cache key, so something "
                          "this key does not distinguish is varying the animation. Shout, stance, "
                          "draw and word count are all keyed, so this is a cause none of them covers",
                          ElapsedMs(), tailMs, cached);
                }

                // MIN-BIAS ON DIVERGENCE, AND THE REASONING IS KEPT SO REPLACE-ON-DIVERGENCE IS
                // NOT REBUILT. When the detector above fires, the cached value describes a
                // DIFFERENT clip, so keeping the minimum of two clips is keeping a measurement of
                // something the next shout will not play. Replacing on divergence looks like the
                // obvious recovery, and it is the larger failure:
                //
                // COMPARE THE WORST CASES.
                //
                //   MIN-BIAS, too SHORT a cached tail: the window opens earlier than the
                //   percentage asked for. The player gets more cancellable tail than intended.
                //   The chain still fires and the attack still comes out.
                //
                //   REPLACE, too LONG a cached tail: `openInMs` can exceed the actual tail, so the
                //   scheduled open lands after `shoutStop` and `OpenWindowLocked` drops it. NO
                //   window opens, the press falls through to `EndShoutLocked`'s hand-back, and the
                //   attack comes out uncut. The chain silently does not happen, and nothing in the
                //   log says the window it promised never arrived.
                //
                // A window that opens early is a worse window; a window that never opens is not a
                // window. The second is the larger failure and it degrades less gracefully, so
                // min-bias stays. Worked example on the driver bucket, which is the one at risk
                // because EVERY cast type shares one key: clips of 1700 and 940 ms
                // alternating. Min-bias settles on 940 and opens early on the long cast, forever.
                // Replace oscillates, and each 940 ms cast scheduled from a cached 1700 computes
                // `openInMs = 1190 ms` against a tail of 940 -- it misses its own shout entirely,
                // on roughly half of all casts.
                //
                // A bucket poisoned by a legitimately SHORT REAL clip still stays
                // poisoned for the session. What no longer poisons it is a 0 ms reading that is not
                // a clip at all -- that one is now refused above rather than recovered from
                // afterwards. The ordinary bucket (`00013E07`, a 5% spread across fifteen sessions)
                // is nowhere near the 15% band; mixed clip lengths are structural in the DRIVER
                // bucket.
                if (tailMs < cached) hit->minTailMs = tailMs;
                SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW tail measured {:.0f}ms (shout {:08X}, {}, {}, {}) -- "
                      "cached minimum now {:.0f}ms", ElapsedMs(), tailMs, key.shout,
                      key.sneaking ? "sneaking"sv : "standing"sv,
                      key.drawn ? "drawn"sv : "sheathed"sv, VariationName(key.variation),
                      hit->minTailMs);
                return;
            }

            auto& slot = g_windowCache[g_windowCacheNext];
            g_windowCacheNext = (g_windowCacheNext + 1) % g_windowCache.size();
            slot.key = key;
            slot.minTailMs = tailMs;
            SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW tail measured {:.0f}ms (shout {:08X}, {}, {}, {}) -- first "
                  "for this key, cached", ElapsedMs(), tailMs, key.shout,
                  key.sneaking ? "sneaking"sv : "standing"sv,
                  key.drawn ? "drawn"sv : "sheathed"sv, VariationName(key.variation));
        }

        void OpenWindowLocked(const Settings& a_settings, std::string_view a_reason, Emit& a_emit) {
            if (!g_state.shoutActive || g_state.windowOpen) return;
            g_state.windowOpen = true;
            SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW open ({})", ElapsedMs(), a_reason);
            TryFireChainLocked(a_settings, a_emit);
        }

        // BACKSTOPS ON THE PROPORTIONAL WINDOW -- NOT TUNING KNOBS.
        //
        // Both are internal constants, and both are set so that they do NOT
        // bind AT THE SHIPPED DEFAULT on any measured shout. That is the whole of their design: a
        // bound that fires in normal play is a second tuning parameter wearing a safety bound's
        // clothes, and it would silently overrule the percentage the owner set.
        //
        //   FLOOR -- below about a frame the scheduled open lands inside the same frame as
        //   `shoutStop`, so the thread is spawned to do nothing and the press falls through to the
        //   natural-end path anyway. 50 ms is three frames at 60 Hz: comfortably above the useless
        //   range and far below the smallest real window (30% of the shortest measured tail, Goetia
        //   short at ~615 ms, is ~185 ms).
        //
        //   CEILING -- a genuine backstop against a wild measurement, not a shape for the feature.
        //   The largest case in the installed load order is Goetia's three-word exhale. Its tail is
        //   UNMEASURED (see the reference list above -- the "~3.82 s" this used to cite is clip
        //   length minus SYHO's offset, not a reading), but it is somewhere around 3.5-3.9 s, which
        //   puts 30% of it near 1.1 s and leaves 2000 ms clear at the default on any figure in that
        //   range. Block 1 of the owed session replaces the estimate with a reading.
        //
        // "AT THE SHIPPED DEFAULT" IS A CORRECTION, and the qualifier is the whole of it (cold
        // review of `3f155b5..ed4c9f6`). This comment used to claim the bounds do not bind on any
        // measured shout, full stop, and the ceiling paragraph derived that from a tail "beyond
        // 6.6 s". Both are true of `iChainWindowPct = 30` and neither is true of the range the INI
        // actually accepts: `Settings.cpp` clamps the setting into 0..100, and on Goetia's own
        // three-word tail -- 3.5-3.9 s on the estimates available -- the ceiling binds from roughly
        // 50-57% upward. A player who raises the percentage to break out of long shouts earlier is
        // exactly the player who would hit it, and the old text told the next reader that case
        // could not arise. The threshold is a range rather than a figure because the tail it
        // divides has never been measured; what is certain is that it falls inside 0..100.
        //
        // So the bounds are still not tuning knobs, and the rule below still stands -- but the
        // engine now SAYS when one of them binds instead of leaving it to be deduced, because
        // "observed binding in a trace" was the stated detection mechanism and nothing emitted it.
        //
        // If either of these is ever observed binding in a trace, the finding is that the tail
        // measurement is wrong, or that the percentage is set past the range these were sized for
        // -- not that the constant needs raising.
        constexpr double kWindowFloorMs = 50.0;
        constexpr double kWindowCeilingMs = 2000.0;

        [[nodiscard]] double ClampWindowMs(double a_ms) {
            // Not `std::clamp`: `min`/`max` are macros here, courtesy of the Windows headers, and
            // this file has been bitten by that twice already.
            if (a_ms < kWindowFloorMs) return kWindowFloorMs;
            if (a_ms > kWindowCeilingMs) return kWindowCeilingMs;
            return a_ms;
        }

        // AT `Voice_SpellFire_Event`: open the window now, or plan to open it later.
        //
        // Decides only. The pacing itself is an emission and runs in phase C -- spawning a thread
        // while holding the engine lock would be a new lock-ordering hazard in the one file that
        // has been careful not to have any.
        void ScheduleOrOpenWindowLocked(const Settings& a_settings, Emit& a_emit) {
            // NO WINDOW FOR A SHOUT THE ENGINE NO LONGER HOLDS.
            //
            // `Observe`'s spellfire arm is gated on `settings->enabled` ALONE, and a driver's cast
            // can raise `Voice_SpellFire_Event` long after the graph left the shout state: a jump
            // during a hotbar cast raises `SBF_ShoutStop`, `EndShoutLocked` clears `shoutActive` --
            // correctly, the state exit is real -- and the cast carries on to fire its own spell
            // 693 ms later. Everything
            // below this line is scoped to a live shout: `windowKey` is the key that shout was
            // armed with, and `g_shoutGeneration` is the generation it claimed.
            //
            // `RecordNaturalTailLocked` guards on `shoutActive` for the same class of reason and
            // says so in its own comment. This is that guard's missing twin.
            //
            // IT COULD NOT MISFIRE, WHICH IS WHY THIS IS HYGIENE RATHER THAN A DEFECT FIX.
            // What stops it is `OpenWindowLocked`, which returns on `!shoutActive` before it sets
            // anything or reaches `TryFireChainLocked` -- and that covers the paced path and both
            // open-now paths below alike, because all three go through it. What was left is a
            // thread spawned and slept to do nothing, and a `>>> WINDOW open scheduled in Nms` line
            // in the log for a shout the engine is not holding. This file's own rule is that a
            // marker whose field contradicts the claim it supports is an assertion rather than an
            // instrument.
            //
            // ONE GUARD, NOT TWO. A generation check as a second guard NEVER FIRES
            // HERE: `g_shoutGeneration` is bumped only in `BeginShoutLocked` and on game load,
            // never in `EndShoutLocked`, so a schedule made after the shout ended carries the
            // CURRENT generation and passes that check untouched. The two are complementary rather
            // than redundant -- the generation covers "a newer shout has begun", `shoutActive`
            // covers "no shout is live at all".
            // `shoutActive` is set true only after the `fetch_add`, so no stale schedule can ever
            // find a matching generation AND a live shout.
            //
            // REFUSED HERE, AND IT SAYS SO. A window silently not scheduled is indistinguishable
            // from `iChainWindowPct = 0`, which has its own line immediately below; refusing at the
            // call site instead would have left the two cases reading the same in a trace.
            if (!g_state.shoutActive) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW none -- spellfire arrived with no live shout, so "
                      "there is nothing to chain out of and nothing is scheduled. A driver's cast "
                      "reaches this when an interruption ended the shout it armed and the cast "
                      "itself carried on", ElapsedMs());
                return;
            }

            if (a_settings.chainWindowPct <= 0) {
                // `0` means no window, which is a supported answer and not a degenerate one: the
                // buffered press fires when the shout ends on its own, through the natural-end
                // path below. Traced so a run with no `>>> WINDOW open` in it is legible.
                SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW none -- iChainWindowPct is 0, so a buffered press "
                      "waits for the shout to end on its own", ElapsedMs());
                return;
            }

            const auto* hit = FindWindowLocked(g_state.windowKey);
            if (!hit) {
                // Said out loud, so a first-shout-per-key is never mistaken for the feature
                // failing -- it looks exactly like the old stopgap, because it is.
                OpenWindowLocked(a_settings,
                                 "spellfire -- first shout for this key, measuring its length"sv, a_emit);
                return;
            }

            // THE WINDOW IS A FRACTION OF THE MEASURED TAIL.
            //
            // It was a fixed millisecond count, and the reason it changed is a
            // measurement rather than a preference. The cancellable
            // tail should be the animation's RECOVERY -- the part after the gesture has landed,
            // which reads as hanging if the player is held through it. Measured on MCO's own light
            // attack, that recovery is 23.4% and 24.9% of the animation, i.e. a
            // fraction, and a fixed count cannot track it: 300 ms was 29.0% of Goetia's one-word
            // exhale, 15.3% of its two-word and 7.6% of its three-word, so the longer the animation
            // the more of it the player was held through. Held for 92.4% of a 3.93 s clip is the
            // defect this replaces.
            //
            // ADR-0004 supersedes ADR-0003's fractional-window rejection and records why both of
            // that decision's premises are spent. Read it before changing this back.
            const double askedMs =
                hit->minTailMs * static_cast<double>(a_settings.chainWindowPct) / 100.0;
            const double windowMs = ClampWindowMs(askedMs);
            // SAID OUT LOUD WHEN A BOUND BINDS. ADR-0004 and the constants above both rest on a
            // binding clamp being "observed binding in a trace", and until this line nothing
            // emitted one -- the schedule line below printed the clamped figure while still
            // attributing it to the percentage, so a bound window was reported as though the
            // arithmetic had produced it. A marker whose own field contradicts the claim it
            // supports is an assertion, not an instrument (the `g_shoutLiveForTrace` error, again).
            if (windowMs != askedMs) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW {}% of a measured {:.0f}ms tail is {:.0f}ms, held at "
                      "the {} of {:.0f}ms -- the percentage is NOT what this shout will use",
                      ElapsedMs(), a_settings.chainWindowPct, hit->minTailMs, askedMs,
                      windowMs > askedMs ? "floor"sv : "ceiling"sv, windowMs);
            }
            const double openInMs = hit->minTailMs - windowMs;
            if (openInMs <= 0.0) {
                // Reachable two ways, and the second was missed when this comment was written: the
                // FLOOR exceeding a very short tail, and `iChainWindowPct = 100`, which the settings
                // clamp explicitly allows and which makes the window the whole tail. The old text
                // said "a percentage of a positive tail is always smaller than that tail", which is
                // true of every percentage except the largest one the INI accepts.
                //
                // Opening now is the correct answer in both cases rather than a special case -- the
                // window cannot start before spellfire, because before spellfire a cut does not
                // deliver the shout.
                OpenWindowLocked(a_settings,
                                 "spellfire -- the measured tail is no longer than the window"sv, a_emit);
                return;
            }

            a_emit.scheduleWindowOpen = true;
            a_emit.scheduleWindowInMs = openInMs;
            a_emit.scheduleGeneration = g_shoutGeneration.load(std::memory_order_relaxed);
            // States the window it will actually use and the tail it sits at the end of, and does
            // NOT assert that one is a percentage of the other -- the clamp above can make that
            // false, and the line that used to claim it was the misreport described there.
            SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW open scheduled in {:.0f}ms -- a {:.0f}ms window at the end "
                  "of a measured {:.0f}ms tail (iChainWindowPct = {}), so it should open ~{:.0f}ms "
                  "before `shoutStop`",
                  ElapsedMs(), openInMs, windowMs, hit->minTailMs, a_settings.chainWindowPct, windowMs);
        }

        // A resolved press is handed back at the shout's end from inside
        // `EndShoutLocked`. A hand-back that lives beside the function
        // that clears the press, rather than inside it, is a hand-back somebody will forget to
        // call. Its reasoning is carried into `EndShoutLocked` below.

        void AbandonQueuedResumeLockedImpl(std::string_view a_reason) {
            if (!g_queuedResume.valid) return;
            SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO snapshot abandoned after {:.0f}ms ({})", ElapsedMs(),
                  ElapsedMs() - g_queuedResume.takenAtMs, a_reason);
            g_queuedResume = QueuedResume{};
            g_comboSampleWanted.store(false, std::memory_order_relaxed);
        }

        // `a_driverCast` GATES EXACTLY ONE LINE -- the no-shout-equipped decline. It briefly gated
        // the root as well; a driver's cast is now rooted on the same rule as a shout, so that guard
        // is gone. Everything else a driver's cast does here is
        // the same body a real shout runs, which is the point of ADR-0006: there is one arming path
        // with one generation bump, one press carry, one snapshot rule and one trace, not two that
        // drift.
        //
        // WHY THE FLAG AND NOT A KEY TEST. The decline below fires on `a_windowKey.shout == 0`, and
        // `0` is EXACTLY the bucket a driver's cast is designed to land in. So the key cannot tell
        // "this is a driver's cast, arm it" from "this is a shout with no power selected, decline
        // it" -- they are the same value. The caller knows, because the caller is the one that saw
        // whether a `BeginCastVoice` vouched for the entry, and it passes what it knows.
        void BeginShoutLocked(const Settings& a_settings, const SampledCombo& a_live, bool a_wasAttacking,
                              const WindowKey& a_windowKey, Emit& a_emit, bool a_driverCast,
                              bool a_sprinting) {
            // VESTIGIAL. `Settings::enabled` is hard-wired true -- the master switch was retired
            // and nothing parses it any more -- so this never returns. Kept because the field is
            // read at eleven sites and unpicking them is surgery on proven runtime code for no
            // gain. If it ever does go, the graph needs nothing: the root is set below, so no
            // lock is written on this path and `SHOUT_lock == 0` leaves the vanilla `moveStart`
            // transition exactly as it was.
            if (!a_settings.enabled) return;

            // DO NOT ENGAGE WHEN NO SHOUT IS EQUIPPED, BECAUSE THAT IS A DRIVER.
            //
            // Spell Hotbar 2 has no casting graph. It casts by notifying the vanilla shout graph
            // directly and runs its own cast timer, using `IsShouting` as the liveness check. Our
            // cut is `shoutStop`, which CLEARS `IsShouting` -- so chaining out of one of its casts
            // tears the cast down and the spell is lost outright. Chaining out of such
            // a cast is data loss, not a feature. A real shout survives the cut because vanilla
            // fires `Voice_SpellFire_Event` a tenth of a second in, so the magic is already out; a
            // driver that does not use that event has nothing already out to survive on.
            //
            // A player shout always has a selected power. A driver cast does not, and
            // `ReadWindowKey` already reads it at `BeginCastVoice` -- the one moment its own comment
            // calls trustworthy -- so this costs one condition and no new game read.
            //
            // ADR-0002 forbids reading COOLDOWN state -- the voice recovery timer and anything
            // derived from it -- and says nothing about equipment state. `ReadShoutVariation` below
            // already reads `currentShout` and argues exactly that distinction.
            //
            // DECLINED BEFORE ANY STATE IS TOUCHED, deliberately. Not arming means no press is
            // taken, no window is scheduled and no cut can fire, which is the whole point -- the
            // engine simply is not present for this cast. Anything a previous shout left behind
            // ages out through the watchdogs exactly as it would have.
            //
            // THE DECIDING VALUE IS TRACED because the record disagrees with itself about what
            // Spell Hotbar 2 does here: this file says it "empties the slot", while other notes say
            // it "swaps `selectedPower` and restores it in `on_reset`". If it swaps in a non-null
            // value this guard silently does nothing, and a trace that only said "declined" would
            // never reveal that. No live hotbar cast has
            // ever been observed at this entry.
            //
            // WHAT THIS DECLINE MEANS WITHOUT CHANGING THE POLICY, and ADR-0006
            // asks for that to be said here rather than discovered. Reached from `BeginCastVoice`
            // -- `a_driverCast == false` -- it still means what it always meant: the engine is not
            // present for a cast it cannot see the commitment of. With `bChainDriverCasts` on, the
            // engine now DOES see that commitment, at the shout-state entry, and arrives here with
            // `a_driverCast == true` having already made the decision this guard used to make.
            //
            // So the guard is not weakened and it is not bypassed. It is asked at the one moment it
            // can answer, and answered somewhere else at the one moment it cannot: a cast raises no
            // `BeginCastVoice`, so a decline taken here has, by construction, never once been the
            // thing that declined a real hotbar cast -- the engine never
            // reaches this function at all on one. What it still guards is the case it was written
            // for: a `BeginCastVoice` with nothing in `selectedPower` behind it.
            if (!a_driverCast && a_windowKey.shout == 0) {
                // THE LINE ITSELF SAYS WHAT THE DECLINE NOW MEANS, because ADR-0006 asks for that
                // in terms -- "the trace line should say so, or the next reader files a bug against
                // it" -- and a reader of a LOG cannot see a C++ comment. With the feature on this
                // is no longer "never engage"; it is "not from here".
                SHOUTMCO_TRACE(
                    "[{:10.2f}] >>> SHOUT begin DECLINED -- no shout equipped (selectedPower null), so this is a "
                    "driver's cast and not the player's shout; cutting it would destroy the spell. "
                    "This declines ONLY this entry point: with bChainDriverCasts = 1 the engine may still arm at "
                    "the shout-state entry, after the driver has committed{}",
                    ElapsedMs(),
                    a_settings.chainDriverCasts ? ""sv : " -- it is 0, so nothing will"sv);
                return;
            }

            // Claimed before anything else can schedule against it: any paced window
            // open still asleep from the previous shout is now stale and its task will drop it.
            g_shoutGeneration.fetch_add(1, std::memory_order_relaxed);

            const bool   wasArmed = g_state.awaitingReady;
            const bool   ownedPress = g_state.pressOwned;
            const double ownedPressAtMs = g_state.pressOwnedAtMs;
            // The whole press, not just its ownership latch -- see below.
            const bool       heldPress = g_state.pressPending;
            const bool       heldResolved = g_state.pressResolved;
            const double     heldPressedAtMs = g_state.pressedAtMs;
            const AttackKind heldKind = g_state.pressKind;
            g_state.Reset();
            // Ownership of a button that is still physically down survives the reset, or its
            // release would reach the game's handler with no matching press. Its clock survives
            // with it: a preserved latch carrying a zeroed timestamp would look to the watchdog
            // like a press owned since the start of the session and be released on the spot.
            g_state.pressOwned = ownedPress;
            g_state.pressOwnedAtMs = ownedPressAtMs;

            // AND THE PRESS ITSELF, WHICH OWNERSHIP ALONE IS NOT.
            //
            // An unresolved press outlives its shout, so the player can still be
            // holding the button when they start the NEXT one. `Reset()` cleared `pressPending`
            // while this function restored `pressOwned` -- which is the worst of both: the engine
            // keeps swallowing every event from a button it no longer has a press for, so the hold
            // resolves nothing, the release fires nothing, and NOTHING IS TRACED. The player's
            // charged attack simply never comes out. That is the same shape as dropping a press
            // at shout end, reintroduced one function away from the fix.
            //
            // Carrying it is not merely safer than dropping it, it is the better behaviour. A press
            // still physically held becomes an ordinary unresolved press inside the new shout and
            // the existing paths chain it out of THAT shout. A resolved
            // press buffered before this shout began is also carried; its `waitingForShoutStart` flag is cleared by
            // the reset above, which is exactly what lets the normal window decide it now. The
            // fields are carried as they stand rather than assumed.
            //
            // Still bounded: `WatchdogReleasePressLocked` caps ownership at `pressOwnershipCapMs`
            // and clears both, so a button held across several shouts cannot accumulate anything.
            g_state.pressPending = heldPress;
            g_state.pressResolved = heldResolved;
            g_state.pressedAtMs = heldPressedAtMs;
            g_state.pressKind = heldKind;
            if (heldPress) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS carried into the new shout ({:.0f}ms old, {}) -- it "
                      "now belongs to this shout and chains out of it", ElapsedMs(),
                      ElapsedMs() - heldPressedAtMs, heldResolved ? Describe(heldKind) : "unresolved"sv);
            }
            if (wasArmed) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN dropped: a new shout began before `inRdy`", ElapsedMs());
            }

            g_state.shoutActive = true;
            g_state.shoutStartedAtMs = ElapsedMs();
            g_state.teardownReadyPending = a_wasAttacking;
            g_state.driverCast = a_driverCast;

            // ROOT THE SHOUT, both routes at once.
            //
            // Set here rather than at spellfire because the ruling is "rooted from
            // `BeginCastVoice`", and here is `BeginCastVoice` on the ordinary path.
            //
            // A DRIVER'S CAST IS ROOTED TOO, ON THE SAME RULE, WITH NO EXCEPTION.
            // This function used to be reached only by the player's own
            // shout, so this block never had to ask; it briefly grew an `if (!a_driverCast)` guard
            // when a cast reached it, and the guard is gone. Casting of any kind
            // takes commitment.
            //
            // THE CASE FOR AN EXCEPTION WAS PUT AND LOST, and it is recorded so it is not re-put.
            // It ran: a hotbar cast is another mod's spell borrowing the shout graph, and rooting
            // it changes that mod's feel from outside. Three things answered it.
            //
            //   1. `Magic Casting Behavior Overhaul` -- this project's reference implementation
            //      (ADR-0001) -- commits casts through ROOT MOTION IN ITS OWN CLIPS. We cannot
            //      copy the mechanism on a driver cast (the clip is the driver's
            //      `MT_BreathExhaleShort`, not ours), so "match MSCO" cannot mean matching its
            //      lever. It also excludes concentration and ritual casts, which is the class of
            //      cast measured here -- so the precedent does not settle this either way.
            //   2. THE SHAPE OF IT, one layer up. Releasing
            //      the root at `Voice_SpellFire_Event` was proposed for a cast and is exactly the
            //      design that was superseded: the exhale is the shout's `weaponSwing`, and MCO
            //      does not hand control back there. A chained MCO attack travels 300 units in
            //      1406 ms with no keys held, 48 of them AFTER `weaponSwing`.
            //      Commitment covers the follow-through.
            //   3. THE COST WAS SMALLER THAN IT LOOKED. A cast is 1342 ms of wind-up plus 929.5 ms
            //      after spellfire -- and that
            //      929.5 ms IS an ordinary shout's exhale, traced at ~950 ms. So a hotbar cast is a
            //      normal shout's exhale with wind-up in front. 2271 ms total is ~1.6x an MCO
            //      chained attack's own 1406 ms, for the slowest spell class there is.
            //
            // AND THE EXCEPTION WOULD HAVE COST THE CHAIN, which is the requirement it was meant to
            // serve. Every chain then fires
            // from a non-steering state, which structurally removes the moving-chain defect class.
            // An unrooted cast hands the player back to
            // a steering state during the chain window -- the one posture no trial in this repo
            // has ever driven.
            //
            // WHAT A DRIVER'S CAST GETS THAT A SHOUT DOES NOT, and it is an improvement rather than
            // a special case: both halves go out from the SAME event. `BeginShoutLocked` is called
            // from `Observe`'s `isShoutStateEntry` branch on this path, and route 2 runs just below
            // that call on the same `Emit` -- so the lock is written and `moveStop` raised inside
            // one deferred task, in that order. That is precisely the ordering
            // engineered for an ordinary shout, where the lock leaves at `BeginCastVoice` and the
            // event waits for the state entry. Here it holds by construction.
            //
            // ONE LIMIT, SHARED WITH ORDINARY SHOUTS AND NOT INTRODUCED HERE: a JUMP lifts the root
            // early. `SBF_ShoutStop` ends the shout and clears the lock, and the graph's re-entry
            // ~60 us later cannot re-arm (no ready token precedes a bounce), so the remainder runs unrooted. An ordinary shout does exactly the
            // same thing today. It fails in the safe direction -- steering returns, nothing sticks
            // -- and it belongs with jump bounce, not here. Do not "fix" it by re-arming on a bounce.
            //
            // TWO EMISSIONS, ONE ORDERING REQUIREMENT, AND THEY ARE TWO HALVES OF ONE MECHANISM.
            //
            // Route 1 is the `moveStart` transition, now gated on `SHOUT_lock == 0` -- writing the
            // lock closes it, and a player standing still simply never leaves `ShoutStanding`.
            //
            // Route 2 exists because `#0094`'s `startStateId` is variable-bound to
            // `iSyncIdleLocomotion`, so a shout begun while already running STARTS in the steering
            // state and route 1's transition never fires at all. It notifies vanilla `moveStop`,
            // which drives the edge vanilla already has (`#0321`, ungated), and needs no node of
            // ours. `SHOUT_root` was tried first and measured undeliverable -- see `kLeaveLocomotionEvent`.
            //
            // THE TWO HALVES ARE SENT AT DIFFERENT MOMENTS, and that is the correction.
            // The lock is a graph VARIABLE, read continuously, so it goes out here at
            // `BeginCastVoice` and route 1's gate is closed as early as it can be. Route 2's
            // `moveStop` is an EVENT, consumed once by whatever is listening at that instant, and
            // sending it here sent it ~5 ms before the shout state existed -- so `#0094` could not
            // consume it, the locomotion graph did, and the machine then started in
            // `ShoutLocomotion` anyway. It is raised at `SBF_ShoutStart` instead.
            //
            // The lock still has to precede the event, and now does so by construction rather than
            // by ordering inside one task: `moveStop` is a one-shot the movement controller undoes
            // on its next update while the key is held, and route 1's gate is the only thing that
            // stops the undo. Leave locomotion into an open gate and the graph walks straight back.
            //
            // Rebinding or unbinding `startStateId` was rejected as the route-2 mechanism. The
            // binding is read once, when the machine activates, which is the same frame as
            // `BeginCastVoice` and therefore before any deferred write of ours can land; and
            // unbinding it outright would root a moving start even with the engine off, which is a
            // behaviour change nobody asked for. Leaving the state a frame or two late, and only
            // while the lock is set, is the smaller change.
            // The LOCK only, ON THE ORDINARY PATH. Route 2's event is raised at `SBF_ShoutStart`,
            // not here -- see the ordering note above and `isShoutStateEntry` in `Observe`. On the
            // DRIVER path this line and route 2 are the same event, which is the improvement noted
            // above rather than a case to handle.
            //
            // UNCONDITIONAL. Every shout-state entry the engine arms on is rooted,
            // whether a `BeginCastVoice` vouched for it or not.
            g_state.rootLocked = true;
            a_emit.setLock = true;
            a_emit.lockValue = 1;

            // Once per session -- and the latch is CLAIMED HERE, in phase B, not inside
            // the task that does the reading.
            //
            // Setting it in the task looks equivalent and is not. Two shouts can be decided
            // before either one's task drains, so both would read the latch as false and both
            // would warn -- and "one plain-language warn" is the cell. Worse in the other
            // direction: a verify task still in flight across a game load would store `true`
            // AFTER the load listener stored `false`, and the new session would never check at
            // all. Claiming the check under the lock makes it exactly once per session in both
            // directions.
            //
            // A DRIVER'S CAST MAY NOW CLAIM IT, and that is correct: the check asks whether the
            // behaviour patch is present in THIS session's graph, which is a fact about the graph
            // and not about what armed. Whichever entry comes first spends it.
            a_emit.verifyLock = !g_rootVerified.exchange(true, std::memory_order_relaxed);

            // Read once, here, and not again at spellfire -- see the field's comment for why the
            // selected power is not safe to re-read mid-shout.
            g_state.windowKey = a_windowKey;
            // Same once-only rule, same reason -- the shout tears sprint down, so this
            // fact is only readable at the arm. See the field.
            g_state.sprintingAtBegin = a_sprinting;
            // Decided here, from the key this arm already
            // read, so it is settled before any press can consult it and cannot carry over from
            // the previous shout. `a_windowKey.shout` is `selectedPower`'s FormID -- zero for a
            // driver cast that emptied the slot, which `IsWhirlwindSprintShout`
            // answers false for, so a hotbar cast falls back to the prior-sprint rule.
            g_state.whirlwindThisShout = IsWhirlwindSprintShout(a_windowKey.shout);

            // The queued snapshot wins when there is one AND it is young enough to describe this
            // shout. The longest legitimate queue-to-shout path is bounded by the
            // wait cap plus the replay tap; a snapshot older than that belongs to a press whose
            // shout never started -- the game refused it, or the replay was superseded -- and
            // restoring its combo into an unrelated shout is a guess wearing a memory's clothes.
            // A live read here is only correct for a shout that began with no attack to remember.
            const double maxAgeMs =
                static_cast<double>(a_settings.shoutWaitCapMs) + kQueuedResumeSlackMs;
            const bool   haveSnapshot = g_queuedResume.valid;
            const double ageMs = haveSnapshot ? ElapsedMs() - g_queuedResume.takenAtMs : 0.0;
            const bool   fromQueue = haveSnapshot && ageMs <= maxAgeMs;
            if (haveSnapshot && !fromQueue) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO snapshot EXPIRED ({:.0f}ms old, cap {:.0f}ms) -- "
                      "reading live instead", ElapsedMs(), ageMs, maxAgeMs);
            }

            const int nextAttack = fromQueue ? g_queuedResume.nextAttack : a_live.v.nextAttack;
            const int nextPower = fromQueue ? g_queuedResume.nextPowerAttack : a_live.v.nextPowerAttack;
            const int currentAttack = fromQueue ? g_queuedResume.currentAttack : a_live.v.currentAttack;
            const int currentPower =
                fromQueue ? g_queuedResume.currentPowerAttack : a_live.v.currentPowerAttack;
            g_queuedResume = QueuedResume{};
            g_comboSampleWanted.store(false, std::memory_order_relaxed);

            g_state.resumeAttack = a_settings.resumeMode == Settings::ResumeMode::kIncrementCurrent
                                       ? currentAttack + 1
                                       : nextAttack;
            g_state.resumePowerAttack = a_settings.resumeMode == Settings::ResumeMode::kIncrementCurrent
                                            ? currentPower + 1
                                            : nextPower;

            // Three distinguishable sources, so a future session can tell which answered:
            // a fresh snapshot, an expired one falling back to live, or a
            // plain live read with no queued press at all.
            // The shout id is printed on the ENGAGED path too, not just the declined one.
            // A reader comparing a working shout against a driver's cast needs the value the
            // guard decided on in both traces; printing it only when it is zero would make the
            // interesting case the one with no evidence in it.
            //
            // THE ARMING REASON IS IN THE LINE, and it is not decoration. With
            // `bChainDriverCasts` on there are two ways to reach this trace and they mean different
            // things -- the player shouted, or another mod's spell entered the shout state -- and
            // `shout 00000000` does not distinguish them from a shout whose `selectedPower` read
            // null. Which of those happened is the whole question, so a reader who
            // cannot tell them apart cannot mark either.
            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT begin{} (shout {:08X}), resume attack={} power={} ({}){}", ElapsedMs(),
                  a_driverCast ? " -- DRIVER CAST armed at the shout-state entry, no `BeginCastVoice` behind it; "
                                 "the window still waits for spellfire"sv
                               : ""sv,
                  a_windowKey.shout,
                  g_state.resumeAttack, g_state.resumePowerAttack,
                  fromQueue          ? std::format("snapshot taken when the press was queued, {:.0f}ms ago", ageMs)
                  : haveSnapshot     ? std::string{"expired snapshot discarded -- read live"}
                                     : std::string{"read live -- no queued press"},
                  g_state.teardownReadyPending ? " (out of an attack: one ready pass owed)"sv : ""sv);
            // PRINTED BOTH WAYS SINCE THE SECOND PASS, and that is the whole lesson of the first
            // one. The engaged line alone made "not from a sprint" and "this build has no sprint
            // code in it" the same observation -- an ABSENT line -- and a failing drive
            // could only be read by noticing a silence. A reading that costs nothing to print is
            // worth printing on the boring branch too.
            SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT sprint reading: {}{} (last sprint {:.0f}ms ago)",
                  ElapsedMs(),
                  g_state.whirlwindThisShout
                      ? "WHIRLWIND SPRINT -- the chain takes the sprint entry events with no prior "
                        "sprint required"sv
                      : g_state.sprintingAtBegin
                            ? "began from a SPRINT -- a chained attack takes the sprint entry events "
                              "if the player is still moving at the fire edge"sv
                            : "NOT from a sprint, NOT a whirlwind -- the ordinary entry events stand"sv,
                  (g_state.whirlwindThisShout && g_state.sprintingAtBegin) ? ", and from a sprint too"sv
                                                                          : ""sv,
                  ElapsedMs() - g_lastSprintingMs.load(std::memory_order_relaxed));
        }

        // THE END OF A SHOUT, AND WHAT HAPPENS TO A PRESS THE PLAYER IS STILL MAKING.
        //
        // This function used to clear `pressPending` on every exit, and the hand-back sat
        // outside it on the `shoutStop` branch alone. The RESOLVED press dying on the stray-ready
        // exit and the UNRESOLVED press dying on the natural one are closed together here, by one
        // rule rather than two special cases.
        //
        // THE RULE: DO NOT DECIDE THE PRESS AT THE SHOUT'S END AT ALL. Let it outlive the shout and
        // let the paths that already exist finish it.
        //
        //   - RESOLVED     -> fire now. There is no shout left to cut, so `TryFireChainLocked`
        //                     takes its plain-attack branch. This is the hand-back, reached
        //                     from every player-facing exit instead of one.
        //   - UNRESOLVED   -> leave `pressPending`, `pressResolved` and `pressOwned` exactly as
        //                     they are. The button is still down and the player has not said what
        //                     they meant by it yet, so the input hooks resolve it after the shout
        //                     exactly as they would have during it: held past the threshold ->
        //                     `OnAttackHold` -> `ResolveAsPowerIfHeldEnoughLocked` -> power;
        //                     released short -> `OnAttackButton`'s release branch -> light; never
        //                     released -> `WatchdogReleasePressLocked` at `pressOwnershipCapMs`.
        //                     `ResolveAsPowerIfHeldEnoughLocked` already works post-shout -- its
        //                     guard never mentions `shoutActive`, it only needs `pressPending` to
        //                     survive, which is what this preserves.
        //   - ABANDONED    -> drop it, deliberately.
        //
        // The worst case of the unresolved branch is that the attack lands later than the shout's
        // end by however long the player keeps holding. THAT IS WHAT HOLD-TO-POWER MEANS; it is the
        // behaviour, not a failure mode.
        //
        // TWO CANDIDATE FIXES WERE TRACED AND REJECTED, and they are recorded here so
        // neither is retried. "Resolve it as a light attack on the spot" always produces an attack
        // and produces the WRONG one -- a player charging a power attack gets a light one at an
        // instant they do not control, and their remaining hold then goes inert. "Hand ownership
        // back to the game" is worse: THE GAME NEVER SAW THE DOWN EDGE, because `OnAttackButton`
        // sets `consumed = true` when it takes the press, so handing back the release gives the
        // game an up edge for a press it has no record of and Skyrim decides light-versus-power
        // from a held duration it tracked itself. Its worst case is the original defect.
        //
        // WHICH EXITS ARE PLAYER-FACING, and the one judgement call in this function.
        //
        // `kFinished` is the player finishing their shout. `kStateExit` is the player's shout being
        // cut short -- a sheathe, a jump, a knockdown, a cancelled hotbar cast. Both are the
        // player's, so both hand the press back. `kAbandoned` is the only exit that is not: the
        // liveness cap means nothing ended the shout for eight seconds, and a press that old is not
        // one they still want.
        //
        // THE `spellFired` SPLIT THAT USED TO STAND HERE IS GONE, and the deletion is the
        // whole of that change. A state exit BEFORE spellfire was classified as abandonment -- "the
        // shout has delivered nothing, so there is nothing to chain out of" -- and the press was
        // dropped. Every word of that is true about the SHOUT and none of it is about the PLAYER,
        // who pressed attack and got nothing whatever the magic did.
        //
        // ARMING ON A HOTBAR CAST MADE IT REACHABLE AND MEASURABLE. That path put 240-1350 ms
        // of release lead in front of spellfire where an ordinary shout's
        // `BeginCastVoice` -> spellfire measures 103-146 ms across six committed traces, so the drive
        // caught in one trial what a shout hides in a sliver: the
        // press taken, then `>>> SHOUT end ... PRESS dropped`, no attack, and the button still
        // physically down 1896 ms later. With `bChainDriverCasts = 0` the same press read `>>> INPUT
        // forwarded to the game` and produced an attack. A feature that turns a forwarded press into
        // a lost one is worse than the feature being off, so the split went rather than the arm.
        //
        // WHAT IT COSTS, stated rather than glossed. A jump raises `SBF_ShoutStop` and re-enters the
        // shout state ~0.06 ms later (3 of 3), so a press buffered inside the pre-spellfire
        // sliver now produces an attack the player did not chain into. That is
        // one unwanted swing -- and it was ALREADY the behaviour for every state exit after
        // spellfire, five for five. This makes one rule out of two rather
        // than adding a case.
        //
        // WHERE THIS ARRIVES FROM CHANGED, NOT WHAT IT DECIDES. It used to be reached by
        // inferring the end from `inRdy` -- which is why it was called `kReturnedToReady` -- and
        // that inference was measured **910 ms late** on a jump, blaming the landing for an end
        // that `SBF_ShoutStop` had already announced before the player left the ground.
        //
        // WHAT IS NOT PROVEN: an interruption arriving with a
        // RESOLVED press buffered fires the attack. Removing the pre-spellfire split
        // widened the interval it happens in; it did not settle it. An
        // UNRESOLVED press is safe either way -- nothing fires at the interruption, the player's own
        // release decides.
        void EndShoutLocked(const Settings& a_settings, ShoutEnd a_why, std::string_view a_reason,
                            Emit& a_emit) {
            // Cleared before the early return, and unconditionally: an interruption that leaves the
            // engine with nothing to say is still the end of the shout as far as the trace is
            // concerned. The `shoutStop` path clears it in `Observe` too (ahead of the enabled
            // gate, so it is right with the engine off); this covers the ready-state interruption,
            // and `FireChainLocked` covers the engine's own cut.
            g_shoutLiveForTrace = false;

            // LIFTED BEFORE THE EARLY RETURN BELOW, and that placement is the whole
            // point. This function returns without doing anything when no shout is active, and a
            // root that outlived its shout is exactly the state where that would be true. Clearing
            // first means every reason a shout can end -- natural `shoutStop`, `SBF_ShoutStop`, the
            // liveness cap -- lifts the root through one line rather than three.
            ClearRootLockLocked(a_emit);

            // WAS `if (!shoutActive && !pressPending) return;`, AND THE CHANGE IS LOAD-BEARING.
            //
            // A press is now allowed to outlive its shout, so `pressPending` with no live shout is
            // the NORMAL state after a hand-back rather than a leak to mop up. Under the old guard
            // every later `inRdy` -- and the graph raises one right after a shout ends -- would
            // have re-entered here, re-announced the end of a shout that already ended, and
            // re-decided a press that is deliberately still waiting. Ending a shout now requires a
            // shout. The old second clause was unreachable in any case: before the hand-back
            // `pressPending` could only be set while `shoutActive` was true.
            if (!g_state.shoutActive) return;

            // Read before the teardown below clears it. It DECIDES nothing -- it
            // reports, and that is worth keeping rather than deleting: the trace lines below are
            // the only place a reader can tell a press handed back from a finished shout from one
            // handed back out of a shout that was cancelled before its magic ever fired.
            const bool delivered = g_state.spellFired;

            g_state.shoutActive = false;
            g_state.windowOpen = false;
            g_state.spellFired = false;
            // This is the "may this shout still be measured" flag, and every caller of
            // this function is a reason it may NOT be: the liveness cap, a state exit, a game
            // load. Only the natural `shoutStop` branch records, and it records BEFORE it calls
            // here. Clearing it in one place beats enumerating the reasons at the record site.
            g_state.spellFiredAtMs = 0.0;
            g_state.teardownReadyPending = false;

            // ARM at confirmed `shoutStop`, DELIVER after a short delay. Same-frame
            // `inRdy` / `IdleStop` ride the shout's own exit bundle and a synthetic tap there is
            // silently refused (down+up, no `BeginCastVoice`). A later ready pass works; so does a
            // paced delay from the arm. ADR-0007 allows the ready pass; the arm stamps the gate.
            if (ShoutInputHook::IsHoldingBehindShoutLocked()) {
                if (a_why != ShoutEnd::kAbandoned) {
                    g_releaseShoutChainOnReady = true;
                    g_releaseShoutChainArmedAtMs = ElapsedMs();
                    // 300ms: a real second shout works ~266ms after `shoutStop`;
                    // 150ms synthetic taps still
                    // completed with no `BeginCastVoice`. Measured on `0476F6F5`.
                    a_emit.scheduleShoutChainRelease = true;
                    a_emit.scheduleShoutChainReleaseInMs = 300.0;
                    a_emit.scheduleShoutChainReleaseReason = "shoutStop+delay"sv;
                    SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT→SHOUT armed at {} -- release in 300ms",
                          ElapsedMs(), a_reason);
                } else {
                    // The same correction as the cut site: the shout ended
                    // `kAbandoned`, so the confirmed `shoutStop` a deferred driver intent is
                    // waiting for is never coming. End the intent here rather than leaving it to
                    // the watchdog, which would answer late and blame itself.
                    g_releaseShoutChainOnReady = false;
                    ShoutInputHook::AbandonSlotLocked(a_reason);
                }
            }

            if (!g_state.pressPending) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT end, no press waiting ({})", ElapsedMs(), a_reason);
                return;
            }

            // One term, and the enum is now carrying the whole decision: the liveness cap
            // is the only ending that is not the player's.
            const bool playerFacing = a_why != ShoutEnd::kAbandoned;

            if (!playerFacing) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT end ({}) -- PRESS dropped, the shout was abandoned, "
                      "not finished, and the press is seconds old", ElapsedMs(), a_reason);
                g_state.pressPending = false;
                g_state.pressResolved = false;
                // `pressOwned` is deliberately NOT cleared. The button may still be physically
                // down, and ownership is what keeps its follow-ups from reaching the game's own
                // handler as a release with no press.
                //
                // IT ENDS AT THE PLAYER'S RELEASE, AND ONLY THERE. `WatchdogReleasePressLocked`'s
                // cap is a backstop, NOT the second half of that sentence -- a cap firing in a
                // trace means this path failed. An earlier draft of this
                // comment named the cap as one of the two normal ways ownership ends, which is
                // citing a backstop as authority for the thing it exists to catch.
                return;
            }

            if (g_state.pressResolved) {
                // A PRESS THE PLAYER MADE AND DID NOT GET IS THE DEFECT CLASS THIS PROJECT KEEPS
                // FIGHTING, and this is the hand-back, now reached from every player-facing
                // exit rather than from `shoutStop` alone.
                //
                // Before the late window it could not happen at all: the window opened ~0.1 s into the
                // exhale, so a buffered press had essentially always fired by `shoutStop`. Moving
                // the window late made the miss reachable -- a cached tail longer than this
                // particular clip, or `iChainWindowPct = 0`. Word count is in
                // the key, but the key still cannot distinguish everything: the >15% divergence
                // line in `RecordNaturalTailLocked` exists precisely to report a cause none of the
                // keyed fields covers, and a press must not vanish the first time one shows up.
                SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT end ({}) -- handing the resolved press back{}", ElapsedMs(),
                      a_reason,
                      delivered ? ""sv
                                : " (cancelled before spellfire -- the shout delivered nothing, the "
                                  "press is still the player's)"sv);
                // `shoutActive` is already false above, so this takes the plain-attack branch.
                TryFireChainLocked(a_settings, a_emit);
                return;
            }

            // A press deliberately outliving its shout has to be VISIBLE, or the next session reads
            // it as the engine hanging on to input. Its counterpart when it finally fires is
            // `TryFireChainLocked`'s existing ">>> ATTACK without a cut ... the shout had already
            // ended" line, which already says the right thing; the two bracket the wait.
            SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS survives the shout end ({}{}) -- unresolved, waiting for the "
                  "player to release or hold it to power", ElapsedMs(), a_reason,
                  delivered ? ""sv : ", cancelled before spellfire"sv);
        }

        // Step two. The reset the cut provokes arrives as a PIE payload just *before* `inRdy`,
        // so ordering against this event -- not against a clock -- is what makes
        // the write survive.
        void OnReadyLocked(const Settings& a_settings, Emit& a_emit) {
            if (!g_state.awaitingReady) {
                // `inRdy` DOES fire inside a shout, in exactly one case: the shout began while an
                // MCO attack was live, and the graph tears that attack down on its way into the
                // exhale -- MCO_AttackExitNotify, attackStop, tailcombatState, inRdy, all within
                // ~3ms of BeginCastVoice and all *before* Voice_SpellFire_Event.
                // That pass is the outgoing attack's, not the shout's, so spend the debt and
                // leave the shout live. This is the whole combo -> shout -> chain case, so
                // treating it as an interruption killed the feature the mod exists for.
                if (g_state.shoutActive && !g_state.spellFired && g_state.teardownReadyPending) {
                    g_state.teardownReadyPending = false;
                    SHOUTMCO_TRACE("[{:10.2f}] >>> READY pass belongs to the attack the shout cut short; "
                          "the shout is still live", ElapsedMs());
                    return;
                }
                // A READY PASS NO LONGER ENDS A SHOUT, AND DELETING THAT IS THE FIX.
                //
                // `EndShoutLocked(kReturnedToReady, "graph returned to ready")` used to sit here. It was an
                // INFERENCE: `inRdy` under a live shout was read as "something must have ended the
                // shout". The inference is wrong in both directions and the SBF drive measured
                // both.
                //
                //   TOO LATE. On the jump trial the graph left the shout state at `SBF_ShoutStop`,
                //   78 ms BEFORE the player even left the ground, and this line did not fire until
                //   the landing's `inRdy` **910 ms later**. The engine held a shout that was over,
                //   swallowing every attack press behind it for most of a second, and then stamped
                //   the end on the landing -- which is why three sessions read the landing as the
                //   cause. It was not the cause. It was the first event the engine happened to
                //   understand.
                //
                //   AND WRONG IN KIND. `inRdy` is the graph saying it is ready, not the graph
                //   saying the shout is over. Every attempt to recover the second meaning from the
                //   first needed a discriminator, and all three candidates failed: `g_mcoAttackEnding`
                //   is true on nearly every `inRdy` (it is set by `attackStop`, which is part of the
                //   generic return-to-ready bundle, NOT an MCO marker), so that escape
                //   degenerated to "never end a shout" and was reverted before commit; a second
                //   `teardownReadyPending` armed at `BeginCastVoice` aims at a teardown that is not
                //   there; and keying on the jump events themselves would tie this engine to whether
                //   **Jumping Attack** is installed, which `AGENTS.md` forbids.
                //
                // The shout now ends at `SBF_ShoutStop`, which states the fact directly and fires on
                // every ending -- see the `kStateExit` branch in `Observe`. There is nothing left
                // for this branch to decide, so it decides nothing and says so.
                //
                // THE DEBT ABOVE IS NOW DIAGNOSTIC RATHER THAN PROTECTIVE, and that is worth stating
                // instead of deleting. `teardownReadyPending` existed to stop the teardown's own
                // `inRdy` from being mistaken for an interruption; no `inRdy` can be
                // mistaken for anything now. It is kept because "this shout began out of an attack"
                // is the marker the combo -> shout -> chain traces are read against, and its trace
                // line is the only place that fact appears.
                if (g_state.shoutActive) {
                    SHOUTMCO_TRACE("[{:10.2f}] >>> READY pass under a live shout -- ignored. A shout ends "
                          "at `SBF_ShoutStop`, not at `inRdy`", ElapsedMs());
                }
                return;
            }

            const auto since = ElapsedMs() - g_state.cutAtMs;
            g_state.awaitingReady = false;

            if (since > static_cast<double>(a_settings.readyWindowMs)) {
                SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN dropped: `inRdy` came {:.1f}ms after the cut (cap {}ms)",
                      ElapsedMs(), since, a_settings.readyWindowMs);
                return;
            }

            SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN ready {:.1f}ms after the cut", ElapsedMs(), since);
            ResumeAndAttackLocked(a_settings, g_state.pendingKind, a_emit);
        }

        // Arm the motion profile for a chained swing. Called from the fire task WITHOUT the lock;
        // decision inside, position read outside.
        void ArmMotionWatch(RE::Actor* a_actor) {
            const auto pos = a_actor->GetPosition();
            {
                std::scoped_lock lock(detail::g_engineLock);
                g_state.motionWatch = true;
                g_state.motionArmedAtMs = ElapsedMs();
                g_state.motionOrigin = pos;
                g_state.motionPeakVelocity = 0.0f;
                // The hint store sits INSIDE the lock, unlike every other atomic here, because
                // its only job is to mirror `motionWatch` -- and mirrored state needs a total
                // order with the thing it mirrors. Stored after the unlock, this thread could be
                // preempted while a graph thread ends the watch (state false, hint false), and
                // the late store would then wedge the hint true with the watch off -- every
                // event paying for motion reads nothing consumes until the next chain re-arms.
                g_motionWatchLive.store(true, std::memory_order_relaxed);
            }
        }

        // Defined below `ExecuteEmits` because it calls it, and called from it. The pacer's task
        // has to run the emissions its own decision produces, exactly as `Observe` does.
        void PostWindowOpen(double a_delayMs, std::uint32_t a_generation);
        void PostAdvancePoll(double a_delayMs, std::uint32_t a_generation);
        void PostShoutChainRelease(double a_delayMs, std::string_view a_reason);

        // Phase C: everything one event decided to emit, in the order the old inline code emitted
        // it -- the release ahead of the restore, the graph emissions last.
        void ExecuteEmits(const Emit& a_emit, RE::Actor* a_actor) {
            if (a_emit.scheduleWindowOpen) {
                PostWindowOpen(a_emit.scheduleWindowInMs, a_emit.scheduleGeneration);
            }
            // Ahead of the cancel below, so a tick that decided to keep waiting is on
            // its way before anything else in this emission runs. It never coexists with
            // `cancelRecovery` in one `Emit` -- the poll either re-arms or cuts.
            if (a_emit.scheduleAdvancePoll) {
                PostAdvancePoll(a_emit.scheduleAdvancePollInMs, a_emit.scheduleAdvanceGeneration);
            }
            if (a_emit.scheduleShoutChainRelease) {
                PostShoutChainRelease(a_emit.scheduleShoutChainReleaseInMs,
                                      a_emit.scheduleShoutChainReleaseReason);
            }
            if (a_emit.releaseHeld) {
                ShoutInputHook::ReleaseHeldShout(a_emit.releaseReason);
            }
            // THE ROOT, AHEAD OF EVERY OTHER GRAPH EMISSION.
            //
            // First of the GRAPH emissions -- ahead of the cancel, the cut and the fire. The two
            // blocks above it are not graph work: they schedule or hand back input. Setting the lock early is what gets it in
            // before the graph can act on a `moveStart`; clearing it early is what keeps an unroot
            // from lagging the cut it accompanies, so the player never steers less than they should
            // for the MCO attack the chain just handed them.
            //
            // ONE TASK, WRITE THEN RAISE, and the order is the mechanism. `moveStop` takes the
            // graph out of locomotion; the movement controller puts it straight back on its next
            // update while the key is held, and only the lock stops that. Raise before the write
            // and the graph leaves locomotion into a gate that is still open, so it walks back in.
            // Keeping both in one task body makes the order a property of the code rather than of
            // two tasks' drain order.
            if (a_emit.setLock) {
                Defer(a_actor, [value = a_emit.lockValue, raise = a_emit.raiseRoot,
                                verify = a_emit.verifyLock](RE::Actor* actor) {
                    actor->SetGraphVariableInt(kRootLockVariable, value);

                    // READ IT BACK, BECAUSE THE WRITE'S OWN ANSWER IS NOT ONE.
                    //
                    // Writing an UNDECLARED graph variable
                    // silently succeeds and reads back 0. So a user who installed the DLL and
                    // never ticked `shmco` in Nemesis gets a graph with no `SHOUT_lock` in it, no
                    // error anywhere, and no rooting -- with nothing in the log to say why. The
                    // readback is the only thing that can tell that apart from a working patch.
                    //
                    // `log::warn` rather than a trace: it is the one
                    // diagnostic a user who has never turned tracing on still needs.
                    if (verify) {
                        const int readback = GraphInt(actor, kRootLockVariable);

                        // TRACED WHETHER IT PASSES OR FAILS, and the passing case is the one worth
                        // arguing for. A check that only speaks up when it fails cannot be told
                        // apart from a check that never ran: the acceptance claim becomes "the log
                        // is silent", which is an assertion, not an instrument -- the error this
                        // file already carries two warnings about. One line per session.
                        SHOUTMCO_TRACE("[{:10.2f}] >>> ROOT verify: wrote {}={}, read back {} -- {}",
                              ElapsedMs(), kRootLockVariable, value, readback,
                              readback == value ? "the behaviour patch is present"sv
                                                : "the behaviour patch is ABSENT"sv);

                        if (readback != value) {
                            // The name here is what NEMESIS DISPLAYS, which is `info.ini`'s
                            // `name=`, not the `shmco` folder code. The code appears nowhere the
                            // user can see, so naming it would send them looking for a line that
                            // is not in the list. `docs/release/README.txt` step 2 says the same
                            // words, deliberately.
                            log::warn(
                                "[ShoutMCO] `{}` read back {} after writing {} -- the behaviour "
                                "patch looks ABSENT, so shouts will NOT root you in place. "
                                "Everything else this mod does still works. To turn rooting on, "
                                "tick \"Shouts for MCO\" in Nemesis, then press Update Engine and "
                                "Launch Nemesis Behavior Engine.",
                                kRootLockVariable, readback, value);
                        }
                    }

                    if (raise) {
                        // `accepted=false` HERE IS A FAULT, and that is a REVERSAL worth stating.
                        //
                        // While this was `SHOUT_root` it came back false 6/6 and the comment here
                        // said so, because a mod-declared event is not deliverable by name.
                        // `moveStop` is vanilla and measured `accepted=true`
                        // with the graph in locomotion. So a false in a trace is now
                        // route 2 failing to trigger, not the known-inert case -- do not read it as
                        // expected.
                        //
                        // It is legitimately false when the graph is ALREADY in `ShoutStanding`:
                        // there is no locomotion state to leave. A still shout is the ordinary case
                        // for that, which is why the standing/moving posture belongs in any note
                        // taken from this line.
                        //
                        // SENT ONLY WITH THE LOCK GOING UP, never with it coming down. Raising it
                        // on the clear would tell the graph the player stopped moving at the exact
                        // moment they are getting steering back, which is the opposite of the job.
                        // `raiseRoot` is set in `Observe`'s `isShoutStateEntry` branch -- route 2,
                        // and NOT in `BeginShoutLocked`, which writes only the lock -- and it is
                        // explicitly cleared in `ClearRootLockLocked`, so that is already true;
                        // this comment is why.
                        // OUR OWN emission, marked as such so the power-attack seam on
                    // `NotifyAnimationGraph` forwards it instead of reading the replay back as a
                    // fresh press. Outside the engine lock, as every emission here is.
                    const bool accepted = [&] {
                        ScopedOwnEmit own;
                        return actor->NotifyAnimationGraph(kLeaveLocomotionEvent);
                    }();
                        SHOUTMCO_TRACE("[{:10.2f}] >>> ROOT wrote {}={} and fired '{}' accepted={}",
                              ElapsedMs(), kRootLockVariable, value, kLeaveLocomotionEvent, accepted);
                    } else {
                        SHOUTMCO_TRACE("[{:10.2f}] >>> ROOT wrote {}={}", ElapsedMs(),
                              kRootLockVariable, value);
                    }
                });
            }

            if (a_emit.cancelRecovery) {
                // Deferred like every other emission: never call into the graph from inside its
                // own event dispatch.
                Defer(a_actor, [](RE::Actor* actor) {
                    // OUR OWN emission, marked as such so the power-attack seam on
                    // `NotifyAnimationGraph` forwards it instead of reading the replay back as a
                    // fresh press. Outside the engine lock, as every emission here is.
                    const bool accepted = [&] {
                        ScopedOwnEmit own;
                        return actor->NotifyAnimationGraph("attackStop");
                    }();
                    SHOUTMCO_TRACE("[{:10.2f}] >>> CANCEL fired 'attackStop' accepted={}  || {}", ElapsedMs(),
                          accepted, GraphSummary(actor));
                });
            }

            if (a_emit.cut) {
                Defer(a_actor, [cut = a_emit.cutEvent](RE::Actor* actor) {
                    // OUR OWN emission, marked as such so the power-attack seam on
                    // `NotifyAnimationGraph` forwards it instead of reading the replay back as a
                    // fresh press. Outside the engine lock, as every emission here is.
                    const bool accepted = [&] {
                        ScopedOwnEmit own;
                        return actor->NotifyAnimationGraph(cut);
                    }();
                    SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN cut '{}' accepted={}  || {}", ElapsedMs(), cut,
                          accepted, GraphSummary(actor));
                });
            }

            if (a_emit.fire) {
                const auto  kind = a_emit.fireKind;
                const auto* var = a_emit.fireVar;
                const int   index = a_emit.fireIndex;
                const bool  resume = a_emit.fireResume;
                // Movement is checked inside the task, not here: the actor can start or stop
                // moving in the ~7ms before it drains, and the graph's state at the moment the
                // attack goes out is what matters.
                Defer(a_actor, [var, index, resume, kind, fromSprint = a_emit.fireFromSprint,
                                evt = a_emit.fireEvent](RE::Actor* actor) {
                    if (resume) {
                        actor->SetGraphVariableInt(var, index);
                        SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN wrote {}={}  || {}", ElapsedMs(), var, index,
                              GraphSummary(actor));
                    }

                    // The player's INPUT, not the animation's flags: the shout is not a locomotive
                    // state, so `actorState1.moving*` reads false with a key still held.
                    // Logged on every chain, moving or not, because it is the
                    // reading the chain was settled on and the two it replaced were both silently blind.
                    const bool moving = HasMovementInput() || IsMoving(actor);
                    SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN movement at the attack edge: {}  || {}", ElapsedMs(),
                          moving ? "moving"sv : "still"sv, MovementSummary(actor));

                    // A chain out of a shout that BEGAN from a sprint takes the sprint
                    // entry events, provided the player is still asking to move -- a sprint lunge
                    // from a standstill would look wrong, so a released stick falls back to the
                    // in-place events. The sprint fact is the ARM's capture, not a read here: the
                    // graph's own sprint state at this edge answers for whatever locomotion
                    // resumed to, which is how a real-input drive got plain attacks
                    // while an auto-move fixture got sprint routing.
                    std::string fireEvt = evt;
                    if (fromSprint) {
                        if (moving) {
                            fireEvt = SprintAttackEvent(actor, kind);
                            // The REASON is not restated here, deliberately: two of them arm this
                            // branch now (a whirlwind, or a sprint at the press) and the arm line
                            // already named which. Repeating one of them here printed "shout began
                            // from a sprint" over a whirlwind chain that began standing still --
                            // a trace that contradicts the trace six lines above it.
                            SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN sprint entry: '{}' replaces '{}' (still moving "
                                  "at the fire edge; see the arm line for why)",
                                  ElapsedMs(), fireEvt, evt);
                        } else {
                            SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN sprint entry declined -- shout began from a "
                                  "sprint but no movement input at the fire edge; '{}' stands",
                                  ElapsedMs(), evt);
                        }
                    }

                    // No `moveStop` is sent here, deliberately. It used to be, as a stopgap for a
                    // moving power attack that played a run cycle instead of a swing -- but that
                    // belonged to one moveset, not to this engine: it reproduced with no shout in
                    // the path at all and vanished when the moveset was disabled.
                    // Sending `moveStop` made our chain differ from the game's own moving power
                    // attack rather than agree with it, which is the wrong direction for a chain
                    // whose whole job is to hand control back to MCO. Nothing roots here either:
                    // the control-map suppression this branch used to make is gone,
                    // and the graph-side root belongs to the shout state, not to the chained swing.

                    // OUR OWN emission, marked as such so the power-attack seam on
                    // `NotifyAnimationGraph` forwards it instead of reading the replay back as a
                    // fresh press. Outside the engine lock, as every emission here is.
                    const bool accepted = [&] {
                        ScopedOwnEmit own;
                        return actor->NotifyAnimationGraph(fireEvt);
                    }();
                    SHOUTMCO_TRACE("[{:10.2f}] >>> CHAIN fired '{}' ({}) accepted={}  || {}", ElapsedMs(), fireEvt,
                          Describe(kind), accepted, GraphSummary(actor));

                    // Armed after the event, so the origin is where the swing starts from.
                    ArmMotionWatch(actor);
                    if (TraceEnabled()) {
                        const auto reading = ReadMotion(actor);
                        SHOUTMCO_TRACE("[{:10.2f}] >>> MOTION armed  || {}", ElapsedMs(),
                              FormatMotion(reading, 0.0f));
                    }
                });
            }
        }

        // THE PACED WINDOW OPEN. Copied in shape from `ShoutInputHook::PostReplayUp`,
        // deliberately and not incidentally -- a detached thread that sleeps and ends in an
        // `AddTask`, with a generation checked inside the task so stale work is dropped.
        //
        // WALL TIME, NOT FRAMES, for the reason `PostReplayUp` already documents: the requirement
        // is a duration measured against an animation, and a frame-counted wait is a different
        // duration on different hardware.
        //
        // NOT RIDDEN ON GRAPH EVENTS, which is the other obvious implementation and is wrong here.
        // A still character stops producing graph events entirely, and
        // both existing watchdogs are limited by exactly that. A shout is a moment where the player
        // may very well be standing still, so an event-driven open would simply never arrive for
        // the case it most needs to serve.
        //
        // One thread per scheduled open, living at most one exhale. The alternative -- a standing
        // worker with a queue -- is more shared state than the thing it schedules, and is asleep
        // across every quit rather than across the one it might overlap.
        void PostWindowOpen(double a_delayMs, std::uint32_t a_generation) {
            const auto delay = std::chrono::milliseconds(static_cast<long long>(a_delayMs));

            std::thread([delay, a_generation]() {
                std::this_thread::sleep_for(delay);

                auto* task = SKSE::GetTaskInterface();
                if (!task) {
                    // The window simply never opens for this shout, and a buffered press then
                    // rides the natural end. Worth an error rather than a silent return: it is
                    // the feature not happening, and nothing else in the log would say so.
                    log::error("[ShoutMCO] no task interface for the paced window open -- the chain "
                               "window will not open for this shout");
                    return;
                }

                task->AddTask([a_generation]() {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!player) return;

                    const auto settings = Settings::Snapshot();
                    Emit       emit;
                    {
                        std::scoped_lock lock(detail::g_engineLock);
                        if (g_shoutGeneration.load(std::memory_order_relaxed) != a_generation) {
                            SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW paced open dropped -- it belongs to an "
                                  "earlier shout", ElapsedMs());
                            return;
                        }
                        // `OpenWindowLocked` returns on its own if the shout is already over, so a
                        // shout that ended early needs no separate check here.
                        OpenWindowLocked(*settings, "paced -- the measured end minus the window"sv, emit);
                    }
                    // Outside the lock, like every other emission. This one can fire the chain,
                    // which cuts the shout and sends an attack.
                    ExecuteEmits(emit, player);
                });
            }).detach();
        }

        // One tick of the deferred cut, in the shape `PostWindowOpen` established: a
        // detached thread that sleeps and ends in an `AddTask`, with a generation checked inside
        // the task so stale work is dropped.
        //
        // ONE THREAD PER TICK, NOT ONE PER WAIT. It is the same trade `PostWindowOpen` takes and
        // the numbers are smaller here -- at most `powerAdvanceWaitMs / 16` ticks, each asleep for
        // a frame, and only while a power attack has a shout queued behind it. A standing worker
        // would be more shared state than the thing it schedules.
        //
        // THE GENERATION IS CHECKED AGAINST THE ATTACK, NOT THE SHOUT. A poll that drains after the
        // player has begun a different attack must not cut that one, and `g_shoutGeneration` cannot
        // see the difference -- both attacks can sit inside one shout's queue.
        void PostAdvancePoll(double a_delayMs, std::uint32_t a_generation) {
            const auto delay = std::chrono::milliseconds(static_cast<long long>(a_delayMs));

            std::thread([delay, a_generation]() {
                std::this_thread::sleep_for(delay);

                auto* task = SKSE::GetTaskInterface();
                if (!task) {
                    // The wait simply never ends on its own. That is not a hang: the attack's own
                    // teardown or `inRdy` clears the arm and the queued shout takes the natural
                    // path, which is the hit-gate behaviour. Still an error -- the feature stopped
                    // happening and nothing else in the log would say so.
                    log::error("[ShoutMCO] no task interface for the power-advance poll -- the cut "
                               "will not wait for MCO's combo advance");
                    return;
                }

                task->AddTask([a_generation]() {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!player) return;

                    // Phase A: the game call, before the lock. Cheaper to take it unconditionally
                    // than to take the lock twice, and it is four graph reads on a path that only
                    // runs mid-power-attack.
                    const SampledCombo live{.v = ShoutChainEngine::SampleCombo(player), .have = true};

                    const auto settings = Settings::Snapshot();
                    Emit       emit;
                    {
                        std::scoped_lock lock(detail::g_engineLock);
                        if (g_attackGeneration.load(std::memory_order_relaxed) != a_generation) {
                            SHOUTMCO_TRACE("[{:10.2f}] >>> ADVANCE poll dropped -- it belongs to an "
                                  "earlier attack", ElapsedMs());
                            return;
                        }
                        OnAdvancePollLocked(live, *settings, emit);
                    }
                    // Outside the lock, like every other emission. This one can send `attackStop`.
                    ExecuteEmits(emit, player);
                });
            }).detach();
        }

        void PostShoutChainRelease(double a_delayMs, std::string_view a_reason) {
            const auto delay = std::chrono::milliseconds(static_cast<long long>(a_delayMs));
            const auto reason = std::string{a_reason};

            std::thread([delay, reason]() {
                std::this_thread::sleep_for(delay);

                auto* task = SKSE::GetTaskInterface();
                if (!task) {
                    log::error("[ShoutMCO] no task interface for shout→shout release -- the queued "
                               "shout will not replay");
                    return;
                }

                task->AddTask([reason]() {
                    Emit emit;
                    {
                        std::scoped_lock lock(detail::g_engineLock);
                        if (!g_releaseShoutChainOnReady ||
                            !ShoutInputHook::IsHoldingBehindShoutLocked()) {
                            return;
                        }
                        g_releaseShoutChainOnReady = false;
                        emit.releaseHeld = true;
                        emit.releaseReason = reason;
                    }
                    if (emit.releaseHeld) {
                        ShoutInputHook::ReleaseHeldShout(emit.releaseReason);
                    }
                });
            }).detach();
        }
    }

    void ShoutChainEngine::Install() {
        Settings::Load();
        Settings::Snapshot()->Log();

        // Player only -- see CONTEXT.md, surface decision. The SINK below is reusable: nothing in
        // it reads the player specifically. THE STATE IT FEEDS IS NOT. The chain state, the single
        // global cast-intent slot and the held-press ownership are one-actor globals, so adding
        // RE::VTABLE_Character[2] would push several actors through state that assumes one. NPC
        // support is a state-model change, not a registration change.
        REL::Relocation<std::uintptr_t> vtblPC{RE::VTABLE_PlayerCharacter[2]};
        _originalPC = vtblPC.write_vfunc(0x1, ProcessEvent_PC);

        // A load carries no graph events across, so state left over from the session before it
        // -- an armed chain, an owned button -- would never be cleared by anything else.
        if (auto* messaging = SKSE::GetMessagingInterface()) {
            messaging->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
                if (!a_message) return;
                switch (a_message->type) {
                    case SKSE::MessagingInterface::kPreLoadGame:
                    case SKSE::MessagingInterface::kPostLoadGame:
                    case SKSE::MessagingInterface::kNewGame:
                        // THE ROOT LOCK, and this clear is UNCONDITIONAL where every
                        // other one is guarded on `rootLocked`.
                        //
                        // A load is the one moment the engine's belief about the graph cannot be
                        // trusted, exactly as the comment above says of the physical button state.
                        // The guard exists to save a task on paths that never rooted; here the
                        // cost of being wrong is a player who cannot steer, so the task is paid
                        // for every load. Loading a save rebuilds the behaviour graph and the
                        // variable returns to its declared default anyway -- this makes that a
                        // belt-and-braces fact rather than something the root depends on.
                        if (auto* loadPlayer = RE::PlayerCharacter::GetSingleton()) {
                            Defer(loadPlayer, [](RE::Actor* actor) {
                                actor->SetGraphVariableInt(kRootLockVariable, 0);
                                SHOUTMCO_TRACE("[{:10.2f}] >>> ROOT wrote {}=0 (game load)", ElapsedMs(),
                                      kRootLockVariable);
                            });
                        }
                        // The next session gets its own root-lock check: a load can change the
                        // behaviour graph, so what was verified for the last one says nothing.
                        g_rootVerified.store(false, std::memory_order_relaxed);
                        // A timestamp from the last session would arm the sprint entry
                        // for the first shout of this one.
                        g_lastSprintingMs.store(-1.0e9, std::memory_order_relaxed);

                        {
                            std::scoped_lock lock(detail::g_engineLock);
                            // A LOAD IS ABANDONMENT, and that is worth saying
                            // rather than inheriting. A press is now allowed to outlive its shout,
                            // so "whatever `Reset()` happens to clear" stopped being an adequate
                            // account of what happens to one. `Reset()` clears `pressPending`,
                            // `pressResolved` AND `pressOwned`, which is the right answer here and
                            // differs from every other abandonment path: a load is the one moment
                            // the physical button state cannot be reasoned about at all, so
                            // ownership goes with the rest instead of waiting for a release that
                            // belongs to a session that no longer exists.
                            g_state.Reset();
                            // `Reset()` carries `rootLocked` across on purpose, so the
                            // one path that genuinely knows the root is gone has to say so itself.
                            // The graph write is above, outside the lock, where every game call
                            // belongs.
                            g_state.rootLocked = false;
                            // The MCO tracking and the queued snapshot live OUTSIDE ChainState
                            // and were never cleared here -- so a load used to carry a "live"
                            // attack, a valid combo snapshot and a held press
                            // into a session none of them belong to.
                            g_mcoAttackLive = false;
                            g_mcoAttackEnding = false;
                            g_mcoSwingLanded = false;
                            g_cancelSent = false;
                            // The generation bump is the load-bearing half. Clearing
                            // the flag stops a poll already asleep on its thread from acting; the
                            // bump stops one that wakes into the NEXT session's first attack and
                            // finds the flag set again by it.
                            g_mcoPowerAttack = false;
                            g_powerAtInitiate = 0;
                            g_advanceWaitArmed = false;
                            g_advanceWaitArmedAtMs = 0.0;
                            g_attackGeneration.fetch_add(1, std::memory_order_relaxed);
                            g_releaseShoutChainOnReady = false;
                            g_releaseShoutChainArmedAtMs = 0.0;
                            // The cache describes the animation pack the last
                            // session was running, and a load can change it -- so it goes, along
                            // with everything else this path clears. The generation bump drops any
                            // paced open still asleep on its thread, the same job
                            // `InvalidateReplays` does below for a sleeping shout release.
                            g_windowCache = {};
                            g_windowCacheNext = 0;
                            g_shoutGeneration.fetch_add(1, std::memory_order_relaxed);
                            // Both halves of the driver-cast discriminator, cleared for
                            // the reason the comment above gives: they live outside `ChainState`,
                            // so nothing else here would clear them, and a load carrying a token
                            // into a session it does not belong to is exactly the class of bug
                            // that comment was written about.
                            //
                            // Each stale token would cost at most one wrong answer on the first
                            // shout-state entry after the load, in the safe direction (a begin
                            // token suppresses one arm; a ready token can only permit an arm the
                            // begin token has already declined to veto). Cleared anyway -- the
                            // convention is what stops the next person having to redo that
                            // reasoning for the next flag.
                            g_beginSeenPending = false;
                            g_readyExitPending = false;
                            AbandonQueuedResumeLockedImpl("game load"sv);
                            ShoutInputHook::ClearHeldOnLoadLocked();
                        }
                        // Same reasoning as the reset above, for the one piece of engine state that
                        // does not live under the lock: a synthetic shout release sleeping on its
                        // thread would otherwise wake up in the loaded session and deliver there.
                        ShoutInputHook::InvalidateReplays();
                        // A SECOND RELOAD POINT, and it earns its place when shouting itself is
                        // what has broken.
                        //
                        // Settings are otherwise re-read only at `BeginCastVoice`, so every INI
                        // edit reaches the engine through a shout. That is exactly unreachable
                        // when whatever has gone wrong stops a shout from starting -- the player
                        // edits the file and nothing they can do makes it take. Reloading on a
                        // load makes "edit it and reload your save" always work, with no restart.
                        Settings::Load();
                        break;
                    default:
                        break;
                }
            });
        }

        log::info("[ShoutMCO] anim-event hook installed");
    }

    ShoutChainEngine::EventResult ShoutChainEngine::ProcessEvent_PC(
        RE::BSTEventSink<RE::BSAnimationGraphEvent>*   a_sink,
        RE::BSAnimationGraphEvent*                     a_event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource) {
        // Player-only: this vfunc is `VTABLE_PlayerCharacter[2]`. NPCs keep vanilla
        // 0.100 s generator fire because they never enter this hook. Returning `kContinue`
        // without calling the original keeps other graph sinks on the event and withholds it
        // from TESObjectREFR's shout handler -- intercept before the shout system, not a
        // broadcast stop.
        if (!Observe(a_event)) {
            return EventResult::kContinue;
        }
        return _originalPC(a_sink, a_event, a_eventSource);
    }

    bool ShoutChainEngine::IsNoise(std::string_view a_tag) {
        return a_tag == "SCAR_UpdateDummy"sv || a_tag == "RdyDummy"sv;
    }

    bool ShoutChainEngine::IsInteresting(std::string_view a_tag) {
        // `inRdy` is the attack-queued release tag and was traced without GraphSummary.
        if (a_tag == "inRdy"sv) return true;
        static constexpr std::string_view kNeedles[]{
            "hout"sv,    // shoutRelease, shoutStop, ShoutSprint*, shoutReleaseSlowTime
            "SHOUT_"sv,  // our own vocabulary, once the C3 patch declares it
            "Breath"sv,  // MT_BreathExhale*, CombatReady_BreathExhale*
            "Exhale"sv,  // CombatReady_ShoutExhaleMedium
            "Voice"sv,   // Voice_SpellFire_Event, BeginCastVoice
            "MCO_"sv,    // every MCO window / recovery / transition event
            "attack"sv,  // attackStart, attackStop
            "Attack"sv,  // AttackWinStart, MCO_AttackInitiate
            "CastOK"sv,  // CastOKStart / CastOKStop
            "Pie"sv,     // Payload Interpreter -- the live tag is "Pie", not "PIE"
            "ocomotion"sv,  // tailCombatLocomotion -- where the running bug lives
            "move"sv,       // moveStart / moveStop
            "Move"sv,
        };
        for (const auto needle : kNeedles) {
            if (a_tag.find(needle) != std::string_view::npos) return true;
        }
        return false;
    }

    // The engine's main entry point, structured as the three phases EngineLock.h names: game
    // reads first, one locked decision block, emissions last. The tag decides most of what phase
    // A must sample; the two hints cover the reads whose need is itself state.
    bool ShoutChainEngine::Observe(RE::BSAnimationGraphEvent* a_event) {
        if (!a_event || !a_event->holder) return true;

        const char* rawTag = a_event->tag.c_str();
        if (!rawTag || !rawTag[0]) return true;
        const std::string_view tag{rawTag};
        if (IsNoise(tag)) return true;

        // The one fact the missing-SBF diagnostic needs, taken from the tag alone.
        if (!g_sbfEverSeen.load(std::memory_order_relaxed) && tag.starts_with("SBF_"sv)) {
            g_sbfEverSeen.store(true, std::memory_order_relaxed);
        }

        auto* holder = const_cast<RE::TESObjectREFR*>(a_event->holder);
        auto* actor = holder ? holder->As<RE::Actor>() : nullptr;
        if (!actor) return true;

        // Registered here rather than only at shout start, because the control that says whether a
        // symptom belongs to this engine is a run with NO shout in it -- and a watcher that first
        // registers on a shout leaves exactly that run with no movement reading at all. The lazy
        // registration itself stays (a `kDataLoaded` listener did not run in this setup); it is only
        // the trigger that widens, from the first shout to the first player animation event. The
        // call is a single already-registered branch after the first.
        AttackInputHook::EnsureInputWatcher();

        // Sampled on EVERY player animation event, deliberately, and
        // ahead of every tag decision below. The sprint bit is down by the time `BeginCastVoice`
        // arrives on real input, so the only reading that catches a sprinting shout is one taken
        // continuously while the sprint is still running. A game read, in phase A, outside the
        // lock (EngineLock.h rule 2).
        SampleSprint(actor);

        // ---- Phase A: game reads, driven by the tag and the hints ----

        const bool isBegin = tag == "BeginCastVoice"sv;

        const bool isEndMarker =
            tag == "MCO_AttackExitNotify"sv || tag == "attackStop"sv || tag == "inRdy"sv;
        // State Behavior Framework's exit annotation on the shout state, and the ONLY event that
        // ends a shout other than vanilla's own `shoutStop`. It reaches
        // here without any change to `IsNoise` or `IsInteresting` -- the "hout" needle already
        // matches it, so it has been in every SBF trace this project has taken.
        //
        // DELIBERATELY NOT ADDED TO `isEndMarker`. That set is the MCO ATTACK's teardown, which is
        // what ends a motion watch and restores suppressed movement; a shout ending is a different
        // event about a different thing, and folding the two together would restore the player's
        // movement at a moment nothing took it away.
        const bool isStateExit = tag == "SBF_ShoutStop"sv;

        // ITS COUNTERPART, AND ROUTE 2 HANGS ON IT. State Behavior Framework's ENTRY
        // annotation on the shout state -- the first moment `#0094` is provably running.
        //
        // Why route 2 needs it rather than `BeginCastVoice`: a state machine cannot take a
        // transition before it exists. Measured twice, standing and running --
        // `BeginCastVoice` at `69756.87`, our `moveStop` at `69765.60`, `SBF_ShoutStart` at
        // `69770.76`. The event went out 5 ms EARLY, so `#0094` had not activated and could not
        // consume it; the locomotion graph took it instead (hence `accepted=true` while running and
        // `false` while standing), `PlayerControls` undid it on the next update, and the shout
        // machine then started in `ShoutLocomotion` through its `startStateId` binding exactly as
        // if nothing had been sent. The whole shout then ran with the legs cycling.
        const bool isShoutStateEntry = tag == "SBF_ShoutStart"sv;

        // State Behavior Framework's exit annotation on the READY state, and the second
        // half of the driver-cast discriminator -- see `g_readyExitPending` for the measurement that
        // forced it and for why ADR-0006's single token is not enough.
        //
        // THIS IS THE ONE NEW TAG COMPARISON ON THE PER-EVENT PATH, and it is the only cost the
        // driver-cast discriminator adds to an engine that is not using the feature. `Observe` runs on every animation
        // event the player raises -- `FootLeft` included -- so a new comparison here is a real
        // decision rather than a line. It is paid because the alternative is a discriminator that
        // arms on the jump bounce, which is forbidden.
        const bool isReadyStateExit = tag == "SBF_ReadyStop"sv;

        // The per-shout reload, ahead of the snapshot so THIS shout runs on the values just read
        // -- an INI edit takes effect at the next shout, as the file documents.
        std::shared_ptr<const Settings> settings;
        if (isBegin) {
            if (Settings::Snapshot()->reloadPerShout) {
                Settings::Load();
            }
            settings = Settings::Snapshot();
            settings->Log();
        } else {
            settings = Settings::Snapshot();
        }

        // The hand-written cost guard that used to wrap this block is gone: `SHOUTMCO_TRACE` is a
        // macro and does not evaluate its arguments when tracing is off, so `GraphSummary`'s
        // graph-variable reads no longer happen at `bTrace = 0`.
        {
            const char*            rawPayload = a_event->payload.c_str();
            const std::string_view payload{rawPayload ? rawPayload : ""};
            if (IsInteresting(tag)) {
                SHOUTMCO_TRACE("[{:10.2f}] {}{}{}  || {}", ElapsedMs(), tag,
                               payload.empty() ? ""sv : " | "sv, payload, GraphSummary(actor));
            } else {
                SHOUTMCO_TRACE("[{:10.2f}] {}{}{}", ElapsedMs(), tag,
                               payload.empty() ? ""sv : " | "sv, payload);
            }
        }

        // An `isInitiate` trigger was a third condition here, and went with the counters it fed:
        // `TrackMcoAttackLocked` returns from its initiate branch before reaching any consumer of
        // the sample, so once `g_comboAtInitiate` and `g_comboPowerAtInitiate` were gone this was
        // four graph reads per attack answering nobody. It is NOT the refresh path's trigger --
        // that is `g_comboSampleWanted`, set when a press is queued -- so an initiate arriving with
        // a press already queued is still sampled, which is the only case that consumed it.
        //
        // RESTORED, AND THE REASON IT LEFT NO LONGER HOLDS. It was
        // deleted for answering nobody; `RecordRollingComboLocked` is now the somebody. The initiate
        // and the hit frame are the two attack-time moments the rolling capture is defined on, and
        // a capture cannot be taken from an event whose sample was never read. `HitFrame` is added
        // for the F3 reason recorded at its branch -- a power attack's combo advance lands there.
        //
        // Cost is four graph variable reads on attack-time events only, which is a handful per
        // swing and none at all when the player is not attacking. It is NOT gated on tracing,
        // because the value it feeds drives a decision rather than a log line.
        const bool isInitiate =
            tag == "MCO_AttackInitiate"sv || tag == "MCO_PowerAttackInitiate"sv;
        const bool isHitFrame = tag == "HitFrame"sv;

        // ADR-0006. Phase A cannot ask whether this entry will actually arm -- that
        // needs `g_beginSeenPending` and `shoutActive`, which live under the lock, and phase A runs
        // before it (EngineLock.h rule 2). So it asks the widest question it CAN answer from the
        // settings alone and reads for every shout-state entry while the feature is on.
        //
        // The over-read is one shout-state entry's worth of graph reads on a shout that turns out
        // to be vouched, plus the same on a jump bounce. That is a handful of reads a few times per
        // shout, against zero when the feature is off -- which is every shipped configuration
        // today. Sampling an entry that does not arm is harmless because the values it produces are
        // read by exactly one thing: the branch below, which discards them unless it arms. That is
        // a property of `driverLiveCombo` being its OWN local, not of what `TrackMcoAttackLocked`
        // does with tags -- see its declaration, which is where the distinction is argued.
        //
        // `g_shoutLiveForTrace` IS NOT AN ACCEPTABLE NARROWING HERE and is named so nobody tries
        // it: it is true for a shout whose begin was DECLINED, which is precisely the case this
        // must not confuse itself about.
        const bool driverArmPossible = isShoutStateEntry && settings->enabled && settings->chainDriverCasts;

        SampledCombo combo{};
        if (isBegin || isInitiate || isHitFrame ||
            g_comboSampleWanted.load(std::memory_order_relaxed)) {
            combo.v = SampleCombo(actor);
            combo.have = true;
        }

        // THE ARM'S OWN READING, AND IT IS A SEPARATE VARIABLE ON PURPOSE.
        //
        // Adding `driverArmPossible` to the condition above would have been one word shorter and
        // would have changed something else: `combo` is handed to `TrackMcoAttackLocked` on every
        // event, and giving that function a populated sample on a tag it has never seen one for
        // widens what it can do. `RecordRollingComboLocked` is safe (it is reached only from the
        // two attack-time branches), but `RefreshQueuedResumeLocked` is called on the fall-through
        // and writes a live reading into the queued snapshot -- and a live reading at a shout-state
        // entry is the post-teardown value this whole discriminator exists to avoid using.
        //
        // Its guards make that unreachable today, by a chain of four conditions and an argument
        // about graph ordering. A separate local makes it unreachable by construction, for the
        // price of one variable, and leaves `TrackMcoAttackLocked` seeing byte-for-byte what it
        // sees now whether the feature is on or off.
        SampledCombo driverLiveCombo{};
        if (driverArmPossible) {
            driverLiveCombo.v = SampleCombo(actor);
            driverLiveCombo.have = true;
        }
        const bool wasAttacking = isBegin && WasAttacking(actor);
        // Read here in phase A (EngineLock.h rule 2). The RECENCY reading, not the live
        // bit -- the shout ends the sprint before this event arrives, which is what made the first
        // pass fire plain attacks on real input. Read for a driver's arm too: a
        // hotbar-cast Whirlwind Sprint from a sprint deserves the same chain.
        const bool wasSprinting = (isBegin || driverArmPossible) && WasSprintingRecently(actor);
        // Read here, in phase A, because it is a game read (EngineLock.h rule 2) and
        // because `BeginCastVoice` is the one moment the selected power is still trustworthy.
        //
        // THE DRIVER'S KEY FORCES ITS SHOUT FIELD TO ZERO RATHER THAN READING
        // ONE. `sneaking` and `drawn` are read from the actor because they are true of the player
        // at that instant whatever raised the entry, and the window tail genuinely differs across
        // them. `shout` is NOT read, and that is the deliberate part: the player may well have a
        // shout equipped while casting from the hotbar, so `ReadWindowKey` would return a real
        // FormID and drop this cast's tail into that shout's bucket -- teaching the cache that
        // Unrelenting Force has a 2271 ms exhale because a fire spell borrowed the state machine.
        //
        // Zero is a real bucket and the right one (`ShoutChainEngine.cpp` window-key comment): a
        // real shout always has a `selectedPower`, so bucket 0 belongs exclusively to driver casts
        // and no pack shares it. Every cast type lands there together and their tails differ, so
        // the cache holds the MINIMUM -- min-bias opens the window early, toward today's behaviour,
        // and never before spellfire.
        WindowKey windowKey{};
        if (isBegin) {
            windowKey = ReadWindowKey(actor);
        } else if (driverArmPossible) {
            windowKey = ReadWindowKey(actor);
            windowKey.shout = 0;
        }
        // The same rule 2 reason, but a different event -- see `ReadShoutVariation` for
        // why word count cannot be read at `BeginCastVoice` with the rest of the key.
        const bool          isSpellFire = tag == "Voice_SpellFire_Event"sv;
        const std::uint32_t shoutVariation =
            isSpellFire ? ReadShoutVariation(actor)
                        : static_cast<std::uint32_t>(RE::TESShout::VariationID::kNone);
        const ClipSpellFireSample clipSpellFire =
            isSpellFire ? ReadPlayingClipSpellFire(actor) : ClipSpellFireSample{};

        // Shout-admissible after an attack-queued hold is `IsAttacking` false, not
        // `inRdy`. Always sampled here (EngineLock rule 2) — not gated on `g_comboSampleWanted`,
        // because cancel used to clear that hint and the next window event then looked idle.
        const bool isAttacking = GraphBool(actor, "IsAttacking");

        MotionReading motion{};
        bool          haveMotion = false;
        if (g_motionWatchLive.load(std::memory_order_relaxed) && TraceEnabled()) {
            motion = ReadMotion(actor);
            haveMotion = true;
        }

        // ---- Phase B: decisions, under the lock ----

        Emit emit;
        bool interceptGenerator = false;
        {
            std::scoped_lock lock(detail::g_engineLock);

            // Before anything else, and on every event: a swallowed press may not outlive what it
            // was taken for, whatever else the engine decides here.
            WatchdogReleasePressLocked(*settings, emit);
            WatchdogEndShoutLocked(*settings, emit);
            // The same bounded cap, for the other kind of occupant. A driver intent
            // waiting on a confirmed state that never arrives -- a moveset that raises
            // no swing at all is the live example -- is abandoned and logged, never forced
            // (ADR-0008). Checked on graph events like the caps above it, so it needs no timer.
            CastIntentApi::CheckWatchdog(static_cast<std::uint32_t>(settings->shoutWaitCapMs));
            TrackMcoAttackLocked(tag, combo, *settings, emit, isAttacking);


            // The motion profile across a chained swing. Every event carries a sample, because
            // the travel accumulates between them and one reading at the start would show
            // nothing. The sample itself was taken in phase A; here it only updates the peak and
            // decides whether the watch ends.
            if (g_state.motionWatch) {
                if (ElapsedMs() - g_state.motionArmedAtMs > static_cast<double>(settings->motionWatchMs)) {
                    emit.motionEnd = true;
                    emit.motionEndReason = "watch window elapsed"sv;
                } else {
                    if (haveMotion) {
                        // Not `std::max`: `max` is a macro here, courtesy of the Windows headers.
                        if (motion.velocity > g_state.motionPeakVelocity) {
                            g_state.motionPeakVelocity = motion.velocity;
                        }
                        emit.motionSample = true;
                        emit.motionTravelled = motion.pos.GetDistance(g_state.motionOrigin);
                    }
                    if (isEndMarker) {
                        emit.motionEnd = true;
                        emit.motionEndReason = tag;
                    }
                }
                if (emit.motionEnd) {
                    emit.motionEndPeak = g_state.motionPeakVelocity;
                    emit.motionEndHeldMs = ElapsedMs() - g_state.motionArmedAtMs;
                    if (haveMotion) {
                        emit.motionTravelled = motion.pos.GetDistance(g_state.motionOrigin);
                    }
                    g_state.motionWatch = false;
                    g_motionWatchLive.store(false, std::memory_order_relaxed);
                }
            }

            // Maintained whatever the `enabled` state, so the trace's shout reading stays correct
            // with the engine switched off. See `g_shoutLiveForTrace`.
            if (isBegin) {
                g_shoutLiveForTrace = true;
            } else if (tag == "shoutStop"sv || isStateExit) {
                // `SBF_ShoutStop` joins the natural end here rather than relying on
                // `EndShoutLocked` to clear the flag, because an interrupted shout with the engine
                // DISABLED never set `shoutActive` and so returns from `EndShoutLocked` before the
                // clear would matter. The trace's own reading of "is a shout live" has to be right
                // with the engine off -- that is the entire reason this flag exists.
                g_shoutLiveForTrace = false;
            }

            // Recorded OUTSIDE the decision chain below, because `SBF_ReadyStop` matches
            // none of its branches and the flag has to be set on an event the chain ignores.
            // Ungated by `enabled` and by `chainDriverCasts` for the same reason the begin flag is:
            // a token read only while the feature is on would be stale the first time somebody
            // switched it on mid-session, and the first cast after that would silently not arm.
            //
            // THE TOKEN IS NOT SHOUT-SPECIFIC AND IT HAS NO AGE BOUND. `SBF_ReadyStop` is raised on
            // EVERY exit from the ready state, an MCO attack included -- one measured trace
            // carried four of them against a single shout-state entry. So the flag can be left
            // standing by an attack and read by an unrelated shout-state entry much later, and
            // only the exchange below ever clears it. NOT a defect in any measured sequence: the
            // jump bounce always follows a shout whose own entry consumed the token.
            // It is a fragility rather than a bug, and the honest fix if one is wanted
            // is an age bound of the kind `RollingComboUsableLocked` already applies to its capture.
            if (isReadyStateExit) {
                g_readyExitPending = true;
            }

            if (isBegin) {
                // ADR-0006, LINE ONE. Set BEFORE the call, and the ordering is the whole reason the
                // discriminator keys on this event instead of on the graph's route into the state.
                // `BeginShoutLocked` returns before `shoutActive` when no shout is equipped, so a
                // GENUINE ordinary shout can enter the shout state with the engine holding nothing.
                // Vouching here means those still vouch, which is what the discriminator asks for;
                // setting it inside the function would tie the vouch to the engine having armed.
                g_beginSeenPending = true;
                BeginShoutLocked(*settings, combo, wasAttacking, windowKey, emit, /*driverCast*/ false,
                                 wasSprinting);
            } else if (isShoutStateEntry) {
                // ADR-0006, LINE TWO -- *A DRIVER CAST IS A SHOUT-STATE ENTRY WITH NO
                // `BeginCastVoice` BEHIND IT.*
                //
                // The exchange is unconditional and runs whatever the settings say, so the flag can
                // never survive its own shout and go on to vouch for a later entry. Reading it only
                // when the feature is on would leave it set through every shout taken with the
                // feature off, and the first entry after it was switched on would be the one that
                // silently failed to arm.
                //
                // THE READY TOKEN IS WHAT STOPS THE JUMP BOUNCE, NOT `shoutActive`.
                // ADR-0006 believed `shoutActive` covered it and the log it cites says otherwise --
                // the state exit ends the shout ~60 us BEFORE the re-entry, so the bounce arrives
                // unvouched AND unheld. The bounce is the one shout-state entry with no ready exit
                // in front of it (3 of 3 against 4 of 4), which is the whole of why the second
                // token exists. `g_readyExitPending` carries the measurement.
                //
                // BOTH ARE EXCHANGED UNCONDITIONALLY, before any settings test, so neither can
                // survive its own event and go on to vouch for a later one. Reading them only when
                // the feature is on would leave both set through every shout taken with the feature
                // off, and the first entry after it was switched on would be the one that silently
                // did the wrong thing.
                //
                // `shoutActive` STAYS, and it is no longer load-bearing for the bounce. What it
                // still does is refuse a second arm while the engine already holds a shout, which
                // is the ordinary meaning of the test and costs nothing.
                //
                // THE SETTINGS HALF IS `driverArmPossible`, THE SAME LOCAL PHASE A USED, and it is
                // shared rather than re-derived on purpose. Re-spelling `settings->enabled &&
                // settings->chainDriverCasts` here would be two predicates that must be kept in
                // step, and the failure mode of them drifting is silent: phase B would arm with
                // `driverLiveCombo.have == false` and a default `WindowKey`. Sharing the local
                // makes "phase A read for this arm" and "phase B takes this arm" the same
                // condition by construction.
                const bool vouched = std::exchange(g_beginSeenPending, false);
                const bool outOfReady = std::exchange(g_readyExitPending, false);
                if (!vouched && outOfReady && !g_state.shoutActive && driverArmPossible) {
                    // THE WIRING, AND IT IS THE ONE CALL THAT WAS BUILT AND LEFT
                    // INERT.
                    //
                    // A live read here is worthless and would look right, which is the trap. The
                    // graph was in the READY state immediately before this entry (`SBF_ReadyStop`
                    // at `145242.17`, `SBF_ShoutStart` at `145242.24`), so the interrupting
                    // teardown had already reset `MCO_nextattack` to 1 before the engine saw
                    // anything. Restoring that 1 would "preserve" a combo the player was never in.
                    // Arming 1342 ms earlier than the old spellfire design does not change this --
                    // the reset happens before the entry, not before spellfire.
                    //
                    // THE FALLBACK IS THE LIVE READ, DELIBERATELY. With no capture -- the player
                    // cast standing still, having attacked nothing -- the live value IS 1, and
                    // attack 1 is the correct answer. "Carry the combo the player was in" and
                    // "there wasn't one" are the same rule with different inputs, so the stale case
                    // lands on the same line as the empty one and neither needs a guess.
                    // A QUEUED SNAPSHOT STILL WINS, AND THE TRACE MUST NOT PRETEND OTHERWISE.
                    // `BeginShoutLocked` prefers a fresh `g_queuedResume` over whatever `a_live`
                    // carries, so substituting the rolling value while a snapshot is valid would
                    // print `>>> COMBO carried across the cast -- next=3` and then resume from a
                    // different number entirely. The trace must distinguish a
                    // correct capture from a lucky one; a trace that reports a value the engine did
                    // not use fails that harder than no trace at all.
                    //
                    // Deferring to the snapshot costs nothing and loses nothing. It is filled by a
                    // shout-key press at attack time -- the same class of pre-teardown reading the
                    // rolling capture is, taken more deliberately -- and `BeginShoutLocked` traces
                    // its own age on the `>>> SHOUT begin` line, so the cell still has its evidence.
                    // It is a rare state here (a hotbar cast arriving while a shout press is queued
                    // and unspent), which is exactly why it would have been found in a live session
                    // rather than in review.
                    double       rollingAgeMs = 0.0;
                    SampledCombo armCombo = driverLiveCombo;
                    if (g_queuedResume.valid) {
                        SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO carry SKIPPED -- a queued shout snapshot is still "
                              "live and `BeginShoutLocked` prefers it; the `SHOUT begin` line below reports "
                              "which value was actually used", ElapsedMs());
                    } else if (RollingComboUsableLocked(rollingAgeMs)) {
                        armCombo.have = true;
                        armCombo.v.nextAttack = g_rollingCombo.nextAttack;
                        armCombo.v.nextPowerAttack = g_rollingCombo.nextPowerAttack;
                        armCombo.v.currentAttack = g_rollingCombo.currentAttack;
                        armCombo.v.currentPowerAttack = g_rollingCombo.currentPowerAttack;
                        // THIS LINE AND NOT THE RESUME VALUE. "The captured index is
                        // provably the pre-cast one" cannot be shown by the number alone -- a
                        // correct capture and a lucky one read identically after the fact. What
                        // separates them is the AGE: a capture taken at an attack-time event before
                        // the cast reads as hundreds of ms old here, while a post-teardown poisoning
                        // would read as ~0.
                        SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO carried across the cast -- next={} power={}, "
                              "remembered {:.0f}ms ago (cap {:.0f}ms); a live read here would say 1, "
                              "because the teardown already reset it",
                              ElapsedMs(), armCombo.v.nextAttack, armCombo.v.nextPowerAttack,
                              rollingAgeMs, kRollingComboMaxAgeMs);
                    } else if (g_rollingCombo.valid) {
                        // Said out loud rather than falling through quietly: a refusal that
                        // looks identical to "there was nothing to carry" cannot be marked.
                        SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO carry REFUSED -- the remembered position is "
                              "{:.0f}ms old (cap {:.0f}ms), which describes a different fight; reading live "
                              "instead, which is today's behaviour",
                              ElapsedMs(), ElapsedMs() - g_rollingCombo.takenAtMs, kRollingComboMaxAgeMs);
                    } else {
                        SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO carry: nothing remembered -- the player has not "
                              "attacked this session, so there is no combo to carry and attack 1 is right",
                              ElapsedMs());
                    }

                    // `wasAttacking` IS PASSED AS FALSE RATHER THAN READ, and it is a statement of
                    // fact rather than a shortcut. It answers "was there an attack for this shout
                    // to interrupt", and it buys `teardownReadyPending` -- one owed ready pass, so
                    // the teardown's own `inRdy` does not read as the shout ending. On this path
                    // the teardown has ALREADY run: the graph passed through ready on its way into
                    // the shout state, which is the same measurement that makes the rolling capture
                    // necessary above. There is no pass left to owe, and reading the graph would
                    // spend four reads to be told so.
                    BeginShoutLocked(*settings, armCombo, /*wasAttacking*/ false, windowKey, emit,
                                     /*driverCast*/ true, wasSprinting);
                }

                // ROUTE 2 -- SEND THE TRIGGER NOW THAT THERE IS A MACHINE TO RECEIVE IT.
                //
                // Guarded on `rootLocked` rather than on `shoutActive`, because `rootLocked` is the
                // fact this actually depends on: the lock is set, so the gate that keeps the player
                // in `ShoutStanding` is closed and it is safe to push them there. It also declines
                // for free on every shout the engine deliberately did not root -- a shout begun
                // with the engine disabled, or one declined for having no shout equipped -- since
                // none of those set it.
                //
                // The LOCK still goes out at `BeginCastVoice`, and only the EVENT moves here. A
                // graph variable is read continuously, so writing it early costs nothing and closes
                // route 1's gate at the earliest possible moment; an event is consumed once, by
                // whatever is listening at that instant, so it has to be timed.
                //
                // A DRIVER'S CAST REACHES THIS WITH THE LOCK ALREADY SET BY THE ARM DIRECTLY ABOVE,
                // and that is why the two blocks are in this order.
                // `BeginShoutLocked` ran a few lines up on the same `Emit` and set
                // `rootLocked`, so this branch sees it true on the very event that armed. The lock
                // and the `moveStop` therefore leave in ONE deferred task, write first -- the
                // ordering an ordinary shout has to reach across two events to get. Re-setting
                // `setLock`/`lockValue` here is idempotent, not a second write.
                if (g_state.rootLocked) {
                    emit.setLock = true;
                    emit.lockValue = 1;
                    emit.raiseRoot = true;
                }
            } else if (tag == "inRdy"sv) {
                // Handled whatever the enabled state, so a toggle flipped mid-shout cannot
                // strand an armed chain.
                OnReadyLocked(*settings, emit);
            } else if (isStateExit) {
                // THE SHOUT IS OVER, BECAUSE THE GRAPH SAID SO.
                //
                // `SBF_ShoutStop` is State Behavior Framework's exit annotation on the shout state.
                // It fires on every ending, including the natural one -- where vanilla's `shoutStop`
                // arrives ~0.1 ms first and has already ended the shout, so this call finds nothing
                // live and returns. What is left for this branch is every OTHER way a shout ends,
                // which is exactly the set the engine used to infer from `inRdy` hundreds of
                // milliseconds late.
                //
                // UNGATED BY `enabled`, for the same reason `inRdy` is: a toggle flipped mid-shout
                // must not strand a live shout with the presses stacked up behind it.
                //
                // SBF IS A DEPENDENCY, so there is deliberately no
                // fallback path here. What stands behind this event is `WatchdogEndShoutLocked`'s
                // 8 s liveness cap, which is a pre-existing backstop against ANY hung shout rather
                // than a second implementation of this one.
                //
                // THE MEASUREMENT LINE BELOW IS AN INSTRUMENT, NOT BOOKKEEPING, AND IT COVERS THE
                // ONE ORDERING THIS CHANGE ASSUMES. `RecordNaturalTailLocked` runs only on the
                // `shoutStop` branch and only while the shout is still live, so if SBF ever won the
                // race on a natural shout the tail would silently stop being measured and the
                // window cache would quietly stop learning. It does not win it -- `shoutStop` came
                // first on both natural shouts traced (`290959.58` -> `290959.71`) and on the hotbar
                // cast (`147513.80` -> `147513.87`), and the order is causal rather than incidental:
                // the exhale clip raising `shoutStop` is what drives the state exit. This line makes
                // a violation legible instead of invisible -- "tail NOT measured" immediately
                // followed by a `shoutStop` is the signature, and on an interrupted shout it is
                // simply the truth.
                if (g_state.shoutActive && g_state.spellFiredAtMs > 0.0) {
                    SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW tail NOT measured -- the graph left the shout "
                          "state {:.0f}ms after spellfire without vanilla's own `shoutStop`, so this "
                          "shout was cut short and its tail is not this clip's length",
                          ElapsedMs(), ElapsedMs() - g_state.spellFiredAtMs);
                }
                EndShoutLocked(*settings, ShoutEnd::kStateExit, "SBF_ShoutStop"sv, emit);
            } else if (settings->enabled) {
                if (isSpellFire) {
                    // ADR-0009. When the playing exhale owns a late annotation, the
                    // generator's 0.100 s fire is withheld from the shout system. Window and
                    // `spellFired` wait for the clip's event so the chain still measures
                    // delivered fire → shoutStop.
                    const auto route = DecideClipOwnedSpellFire(
                        clipSpellFire.annotationS, clipSpellFire.localTimeS,
                        g_state.generatorIntercepted);
                    if (g_state.shoutActive && route == SpellFireRoute::kInterceptGenerator) {
                        g_state.generatorIntercepted = true;
                        interceptGenerator = true;
                        SHOUTMCO_TRACE(
                            "[{:10.2f}] >>> SPELLFIRE intercept generator -- clip owns "
                            "Voice_SpellFire_Event at {:.3f}s (local {:.3f}s); not delivered",
                            ElapsedMs(), clipSpellFire.annotationS.value_or(-1.f),
                            clipSpellFire.localTimeS.value_or(-1.f));
                    } else {
                        // The magic has fired, so from here a cut still delivers the shout -- and it
                        // is the start of the interval the window measures. It is no longer where the
                        // window opens: it is where the engine decides WHEN the window opens.
                        g_state.spellFired = true;
                        g_state.spellFiredAtMs = ElapsedMs();
                        // The key's last field, completed here rather than at
                        // `BeginCastVoice`, and completed BEFORE the schedule below reads it -- the
                        // lookup and the later tail record must agree on the same bucket, or a shout
                        // would schedule from one entry and write its measurement into another.
                        //
                        // A DRIVER'S CAST KEEPS `kNone` AND IS NOT ASKED. `ReadShoutVariation`
                        // should already answer `kNone` here via its `currentShout` null test, but that
                        // test has never been observed on a hotbar cast and the failure it guards is
                        // silent -- see `ChainState::driverCast`, which carries the whole argument and
                        // says what would let this condition be deleted.
                        if (!g_state.driverCast) {
                            g_state.windowKey.variation = shoutVariation;
                        }
                        if (settings->windowSource == Settings::WindowSource::kSpellFire) {
                            ScheduleOrOpenWindowLocked(*settings, emit);
                        }
                    }
                } else if (settings->windowSource == Settings::WindowSource::kGraph &&
                           tag == settings->windowEvent) {
                    // THE SAME HOLE ON THE OTHER WINDOW SOURCE.
                    // `OpenWindowLocked` refuses on `!shoutActive` on its own, so nothing can
                    // misfire here either; what it does NOT do is say so, and the rule is
                    // that a refusal has to leave a line or it cannot be told from a window that
                    // was never asked for. `sWindowSource = graph` is not the shipped default,
                    // which is why the spellfire path was the one that got filed -- the hole is
                    // identical and a session run on the graph trigger would have produced no
                    // evidence of it at all.
                    if (!g_state.shoutActive) {
                        SHOUTMCO_TRACE("[{:10.2f}] >>> WINDOW none -- the graph trigger '{}' arrived with "
                              "no live shout, so there is nothing to chain out of and no window is "
                              "opened", ElapsedMs(), tag);
                    }
                    OpenWindowLocked(*settings, "graph trigger"sv, emit);
                } else if (tag == "shoutStop"sv) {
                    // ORDER STILL MATTERS, and it is now two steps rather than three.
                    //
                    // Record FIRST, because `EndShoutLocked` clears the flag that says a
                    // measurement is in flight. The hand-back used to be the middle step and a
                    // separate call; it lives inside `EndShoutLocked`, because being a
                    // separate call at exactly one of three exits is what dropped the press.
                    RecordNaturalTailLocked();
                    EndShoutLocked(*settings, ShoutEnd::kFinished, "shoutStop"sv, emit);
                }
            }

            // LAST IN THE BLOCK, AND THAT IS THE POINT. Every branch above has had its
            // chance to clear `awaitingReady` on this event; what is left is a chain armed against
            // an `inRdy` that is not coming. Ungated by `enabled` for the same reason the `inRdy`
            // and state-exit branches are: a toggle flipped mid-shout must not strand an arm. See
            // the function for why it drops rather than fires, and why it is not up with the other
            // two watchdogs.
            WatchdogDropChainLocked(*settings);
        }

        // ---- Phase C: emissions ----

        if (emit.motionSample && haveMotion) {
            SHOUTMCO_TRACE("[{:10.2f}] >>> MOTION {}  || {}", ElapsedMs(), tag,
                  FormatMotion(motion, emit.motionTravelled));
        }
        if (emit.motionEnd) {
            SHOUTMCO_TRACE("[{:10.2f}] >>> MOTION end ({}): travelled {:.1f} units in {:.0f}ms, peak vel {:.1f}",
                  ElapsedMs(), emit.motionEndReason, emit.motionTravelled, emit.motionEndHeldMs,
                  emit.motionEndPeak);
        }
        ExecuteEmits(emit, actor);
        return !interceptGenerator;
    }

    bool ShoutChainEngine::OnAttackButton(const RE::ButtonEvent& a_event, float a_gameHoldThreshold) {
        const auto settings = Settings::Snapshot();
        if (!settings->enabled) return false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        Emit emit;
        bool consumed = false;
        {
            std::scoped_lock lock(detail::g_engineLock);

            if (a_event.IsDown()) {
                // `g_held.holding` ends at `inRdy`, before the replay reaches `BeginCastVoice`.
                // The queued combo snapshot spans the whole operation instead: it is created in
                // the same locked decision that swallows the shout down, consumed when that shout
                // begins, and abandoned when its replay cannot become a shout. That makes it the
                // state which actually means "a queued shout has not started yet" throughout the
                // cancel/replay gap this queue exists to cover.
                const bool queuedShout = !g_state.shoutActive && g_queuedResume.valid;
                if (g_state.shoutActive || queuedShout) {
                    g_state.pressOwned = true;
                    g_state.pressOwnedAtMs = ElapsedMs();
                    g_state.pressPending = true;
                    g_state.pressedAtMs = ElapsedMs();
                    g_state.pressKind = AttackKind::kLight;
                    g_state.waitingForShoutStart = queuedShout;
                    // While the power press is a hold, this press is not yet a light attack: the
                    // game itself only decides at the threshold or on release, and deciding
                    // earlier here is what would make a power chain unreachable. With a dedicated
                    // power key there is nothing to wait for, so a light attack stays crisp.
                    g_state.pressResolved = !settings->HoldToPower();

                    if (queuedShout) {
                        SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS taken (waiting for queued shout to start)",
                              ElapsedMs());
                    } else {
                        SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS taken (window {})", ElapsedMs(),
                              g_state.windowOpen ? "open"sv : "shut"sv);
                    }
                    TryFireChainLocked(*settings, emit);
                    consumed = true;
                }
            } else if (g_state.pressOwned) {
                // Follow-ups belong to us only if we took the down edge. A press that began
                // before the shout -- including a block being held -- is never touched.
                //
                // `!IsPressed()` and NOT `IsUp()`, which additionally requires a non-zero held
                // duration (see the same correction in `ShoutInputHook`). This is the ONLY place
                // a press we have taken is handed back on the player's own terms, so a release it
                // fails to recognise is a dead attack button rather than a missed frame. Any
                // zero-value event ends ownership.
                if (!a_event.IsPressed()) {
                    g_state.pressOwned = false;
                    // `pressPending` as well: a press whose chain has already fired is not still
                    // waiting to be called light or heavy. Without it the release of a chained
                    // press logs "released as light" *after* the attack went out, which reads in
                    // a trace as a second press that never happened.
                    if (g_state.pressPending && !g_state.pressResolved) {
                        g_state.pressResolved = true;  // released short of the threshold: a light attack
                        SHOUTMCO_TRACE("[{:10.2f}] >>> PRESS released as light ({:.2f}s held)", ElapsedMs(),
                              a_event.HeldDuration());
                        TryFireChainLocked(*settings, emit);
                    }
                } else {
                    ResolveAsPowerIfHeldEnoughLocked(*settings, a_event.HeldDuration(),
                                                     a_gameHoldThreshold, emit);
                }
                // We own this press either way, so the game must not see any of its events.
                consumed = true;
            }
        }

        ExecuteEmits(emit, player);
        return consumed;
    }

    bool ShoutChainEngine::OnAttackHold(const RE::ButtonEvent& a_event, float a_gameHoldThreshold) {
        const auto settings = Settings::Snapshot();
        if (!settings->enabled) return false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        Emit emit;
        bool consumed = false;
        {
            std::scoped_lock lock(detail::g_engineLock);
            if (g_state.pressOwned) {
                ResolveAsPowerIfHeldEnoughLocked(*settings, a_event.HeldDuration(), a_gameHoldThreshold,
                                                 emit);
                // We own this press, so the game must not charge a power attack of its own from it.
                consumed = true;
            }
        }

        ExecuteEmits(emit, player);
        return consumed;
    }

    double ShoutChainEngine::ElapsedMs() { return ShoutMCO::ElapsedMs(); }

    std::uint32_t ShoutChainEngine::FrameCount() { return ShoutMCO::FrameCount(); }

    bool ShoutChainEngine::IsShoutLive() { return g_shoutLiveForTrace; }

    ShoutChainEngine::ComboSnapshot ShoutChainEngine::SampleCombo(RE::Actor* a_actor) {
        return ComboSnapshot{
            .nextAttack = GraphInt(a_actor, "MCO_nextattack"),
            .nextPowerAttack = GraphInt(a_actor, "MCO_nextpowerattack"),
            .currentAttack = GraphInt(a_actor, "MCO_currentattack"),
            .currentPowerAttack = GraphInt(a_actor, "MCO_currentpowerattack"),
        };
    }

    // See the header for why the reading lives on this edge rather than at
    // `BeginCastVoice` or on the animation-event path.
    //
    // TRACED UNCONDITIONALLY, INCLUDING THE NOT-SPRINTING PRESS. Two passes have
    // now been misread from an absent line, and the question this edge answers -- "was the player
    // actually sprinting when they pressed the key" -- is one no other instrument in this engine
    // can reach. A press that reads `sprinting=false` here means the shout did not begin from a
    // sprint at all, which is a different problem from the engine losing the fact afterwards, and
    // the two are indistinguishable without this line.
    void ShoutChainEngine::NoteShoutKeyDown(RE::Actor* a_actor) {
        if (!a_actor) return;
        const bool sprinting = IsSprintingNow(a_actor);
        if (sprinting) {
            g_lastSprintingMs.store(ElapsedMs(), std::memory_order_relaxed);
        }
        SHOUTMCO_TRACE("[{:10.2f}] >>> SHOUT KEY down: sprinting={} (last sprint {:.0f}ms ago)",
              ElapsedMs(), sprinting,
              ElapsedMs() - g_lastSprintingMs.load(std::memory_order_relaxed));
    }

    // Called from the input hook once its lock is DOWN -- never from inside it. The cut
    // notifies the graph, and notifying from inside the graph's own dispatch is forbidden,
    // so this follows the file's standing shape: build an `Emit` under the lock, execute it after.
    // Safe to call on any swallowed shout press; every precondition is rechecked under the lock.
    void ShoutChainEngine::CutShoutForQueuedShoutChain() {
        const auto settings = Settings::Snapshot();
        if (!settings->enabled) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        Emit emit;
        {
            std::scoped_lock lock(detail::g_engineLock);
            TryCutForShoutChainLocked(*settings, emit);
        }
        ExecuteEmits(emit, player);
    }

    void ShoutChainEngine::NoteQueuedShoutLocked(const ComboSnapshot& a_combo) {
        g_queuedResume = QueuedResume{.valid = true,
                                      .takenAtMs = ElapsedMs(),
                                      .nextAttack = a_combo.nextAttack,
                                      .nextPowerAttack = a_combo.nextPowerAttack,
                                      .currentAttack = a_combo.currentAttack,
                                      .currentPowerAttack = a_combo.currentPowerAttack};
        g_comboSampleWanted.store(true, std::memory_order_relaxed);

        SHOUTMCO_TRACE("[{:10.2f}] >>> COMBO snapshot at queue time: MCO_currentattack={} MCO_nextattack={} "
              "MCO_currentpowerattack={} MCO_nextpowerattack={}", ElapsedMs(), a_combo.currentAttack,
              a_combo.nextAttack, a_combo.currentPowerAttack, a_combo.nextPowerAttack);
    }

    void ShoutChainEngine::AbandonQueuedResume(std::string_view a_reason) {
        std::scoped_lock lock(detail::g_engineLock);
        AbandonQueuedResumeLockedImpl(a_reason);
    }

    void ShoutChainEngine::QueuedShoutDidNotStart() {
        const auto settings = Settings::Snapshot();
        auto*      player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        Emit emit;
        {
            std::scoped_lock lock(detail::g_engineLock);
            const bool parked = g_state.pressPending && g_state.waitingForShoutStart;
            if (!g_queuedResume.valid && !parked) return;
            if (g_state.shoutActive) return;

            AbandonQueuedResumeLockedImpl("replayed shout press did not start a shout"sv);
            if (parked) {
                const auto waited = ElapsedMs() - g_state.pressedAtMs;
                const auto kind = g_state.pressKind;
                g_state.waitingForShoutStart = false;
                g_state.pressPending = false;
                SHOUTMCO_TRACE("[{:10.2f}] >>> ATTACK without a cut, press {:.1f}ms old ({}) -- the replayed "
                      "shout did not start, press handed back",
                      ElapsedMs(), waited, Describe(kind));
                if (g_state.pressResolved) {
                    AttackWithoutResumeLocked(*settings, kind, emit);
                }
            }
        }
        ExecuteEmits(emit, player);
    }

    void ShoutChainEngine::AbandonQueuedResumeLocked(std::string_view a_reason) {
        AbandonQueuedResumeLockedImpl(a_reason);
    }

    ShoutChainEngine::ShoutVerdict ShoutChainEngine::ShouldHoldShoutLocked() {
        // Never asks about shout cooldown, and never asks whether the player *may* shout. Only
        // whether an MCO attack is running, or whether the current shout's legal chain window is
        // open (ADR-0002: issue the attempt, let the game refuse it).
        //
        // `g_mcoSwingLanded` USED TO EXCUSE A PRESS from the hold: after `HitFrame` the shout went
        // straight through, on the reasoning that the hit was already banked so nothing was left to
        // protect. That is false -- the attack runs ~900ms past its hit frame,
        // and a shout entered anywhere in that tail dies to the attack's teardown exactly like one
        // entered before it. The hit being safe was never the question; the attack still owning the
        // character was.
        //
        // It stays in the verdict for the trace, which is the only honest reading of "the swing has
        // already landed" -- see the marker in `ShoutInputHook`.
        //
        // v1.0.0's clause -- `&& !g_mcoSwingLanded`, letting a post-hit press through unheld -- was
        // restored here and removed again the same hour, because driving it showed
        // why it cannot work: an unheld press reaches a graph that is still running the attack, and
        // the shout starts but never exhales. So the press is held whatever the swing has done, and
        // the cancel in `TryCancelRecoveryLocked` is what brings the wait to an end.
        //
        // TAKES NO SETTINGS. It briefly took them so the gate
        // could be chosen here, and when the gate went the parameter stayed behind as
        // `(void)a_settings;`. `TryCancelRecoveryLocked` got this treatment at the time and this
        // one did not. Whether the press is held at all is `bShoutWaitsForSwing`, and that is
        // checked by the caller in `ShoutInputHook` before it ever gets here.
        //
        // A live MCO attack wins over an open shout window when both could apply (a
        // shout press mid-attack during someone else's shout is still "behind the attack"). The
        // shout-window hold is otherwise the same one-entry buffer, released at `shoutStop`.
        if (g_mcoAttackLive) {
            return ShoutVerdict{.hold = true,
                                .reason = ShoutHoldReason::kBehindAttack,
                                .attackLive = true,
                                .swingLanded = g_mcoSwingLanded};
        }
        if (g_state.shoutActive && g_state.windowOpen) {
            return ShoutVerdict{.hold = true,
                                .reason = ShoutHoldReason::kBehindShout,
                                .attackLive = false,
                                .swingLanded = false};
        }
        return ShoutVerdict{};
    }

    bool ShoutChainEngine::OnPowerAttackEvent(std::string_view a_eventName) {
        const auto settings = Settings::Snapshot();

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        // Sampled before the lock (a game read); composed only when the trace will use it.
        const std::string summary = TraceEnabled() ? MovementSummary(player) : std::string{};

        Emit emit;
        bool consumed = false;
        {
            std::scoped_lock lock(detail::g_engineLock);

            // Logged on EVERY power press, including one with no shout anywhere near it. A power
            // attack taken outside a shout is the control that says whether a symptom belongs to
            // this engine at all, and a control carrying no movement reading of its own cannot
            // evidence "a movement key was held" -- it can only assert it in a header, so the
            // instrument has to reach it.
            SHOUTMCO_TRACE("[{:10.2f}] >>> POWER EVENT \"{}\" seen, shout {}  || {}", ElapsedMs(), a_eventName,
                  g_state.shoutActive ? "active"sv : "inactive"sv, summary);

            // LIVE SHOUT ONLY. A power press while a shout is merely QUEUED behind an attack is
            // not buffered any more: the seam forwards the event, so the game plays the power
            // attack and the queued shout stays queued behind it, which is what the button did.
            // Buffering it meant waiting on a shout that could never start (a shout on cooldown)
            // and the watchdog replaying the press five seconds late. Owner: "i kind've don't care
            // about buffering if it's causing more problems than the edge-case it solves."
            if (settings->enabled && g_state.shoutActive) {
                // An outgoing `attackPowerStart*` is unambiguous the moment it is sent -- whichever
                // mod decided it, the decision is already made, so this direction fires immediately
                // where the hold path has to wait its threshold out.
                g_state.pressPending = true;
                g_state.pressResolved = true;
                g_state.pressedAtMs = ElapsedMs();
                g_state.pressKind = AttackKind::kPower;
                g_state.waitingForShoutStart = false;

                SHOUTMCO_TRACE("[{:10.2f}] >>> POWER EVENT \"{}\" (window {})", ElapsedMs(), a_eventName,
                      g_state.windowOpen ? "open"sv : "shut"sv);
                TryFireChainLocked(*settings, emit);
                consumed = true;
            }
        }

        ExecuteEmits(emit, player);
        return consumed;
    }
}
