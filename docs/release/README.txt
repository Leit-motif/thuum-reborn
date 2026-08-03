SHOUTS FOR MCO 1.0.0 -- THIS MOD SHIPS NO ANIMATIONS. IT IS AN ENGINE.

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
returns the exhale to ready, and ready already accepts `attackStart`. The engine adds no
behaviour graph patch of its own and requires no Nemesis run beyond the one your load order
already does.

Because it never inspects which animation is playing, it works with any pack without that
pack's author changing anything. Shout animation packs carry zero annotations -- verified
across 137 files from two independent authors -- and this engine is built so that stays true.

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

Optional, and detected automatically if present:

  * One Click Power Attack NG                                 1.11
      The engine reads OCPA's own key binding from its config, so there is nothing to set. With
      OCPA absent, a power chain is a held attack button using YOUR game's own power-attack hold
      threshold -- no setting of ours to tune.

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

Install with a mod manager and let it load after ADXP I MCO. There is no ESP and no Nemesis
patch of ours, so no Nemesis re-run is needed for this mod.


CONFIGURATION
-------------

SKSE\Plugins\ShoutMCO.ini. Five settings, all five listed here, each documented at more
length in the file itself. Edits take effect on your next shout -- no restart, no new save.

  bEnabled = 1             0 turns the engine off completely and gives you stock shouting
                           back without uninstalling. Try this first when isolating a mod
                           conflict: if the problem survives, it is not this mod.

  bTrace = 0               Off, and it should stay off. At 1 it logs every animation event
                           on your character -- megabytes of log and constant disk writes
                           during combat. Turn it on only to capture a bug report, then off.

  bShoutWaitsForSwing = 1  A shout pressed mid-attack waits for your swing to land first.
                           At 0 you get vanilla timing, where that shout cancels the attack
                           before it connects and the hit is lost.

  sPowerSource = auto      Where a power attack press comes from. `auto` uses One Click
                           Power Attack's key if you have it, otherwise a held attack
                           button. Also accepts ocpa, hold, or off.

  iPowerAttackKeycode = -1 -1 reads OCPA's own binding, so there is nothing to set. Put a
                           scan code here only if some other mod owns your power attack key.

Everything else is fixed internally, and deliberately so: those values were measured in game
and a wrong one fails silently. In particular there is no chain-window length to tune and no
power-attack hold time to set -- both come from the game's own timing and your own settings,
so a number of ours could only disagree with your game.

If you run a power attack mod that this engine does not recognise -- Elden Power Attack is
the known one, since it makes HOLDING attack do repeated light attacks -- set
sPowerSource = off. Shouts still chain into normal attacks.

Log file, if you need to report a problem: Documents\My Games\Skyrim Special Edition\SKSE\
ShoutMCO.log


KNOWN LIMITS
------------

  * Only a rapier has been driven in testing. Other melee weapons use the same events and are
    expected to work -- MCO patches one universal attack graph, not a per-weapon one -- but
    they have not been individually verified. If you hit a problem with a specific weapon
    type, please report it.

  * Bows and crossbows are not applicable. MCO is a melee moveset framework, so there is no
    shout -> bow chain to have.

  * MCO attack -> shout without the game tearing the attack down, and shout -> shout, are not
    implemented. The mid-swing case that actually hurt -- losing your hit -- is fixed.


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
