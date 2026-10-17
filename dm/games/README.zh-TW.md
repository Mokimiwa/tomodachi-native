# 遊戲檔案

本移植認得的每個遊戲都有一個目錄 `dm/games/<名>/`，以基礎版的標題號命名：《朋友收集》是 `010051F0207B2000`。目錄裡有：

- **`game.tsv`**：這個遊戲的常數，每行 `鍵<TAB>值`，注釋獨佔一行。
- **著色器表**（`shader_split.py` 寫出）：`shader_map.tsv`（含 `.conflicts.tsv`）、`shader_facts.tsv`、`shader_pairs.tsv`。
- **`c1_full.tsv`**：每次出著色器庫後由 `c1_guard.py` 重生成。
- **`pso_manifest.tsv`**：預熱種子清單。
- **`bf_want.tsv`**。
- **`dmsh_chain.h`**：翻譯鏈版本，由 `dmsh_chain.py --header` 寫出。

和遊戲有關的都放這裡。代碼讀檔案，不寫死某個遊戲。

| 誰 | 用什麼選檔案 | 讀什麼 |
|---|---|---|
| 著色器工具（`shader_split.py`、`dmsh_chain.py`、`dmsh_build.py`、`dmsh_facts.py`） | `DM_GAME=<名>` | 各表，`nvn_*`、`shader_*` 鍵 |
| 宿主（`make -C dm/host`、`dm/gfx/tests`） | `GAME=<名>` | `gen/<名>/game_profile.h`（`game.py header` 由 `game.tsv` 生成） |
| `dm/ios/tools/build_ipa.sh` | `GAME=<名>` | App 名、包名、IPA 名、代碼模塊、RomFS 認定路徑、各表 |
| `scripts/build_ipa_from_nsp.py` | NSP 的標題號 | 以上全部。沒有對應檔案時只寫粗查報告 |

都沒有缺省遊戲：不給就停下並列出檔案。查檔案：
- `python3 dm/games/game.py list` 列出全部檔案。
- `python3 dm/games/game.py <名> <鍵>` 打印一個鍵。

## 鍵

| 鍵 | 含義 |
|---|---|
| `title_id`、`program_id` | 身份。`program_id` 是 `svcGetInfo(ProgramId)` 報給遊戲的值。遊戲名不存：NSP 腳本從 NSP 的控制數據（NACP）讀 |
| `name` | 遊戲名，只給 `game.py list`、報錯信息和 Mac 窗口標題當標籤。iOS App 名取基礎版 NSP 控制數據裡的遊戲名，包名是 `org.nxnative.<標題號小寫>`；NSP 腳本的 `APP_NAME` / `BUNDLE_ID`、`build_ipa.sh` 的 `DM_DISPLAY_NAME` / `DM_BUNDLE_ID` 可改 |
| `ipa`、`app_category` | iOS App：缺省 IPA 文件名與 App Store 分類 |
| `build <版本> <顯示版本> <模塊>=<構建號>…` | 每個已知版本一行。NSP 腳本只認這些構建；宿主按 `main` 的構建號報顯示版本 |
| `package` | 缺省打進 IPA 的版本 |
| `shader_versions` | 著色器表是這幾個版本的並集（基礎版在前）。出包時這些版本都要有 |
| `modules` | 改寫並裝載的代碼模塊，按裝載順序。`rtld` 由宿主代替 |
| `romfs_marker` | RomFS 裡必須有的路徑 |
| `zip_store` | IPA 裡不再壓縮的擴展名 |
| `mem_reserve`、`mem_alias`、`mem_stack`、`app_pool` | 客體地址空間分區與應用內存池（`hle_svc.c`） |
| `present_interval` | `game`（缺省）：按遊戲設的出圖間隔出幀，同 Switch（遊戲設 2 就是 30 幀，iOS 上顯示節拍隨之 30 Hz；《朋友收集》設 2，它的可選改寫剪口 `fps60` 讓它設 1，見 `dm/tools/patch_nso.py`）。`display`：不管遊戲設多少，每個顯示節拍一幀——只適合邏輯不按幀計數的遊戲（D2R 的邏輯按自己的 25 Hz 走）。`DM_PRESENT_INTERVAL=game` / `display` 可覆蓋，也可寫在 `Documents/env.txt` |
| `core_preempt` | `on` / `off`（缺省）：近似 Horizon 的按核搶佔。一次 SVC 叫醒了釘在調用線程本核、優先級更高的線程，或這類線程本就在跑，調用線程就等它們再次阻塞（最多 5 ms，`hle_thread.c`）。上游銀河戰士 生存恐懼的影片播放器靠它；馬力歐開了讀盤變慢（首幀 0.6 → 2.9 s），所以只有需要的遊戲才開。`DM_PREEMPT=0` / `1` 可覆蓋 |
| `shared_font` | 共享字體地址處放什麼：`bfttf`（缺省）= 8 字節 BFTTF 頭（「已解」標記）+ TTF，馬力歐拿去調 `ScalableFontEngineHelper::Decode`；`ttf` = TTF 本身，上游的超級馬力歐 3D 世界 + 狂怒世界（SDK 10）直接交給 `ScalableFontEngine::LoadFont`（帶頭時引擎報錯 403，遊戲查字形時崩潰） |
| `pad_layout` | iPhone 虛擬手柄首次的佈局：`simple`（缺省）= 2D（左搖桿、A/B/X/Y、肩鍵、− / +），`full` = 3D（另加十字鍵、右搖桿、L3 / R3）。頂邊正中的佈局鍵在遊戲裡切換，App 記住玩家的選擇（`DmGamepad.m`） |
| `device_api` | `nvnDeviceGetInteger` 報的 NVN API 版本 `主.次`（缺省 `55.15`，馬力歐與《朋友收集》都接受）。遊戲會核對，不符就不建設備：超級馬力歐 3D 世界 + 狂怒世界（SDK 10）要求主版本 53、次版本 ≥ 313。`DM_NVN_API` 可改。不進著色器翻譯鏈版本（只有 `nvn_*` / `shader_*` 鍵進） |
| `nvn_tex_base`、`nvn_tex_slots`、`nvn_samp_base`、`nvn_img_base`、`nvn_img_slots`、`nvn_ssbo_bases` | NVN 驅動常量段，按全庫著色器實測 |
| `shader_groups`、`shader_note` | `shader_split.py` 的來源組與映射表表頭說明 |
| `shader_main_glslc` | `on` / `off`（缺省）：`shader_split.py` 另讀 `main` 裡的 GLSLC 編譯輸出（魔數 `0x19866891`）。上游十三機兵防衛圈的著色器全在那裡；Engage 的 `main` 裡有 11 份用不到的，所以別的遊戲不開 |
| `shader_separable` | `on` / `off`（缺省）：遊戲建單階段程序、繪製時按階段拼，真正的頂點 / 像素配對只在運行時才有。`shader_split.py` 於是給每個頂點著色器配一組「覆蓋」——讀到的輸入合起來等於全部像素著色器讀的輸入的像素著色器，`dmsh_build.py` 的 `vs_pad` 讓每個頂點著色器能接任何像素著色器（上游十三機兵防衛圈）。檔案一設這兩個鍵就進著色器翻譯鏈版本 |

