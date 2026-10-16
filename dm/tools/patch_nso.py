# 通用 NSO 改寫器：Switch aarch64 模塊 → 可在蘋果系統原生執行的映像。
# 用法：patch_nso.py <NSO 文件> <輸出目錄> <模塊名>
# 改寫（蘋果內核會改 x18 與 tpidr_el0；Horizon 的線程區、svc、計時器在蘋果上沒有對應物）：
#   x18 讀寫            → 線程槽 K18（TSD，經 tpidrro_el0）
#   mrs tpidr_el0       → 讀線程槽 KTLS；msr tpidr_el0 → 寫 KTLS
#   mrs tpidrro_el0     → 讀線程槽 KTLR（Horizon ThreadLocalRegion 指針）
#   svc #n、mrs cntpct_el0 → 宿主門（門地址取自線程槽 KGATE，進 C 後按返回點查 gates.tsv）
#   已登記剪口（CUTS，按 NSO 構建號限定）：改寫前逐條斷言原字節，不符即中止；結果列在 <名>.cuts.tsv
#   已登記觀測門（OBS，按構建號限定）：入口一條指令換成「進宿主觀測回調 → 執行原指令 → 跳回」，診斷零運行時改代碼；列在 <名>.obs.tsv
# 鍵頁只在數據段末尾保存“實際鍵號 × 8”，補丁代碼通過鍵頁間接取線程槽：裝載器可使用任意鍵號。
# 輸出：<名>.patched.bin（鍵頁與補丁區接在映像後 PATCH_OFF）、<名>.fixups.tsv（佈局與鍵頁下標表頭）、<名>.gates.tsv、<名>.sites.tsv、<名>.cuts.tsv、<名>.obs.tsv
# Generic NSO rewriter: Switch aarch64 module → an image that can execute natively on Apple systems.
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Rewrites (the Apple kernel modifies x18 and tpidr_el0; Horizon's thread area, svc and timers have no counterpart on Apple):
#   x18 read/write      → thread slot K18 (TSD, via tpidrro_el0)
#   mrs tpidr_el0       → read thread slot KTLS; msr tpidr_el0 → write KTLS
#   mrs tpidrro_el0     → read thread slot KTLR (Horizon ThreadLocalRegion pointer)
#   svc #n, mrs cntpct_el0 → host gate (gate address taken from thread slot KGATE; once in C, gates.tsv is looked up by return point)
#   Registered cuts (CUTS, restricted by NSO build ID): before rewriting, the original bytes are asserted one by one and any mismatch aborts; results are listed in <name>.cuts.tsv
#   Registered observation gates (OBS, restricted by build ID): one instruction at the entry is replaced with "enter the host observation callback → execute the original instruction → jump back", so diagnostics modify zero code at runtime; listed in <name>.obs.tsv
# The key page only stores "actual key number × 8" at the end of the data segment, and patch code fetches thread slots indirectly through the key page: the loader may use any key number.
# Output: <name>.patched.bin (key page and patch area appended after the image at PATCH_OFF), <name>.fixups.tsv (layout and key page index header), <name>.gates.tsv, <name>.sites.tsv, <name>.cuts.tsv, <name>.obs.tsv
import struct, re, sys, os
from itertools import combinations
import lz4.block
from capstone import *

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(src, 'rb').read()
assert d[:4] == b'NSO0'
flags = struct.unpack_from('<I', d, 0xc)[0]
segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
bss = struct.unpack_from('<I', d, 0x3c)[0]
csz = struct.unpack_from('<III', d, 0x60)
IMG = bytearray(segs[2][1] + segs[2][2] + bss)
for i, (fo, mo, sz) in enumerate(segs):
    raw = d[fo:fo + csz[i]]
    if flags >> i & 1: raw = lz4.block.decompress(raw, uncompressed_size=sz)
    IMG[mo:mo + sz] = raw
TEXT_END = segs[0][2]
RO_OFF, DATA_OFF = segs[1][1], segs[2][1]
KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
IMG.extend(bytes(0x4000))
PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF

# ---- 已登記剪口：{NSO 構建號: [(偏移, 原字節, 新字節, 理由)]} ----
# 聯網剪口（排除範圍：戰網 / 聯機）：取自社區離線修復（LFS Mod Offline Fix for v2228224，與官方 94012 main 只差這 7 個字）。
# 實測系統服務層（nifm 無網 / DNS 黑洞 / 賬號服務）繞不過：遊戲自己直接發戰網請求、按「上次在線」時間放行離線。
# ---- Registered cuts: {NSO build ID: [(offset, original bytes, new bytes, reason)]} ----
# Networking cuts (excluded scope: Battle.net / online play): taken from a community offline fix (LFS Mod Offline Fix for v2228224, which differs from the official 94012 main in only these 7 words).
# Measured: the system service layer (nifm offline / DNS black hole / account service) cannot get around it: the game itself sends Battle.net requests directly and allows offline play according to the "last online" time.
CUTS = {
  # Super Mario Bros. Wonder v0 main
  'cd6e42aee7934f4d8393cd43893ae25e': [
    # 資源堆留餘量：資源單元的 ExpHeap 按資源尺寸表值 + 0x1008 建（main+0x1e603c → ExpHeap::create main+0x4aac38），表值是任天堂實測的峰值，幾乎不留餘量
    # （PlayerYoshiAnm 用到 99%）。宿主上開場關 Course900 的 Model/NpcCommonerA.bfres 差約 0x70 字節：模型資源初始化最後一塊顯存（MatBlock，
    # 0x9000 字節、4 KB 對齊，main+0x4290c0）放下後，它的 0x598 字節描述塊放不下，顯存分配失敗 → 緩衝區建在空內存池上 → main+0x106f5c8 讀空崩潰。
    # 與真機差在哪裡未查明（本塊之前的每一項分配尺寸都由遊戲代碼與數據決定）；先給每個資源堆 +16 KB。
    # Resource heap slack: each resource unit's ExpHeap is created with the resource size table value + 0x1008 (main+0x1e603c → ExpHeap::create main+0x4aac38); the table holds Nintendo's measured peaks with almost no margin
    # (PlayerYoshiAnm uses 99%). On the host, Model/NpcCommonerA.bfres in the opening course Course900 is about 0x70 bytes short: after the model resource's last GPU chunk (MatBlock,
    # 0x9000 bytes, 4 KB aligned, main+0x4290c0) is placed, its 0x598-byte descriptor does not fit, the GPU allocation fails → a buffer is built on a null memory pool → null read crash at main+0x106f5c8.
    # Where this differs from hardware is not known (every allocation size before this block is determined by game code and data); for now every resource heap gets +16 KB.
    (0x1e62fc, 'e00317aa', 'e0124091', '資源堆尺寸：mov x0,x23 → add x0,x23,#0x4000（ExpHeap::create 的尺寸參數 +16 KB）'),
  ],
  # Super Mario Bros. Wonder 1.2.1（v327680）main：同上。位置由 dm/tools/nso_fnmap.py 從 v0 搬來（所在函數 0x1e603c → 0x1cf6f0 屏蔽地址後全同，ExpHeap::create 0x4aac38 → 0x4c4a18）
  # Super Mario Bros. Wonder 1.2.1 (v327680) main: same as above. Location carried over from v0 by dm/tools/nso_fnmap.py (containing function 0x1e603c → 0x1cf6f0 identical after address masking, ExpHeap::create 0x4aac38 → 0x4c4a18)
  'ff773e90972d544eb79406eaa65396d5': [
    (0x1cf9b0, 'e00317aa', 'e0124091', '資源堆尺寸：mov x0,x23 → add x0,x23,#0x4000（ExpHeap::create 的尺寸參數 +16 KB）'),
  ],
  'db0507f36b84200ac05f75d76ed5026f': [
    # （放進所有產物共用的基礎改寫）：遊戲顯存分配器預算 260 MB → 512 MB
    # (Placed in the base rewrite shared by all build outputs): game video memory allocator budget 260 MB → 512 MB
    (0x8524c4, '0808a2d2', '0800a4d2', '顯存分配器初始化配置：mov x8,#0x10400000（260 MB）→ mov x8,#0x20000000（512 MB），隨後 movk #0xa0,lsl48 與 str [sp,#0x238] 不變；'
     '原預算下內部分辨率 > 約 345 萬像素時（2736x1260 過、2864x1240 崩）渲染目標放不下，main+0x174b018 返回空、main+0x17417a4 讀空崩潰；'
     '只是顯存預算，不進遊戲邏輯：cvd5 逐拍對拍在 1600x900 與 2868x1320 兩檔均一致'),
    (0x1979bc, 'e8a79f1a', 'e8d79f1a', '離線放行窗口：讀存檔設置鍵（0x1d53e80 處字串）的上次在線時刻，'
     '「當前時刻 − 上次在線 < 0x0009356907420000 ns（= 30 天）」置位 [x20+0x2c]；lt→gt'),
    (0x5b14dc, 'e8a79f1a', 'e8d79f1a', '調 0xddab50（緩衝區 [x10+0x138]+x25、剩餘長度、回調 0x527d50、表 0x209cdb8、9 項）返回值 <1 置位，'
     '為真時走 0x5b14f8 分支；lt→gt（業務含義未從字串定出）'),
    (0x5f3184, 'e8a79f1a', 'e8d79f1a', '數據表 [0x2d64a18+idx*16] → +0xf90 記錄（0x1fc 字節）+0x4c 下標 → +0xfd0 記錄（0x128 字節）+0xf1 有符號字節 <0 置位，'
     '與（w26>0x24）相與後作第 4 參傳下游；lt→gt（業務含義未從字串定出）'),
    (0x1d53e80, '55736572204c617374204f6e6c696e6500', '566f6963654c616e677561676500000000',
     '只讀字串 "User Last Online" → "VoiceLanguage"：第 1 處改讀一個必定存在的設置鍵，「上次在線」取到小數值，差值恆超 30 天，配合 gt 恆放行'),
    # 產品剪口：開場 Logo 與「按 A 開始」頁剪掉，啓動直達主菜單（裝載畫面保留）
    # Product cut: the opening Logo and the 「按 A 開始」(Press A to Start) page are cut, and startup goes straight to the main menu (the loading screen is kept)
    (0x8019ec, '280f0034', 'e8feff34', '前端引導首次進入分支：cbz w8,#0x801bd0（LogoMenuPanel → Logo 視頻 → 標題頁）改為 cbz w8,#0x8019c8'
     '（落到直達路徑的 0x23e5e0(1) → 0xb156a0 進主菜單；跳過 0x8019bc 起置聯機模式 [0x2d7badc]=1 的兩條，聯機模式保持啓動初值 0 = 離線，'
     '不觸發 0x80ec44 的聯網檢查、斷網彈窗與回標題頁）；r4 版目標 0x8019bc 會置聯機模式 1，實測仍回標題頁並彈斷網框'),
    # 宿主原生實現剪口（僅宿主：真值模擬器不應用，Ryujinx 不認識 0x80 起的宿主調用號；等價由逐塊解碼 5511/5511 一致證明）
    # Cuts for host-native implementations (host only: the ground-truth emulator does not apply them, Ryujinx does not know host call numbers from 0x80 up; equivalence is proven by per-block decoding matching 5511/5511)
    (0x1b0ee60, 'fc6fbaa9fa6701a9', '011000d4c0035fd6', 'TACT BLTE 塊 zlib 解碼入口（DecodeZlib）→ svc #0x80 ; ret：宿主用 libdeflate 實現'
     '（hle_tact.c，調用號登記在 hle.h SVC_HOST_TACT_ZLIB）；改寫器先打剪口再掃描，svc #0x80 自動成為宿主門，門塊保存恢復 x30，ret 回原調用者', '僅宿主'),
    # 宿主測試開關的落點（僅宿主）：缺省宿主原樣轉調，行為不變
    # Hook points for host test switches (host only): by default the host forwards as is, behavior unchanged
    (0x5d3a88, 'a27a0194', '211000d4', '收包泵 0x5d3960 投遞客戶端命令包的調用點 bl 0x632510 → svc #0x81：宿主缺省以原參數原樣調用 0x632510（返回值調用方不用）；'
     'DM_C2S_REPLAY 時丟棄客戶端自發命令、改由拍入口按文件注入（diag.c，調用號登記在 hle.h SVC_HOST_C2S_DELIVER）', '僅宿主'),
    (0x5d39c8, 'ba750194', '411000d4', '收包泵 0x5d3960 投遞系統類消息的調用點 bl 0x6310b0 → svc #0x82：宿主缺省以原參數原樣調用 0x6310b0（返回值調用方不用）；'
     'DM_C2S_REPLAY 時，回放文件首拍起丟棄宿主客戶端自發的系統類消息（之前的加入消息放行），改由拍入口按文件注入（diag.c，調用號登記在 hle.h SVC_HOST_C2S_SYS）', '僅宿主'),
    # 屏幕比例（僅宿主，配合下方 PARAMS 分辨率參數位）：輸出分辨率上限 {u32 寬, u32 高}，只被分辨率選擇的三處讀寫
    # （0xe12e28 / 0xe10d70 讀：選中值的寬或高超上限即不換；0xe12f54 初始化選完後寫成選中值）；初值 1920x1080 → 4096x2160，讓按屏幕比例放寬的寬度能通過
    # Screen aspect (host only, together with the resolution parameter slots in PARAMS below): output resolution cap {u32 width, u32 height}, read or written at only three places in resolution selection
    # (0xe12e28 / 0xe10d70 read: if the selected value's width or height exceeds the cap, no switch is made; 0xe12f54 writes it to the selected value after initialization finishes selecting); initial value 1920x1080 → 4096x2160, so that a width widened to the screen aspect can pass
    (0x211da30, '8007000038040000', '0010000070080000', '輸出分辨率上限初值 1920x1080 → 4096x2160（數據段）：宿主按屏幕比例給的寬度（如 iPhone 19.5:9 的 1956x900）'
     '須不超上限才會被選中；Mac 前端仍給原值 1600x900 / 1280x720，行為不變', '僅宿主'),
    # 運行模式通知處理（0xe10d30）給「窗口尺寸變化」事件的寬度：原碼 mov w9,#0x740; and x9,x25,x9——編譯器知道寬只可能是 1280（0x500）/ 1600（0x640），
    # 把「取低 32 位」優化成了 & (0x500|0x640)。KRES0 / KRES1 參數位換掉了那兩個常數，這個推導出來的掩碼沒跟著改：2868 & 0x740 = 768、2296 & 0x740 = 64
    # （運行中切主機 / 掌機後輸出寬 64 / 768，iOS 過熱降級走的就是這條）。改成直接取低 32 位：對原版兩個寬度結果相同，對任意寬度正確
    # The width that the operation-mode notification handler (0xe10d30) gives to the "window size changed" event: original code mov w9,#0x740; and x9,x25,x9 -- the compiler knew the width could only be 1280 (0x500) / 1600 (0x640),
    # and optimized "take the low 32 bits" into & (0x500|0x640). The KRES0 / KRES1 parameter slots replaced those two constants, but this derived mask was not changed along with them: 2868 & 0x740 = 768, 2296 & 0x740 = 64
    # (after switching docked / handheld while running, the output width was 64 / 768; the iOS overheating downgrade takes exactly this path). Changed to take the low 32 bits directly: same result for the original game's two widths, correct for any width
    (0xe10e48, '09e880522903098a', 'e903192a1f2003d5', '運行模式通知發窗口尺寸事件的寬度：mov w9,#0x740; and x9,x25,x9 → mov w9,w25; nop（取低 32 位，不再用原版兩檔寬度推出的常數掩碼）', '僅宿主'),
    (0xe12e88, '09e880528902098a', 'e903142a1f2003d5', '啓動初始化選分辨率（0xe12d70）同一處掩碼：mov w9,#0x740; and x9,x20,x9 → mov w9,w20; nop（structck 參數門下游掩碼檢查查出；啓動時事件接收對象通常未建，平時不顯形）', '僅宿主'),
    # 產品剪口（復刻排除清單「經典畫面切換」）：經典畫面切換 0xb15d60 的兩條狀態寫入 → nop。
    # 畫面過渡狀態在 0x2dc0758 起（+4 起點、+8 目標、+0xc 進度；1.0 = 新畫面）；全程序寫它的只有 0x852280（啓動初始化寫 1）、0x85cb20（強制回新畫面寫 1）、
    # 0x856be0（只寫 +0xc）與 0xb15d60（不在局內或局版本 1 / 2 時按當前系數在 0 / 1 間翻轉目標）——後者是唯一能把目標推向 0 的地方；
    # 調用方 0x36a5a0 / 0x4035e8 / 0xc25f48 / 0xc2725c 都是切換動作（視頻選項 LegacyGraphicsMode、建角頁 ToggleLegacy、主機版 ClassicToggle、按鍵 LegacyToggle 匯入此處）。
    # 剪掉的是兩條普通存儲、不含調用：被剪部分的同步原語與對外副作用清單為空（剪口規矩）。局內局版本 3 的提示分支（0xb15e2c 起）不動
    # Product cut (reproducing the exclusion list item "classic picture toggle"): the two state writes of the classic picture toggle 0xb15d60 → nop.
    # The picture transition state starts at 0x2dc0758 (+4 start, +8 target, +0xc progress; 1.0 = new picture); in the whole program the only writers are 0x852280 (startup init writes 1), 0x85cb20 (force back to the new picture, writes 1),
    # 0x856be0 (writes only +0xc) and 0xb15d60 (when not in a run, or when the run version is 1 / 2, flips the target between 0 / 1 according to the current coefficient) -- the last is the only place that can push the target toward 0;
    # the callers 0x36a5a0 / 0x4035e8 / 0xc25f48 / 0xc2725c are all toggle actions (the video option LegacyGraphicsMode, the character creation page's ToggleLegacy, the console version's ClassicToggle and the key LegacyToggle all converge here).
    # What is cut is two plain stores, containing no calls: the list of synchronization primitives and external side effects of the cut part is empty (the cut rule). The hint branch for run version 3 in a run (from 0xb15e2c) is untouched
    (0xb15e20, '0281002d', '1f2003d5', '經典畫面切換寫起點 / 目標（stp s2, s0, [x8, #4]）→ nop：目標恆為 1.0（新畫面）'),
    (0xb15e24, '1f0d00b9', '1f2003d5', '經典畫面切換清進度（str wzr, [x8, #0xc]）→ nop'),
    # 寬屏（僅宿主，配合 PARAMS 的 KASPECT 寬高比上限）：界面畫布寬 = min(屏寬, 4560 = 2160 × 19/9) 的上限放開到 0xffff。
    # 原版 16:9 下屏寬 3840 單位 < 4560，結果不變；iPhone 1952x900 下屏寬約 4685 單位，原先被夾到 4560、兩側各留約 26 像素
    # Widescreen (host only, together with the KASPECT aspect ratio cap in PARAMS): the cap in UI canvas width = min(screen width, 4560 = 2160 × 19/9) is opened up to 0xffff.
    # In the original game at 16:9 the screen width of 3840 units < 4560, so the result is unchanged; on iPhone 1952x900 the screen width is about 4685 units, which used to be clamped to 4560, leaving about 26 pixels on each side
    (0xb1a660, '083a8252', 'e8ff9f52', '界面畫布寬上限 mov w8, #4560 → mov w8, #0xffff（畫布寬 = 屏寬）', '僅宿主'),
    # （撤銷：原 r13 / r15 起的「SetLanguage 0x317298 清空 + 重裝字符串表」剪口 0x3172f8 / 0x3173e4（原為排除簡中以外的語言而設），
    # 因英文版撤銷——原版切語言要靠這次重裝。）
    # 產品剪口：字符串表 A（legacy，0x2d70ff0 / 0x2d71010）的裝載整段跳過。裝載函數 0x7f64f0(w0 = 是否裝 legacy)
    # 在 0x7f669c tbz w23 處分支：不裝時成功標誌置 1（0x7f6800），與 17 個 strings-legacy/*.json 全部裝成功的結果相同。
    # 無讀者證明：全程序引用表 A 的 128 個函數裡 124 個同時讀選表標誌 [0x2d71098] 並按「標誌 = 0 → 表 B（0x2d71030 / 0x2d71050）」選表
    # （如 0x13b7d8 csel），其餘 4 個是表自身的靜態構造 0x7f8f80、裝載 0x7f64f0、插表 0x7f72e0、清空 0x7f5b60。標誌唯一寫入點 0xb1bcf4
    # 寫 !obj[0xb9]，obj[0xb9] 由畫面過渡狀態 0x2dc0758 與 1.0 比較得出——r14 已剪掉唯一能把目標推離 1.0 的經典畫面切換；標誌在 .bss
    # （初值 0），r15 cvd5 全程探針 0xb1bcfc 命中 0 次（寫入點從未執行）⇒ 全程只讀表 B。被剪樹 = 17 次 0x7f72e0 單文件裝載，同上方
    # 語言重裝剪口的補證（同一 0xdf67f0 讀事務，成對、無外部等待者）
    # (Reverted: the cuts 0x3172f8 / 0x3173e4 for "SetLanguage 0x317298 clear + reload string tables" in place since r13 / r15 (originally set up to exclude languages other than Simplified Chinese),
    # reverted because of the English version -- switching language in the original game relies on this reload.)
    # Product cut: the loading of string table A (legacy, 0x2d70ff0 / 0x2d71010) is skipped as a whole. The load function 0x7f64f0(w0 = whether to load legacy)
    # branches at the tbz w23 at 0x7f669c: when not loading, the success flag is set to 1 (0x7f6800), the same result as all 17 strings-legacy/*.json loading successfully.
    # No-reader proof: of the 128 functions in the whole program that reference table A, 124 also read the table-select flag [0x2d71098] and select the table by "flag = 0 → table B (0x2d71030 / 0x2d71050)"
    # (e.g. the csel at 0x13b7d8); the other 4 are the table's own static constructor 0x7f8f80, load 0x7f64f0, table insert 0x7f72e0 and clear 0x7f5b60. The flag's only write site 0xb1bcf4
    # writes !obj[0xb9], and obj[0xb9] is obtained by comparing the picture transition state 0x2dc0758 with 1.0 -- r14 already cut the classic picture toggle, the only thing that can push the target away from 1.0; the flag is in .bss
    # (initial value 0), and the probe at 0xb1bcfc over the whole r15 cvd5 run hit 0 times (the write site never executed) ⇒ only table B is read throughout. The cut tree = 17 single-file loads via 0x7f72e0; same as the supplementary proof of
    # the language reload cut above (the same 0xdf67f0 read transaction, paired, no external waiters)
    (0x7f669c, '370b0036', '59000014', '字符串表裝載 tbz w23, #0（是否裝 legacy 表 A）→ b 0x7f6800：表 A 無讀者（選表標誌恆 0 → 表 B），整段 17 個 legacy 文件不裝'),
    # Granny 段解碼（BitKnit2）兩條流式段讀取路徑的逐塊解碼調用點：宿主缺省原樣轉調 0x11f4af0（預解碼方案 A 的替換落點，hle_granny.c）
    # The per-block decode call sites of the two streaming segment read paths of Granny segment decoding (BitKnit2): by default the host forwards as is to 0x11f4af0 (the replacement hook point of pre-decode plan A, hle_granny.c)
    (0x11f02dc, '05120094', '611000d4', '段讀取 0x11f0110 逐塊解碼 bl 0x11f4af0(ctx, 塊, 長度) → svc #0x83：宿主缺省以原參數原樣調用並回填 w0（hle.h SVC_HOST_GRANNY_A）', '僅宿主'),
    # 掉落光（僅宿主）：物品取外觀定義路徑 bl 0xa14dc4(資源管理器, 單位, x8 輸出字符串) → svc #0x87：宿主原樣代調，
    # 對模式 3 / 5（地上 / 掉落中）的物品按規則把路徑改指覆蓋層裡的帶光變體（dm_dropfilter.c svc_ground_asset；hle.h SVC_HOST_GROUND_ASSET）
    # 一檔掉落音（僅宿主）：落地取掉落音 bl 0x470c20(物品, &觸發幀) → svc #0x88：宿主原樣代調，一檔物品換成提示音（dm_dropfilter.c svc_drop_sound）
    # Drop light (host only): the item's appearance definition path lookup bl 0xa14dc4(resource manager, unit, x8 output string) → svc #0x87: the host forwards the call as is,
    # and for items in mode 3 / 5 (on the ground / dropping) repoints the path by rule to the lit variant in the overlay (dm_dropfilter.c svc_ground_asset; hle.h SVC_HOST_GROUND_ASSET)
    # Tier-one drop sound (host only): fetching the drop sound on landing bl 0x470c20(item, &trigger frame) → svc #0x88: the host forwards the call as is, and tier-one items get a notification sound instead (dm_dropfilter.c svc_drop_sound)
    (0x468f2c, '3d1f0094', '011100d4', '物品落地取掉落音調用點 bl 0x470c20 → svc #0x88：宿主原樣代調後按規則把一檔物品的掉落音換成提示音（其餘原樣）', '僅宿主'),
    (0x8fa590, '0d6a0494', 'e11000d4', '物品外觀路徑調用點 bl 0xa14dc4 → svc #0x87：宿主原樣代調後按規則改指地上物品的帶光變體（存儲 / 穿戴 / 腰帶 / 鑲嵌不碰）', '僅宿主'),
    # 進局出生點（進局落在城鎮小站旁、屍體與人同處；僅宿主）：玩家進局函數 0x5d7f70 裡給玩家、給屍體各取一次出生點，都是 bl 0x5596c0(數據版本, 幕, 關卡, 格類型 0, &x, &y, 單位尺寸)
    # → svc #0x8a（玩家）/ #0x8b（屍體）：宿主原樣代調；進局在城鎮時改按「小站格 13 + 固定安全距離」取點，人、屍體、傭兵由原版一次放好（不再第 2 拍換關挪人，屍體不再留在原出生點）（dm_townwp.c svc_join_spawn）
    # Run-entry spawn point (entering a run lands next to the town waypoint, with the corpse in the same place as the player; host only): the player join function 0x5d7f70 fetches a spawn point once for the player and once for the corpse, both via bl 0x5596c0(data version, act, level, cell type 0, &x, &y, unit size)
    # → svc #0x8a (player) / #0x8b (corpse): the host forwards the call as is; when the run is entered in town it instead picks the point by "waypoint cell 13 + a fixed safe distance", and the player, corpse and mercenary are placed in one go by the original game (the player is no longer moved by a level change on tick 2, and the corpse no longer stays at the original spawn point) (dm_townwp.c svc_join_spawn)
    (0x5d8040, 'a005fe97', '411100d4', '玩家進局取出生點調用點 bl 0x5596c0 → svc #0x8a：宿主原樣代調，進局在城鎮時出生點換成小站旁（hle.h SVC_HOST_JOIN_SPAWN）', '僅宿主'),
    (0x5d8400, 'b004fe97', '611100d4', '進局給屍體取落點調用點 bl 0x5596c0 → svc #0x8b：同上，屍體與人取同一個點（hle.h SVC_HOST_JOIN_CORPSE）', '僅宿主'),
    (0x11f0914, '77100094', '811000d4', '段讀取 0x11f07d0 逐塊解碼 bl 0x11f4af0(ctx, 塊, 長度) → svc #0x84：同上（hle.h SVC_HOST_GRANNY_B）', '僅宿主'),
  ],
  # 火焰紋章 Engage 2.0.0（v327680）sdk：AddressRegionManager 節點區 0x3fe00000（約 1 GB）→ 0x3e00000（62 MB）。
  # 構造函數（sdk+0x346420）在別名區開頭划出節點區（0x30 字節一個節點的單元堆，用到才映射），其後才給 AllocateAddressRegion 分區域。
  # 真機別名區 64 GB，這 1 GB 無所謂；宿主別名區受 iOS 單塊預留 6 GB 所限只有 5.375 GB，StandardAllocator 一塊 3.14 GB（按 1 GB 對齊）之後
  # Unity 的低層虛擬分配器（main+0x9aa0f4 → AllocateAddressRegion）只剩約 1.2 GB，序章戰鬥約 357 s 時分不到 → 返回 0 → main+0x9a4664 寫空崩潰。
  # 62 MB 夠約 135 萬個節點（實測活著的區域幾百個）。兩條 mov 必須一起改：+0x346430 定節點區上限，+0x3464cc 把它登記成已佔區域。
  # Fire Emblem Engage 2.0.0 (v327680) sdk: AddressRegionManager node area 0x3fe00000 (about 1 GB) → 0x3e00000 (62 MB).
  # The constructor (sdk+0x346420) carves the node area out of the start of the alias region (a unit heap of 0x30-byte nodes, mapped only as used) before AllocateAddressRegion hands out regions.
  # The hardware alias region is 64 GB, so the 1 GB does not matter there; the host's alias region is only 5.375 GB (iOS caps a single reservation at 6 GB), and after StandardAllocator's 3.14 GB block (1 GB aligned)
  # Unity's low-level virtual allocator (main+0x9aa0f4 → AllocateAddressRegion) had only about 1.2 GB left; about 357 s into the prologue battle it got no region → returned 0 → null write crash at main+0x9a4664.
  # 62 MB holds about 1.35 million nodes (a few hundred regions are live in measurements). Both movs must change together: +0x346430 sets the node area limit, +0x3464cc registers it as an occupied region.
  # 火焰紋章 Engage 2.0.0 (v327680) main：按遊戲自己的 60 幀路徑跑（垂直同步計數恆為 1）。
  # 遊戲的時間類 App.GameTime 按場景選垂直同步計數（Slow = 2，即 30 幀：系統、片頭、演示、戰鬥演出、據點；Normal 只在 GameConfig.IsPerformance 時為 1），
  # 經 GameTime.SetVsyncCount(int) 交給 Unity（QualitySettings.vSyncCount → NVN 出圖間隔）。約 96 處逐幀邏輯（鏡頭、地圖光標、單位演員、菜單滾動與光標、據點鏡頭、小遊戲）
  # 按 GameTime.VsyncFixedCount（= vSyncCount，每幀推進幾個 60 Hz 節拍）走：計數 2 而屏幕 60 幀時這些都快一倍，計數 1 時 60 幀剛好。
  # 按真實時間走的 VsyncDeltaCount / VsyncDeltaTime（據點人物移動、釣魚、龍騎等約 120 處）與幀率無關。
  # 剪在 Unity 的托管包裝 QualitySettings.set_vSyncCount（main+0x32f92c0）：兩條路徑都用 w19 當參數，mov w19,w0 → mov w19,#1，
  # 所有調用者（GameTime、QualitySettingsStack.PopImpl、XRSystem）都只能設 1；讀值（get_vSyncCount）隨之為 1，遊戲自洽。沒有代碼調 SetQualityLevel。
  # Fire Emblem Engage 2.0.0 (v327680) main: run the game's own 60 fps path (vertical sync count always 1).
  # The game's time class App.GameTime picks a vertical sync count per scene (Slow = 2, i.e. 30 fps: system, movies, demos, combat cutscenes, the hub; Normal is 1 only when GameConfig.IsPerformance)
  # and hands it to Unity through GameTime.SetVsyncCount(int) (QualitySettings.vSyncCount → NVN present interval). About 96 pieces of per-frame logic (cameras, map cursor, unit actors, menu scrolling and cursors, hub camera, minigames)
  # step by GameTime.VsyncFixedCount (= vSyncCount, how many 60 Hz ticks each frame advances): with a count of 2 and 60 frames on screen they all run twice as fast, with a count of 1 at 60 fps they are right.
  # The real-time VsyncDeltaCount / VsyncDeltaTime (hub character movement, fishing, dragon ride and so on, about 120 places) do not depend on the frame rate.
  # The cut is in Unity's managed wrapper QualitySettings.set_vSyncCount (main+0x32f92c0): both paths use w19 as the argument, mov w19,w0 → mov w19,#1,
  # so every caller (GameTime, QualitySettingsStack.PopImpl, XRSystem) can only set 1; the read side (get_vSyncCount) follows at 1 and the game stays consistent. No code calls SetQualityLevel.
  '8c08b9719e085f91847b5e0f935d9488': [
    (0x32f92d4, 'f303002a', '33008052', 'QualitySettings.set_vSyncCount 包裝：參數 mov w19,w0 → mov w19,#1（遊戲自己的 60 幀路徑）', '僅宿主'),
  ],
  '56ec24747ec9e7f19b3efb062483a19d': [
    (0x346430, '08fca752', '087ca052', 'AddressRegionManager 構造：節點區上限 mov w8,#0x3fe00000 → mov w8,#0x3e00000', '僅宿主'),
    (0x3464cc, '09fca752', '097ca052', 'AddressRegionManager 構造：節點區登記為已佔區域的長度 mov w9,#0x3fe00000 → mov w9,#0x3e00000', '僅宿主'),
  ],
  # 朋友收集 夢想生活 v0 main：點哪選哪（Mac 滑鼠點擊 = 觸屏）。界面的手柄 T（sead::Controller 子類，虛表 main+0x31f9308）每幀在 main+0x3f7600 選指針模式：
  # 1 自由指針、2 觸屏指針、3 焦點（小手在按鈕間跳）。它讀兩次 T+0x1c0（觸屏允許位，各場景經 main+0x1cfaf50 / 0x1cfaf80 設清）：非 0 時才看觸屏元件的 0 號手指
  # 有沒有剛按下、按下就切到 2（0x3f774c）；為 0 而當前是 2 就立刻切回 3（0x3f77c0）。在 2 時 main+0x1d32cb0 拿 0 號手指的位置當指針，界面按指針點中按鈕。
  # 原版只有捏 Mii、工作室等場景設這個位（任天堂 FAQ：觸屏只在部分場景能用），島名頁等選單點了沒反應。兩處剪口讓這兩次讀都當「允許」：手指按下就切觸屏指針、
  # 不被拉回焦點模式，推搖桿 / 按鍵照原版切回 1 / 3。T+0x1c0 本身不改（它另管指針範圍與鏡頭分支）。只剪第一處不夠：同一幀就被第二處拉回 3（實測點了不選中）。
  # 實測（掌機）：點島名 → 選中、OK 變亮；點 OK → 進入確認頁。
  # Tomodachi Life: Living the Dream v0 main: tap to select (a Mac mouse click = touch). The UI controller T (a sead::Controller subclass, vtable main+0x31f9308) picks its pointer mode every frame in main+0x3f7600:
  # 1 free pointer, 2 touch pointer, 3 focus (the hand hops between buttons). It reads T+0x1c0 (touch-allowed bits, set and cleared per scene through main+0x1cfaf50 / 0x1cfaf80) twice: only when it is non-zero does it look at whether the touch element's finger 0
  # was just pressed and switch to 2 (0x3f774c); when it is 0 while the mode is 2 it switches straight back to 3 (0x3f77c0). In mode 2, main+0x1d32cb0 takes finger 0's position as the pointer and the UI hits buttons with it.
  # The original sets those bits only in scenes such as Mii editing and the workshop (Nintendo's FAQ: touch works only in some scenes), so menus such as the island name page ignore taps. The two cuts make both reads "allowed": a finger press switches to the touch pointer
  # and is not pulled back to focus mode, and the stick / buttons switch back to 1 / 3 as in the original. T+0x1c0 itself is untouched (it also governs the pointer bounds and a camera branch). Cutting only the first is not enough: the second pulls it back to 3 in the same frame (measured: the tap selected nothing).
  # Measured (handheld): tapping an island name selects it and lights OK; tapping OK moves on to the confirmation page.
  '56bf85bd535413464cb75bb6c2683b67': [
    (0x3f774c, '69824379', '29008052', 'UI 手柄選指針模式：觸屏允許位 ldrh w9,[x19,#0x1c0] → mov w9,#1（每頁都看 0 號手指按下，按下就切觸屏指針）', '僅宿主'),
    (0x3f77c0, '69824379', '29008052', 'UI 手柄選指針模式：觸屏允許位 ldrh w9,[x19,#0x1c0] → mov w9,#1（觸屏指針模式不被拉回焦點模式）', '僅宿主'),
  ],
}
# ---- 可選剪口：{NSO 構建號: {選項名: [(偏移, 原字節, 新字節, 理由)]}} ----
# 出包時照樣逐條斷言原字節，但不寫進映像，只列進 <名>.options.tsv；Mac 宿主裝載時按 DM_OPTIONS（逗號分隔的選項名）在封頁之前套上，套前再核一次原字節（loader.c）。
# 給玩家在 App 裡開關、重開遊戲才生效的改動（如 60 FPS）。iOS 的代碼來自簽名的可執行文件，裝載時改不了，這類選項目前只有 Mac 有
# ---- Optional cuts: {NSO build ID: {option name: [(offset, original bytes, new bytes, reason)]}} ----
# At packaging the original bytes are asserted one by one as usual, but nothing is written into the image; they are only listed in <name>.options.tsv, and the Mac host applies them at load time, before sealing the pages, per DM_OPTIONS (comma-separated option names), checking the original bytes again (loader.c).
# For changes the player switches in the app that take effect after the game restarts (such as 60 FPS). On iOS the code comes from the signed executable and cannot be changed at load time, so such options exist only on the Mac for now
OPTIONS = {
  # 朋友收集 夢想生活 v0 main：60 FPS。幀率控制函數 0x28b2b8 從設定讀出圖間隔，存 +0x40，再算 60 / 間隔 存 +0x50（遊戲的每幀步長按它算）。
  # 0x28b35c 的 ldr w9,[x0,#0x24] 換成 mov w20,#1：出圖間隔 1，幀率 60，步長減半，遊戲速度不變。位址取自 Fl4sh9174 的 60 FPS 模組（v1.0.0 pchtxt），
  # 宿主反匯編核過；實測同一份操作按時間對齊後，60 FPS 第 2N 幀與 30 FPS 第 N 幀畫面相同（只把宿主出圖改成每節拍一幀則是兩倍速）
  # Tomodachi Life: Living the Dream v0 main: 60 FPS. The frame-rate control function 0x28b2b8 reads the present interval from its settings, stores it at +0x40, then stores 60 / interval at +0x50 (the game's per-frame step follows it).
  # Replacing ldr w9,[x0,#0x24] at 0x28b35c with mov w20,#1 gives interval 1, 60 fps and half the step, so the game runs at the same speed. Address from Fl4sh9174's 60 FPS mod (v1.0.0 pchtxt),
  # checked by disassembly in the host; measured: with the same input aligned by time, frame 2N at 60 FPS matches frame N at 30 FPS (only making the host present every tick runs the game at double speed)
  '56bf85bd535413464cb75bb6c2683b67': {
    'fps60': [(0x28b35c, '092440b9', '34008052', '幀率控制：出圖間隔 ldr w9,[x0,#0x24] → mov w20,#1（60 FPS，步長減半）')],
  },
}
# ---- 已登記觀測門：{NSO 構建號: [(偏移, 編號, 原指令字, 理由)]}。編號與宿主 hle.h 的 OBS_* 一致；
# 原指令須與位置無關（不得 PC 相對 / 分支 / 涉 x18·tpidr），離線斷言；宿主未掛回調時門塊進出一次即返回 ----
# ---- Registered observation gates: {NSO build ID: [(offset, number, original instruction word, reason)]}. Numbers match OBS_* in the host's hle.h;
# the original instruction must be position independent (no PC-relative / branch / anything involving x18 or tpidr), asserted offline; when the host has no callback attached the gate block goes in and out once and returns ----
OBS = {'db0507f36b84200ac05f75d76ed5026f': [
    (0xb1dc60, 1, 'd10603ff', 'UI 消息總線虛函數實現入口（x1 = 消息 {u64 類型哈希, u64 動作哈希, char* 面板名, …}）：'
     'DM_UIMSG 消息流與 DM_BOOTPROF 啓動分段的唯一來源；原指令 sub sp, sp, #0x180'),
    (0x5d3bc0, 2, 'd10503ff', '服務端拍入口（x0 = 遊戲對象 G）：DM_TICKS 逐拍 6 格狀態哈希（dm/verify/tick_state_format.md，與真值 swtick.py 同一採集點）；'
     '原指令 sub sp, sp, #0x140'),
    (0x6e5b40, 3, 'd10103ff', '客戶端命令分派入口 (G, 單位, 包, 長度)：DM_C2S_LOG 命令包序列（dm/verify/command_packet_format.md，與真值同一採集點）；'
     '原指令 sub sp, sp, #0x40'),
    (0x6310b0, 4, 'a9bb67fc', '系統類客戶端消息處理入口 (buf = u32 客戶端號 + 包, 包長)：包號 ≥ 0x67（加入 / 心跳 0x6d / 0x6f 等），收包泵先抽乾此隊列；'
     'DM_C2S_LOG 記為 "q":"sys"；原指令 stp x28, x25, [sp, #-0x50]!'),
    (0x317298, 5, 'd10183ff', 'SetLanguage 入口（x1 → u32 新語言號，語言名表 main+0x20ae478）：配合上方語言重載剪口，宿主斷言新語言為 zhCN；'
     '原指令 sub sp, sp, #0x60'),
]}
BUILD_ID = d[0x40:0x50].hex()
# 構建期產品規則：舊原資源/對拍域默認不變；觸屏版單獨出新版本，不在運行時寫客體代碼。
# Build-time product rules: the old original-resource / comparison domain is unchanged by default; the touchscreen version is issued separately as a new version, and guest code is not written at runtime.
RULESET = os.environ.get('DM_RULESET', 'native')
assert RULESET in ('native', 'touch-run-v4'), f'未知規則域 {RULESET}'
RULE_BUILD = 'db0507f36b84200ac05f75d76ed5026f'
RULE_RUN = RULESET == 'touch-run-v4' and NAME == 'main'
if RULE_RUN:
    import hashlib
    assert BUILD_ID == RULE_BUILD, f'Run規則構建號不符 {BUILD_ID}'
    RULE_ORIG_SHA = hashlib.sha256(IMG).hexdigest()
cuts = list(CUTS.get(BUILD_ID, []))
# 觸屏版規則域默認同時帶宿主私有門；規則閘門可顯式關閉，保留同一出包入口和規則域。
# The touchscreen rule domain by default also carries the host-private gates; the rule gate check can turn them off explicitly, keeping the same packaging entry point and rule domain.
PRIVATE_GATES = os.environ.get("DM_PRIVATE_GATES", "1") != "0"
PRIVATE_AIM = RULE_RUN and PRIVATE_GATES
if PRIVATE_AIM:
    cuts.append((0x2faf04, 'fde7fe97', 'c11000d4',
      'skill-aim-v1-private: skill ground direction call only; svc86 delegates original2b4ef8 then sampled-right/original rotation+grid; zero/nonplayer/non-skill retain original; busy/town/skill/server untouched', '僅宿主'))
if RULE_RUN:
    # 方向光陰影圖邊長（畫質檔位 1，0x168a520 填 +0x48 → 建 2 層數組 0x1697394）：2100 → 1024。授權「視覺無差別即可」；
    # mini 108 張 + 連續 16 幀：營地 / 野外 / 戰鬥各檔到 1024 的 FLIP 與同設置重跑的噪聲分不開、影子區相鄰幀變化量 0.71 → 0.75；真機兩張陰影合計約 0.7–1.4 ms / 幀。
    # 遊戲傳給著色器的陰影尺寸常量與視口隨同一配置值變（轉儲核過）。native 規則域（Mac 對原版真值的閘門）保持 2100
    # Directional light shadow map edge length (quality level 1, 0x168a520 fills +0x48 → creates the 2-layer array 0x1697394): 2100 → 1024. Authorization: "visually indistinguishable is enough";
    # on the second Mac, 108 images + 16 consecutive frames: for camp / wilderness / combat, at every level down to 1024 the FLIP cannot be told apart from the noise of a rerun with the same settings, and the change between adjacent frames in shadow regions is 0.71 → 0.75; on a real device the two shadow maps total about 0.7–1.4 ms / frame.
    # The shadow size constants and the viewport that the game passes to the shader change with the same config value (verified by dump). The native rule domain (the Mac gate check against the original game's ground truth) stays at 2100
    SHADOW_SIDE = 1024
    movz_w14 = lambda n: struct.pack('<I', 0x52800000 | (n << 5) | 14).hex()   # movz w14,#n——字節由表達式算，不手寫（手算成 #32768，陰影圖 32768² 分配失敗啓動即崩） | movz w14,#n -- the bytes are computed by an expression, not written by hand (a hand calculation came out as #32768, and the shadow map allocation of 32768² failed, crashing at startup)
    cuts.append((0x168a580, movz_w14(2100), movz_w14(SHADOW_SIDE), f'陰影圖邊長 mov w14,#2100 → mov w14,#{SHADOW_SIDE}（檔位 1）', '僅宿主'))
if RULE_RUN:
    # 純提示的系統通知不吞功能鍵：手柄版通知面板處理函數 0x3d84a0 遇「動作 9（PLUS）按下」就執行通知帶的確認動作併吞鍵（給「升級通知按 PLUS 直達技能樹」用）。
    # 系統通知（事件隊列第 12 類，0xab5c08；如改難度後的「遊戲難度設為 N…」）帶的確認動作只是關掉自己，結果是通知停留的 10 秒裡第一下紅十字沒反應（真機 + Mac 復現）。
    # 改法：系統通知不帶確認動作（類別哈希存 0）；沒有確認動作的通知不處理 PLUS（走原函數的「轉給子控件」出口 0x3d880c），到 notificationSeconds 自己關。帶確認動作的通知不變
    # Purely informational system notifications do not swallow the function key: the controller version's notification panel handler 0x3d84a0, on "action 9 (PLUS) pressed", executes the confirm action carried by the notification and swallows the key (meant for "press PLUS on a level-up notification to go straight to the skill tree").
    # The confirm action carried by a system notification (event queue class 12, 0xab5c08; e.g. 「遊戲難度設為 N…」(game difficulty set to N...) after changing the difficulty) only closes itself, and the result is that during the 10 seconds the notification stays up the first press of the red cross does nothing (reproduced on a real device + Mac).
    # Fix: system notifications carry no confirm action (the category hash is stored as 0); notifications without a confirm action do not handle PLUS (they take the original function's "forward to child widgets" exit 0x3d880c) and close by themselves at notificationSeconds. Notifications with a confirm action are unchanged
    cbz_x8 = lambda at, to: struct.pack('<I', 0xB4000008 | (((to - at) >> 2) & 0x7ffff) << 5).hex()
    str_x19_178 = lambda rt: struct.pack('<I', 0xF9000000 | (0x178 // 8) << 10 | 19 << 5 | rt).hex()
    cuts.append((0xab5c28, str_x19_178(8), str_x19_178(31), '系統通知的確認動作類別 str x8,[x19,#0x178] → str xzr（不帶確認動作）', '僅宿主'))
    cuts.append((0x3d853c, cbz_x8(0x3d853c, 0x3d8560), cbz_x8(0x3d853c, 0x3d880c), '通知面板：確認動作類別為空 → 不處理 PLUS', '僅宿主'))
    cuts.append((0x3d8544, cbz_x8(0x3d8544, 0x3d8560), cbz_x8(0x3d8544, 0x3d880c), '通知面板：確認動作哈希為空 → 不處理 PLUS', '僅宿主'))
if RULE_RUN:
    # 技能說明框出在技能樹側邊、不蓋樹：技能樹面板每幀更新（0xc356b0）在 0xc35fd4 調說明框入口 0xb16660，位置參數 x1 = 圖標絕對坐標（低 32 位 x、高 32 位 y + 3），
    # 手柄版說明框貼著圖標右下展開，觸屏上正好蓋住手指周圍的幾個技能。0xc35fb0 的 bfxil x1,x8,#0,#32 是把 x1 的低半拷回 x1 的空操作（x8 = x1），
    # 換成 movk x1,#3000：橫坐標給到屏幕外，由說明框入口自己的回夾（0xb0d560，按 TooltipsPanel 的 screenEdgePadding）貼到右安全區邊；縱坐標不動
    # The skill tooltip appears beside the skill tree and does not cover the tree: the skill tree panel's per-frame update (0xc356b0) calls the tooltip entry 0xb16660 at 0xc35fd4, with position argument x1 = the icon's absolute coordinates (low 32 bits x, high 32 bits y + 3);
    # the controller version's tooltip expands against the icon's lower right, which on a touchscreen covers exactly the few skills around the finger. The bfxil x1,x8,#0,#32 at 0xc35fb0 is a no-op that copies the low half of x1 back into x1 (x8 = x1),
    # and is replaced with movk x1,#3000: the x coordinate is put off screen, and the tooltip entry's own clamp-back (0xb0d560, by TooltipsPanel's screenEdgePadding) snaps it to the right safe-area edge; the y coordinate is untouched
    cuts.append((0xc35fb0, struct.pack('<I', 0xB3407D01).hex(), struct.pack('<I', 0xF2800000 | 3000 << 5 | 1).hex(), '技能說明框橫坐標給到屏幕外，由原回夾貼右邊（不蓋技能樹）', '僅宿主'))
if RULE_RUN and PRIVATE_GATES:
    # 只登記精力資格/移動選擇/走跑偏好；不改方向、真實幅度、合法mode與零軸分支。
    # 兩個成本/選擇器通用點的type0防護在下方原生小跳板，非player保原行為。
    # Only stamina eligibility / movement selection / walk-run preference are registered; direction, real magnitude, legal mode and the zero-axis branch are not changed.
    # The type0 guard for the two generic cost / selector points is in the small native trampoline below; non-player keeps the original behavior.
    run_words = [
        (0x2c620c, 0xb90e511f, 0x52800029, '偏好初始化：mov w9,1（x8仍為2d20000）'),
        (0x2c6210, 0xd00152c8, 0xb90e5109, '偏好初始化：str w9,[x8,e50]；替代同頁重復adrp，保原x8'),
        (0x369a90, 0xd100c3ff, None, '客戶端RunOff入口尾跳原RunOn；保其player0/合法mode/環境門'),
        (0x369980, 0x340006a0, 0xd503201f, '客戶端RunOn僅取消既有player0門之後的零精力拒絕'),
        (0x2b2c74, 0x340000a0, 0xd503201f, '控制器d29資格僅取消stat10零值限制；幅度比較與停桿保原'),
        (0x2b2dc0, 0x3900031f, 0xd503201f, '控制器d29僅取消stat10零值清除；幅度歸一化保原'),
        (0x2b30fc, 0x0a090101, 0x52800021, '已通過合法動作/零軸門的移動編碼選擇位固定Run；幅度不改'),
        (0x2b3104, 0x2a1f03e1, 0x52800021, '無用戶走跑偏好時亦固定Run編碼位；方向/幅度不改'),
        (0x6dc2c4, 0x1a880502, 0x52800062, 'server42合法移動請求固定Run3，原type0/死亡/鎖定/幅度/方向門保留'),
        (0x6dce90, 0x340000e0, 0xd503201f, 'server49僅取消既有player0/mode2門之後的零精力拒絕'),
        (0x6dcec0, 0xa9bf7bfd, None, 'server4A入口尾跳原49；長度/type0/mode門保留'),
    ]
    for off, old, new, why in run_words:
        if new is None:
            target = 0x369900 if off == 0x369a90 else 0x6dce50
            new = 0x14000000 | (((target - off) >> 2) & 0x3ffffff)
        cuts.append((off, struct.pack('<I', old).hex(), struct.pack('<I', new).hex(), 'touch-run-v4：' + why))
cuts = [c if len(c) == 5 else (*c, '全') for c in cuts]          # 第 5 項作用域：全 | 僅宿主（僅宿主 = 真值模擬器不應用） | 5th item is the scope: 全 (all) | 僅宿主 (host only) (僅宿主 = not applied by the ground-truth emulator)
for off, ob, nb, why, scope in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(ob) == len(nb) and IMG[off:off + len(ob)] == ob, f'剪口原字節不符 {off:#x}：{bytes(IMG[off:off + len(ob)]).hex()}'
    IMG[off:off + len(nb)] = nb
options = [(name, off, ob, nb, why) for name, rows in OPTIONS.get(BUILD_ID, {}).items() for off, ob, nb, why in rows]
for name, off, ob, nb, why in options:   # 可選剪口只核不寫 | optional cuts are checked, not written
    o, n = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(o) == len(n) and IMG[off:off + len(o)] == o, f'可選剪口 {name} 原字節不符 {off:#x}：{bytes(IMG[off:off + len(o)]).hex()}'
# 鍵頁下標固定；鍵頁內容由裝載器寫入“實際鍵號 × 8”。
# Key page indices are fixed; the key page content is written by the loader as "actual key number × 8".
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511, 'KRES0': 506, 'KRES1': 507, 'KASPECT': 505, 'KASPECTUI': 504}   # KRES*：分辨率參數位；KASPECT：畫面寬高比上限（見 PARAMS） | KRES*: resolution parameter slots; KASPECT: picture aspect ratio cap (see PARAMS)
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE', 'KRES0', 'KRES1', 'KASPECT', 'KASPECTUI'))}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_OFF + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

# ---- 編碼器 ----
# ---- Encoders ----
def B(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x14000000 | ((off >> 2) & 0x3FFFFFF)
def STP(t, t2, n, off): return 0xA9000000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP(t, t2, n, off): return 0xA9400000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STP_PRE(t, t2, n, off): return 0xA9800000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP_POST(t, t2, n, off): return 0xA8C00000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STUR(t, n, off): return 0xF8000000 | ((off & 0x1FF) << 12) | (n << 5) | t   # stur xt,[xn,#off]（off 可負） | stur xt,[xn,#off] (off may be negative)
def LDUR(t, n, off): return 0xF8400000 | ((off & 0x1FF) << 12) | (n << 5) | t   # ldur xt,[xn,#off]
def LDRX(t, n, imm12=0): return 0xF9400000 | (imm12 << 10) | (n << 5) | t
def STRX(t, n, imm12=0): return 0xF9000000 | (imm12 << 10) | (n << 5) | t
def MRS_TPIDRRO(t): return 0xD53BD060 | t
def ADRP(dd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12; assert -(1 << 20) <= imm < (1 << 20)
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | dd
def ADDI(dd, n, imm12, sh=0): return 0x91000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def ADDR(dd, n, m): return 0x8B000000 | (m << 16) | (n << 5) | dd
def SUBI(dd, n, imm12, sh=0): return 0xD1000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def BR(n): return 0xD61F0000 | (n << 5)
def CMPR(n, m): return 0xEB00001F | (m << 16) | (n << 5)   # cmp xn, xm（subs xzr） | cmp xn, xm (subs xzr)
def BEQ(frm, to):
    off = to - frm; assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0
    return 0x54000000 | (((off >> 2) & 0x7FFFF) << 5)
def BRK(imm): return 0xD4200000 | ((imm & 0xFFFF) << 5)
def BLR(n): return 0xD63F0000 | (n << 5)
NOP = 0xD503201F

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except Exception: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """x18/w18 → xS/wS：只改 4 個寄存器字段位置，用反匯編文本核對。 | x18/w18 → xS/wS: only the 4 register field positions are changed, verified against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

# ---- 各類改寫塊 ----
# ---- Rewrite blocks of each kind ----
def sp_delta(ins):
    """指令對 sp 的寫回量（前變址 [sp, #i]! / 後變址 [sp], #i）；不寫回 sp 返回 0。 | The amount an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); returns 0 if sp is not written back."""
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

def site_plain(ins):
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    save = -16 if dlt == 0 else -112          # 改 sp 的指令會寫到 sp 下方，暫存放到保留區深處 | Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # 一律寫回：capstone 會漏報寫入（adds w18 實例） | Always write back: capstone can miss reporting a write (the adds w18 instance)
    emit(LDP(S, T, 31, save - dlt)); emit(B(here(), ins.address + 4))
    return start

def site_sp(ins):
    """目標是 sp、源裡有 x18（Engage 的 alloca：mov x18, sp; sub x18, x18, x0; mov sp, x18）。site_plain 的暫存按舊 sp 存、按新 sp 取，會取錯：
    先把舊 sp 抄進 T，指令執行後按 T 恢復 S / T（ldp 不寫回時基址寄存器可以同時是目標）。x18 不被寫，不寫回槽
    Destination is sp and a source is x18 (Engage's alloca: mov x18, sp; sub x18, x18, x0; mov sp, x18). site_plain saves its scratch relative to the old sp and would restore it relative to the new one:
    copy the old sp into T first, and after the instruction restore S / T relative to T (without writeback, ldp may use the base register as a destination). x18 is not written, so the slot is not written back"""
    S, T = pick(regs_in(ins))
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(ADDI(T, 31, 0))             # mov T, sp（add 立即數裡 31 是 sp） | mov T, sp (in add immediate, 31 is sp)
    emit(subst(ins, S))
    emit(LDP(S, T, T, -16)); emit(B(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    m = ins.mnemonic
    taken = here() + 4 * 3
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    assert here() == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), tgt))
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18')   # S 先作鍵頁偏移暫存，下一條再裝目標值 | S first serves as a temporary for the key page offset; the next instruction then loads the target value
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """跳轉表 br x18：目標 = base + 4i。x16 暫存紅區 → 等距落地墊 → 恢復 x16 → 原目標。 | Jump table br x18: target = base + 4i. Stash x16 in the red zone → equally spaced landing pads → restore x16 → original target."""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # x16 / x17 是函數內可存活的臨時寄存器，借用前存紅區 | x16 / x17 are temporary registers that can stay live within a function; save them to the red zone before borrowing
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDUR(17, 31, -24))
    padbase = start + 4 * 24
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi: h = min(hi, 0xFFF); ops.append(op(16, 16, h, 1)); hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    while here() < padbase: emit(NOP)
    tails = padbase + 4 * count
    for i in range(count): emit(B(here(), tails + 8 * i))
    for i in range(count):
        emit(0xF85F0000 | (31 << 5) | 16)      # ldur x16,[sp,#-16]
        emit(B(here(), base + 4 * i))
    return start

def site_br_tab(ins, targets):
    """偏移表 br x18（ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB）：目標 = 表基 + 有符號表項；表是常量，改寫時讀出全部目標。
    x16 = 客體 x18，逐個與目標比（adrp + add 取目標地址到 x17），命中處恢復 x16 / x17 再跳原目標；都不中就 brk（原碼的界檢查已擋掉越界）
    Offset-table br x18 (ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB): target = table base + signed entry; the table is constant, so all targets are read at rewrite time.
    x16 = guest x18, compared with each target in turn (adrp + add put the target address in x17); on a match restore x16 / x17 and jump to the original target; no match → brk (the original bounds check already stops out-of-range indices)"""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # 同 site_br：x16 / x17 借用前存紅區 | as in site_br: save x16 / x17 to the red zone before borrowing
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    uniq = sorted(set(targets))
    pads = here() + 4 * (4 * len(uniq) + 1)
    for k, t in enumerate(uniq):
        emit(ADRP(17, here(), t)); emit(ADDI(17, 17, t & 0xFFF)); emit(CMPR(16, 17)); emit(BEQ(here(), pads + 12 * k))
    emit(BRK(0x18))
    for t in uniq:
        emit(LDUR(16, 31, -16)); emit(LDUR(17, 31, -24)); emit(B(here(), t))
    return start

def site_blr(ins):
    """blr x18（Engage main+0x9ff324）：x16 = 客體 x18，x30 = 原 blr 的下一條，br x16。調用點上 x16 / x17 按調用約定是過程間臨時寄存器，不必存；
    返回地址照原樣，被調者直接返回原處（IL2CPP 的 C++ 異常要按返回地址查展開表，不能停在補丁區）
    blr x18 (Engage main+0x9ff324): x16 = guest x18, x30 = the instruction after the original blr, br x16. At a call site x16 / x17 are intra-procedure-call temporaries under the calling convention, so they need no saving;
    the return address is the original one, so the callee returns straight there (IL2CPP's C++ exceptions look up unwind tables by return address and must not stop in the patch area)"""
    start = here()
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    ret = ins.address + 4
    emit(ADRP(30, here(), ret)); emit(ADDI(30, 30, ret & 0xFFF)); emit(BR(16))
    return start

def site_sysreg_read(ins, kind):
    """mrs xN, tpidr_el0 / tpidrro_el0 → xN = 線程槽值。 | mrs xN, tpidr_el0 / tpidrro_el0 → xN = thread slot value."""
    N = ins.bytes[0] & 31
    tmp = 16 if N != 16 else 17
    start = here()
    emit(STUR(tmp, 31, -16)); emit(MRS_TPIDRRO(N)); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDUR(tmp, 31, -16))   # tmp 存紅區借用 | tmp is saved by borrowing the red zone
    emit(B(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    (T,) = pick({N}, 1)
    start = here()
    emit(0xF81F0000 | (31 << 5) | T)           # stur xT,[sp,#-16]
    tmp = next(x for x in (16, 17) if x not in (N, T))
    emit(STUR(tmp, 31, -24)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T)); emit(LDUR(tmp, 31, -24))
    emit(0xF85F0000 | (31 << 5) | T)           # ldur xT,[sp,#-16]
    emit(B(here(), ins.address + 4))
    return start

def emit_gate_call():
    """門塊公共段（緊跟 stp x29,x30,[sp,#-16]! 之後、blr x30 之前）：x30 = 線程槽 KGATE 的門地址。
    鍵頁取槽要借第二個寄存器，借 x29 後立即從棧上恢復：_dm_gate 存下的 x29 必須是客體原值——
    宿主按 [x29+8] 認調用者（技能綁定觀測門 6、私有瞄準門 svc86）、崩潰回溯也走 x29 鏈
    （r24 方案 C 起借了不還，宿主拿到的 x29 是鍵頁裡的槽偏移，技能圖標退回佔位字）。
    Common section of a gate block (right after stp x29,x30,[sp,#-16]! and before blr x30): x30 = the gate address in thread slot KGATE.
        Fetching the slot through the key page needs a second register to be borrowed; x29 is borrowed and then immediately restored from the stack: the x29 saved by _dm_gate must be the guest's original value --
        the host identifies the caller by [x29+8] (skill binding observation gate 6, private aiming gate svc86), and the crash backtrace also walks the x29 chain
        (since r24 plan C it was borrowed and not returned, the x29 the host got was the slot offset from the key page, and skill icons fell back to placeholder text)."""
    emit(MRS_TPIDRRO(30)); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """svc / mrs cntpct_el0 → 宿主門。門地址取自線程槽 KGATE；門按返回點（本塊 ldp 的地址）查 gates.tsv。 | svc / mrs cntpct_el0 → host gate. The gate address is taken from thread slot KGATE; the gate looks up gates.tsv by return point (the address of this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(B(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_obs(off, oid, word):
    """觀測門：入口原指令 → 進宿主門（arg = 觀測編號）→ 恢復 x29/x30 → 執行原指令 → 跳回 off+4。
    門塊前半與 site_gate 逐字同構；宿主只讀寄存器、不改（dm_gate 回寫的 x0–x8 即原值）。
    Observation gate: original entry instruction → enter the host gate (arg = observation number) → restore x29/x30 → execute the original instruction → jump back to off+4.
        The first half of the gate block is word-for-word isomorphic with site_gate; the host only reads registers and does not modify them (the x0–x8 written back by dm_gate are the original values)."""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'觀測門原指令不可搬 {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(B(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

# ---- 獨佔窗口 ----
# ---- Exclusive windows ----
EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST: j += 1
        if j >= len(insl) or j - i >= 16: continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def site_window(seg):
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    # 出口段：緊跟窗口的第一段必須是「順序落下」（窗口末條之後，hi），窗口內跳出去的目標排在它後面。
    # 原先順序落下排在最後：窗口裡一有跳出去的條件分支（如 ldxr; cmp; b.ne 外; stxr; cbnz），stxr 成功後就落進第一個跳出口，
    # 等於走了「比較不等」那條路（SMBW main 0x81c40：入隊永遠重試，尾指針漲滿、一個任務也沒存進去，任務系統卡死）。D2R 的窗口裡沒有跳出分支，沒碰到。
    # Exit stubs: the first stub right after the window must be the fall-through one (after the last window instruction, hi); targets of branches leaving the window come after it.
    # Previously the fall-through stub came last: as soon as a window contains a conditional branch out (e.g. ldxr; cmp; b.ne out; stxr; cbnz), a successful stxr fell into the
    # first branch-out stub, i.e. took the "compare not equal" path (SMBW main 0x81c40: the enqueue retried forever, the tail index filled up without storing a single task, and the job system hung). D2R's windows had no branch out, so it never hit this.
    exits = copy0 + 4 * n + 16
    ex_targets = []; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        m = x.mnemonic
        if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else: ex_targets.append(tgt); newt = exits + 16 * (len(ex_targets) - 1)
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        elif m == 'adrp':
            # 窗口裡夾的 adrp（朋友收集 main 0x1e2a864：ldxr 與 stxr 之間編譯器排進了 adrp x15/x16/x17）：按副本的位置重編，目標頁不變；adrp 有 ±4 GB，一條換一條
            # An adrp inside the window (Tomodachi Life main 0x1e2a864: the compiler scheduled adrp x15/x16/x17 between ldxr and stxr): re-encode it for the copy's address with the same target page; adrp reaches ±4 GB, so it stays one instruction
            pc = copy0 + 4 * k; tgt = x.operands[1].imm
            w = ADRP(w & 31, pc, tgt)
            dd = dis(w, pc); assert dd.mnemonic == 'adrp' and dd.operands[1].imm == tgt
        else:
            assert m not in ('adr', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    for t in [hi] + ex_targets:   # 順序落下在前 | fall-through first
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), t)); emit(NOP)
    return start, lo, hi

# ---- 函數邊界（eh_frame_hdr）----
# ---- Function boundaries (eh_frame_hdr) ----
m0 = struct.unpack_from('<I', IMG, 4)[0]
assert IMG[m0:m0 + 4] == b'MOD0'
ehs = m0 + struct.unpack_from('<i', IMG, m0 + 16)[0]
def rd(o, enc):
    fmt, app = enc & 0xF, enc & 0x70
    v, sz = {0x3: (struct.unpack_from('<I', IMG, o)[0], 4), 0xB: (struct.unpack_from('<i', IMG, o)[0], 4),
             0x4: (struct.unpack_from('<Q', IMG, o)[0], 8)}[fmt]
    if app == 0x10: v += o
    if app == 0x30: v += ehs
    return v, sz
_, penc, cenc, tenc = IMG[ehs:ehs + 4]
o = ehs + 4
_, s = rd(o, penc); o += s
nfde, s = rd(o, cenc); o += s
starts = set()
for _ in range(nfde):
    a, s = rd(o, tenc); o += s
    _, s = rd(o, tenc); o += s
    starts.add(a)
starts = sorted(x for x in starts if x < TEXT_END)

# ---- 線性掃描找全部改寫點（不靠寄存器讀寫報告）----
# ---- Linear scan to find all rewrite points (not relying on register read/write reports) ----
kinds = {}
for o in range(0, TEXT_END, 4):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    # mrs xN, ctr_el0（緩存類型）：蘋果內核不放給用戶態（SIGILL）；SDK 16 的 nn::os::FlushDataCache 讀它取緩存行長
    # mrs xN, ctr_el0 (cache type): the Apple kernel does not expose it to user mode (SIGILL); SDK 16's nn::os::FlushDataCache reads it for the cache line size
    if w & 0xFFFFFFE0 == 0xD53B0020: kinds[o] = ('ctr', w & 31); continue
    for ins in md.disasm(IMG[o:o + 4], o):
        if has18(ins): kinds[o] = ('x18', None)

# 按函數取指令表，找窗口與跳轉表
# Take the instruction table per function, find the windows and jump tables
import bisect
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END) if i >= 0 else (a, a + 4)
done = set(); windows = []; jumps = {}; tabjumps = {}
def jump_offsets(insl, j):
    """br x18 前是偏移表形（Engage main+0x12eec94）：cmp w18,#N; b.hi; ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB。
    表基 xB 往回找最近的寫者：須是 add xB,xB,#lo，再往前的寫者須是 adrp xB。返回全部目標，認不出返回 None
    The br x18 is preceded by the offset-table form (Engage main+0x12eec94): cmp w18,#N; b.hi; ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB.
    For the table base xB, search back for the nearest writer: it must be add xB,xB,#lo, and the writer before it must be adrp xB. Returns all targets, or None if not recognized"""
    ctx = insl[max(0, j - 16):j]
    add = next((p for p in reversed(ctx) if p.mnemonic == 'add' and re.fullmatch(r'x18, x18, x\d+', p.op_str)), None)
    if not add: return None
    rb = add.op_str.split(', ')[2]
    ld = next((p for p in reversed(ctx) if p.mnemonic == 'ldrsw' and p.op_str == f'x18, [{rb}, x18, lsl #2]'), None)
    cmp = next((p for p in reversed(ctx) if p.mnemonic == 'cmp' and re.fullmatch(r'w18, #(0x[0-9a-f]+|\d+)', p.op_str)), None)
    if not ld or not cmp: return None
    n = int(cmp.op_str.split('#')[1], 0) + 1
    k = insl.index(ld); lo = page = None
    for p in reversed(insl[max(0, k - 256):k]):   # 表基的寫者鏈 | the writer chain of the table base
        _, w = p.regs_access()
        if not any(p.reg_name(r) == rb for r in w): continue
        if lo is None and p.mnemonic == 'add' and re.fullmatch(rf'{rb}, {rb}, #(0x[0-9a-f]+|\d+)', p.op_str): lo = int(p.op_str.split('#')[1], 0); continue
        if lo is not None and p.mnemonic == 'adrp' and p.op_str.startswith(rb + ','): page = p.operands[1].imm
        break
    if page is None: return None
    tb = page + lo
    return [tb + struct.unpack_from('<i', IMG, tb + 4 * i)[0] for i in range(n)]
def table_base(insl, k, rb):
    """表基 rb 的寫者鏈：從第 k 條往回找最近的寫者，須是 add rb,rb,#lo，再往前的寫者須是 adrp rb；返回表基地址，認不出返回 None
    The writer chain of the table base rb: search back from instruction k for the nearest writer, which must be add rb,rb,#lo, and the writer before it must be adrp rb; returns the table base, or None if not recognized"""
    lo = None
    for p in reversed(insl[max(0, k - 256):k]):
        _, w = p.regs_access()
        if not any(p.reg_name(r) == rb for r in w): continue
        if lo is None and p.mnemonic == 'add' and re.fullmatch(rf'{rb}, {rb}, #(0x[0-9a-f]+|\d+)', p.op_str): lo = int(p.op_str.split('#')[1], 0); continue
        if lo is not None and p.mnemonic == 'adrp' and p.op_str.startswith(rb + ','): return p.operands[1].imm + lo
        return None
    return None
def jump_anchor_offsets(insl, j):
    """br x18 前是錨點相對的偏移表形（朋友收集 main+0x31c7c）：cmp wI,#N; b.hi; adr x18,錨點; ldrsw xO,[xB,xI,lsl #2]; add x18,x18,xO。
    表項相對 adr 的錨點而非表基；目標 = 錨點 + 有符號表項。返回全部目標，認不出返回 None
    The br x18 is preceded by the anchor-relative offset-table form (Tomodachi Life main+0x31c7c): cmp wI,#N; b.hi; adr x18,anchor; ldrsw xO,[xB,xI,lsl #2]; add x18,x18,xO.
    Entries are relative to the adr anchor, not to the table base; target = anchor + signed entry. Returns all targets, or None if not recognized"""
    ctx = insl[max(0, j - 16):j]
    add = next((p for p in reversed(ctx) if p.mnemonic == 'add' and re.fullmatch(r'x18, x18, x\d+', p.op_str)), None)
    adr = next((p for p in reversed(ctx) if p.mnemonic == 'adr' and p.op_str.startswith('x18,')), None)
    if not add or not adr: return None
    ro = add.op_str.split(', ')[2]
    ld = next((p for p in reversed(ctx) if p.mnemonic == 'ldrsw' and re.fullmatch(rf'{ro}, \[x\d+, x\d+, lsl #2\]', p.op_str)), None)
    if not ld: return None
    rb, ri = re.fullmatch(rf'{ro}, \[(x\d+), (x\d+), lsl #2\]', ld.op_str).groups()
    k = insl.index(ld)
    cmp = next((p for p in reversed(insl[max(0, k - 8):k]) if p.mnemonic == 'cmp' and re.fullmatch(rf'w{ri[1:]}, #(0x[0-9a-f]+|\d+)', p.op_str)), None)
    if not cmp: return None
    nxt = insl[insl.index(cmp) + 1]
    if nxt.mnemonic not in ('b.hi', 'b.hs', 'b.cs'): return None
    n = int(cmp.op_str.split('#')[1], 0) + (1 if nxt.mnemonic == 'b.hi' else 0)
    tb = table_base(insl, k, rb)
    if tb is None: return None
    anchor = adr.operands[1].imm
    return [anchor + struct.unpack_from('<i', IMG, tb + 4 * i)[0] for i in range(n)]
def disasm_range(fa, fb):
    """反匯編 [fa, fb)，遇到解不出的字跳過 4 字節接著解。capstone 的 disasm 碰到第一個解不出的字就停：朋友收集 main 的函數裡夾著 148 個陷阱字 e7ffdefe（不返回的調用之後），
    停在那裡會漏掉後面的 br x18（main 0x1389754 的字節跳轉表就是這樣漏的）
    Disassemble [fa, fb), skipping 4 bytes past any word that does not decode. capstone's disasm stops at the first word it cannot decode: Tomodachi Life main has 148 trap words e7ffdefe inside functions (after calls that do not return),
    and stopping there missed the br x18 after them (that is how the byte jump table at main 0x1389754 was missed)"""
    out, a = [], fa
    while a < fb:
        got = list(md.disasm(bytes(IMG[a:fb]), a))
        out += got
        a = (got[-1].address + 4) if got else a
        if a < fb and (not got or not list(md.disasm(bytes(IMG[a:a + 4]), a))): a += 4
    return out
funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
for fa, fb in funcs:
    insl = disasm_range(fa, fb)
    ws = find_windows(insl)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            ctx = insl[max(0, j - 8):j]
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'): base = p.operands[1].imm; break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    idx = num(p.reg_name(p.operands[2].reg))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = 65536
                        if width: break
                    break
            if not (base and width):
                tgt = jump_offsets(insl, j) or jump_anchor_offsets(insl, j)
                assert tgt, f'jump table {ins.address:x}'
                tabjumps[ins.address] = tgt; continue
            jumps[ins.address] = (base, width)
    # 跳轉落進窗口中間：只有落點到窗口末條都不碰 x18 才行。窗口只改首條（跳到補丁區），其餘原碼原樣留著，落進來就照原碼執行；
    # 落點之後若有 x18 指令，原碼會碰真 x18，不許。朋友收集 main 0x1e2a898：重試環 ldxr; cmp; b.eq 跳回窗口裡的 stxr，x18 指令（mov w18, w25）在 stxr 之前
    # A jump landing in the middle of a window: allowed only if nothing from the landing point to the window's last instruction touches x18. Only a window's first instruction is replaced (by a branch to the patch area); the rest of the original code stays as it was, so a jump landing there runs the original code;
    # an x18 instruction after the landing point would touch the real x18, which is not allowed. Tomodachi Life main 0x1e2a898: a retry loop ldxr; cmp; b.eq jumps back to the window's stxr, and the x18 instruction (mov w18, w25) comes before the stxr
    seg_of = {x.address: wseg for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    def bad_landing(t):
        if t not in seg_of or t in heads: return False
        return any(has18(y) for y in seg_of[t] if y.address >= t)
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            assert not bad_landing(x.operands[-1].imm), f'branch into window {x.address:x}'
    for a_, tgt in tabjumps.items():
        if fa <= a_ < fb: assert not any(bad_landing(t) for t in tgt), f'jump table target into window {a_:x}'

def put_branch(at, to): struct.pack_into('<I', IMG, at, B(at, to))
for wseg in windows:
    st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for a in sorted(kinds):
    if a in done: continue
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br': st = site_br_tab(ins, tabjumps[a]) if a in tabjumps else site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m == 'blr': st = site_blr(ins); stat('blr')
        elif re.match(r'(sp|wsp),', ins.op_str): st = site_sp(ins); stat('sp')
        elif m in ('ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct'): st = site_gate(ins, k, arg); stat(k)
    elif k == 'ctr':   # 就地換成常數：DminLine = IminLine 字段只填 DminLine = 4（64 字節行）；movz xN, #4, lsl #16 | replaced in place by a constant: DminLine = 4 (64-byte lines); movz xN, #4, lsl #16
        assert arg != 18, f'{a:x} mrs x18, ctr_el0'
        struct.pack_into('<I', IMG, a, 0xD2A00080 | arg); stat(k); continue
    put_branch(a, st)

# 私有視覺分支；非OBS，0x85只讀查詢。只有本實驗構建顯式opt-in；原Qty/children和Update不變。
# Private visual branch; not OBS, 0x85 is a read-only query. Only this experimental build explicitly opts in; the original Qty/children and Update are unchanged.
if RULE_RUN and PRIVATE_GATES:
    off, word = 0x3e443c, 0x3969c268
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))  # w8查詢結果僅用於cbz；兩路恢復原x8。 | The w8 query result is used only for cbz; both paths restore the original x8.
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    # cbz w8, nonmatch；不改NZCV。match恢復寄存器後保children。
    # cbz w8, nonmatch; NZCV is not changed. On match, restore the registers and then preserve children.
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), 0x3e4460))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), 0x3e4440))
    gates.append((ret, 'svc', 0x85, off))
    put_branch(off, start); stat('private_skill_render')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off,start)).hex(),
                 'private confirmed six HUD only: own image/text skip ->4460; unknown original ->4440; svc85 full800', '僅宿主'))
