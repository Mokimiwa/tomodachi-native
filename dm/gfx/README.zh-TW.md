# dm/gfx — 圖形層（NVN → Metal）

English: [README.md](README.md)

遊戲的 Switch 版用 NVN（Switch 的原生圖形接口）畫圖。圖形層在蘋果的 Metal 上把這套接口重新實現一遍：遊戲的每一次 NVN 調用落到這裡，錄成命令，提交時回放成 Metal 命令。遊戲的著色器不在運行時翻譯，而是在電腦上一次性離線翻譯成 Metal 著色器庫（電腦上沒有蘋果的 Metal 編譯器時，最後的編譯一步改在手機上做，見下文）。

整個項目的構建步驟見倉庫根目錄的 `README.zh-TW.md` 與 `setup.sh`，依賴見 `docs/DEPENDENCIES.zh-TW.md`；這裡只講圖形層自己的事。

## 怎麼工作

### 接管 NVN 接口

- 遊戲主程序只導入一個 NVN 符號 `nvnBootstrapLoader`，其餘 NVN 函數都通過它按名字取地址。宿主強制接管這個導入（`nvn/hle_nvn.c`），原版圖形驅動一條指令都不執行。
- 遊戲會取址的名字共 523 個（`nvn/names.txt`）。每個名字對應一個跳板（`nvn/nvn_thunks.S`，生成文件）：先給該函數的調用計數加一，再跳到實現。
- 其中遊戲主程序裡有調用點的 216 個（`nvn/nvn_slots.tsv`）都有實現：203 個按 NVN 語義實現，13 個是空實現（`hle_nvn.c` 的 `g_nops`）。其餘 307 個接報紅樁，被調到時日誌裡打印 `【紅】NVN 未實現被調用：名字`。
- 每個實現都按 `nvn/nvn_protos.h`（生成文件）做編譯期簽名檢查。蘋果 arm64 與 Switch 的調用約定在小於 32 位的整型參數上不同，所以 `NVNboolean` 參數在宿主側一律按 `uint32_t` 接、只看低 8 位。

### 兩層：錄製與回放

- **接口層 `nvn/hle_nvn.c`**：宿主側的對象就地存在遊戲給 NVN 對象分配的內存裡，佈局見 `nvn/nvn_obj.h`，每個都有靜態斷言保證不超過 NVN 的保留大小。命令按 `nvn_obj.h` 的記錄格式錄進遊戲提供的命令內存。
- **回放層 `nvn/nvn_mtl.m`**：提交時把記錄流回放成 Metal 命令。
  - 內存池建成零拷貝的 `MTLBuffer`，GPU 地址等於 CPU 地址，所以只支持統一內存設備。
  - 「GPU 完成」的可見效果（同步對象就緒、事件寫值、計數器）只在 Metal 命令緩衝真正完成之後發生。
  - 管線狀態、深度模板狀態按狀態哈希緩存；啓動時按預熱清單（隨包的種子清單 `pso_manifest.tsv`，再加本機的 `pso_user.tsv`，見下）在後台預建著色器與管線。建不成的管線記為失敗、不再重試，按著色器鍵報紅一次。
  - 遊戲打包貼圖是 Tegra 塊線性排布，上傳前解排布（見 `tex/README.zh-TW.md`）。
  - 出圖：遊戲的交換鏈貼圖畫到 `CAMetalLayer`，出圖節拍由 `nvn/nvn_pace.h` 的節拍器排。

各 NVN 對象與命令落到 Metal 的哪個對象，見 [nvn_metal_mapping.zh-TW.md](nvn_metal_mapping.zh-TW.md)。

### 著色器翻譯鏈

遊戲的著色器是 Tegra X1 的 GPU 機器碼（Maxwell 指令集）。從這種機器碼到 Metal 著色語言的翻譯不在手機上做，全部離線完成：

```
Switch 原版著色器包
  → tools/dmsh_xlate        用 Ryujinx 的著色器翻譯庫出 SPIR-V（C# 工具）
  → spirv-cross             SPIR-V 轉 Metal 著色語言（MSL）
  → xcrun metal / metallib  編成 Metal 著色器庫
```

