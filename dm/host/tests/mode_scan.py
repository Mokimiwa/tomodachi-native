#!/usr/bin/env python3
"""手柄 / 觸屏模式的登記閘門（宿主單測裡跑，秒級）。只讀源碼，不跑遊戲。
三條：
1. dm_cursor.c 每幀那一長行裡的每個泵，要麼帶「if (tm) 」（觸屏專屬：手柄模式與切換進行中不跑），要麼在它上方注釋「兩種模式都跑：」的單子上；
   文件裡定義的每個 *_pump 也必須在這一行或那張單子上出現——新加一個泵、兩邊都不登記就紅。
2. 散判模式（dm_padmode_current()）與替玩家按鍵 / 發消息（fe_key / ui_auto_key / fe_send / dm_ui_key_pulse）的處數按文件記在下面兩張表裡，只許減不許加。
   要加：先想清楚它在另一種模式下該停、照常還是換做法，登記進 dm_mode_items.h（要隨切換清理的）或上面那張單子，再改這裡的數並在提交說明裡寫原因。
3. dm_mode_items.h 裡登記的進 / 出函數都得有定義。
用法：mode_scan.py <dm 目錄>
Registration gate for controller / touch modes (run in the host unit tests, takes seconds). Reads source only, does not run the game.
Three rules:
1. Every pump on the long per-frame line in dm_cursor.c either carries "if (tm) " (touch-only: not run in controller mode or while a switch is in progress) or is on the list in the comment above it headed 「兩種模式都跑：」(runs in both modes:);
   every *_pump defined in the file must also appear on this line or on that list -- a newly added pump registered in neither fails.
2. The number of places that check the mode ad hoc (dm_padmode_current()) and that press keys / send messages on the player's behalf (fe_key / ui_auto_key / fe_send / dm_ui_key_pulse) is recorded per file in the two tables below, and may only decrease, never increase.
   To add one: first think through whether in the other mode it should stop, carry on as usual, or work differently, register it in dm_mode_items.h (for things to be cleaned up on a switch) or on the list above, then change the number here and state the reason in the commit message.
3. Every enter / leave function registered in dm_mode_items.h must have a definition.
Usage: mode_scan.py <dm directory>
"""
import re, sys, os, glob
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..')
red = []
def src(rel): return open(os.path.join(root, rel), encoding='utf-8').read()
def strip_comments(s): return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', s, flags=re.S))

# ---- 1 ----
cur = src('host/dm_cursor.c')
m = re.search(r'^.*int tm = ui_touch_mode\(\);.*$', cur, re.M)
if not m: red.append('dm_cursor.c 裡找不到每幀那一行（int tm = ui_touch_mode();）')
else:
    line = m.group(0)
    both_txt = re.search(r'兩種模式都跑：(.*?)\nstatic int ui_touch_mode', cur, re.S)
    both = set(re.findall(r'\b(\w+_pump|ui_map_center|ui_panel_heal)\b', both_txt.group(1))) if both_txt else set()
    if not both: red.append('dm_cursor.c 裡找不到「兩種模式都跑：」那張單子')
    on_line = {}
    for mm in re.finditer(r'(if \(tm\) )?\b(\w+_pump|ui_map_center)\(', line): on_line[mm.group(2)] = bool(mm.group(1))
    for name, gated in sorted(on_line.items()):
        if not gated and name not in both: red.append('每幀那一行裡的 %s 既沒帶 if (tm)、也不在「兩種模式都跑」單子上' % name)
        if gated and name in both: red.append('%s 帶了 if (tm) 又列在「兩種模式都跑」單子上，二選一' % name)
    for name in sorted(set(re.findall(r'^static void (\w+_pump)\(', cur, re.M))):
        if name not in on_line and name not in both: red.append('dm_cursor.c 定義了 %s，但每幀那一行和「兩種模式都跑」單子上都沒有它' % name)

# ---- 2 ----
PADMODE = {'host/dm_cursor.c': 8, 'host/dm_custom.c': 2, 'host/dm_kills.c': 2, 'host/dm_rift_ui.c': 4, 'host/dm_runeword.c': 3, 'host/dm_vault_ui.c': 1, 'host/dm_update.c': 2, 'host/hle_tact.c': 1}
AUTOKEY = {'host/dm_cursor.c': 38, 'host/dm_rift_ui.c': 6, 'host/hle_hid.c': 2, 'ios/Sources/front_ios.m': 4}
def count(pat, skip_defs):
    out = {}
    for f in sorted(glob.glob(os.path.join(root, 'host', '*.c')) + glob.glob(os.path.join(root, 'ios', 'Sources', '*.m')) + glob.glob(os.path.join(root, 'ios', 'Sources', '*.c'))):
        rel = os.path.relpath(f, root); s = strip_comments(open(f, encoding='utf-8').read()); n = 0
        for mm in re.finditer(pat, s):
            ls = s.rfind('\n', 0, mm.start()) + 1; head = s[ls:mm.start()]
            if skip_defs and re.search(r'(^|\s)(static\s+)?(void|int|uint64_t)\s+$', head): continue   # 函數定義 / 聲明本身不算調用 | A function definition / declaration itself does not count as a call
            n += 1
        if n: out[rel] = n
    return out
for label, table, got in (('散判模式 dm_padmode_current()', PADMODE, count(r'\bdm_padmode_current\(\)', True)),
                          ('替玩家按鍵 / 發消息', AUTOKEY, count(r'\b(fe_key|ui_auto_key|fe_send|dm_ui_key_pulse|dm_ui_key_pulse_alone|dm_ui_send|dm_ui_send_queue)\(', True))):
    if label.startswith('散判') and 'host/dm_padmode.c' in got: got.pop('host/dm_padmode.c')
    for rel in sorted(set(got) | set(table)):
        g, t = got.get(rel, 0), table.get(rel, 0)
        if g > t: red.append('%s：%s 現有 %d 處，表裡記的是 %d 處——新增的先登記（見本文件開頭），再改表' % (label, rel, g, t))
        elif g < t: red.append('%s：%s 現有 %d 處，比表裡的 %d 少了——把表裡的數改小（只許減）' % (label, rel, g, t))

# ---- 3 ----
items = src('host/dm_mode_items.h'); allsrc = ''.join(open(f, encoding='utf-8').read() for f in glob.glob(os.path.join(root, 'host', '*.c')))
for name, side, enter, leave in re.findall(r'X\("([^"]+)",\s*(\d),\s*(\w+),\s*(\w+)\)', items):
    for fn in (enter, leave):
        if not re.search(r'\bvoid %s\(void\)\s*\{' % fn, allsrc): red.append('dm_mode_items.h 登記項「%s」的函數 %s 沒有定義' % (name, fn))

if red:
    print('模式登記閘門 紅：%d 條' % len(red)); [print('  ' + r) for r in red]; sys.exit(1)
print('✓ 模式登記閘門：每幀泵 %d 個都已分類，散判模式與替按鍵的處數沒有超出登記，登記項函數齊全' % (len(on_line) if m else 0))