elif RULE_RUN:
    patch.extend(bytes(0x80))
    stat('private_gates_reserved')

obs = list(OBS.get(BUILD_ID, []))
if RULE_RUN and PRIVATE_GATES:
    obs.append((0x43ecc8, 6, "b8686953", "private touch-run-v4 PRE-ldr; full800 host ABI"))
    obs.append((0x1237c60, 8, "d10143ff", "ui world anchor: camera world->screen entry (camera, point, out); host swaps x0 to a copy whose projection is shifted by the UI rect y (iPad UI kept 16:9 inside a taller screen), game camera untouched"))
    # 面板開著時凍結世界（宿主 diag.c freeze_tick，缺省關，DM_PANEL_FREEZE=1 開）。四個入口門，都只換入口參數。
    # Freeze the world while a panel is open (host diag.c freeze_tick, off by default, DM_PANEL_FREEZE=1 turns it on). Four entry gates, all of which only swap entry arguments.
    obs.append((0x5eb2c0, 10, "a9bc5ff8", "server world-sim entry (game G): while frozen the host swaps x0 to a stand-in whose event queue is empty, so no unit / timer callback runs this tick; the real queue is left untouched"))
    obs.append((0x4b7230, 11, "d10183ff", "client per-unit tick update entry (unit): while frozen the host swaps x0 to a stand-in unit carrying only the skip flag ([+0x124] bit 23), so the function takes its own early-out to the epilogue; all six call sites are inside the client tick simulation 0x461b80 / 0x461e60"))
    obs.append((0x4b74d0, 12, "f81e0ff3", "client per-unit per-frame update entry (unit): drives model animation / interpolation from the tick fraction; while frozen the host swaps x0 to a stand-in unit (type 9, no render component) so both the callee 0x2a18d0 and this function take their own early-outs"))
    obs.append((0x2c3c90, 13, "f81c0ff7", "render per-frame entry, called right after the main loop writes the world frame step [0x20cebb8] and world time [0x3114208]: while frozen the host rewrites the step to 0 and the world time to its value at freeze, the same thing the game's own pause path does at 0x2c1574"))
    obs.append((0xadc434, 14, "d10143ff", "TextBoxWidget text-changed entry (widget): reads the raw text at +0x88 and rebuilds the display; host rewrites +0x88 for strings our item-notes generator annotated with colour codes (TextBoxWidget draws U+E07E as '*'), exact-match table only, everything else untouched"))
    obs.append((0x442bc0, 15, "a9ba6ffc", "client event-message entry (x0 = packet; byte 1 = event subtype, jump table 0x1da6752): subtype 0x17 = offline difficulty scale changed -> 0x20f91c chat line + system notification; host rewrites byte 1 past the table (default branch, nothing shown) only for 0x17 and only in a rift game (dm_rift.c); otherwise untouched. orig stp x28, x27, [sp, #-0x60]!"))
    obs.append((0x634000, 16, "d10143ff", "server level change by level number (game, player, level, kind), used by waypoints / portals / quest moves: in a rift game, when the player leaves a rift map for a level that is neither town nor a rift map, the host swaps w2 to the rift's next map so the destination is replaced inside the single native transition"))
    obs.append((0x711040, 17, "d101c3ff", "server quest event 'monster killed' entry (game, monster, attacker), the only entry (callers 0x7d8550 kill, 0x7ab8dc); in a rift game, for a monster that died on a rift map, the host swaps x1 to a stand-in whose +0xc0 is null so the function takes its own early return (0x711064): bosses in the rift are combat only, no quest state / quest notifications"))
    obs.append((0x3d84a0, 18, "d10143ff", "NotificationPanel message handler entry (panel, message): on InputMessage / ControllerActionBegin with action id 9 (the menu key) the native handler re-sends the notice callback and marks the message bus consumed, so the first menu press only dismisses the notice. In touch mode the host swaps x1 to a copy of the message whose action id is not 9: the notice stays until it times out and the menu key opens the menu; controller mode keeps the native behaviour"))
    obs.append((0x2f7664, 7, "6dbb23e9", "touch target rank: candidate compare entry (closure, unit, kind, s0=score); host raises Boss/elite score, native eligibility unchanged"))
