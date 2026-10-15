# 過場空播靜態核對（hle_movie.c 頭注釋裡的遊戲側協議）：在原映像上逐條核關鍵指令與虛表槽名，任一不符即紅。
# 用法：movie_cut.py <main.orig.bin> <subsdk0.orig.bin>      （.orig.bin 由 dm/tools/nso.py 從 ExeFS 生成）
# Static check of cutscene no-op playback (the game-side protocol in the header comment of hle_movie.c): check the key instructions and vtable slot names one by one on the original image; any mismatch is a failure.
# Usage: movie_cut.py <main.orig.bin> <subsdk0.orig.bin>      (.orig.bin is generated from ExeFS by dm/tools/nso.py)
import struct, sys
from capstone import *
main, sub = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
def ins(a): i = next(md.disasm(main[a:a + 4], a)); return f'{i.mnemonic} {i.op_str}'

# (偏移, 期望反匯編, 這條在鏈裡的意義)——過場「按真實時長空播」所依賴的遊戲側協議（hle_movie.c 頭注釋）
# (offset, expected disassembly, meaning of this instruction in the chain) -- the game-side protocol that cutscene "no-op playback for the real duration" (plays nothing but lasts the real duration) depends on (header comment of hle_movie.c)
CHAIN = [
    (0x898ed0, 'mov w1, #0x10', 'Extractor 對象 0x10 字節：剪口把狀態指針存在第二個字'),
    (0x899960, 'mov w1, #0x10', 'Decoder 對象 0x10 字節：同上'),
    (0x899780, 'ldr x8, [x8, #0x20]', 'Setup 調 Extractor 槽4 GetTrackCount'),
    (0x89979c, 'b.lt #0x899928', '軌數 < 1 跳過選軌（剪口給 0 軌）'),
    (0x898f70, 'stp x24, x23, [x19, #0x1b8]', 'DecoderEvents[0..1] = {P+0x18, P+0x40}'),
    (0x899044, 'mov w8, #1', 'P+0x18 的等待項種類 1 = 輸入可用'),
    (0x8990b8, 'mov w8, #2', 'P+0x40 的等待項種類 2 = 輸出可用'),
    (0x8999e4, 'ldr x8, [x8, #0x28]', 'Setup 調 Decoder 槽5 Start'),
    (0x899f54, 'cmp w21, #0', 'Open 返回 (Start 狀態 == 0)：剪口 Start 成功 ⇒ 在播'),
    (0x899f5c, 'cset w0, eq', ''),
    (0xbbfe40, 'bl #0x898230', '每幀更新調播放器更新'),
    (0x89832c, 'ldrb w8, [x8, #0x489]', '幀隊列空時返回 !P[+0x489]（播完標誌）…'),
    (0x898334, 'cset w0, eq', '… 未播完即視為在播'),
    (0x8992e0, 'cmp w21, #1', '解碼線程：種類 1 走輸入路徑'),
    (0x8992e8, 'cmp w21, #2', '解碼線程：種類 2 走輸出路徑'),
    (0x899508, 'ldr x8, [x8, #0x48]', '輸入：Decoder 槽9 AcquireInputBufferIndex'),
    (0x899534, 'ldr x8, [x8, #0x50]', '輸入：Decoder 槽10 GetInputBuffer'),
    (0x8995a8, 'ldr x8, [x8, #0x50]', '輸入：Extractor 槽10 Read'),
    (0x8995b8, 'cmp w23, #2, lsl #12', 'Read 返回 0x2000 = 流尾 …'),
    (0x8995c0, 'ldrb w8, [x19, #0x12]', '… 循環標誌 …'),
    (0x8995d4, 'ldr x8, [x8, #0x40]', '… 循環則 Extractor 槽8 SeekTo（剪口重掛定時）'),
    (0x899620, 'mov w26, #4', '… 否則送入標誌 4（流尾）'),
    (0x899634, 'ldr x8, [x8, #0x60]', '輸入：Extractor 槽12 GetSampleTime'),
    (0x899670, 'ldr x8, [x8, #0x58]', '輸入：Decoder 槽11 SendInputBufferForDecode'),
    (0x89968c, 'ldr x8, [x8, #0x58]', '輸入：Extractor 槽11 Advance'),
    (0x89932c, 'ldr x8, [x8, #0x60]', '輸出：Decoder 槽12 AcquireOutputBufferIndex'),
    (0x899338, 'tst w0, #0xffffdfff', '狀態 0 / 0x2000 才入幀隊列（剪口無輸出回 0x1008）'),
    (0x899cb4, 'cmp x9, #2', '丟幀只在隊列 ≥2 時（單個流尾項不會被丟）'),
    (0x89a650, 'ldr x8, [x8, #0x90]', '取幀：Decoder 槽18 GetOutputBufferFormat（剪口回非 0）'),
    (0x89a65c, 'cmp w27, #4', '標誌 4 …'),
    (0x89a670, 'strb w8, [x25, #0x489]', '… 置播完 ⇒ 遊戲走正常播完路徑'),
    (0x899fdc, 'ldr x8, [x8, #0x30]', '收尾調 Decoder 槽6 Stop（剪口撤銷定時）'),
    (0x899ff0, 'ldr x8, [x8, #8]', '收尾調 Decoder 槽1 刪除析構'),
    (0x89a004, 'ldr x8, [x8, #8]', '收尾調 Extractor 槽1 刪除析構'),
]
bad = 0
for a, want, why in CHAIN:
    got = ins(a); ok = got == want; bad += not ok
    print(f'{"✓" if ok else "✗"} {a:#x}  {got:28s} {"" if ok else "期望 " + want}  {why}')

