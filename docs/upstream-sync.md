# Upstream sync

This fork's history is unrelated to upstream nxnative ([seedds/smbw-mobile](https://github.com/seedds/smbw-mobile), remote `upstream`), so upstream work is ported by hand. This page records what was compared, what was ported, and what was left out and why. The next sync starts from the last upstream commit listed under "Synced up to".

- **Our tree started from:** upstream `fca6a64`.
- **Synced up to:** upstream `20cba12` (2026-10-17), the 11 commits `fca6a64..20cba12`.
- **Rule:** port what any game can use (host HLE, NVN replay, loader / rewriter rules, tools, diagnostics, iOS front end). Leave out upstream's game profiles and the per-build cuts and gates for their games. Where both sides changed the same thing, our variant stays (transactional saves, SDK 20 audio, InlineKeyboard, DM_KEYS syntax, PackagedTextureData, attachment feedback copies).
- **Comments:** upstream's comments are English-only from `20cba12` on. Ported code carries our bilingual comments (a Traditional Chinese line, then an English line), translated from upstream's earlier bilingual text.

## How to repeat this

1. `git fetch upstream`, then list `git log <last synced>..upstream/main`.
2. Upstream's comment-only edits make plain diffs useless. Compare files with comments stripped (C: `//` and `/* */`; Python: comments and docstrings) to find real changes.
3. Hook coverage: collect the mangled names in `HLE(...)` / `HLE_FORCE(...)` on both sides. Our `SETCB(...)` in `hle_swkbd_inline.c` registers hooks through a macro argument, so add those by hand.
4. To see which hooks Tomodachi Life really calls, boot with `DM_IMPORTS=1` and read the `force <name>` lines; a name only in `sdk`'s symbol table is not a call.

## Hooks

Upstream had 43 hook names we lacked (`fs` 4, `oe` 5, `pl` 4, `hid` 7, `nfp` 8, `nifm` 2, `time` 2, `swkbd` 2, `settings` 2, `mem`, `pctl`, `prepo`, `account`, `audio`, `vi`, `movie` 1 each); we had 36 they lack (our SDK 20 bring-up: `mii`, `album`, `ldn`, `ngc`, `ssl`, region code, transactional save sizes, more `time` calls, `ro::ProtectRelro`). `InlineKeyboard::SetDecidedCancelCallback` was in fact already ours through `SETCB`.

So 42 were really missing; 41 are ported, in the host files that hold their neighbours (`ec6d5fe`, plus `InlineKeyboard` D1 on our own keyboard and `movie::Decoder` D1 with the movie work in `999b6c3`). Left out: `nn::mem::StandardAllocator::Allocate(size)`, which only changes behaviour on 13 Sentinels 1.10 and would route every Tomodachi Life `malloc` through the host.

Tomodachi Life newly resolves five of them at load (`fs::DeleteDirectory`, `vi::GetIndirectImageMemoryRequirement`, `pctl::CheckStereoVisionPermission`, `account::ListAllUsers`, `settings::operator==(LanguageCode, Language)`); each replaces a path that would have reached a missing Horizon service.

## Per commit

Kinds: **generic** (any game can use it), **game** (a profile, or code gated to one upstream build), **docs**.