for off, oid, ow, why in obs:
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'觀測門原指令不符 {off:#x}：{w:08x}'
    # 只許函數入口：dm_gate 只回寫 x0–x8 / x30，宿主 C 代碼可能改 x9–x17；入口處這些按 AAPCS64 不承載值（參數只在 x0–x7 / x8）
    # Function entries only: dm_gate writes back only x0–x8 / x30, and host C code may modify x9–x17; at entry these carry no values under AAPCS64 (arguments are only in x0–x7 / x8)
    assert off in set(starts) or (oid == 6 and off == 0x43ecc8), f'private obs point not approved {off:#x}'
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'觀測門與改寫點衝突 {off:#x}'
    put_branch(off, site_obs(off, oid, w)); stat('obs')

# 秘境出口改道（r36 起）：分支門 0x89，掛在「按出口磚的連接換關」main+0x7cc1e0(遊戲, 玩家, 磚) 的入口。它的四個調用者（點出口磚 0x6dbea0、
# 入口物件 0x6aeaa0 / 0x6b0500、任務 0x7314c0）都在各自的判定通過之後才調它；目的地不是關卡號而是磚的對端房間（同一幕已生成的地圖裡），換不成別的幕。
# 宿主在秘境局裡把 w2 / w3 換成（秘境下一張圖, 11）並讓 x8≠0 → 門塊直接跳去「按關卡號換關」0x634000，同一個調用者、只發生一次換關；其餘情形執行原指令回 0x7cc1e4。
# 門塊與 0x85 分支門逐字同構（structck ⑪ 按 hle.h 登記表核）；與觀測門 15 / 16 一樣只在帶宿主私有門的產物裡（touch）
# Rift exit rerouting (since r36): branch gate 0x89, hooked at the entry of "change level by the exit tile's connection" main+0x7cc1e0(game, player, tile). Its four callers (clicking an exit tile 0x6dbea0,
# entrance objects 0x6aeaa0 / 0x6b0500, quest 0x7314c0) all call it only after their own checks have passed; the destination is not a level id but the tile's peer room (in the already generated map of the same act), so it cannot be switched to another act.
# In a rift run the host replaces w2 / w3 with (the rift's next map, 11) and makes x8≠0 → the gate block jumps straight to "change level by level id" 0x634000, same caller, exactly one level change; in all other cases the original instruction is executed and control returns to 0x7cc1e4.
# The gate block is word-for-word isomorphic with the 0x85 branch gate (structck ⑪ checks it against the hle.h registry); like observation gates 15 / 16 it exists only in build outputs with host-private gates (touch)
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x7cc1e0, 0xd10183ff, 0x634000
    assert struct.unpack_from('<I', IMG, off)[0] == word and off in set(starts) and tgt in set(starts)
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x89, off))
    put_branch(off, start); stat('rift_exit')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'rift exit redirect: tile-link level change entry -> host swaps (level, kind) and x8!=0 takes the branch to level-number change 0x634000; otherwise original ->0x7cc1e4; svc89', '僅宿主'))

