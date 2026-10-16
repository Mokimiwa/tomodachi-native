# Porting a Switch title: agent guide

These pages are for an agent asked to analyse a Switch NSP and port it with the native-execution approach of this repository and [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile): the game's own aarch64 code runs natively on Apple hardware, a rewriter patches only the instructions Apple's kernel cannot run, the host replaces Horizon system services, and NVN is reimplemented on Metal.

Everything here was learned on two titles: Diablo II: Resurrected (SDK ~12, NVN 53, the original target of d2r-mobile) and Super Mario Bros. Wonder (SDK 16.2.0, NVN 55, the first target of nxnative, whose profile now lives upstream; its update 1.2.1 uses SDK 16.2.12). Where the two differed, both are given; a third title will differ again, so treat every number below as "measure it, don't assume it".

| Page | Covers |
|---|---|
| [01-extract.md](01-extract.md) | Keys, NSP → NCA → ExeFS / RomFS, update NSPs, the workspace layout |
| [02-survey.md](02-survey.md) | Static survey of a new title before touching the host: SDK version, imports vs. host hooks, SVCs, instructions to rewrite, NVN surface, data containers, Ghidra / REA setup |
| [03-bringup.md](03-bringup.md) | Getting the host to boot the game: the run loop, how to read the log, and a catalogue of every failure pattern met so far with its root cause and fix |
| [04-graphics.md](04-graphics.md) | NVN on Metal for a new title: object sizes, shader containers and keys, binding layouts, bindless textures, depth mode, index sizes, swizzle, transient render targets, the diagnostic switches |
| [05-debugging.md](05-debugging.md) | Recipes: crash reports, lldb scripts, Ghidra headless decompilation, objdump, memory growth, render-pass and texture dumps, on-device diagnostics, offline pipeline link checks |

## Ground rules

- **Bring your own files.** Keys, NSPs and anything extracted from them stay in the user's workspace (`~/<title>-work`), never in the repository, never in a commit, never printed. Print key *names* and lengths only.
- **Measure, then change.** Each fix in this repository started from a concrete observation (a crash address and its decompilation, a census over all shaders, a memory profile). Write the evidence into the code comment next to the fix, the way the existing code does; the next title will need it.
- **One repository, one profile per title.** nxnative (first called smbw-mobile) started as a copy of d2r-mobile's generic parts. Title- or SDK-specific constants live in the game profile `dm/games/<id>/` ([dm/games/README.md](../../dm/games/README.md)). That covers program ID, known builds, modules, address-space partition, NVN driver tables, the shader tables and the app identity. The code reads the profile through `game_profile.h` (host) or `dm/games/game.py` (tools). Behaviour that fits only one title's code is gated by that title's build ID (`dm_main_build()`), never left on for every title. Some things are still built into the translator: the texture / sampler handle bases and the depth mode (see [04](04-graphics.md)). `dmsh_build.py` refuses a profile whose bases differ from the translator's.
- **Keep the bilingual comment style** of the code base (Chinese line, then the English line), and the `【紅】` prefix for red log lines that mark something unimplemented or wrong.
- **Test where it runs.** The Mac host (`dm/host/dmhost`) is the development loop; the iOS app reuses the same host and graphics code. Never claim something works on iOS without running it there.

## Checklist for a new title

1. Run `scripts/build_ipa_from_nsp.py` on the title's NSP folder. With no profile for its title ID, it writes `<NSP folder>/<title ID>_survey/survey.md`:
   - modules and build IDs, SDK version and components, engine and graphics API markers;
   - RomFS containers;
   - the rewrite-site census and imports vs. host hooks.

   It extracts only the code modules plus a RomFS list.
2. Extract base (and update, if in scope) — [01](01-extract.md). Finish the survey — [02](02-survey.md). Write down: SDK version, NVN API version, module list, imports the host lacks, SVCs, instruction census, NVN loader pattern, shader containers.
3. Add a game profile `dm/games/<title ID>/game.tsv`, named after the base game's title ID (start from `010051F0207B2000/game.tsv`: identity, `build` rows, `modules`; keep the memory / NVN values until they are measured) and build the host for it with `make -C dm/host GAME=<id>`.
4. Run the rewriter (`DM_RULESET=native`) and check its counts against `insn_census.py`.
5. Boot on the Mac and iterate on the failure catalogue in [03](03-bringup.md) until the game reaches its frame loop.
6. Extract and translate the shaders, find the binding layout by census, and get the first frame — [04](04-graphics.md).
7. Check memory over a minute of running (`footprint`, the host's memory report line); fix growth before moving on.
8. Input, audio, then the iOS bundle (d2r-mobile's `dm/ios`, RomFS inside the app).
9. Test on a phone: an unsigned IPA the user sideloads, switches through `env.txt`, results through `Documents/log.txt` ([05](05-debugging.md)). Several bugs only showed there (the command-buffer byte and the audio buffer size in [03](03-bringup.md), pipeline link failures in later levels in [04](04-graphics.md)).
10. For a game update: extract it over the base game, diff the imports, carry the per-build addresses over, and rebuild the shader tables for both versions ([01](01-extract.md)).
