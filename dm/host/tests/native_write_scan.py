#!/usr/bin/env python3
# 我們自己的功能不許寫原版儲藏箱控件（編年史倉庫是獨立頁面；丟裝備事故：編年史頁把原版 grid 設成不可見、遊戲從不重設，個人 / 共享頁看起來全空）。
# 靜態掃：編年史頁（dm_vault_ui.c 全文）與個人倉庫頁碼（dm_cursor.c 的 ui_pages_pump）裡不許出現原版儲藏箱控件名——這兩處只該碰 TouchVault / TouchPgPage 底下的控件。
# 運行時另有一道：dm_vault_ui.c vu_guard（寫之前沿父鏈核是不是我們的控件，不是就報紅不寫）。
# 第二道（不寫原版控件）：整個宿主（dm/host/*.c）不許直接寫控件的可見位 +0x50 / +0x51，只許在帶守衛的函數裡寫——
# dm_cursor.c ui_show_ours、dm_vault_ui.c vu_show、dm_kills.c kb_show（名字不是 Touch 開頭就報紅不寫）。別處要改可見，調它們
# 用法：native_write_scan.py <dm/host 目錄>；--selftest 先拿一段故意違規的文本驗這個掃描本身會報紅
# Our own features must not write the original game's stash widgets (the chronicle vault is a separate page; lost-equipment incident: the chronicle page set the original grid invisible and the game never resets it, so the personal / shared pages looked completely empty).
# Static scan: the chronicle page (all of dm_vault_ui.c) and the personal stash page numbers (ui_pages_pump in dm_cursor.c) must not contain original stash widget names -- these two places should only touch widgets under TouchVault / TouchPgPage.
# There is another check at run time: dm_vault_ui.c vu_guard (before writing, walk the parent chain to check whether it is our widget; if not, report a failure and do not write).
# Second check (do not write original-game widgets): the whole host (dm/host/*.c) must not write a widget's visible flag +0x50 / +0x51 directly, only inside guarded functions --
# dm_cursor.c ui_show_ours, dm_vault_ui.c vu_show, dm_kills.c kb_show (a name not starting with Touch is reported as a failure and not written). To change visibility elsewhere, call them
# Usage: native_write_scan.py <dm/host directory>; --selftest first verifies with a deliberately violating piece of text that the scan itself reports a failure
import os, re, sys

NATIVE = ['grid', 'gold_amount', 'gold_withdraw', 'gold_max', 'basicstash_container', 'advancedstash_gems', 'advancedstash_materials', 'advancedstash_runes',
          'advancedstash_horadriccube', 'SharedStashTabContainer', 'StashNavigation', 'CurrentSharedStashText', 'background', 'PreviousSeasonToggleDisplay']

def scan(text, where):
    bad = []
    for no, line in enumerate(text.split('\n'), 1):
        code = line.split('//', 1)[0]
        for nm in NATIVE:
            if re.search(r'"%s"' % re.escape(nm), code): bad.append('%s:%d 出現原版控件名「%s」：%s' % (where, no, nm, line.strip()[:120]))
    return bad

VIS = re.compile(r'\+\s*0x5[01]\)\s*=(?!=)')
GUARDED = ('ui_show_ours', 'vu_show', 'kb_show')
def vis_scan(text, where):   # 直接寫可見位的行；在帶守衛的函數裡（定義那一行或其函數體內）的不算 | lines that write the visible flag directly; those inside a guarded function (its definition line or its body) do not count
    bad, inside = [], None
    for no, line in enumerate(text.split('\n'), 1):
        m = re.match(r'static\s+\w+\s+(\w+)\(', line)
        if m: inside = m.group(1) if m.group(1) in GUARDED else None
        code = line.split('//', 1)[0]
        if VIS.search(code) and not inside: bad.append('%s:%d 直接寫了可見位（要走 %s）：%s' % (where, no, ' / '.join(GUARDED), line.strip()[:120]))
        if line.startswith('}'): inside = None
    return bad

