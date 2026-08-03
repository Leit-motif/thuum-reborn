# Shouts for MCO

Chain any shout animation into an MCO attack. **Ships no animations** — it is an engine, and it
works with whatever shout pack you already have.

This is the source. The mod itself is on Nexus: *Shouts for MCO*, by **Leitmotives**.

Press attack during a shout and the attack comes out immediately, resuming your MCO combo where
it left off rather than restarting it. Press shout during a swing and the swing lands first, so
the hit is no longer eaten.

## How it works

Two transitions that already exist in Skyrim's own shout graph, fired in order: `shoutStop`
returns the exhale to ready, and ready already accepts `attackStart`. There is no behaviour graph
patch and no Nemesis run beyond the one your load order already does.

Because the engine never inspects which animation is playing, it works with any pack without that
pack's author changing anything. Shout animation packs carry zero annotations — verified across
137 files from two independent authors — and this is built so that stays true.

Targets Skyrim SE 1.5.97 and AE 1.6.1170. **Not VR** — `ENABLE_SKYRIM_VR` is off, deliberately.

## Build

Requires MSVC 2022, CMake, and a vcpkg toolchain with `VCPKG_ROOT` set.

CommonLibSSE-NG is not vendored here. Clone it into `extern/` first:

```bash
git clone --recursive -b ng https://github.com/alandtse/CommonLibVR.git extern/CommonLibSSE-NG
```

Then:

```bash
cmake --preset ALL && cmake --build build --preset ALL-Release
```

The build stages `ShoutMCO.dll` into `dist/SKSE/Plugins/`.

## Package

```bash
pwsh -File tools/package.ps1
```

Builds `release/Shouts for MCO <version>/` and a matching `.zip` from a **named allow-list** of
files rather than a directory tree, then asserts the archive contents afterwards and fails if
anything forbidden got in. Version is read from `CMakeLists.txt` and checked against the built
DLL's own version resource, so the folder name cannot drift from the binary.

`-Deploy` installs into an MO2 mods folder. It is opt-in, previews what it will overwrite, and
prompts before writing. `-ModsRoot` defaults to the author's instance; pass your own.

## Naming

`ShoutMCO` is the internal identifier — it fixes `ShoutMCO.dll`, `ShoutMCO.ini` and the SKSE log
filename. *Shouts for MCO* is the public name. The two are deliberately different.

## Design decisions

`docs/adr/` records the three that constrain everything else: consume MCO rather than reimplement
it, never inspect shout cooldown state, and why the chain window is end-relative.

## Licence

Published here to satisfy GPL-3.0: this links CommonLibSSE-NG, whose modding exception covers the
game's own code and not this plugin, so the plugin is a covered work and its source has to reach
anyone who receives the binary.

GPL-3.0 — see [`LICENSE`](LICENSE). This links CommonLibSSE-NG, which is GPL-3.0 with a modding
exception. Note that CommonLibSSE-NG's own `LICENSE` file is the *original* CommonLibSSE's MIT and
is not the licence this builds under; `COPYING` and `EXCEPTIONS.md` are.
