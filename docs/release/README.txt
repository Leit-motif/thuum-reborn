THU'UM REBORN

Now shouts are soulslike too! Shout mid-combo and the chain keeps going; press attack
during a shout and the swing seamlessly transitions. Shouts commit — they actually feel
like a part of the modern combat stack.

Every vanilla and Thunderchild shout has its own full animation, built from BOTuser998's
Thu'um - Fully Animation Shouts and ToyzFX's S.Y.H.O.


COMPATIBILITY
-------------

  * ADXP | MCO and BFCO -- both supported. Install whichever one you play with.

  * One Click Power Attack NG -- supported, and you need it on MCO, which has no power
    attack of its own. BFCO has one built in, so it is optional there. Nothing is
    detected or configured: this mod watches the attack, not the key, so any mod that
    starts a power attack the normal way works. Elden Power Attack included.

  * Vanilla's hold-to-power-attack -- not supported during a shout. Holding the attack
    button gives you a light attack.

  * Thunderchild - Epic Shouts -- supported, detected if present.

  * First person -- shouts play their vanilla animation and this mod stays out of the
    way. Everything here is a third-person change.

  * Skyrim versions -- SE 1.5.97, AE 1.6.1170 and 1.7.104. Played most on SE 1.5.97.

  * VR -- not supported.

  * Other shout animation packs -- not supported.


PLANNED
-------

  * Spell Hotbar 2


INSTALLATION
------------

Install with your mod manager, then regenerate behaviors with Nemesis or Pandora --
whichever you use. In Nemesis, tick "Thu'um Reborn", press Update Engine, then Launch.
No ESP, no scripts -- safe to install and uninstall at any time, mid-playthrough included.


UNINSTALLATION
--------------

Delete the folder. Everything is written at runtime; nothing is baked into your save.


SETTINGS
--------

SKSE\Plugins\ShoutMCO.ini. Changes apply on your next shout -- no restart. With SKSE Menu
Framework, the same two values are on the in-game page.

  * bTrace = 0 -- leave it off. Turn it on only to capture a log for a bug report.

  * fWordTwoHoldSec = 0.4 -- the fix for a tap that comes out as a two-word shout. How long
    you hold the shout key before it charges to word two: 0.4 keeps a tap reliably one word,
    0.2 is vanilla, 0 leaves the game's value alone.


CREDITS
-------

BOTuser998 -- Thu'um - Fully Animated Shouts
(https://www.nexusmods.com/skyrimspecialedition/mods/50559), the major inspiration behind
this mod.

ToyzFX -- SYHO - Shout Your Heart Out
(https://www.nexusmods.com/skyrimspecialedition/mods/172591) animations.

Engine is GPL-3.0. Source: https://github.com/Leit-motif/thuum-reborn
