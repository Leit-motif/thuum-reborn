SHOUTS FOR MCO -- THIS MOD SHIPS NO ANIMATIONS. IT IS AN ENGINE.

(The exact version and the source commit that built this copy are stamped at the bottom of
this file by the packaging script -- one place to look, and it cannot drift from the binary.)

It makes whatever shout animations you already have chain into MCO attacks. It contains no
shout animations, no movesets, and no replacers of its own. If you install it expecting new
shout animations you will see nothing change, and that is the mod working as designed.

Bring your own shout animation pack. SYHO - Shout Your Heart Out and Goetia Animations -
Conditional Shouts are both known to work; so does vanilla, and so should any pack, because
the engine never looks at which clip is playing.


WHAT IT DOES
------------

Press attack during a shout and the attack comes out immediately, resuming your MCO combo
where it left off rather than restarting it. Verified working:

  * shout -> MCO light attack
  * shout -> MCO power attack (both the held attack button and One Click Power Attack's key)
  * shout out of the middle of a combo, with the combo resumed at the right index
  * a shout pressed mid-swing no longer eats the hit -- the swing lands, then the shout starts

The chain costs about 10-30ms, well inside a combo window.


HOW IT WORKS, BRIEFLY
---------------------

Two transitions that already exist in Skyrim's own shout graph, fired in order: `shoutStop`
returns the exhale to ready, and ready already accepts `attackStart`. Chaining itself therefore
needs no new graph edge. The mod does ship a small Nemesis patch on the shout graph -- see
INSTALLATION -- and it is small by design: it declares what the engine needs to read and
touches nothing about how your shout animations play.

Because it never inspects which animation is playing, it works with any pack without that
pack's author changing anything. Shout animation packs carry no event annotations -- 133 of 137
files across two independent authors carry nothing at all, and the four exceptions carry only
root-motion data, which is not an event -- and this engine is built so that stays true.

Your shout still goes off. The magic fires 0.1 seconds into the exhale, so cutting the exhale
short to attack does not cancel the shout.


REQUIREMENTS
------------

Each one checked against the load order this was developed and tested in, with the version
that was actually present:

  * SKSE64                                                    (Skyrim SE 1.5.97 or AE 1.6.1170)
  * Address Library for SKSE Plugins                          2.0
  * Nemesis Unlimited Behavior Engine                         0.84b
  * ADXP I MCO (Attack MCO-DXP)                               1.6.0.6
  * Payload Interpreter                                       1.1
  * Open Animation Replacer                                   3.2.0.0
  * State Behavior Framework                                  2.0.0
      Not optional, and the one requirement worth a sentence: it is how this learns that a
      shout has ENDED. Without it, every jump, sheathe or knockdown during a shout leaves your
      attack button held down until an internal timeout releases it -- which is worse than the
      behaviour it replaced.
  * SYHO - Shout Your Heart Out - Shout Animation Overhaul
      Some of the animations in this pack are ToyzFX's, incorporated with their permission on
      the condition that SYHO is listed as a requirement. Listed here for the same reason.

Optional, and detected automatically if present:

  * One Click Power Attack NG                                 1.11
      The engine reads OCPA's own key binding from its config, so there is nothing to set. With
      OCPA absent, a power chain is a held attack button using YOUR game's own power-attack hold
      threshold -- no setting of ours to tune.

  * SKSE Menu Framework                                       3.14.1
      Adds an in-game page for the settings below, so they can be tuned while playing rather
      than by alt-tabbing to a text file. Without it the INI is the whole of the surface and
      nothing else changes.

Not supported: Pandora, BFCO. Nemesis only.


RECOMMENDED -- SHOUT COOLDOWN
-----------------------------

Skyrim runs ONE voice-recovery timer per character, not one per shout, and Unrelenting Force
alone is 15/20/45 seconds. So chaining shout -> shout is unreachable in normal play for
gameplay reasons rather than technical ones, and this mod deliberately does not touch shout
cooldown at all.

If you want shouts available often enough to chain them freely, install something like
TISC - True Individual Shout Cooldown, Individual Shout Cooldown Remake, or Individual Shout
Cooldowns. None of them is required, and this mod does not patch or depend on any of them.


INSTALLATION
------------

Install with a mod manager and let it load after ADXP I MCO. There is no ESP.

This mod ships a small Nemesis patch, so run Nemesis after installing it:

  1. Open Nemesis Unlimited Behavior Engine through your mod manager.
  2. Tick "Thu'um Reborn" in the patch list.
  3. Press Update Engine, then Launch Nemesis Behavior Engine.
  4. Make sure the Nemesis output mod is enabled and wins over other behaviour mods.

Do this again any time you add, remove, or update a mod that patches behaviour.

If you skip it, nothing breaks. Every other feature above keeps working; the one thing you lose
is being rooted in place while you shout. The mod notices and says so once in its log rather
than failing quietly, so a missed step costs you a feature and not your game.


CONFIGURATION
-------------

SKSE\Plugins\ShoutMCO.ini. Three settings, all three listed here, each documented at more
length in the file itself. Edits take effect on your next shout -- no restart, no new save.

With SKSE Menu Framework installed there is also an in-game page, under "Thu'um Reborn", that
writes this same file. There is no second settings file and no separate copy to keep in step:
whichever you use, the other shows it. bTrace is the one exception -- it is read once when the
game starts, so the menu marks it restart-only.

  bTrace = 0               Off, and it should stay off. At 1 it logs every animation event
                           on your character -- megabytes of log and constant disk writes
                           during combat. Turn it on only to capture a bug report, then off.

  sPowerSource = auto      Where a power attack press comes from. `auto` uses One Click
                           Power Attack's key if you have it, otherwise a held attack
                           button. Also accepts ocpa, hold, or off.

  fWordTwoHoldSec = 0.4    How long the shout key must be HELD for the second word, in
                           seconds. This is the fix for a tap that comes out as a two-word
                           shout: the game promotes your hold at 0.2 s, which is inside an
                           ordinary deliberate press. 0.2 puts vanilla's timing back, and 0
                           leaves your game's own value alone entirely.

                           It is the same number in both directions -- raising it also means
                           every two-word shout needs a longer hold. Nothing is saved or
                           edited: the value is written into the running game each time you
                           shout, and put back if you set this to 0. Deleting the mod folder
                           restores your game exactly.

Everything else is fixed internally, and deliberately so: those values were measured in game
and a wrong one fails silently. In particular there is no power-attack hold time to set --
that comes from the game's own threshold and your own settings, so a number of ours could
only disagree with your game.

If you run a power attack mod that this engine does not recognise -- Elden Power Attack is
the known one, since it makes HOLDING attack do repeated light attacks -- set
sPowerSource = off. Shouts still chain into normal attacks.

Log file, if you need to report a problem: Documents\My Games\Skyrim Special Edition\SKSE\
ShoutMCO.log


KNOWN LIMITS
------------

  * Jumping during a shout cuts the shout short. The landing returns your character to a
    ready stance and the engine reads that as the shout having ended. The magic still goes
    off -- it fires about a tenth of a second into the shout -- so what you lose is the tail
    of the animation, not the effect. If you had already pressed attack, that attack comes
    out at the landing rather than where you expected it. Reproducible, understood, and
    being worked on. Until then, jumping mid-shout is the one thing worth avoiding.

  * Only a rapier has been driven in testing. Other melee weapons use the same events and are
    expected to work -- MCO patches one universal attack graph, not a per-weapon one -- but
    they have not been individually verified. If you hit a problem with a specific weapon
    type, please report it.

  * Bows and crossbows are not applicable. MCO is a melee moveset framework, so there is no
    shout -> bow chain to have.

  * MCO attack -> shout without the game tearing the attack down, and shout -> shout, are not
    implemented. The mid-swing case that actually hurt -- losing your hit -- is fixed.

  * Elden Power Attack inverts the attack hold -- holding attack there means repeated LIGHT
    attacks -- so this engine's held-button power detection would fire power chains you never
    asked for. Set sPowerSource = off (see CONFIGURATION above); shouts still chain into
    normal attacks. One Click Power Attack and OCPA NG are fine and detected automatically.

  * Spell Hotbar 2 is safe to run alongside this mod, but its hotbar casts do not chain.
    Its casts drive the shout graph directly rather than going through a normal shout, and
    this engine only engages on a real shout, so it never touches them: your attack press
    goes straight to the game and no cut is ever sent. Nothing breaks. What you also do not
    get is a chain out of a hotbar cast into an MCO combo -- that is a feature this engine
    does not have yet, not a conflict to report. Ordinary shouts chain normally with Spell
    Hotbar 2 installed.


CREDITS AND LICENCE
-------------------

Continuing the idea of "Thu'um - Fully Animated Shouts" by BOTuser999, which is where the
notion of shouts that behave like attacks came from. This mod is NOT affiliated with or
endorsed by that author, and ships none of that mod's content.

Built with CommonLibSSE-NG, maintained by alandtse and contributors and originally CommonLibSSE
by Ryan-rsm-McKenzie. It is licensed under the GNU General Public License version 3 with a
modding exception, so this mod is GPL-3.0 too -- see the included LICENSE file for the full
text, the exception, and what it does and does not permit.

Source code, as the licence requires:  https://github.com/Leit-motif/shouts-for-mco
You are free to use, modify and redistribute this, including on other sites, provided the
licence and copyright notices stay intact, you say what you changed, and your version is
available under the same licence.

Standing on other people's work. None of it is included here; each is a separate mod by its own
author, required or supported but distributed by them, not by me:

  * SKSE64                     Ian Patterson, Stephen Abel, Paul Connelly
  * ADXP I MCO                 Distar -- the attack framework this engine chains into
  * Payload Interpreter        dTry and alexsylex
  * Open Animation Replacer    Ersh
  * Address Library            meh321
  * Nemesis                    Shikyo Kira
  * One Click Power Attack NG  Bingle -- detected and honoured automatically when present

Tested against the SYHO and Goetia Animations - Conditional Shouts shout packs. Neither is
included, required, or modified, and the engine never looks at which pack you use.

Built with houseCARL and the Skyrim Claude Code Modding Toolkit, which did the load-order reads,
the behaviour-graph tracing and the in-game telemetry this was measured with. Thank you.
