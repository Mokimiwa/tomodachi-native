# NVN → Metal 對象映射

English: [nvn_metal_mapping.md](nvn_metal_mapping.md)

這份文檔列出 NVN（Switch 的原生圖形接口）的各類對象與命令，在 D2R Mobile 的圖形層裡分別落到 Metal 的什麼上。圖形層的整體結構見 [README.zh-TW.md](README.zh-TW.md)。

範圍：遊戲主程序裡有調用點的 216 個 NVN 函數（清單在 `nvn/nvn_slots.tsv`）。其餘 307 個接報紅樁，被調到時日誌打印 `【紅】NVN 未實現被調用：名字`。

兩條貫穿全表的約定：

- **GPU 地址 = CPU 地址**。NVN 的內存池用的是遊戲自己分配的內存，圖形層在同一段內存上建零拷貝的 Metal 緩衝。遊戲傳來的任何緩衝地址，都用內存池的區間表反查成「哪個 `MTLBuffer` + 偏移」（`mtl_addr()`）。
- **先錄後放**。命令緩衝的調用只是把記錄寫進遊戲提供的命令內存（格式見 `nvn/nvn_obj.h`），到提交時才由 `nvn/nvn_mtl.m` 回放成 Metal 命令。

## 1. 對象

| NVN 對象 | Metal 實現 | 要點 |
|---|---|---|
| Device | `MTLDevice`（要求統一內存） | `nvnDeviceGetInteger` 返回的設備屬性仍按 Tegra X1 的值給遊戲（`hle_nvn.c` 的 `g_devinfo`） |
| MemoryPool | 遊戲內存上的 `newBufferWithBytesNoCopy` | 蘋果芯片的頁是 16 KiB，池不保證按它對齊：起點向下、終點向上取整到頁邊界再建緩衝，相鄰池的緩衝允許重疊（`mtl_pool_add()`；做法由 `metal/probe/p_nocopy.m` 驗證） |
| 緩衝地址（`NVNbufferAddress`） | （`MTLBuffer`, 偏移） | 按池的區間表反查 |
| Texture | `MTLTexture` | 遊戲打包貼圖是塊線性排布，上傳時解排布；格式映射與解排布在 `tex/`。池內存被遊戲改寫後重新上傳，靠 `nvn/nvn_dirty.c` 的寫保護髒頁跟蹤發現。`ReadTexels`：打包貼圖直接從池內存解排布讀，GPU 寫出的貼圖從 `MTLTexture` 讀回 |
| TextureView | `newTextureViewWithPixelFormat:textureType:levels:slices:` | — |
| Sampler | `MTLSamplerState` | 邊框色 Metal 只有三種預設（透明黑、不透明黑、不透明白），按 NVN 給的邊框色就近取 |
| TexturePool / SamplerPool | 宿主側的表：描述符號 → 貼圖 / 採樣器 | 分離貼圖句柄的值就是描述符號 |
| Program | 每個階段一個 `MTLFunction` | 以著色器「控制段 + 代碼區」的哈希為鍵，從離線翻譯好的 `<鍵>_b.metallib` 加載；運行時不翻譯。翻譯鏈見 README |
| 狀態對象（Blend / Color / ChannelMask / Multisample / Polygon / DepthStencil / VertexAttrib / VertexStream） | 渲染管線描述符的字段 + `MTLDepthStencilState` + 編碼器狀態 | 遊戲按值拷貝這些小結構，宿主側用位域就地打包。渲染管線、深度模板狀態按狀態哈希緩存 |
| CommandBuffer | 錄在遊戲命令內存裡的記錄流 | 提交時回放。宿主放在遊戲 `NVNcommandBuffer` 裡的記錄讓開 +0x10 那個字節：遊戲內聯的 NVN 代碼直接寫它。錄製超出遊戲給的內存時宿主借出段，只在遊戲重新給出這段錄製起始所在的內存時才收回；`GetCommandMemoryUsed` 兩邊的字節都算 |
| Queue | Metal 命令隊列 | `SubmitCommands` = 回放並提交 |
| Sync | 提交的柵欄值 | `FenceSync` 記下最近一次提交的柵欄值；`SyncWait` 只在 GPU 真正完成到該值之後放行（`MTLSharedEvent` 的已完成值）。沒圍欄過的同步對象等待直接返回超時 |
| Event | 遊戲內存裡的一個 32 位值 | 寫值在 GPU 完成之後發生 |
| Window | 遊戲自己的交換鏈貼圖 ↔ `CAMetalLayer` | `AcquireTexture` 返回交換鏈貼圖的下標，垂直同步的等待發生在這裡；`QueuePresentTexture` 只把出圖排進隊列立即返回，由出圖線程把該貼圖按 `WindowSetCrop` 給的裁切區畫到 drawable 上。畫的是貼圖裡存的字節，和 Switch 掃描輸出一致：sRGB 交換鏈貼圖經非 sRGB 視圖採樣（`present_raw_src`） |

## 2. 命令

