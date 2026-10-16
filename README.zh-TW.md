<div align="center">

# Tomodachi Native

**在 Apple Silicon 的 Mac 上直接玩《朋友收集 夢想生活》。** iPhone 和 iPad 版之後會做。

遊戲原本的 ARM64 程式直接在 Mac 的晶片上執行，Switch 的系統服務和繪圖則用 macOS 與 Metal 4 重新做了一套。

[![License: MIT](https://img.shields.io/badge/license-MIT-f4c430?style=flat-square)](LICENSE)
[![macOS 26+](https://img.shields.io/badge/macOS-26%2B-000000?style=flat-square&logo=apple&logoColor=white)](README.md#mac-app)
[![Apple Silicon](https://img.shields.io/badge/Apple%20Silicon-arm64-555555?style=flat-square)](README.md#how-it-works)
[![Fork of nxnative](https://img.shields.io/badge/fork%20of-nxnative-6e5494?style=flat-square&logo=github)](https://github.com/seedds/smbw-mobile)

[English](README.md) · [繁體中文](README.zh-TW.md)

</div>

> [!IMPORTANT]
> 這裡沒有附任何遊戲檔、金鑰或任天堂的素材。你需要準備從自己的 Switch 匯出的遊戲檔（NSP）和 `prod.keys`。本專案和任天堂沒有任何關係。

這個專案 fork 自 seedds 的 [nxnative](https://github.com/seedds/smbw-mobile)，nxnative 讓 Switch 遊戲直接在 Apple 裝置上執行。這個版本專注在《朋友收集 夢想生活》：加上這款遊戲的設定檔、它需要的 SDK 20 支援，以及專為它做的 Mac app。

## 目前進度

| | |
|---|---|
| **Mac 上可以玩** | 小島導覽、幫島取名、建立 Mii（回答問題或自己捏），有聲音 |
| **Mac app** | 選單、全螢幕、暫停、截圖、60 FPS、MetalFX 放大，滑鼠、觸控板、鍵盤，13 種遊戲語言 |
| **iPhone、iPad** | 還沒打包 |

## 開始玩（Mac）

需要準備：搭載 Apple Silicon、macOS 26 以上的 Mac；Xcode、.NET SDK 10、spirv-cross；已安裝 `lz4`、`capstone`、`fonttools` 的 Python 3；以及遊戲的 NSP 和你自己的 `prod.keys`。

1. 先完成一次前置作業：解包、改寫程式、轉譯著色器。步驟見英文版的 [Build by hand](README.md#build-by-hand-mac-for-development)，其中 `G=010051F0207B2000`、`VER=v0`。
2. 打包成 app。加上 `--embed` 會把資料完整打包進 app，之後就算拔掉存放工作區的磁碟也能玩：

```sh
python3 scripts/build_mac_app.py 010051F0207B2000 -w ~/010051F0207B2000-work -o "/Applications/Tomodachi Life.app" --embed
```

3. 在「應用程式」裡打開 **Tomodachi Life**。

## Mac app

- **像一般的 Mac app：** 有遊戲圖示、全螢幕和三種視窗大小、暫停，截圖會存到你平常的截圖資料夾，關掉前會先問一聲。
- **滑鼠、觸控板、鍵盤：** 滑鼠就像手指，點到哪裡就觸控哪裡。兩指手勢轉鏡頭，捏合和三指滑動對應肩鍵。按鍵都能在 `keymap.txt` 裡改（選單「編輯按鍵…」，⌘,）。
- **畫質：** 視窗或全螢幕比遊戲的 1920 × 1080 大時，MetalFX 放大能讓邊緣和文字更清楚（選單「顯示 → MetalFX 放大」）。「遊戲 → 幀率」可以在原版的 30 FPS 和 60 FPS 之間切換，下次開啟遊戲時生效。
- **輸入名字：** 輸入框用的是遊戲本身的配色，支援你慣用的輸入法，也能直接貼上。
- **13 種語言：** 遊戲語言預設跟著 Mac 的系統語言，也可以在「遊戲 → 遊戲語言」切換。
- **存檔** 放在 `~/Library/Application Support/nxnative/010051F0207B2000/save`。

## 運作原理

- **改寫器**只改 Apple 系統底下跑不動的少數指令（x18、執行緒指標、`svc`、計時器）。
- **宿主程式**負責 Switch 的核心和各種系統服務。
- **NVN 繪圖 API 改用 Metal 4 實作**，遊戲的 14,388 個著色器事先轉譯好。

各部分的細節在英文版的 [How it works in detail](README.md#how-it-works-in-detail)。

## 常見問題

**一定要有 Switch 嗎？** 要有遊戲檔和金鑰，而且要從你自己的主機匯出。

**哪些 Mac 能玩？** 搭載 Apple Silicon、macOS 26 以上（繪圖用到 Metal 4）。目前在 macOS 27 上開發；macOS 26 有支援，但還沒實際試過。

**存檔在哪裡？** `~/Library/Application Support/nxnative/010051F0207B2000/save`，想備份就複製這個資料夾。

**可以接手把嗎？** Mac 版目前還不支援手把，先用鍵盤、滑鼠和觸控板。

**之後會支援其他遊戲嗎？** 程式裡還留著 nxnative 原本做的《超級瑪利歐兄弟 驚奇》和《聖火降魔錄 Engage》設定檔（見英文版的 [Game profiles](README.md#game-profiles)），不過這邊會先把《朋友收集》做好。

## 已知問題

- 用滑鼠點畫面目前只在開頭的選單試過（幫島取名和確認畫面）。之後的場景如果點了沒反應，在 `keymap.txt` 加一行 `mouse:Left A`，就能改回用滑鼠移動小手。
- 開成 60 FPS 時，畫面上東西很多的場景（例如開頭飛過整座島那段）大約只有 55 FPS。

## 授權與致謝

MIT 授權（見 [LICENSE](LICENSE)）。程式延續自 seedds 的 [nxnative](https://github.com/seedds/smbw-mobile)，原生執行的做法最早來自 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)，著色器翻譯用的是 Ryujinx，60 FPS 的改法來自 Fl4sh9174 的模擬器模組。其他用到的專案和第三方授權，見英文版的 [Acknowledgements](README.md#acknowledgements) 和 [dm/third_party](dm/third_party/README.md)。