# 秘境裡存檔時的當前幕（r38 起）：分支門 0x8c，掛在原版寫存檔頭函數 0x6fa3e0 裡「取難度」那條 main+0x6fa9d0（ldrb w10, [x21, #0x104]）。
# 它的上一條 0x6fa9cc 剛把「玩家當前在第幾幕」讀進 w9（ldrb w9, [x24, #0x2e8]），下面 0x6fa9e4 / 0x6fa9e8 寫成難度字節 0x80 | 幕號（.d2s 0x98–0x9a），
# 下次進局落哪個城由它定。宿主在人處於秘境圖裡時把 w9 換成 4（第五幕 → 重進落哈洛加斯），只動寫進存檔的這一個值，運行中的幕號不碰。
# 用分支門而不是「替換一條指令」：structck 只認「bl 調用點」「入口」「分支門」三種登記形態。宿主永遠讓 x8 = 0（不走跳轉那一路），
# 門塊恢復 x8、執行原指令、回 0x6fa9d4；跳轉目標填回點本身只為滿足模板（走不到）。門塊與 0x85 / 0x89 逐字同構
# Current act written to the save while in a Rift (since r38): branch gate 0x8c, hooked at the "read difficulty" instruction main+0x6fa9d0 (ldrb w10,
# [x21, #0x104]) inside the original save-header writer 0x6fa3e0.
# The preceding instruction 0x6fa9cc has just loaded "which act the player is currently in" into w9 (ldrb w9, [x24, #0x2e8]); below, 0x6fa9e4 / 0x6fa9e8
# write it as the difficulty byte 0x80 | act number (.d2s 0x98–0x9a),
# which decides which town the next session starts in. While the player is in a Rift map the host replaces w9 with 4 (Act 5 → re-enter in Harrogath);
# only this one value written to the save is changed, the running act number is untouched.
# A branch gate is used rather than "replace one instruction": structck only recognizes three registered forms, "bl call site", "entry" and "branch
# gate". The host always sets x8 = 0 (the jump path is never taken);
# the gate block restores x8, executes the original instruction and returns to 0x6fa9d4; the jump target is set to the return point itself only to
# satisfy the template (never reached). The gate block is word-for-word identical in structure to 0x85 / 0x89
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x6fa9d0, 0x394412aa, 0x6fa9d4   # 函數中間的一條指令（同 0x85 那扇門），不要求是函數入口；原指令不依賴 pc | An instruction in the middle of a function (same as gate 0x85); it need not be a function entry; the original instruction does not depend on pc
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x8c, off))
    put_branch(off, start); stat('rift_save_act')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'rift save act: save-header writer, right after it loads the current act into w9 -> host swaps w9 to 4 (act V) while the player is in a rift map; x8 stays 0, original insn runs ->0x6fa9d4; svc8c', '僅宿主'))