沒有蘋果的 Metal 編譯器時（Windows、Linux、沒裝 Xcode 的 Mac），最後一步做不了。`tools/nometal_run.py` 讓同一條鏈照常跑完前兩步，把翻譯出的 MSL 源碼原樣放進庫文件；這樣做出的包在手機上把著色器編譯一次，系統會緩存編好的結果。這條路由 `dm/tools/d2rm_pack.py` 使用，見倉庫根目錄的 `docs/NO_XCODE.zh-TW.md`。

整條鏈由 `tools/dmsh_build.py` 驅動，`setup.sh` 會替你跑。要點：

- **鍵**：每個著色器以「控制段（前 0x880 字節；控制段 +0x20 自報的長度更短時取其長度，如 SDK 10 的 0x850 字節控制段）+ 代碼區」的 sha256 前 16 位十六進制為鍵（馬力歐有很多著色器控制段相同、代碼不同）。運行時遊戲把兩者交給 `nvnProgramSetShaders`，回放層算出同一個鍵去找庫。
- **產物**：`<鍵>_b.metallib`（著色器庫）、`<鍵>_b.json`（回放用的反射：各綁定的種類、槽號、在參數緩衝裡的序號等）、`<鍵>_b.metal`（MSL 源）。部分片元著色器另有 `<鍵>_bf.metallib`，是「可讀 tile」版本，供通道合併用（見 `tools/dmsh_fetch.py` 文件頭）。目錄裡的 `_b_build.json` 記本次每個鍵的成敗。
- **`dm/games/<名>/shader_map.tsv`**：著色器映射表，每個著色器變體一行。運行時只用其中「鍵 → 階段（頂點 / 片元 / 計算）」。
- **`dm/games/<名>/shader_facts.tsv`**：翻譯查詢的作答表。翻譯器需要知道一些機器碼裡讀不出的事實（貼圖的維度類型、圖像格式、頂點屬性與片元輸出的分量類型、計算著色器的線程組大小與共享內存字節數），這張表按鍵給出答案。表隨倉庫，出著色器庫只讀這張表；表由 `tools/dmsh_facts.py` 維護，文件頭有說明。
- **翻譯鏈版本與 `b-<V>/` 目錄**：版本 V 是一串 12 位十六進制，由翻譯鏈涉及的源文件內容、翻譯選項和工具指紋（`tools/dmsh_chain.lock`）一起哈希得出，`tools/dmsh_chain.py` 負責計算。著色器庫寫進 `<輸出目錄>/b-<V>/`，每個反射 json 裡也記著 V。宿主編譯時從 `dm/games/<名>/dmsh_chain.h` 取 V，只從 `b-<V>/` 加載，並逐個核對 json 裡的版本；對不上就報紅、不加載。所以改了鏈上任何一個文件、或換了工具，都要重新出庫並重編宿主。
  - `dmsh_chain.py`：打印 V。
  - `dmsh_chain.py --check-tools`：核對本機工具與鎖文件一致。
  - `dmsh_chain.py --update-lock`：按本機工具重寫鎖文件。
  - `dmsh_chain.py --header`：重寫 `dm/games/<名>/dmsh_chain.h`。
  - 本機的 Xcode 或 spirv-cross 版本與倉庫登記的不同是正常的，`setup.sh` 會自動按本機工具重新登記。
- **`dm/games/<名>/c1_full.tsv`**：著色器自帶常數段（c1）的下標清單，由 `tools/c1_guard.py` 掃描翻譯出的 MSL 生成，列出「運行時下標沒能證明有界」的著色器鍵。首行記著它對應的 `b-<V>`。回放層對清單裡的著色器給 c1 單獨分配整塊 64 KiB 緩衝；清單缺失或版本對不上時報紅，並對全部著色器都這樣保守處理。
- **`dm/games/<名>/shader_pairs.tsv`**：每個 BNSH 變體的（頂點鍵, 像素鍵），由 `tools/shader_split.py` 寫出。Metal 要求像素著色器的每個輸入都是頂點著色器的輸出才肯建管線，而 Maxwell 對沒寫的屬性給缺省值；所以 `dmsh_build.py` 給頂點著色器補上配對像素著色器讀、它卻沒寫的輸出，值為 (0, 0, 0, 1)（`vs_pad`）。這張表算進翻譯鏈版本。
- **多個遊戲版本**：`shader_split.py` 的 RomFS 與 `main` 參數可寫成逗號分隔的列表（基礎版在前），輸出並集；映射表、作答表、配對表目前覆蓋馬力歐 v0 與 1.2.1。
- **`dm/games/<名>/pso_manifest.tsv`**：隨包的預熱種子清單：啓動時預建的著色器（`S`）、渲染管線（`R`，即 `nvn/nvn_pso_skey.h` 裡 `pso_skey` 的十六進制）與計算管線（`C`）。行序就是預建順序（首次使用的順序），不要排序。與它配套的是 `pso_user.tsv`：宿主每次不得不當場建著色器或管線時追加記到存檔目錄旁，下次啓動在種子清單之後預建。兩者都只以鍵指認著色器、只描述管線設置，所以一台設備錄的清單在別的設備上同樣有效；本構建用不上的行跳過。錄到的行可以追加到種子清單表頭之後（目前為空）。
- **程序本體內嵌的著色器**：主程序裡另有 4 個計算著色器不在著色器包裡，按反匯編手寫成 MSL，在 `nvn/nvn_builtin.h`。

