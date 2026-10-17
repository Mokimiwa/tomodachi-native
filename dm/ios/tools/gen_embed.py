# 生成內嵌匯編：把改寫後的模塊映像編進可執行文件，代碼只進 __TEXT 一個執行段（蘋果 ITMS-90999：只許一個帶執行權限的段）。
#   __TEXT,__dm<名>x0   原 text：[0, 圓整16K(TEXT_END))（含 ro 首頁的一部分，只讀）
#   __DM<名>W,__img     RW 段：[圓整16K(TEXT_END), PATCH_OFF)，ro 餘部 + data + bss（重定位只落這裡）
#   __TEXT,__dm<名>x1   補丁區：[PATCH_OFF, 圓整16K(文件尾))
#   補丁區在首址前的模塊（PATCH_OFF 為負，見 patch_nso.py FAR）：W 是 [圓整16K(TEXT_END), FILE_IMAGE_LEN)，x1 是文件裡 FILE_IMAGE_LEN 之後的補丁區；bss 與鍵頁不進可執行文件
# 三塊在可執行文件裡不相鄰；運行時由裝載器（loader.c iOS 分支）保留一塊連續地址，按原相對位置 vm_remap 回去（代碼頁共享已簽名頁，W 寫時複製），
# 所以運行時佈局與原映像逐字節相同：原代碼與 data 之間的 PC 相對尋址、補丁區的相對跳轉、宿主按「基址 + 原偏移」的引用都不用動。
# 用法：gen_embed.py <產物目錄> <輸出 .S> <模塊名...>；鏈接參數打印到 stdout（W 段的 -segprot）
# Generate the embedded assembly: compile the rewritten module image into the executable, with code going into the single executable segment __TEXT only (Apple ITMS-90999: only one segment with execute permission is allowed).
#   __TEXT,__dm<name>x0   original text: [0, round16K(TEXT_END)) (includes part of the first ro page, read-only)
#   __DM<name>W,__img     RW segment: [round16K(TEXT_END), PATCH_OFF), rest of ro + data + bss (relocations land only here)
#   __TEXT,__dm<name>x1   patch area: [PATCH_OFF, round16K(end of file))
#   Modules with the patch area before the base (negative PATCH_OFF, see patch_nso.py FAR): W is [round16K(TEXT_END), FILE_IMAGE_LEN), x1 is the patch area after FILE_IMAGE_LEN in the file; bss and the key page are not in the executable
# The three pieces are not adjacent in the executable; at run time the loader (loader.c iOS branch) reserves one contiguous address range and vm_remaps them back at their original relative positions (code pages share the signed pages, W is copy-on-write),
# so the run-time layout is byte-identical to the original image: PC-relative addressing between original code and data, relative jumps in the patch area, and the host's "base + original offset" references all need no change.
# Usage: gen_embed.py <build output dir> <output .S> <module name...>; link flags are printed to stdout (-segprot for the W segment)
import os, sys
out, dst, names = sys.argv[1], sys.argv[2], sys.argv[3:]
PG = 0x4000
def r16(x): return (x + PG - 1) & ~(PG - 1)
asm = ['// 由 dm/ios/tools/gen_embed.py 生成，勿手改']
flags, opts = [], []
for n in names:
    hdr = dict(l[2:].split() for l in open(f'{out}/{n}.fixups.tsv'))
    te, po = int(hdr['TEXT_END'], 16), int(hdr['PATCH_OFF'], 16)
    ko, ks = int(hdr['KEY_PAGE_OFF'], 16), int(hdr['KEY_PAGE_SIZE'], 16)
    bin_ = os.path.abspath(f'{out}/{n}.patched.bin'); sz = os.path.getsize(bin_)
    a, end = r16(te), r16(sz)
    if po < 0:   # 補丁區在首址前（patch_nso.py FAR）：W 只到 data 尾，bss 與鍵頁由裝載器給零頁；文件裡補丁區接在 FILE_IMAGE_LEN 處 | patch area before the base (patch_nso.py FAR): W ends at the end of data, the loader supplies zero pages for bss and the key page; in the file the patch area follows at FILE_IMAGE_LEN
        fl = int(hdr['FILE_IMAGE_LEN'], 16)
        assert fl % PG == 0 and a <= fl and ko % PG == 0 and ks == PG and ko >= fl
        parts = ((f'__TEXT,__dm{n}x0', 0, a), (f'__DM{n}W,__img', a, fl), (f'__TEXT,__dm{n}x1', fl, end))
    else:
        assert po % PG == 0 and a <= ko and ko + ks <= po and ko % PG == 0 and ks == PG
        parts = ((f'__TEXT,__dm{n}x0', 0, a), (f'__DM{n}W,__img', a, po), (f'__TEXT,__dm{n}x1', po, end))
    for sect, lo, hi in parts:
        assert all(len(x) <= 16 for x in sect.split(','))
        fill = min(hi, sz) - lo
        asm += [f'    .section {sect}', '    .p2align 14', f'    .incbin "{bin_}", {lo:#x}, {fill:#x}']
        if hi - lo > fill: asm.append(f'    .space {hi - lo - fill:#x}')
        asm.append('    .p2align 14')   # 節尾補到 16K：vm_remap 按整頁取 | Pad the section end to 16K: vm_remap takes whole pages
    flags.append(f'-Wl,-segprot,__DM{n}W,rw,rw')
    # 可選剪口（<名>.options.tsv，見 patch_nso.py OPTIONS）：iOS 不能在運行時改代碼頁，所以把「套上剪口後的那幾頁」也編進 __TEXT（__dm<名>o<序>），
    # 裝載器在 DM_OPTIONS 要它時把這頁 vm_remap 到原位置（同樣是已簽名頁）；兩種版本都在包裡，開機時挑，不必為 60 FPS 另出一個包
    # Optional cuts (<name>.options.tsv, see patch_nso.py OPTIONS): iOS cannot change code pages at run time, so the pages with the cut applied are compiled into __TEXT too (__dm<name>o<index>),
    # and the loader vm_remaps such a page over its original place when DM_OPTIONS asks for it (signed pages either way); both versions ship, picked at boot, with no separate build for 60 FPS
    opt = f'{out}/{n}.options.tsv'
    if os.path.exists(opt):
        img, pages = open(bin_, 'rb').read(), {}
        for l in open(opt):
            if l.startswith('#') or not l.strip(): continue
            name, off, ob, nb = l.rstrip('\n').split('\t')[:4]
            off, ob, nb = int(off, 16), bytes.fromhex(ob), bytes.fromhex(nb)
            assert len(ob) == len(nb) and img[off:off + len(ob)] == ob, f'{n} 可選剪口 {name} {off:#x} 原字節不符'
            assert off + len(ob) <= a, f'{n} 可選剪口 {name} {off:#x} 不在代碼區 [0, {a:#x})'   # 目前只有代碼區的剪口 | only code-area cuts for now
            for i, b in enumerate(nb):
                pg = (off + i) & ~(PG - 1)
                page = pages.setdefault((name, pg), bytearray(img[pg:pg + PG]))
                page[off + i - pg] = b
        owner = {}
        for name, pg in pages: assert owner.setdefault(pg, name) == name, f'{n} 可選剪口 {owner[pg]} 和 {name} 改到同一頁 {pg:#x}'
        for k, ((name, pg), page) in enumerate(sorted(pages.items())):
            pf = os.path.abspath(f'{os.path.dirname(dst)}/opt_{n}_{k}.bin'); open(pf, 'wb').write(page)
            sect = f'__TEXT,__dm{n}o{k}'; assert all(len(x) <= 16 for x in sect.split(','))
            asm += [f'    .section {sect}', '    .p2align 14', f'_dmopt_page_{n}_{k}:', f'    .incbin "{pf}"', '    .p2align 14']
            opts.append((name, n, pg, f'_dmopt_page_{n}_{k}'))
# 剪口表（loader.c 讀 dm_embed_opts，名為空結束）：選項名、模塊名、頁的偏移、內嵌頁 | the cut table (read by loader.c as dm_embed_opts, ended by an empty name): option, module, page offset, embedded page
asm += ['    .section __TEXT,__cstring,cstring_literals']
for i, (name, n, pg, lab) in enumerate(opts): asm += [f'_dmopt_name_{i}: .asciz "{name}"', f'_dmopt_mod_{i}: .asciz "{n}"']
asm += ['    .section __DATA,__const', '    .p2align 3', '    .globl _dm_embed_opts', '_dm_embed_opts:']
for i, (name, n, pg, lab) in enumerate(opts): asm += [f'    .quad _dmopt_name_{i}, _dmopt_mod_{i}, {pg:#x}, {lab}']
asm.append('    .quad 0, 0, 0, 0')
open(dst, 'w').write('\n'.join(asm) + '\n')
print(' '.join(flags))