翻譯器（`dmsh_xlate/Program.cs`）裡仍內置貼圖 / 採樣器句柄表基址與深度口徑。檔案的 `nvn_tex_base` / `nvn_samp_base` 與之不同時 `dmsh_build.py` 會停下，所以需要別的值的遊戲要先讓翻譯器可配。

## 加一個遊戲

1. **粗查。** 對這個遊戲的 NSP 文件夾跑 `scripts/build_ipa_from_nsp.py`。沒有檔案時它只解出代碼模塊和 RomFS 清單，寫出 `<NSP 文件夾>/<標題號>_survey/survey.md`。報告包括模塊與構建號、SDK、引擎與圖形接口標記、RomFS 容器、改寫點統計、導入與宿主鈎子的對比。
2. **檔案。** 把 `010051F0207B2000/game.tsv` 拷成 `dm/games/<標題號>/game.tsv`（目錄名用基礎版的標題號），填身份、`build` 行和 `modules`。內存與 NVN 的值先沿用拷來的，等起機時實測再改（[docs/agents](../../docs/agents/README.md)）。
3. **著色器表。** 跑 `DM_GAME=<名> shader_split.py … dm/games/<名>/shader_map.tsv dm/games/<名>/shader_facts.tsv dm/games/<名>/shader_pairs.tsv`。再補空的 `shader_map.tsv.conflicts.tsv`、`bf_want.tsv` 和 `pso_manifest.tsv`（表頭照 `010051F0207B2000` 的抄）。
4. **翻譯鏈。**
   1. `DM_GAME=<名> dmsh_chain.py --header`
   2. `DM_GAME=<名> [IOSOUT=…] dmsh_build.py <包目錄> <緩存目錄>`
   3. `c1_guard.py <緩存目錄>/b-<V> dm/games/<名>/c1_full.tsv`
5. **宿主起機。** 跑 `make -C dm/host GAME=<名>`，然後按 docs/agents 的起機循環走。只適合某一個遊戲的代碼按它的構建號限定（`dm_main_build()`），不留給所有遊戲跑。
6. **IPA。** 用 `GAME=<名> dm/ios/tools/build_ipa.sh`，或者用 NSP 腳本（它現在找得到這個檔案了）。