# 第三道（位置守衛）：dm_cursor.c 裡寫控件矩形（+0x70 x、+0x74 y、+0x78 寬、+0x7c 高）只許在 mv_offset（原版控件，名單 + 記原值 + 必寫回）
# 與 ui_move_ours（只許 Touch 開頭）裡。認兩種寫法：直接 *(int32_t *)(… + 0x7?) = …；或同一函數裡先取 int32_t *名 = (int32_t *)(… + 0x7?)，再寫 名[k] = / *名 =
# Third check (position guard): in dm_cursor.c, writing a widget rectangle (+0x70 x, +0x74 y, +0x78 width, +0x7c height) is allowed only in mv_offset (original-game widgets, allow list + record original value + must write back)
# and in ui_move_ours (only names starting with Touch). Two forms are recognized: a direct *(int32_t *)(... + 0x7?) = ...; or, within the same function, first taking int32_t *name = (int32_t *)(... + 0x7?) and then writing name[k] = / *name =
RECT_OK = ('mv_offset', 'ui_move_ours')
RECT_PTR = re.compile(r'int32_t\s*\*\s*(\w+)\s*=\s*\(int32_t\s*\*\)\s*\([^;]*\+\s*0x7[0-9a-c]\b')
RECT_DIRECT = re.compile(r'\*\s*\(int32_t\s*\*\)\s*\([^)]*\+\s*0x7[0-9a-c]\)\s*=(?!=)')
def rect_scan(text, where):
    bad, fn, ptrs = [], None, set()
    for no, line in enumerate(text.split('\n'), 1):
        m = re.match(r'(?:static\s+)?\w+\s+\*?(\w+)\(', line)
        if m and not line.startswith(' '): fn, ptrs = m.group(1), set()
        code = line.split('//', 1)[0]
        for pm in RECT_PTR.finditer(code): ptrs.add(pm.group(1))
        code = RECT_PTR.sub('', code)   # 聲明那一處（int32_t *名 = …）本身不算寫 | the declaration itself (int32_t *name = ...) does not count as a write
        hit = RECT_DIRECT.search(code) or any(re.search(r'(?<![\w.>])(?:\*\s*%s|%s\s*\[\s*\d\s*\])\s*=(?!=)' % (q, q), code) for q in ptrs)
        if hit and fn not in RECT_OK: bad.append('%s:%d 在 %s 裡直接寫了控件矩形（原版控件走 mv_offset、我們的走 ui_move_ours）：%s' % (where, no, fn, line.strip()[:120]))
    return bad

def pages_pump(src):
    i = src.find('static void ui_pages_pump(')
    if i < 0: raise SystemExit('native_write_scan：dm_cursor.c 裡找不到 ui_pages_pump（改名了要同步這裡）')
    j = src.find('\n}\n', i)
    return src[i:j]

def main():
    if '--selftest' in sys.argv:
        if not scan('    vu_show(vu_find(bk, "grid", 2), 0);', 'selftest') or scan('    vu_show(vu_find(vt, "TouchV_Back", 0), 1);   // "grid" 只在注釋裡', 'selftest') \
           or not vis_scan('static void f(uint64_t w) {\n    *(uint8_t *)(w + 0x50) = 0;\n}', 'selftest') or vis_scan('static void ui_show_ours(uint64_t w, int on) {\n    *(uint8_t *)(w + 0x50) = 1;\n}\n    if (*(uint8_t *)(w + 0x50) == 1) x();', 'selftest') \
           or not rect_scan('static void f(uint64_t w) {\n    int32_t *r = (int32_t *)(w + 0x70); r[0] = 5;\n}', 'selftest') or not rect_scan('static void g(uint64_t w) {\n    *(int32_t *)(w + 0x74) = 3;\n}', 'selftest') \
           or rect_scan('static void k(uint64_t w) {\n    int32_t *rx = (int32_t *)(w + 0x70), ww = *(int32_t *)(w + 0x78); if (*rx) x();\n}\nstatic void mv_offset(uint64_t w) {\n    int32_t *v = (int32_t *)(w + 0x70); *v = 1;\n}\nstatic int h(float *r) {\n    r[0] = 1.0f; int x = *(int32_t *)(w + 0x70); return x;\n}', 'selftest'):
            print('✗ native_write_scan 自檢失敗：該報的沒報或不該報的報了'); return 1
        print('✓ native_write_scan 自檢：違規行會報紅、注釋裡的名字不算')
    host = next(a for a in sys.argv[1:] if not a.startswith('--'))
    bad = scan(open(os.path.join(host, 'dm_vault_ui.c'), encoding='utf-8').read(), 'dm_vault_ui.c')
    bad += scan(pages_pump(open(os.path.join(host, 'dm_cursor.c'), encoding='utf-8').read()), 'dm_cursor.c ui_pages_pump')
    srcs = sorted(f for f in os.listdir(host) if f.endswith('.c'))
    for f in srcs: bad += vis_scan(open(os.path.join(host, f), encoding='utf-8', errors='replace').read(), f)
    bad += rect_scan(open(os.path.join(host, 'dm_cursor.c'), encoding='utf-8').read(), 'dm_cursor.c')
    if bad:
        print('✗ 我們的功能碰了原版控件（編年史頁 / 個人倉庫頁碼只許寫 TouchVault / TouchPgPage 底下的；可見位只許走帶守衛的函數）：'); [print('   ' + b) for b in bad]; return 1
    print('✓ 編年史頁與個人倉庫頁碼不碰原版儲藏箱控件（掃 %d 個名字）；宿主 %d 個源文件沒有繞過守衛直接寫可見位；dm_cursor.c 寫控件矩形只在 mv_offset / ui_move_ours 裡' % (len(NATIVE), len(srcs))); return 0

if __name__ == '__main__':
    sys.exit(main())