| Upstream | Piece | Kind | Here |
|---|---|---|---|
| `a9c56fd` Metroid Dread | profile `010093801237C000` | game | left out |
| | front patch area for a giant bss (`patch_nso.py` FAR, loader, iOS embedding, QueryMemory, used memory) | generic | `20b7ead`; Tomodachi's rewriter and `gen_embed.py` outputs byte-identical; a synthetic sdk with 104 MB more bss boots |
| | observation gates right after a `bl` | generic | `20b7ead` |
| | Dread's movie-ring OBS gates 19 / 20 and `movie_ring_wait` | game | left out |
| | hooks `FinishStartupLogo`, `CommitSaveData`, `DeleteDirectory` (`SendVibrationValues` was already ours) | generic | `ec6d5fe` |
| | guest paths resolve `.` / `..` | generic | `f61ceba` |
| | movies: native demuxer, native-texture decoder mode, D1, copied events, MediaCodec-like Flush / Stop, deferred input signal | generic | `999b6c3` (Tomodachi imports no `movie::`) |
| | same-core preemption approximation, profile key `core_preempt` | generic | `f8efcee`, off for Tomodachi |
| | precise SleepThread | generic | `f61ceba` |
| | `DM_KEYS` `*count/interval` | generic | `f61ceba`, on top of our own syntax |
| | per-stage `BindProgram`, in-place `WriteTexels`, Z lines | generic | `f3d316d` |
| | `shader_split.py` MSHD containers | generic | `758ba5c` |
| `c7e5b57` 13 Sentinels | profile `01003FC01670C000` | game | left out |
| | hooks `GetMouseState`, `CleanDirectoryRecursively`, `EnableGlobalFileDataCache`, `SetUserInactivityDetectionTimeExtendedUnsafe`, `ToPosixTime`, `GetAudioOutName` | generic | `ec6d5fe` |
| | `StandardAllocator::Allocate` 64 KB alignment (one build) | game | left out |
| | triangle fans, indexed quads, streams 14 / 15, `InsertDebugMarker` | generic | `6978b9a` |
| | `TextureBuilderSetPackagedTextureData` | generic | ours already |
| | interval-2 frames held for their period, 30 fps target after one second of interval 2, `DM_TEST_PRESENT_LAG` | generic | `6978b9a`, our variant: half-tick hold threshold |
| | `t_mip_extent` / `t_present_crop` build again | generic | `6978b9a` |
| | `shader_split.py` GLSLC outputs and separable pairs; survey note | generic | `758ba5c` |
| `42a50ef` Super Mario 3D World | profile `010028600EBDA000` | game | left out |
| | profile keys `device_api`, `shared_font`; short control sections in shader keys | generic | `3b28d70`; all 14,388 Tomodachi controls are 0x880+ |
| | `HLE_FORCE_BIG` / `hle_bigstack.c` for fs hooks | generic | `6bb678d` |
| | compatible texture views (`tex_fit`) | generic | ours already (`tex_type_fix`); its two extra cases added in `3b28d70` |
| | unwritten colour outputs masked, 1D as one-row 2D, `QueueAcquireTexture`, `SamplerCompare`, `TextureGetStorageClass`, NVN 53 debug groups, PASSDUMP extras | generic | `3b28d70` (`TextureGetSwizzle` was ours) |
| | hooks: InlineKeyboard, LanguageCode, `ListAllUsers`, oe perf / album, nifm SDK 10, prepo session, vi indirect image | generic | `ec6d5fe`; InlineKeyboard stays ours, plus D1 |
| | NSP script builds nx_extract in a temp folder; `DM_KEYS` left stick | generic | `758ba5c`; sticks were ours already |
| | `shader_split.py` Yaz0, BAHS v9, headerless compute | generic | `758ba5c` |
| `d6cd8a8` Breath of the Wild | profile `01007EF00011E000`, 60 fps cuts | game | left out |
| | DT_RELR in the loader and `DM_LOADHASH` | generic | ours already |
| | DT_RELR in `nvn_slots_got.py` | generic | `758ba5c` |
| | deferred texture binds read at execution | generic | `b72dcba` |
| | a texture at two colour slots attached once | generic | `b72dcba`, our variant: pick the slot by write mask, move its clear |
| | cube textures as 6-layer 2D arrays | generic | left out: `tex_type_fix` makes cube-array views from cubes, which Metal accepts here |
| | NVN calls on the big host stack | generic | `6bb678d` |
| | pool lookup index | generic | `b72dcba` (Tomodachi submit CPU per frame about 5.4 → 2.6 ms) |
| | SDK 20 audio layout, ARUID hook | generic | ours already (`04502e6`, `hle_sdk20.c`) |
| | nfp / VR / seven-six-axis / steady clock / launch parameter / stereo vision hooks | generic | `ec6d5fe` |
| | compressed render targets skipped | generic | left out: our block-view proxies draw them |
| | `DM_KEYS` frame numbers; TEXDUMP 2D-array layers; PASSDUMP skipped passes | generic | frames were ours; the rest `b72dcba` |
| | NSP script: skip empty NSPs, one copy per DLC | generic | `758ba5c` |
| `ff74840` | segmented submits; SDK 20 audio through the locked path | generic | ours already (`519b5db`, `04502e6`) |
| `734a286` | `DM_NVN_KEYDUMP`, PASSDUMP sampler / swizzle detail | generic | `1ee6cad` |
| `b65c8b2` | 3D touch gamepad, profile key `pad_layout` | generic | `a26e76c` (compiled for iOS, not run) |
| `59e40f3`, `e6e6aab`, `e6eff3a` | player-facing README, `docs/development.md` | docs | left out (our README is our own) |
| `20cba12` | same-pass feedback snapshot (`feedback_snap`, `DM_NVN_FB_SNAP`) | generic | left out: our `fbx_prepare` copies any sampled attachment for every shader |
| | `DM_NVN_MAGSCAN` device self-check | generic | left out: built for 3D World's magenta blocks; port it if a device shows them |
| | English as the default game language | generic | left out: a product choice; ours stays |
| | English-only comments, zh-CN twins removed | docs | left out: our comments stay bilingual |
| all | `docs/agents` lessons | docs | `2e66281`, with notes where we differ |

