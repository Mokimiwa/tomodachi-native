# dm/gfx/tex — 貼圖格式映射、塊線性解排布、BC 解壓

English: [README.md](README.md)

圖形層裡和貼圖數據打交道的部分，供回放層 `../nvn/nvn_mtl.m` 調用。做三件事：

1. 把 NVN 的貼圖格式號換成 Metal 的像素格式；
2. 把 Switch 的「塊線性」排布解成普通的逐行排布；
3. 在不支持 BC 壓縮貼圖的顯卡上把 BC 貼圖解壓。

塊線性（block linear）是 Tegra X1 顯卡存貼圖的方式：數據不是一行接一行，而是按 64 字節 × 8 行的小塊（GOB）交錯排列，再按「塊高」「塊深」把若干 GOB 疊成更大的塊。遊戲打包好的貼圖文件裡就是這種排布，上傳給 Metal 之前必須先解開。

## 文件

| 文件 | 內容 |
|---|---|
| `nvn_fmt_mtl.h`、`nvn_fmt_mtl.c` | `nvn_fmt_to_mtl()`：NVN 格式（0x01–0x9C）→ Metal 像素格式 + 分量重映射。返回 1 表示精確對應，2 表示替代（精度、位序或讀法不同，需要轉換），0 表示 Metal 沒有對應格式。每個格式的處理辦法寫在表內注釋裡。純 C，不依賴 Metal 頭文件 |
| `nvn_deswizzle.h`、`nvn_deswizzle.c` | `nvn_deswizzle()`：把某一級、某一層從塊線性解到線性；`nvn_swizzle()`：反向，單測往返用；`nvn_bl_offset()`：單點的塊線性地址。純 C |
| `nvn_deswizzle_gpu.h`、`nvn_deswizzle_gpu.m` | 同一個解排布算法的 Metal 計算著色器版本，回放層上傳打包貼圖時用；初始化失敗或遇到不支持的格式時回放層退回 CPU 版 |
| `nvn_bcdec.h`、`nvn_bcdec.c` | `nvn_bc_unpacked()` / `nvn_bc_decode()`：BC 貼圖在 CPU 上解壓成不壓縮格式，解碼用 `dm/third_party/bcdec` |
| `tests/` | 單元測試 |

各級、各層在存儲裡的偏移，以及每一級實際使用的塊高、塊深，一律取自 `../nvn/nvn_tex.c`（`nvn_tex_layout`、`nvn_tex_level_offset`、`nvn_tex_level_tiles`），這裡只做級內的尋址與搬運。遊戲的打包貼圖自帶佈局參數，其第 0 字節是第 0 級塊高的 log2，有它時以它為準（`nvn_texdesc` 的 `pkg_block_h` 字段）。

## 不支持 BC 的設備

一部分較老的蘋果顯卡不支持 BC 壓縮貼圖（由 `MTLDevice` 的 `supportsBCTextureCompression` 判斷）。在這些設備上：

- 遊戲貼圖可以用可選的補充包事先離線換成 ASTC 格式（`dm/tools/astc_pack.py`，依賴見 `docs/DEPENDENCIES.zh-TW.md` 的「可選」一節）；
- 剩下的 BC 貼圖由回放層建成解壓後的格式，上傳時解壓，CPU 上的解壓走本目錄的 `nvn_bcdec.c`。

環境變量 `DM_TEST_NO_BC=1` 可以在任何機器上強制走這條路徑。

## 測試

```sh
make -C dm/gfx/tex/tests samples   # 取真數據樣本（見下）
make -C dm/gfx/tex/tests run       # 跑 t_fmt_mtl 與 t_tex
```

- **`t_fmt_mtl`**：對 0x01–0x9C 每個 NVN 格式取映射，凡映射不為 0 的，都在本機的 Metal 設備上真建一張貼圖，必須全部建成；另核名字帶 sRGB 的 NVN 格式都落到 Metal 的 sRGB 格式。不需要遊戲文件。
- **`t_tex`**：解排布的測試，分幾組：

  | 組 | 判據 |
  |---|---|
  | A 往返 | 往各級、各層寫入隨機數據再解出，逐字節相等。覆蓋 2D、2D 數組、3D，多種每塊字節數與非 2 的冪尺寸 |
  | B 地址公式 | `nvn_bl_offset()` 的結果與按公開排布規則手算的地址相等 |
  | E 手工 3D | 在手算地址放標記，解排布後核對落點，並核塊高、塊深與存儲尺寸 |
  | C 真數據 | 同一張貼圖的 Switch 版（塊線性）解排布後與 PC 版（線性）比：同分辨率的逐字節相等；Switch 版降了分辨率的是重新壓縮過的，改比解碼後的 PSNR（峰值信噪比，衡量兩張圖有多像）不低於 22 dB。同時核文件裡的逐級偏移與 `nvn_tex_level_offset` 一致 |
  | D 負對照 | 故意把塊高錯一級再解，結果必須不再逐字節相等、PSNR 低於 22 dB，證明 C 組的判據能分辨錯誤的排布 |

  A 往返只能證明「一一對應、不越界」，證明不了排布公式是對的；真正兜底的是 B 組的手算地址與 C 組同分辨率樣本的逐字節相等。

**關於真數據樣本**：C、D 兩組需要同一張貼圖的 Switch 版與 PC 版各一份。`tests/fetch_samples.py` 從你工作區裡的遊戲文件取樣，Switch 版讀 `$DM_ROOT/data/switch/3.3.94012/Update/RomFS/data`，PC 版讀 `$DM_ROOT/data/assets-src/raw`，輸出到 `$DM_ROOT/build/cache/dm/tex_samples`。沒有樣本時 `t_tex` 的真數據項算失敗，不會跳過；A、B、E 三組不需要遊戲文件。樣本不進倉庫。

## 貼圖文件頭裡與排布有關的字段

`t_tex.c` 解析遊戲的 `.texture` 文件時用到的事實：

- 文件以 `<DE(` 開頭，Switch 版與 PC 版是同一種容器。
- 經過 Switch 轉換的文件，偏移 0x14 起的四個字節是 `00 00 00 07`（PC 版為 0），表示像素區是塊線性排布；偏移 0x13 是第 0 級塊高的 log2，與遊戲運行時傳給 `SetPackagedTextureLayout` 的佈局參數第 0 字節含義相同。
- 沒有這個標記的文件是線性存放的，與 PC 版逐字節相同。

## 局限

- `nvn_deswizzle()` 不支持多重採樣貼圖、帶 LINEAR 標誌的貼圖（本來就是線性的）和 BUFFER 目標，遇到返回 -1。
- 立方體與立方體數組按分層貼圖處理，沒有單獨用真數據核對。
- 3D 貼圖只有往返與手算地址的核對，沒有真數據樣本。
- 16 位打包格式（565 / 5551 / 4444）的分量位序是按格式名對應的，沒有用真數據核過，在格式表裡標為「替代」。
- 真數據樣本集小，覆蓋 RGBA8、BC1、BC3、BC4 四種格式。