翻譯時遵循的綁定約定（`tools/maxwell_refs.py` 文件頭）：

- uniform 緩衝 r 在機器碼裡是常量段 `c[3 + r]`；
- 分離貼圖 r 的句柄在 `c[2][0x168 + 8r]`，分離採樣器 r 的句柄在 `c[2][0x568 + 8r]`；
- 存儲緩衝 r 的地址在 `c[0][基址 + 16r]`，基址：頂點 0x110、計算 0x310、片元 0x510；
- 窗口原點在左上、深度範圍 0 到 1，與 Metal 一致，翻譯時不翻轉。

## 目錄結構

| 位置 | 內容 |
|---|---|
| `nvn/hle_nvn.c` | 接口層：接管 NVN、對象實現、命令錄製、出圖節拍 |
| `nvn/nvn_mtl.m`、`nvn/nvn_mtl.h` | 回放層：Metal 設備、內存池、貼圖、著色器加載、管線、繪製、出圖 |
| `nvn/nvn_obj.h` | 宿主側對象佈局與命令記錄格式，兩層共用 |
| `nvn/nvn_entry.S`、`nvn/nvn_thunks.S` | 未實現樁與計時包裝的入口、逐函數跳板 |
| `nvn/nvn_tex.c`、`nvn/nvn_tex.h` | 塊線性貼圖的存儲尺寸、對齊、各級偏移（移植自 deko3d） |
| `nvn/nvn_dirty.c`、`nvn/nvn_dirty.h` | 用寫保護跟蹤遊戲內存的髒頁，判斷貼圖內容是否變了需要重傳 |
| `nvn/nvn_refl.m`、`nvn/nvn_refl.h` | 著色器反射 json 轉成定長 C 數組，繪製時只遍歷數組 |
| `nvn/nvn_hmap.h`、`nvn/nvn_cover.h`、`nvn/nvn_pace.h`、`nvn/nvn_pso_skey.h` | 哈希表、全覆蓋繪製的幾何證明、出圖節拍器、管線清單鍵 |
| `nvn/nvn_builtin.h` | 主程序內嵌的 4 個計算著色器的手寫 MSL |
| `nvn/include/nvn.h` | NVN 接口聲明（第三方，許可證見文件頭） |
| `nvn/names.txt`、`nvn/nvn_slots.tsv` | NVN 接口名表與 D2R 的槽位證據。上文講的各張遊戲表與 `dmsh_chain.h` 在遊戲檔案 `dm/games/<名>/`（以基礎版標題號命名；工具用 `DM_GAME` 選，make 用 `GAME`，沒有缺省遊戲） |
| `tex/` | 貼圖格式映射、解排布、BC 解壓，見 [tex/README.zh-TW.md](tex/README.zh-TW.md) |
| `metal/` | 獨立的最小 Metal 離屏示例，不編進宿主，見 [metal/README.zh-TW.md](metal/README.zh-TW.md) |
| `tools/` | 離線工具，見下 |
| `tests/` | 單元測試 |
| `hle_vi.c`、`hle_nv.c` | 顯示服務與圖形驅動初始化的膠水層：返回宿主靜態對象或空操作 |

`tools/` 裡的腳本，用法都寫在各自文件頭：