## Left out

- **Upstream's game profiles and their per-build code**: Metroid Dread, 13 Sentinels, Super Mario 3D World + Bowser's Fury, Breath of the Wild (profiles, 60 fps cuts, Dread's movie-ring gates, the 13 Sentinels allocator hook).
- **Where ours already does the job**: `tex_fit` (our `tex_type_fix`), cube textures as 2D arrays (cube-array views from cubes work here), the compressed render-target skip (block-view proxies), `feedback_snap` (our `fbx` copies), DT_RELR in the loader, SDK 20 audio, ARUID, PackagedTextureData, `TextureGetSwizzle`, InlineKeyboard, `DM_KEYS` frames and sticks.
- **Diagnostics tied to one symptom**: `DM_NVN_MAGSCAN`.
- **Product and documentation choices**: English as default language, English-only comments, the player-facing README split.

## Checks

- Tomodachi Life's rewriter outputs (`main`, `sdk`) and `gen_embed.py` output are byte-identical to before (only `build_src.sha256` changes).
- The new `shader_split.py` on Tomodachi Life's v0 RomFS and `main` gives `shader_map` / `shader_facts` / `shader_pairs` data rows byte-identical to the profile's (the NSP script's own check), so the shader chain is untouched.
- Headless boots after each step from the app save reach the 3D room with the known red lines (`8 位索引緩衝…`, `貼圖級數 15…`). Some boots crash early in the font engine (`main+0x10b2838` ← `main+0x264ad8`), a known race: 1 of 4 stress boots of the new host, 2 of 4 of the old one. One boot with `DM_NVN_PASSDUMP` on showed no first frame within 30 s (main thread waiting on a sync object); it did not happen again in the following runs.
- Graphics unit tests: the 15 that passed before pass; `t_mip_extent` and `t_present_crop` build again and fail the same checks as the tree before this sync; the other six fail as before.
- Host unit tests that build: pass. `t_condvar` / `t_condvar_old` pass when built by hand (their Makefile rules lack the profile include path); `t_movie` and several others still do not build, as before.
- Not checked: iOS (the gamepad only compiles), a ProMotion-free display for the interval hold, upstream's games.
