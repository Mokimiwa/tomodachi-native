# 生成內嵌匯編：把改寫後的模塊映像編進可執行文件，代碼只進 __TEXT 一個執行段（蘋果 ITMS-90999：只許一個帶執行權限的段）。
#   __TEXT,__dm<名>x0   原 text：[0, 圓整16K(TEXT_END))（含 ro 首頁的一部分，只讀）
#   __DM<名>W,__img     RW 段：[圓整16K(TEXT_END), PATCH_OFF)，ro 餘部 + data + bss（重定位只落這裡）
#   __TEXT,__dm<名>x1   補丁區：[PATCH_OFF, 圓整16K(文件尾))
# 三塊在可執行文件裡不相鄰；運行時由裝載器（loader.c iOS 分支）保留一塊連續地址，按原相對位置 vm_remap 回去（代碼頁共享已簽名頁，W 寫時複製），
# 所以運行時佈局與原映像逐字節相同：原代碼與 data 之間的 PC 相對尋址、補丁區的相對跳轉、宿主按「基址 + 原偏移」的引用都不用動。
# 用法：gen_embed.py <產物目錄> <輸出 .S> <模塊名...>；鏈接參數打印到 stdout（W 段的 -segprot）
# Generate the embedded assembly: compile the rewritten module image into the executable, with code going into the single executable segment __TEXT only (Apple ITMS-90999: only one segment with execute permission is allowed).
#   __TEXT,__dm<name>x0   original text: [0, round16K(TEXT_END)) (includes part of the first ro page, read-only)
#   __DM<name>W,__img     RW segment: [round16K(TEXT_END), PATCH_OFF), rest of ro + data + bss (relocations land only here)
#   __TEXT,__dm<name>x1   patch area: [PATCH_OFF, round16K(end of file))
# The three pieces are not adjacent in the executable; at run time the loader (loader.c iOS branch) reserves one contiguous address range and vm_remaps them back at their original relative positions (code pages share the signed pages, W is copy-on-write),
# so the run-time layout is byte-identical to the original image: PC-relative addressing between original code and data, relative jumps in the patch area, and the host's "base + original offset" references all need no change.
# Usage: gen_embed.py <build output dir> <output .S> <module name...>; link flags are printed to stdout (-segprot for the W segment)
import os, sys
out, dst, names = sys.argv[1], sys.argv[2], sys.argv[3:]
PG = 0x4000
def r16(x): return (x + PG - 1) & ~(PG - 1)
asm = ['// 由 dm/ios/tools/gen_embed.py 生成，勿手改']
flags = []
for n in names:
    hdr = dict(l[2:].split() for l in open(f'{out}/{n}.fixups.tsv'))
    te, po = int(hdr['TEXT_END'], 16), int(hdr['PATCH_OFF'], 16)
    ko, ks = int(hdr['KEY_PAGE_OFF'], 16), int(hdr['KEY_PAGE_SIZE'], 16)
    bin_ = os.path.abspath(f'{out}/{n}.patched.bin'); sz = os.path.getsize(bin_)
    a, end = r16(te), r16(sz)
    assert po % PG == 0 and a <= ko and ko + ks <= po and ko % PG == 0 and ks == PG
    for sect, lo, hi in ((f'__TEXT,__dm{n}x0', 0, a), (f'__DM{n}W,__img', a, po), (f'__TEXT,__dm{n}x1', po, end)):
        assert all(len(x) <= 16 for x in sect.split(','))
        fill = min(hi, sz) - lo
        asm += [f'    .section {sect}', '    .p2align 14', f'    .incbin "{bin_}", {lo:#x}, {fill:#x}']
        if hi - lo > fill: asm.append(f'    .space {hi - lo - fill:#x}')
        asm.append('    .p2align 14')   # 節尾補到 16K：vm_remap 按整頁取 | Pad the section end to 16K: vm_remap takes whole pages
    flags.append(f'-Wl,-segprot,__DM{n}W,rw,rw')
open(dst, 'w').write('\n'.join(asm) + '\n')
print(' '.join(flags))