| 腳本 | 用途 |
|---|---|
| `dmsh_xlate/` | 著色器翻譯工具（C#），引用 `dm/third_party/ryujinx` 裡的翻譯庫 |
| `dmsh_build.py` | 驅動整條翻譯鏈出著色器庫；設環境變量 `IOSOUT=目錄` 時同時出 iOS 版 |
| `dmsh_chain.py`、`dmsh_chain.lock` | 翻譯鏈版本與工具指紋 |
| `dmsh_facts.py` | 作答表 `shader_facts.tsv` 的生成與核對 |
| `dmsh_fetch.py` | 生成片元著色器的「可讀 tile」版本 |
| `c1_guard.py` | 生成 `c1_full.tsv` |
| `dmsh_dryrun.py` | 全庫乾轉，只量「能不能轉」 |
| `shader_map.py`、`maxwell_refs.py` | 生成著色器映射表；從機器碼讀資源引用 |
| `nvn_slots.py`、`gen_protos.py` | 生成 `nvn_slots.tsv` / `names.txt`、`nvn_protos.h` / 跳板 |
| `tbdr_budget.py`、`tbdr_load_dead.py`、`tbdr_store_dead.py`、`tbdr_pass_merge.py` | 讀 `DM_NVN_PASSDUMP` 的日誌，分析每幀各渲染通道附件的讀入 / 寫回量 |
| `passgpu_top.py` | 讀 `DM_NVN_PASSGPU` 的輸出，按通道列 GPU 時間 |
| `nometal_run.py` | 在沒有蘋果 Metal 編譯器的機器上跑這條翻譯鏈：著色器以源碼進庫，到手機上再編 |

## 構建

圖形層沒有單獨的庫。`dm/host/Makefile` 按通配把 `dm/gfx/*.c`、`dm/gfx/nvn/` 與 `dm/gfx/tex/` 下的源文件編進宿主 `dmhost`；iOS App 由 `dm/ios/tools/build_ipa.sh` 構建，同時把映射表、`pso_manifest.tsv`、`c1_full.tsv` 和 `b-<V>/` 著色器庫拷進 App。`tests/` 與 `metal/` 不進宿主。

著色器庫由 `setup.sh` 生成：

- Mac 版在 `$DM_ROOT/build/cache/metal_shader_cache/b-<V>/`；
- iOS 版在 `$DM_ROOT/build/ios/dmsh_b/b-<V>/`。

改了翻譯鏈上的文件後，手動重出的順序與 `setup.sh` 裡相同：`dmsh_chain.py --header` → `dmsh_build.py` → `c1_guard.py` → 重編宿主。`$DM_ROOT` 缺省是 `~/dm-work`。

## 測試

```sh
make -C dm/gfx/tests run        # 圖形層單測
make -C dm/gfx/tex/tests run    # 貼圖子模塊單測；真數據項要 Switch 與 PC 兩版的貼圖樣本，見 tex/README.zh-TW.md
make -C dm/gfx/metal run        # 離屏示例自測，見 metal/README.zh-TW.md
```

`dm/gfx/tests` 的測試直接編入生產代碼，需要本機有 Metal 設備；其中幾項在 `MTL_DEBUG_LAYER=1`（Metal 校驗層）下運行。多數測試不讀工作區；例外是 `t_refl` 與 `t_rt_range_ro` 要讀 `$DM_ROOT/build/cache/metal_shader_cache` 下翻譯好的著色器反射 json，`test_tbdr_budget.py` 會在 `$DM_ROOT/run/gfx` 下建臨時目錄。主要內容：

| 測試 | 核什麼 |
|---|---|
| `t_nvn` | 接口層：照遊戲的用法從 `nvnBootstrapLoader` 取址，一路走到出圖 |
| `t_builtin` | 4 個內嵌計算著色器 |
| `t_refl`、`t_hmap`、`t_cover`、`t_pace`、`t_pso_skey`、`t_dirty_slots` | 反射解析、哈希表、全覆蓋證明、節拍器、管線清單鍵、髒頁跟蹤 |
| `t_deswz_gpu` | GPU 解排布 |
| `t_present_crop` | 出圖裁切 |
| `t_mip_extent` | 渲染到非第 0 級 mip 時的有效尺寸 |
| `t_fuse_check`、`t_reorder_chain`、`t_defer_chain`、`t_zskip`、`t_xv`、`t_ub`、`t_pfv`、`t_rt_range`、`t_rt_range_shape`、`t_rt_range_ro`、`t_rt_depth_ro` | 直接編入回放層 `nvn_mtl.m`，核回放層內部的各項優化與診斷邏輯 |
| `test_tbdr_budget.py` | `tools/tbdr_budget.py` 的解析 |