| NVN 命令 | Metal | 說明 |
|---|---|---|
| SetRenderTargets | 結束當前渲染編碼器，開新的渲染通道 | 通道裡第一條命令之前的清除並進附件的讀入動作（load = Clear）。換格式的視圖（`RGBA8` 貼圖的 `RGBA8_SRGB` 視圖）掛成該格式的 Metal 視圖，寫入時與 Switch 一樣編碼 |
| ClearColor / ClearDepthStencil | 同上 | 通道中途的清除、帶部分掩碼或部分剪裁的清色沒有實現，遇到報紅 |
| ClearTexture / ClearTextureui | 單獨清一張貼圖 | 部分形態沒有實現，遇到報紅 |
| BindProgram 與各狀態 Bind* | 記進當前狀態，下一次繪製前按需取管線 | — |
| BindUniformBuffer(s) / BindStorageBuffer(s) / BindSeparateTextures / BindSeparateSamplers / BindImages | 按著色器的反射 json 填參數緩衝（argument buffer） | 槽號與著色器裡常量段的對應見 README「著色器翻譯鏈」 |
| BindVertexBuffers | 頂點緩衝 = （`MTLBuffer`, 偏移） | — |
| SetViewports / SetScissor / SetDepthRange / Stencil* / SetBlendColor / SetPolygonOffsetClamp | 編碼器上對應的設置 | — |
| DrawArrays / DrawElementsInstanced | 繪製 / 帶索引繪製 | Metal 沒有四邊形圖元：不帶索引的四邊形用共享的索引 0,1,2,0,2,3 畫成兩個三角形；帶索引的四邊形、8 位索引沒有實現，遇到報紅 |
| DispatchCompute | 計算編碼器，線程組大小取反射 json | 主程序內嵌的 4 個計算著色器用 `nvn/nvn_builtin.h` 裡手寫的版本 |
| CopyBufferToBuffer / UpdateUniformBuffer | 命令流裡按序插入的緩衝拷貝 | `UpdateUniformBuffer` 的數據在錄製時就拷進命令內存，因為 NVN 允許調用方錄完後改原數據 |
| CopyBufferToTexture / CopyTextureToBuffer / CopyTextureToTexture | 貼圖拷貝 | 貼圖到貼圖的部分形態沒有實現，遇到報紅 |
| ReportCounter / ResetCounter | 計數器的值在 GPU 完成之後寫回 | 「通過採樣數」這一類用 Metal 的可見性結果緩衝實現 |
| SignalEvent | 事件寫值，GPU 完成之後發生 | — |
| SetTexturePool / SetSamplerPool | 錄進命令流，記下當前用的描述符池 | — |

下面這些是空實現（`hle_nvn.c` 的 `g_nops`），調用不報紅也不產生效果：

`SetPatchSize`、`MultiDrawArraysIndirectCount`、`MultiDrawElementsIndirectCount`、`DispatchComputeIndirect`、`SetDepthBounds`、`DownsampleTextureView`、`WaitSync`、`SetShaderScratchMemory`、`PushDebugGroupDynamic`、`PopDebugGroup`、`InsertDebugMarkerDynamic`、`WaitEvent`，以及 `nvnWindowFinalize`。

## 3. 著色器綁定

Switch 的著色器通過驅動維護的幾個常量段拿到資源，圖形層在回放時把同樣的內容填進 Metal 的參數緩衝。反射 json 裡每個綁定帶一個種類（`nvn/nvn_refl.h` 的 `NVN_RB_*`）：

| 種類 | 含義 |
|---|---|
| `CBUF` | uniform 緩衝槽 r |
| `TEX` / `SAMP` / `SAMPT` | 分離貼圖槽、分離採樣器槽；`SAMPT` 表示採樣器取自貼圖槽的合併句柄 |
| `IMG` | 可寫貼圖（圖像槽） |
| `TBUF` | 類型化緩衝（緩衝貼圖） |
| `SBUFP` | 存儲緩衝槽的設備指針 |
| `UBOP` | 計算階段 uniform 槽的設備指針 |
| `C1` | 著色器自帶的常數段 |
| `DRV0` / `DRV2` | 驅動常量段 c[0]（存儲緩衝地址表）與 c[2]（貼圖、採樣器句柄表），由回放層按當前綁定現填 |
| `SUPPORT` | 翻譯器自己用的輔助常量 |

沒有綁定的貼圖槽給一張啞貼圖，類型與像素類別取自反射。

## 4. 加一個還沒實現的函數

1. 在 `nvn/hle_nvn.c` 裡用 `NVN(名字, 返回類型, 參數…)` 宏寫實現。宏會按 `nvn/nvn_protos.h` 做簽名檢查，並把實現登記進槽表；名字不在 `nvn/names.txt` 裡啓動即中止。
2. 如果是命令：在 `nvn/nvn_obj.h` 裡加記錄格式，錄製寫在 `hle_nvn.c`，回放寫在 `nvn/nvn_mtl.m`。
3. 在 `tests/` 裡加測試，`make -C dm/gfx/tests run`。
