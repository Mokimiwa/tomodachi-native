# Notes for agents

- This repository runs Nintendo Switch games natively on Apple platforms: the game's own code runs, rewritten only where Apple's kernel requires it. Super Mario Bros. Wonder is supported (base game v0 and update 1.2.1, profile `010015100B514000`); Fire Emblem Engage (update 2.0.0, profile `0100A6301214E000`) is in bring-up and already packs into an IPA. Start with [README.md](README.md) for status and build steps.
- Each game's constants and shader tables form a game profile in `dm/games/<id>/`, where `<id>` is the base game's title ID: `game.tsv` plus the tables and `dmsh_chain.h`. Mario's profile is `010015100B514000`. Tools pick the profile with `DM_GAME`; `make -C dm/host` and `build_ipa.sh` pick it with `GAME`. There is no default game: set it every time (the tools stop and list the profiles otherwise). Put per-game values in the profile, not in code ([dm/games/README.md](dm/games/README.md)).
- Per-build knowledge is keyed by the NSO build ID: rewriter cuts in `dm/tools/patch_nso.py`, the profile's `build` rows (the builds the NSP script accepts, and their display versions), and host behaviour that uses fixed module addresses checks `module_t.build_id` (from `fixups.tsv`; `dm_main_build()`, `DM_D2R_BUILD` for code inherited from D2R). When a new game build appears, carry addresses over with `dm/tools/nso_fnmap.py` and check them by disassembly; never let fixed addresses run on a build they were not found in.
- To analyse or port **another** Switch title, first run `scripts/build_ipa_from_nsp.py` on its NSP folder. With no matching profile, it writes a survey to `<NSP folder>/<title ID>_survey/survey.md`. Then follow [docs/agents](docs/agents/README.md) (extraction, binary survey, host bring-up failure catalogue, graphics specifics, debugging recipes) and add a profile.
- Scripts the user runs live in `scripts/` at the repository root (now `build_ipa_from_nsp.py`). Each one runs both in Spyder (F5, configured by the settings block at its top) and from a terminal (command-line options override those settings; non-zero exit on error). Tools that only agents or other scripts call stay under `dm/`.
- Game files, keys and anything extracted from them live in the user's workspace (`~/<id>-work`, Mario `~/010015100B514000-work`) or in the NSP folder's `<id>_build/`, never in this repository. Do not print key values.
- Code comments are bilingual: a Traditional Chinese line, then an English line. Red log lines start with `【紅】`. Keep both conventions. No Simplified Chinese anywhere in the repository (comments, logs, docs, user-facing text).
- After changing anything on the shader translation chain, run the chain procedure for each affected profile. The chain covers `dm/gfx/tools/dmsh_*`, `Program.cs`, the profile's `shader_map.tsv` / `shader_facts.tsv` / `shader_pairs.tsv`, and its `nvn_*` / `shader_*` keys. The procedure:
  1. Run `dmsh_chain.py --header`, which writes the profile's `dmsh_chain.h`.
  2. Rebuild the shaders (Mac and, with `IOSOUT`, iOS).
  3. Regenerate the profile's `c1_full.tsv`.
  4. Rebuild the host.

  The host only loads libraries of its own chain version. The translator is `dm/gfx/tools/dmsh_xlate_bin`, which matches `dmsh_chain.lock`; a fresh `dotnet publish` does not.
- iOS builds are unsigned IPAs that the user sideloads and tests; diagnostics come back as the app's `Documents/log.txt`. Do not sign or install builds on the user's devices.
- Commit only when asked, with the repository's own git identity.
