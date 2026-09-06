# Thu'um Reborn

Full MCO support for shouts. Press attack during a shout and the attack comes out at once,
resuming your MCO combo where it left off rather than restarting it; press shout during a swing
and the swing lands first, so the hit is no longer eaten. Shout during a shout and the first
one's exhale is cut rather than played out.

This is the source. The mod is on Nexus as *Thu'um Reborn*, by **Leitmotives**, and it is a
successor to BOTuser998's
[Thu'um - Fully Animated Shouts](https://www.nexusmods.com/skyrimspecialedition/mods/50559),
whose assets it carries with credit. BOTuser998 had no involvement in this.

The download is one file: the SKSE plugin, its INI, a Nemesis patch, and the animation pack.
The engine never inspects which animation is playing; the animations ship in the same download.

Targets Skyrim SE 1.5.97 and AE 1.6.1170. **Not VR** — `ENABLE_SKYRIM_VR` is off, deliberately.

## How it works

The chain out of a shout is two transitions the vanilla shout graph already has, fired in order:
`shoutStop` returns the exhale to ready, and ready already accepts `attackStart`. The window that
opens on is measured rather than annotated — the engine times `Voice_SpellFire_Event` to
`shoutStop` on each shout and caches that per shout, stance, draw state and word count, then
opens the cancel window on the last `iChainWindowPct` percent of the next one. That is a fixed
45 and is not offered to players: it trades responsiveness against how much of the shout
animation you see, which is one number pulling against both things the mod is for.

A percentage rather than a fixed count, because clip lengths differ by 4x across packs and even
within one. At a fixed 300 ms the player was released for 29% of a one-word shout and 7.6% of a
three-word one, so the longer and heavier the animation the less of its recovery they got back.

The reason none of this reads the animation's annotations: shout packs in the wild do not carry
any. Across 137 files from two independent authors, 133 carry nothing at all and the four
exceptions carry `animmotion` root-motion entries, which are motion data rather than events.
Anything built on annotations would require every pack author to re-annotate.

A behaviour patch does ship — `nemesis/` roots the player during the shout, which the graph
cannot do on its own — so Nemesis is a requirement and a run is needed after installing.

## Requirements

SKSE64, Address Library, Nemesis, Payload Interpreter, Open Animation Replacer, ADXP | MCO,
State Behavior Framework, and SYHO - Shout Your Heart Out. One Click Power Attack NG is optional
and auto-detected.

**SKSE Menu Framework is optional.** With it installed the mod registers an in-game page under
*Thu'um Reborn* for tuning the feel values live; without it the plugin logs one line and the INI
is the whole configuration surface. The menu writes that same INI, so the two never disagree.

## Build

Requires MSVC 2022, CMake, and a vcpkg toolchain with `VCPKG_ROOT` set.

```bash
git submodule update --init --recursive
cmake --preset ALL && cmake --build build --preset ALL-Release
```

The build stages `ShoutMCO.dll`, `ShoutMCO.ini` and the public `ShoutMCO_CastIntent.h` into
`dist/`.

The host test suite is a standalone CMake project that pulls in no game SDK:

```bash
cmake -S tests -B build-tests && cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

## Package

```bash
pwsh -File tools/package.ps1
```

Builds a release folder and a matching `.zip` from a **named allow-list** of files rather than a
directory tree, then asserts the archive contents afterwards and fails if anything forbidden got
in. The version is read from `CMakeLists.txt` and checked against the built DLL's own version
resource, so the folder name cannot drift from the binary.

`-Deploy` installs into an MO2 mods folder. It is opt-in, previews what it will overwrite, and
prompts before writing. `-ModsRoot` defaults to the author's instance; pass your own.

## Driving a shout from another mod

`include/ShoutMCO_CastIntent.h` is the public ABI. A mod that casts a shout by its own route —
a hotbar, a controller binding — announces the intent through it, and the engine then treats that
cast as a shout for chaining purposes. The header ships beside the DLL as well as here, so a
driver author does not need this repository.

## Naming

`ShoutMCO` is the internal identifier: it fixes `ShoutMCO.dll`, `ShoutMCO.ini`, the SKSE log
filename and the ABI header. *Thu'um Reborn* is the public name. The two are deliberately
different and both are permanent: the internal one is what a driver links against, so it does
not follow the public one when that changes.

## Licence

GPL-3.0 — see [`LICENSE`](LICENSE). This links CommonLibSSE-NG, whose modding exception covers the
game's own code and not this plugin, so the plugin is a covered work and its source has to reach
anyone who receives the binary. That is why this repository exists.

Note that CommonLibSSE-NG's own `LICENSE` file is the *original* CommonLibSSE's MIT and is not the
licence this builds under; its `COPYING` and `EXCEPTIONS.md` are.

The bundled animations are not covered by that: they come from Thu'um - Fully Animated Shouts
and SYHO - Shout Your Heart Out. `CREDITS.txt` in the release archive records the provenance and
permissions for every author whose work travels with it.
