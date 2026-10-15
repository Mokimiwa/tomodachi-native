# dm/third_party：第三方代碼

English: [README.md](README.md)

這裡是別人寫的代碼，隨倉庫一起提供，不用另外下載。它們各自沿用原來的許可證，許可證文件就在各自目錄裡。

| 目錄 | 是什麼 | 版本與改動 | 用在哪 | 許可證文件 |
|---|---|---|---|---|
| `bcdec/` | BC 壓縮貼圖的解壓，單個頭文件 `bcdec.h` | 0.985（上游提交 `80859ed`），未改動 | 圖形層：顯卡不支持 BC 格式時在 CPU 上解壓貼圖（`dm/gfx/tex/nvn_bcdec.c`） | `bcdec/LICENSE`（MIT 或 Unlicense，任選其一） |
| `flip/` | NVIDIA FLIP：模擬人眼觀看的圖像差異度量。`FLIP.h` 加命令行工具的源碼 `tool/` | 目錄裡沒有記錄 | 驗證：`dm/verify/gfxq/` 的畫質度量，先 `make -C dm/third_party/flip` 編出 `flip` | `flip/LICENSE`（BSD 3-Clause） |
| `libdeflate/` | 解壓庫 | 上游提交 `92e6a0d`，未改動 | 宿主：遊戲資源塊的 zlib 解碼（`dm/host/hle_tact.c`）。只編解壓相關的五個源文件，名單在 `dm/host/Makefile` | `libdeflate/COPYING`（MIT） |
| `libvpx/` | libvpx 的 VP9 解碼器：只編 VP9 解碼器時編到的 71 個源文件、它們的頭文件，以及 `config/` 裡生成的配置 | v1.17.0（上游提交 `6df3ec3`），未改動 | 宿主：解遊戲的 VP9 影片（`dm/host/hle_movie.c`）。`dm/host/Makefile` 把它編成 `gen/vpx/libvpx.a`，iOS 構建編同一批文件 | `libvpx/LICENSE`（BSD 3-Clause），專利授權見 `libvpx/PATENTS` |
| `ryujinx/` | Ryujinx 的著色器翻譯庫，三個項目：`Ryujinx.Graphics.Shader`、`Ryujinx.Common`、`Spv.Generator` | Ryujinx 1.3.2，未改動 | 著色器翻譯工具 `dm/gfx/tools/dmsh_xlate` 引用它，把 Switch 原版著色器翻譯成 SPIR-V | `ryujinx/LICENSE.txt`（MIT）；`ryujinx/src/Spv.Generator/LICENSE` 是該子項目自帶的許可證文本 |

幾點補充：

- `flip/tool/` 裡還帶著 FLIP 命令行工具用到的三個單頭文件庫：`stb_image.h`（2.23）、`stb_image_write.h`（1.13）、`tinyexr.h`。它們的許可聲明寫在各自文件裡。
- `bcdec/README.md`、`libdeflate/VERSION.md`、`libvpx/README.md`、`ryujinx/README.md` 記著各自的來源與用途。
- 來源地址與許可證的匯總在 [docs/DEPENDENCIES.zh-TW.md](../../docs/DEPENDENCIES.zh-TW.md) 的「隨倉庫的第三方代碼」一節。那一節還列了兩處不在本目錄的第三方代碼：`dm/gfx/nvn/include/nvn.h`（MIT，作者見文件頭）和 `dm/gfx/nvn/nvn_tex.c` 裡移植自 deko3d 的排布尺寸算法（zlib 許可證）。