# 傭兵加裝備位（r39 起）：分支門 0x8d，掛在原版通用放置校驗 main+0x6493a8 的怪物主人分支裡 main+0x64a34c（mov w9, w25）。
# 這一段（0x64a314–0x64a354）對怪物類主人只放行兩類物品：0x574bc0(物品, 3 護甲)、0x574bc0(物品, 0x25 頭盔)，都不是就走武器那條（按 monstats 的左右手類型），再不中就拒。
# 門掛在「是不是頭盔」返回之後、cbnz w0 之前：宿主在「主人是玩家的傭兵 + 秘境最高通關 ≥ 10 層 + 物品是腰帶 / 鞋 / 手套 / 戒指 / 項鍊」時把 w0 改成 1，
# 走頭盔同一條放行路徑（後面的部位格子、部位空不空等通用檢查照常）。宿主永遠讓 x8 = 0；門塊與 0x85 / 0x89 / 0x8c 逐字同構
# Extra mercenary equipment slots (since r39): branch gate 0x8d, hooked in the monster-owner branch of the original game's generic placement check main+0x6493a8 at main+0x64a34c (mov w9, w25).
# This section (0x64a314–0x64a354) lets through only two kinds of items for monster-type owners: 0x574bc0(item, 3 armor) and 0x574bc0(item, 0x25 helm); if neither, it takes the weapon path (by the left / right hand types in monstats), and if that fails too it rejects.
# The gate is hooked after the "is it a helm" call returns and before cbnz w0: when "the owner is the player's mercenary + highest rift level cleared ≥ 10 + the item is a belt / boots / gloves / ring / amulet", the host changes w0 to 1,
# taking the same pass path as the helm (the generic checks that follow, such as the body location cell and whether the location is empty, run as usual). The host always sets x8 = 0; the gate block is word-for-word isomorphic with 0x85 / 0x89 / 0x8c
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x64a34c, 0x2a1903e9, 0x64a350   # 原指令不依賴 pc | The original instruction does not depend on pc
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x8d, off))
    put_branch(off, start); stat('merc_slot')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'merc slots: placement check, monster-owner branch right after the helm type test -> host sets w0=1 for ring/amulet/boots/gloves/belt on a hireling (rift best >= 10 for new equips; always on load); x8 stays 0, original insn runs ->0x64a350; svc8d', '僅宿主'))