## 常用運行時開關

都是環境變量。iOS App 啓動時自己設好 `DM_NVN_SHMAP` 與 `DM_MSL_DIR`。

資源位置：

| 開關 | 作用 |
|---|---|
| `DM_NVN_SHMAP=路徑` | 著色器映射表。缺省是可執行文件目錄下的 `../games/<名>/shader_map.tsv`；`c1_full.tsv` 與 `pso_manifest.tsv` 取同目錄 |
| `DM_MSL_DIR=目錄` | 著色器庫的上級目錄，宿主從其下的 `b-<V>/` 加載。在 Mac 上跑宿主時設為 `$DM_ROOT/build/cache/metal_shader_cache` |
| `DM_PSO_MANIFEST=路徑` 或 `off` | 換一份預熱清單，或關掉預熱 |
| `DM_PSO_RECORD=路徑` | 把本次運行用到的著色器與管線追加記到文件，用來重錄預熱清單 |
| `DM_PSO_USER=路徑` | 本機「當場建的著色器與管線」清單放哪裡（缺省：`DM_SAVE` 父目錄下的 `pso_user.tsv`） |

日誌與抓取：

| 開關 | 作用 |
|---|---|
| `DM_NVN_LOG=1` | 記錄 NVN 對象的創建 |
| `DM_NVN_FRAMES=N` | 前 N 幀逐幀打印調用表（缺省 3） |
| `DM_NVN_TSV=路徑` | 每幀調用表與計時寫成 TSV；再加 `DM_NVN_TSV_CALLS=0` 只寫計時行 |
| `DM_SLOWFRAME=1` | 只把掉幀那幾幀的計時分項寫進日誌；歸因腳本是 `dm/ios/tools/slowframe.py` |
| `DM_NVN_SHOT=幀號[,幀號…]:路徑` | 在指定幀把交換鏈貼圖存成圖片，路徑裡的 `%d` 換成幀號 |
| `DM_NVN_SHOT_OUT=寬x高` | 上面的截圖改截放大到該分辨率後的畫面 |
| `DM_NVN_TEXDUMP=幀號:路徑` | 把該幀用過的顏色渲染目標逐張存圖 |
| `DM_NVN_PASSDUMP=幀號` 或 `起-止` | 把這些幀每個渲染通道的附件、採樣、出圖逐條寫到標準錯誤，供 `tools/tbdr_*.py` 分析；再設 `DM_NVN_PASSDUMP_ALL` 則每次繪製都記 |
| `DM_NVN_PASSGPU=文件` | 逐通道 GPU 時間（需要設備支持階段邊界的計數採樣） |
| `DM_NVN_PROF=1` | 給已實現的 NVN 函數套計時包裝，統計錄製期各函數耗時 |

對照與測試：

| 開關 | 作用 |
|---|---|
| `DM_NVN_TBDR=0` | 關掉回放層全部通道優化，走「附件全掛、全讀入、全寫回」的最樸素路徑，用來做等價對照 |
| `DM_NVN_UNWRITTEN_DC=0` | 關掉「本通道沒寫的附件不寫回」這一項 |
| `DM_NVN_FUSE_CHECK=1` | 通道合併的同幀核對，逐次把比對結果記進日誌 |
| `DM_TEST_NO_BC=1` | 在任何機器上強制走「顯卡不支持 BC 壓縮貼圖」的解壓路徑 |
| `DM_FPS=30` 或 `60` | 釘住目標幀數 |
| `DM_PRESENT_HZ=N` | 出圖節拍的頻率（缺省 60）；0 表示不限幀，只用於性能剖析 |

## 已知限制

下面這些路徑沒有實現，走到會在日誌裡報紅（`nvn/nvn_mtl.m` 裡的 `RED_ONCE`）：

- 渲染通道中途的清除、帶部分掩碼或部分剪裁的清色；
- 帶索引的四邊形圖元（8 位索引在回放時擴成 16 位）；
- 部分形態的貼圖到貼圖拷貝與清貼圖。

`MultiDraw*IndirectCount`、`DispatchComputeIndirect`、`SetDepthBounds` 等列在 `g_nops` 裡的函數是空實現，調用不報紅也不生效。`Downsample` / `DownsampleTextureView` 按貼圖拷貝錄（帶宿主標誌 `DM_COPY_RESOLVE`），回放用 Metal 的多重採樣解析。
