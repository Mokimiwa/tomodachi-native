<div align="center">

# Tomodachi Native

**Tomodachi Life: Living the Dream as a native app for Apple Silicon Macs.** iPhone and iPad come next.

The game's own ARM64 code runs directly on the Mac's chip; the Switch's system services and graphics are rebuilt on macOS and Metal 4.

[![License: MIT](https://img.shields.io/badge/license-MIT-f4c430?style=flat-square)](LICENSE)
[![macOS 26+](https://img.shields.io/badge/macOS-26%2B-000000?style=flat-square&logo=apple&logoColor=white)](#mac-app)
[![Apple Silicon](https://img.shields.io/badge/Apple%20Silicon-arm64-555555?style=flat-square)](#how-it-works)
[![Fork of nxnative](https://img.shields.io/badge/fork%20of-nxnative-6e5494?style=flat-square&logo=github)](https://github.com/seedds/smbw-mobile)

[English](README.md) · [繁體中文](README.zh-TW.md)

</div>

> [!IMPORTANT]
> This repository contains no game files, keys or other Nintendo content. You need your own copy of the game (its NSP files) and your own `prod.keys`, dumped from a console you own. The project is not affiliated with Nintendo.

This is a fork of [nxnative](https://github.com/seedds/smbw-mobile) by seedds, which runs Switch games natively on Apple devices. This fork focuses on Tomodachi Life: it adds the game's profile, the SDK 20 support it needs, and a Mac app built around it.

## Status

| | |
|---|---|
| **Playable on the Mac** | The island tour, naming the island, creating Miis (by questions or in the editor), with sound |
| **Mac app** | Menus, full screen, pause, screenshots, mouse, trackpad and keyboard, 13 game languages |
| **iPhone and iPad** | Not packaged yet |

## Quick start (Mac)

You need an Apple Silicon Mac on macOS 26 or later, Xcode, the .NET SDK 10, spirv-cross, a Python 3 with `lz4`, `capstone` and `fonttools`, the game's NSP and your `prod.keys`.

1. Bring the game up once: extract it, rewrite its code and translate its shaders, following [Build by hand](#build-by-hand-mac-for-development) with `G=010051F0207B2000` and `VER=v0`.
2. Pack the app. With `--embed` it carries everything, so it runs with the workspace disk unplugged:

```sh
python3 scripts/build_mac_app.py 010051F0207B2000 -w ~/010051F0207B2000-work -o "/Applications/Tomodachi Life.app" --embed
```

3. Open **Tomodachi Life** from Applications.

## The Mac app

- **A real Mac app:** the game's icon, full screen and three window sizes, pause, screenshots saved to your screenshot folder, and a question before quitting.
- **Mouse, trackpad and keyboard:** click anywhere to tap the screen there. Two-finger gestures turn the camera; pinches and three-finger swipes reach the shoulder buttons. Every binding can be changed in `keymap.txt` (**Edit Controls…**, ⌘,).
- **Typing names:** a sheet in the game's own colours, with your input methods and paste.
- **13 languages:** the game follows your Mac's language; switch it under **Game → Game Language**.
- **Saves** live in `~/Library/Application Support/nxnative/010051F0207B2000/save`.

## How it works

- **A rewriter** changes only the instructions that cannot run under Apple's kernel (x18, thread pointers, `svc`, timers).
- **A host program** stands in for the Switch's kernel and system services.
- **The NVN graphics API is reimplemented on Metal 4**, with the game's 14,388 shaders translated ahead of time.

Details per part are in [How it works in detail](#how-it-works-in-detail).

## FAQ

**Do I need a Switch?** You need the game files and keys, dumped from a console you own.

**Which Macs?** Apple Silicon on macOS 26 or later (the graphics layer uses Metal 4). It is developed on macOS 27; macOS 26 is the build target but has not been tried.

**Where are my saves?** In `~/Library/Application Support/nxnative/010051F0207B2000/save`. Copy that folder to back them up.

**Can I use a controller?** Not on the Mac yet: keyboard, mouse and trackpad for now.

**Will other games come?** The code still carries nxnative's profiles for Super Mario Bros. Wonder and Fire Emblem Engage (see [Game profiles](#game-profiles)); this fork works on Tomodachi Life first.

## Known issues

- Clicks have only been tried on the opening menus (naming the island and its confirmation). If a click does nothing in a later scene, add `mouse:Left A` to `keymap.txt` to move the hand with the mouse instead.

## Game profiles

| Game | Profile | NSPs the folder needs | State |
|---|---|---|---|
| Super Mario Bros. Wonder | `010015100B514000` | the base game (1.0.0) and update 1.2.1 | **Playable on the iPhone with sound.** An 11-minute session through several levels ran at 60 fps with no crash. The IPA packs 1.2.1 (`-p v0` packs 1.0.0) and is about 3.9 GB |
| Tomodachi Life: Living the Dream | `010051F0207B2000` | the base game (1.0.0) | **Playable on the Mac with sound.** The island tour, island name and Mii creation work; see [Known issues](#known-issues). Not packaged for iOS yet |
| Fire Emblem Engage | `0100A6301214E000` | the base game and update 2.0.0; DLC NSPs (Expansion Pass) are packed too when present | **In bring-up.** On the Mac it plays the prologue, battles included. The first iPhone run (2026-10-15) went through a new game's opening movie and name entry at 60 fps with no crash. After a trip to the background, one of the game's two audio outputs stopped being fed; this is being investigated. It runs at 60 fps at the right speed: the Switch version runs at 30, and one patch to its code switches it to the 60 fps timing the game already has, so nothing runs too fast. The IPA is about 15 GB |

For any other game, the script writes a survey instead of an IPA (see [Port another game](#port-another-game)).

## Pack an IPA

You need:
- a Mac with Xcode, XcodeGen, the .NET SDK 10, zstd and spirv-cross;
- a Python 3 with `lz4`, `capstone` and `fonttools`;
- your `prod.keys`;
- a folder holding one game's NSPs. The script finds the game by the `[title ID]` in the file names or by the NSP's ticket, and the update by its `[vN]` tag.

Free disk space: about the size of the NSPs plus 6 GB, and room for the IPA.

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

From a terminal (`-h` lists every option):

```
# Super Mario Bros. Wonder → ~/Games/MarioWonder/SMBWMobile.ipa
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys

# the same, written to the Desktop with your own home-screen name
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys -o ~/Desktop/Mario.ipa --app-name "Mario Wonder"

# pack Mario 1.0.0 instead of 1.2.1 (the folder still needs both NSPs: the shader tables cover both versions)
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys -p v0

# Fire Emblem Engage → ~/Games/FEEngage/FEEngage.ipa
python3 scripts/build_ipa_from_nsp.py ~/Games/FEEngage -k ~/keys/prod.keys

# keep the extracted game files (to run the game on the Mac, or to skip extraction next time)
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys --keep-extracted

# a diagnostic build with environment switches baked in (one KEY=VALUE per line)
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys --env-file diag_env.txt
```

In Spyder, open `scripts/build_ipa_from_nsp.py`, fill in the settings at the top (`NSP_DIR`, `PROD_KEYS`, and optionally `PACKAGE`, `OUT_IPA`, `APP_NAME`, `BUNDLE_ID`), and press F5. Empty paths are asked for at run time.

What the script does:
1. It finds the game profile from the title ID.
2. It extracts the NSPs.
3. It rewrites the code modules, checking each build ID against the profile.
4. It splits out the shaders and checks them against the profile's tables.
5. It builds the iOS shader libraries.
6. It calls `build_ipa.sh`.

Everything it writes stays in the NSP folder: intermediate files and logs in `<id>_build/`, the IPA next to the NSPs. The extracted game files are deleted at the end unless you pass `--keep-extracted`. It exits with 0 when done and 1 on an error. For Mario, the first run takes about 35 minutes, mostly the shader libraries; a second run takes about 10.

## Mac app

Once a game has been brought up on the Mac (its workspace holds the RomFS, the rewritten modules and the Mac shader libraries), `scripts/build_mac_app.py` packs it into a double-clickable `.app` with the game's icon:

```sh
python3 scripts/build_mac_app.py 010051F0207B2000 -w /path/to/010051F0207B2000-work -o "/Applications/Tomodachi Life.app" --embed
```

Without `--embed` the app is about 2 MB and reads the game data from the workspace, so the disk holding it must stay attached. With `--embed` the app carries all of it (about 7 GB): cloned at no extra cost on the same APFS volume, copied across volumes. Saves and `keymap.txt` live in `~/Library/Application Support/nxnative/<title ID>/`, the log in `~/Library/Logs/nxnative/`. Keep the app for your own use: it points at (or contains) your game data.

The game language follows the Mac's preferred language (US English when the game lacks it). Change it under **Game → Game Language** (applies the next time the game opens), pin one when packing with `--lang`, or set it from a terminal with `defaults write org.nxnative.<title ID in lower case>.mac GameLanguage en-US`.

In the app:

- **Mouse:** in Tomodachi Life the mouse works like a finger: a click taps the screen where you click, and the right button is B. Other games capture the mouse when you click the picture; it then moves the game's hand, with the left button as A and the right as B, until you press ⌘ (or an unmapped Esc). Set `mouse:Left` in `keymap.txt` to `Pointer` or `A` to switch between the two. The trackpad turns the camera (two-finger scroll or rotate), pinches for ZL / ZR and swipes with three fingers for L / R.
- **Menus:** pause (⌘P), screenshot to your screenshot folder (⇧⌘S), window sizes (⌘1–⌘3), full screen (⌃⌘F, or the green button), Edit Controls (⌘,) to open `keymap.txt`, which is reread when you come back to the game. Closing the window asks before quitting.
- **Text entry:** names and other text are typed in a sheet over the game, with paste and your input methods.

## Install and play

Sign and install the IPA with your own tool (AltStore, Sideloadly, LiveContainer, ...). The app takes its name and icon from the base game NSP, and its bundle ID is `org.nxnative.<base game title ID>` (for example `org.nxnative.0100a6301214e000`); `APP_NAME` / `BUNDLE_ID` in the script override them. Your signing tool may replace the bundle ID.

- **Controls:** a translucent touch gamepad over the picture:
  - ZL above L in the top-left corner, ZR above R in the top-right;
  - − and + side by side at the top centre;
  - the left stick in the bottom-left corner, and A/B/X/Y (Nintendo positions) in the bottom-right, the same size and mirrored.

  There is no on-screen D-pad or right stick. A connected physical controller hides the touch gamepad and keeps every button, mapped by position.
- **Saves and logs** live in the app's Documents, visible in the Files app: `save/`, `log.txt` (the previous run in `log.prev.txt`) and `pso_user.tsv`. When reporting a problem, send `log.txt`.
- **Reinstalling:** sideloading tools may give a reinstalled app a new data folder; LiveContainer did, under its `Data/Application/<UUID>/Documents`. To keep your saves and prewarm list, copy them from the old folder while the app is closed.
- **Switches without rebuilding:** environment switches can go in `Documents/env.txt` (one `KEY=VALUE` per line). Game files are also accepted at `Documents/romfs`.

The first time each new scene appears, it can stutter while Metal compiles its pipelines; later launches prewarm them (see [Prewarm lists](#prewarm-lists)).

## How it works in detail

| Part | State |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`, `DM_RULESET=native`) | Runs unchanged on every module of both games. Per-build cuts are keyed by the module's build ID |
| Host (`dm/host`) | Stands in for the Horizon kernel and the system services the games call: threads and synchronization, memory, file system with transactional saves, HID, audio, movies (VP9 through libvpx), software keyboard, focus and lifecycle |
| Graphics (`dm/gfx`) | NVN on Metal 4. Shaders are extracted by `shader_split.py` from Nintendo's containers and from Unity's `Shader` objects, then translated offline. Mario: 16,266 of 16,268 shaders compile. Engage: 9,268 of 9,269. Tomodachi Life: 14,381 of 14,388 |
| Audio | Games that carry the SDK's audio renderer (Mario, Tomodachi Life) run it in-process; games that use AudioOut directly (Engage: Wwise) feed it straight. Output goes through Core Audio (default on iOS, `DM_AUDIO=1` on the Mac) |
| Input | Every source ends up as one virtual Switch controller (buttons, both sticks, touch points); `dm/host/dm_bind.h` maps sources to it from a text table. iOS: the touch gamepad and physical controllers. Mac: `DM_WINDOW=1` for a window with keyboard, mouse and trackpad input (remap with `DM_KEYMAP=<file>`, default `$DM_ROOT/keymap.txt`; mapping `mouse:Left` to `Pointer` makes the mouse the touchscreen), or scripted input (`DM_KEYS=<seconds>:<control>,…` with buttons, stick directions such as `StickLUp~2` and `Touch=x/y`, and `DM_KBD=<text>` answering the system keyboard) |
| iOS app | One unsigned IPA for iPhone and iPad. The rewritten code is embedded in the executable; the shader libraries, the RomFS and the game icon are inside the app |
| Known gaps | Sparse virtual-pool mappings; `DrawElementsIndirect`. TBDR optimizations are off by default (they don't understand bindless textures yet). No amiibo. The bundled prewarm lists are empty. On the Mac, Mario's menu and dialogue text render blank (fine on the iPhone) |

## Port another game

Each game has a profile in `dm/games/<id>/`:
- `game.tsv` holds the title ID, known builds, code modules, RomFS marker, memory split, NVN driver tables, app name and bundle ID;
- the shader tables: `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `c1_full.tsv`, `pso_manifest.tsv`;
- `dmsh_chain.h` holds the chain version.

The shader tools pick a profile with `DM_GAME=<id>`; `make -C dm/host` and `build_ipa.sh` pick it with `GAME=<id>`. Profiles are named after the base game's title ID. There is no default game: without `GAME` / `DM_GAME` the tools stop and list the profiles. `python3 dm/games/game.py list` lists the profiles. Host code that relies on one build's fixed addresses checks that build's ID.

To add a game:
1. Run `scripts/build_ipa_from_nsp.py` on its NSP folder. With no matching profile, it writes a survey to `<title ID>_survey/survey.md`. The survey covers the modules, SDK, engine, containers, rewrite sites, and the imports compared with the host's hooks.
2. Add a profile as described in [dm/games/README.md](dm/games/README.md).
3. Bring the game up following [docs/agents](docs/agents/README.md): extraction, binary survey, a catalogue of host bring-up failures, graphics, and debugging recipes.

## Build by hand (Mac, for development)

You need the same tools as the script. The shader translator is the prebuilt `dm/gfx/tools/dmsh_xlate_bin`, the build that matches `dmsh_chain.lock`; a fresh `dotnet publish` gives different fingerprints. The commands use three variables:
- `G`, the profile;
- `W`, the game's workspace outside this repository;
- `VER`, a version folder named after the NSP version.

```
G=010015100B514000; W=~/$G-work; VER=v327680
# 1. extract: the base game alone (update -), or an update layered over it (the last argument lets unchanged files become clones)
dotnet build dm/tools/nx_extract -c Release -o $W/build/nx_extract
$W/build/nx_extract/nx_extract <prod.keys> <base NSP> - $W/data/v0
$W/build/nx_extract/nx_extract <prod.keys> <base NSP> <update NSP> $W/data/$VER $W/data/v0/RomFS
# 2. rewrite the profile's code modules
for m in $(python3 dm/games/game.py $G modules); do DM_RULESET=native python3 dm/tools/patch_nso.py $W/data/$VER/ExeFS/$m $W/build/patched_$VER $m; done
# 3. shaders: extract from every version in the profile's shader_versions (comma-separated, base first), then translate into <cache>/b-<chain version>/
DM_GAME=$G python3 dm/gfx/tools/shader_split.py $W/data/v0/RomFS,$W/data/$VER/RomFS $W/data/v0/ExeFS/main,$W/data/$VER/ExeFS/main $W/shader/pkg dm/games/$G/shader_map.tsv dm/games/$G/shader_facts.tsv dm/games/$G/shader_pairs.tsv
DM_GAME=$G python3 dm/gfx/tools/dmsh_chain.py --header
DM_GAME=$G python3 dm/gfx/tools/dmsh_build.py $W/shader/pkg $W/build/cache/metal_shader_cache
python3 dm/gfx/tools/c1_guard.py $W/build/cache/metal_shader_cache/b-<chain version> dm/games/$G/c1_full.tsv
# 4. the host, and the button-icon font for the Nintendo-extension shared font
make -C dm/host GAME=$G dmhost
python3 dm/tools/make_ext_font.py $W/build/font/nintendo_ext.ttf
# 5. run on the Mac
DM_ROOT=$W DM_ROMFS=$W/data/$VER/RomFS DM_SAVE=$W/run/save DM_TEMP=$W/run/temp DM_MSL_DIR=$W/build/cache/metal_shader_cache DM_FONT_DIR=$W/build/font dm/host/dmhost $W/build/patched_$VER
# 6. iOS shader libraries (about 15 min for Mario), then the IPA (default ~/Desktop/<the profile's ipa>)
IOSOUT=$W/build/ios/dmsh_b DM_GAME=$G python3 dm/gfx/tools/dmsh_build.py $W/shader/pkg $W/build/cache/metal_shader_cache
GAME=$G dm/ios/tools/build_ipa.sh [out.ipa]
```

Notes on these steps:
- `build_ipa.sh` takes the rewritten modules from `$W/build/patched_<package>` and the RomFS from `$W/data/<package>/RomFS`; `PATCHED` and `ROMFS` override them.
- It also bundles a standard font (macOS's Arial Unicode, `STD_FONT=` overrides it) and the generated `nintendo_ext.ttf`.
- The icon is the `icon.jpg` that `nx_extract` saves next to `RomFS/`; `DM_ICON=` points at another image.
- `DM_ENV_FILE=` bakes environment switches into the app, and `DM_DISPLAY_NAME=` renames it.
- If Xcode's Locations setting puts products in a custom place, the script finds the app through the build log.
- hactool cannot read every update (Mario 1.2.1's patch NCA gives "Invalid BKTR layout!"), which is why `nx_extract` uses LibHac.

## Prewarm lists

Metal compiles each combination of shaders and pipeline settings for the GPU the first time it is used. Each takes tens of milliseconds, so a burst of them on entering a level is a visible freeze. The host records what it had to compile and rebuilds those pipelines on a background queue at the next launch:

- `pso_user.tsv` sits next to the save directory: `Documents/pso_user.tsv` on iOS, the parent of `DM_SAVE` on the Mac (`DM_PSO_USER=path` overrides it). It is appended while playing, one line per item built on the spot: a shader (`S`), a render pipeline (`R`, the hex of `pso_skey` in `dm/gfx/nvn/nvn_pso_skey.h`) or a compute pipeline (`C`).
- `dm/games/<id>/pso_manifest.tsv` is the seed list bundled with the app, prewarmed before `pso_user.tsv`.

The lines name shaders by content key and hold no compiled code and nothing personal, so a list recorded on one device works on any other. To seed the bundled list:
1. Record on the Mac with prewarm off, so everything is built on demand: `DM_PSO_MANIFEST=off DM_PSO_RECORD=<file>`, playing the scenes players reach first. Engage's list covers boot, the opening movie, the title, a save load and the prologue battle.
2. Append the recorded lines below the header comments, in the recorded order. Duplicates are dropped at load.
3. Rebuild the IPA.

Record on the game version you ship. At launch the log names each list it read and its line count (`管線預熱清單 … 行`), or says that `pso_user.tsv` does not exist yet.

## Game notes

**Super Mario Bros. Wonder (`010015100B514000`)**
- **Versions:** v0 (1.0.0) and 1.2.1 (v327680). With the update layered over v0, unchanged files become APFS clones, so the merged RomFS adds only about 114 MB to the 3.5 GB base.
- **Shaders:** 12,811 in v0 and 16,268 across both versions; the tables hold both.
- **1.2.1:** it is a new `main` build (SDK 16.2.12), whose resource-heap cut (+16 KB slack) was carried over with `dm/tools/nso_fnmap.py`. It also makes new SDK calls, which the host answers: `nn::nfp` finds no amiibo, `pl::IsRunningOnOunce` is false, the shop call is a no-op, and `account::PopOpenUsers` returns none.
- **Mac input:** `DM_KEYS="12:A,14:A,20:A,26:A,32:A"` reaches the opening course.

**Fire Emblem Engage (`0100A6301214E000`)**
- **Engine:** Unity 2020.3 IL2CPP, SDK 12.3.7. The shaders are read out of Unity bundles (`unityfs.py`).
- **Memory:** a 5 GB guest block, the total proven on a phone, with a 2.5 GB application pool. A per-build cut shrinks the SDK's address-region node area to 62 MB.
- **Audio:** Wwise drives AudioOut directly.
- **Movies:** VP9, decoded on the host.

## Tools (`dm/tools`)

| Tool | What it does |
|---|---|
| `nx_extract/` | NSP → ExeFS / RomFS / icon with LibHac, merging an update over the base game (C#, `dotnet build`). `--manifest` writes ExeFS and a RomFS file list only, for surveys |
| `nso_fnmap.py` | Carries offsets from one build to another (function matching with addresses masked; globals via their references) |
| `nso2elf.py` | Converts NSO to ELF for Ghidra / REA |
| `insn_census.py` | Counts the instruction sites the rewriter must handle |
| `nso_syms.py` | Compares the SDK import surface of two games, or of one game (`-` for the reference) with the host hooks |
| `nvn_slots_got.py` | Builds the NVN fetch table and call sites (GOT-indirect loaders) |

## Acknowledgements

This project is built on other people's work:

- **[nxnative](https://github.com/seedds/smbw-mobile)** (MIT) by seedds: this repository continues it, with Tomodachi Life and the Mac app added.
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative (first called smbw-mobile) started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2 in `dm/third_party/ryujinx`, with one change: the FMZ multiply mode (`Instructions/InstEmitFloatArithmetic.cs`, where a zero factor makes the product 0 as on Maxwell)) turns the games' Maxwell shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **yuzu**: Maxwell instruction encodings for the shader reference tool, and a cross-check for kernel behaviour.
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`. **hactool** did the first extractions.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language.
- **deko3d** (zlib): its texture layout size algorithm is ported in `dm/gfx/nvn/nvn_tex.c`.
- **libvpx** (VP9 movies), **libdeflate**, **bcdec** and **NVIDIA FLIP** ship in `dm/third_party` with their licenses ([list](dm/third_party/README.md)).
- **Capstone**, **lz4**, **fontTools** and **XcodeGen** are used by the build.

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license; see [dm/third_party](dm/third_party/README.md).