# 傭兵加裝備位的客戶端那一半（r39 同代）：分支門 0x8e，掛在客戶端「傭兵能不能穿這件」函數 main+0x3bea44(傭兵, 物品) 裡 main+0x3beb58（mov x0, x19）。
# 這個函數和服務端那段同一個規則：0x574bc0(物品, 3 護甲)、0x574bc0(物品, 0x25 頭盔) 中一個就返回 1，都不是走武器那條。物品格控件拿它的結果畫「不可放置」的紅色、
# 決定鬆手時放不放——不改它，已解鎖的新格拖著對的物品懸停也是紅的、原版放下不成（原先靠觸摸層兜底直接調 0x3bed60）。
# 門掛在「mov w1, #0x25」之後、這次類型判斷的 bl 之前：宿主在「秘境最高通關 ≥ 10 層 + 物品是腰帶 / 鞋 / 手套 / 戒指 / 項鍊」時把 w1 換成該物品自己的類型號，
# 類型判斷自然成立、走頭盔同一條返回 1 的路。宿主永遠讓 x8 = 0；門塊與 0x8d 逐字同構
# Client half of the extra mercenary equipment slots (same generation as r39): branch gate 0x8e, hooked inside the client's "can the mercenary wear this" function main+0x3bea44(mercenary, item) at main+0x3beb58 (mov x0, x19).
# This function follows the same rule as the server-side section: if either 0x574bc0(item, 3 armor) or 0x574bc0(item, 0x25 helm) matches it returns 1, and if neither does it takes the weapon path. The item cell widget uses its result to draw the "cannot place" red and
# to decide whether to place on release -- without changing it, an unlocked new cell still shows red when the right item is dragged over it, and the original game's drop fails (previously the touch layer's fallback called 0x3bed60 directly).
# The gate is hooked after "mov w1, #0x25" and before the bl of this type check: when "highest rift level cleared ≥ 10 + the item is a belt / boots / gloves / ring / amulet", the host replaces w1 with the item's own type number,
# so the type check passes naturally and takes the same return-1 path as the helm. The host always sets x8 = 0; the gate block is word-for-word isomorphic with 0x8d
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x3beb58, 0xaa1303e0, 0x3beb5c   # 原指令不依賴 pc | The original instruction does not depend on pc
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x8e, off))
    put_branch(off, start); stat('merc_slot_client')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'merc slots (client): hireling can-equip check, between "mov w1,#0x25" and the type test -> host swaps w1 to the item\'s own type for ring/amulet/boots/gloves/belt (rift best >= 10); x8 stays 0, original insn runs ->0x3beb5c; svc8e', '僅宿主'))

