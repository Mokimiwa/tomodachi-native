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
| **iPhone、iPad** | 在 iPad Air 11 吋（M2）上能開、照螢幕形狀滿版、有聲音，30 或 60 FPS；按 HOME 有設定（遊戲語言、畫面、按鍵、存檔備份），長按 App 圖示可以直接開；還沒玩完整個流程 |

## 開始玩（Mac）

需要準備：搭載 Apple Silicon、macOS 26 以上的 Mac；Xcode、.NET SDK 10、spirv-cross；已安裝 `lz4`、`capstone`、`fonttools` 的 Python 3；以及遊戲的 NSP 和你自己的 `prod.keys`。

1. 先完成一次前置作業：解包、改寫程式、轉譯著色器。步驟見英文版的 [Build by hand](README.md#build-by-hand-mac-for-development)，其中 `G=010051F0207B2000`、`VER=v0`。
2. 打包成 app。加上 `--embed` 會把遊戲資料複製到這台 Mac，之後就算拔掉存放工作區的磁碟也能玩。再次打包時多加 `--keep-data`，就不用重新複製 RomFS；舊版打包把資料放在 app 裡、要一分多鐘才打得開，加上它也會把 RomFS 搬出來：

```sh
python3 scripts/build_mac_app.py 010051F0207B2000 -w ~/010051F0207B2000-work -o "/Applications/Tomodachi Life.app" --embed
```

3. 在「應用程式」裡打開 **Tomodachi Life**。

## 裝到自己的 iPhone 或 iPad

用你自己的 Apple 開發者團隊簽名，可以從 Mac 直接裝到自己的裝置，不用側載工具。只給自己用：App 裡是從你的遊戲改寫出來的程式。

需要準備：英文版 [Pack an IPA](README.md#pack-an-ipa) 列的東西（含你自己的 NSP 和 `prod.keys`）、XcodeGen、在 Xcode 登入你的開發者帳號（Xcode → 設定 → 帳號），以及團隊 ID（同一頁）和裝置的 UDID（`xcrun devicectl list devices`）。

1. 改寫程式、做 iOS 著色器，並保留解出的遊戲檔（同時會出一個沒簽名的 IPA，可以不管）：

   ```
   python3 scripts/build_ipa_from_nsp.py ~/Games/Tomodachi -k ~/keys/prod.keys --keep-extracted
   ```

2. 出簽好名的 App。第一次要用 USB 接上裝置並解鎖，Xcode 才能把它加進你的團隊：

   ```
   B=~/Games/Tomodachi/010051F0207B2000_build
   SIGN_TEAM=<團隊 ID> SIGN_DEVICE=<裝置 UDID> GAME=010051F0207B2000 W=$B/ipa PATCHED=$B/patched_v0 \
     ROMFS=$B/extract/v0/RomFS MSL_IOS=$B/shader/ios dm/ios/tools/build_ipa.sh
   ```

   腳本最後會印出 App 的位置和下面兩行指令。App 約 500 MB，大部分是著色器。

3. 安裝：`xcrun devicectl device install app --device <裝置 UDID> <DmIOS.app 的路徑>`

4. 把遊戲資料拷進 App，只要做一次（約 6 GB，之後重裝會留著）：

   ```
   xcrun devicectl device copy to --device <裝置 UDID> --domain-type appDataContainer \
     --domain-identifier org.nxnative.010051f0207b2000 --source $B/extract/v0/RomFS --destination Documents/romfs
   ```

5. 在裝置上打開 App。

- **更新：** 用同樣的方式裝新版，只換 App，存檔和遊戲資料都會留著。從裝置上刪掉 App 會連這兩樣一起刪掉。
- **之後不用接線：** 用 USB 配對過一次，在同一個 Wi-Fi 下 devicectl 就能直接裝。
- **執行：** 裝好之後，App 不需要 Mac 也能開。
- **能開多久：** 看描述檔的期限。付費的開發者團隊是一年；用免費 Apple ID 簽的，7 天後就打不開，要重裝。

## Mac app

- **像一般的 Mac app：** 有遊戲圖示、全螢幕（綠色按鈕、雙擊標題列或 ⌃⌘F）和三種視窗大小、暫停，截圖會存到你平常的截圖資料夾，關掉前會先問一聲。
- **滑鼠、觸控板、鍵盤：** 滑鼠就像手指，點到哪裡就觸控哪裡。兩指手勢轉鏡頭，捏合和三指滑動對應肩鍵。按鍵都能在 `keymap.txt` 裡改（選單「編輯按鍵…」，⌘,）。
- **畫質：** 視窗或全螢幕比遊戲的 1920 × 1080 大時，MetalFX 放大能讓邊緣和文字更清楚（選單「顯示 → MetalFX 放大」）。「遊戲 → 幀率」可以在原版的 30 FPS 和 60 FPS 之間切換，下次開啟遊戲時生效。
- **螢幕形狀：** 可以讓遊戲照螢幕的形狀渲染，不再補黑邊：在終端機執行 `defaults write org.nxnative.010051f0207b2000.mac GameAspect screen`（也可以寫 `21:9`、`16:10` 這類比例），下次開啟遊戲時生效。比 16:9 寬的最寬約 21.5:9；比 16:9 高的（16:10、4:3、iPhone Duo）最高到 1920 × 1350，左右視野和 16:9 一樣、上下多看到一些。選單和文字維持 16:9 的形狀放在中間，整頁底圖、變暗效果和換場畫面會鋪滿螢幕。畫面比 16:9 寬時，Mii 頭上的泡泡和提示在畫面上任何位置都跟著 Mii。
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

**之後會支援其他遊戲嗎？** 這個版本只做《朋友收集》。想玩《超級瑪利歐兄弟 驚奇》或《聖火降魔錄 Engage》，請用原本的 [nxnative](https://github.com/seedds/smbw-mobile)。想自己移植別的遊戲，見英文版的 [Port another game](README.md#port-another-game)。

## 已知問題

- 用滑鼠點畫面目前只在開頭的選單試過（幫島取名和確認畫面）。之後的場景如果點了沒反應，在 `keymap.txt` 加一行 `mouse:Left A`，就能改回用滑鼠移動小手。
- 開成 60 FPS 時，畫面上東西很多的場景（例如開頭飛過整座島那段）大約只有 55 FPS。

## 授權與致謝

MIT 授權（見 [LICENSE](LICENSE)）。程式延續自 seedds 的 [nxnative](https://github.com/seedds/smbw-mobile)，原生執行的做法最早來自 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)，著色器翻譯用的是 Ryujinx，60 FPS 的改法和寬螢幕的線索來自 Fl4sh9174 的模擬器模組。其他用到的專案和第三方授權，見英文版的 [Acknowledgements](README.md#acknowledgements) 和 [dm/third_party](dm/third_party/README.md)。