# 虛表槽名（subsdk0 動態符號 + RELATIVE 重定位）：剪口虛表按這些槽號排
# Vtable slot names (subsdk0 dynamic symbols + RELATIVE relocations): the cut vtable is ordered by these slot numbers
m0 = struct.unpack_from('<I', sub, 4)[0]; dyn = m0 + struct.unpack_from('<i', sub, m0 + 4)[0]
D = {}; o = dyn
while True:
    t, v = struct.unpack_from('<qQ', sub, o); o += 16
    if t == 0: break
    D.setdefault(t, v)
rel = {}
for k in range(D[8] // 24):
    off, info, add = struct.unpack_from('<QQq', sub, D[7] + 24 * k)
    rel[off] = add if info & 0xffffffff == 1027 else ('sym', info >> 32)   # RELATIVE 給地址；ABS64 / GLOB_DAT 給符號號 | RELATIVE gives an address; ABS64 / GLOB_DAT give a symbol number
names, sym, symn = {}, {}, {}
for k in range(1, (D[5] - D[6]) // 24):
    nm, info, oth, shn, val, sz = struct.unpack_from('<IBBHQQ', sub, D[6] + 24 * k)
    s = sub[D[5] + nm:sub.index(b'\0', D[5] + nm)].decode(); symn[k] = s
    if shn and val: names.setdefault(val, s); sym[s] = val
def slot_name(r): return symn.get(r[1], '?') if isinstance(r, tuple) else names.get(r, '?')
VT = [('_ZTVN5movie9ExtractorE', 4, '_ZN5movie9Extractor13GetTrackCountEPi'),
      ('_ZTVN5movie9ExtractorE', 15, '_ZN5movie9Extractor13SetDataSourceEPNS_18ClientStreamReaderEPKc'),
      ('_ZTVN5movie7DecoderE', 2, '_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE'),
      ('_ZTVN5movie7DecoderE', 3, '_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE'),
      ('_ZTVN5movie7DecoderE', 5, '_ZN5movie7Decoder5StartEv'),
      ('_ZTVN5movie7DecoderE', 6, '_ZN5movie7Decoder4StopEv'),
      ('_ZTVN5movie9ExtractorE', 8, '_ZN5movie9Extractor6SeekToEl'),
      ('_ZTVN5movie9ExtractorE', 10, '_ZN5movie9Extractor4ReadEPNS_6BufferE'),
      ('_ZTVN5movie9ExtractorE', 11, '_ZN5movie9Extractor7AdvanceEv'),
      ('_ZTVN5movie9ExtractorE', 12, '_ZN5movie9Extractor13GetSampleTimeEPl'),
      ('_ZTVN5movie7DecoderE', 9, '_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_'),
      ('_ZTVN5movie7DecoderE', 10, '_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE'),
      ('_ZTVN5movie7DecoderE', 11, '_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj'),
      ('_ZTVN5movie7DecoderE', 12, '_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_'),
      ('_ZTVN5movie7DecoderE', 15, '_ZN5movie7Decoder24ReleaseOutputBufferIndexEi'),
      ('_ZTVN5movie7DecoderE', 18, '_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE')]
for vt, slot, want in VT:
    got = slot_name(rel.get(sym[vt] + 16 + 8 * slot, 0)); ok = got == want; bad += not ok
    print(f'{"✓" if ok else "✗"} {vt} 槽{slot}  {got}{"" if ok else "  期望 " + want}')
n = len(CHAIN) + len(VT)
print(f'{"全過" if not bad else "紅"}：{n - bad}/{n}')
sys.exit(1 if bad else 0)