# ---- 參數位（僅宿主）：{NSO 構建號: [(偏移 A, 原字 A, 偏移 B, 原字 B, 寄存器, 鍵, 理由)]}。
# 原處兩條造常數指令改成第一條跳補丁區參數跳板、第二條 nop；跳板間接取鍵頁值後跳回第一條之後（A+4）：A、B 不相鄰時中間的原指令照常執行
# （曾跳回 B+4，運行模式通知處理 0xe10d5c/0xe10d60 越過 adrp x19.. 與另一跳板，切掌機即崩；structck ⑩ 核跳板出口）。
# A、B 之間不得讀寫 xR（離線斷言：中間指令不含 xR）。宿主按前端給值，Mac 給原常數 → 行為不變 ----
# ---- Parameter slots (host only): {NSO build ID: [(offset A, original word A, offset B, original word B, register, key, reason)]}.
# The two constant-building instructions at the original site are changed so that the first jumps to a parameter trampoline in the patch area and the second is a nop; the trampoline fetches the key page value indirectly and jumps back to just after the first (A+4): when A and B are not adjacent, the original instructions in between execute as usual
# (it used to jump back to B+4; in the operation-mode notification handler 0xe10d5c/0xe10d60 that skipped adrp x19.. and another trampoline, and switching to handheld crashed immediately; structck ⑩ checks the trampoline exit).
# xR must not be read or written between A and B (offline assertion: the instructions in between do not contain xR). The host supplies values according to the front end, Mac supplies the original constants → behavior unchanged ----
PARAMS = {'db0507f36b84200ac05f75d76ed5026f': [
    (0xe12d8c, 'd280c813', 0xe12d90, 'f2c07093', 19, 'KRES1', '啓動初始化：主機模式輸出分辨率 1600x900（{u32 寬, u32 高} 打包）'),
    (0xe12e1c, 'd280a008', 0xe12e20, 'f2c05a08', 8, 'KRES0', '啓動初始化：掌機模式輸出分辨率 1280x720'),
    (0xe10d60, 'd280c818', 0xe10d80, 'f2c07098', 24, 'KRES1', '運行模式變化通知（0x1f）處理：主機模式 1600x900'),
    (0xe10d5c, 'd280a016', 0xe10d74, 'f2c05a16', 22, 'KRES0', '運行模式變化通知（0x1f）處理：掌機模式 1280x720'),
    # 寬高比上限 19:9（float 0x40071c72）：寬於 19:9 的屏幕兩側留黑柱。宿主給 max(19/9, 輸出寬 / 高) 的浮點位模式（低 32 位）；
    # 原版輸出 16:9（1600x900 / 1280x720）⇒ 仍為 19/9，行為不變；iPhone 1952x900（2.17:1）⇒ 2.169，黑柱寬 0
    # Aspect ratio cap 19:9 (float 0x40071c72): screens wider than 19:9 get black bars on both sides. The host supplies the float bit pattern (low 32 bits) of max(19/9, output width / height);
    # the original game outputs 16:9 (1600x900 / 1280x720) ⇒ still 19/9, behavior unchanged; iPhone 1952x900 (2.17:1) ⇒ 2.169, black bar width 0
    (0x1663ee8, '52838e49', 0x1663eec, '72a800e9', 9, 'KASPECTUI', '界面常量緩衝 0x15 號槽 [屏寬, 屏高, 寬高比上限]：界面精靈著色器按它在上限以外畫兩側黑柱（帶約 20–30 像素向內的柔邊，宿主另加餘量把柔邊推出屏幕）'),
    (0xb152bc, '52838e48', 0xb152c0, '72a800e8', 8, 'KASPECT', '新畫面模式視口：邊距 = (屏寬 − 屏高 × 上限) / 2，經 0x85bc90 設居中視口'),
    (0x23e888, '52838e48', 0x23e88c, '72a800e8', 8, 'KASPECT', '新畫面模式坐標鉗制（0x23e760）：x 夾進 [邊距, 邊距 + 屏高 × 上限]，寬屏兩側的指針 / 觸摸不再被夾回中間'),
]}
params = PARAMS.get(BUILD_ID, [])
for oa, wa, ob_, wb, r, key, why in params:
    for o, w0 in ((oa, wa), (ob_, wb)):
        w = struct.unpack_from('<I', IMG, o)[0]
        assert f'{w:08x}' == w0, f'參數位原指令不符 {o:#x}：{w:08x}'
        assert o not in kinds and not any(lo <= o < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'參數位與改寫點衝突 {o:#x}'
    for o in range(oa + 4, ob_, 4):
        ins = dis(struct.unpack_from('<I', IMG, o)[0], o)
        assert r not in regs_in(ins), f'參數位中間用到 x{r}：{o:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    tmp = 16 if r != 16 else 17
    emit(STUR(tmp, 31, -16)); emit(MRS_TPIDRRO(r)); emit_slot_addr(r, tmp, key); emit(LDRX(r, r)); emit(LDUR(tmp, 31, -16)); emit(B(here(), oa + 4))
    struct.pack_into('<I', IMG, oa, B(oa, start)); struct.pack_into('<I', IMG, ob_, NOP); stat('param')

# 既有三塊原生規則跳板均只使用後繼覆蓋的死臨時w8，不改w0/stat返回、sp或NZCV。
# 6c6290位於原mode3門後；前方Blaze/引用計數與數據失效6c62dc分支、後方狀態更新不動。
# The three existing native rule trampolines all use only the dead temporary w8 that is overwritten afterwards, and do not change the w0/stat return, sp or NZCV.
# 6c6290 is after the original mode3 gate; the Blaze / reference count and data invalidation 6c62dc branches before it and the state update after it are untouched.
rule_blocks = []
if RULE_RUN:
    def run_branch(off, old, body, why):
        assert struct.unpack_from('<I', IMG, off)[0] == old, f'Run跳板原字不符 {off:#x}'
        start = here()
        for word in body(start): emit(word)
        new = B(off, start)
        struct.pack_into('<I', IMG, off, new)
        cuts.append((off, struct.pack('<I', old).hex(), struct.pack('<I', new).hex(), why, '全'))
        rule_blocks.append((start, bytes(patch[start - PATCH_OFF:]).hex(), why))
    def cbz(at, target, r, nonzero=False):
        delta = target-at; assert delta%4==0 and -(1<<20)<=delta<(1<<20)
        return (0x35000000 if nonzero else 0x34000000) | (((delta>>2)&0x7ffff)<<5) | r
    run_branch(0x6c6648, 0x34000060, lambda a: [
        0xb9400268, cbz(a+4,a+12,8,True), B(a+8,0x6c664c),
        cbz(a+12,a+20,0), B(a+16,0x6c664c), B(a+20,0x6c6654)],
        'touch-run-v4：Run選擇僅player0忽略精力門，非player按原stat返回；w8後繼覆蓋')
    run_branch(0x6c6290, 0x710006df, lambda a: [
        0xb9400268, cbz(a+4,a+12,8,True), B(a+8,0x6c62ec), 0x710006df, B(a+16,0x6c6294)],
        'touch-run-v4：原合法Run成本段僅player0跳stat10扣除/耗盡降走；非player保持原cmp')


    # client4abc60原mode3/當前受控unit門之後；實際GetStat已返回，保W0，只type0取消STA自動Stop。
    # After the original mode3 / currently controlled unit gate at client4abc60; the actual GetStat has already returned, W0 is preserved, and only for type0 is the STA auto Stop cancelled.
    run_branch(0x4ac368, 0x35000460, lambda a: [
        0xb9400268, cbz(a+4,a+12,8), B(a+8,a+16), B(a+12,0x4ac3f4),
        cbz(a+16,a+24,0), B(a+20,0x4ac3f4), B(a+24,0x4ac36c)],
        'touch-run-v4：client當前受控Run3僅player0跳STA零值自動Stop；原getter值與真實松桿保留，非player保原門')


    # 原unit null/type5門之後、w8仍為原unitType；只規範player請求2，原其它動作分派/資格完整執行。
    # After the original unit null/type5 gate, with w8 still the original unitType; only player request 2 is normalized, and the other original action dispatch / eligibility checks run in full.
    run_branch(0x6c6580, 0x51000849, lambda a: [
        cbz(a,a+16,8,True), 0x51000849, cbz(a+8,a+16,9,True),
        0x52800062, 0x51000849, B(a+20,0x6c6584)],
        'touch-run-v4：原通用合法移動僅player0請求Walk2規範為Run3，非player與其它請求保原，w8/原資格不改')

os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
import hashlib
with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:
    f.write(hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).hexdigest() + '  patch_nso.py\n')
