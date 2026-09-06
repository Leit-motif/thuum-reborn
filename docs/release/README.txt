THU'UM REBORN

Shouts are attacks now. Shout mid-combo and the chain keeps going; press attack during a
shout and the swing comes out the moment the shout releases. Shouts commit the way MCO
attacks commit -- you stop dropping your combo to shout.

Every vanilla and Thunderchild shout has its own full animation, built from BOTuser998's
Thu'um and ToyzFX's SYHO.


COMPATIBILITY
-------------

  * One Click Power Attack NG -- supported, detected if present.
  * Thunderchild - Epic Shouts -- supported, detected if present.
  * Elden Power Attack -- not supported. Set sPowerSource = off.
  * AE (1.6.x) -- untested. Tested on SE 1.5.97.
  * VR -- not supported.
  * Other shout animation packs -- not supported.


PLANNED
-------

  * Spell Hotbar 2
  * Pandora
  * BFCO


INSTALLATION
------------

Install with your mod manager. Tick "Thu'um Reborn" in Nemesis, press Update Engine, then
Launch. No ESP, no scripts -- safe to install and uninstall at any time, mid-playthrough
included.


UNINSTALLATION
--------------

Delete the folder. Everything is written at runtime; nothing is baked into your save.


SETTINGS
--------

SKSE\Plugins\ShoutMCO.ini. Changes apply on your next shout -- no restart. With SKSE Menu
Framework, the same three values are on the in-game page.

  * bTrace = 0 -- leave it off. Turn it on only to capture a log for a bug report.

  * sPowerSource = auto -- where a power attack press comes from: OCPA's key if present,
    otherwise a held attack button. off for Elden Power Attack.

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