if PRIVATE_AIM:
    with open(P('aim.tsv'), 'w') as f:
        f.write('# RULESET\tskill-aim-v1-private\n# ORIGINAL_CALL\t0x2faf04\t97fee7fd\n# ORIGINAL_CALLEE\t0x2b4ef8\n# SKILL_CALLER\t0x2b5b80\n# SVC\t0x86\n# INPUT\tactual GetNpad sampled right; move axis unchanged\n')
        f.write('# IMAGE_SHA256\t'+hashlib.sha256(open(P('patched.bin'),'rb').read()).hexdigest()+'\n')
if RULE_RUN:
    with open(P('rules.tsv'), 'w') as f:
        f.write(f'# RULESET\t{RULESET}\n# BUILD_ID\t{BUILD_ID}\n# ORIGINAL_SHA256\t{RULE_ORIG_SHA}\n# IMAGE_SHA256\t{hashlib.sha256(open(P("patched.bin"), "rb").read()).hexdigest()}\n')
        for off, raw, why in rule_blocks: f.write(f'{off:#x}\t{raw}\t{hashlib.sha256(bytes.fromhex(raw)).hexdigest()}\t{why}\n')
elif os.path.exists(P('rules.tsv')):
    os.unlink(P('rules.tsv'))  # 重出native時清本模塊舊規則登記，不讓舊manifest混入原資源域 | When rebuilding native, clear this module's old rule registration so the old manifest does not leak into the original-resource domain
with open(P('fixups.tsv'), 'w') as f:
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')   # 宿主據此認構建：寫死地址的功能只在它們所屬的構建上開 | the host identifies the build by it: features with hard-coded addresses only run on the build they belong to
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
    for a, tgt in tabjumps.items(): f.write(f'{a:#x}\tT\t' + ' '.join(f'{t:#x}' for t in tgt) + '\n')   # 偏移表跳轉與全部目標 | offset-table jump and all its targets
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why, scope in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{scope}\t{why}\n')   # 列：偏移 原字節 新字節 作用域 理由 | Columns: offset, original bytes, new bytes, scope, reason
with open(P('options.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for name, off, ob, nb, why in options: f.write(f'{name}\t{off:#x}\t{ob}\t{nb}\t{why}\n')   # 列：選項 偏移 原字節 新字節 理由 | Columns: option, offset, original bytes, new bytes, reason
with open(P('params.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for oa, wa, ob_, wb, r, key, why in params: f.write(f'{oa:#x}\t{ob_:#x}\tx{r}\t{key}\t{why}\n')
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch), 'PATCH_OFF', hex(PATCH_OFF), 'slot_refs', len(fix), 'gates', len(gates))
# 啓動塊清單隨 main 出：取倉庫版本化數據 dm/data/bootlist_<main 構建號>.tsv 原樣拷進產物目錄並寫 sha256（清單入庫、不每版重錄；
# 記錄本身有競態——同產物三次記錄塊集合 5292 / 5114 / 5281、發起線程亦有不同——重錄只在構建號變化或主動改進時做，提交附 mini A/B）
# The boot block list ships with main: take the repository's versioned data dm/data/bootlist_<main build ID>.tsv, copy it as is into the build output directory and write its sha256 (the list is checked in, not re-recorded for every version;
# the recording itself is racy -- three recordings of the same build output gave block sets of 5292 / 5114 / 5281, and the initiating threads also differed -- re-record only when the build ID changes or for a deliberate improvement, and attach an A/B from the second Mac to the commit)
if NAME == 'main':
    import hashlib, shutil
    BL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', f'bootlist_{BUILD_ID}.tsv')
    if not os.path.isfile(BL):   # 公開倉庫不帶這份清單（它每行帶原版資源塊的開頭字節）：沒有就不預取，啓動時少一步提前解碼，別的不受影響 | the public repository does not ship this list (each line carries the first bytes of an original resource block): without it there is no prefetch, so launch skips one ahead-of-time decode step and nothing else is affected
        print('bootlist 沒有啓動塊清單，跳過（不預取）。要用的話自己錄一份：dm/tools/tact_bootlist.py')
    else:
        hd = open(BL, encoding='utf-8').readline().split()
        if hd[-1:] != [BUILD_ID]: sys.exit(f'【紅】{BL} 頭行構建號 {hd[-1:]} 與 main {BUILD_ID} 不符')
        shutil.copyfile(BL, os.path.join(outdir, 'bootlist.tsv'))
        sha = hashlib.sha256(open(BL, 'rb').read()).hexdigest()
        open(os.path.join(outdir, 'bootlist.sha256'), 'w').write(f'{sha}  bootlist.tsv\n')
        print('bootlist', os.path.relpath(BL), sha[:12])
