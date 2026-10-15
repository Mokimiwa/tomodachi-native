#!/usr/bin/env python3
# 每幀附件帶寬預算表（目標 120 幀、≤ 20 GB/s → 每幀 ≤ 170 MB）：從 DM_NVN_PASSDUMP 的一幀數據流按通道列讀入 / 寫回 MB，
# 按兩者之和從大到小排。讀入按 P 行 load 計（clear / dontcare 不計）；寫回按 P 行的附件計、扣掉 Z 行判 dontcare 的（寫回前瞻）。
# 合一深度模板格式（fmt 260 = Depth32Float_Stencil8）按深度 4 + 模板 1 字節拆：d 行記深度平面，s 行（只掛模板）記模板平面。
# 紋理採樣讀取（補列，粗估）：
#   全屏類通道（繪製頂點數 ≤ 6 且繪製 ≤ 3 次；繪製次數取 PASSDUMP_ALL 的 d 行，沒有就取片元程序綁定行數）：每個被採樣的綁定視圖按 min(綁定視圖整張字節, 目標像素數 × 每紋素字節)；
#   幾何類通道：目標像素數 × 通道內片元著色器聲明的貼圖槽數（取最大）× 被採樣貼圖的平均每紋素字節（每像素每槽約採一次，材質貼圖有 mip）。
#   貼圖格式與尺寸取同幀同通道每個描述符的 B 行（同一序號可綁定不同級 / 層的視圖），查不到按 4 字節；槽數取 B 庫反射 json。
# 用法：tbdr_budget.py <含 PASSDUMP 的日誌> [幀號（缺省取第一幀）] [預算 MB=170] [--tsv=全量表路徑]
# TSV 不截前 25 名 / 著色器鍵；帶逐次繪製來源、未知貼圖分母與合併通道備注。紋理列始終是綁定模型粗估，不是外存實測。
# Per-frame attachment bandwidth budget table (target 120 fps, ≤ 20 GB/s → ≤ 170 MB per frame): from one frame of the DM_NVN_PASSDUMP data stream, list load / store MB per pass,
# sorted by their sum in descending order. Load is counted from P rows with load (clear / dontcare not counted); store is counted from the P rows' attachments, minus those the Z rows judge dontcare (store lookahead).
# Combined depth-stencil formats (fmt 260 = Depth32Float_Stencil8) are split as depth 4 + stencil 1 bytes: d rows record the depth plane, s rows (stencil-only attachment) record the stencil plane.
# Texture sampling reads (supplementary column, rough estimate):
#   fullscreen-class passes (draw vertex count ≤ 6 and ≤ 3 draws; draw count from the d rows of PASSDUMP_ALL, or else the number of fragment program binding rows): for each sampled bound view, min(whole bound view bytes, target pixel count × bytes per texel);
#   geometry-class passes: target pixel count × number of texture slots declared by the fragment shaders in the pass (take the max) × average bytes per texel of the sampled textures (about one sample per pixel per slot; material textures have mips).
#   Texture format and size come from the B row of each descriptor in the same frame and pass (the same serial can bind views of different levels / layers); 4 bytes if not found; slot counts come from the B library reflection json.
# Usage: tbdr_budget.py <log containing PASSDUMP> [frame number (default: first frame)] [budget MB=170] [--tsv=path of the full table]
# The TSV is not truncated to the top 25 / shader keys; it carries per-draw sources, the unknown-texture denominator and merged-pass notes. The texture column is always a rough binding-model estimate, not an external-memory measurement.
import re, sys, collections
BPP = {70: 4, 71: 4, 80: 4, 81: 4, 90: 4, 92: 4, 93: 4, 115: 8, 125: 16, 55: 4, 25: 2, 65: 4, 105: 4, 103: 4, 252: 4, 250: 2, 260: 5, 255: 4, 10: 1, 20: 2, 30: 2, 60: 4, 53: 4}
TSV = next((a[6:] for a in sys.argv[1:] if a.startswith('--tsv=')), None)
args = [a for a in sys.argv[1:] if not a.startswith('--tsv=')]
log = args[0]; FR = args[1] if len(args) > 1 else None; BUDGET = float(args[2]) if len(args) > 2 else 170
P = collections.OrderedDict(); dead = set(); shader = collections.defaultdict(collections.Counter)
TBPP = {10: 1, 20: 2, 25: 2, 30: 2, 53: 4, 55: 4, 60: 4, 65: 8, 70: 4, 71: 4, 80: 4, 81: 4, 90: 4, 92: 4, 93: 4, 103: 8, 105: 8, 115: 8, 125: 16,
        130: .5, 131: .5, 132: 1, 133: 1, 134: 1, 135: 1, 140: .5, 141: .5, 142: 1, 143: 1, 150: 1, 151: 1, 152: 1, 153: 1, 250: 2, 252: 4, 255: 4, 260: 4}
texinfo = collections.defaultdict(dict); sampled = collections.defaultdict(set); tgt = {}; maxvc = collections.defaultdict(int); ndraw = collections.defaultdict(int); fskeys = collections.defaultdict(set); fused = set(); att_desc = collections.defaultdict(list)
import os, json; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_chain
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作區根目錄（遊戲文件、產物、運行現場都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)
LIB = os.path.join(DM_ROOT, 'build/cache/metal_shader_cache/b-{dmsh_chain.version()}')
def tex_slots(key):
    try: return sum(1 for b in json.load(open(f'{LIB}/{key}_b.json'))['bindings'] if b.get('kind') in ('tex', 'samp_tex'))
    except Exception: return 4
nm = {}
for ln in open(dmsh_chain.GAME.table('shader_map.tsv')):   # 當前遊戲檔案的映射表 | the current game profile's map
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): nm.setdefault(c[3], c[0][:-4])
for l in open(log, errors='replace'):
    m = re.search(r'\[pass\] F(\d+) (\w) (\d+) (.*)', l)
    if not m: continue
    f, k, p, r = m.groups()
    if FR is None: FR = f
    if f != FR: continue
    p = int(p); e = P.setdefault(p, {'att': [], 'load': 0, 'store': 0})
    if k == 'P':
        a = re.match(r'(c\d+|d|s) ser=(\d+) fmt=(\d+) (\d+)x(\d+)(?:.*? lv(\d+) sl(\d+))?.*?\b(clear|load|dontcare)', r)
        if not a: continue
        slot, ser, fmt, w, h, lv, sl, act = a.groups(); fmt = int(fmt); lv = int(lv or 0)
        b = max(int(w) >> lv, 1) * max(int(h) >> lv, 1) * BPP.get(fmt, 4)
        if fmt == 260: b = b // 5 * (4 if slot == 'd' else 1)
        e['att'].append((slot, int(ser), lv, int(sl or 0), b, act))
        att_desc[p].append(r)
        tgt.setdefault(p, max(int(w) >> lv, 1) * max(int(h) >> lv, 1))
    elif k == 'B':
        b = re.match(r'st\d+ (?:tex|tbuf) reg\d+ tid=(\d+) 實 ser=(\d+) fmt=(\d+) (\d+)x(\d+)x(\d+)', r)
        if b:
            tid, ser, fmt, w, h, dd = map(int, b.groups())
            texinfo[p].setdefault(ser, set()).add((tid, fmt, w, h, dd))
    elif k == 'R':
        rr = re.match(r'st\d+ ser=(\d+)', r)
        if rr: sampled[p].add(int(rr.group(1)))
    elif k == 'Z':
        z = re.match(r'(c|d|s) ser=(\d+) lv(\d+) sl(\d+) (dontcare|store)', r)
        if z and z.group(5) == 'dontcare': dead.add((p, z.group(1), int(z.group(2)), int(z.group(3)), int(z.group(4))))
    elif k == 'F' and '合併下一個通道' in r: fused.add(p)
    elif k == 'K' and ' st1 ' in f' {r} ':
        key = r.split('key=')[1].split()[0].replace('_b', ''); shader[p][nm.get(key, key[:8])] += 1; fskeys[p].add(key)
    elif k in ('D', 'd'):   # d 行（PASSDUMP_ALL）每次繪製一行，可數繪製次數；D 行只有每通道第一次 | d rows (PASSDUMP_ALL) are one per draw, so draws can be counted; D rows only have the first of each pass
        v = re.search(r'vc=(\d+)', r)
        if v: maxvc[p] = max(maxvc[p], int(v.group(1)))
        if k == 'd': ndraw[p] += 1
ALLD = any(ndraw.values())   # 有逐次繪製行時按繪製次數分類，否則按片元程序綁定行數（只記首次時會把多次小方塊誤判成全屏類） | classify by draw count when per-draw rows exist, otherwise by the number of fragment program binding rows (when only the first is recorded, many small quads are misclassified as fullscreen class)
rows = []
for p, e in P.items():
    ld = sum(b for *_, b, act in e['att'] if act == 'load')
    st = sum(b for slot, ser, lv, sl, b, act in e['att'] if (p, slot[0], ser, lv, sl) not in dead)
    px = tgt.get(p, 0); tx = 0
    nd = ndraw[p] if ALLD else sum(shader[p].values())
    if maxvc[p] <= 6 and nd <= 3:   # 全屏類（繪製頂點 ≤ 6 且繪製 ≤ 3 次；UI 等多次小方塊按幾何類算） | fullscreen class (draw vertices ≤ 6 and ≤ 3 draws; many small quads such as UI count as geometry class)
        for ser in sampled[p]:
            for tid, f, w, h, dd in texinfo[p].get(ser, {(-1, None, 0, 0, 1)}):
                tx += min(w * h * dd * TBPP.get(f, 4), px * TBPP.get(f, 4)) if f is not None else px * 4
    elif sampled[p]:    # 幾何類 | geometry class
        bp = [TBPP.get(v[1], 4) for s_ in sampled[p] for v in texinfo[p].get(s_, ())] or [4]
        tx = px * max([tex_slots(k_) for k_ in fskeys[p]] or [4]) * (sum(bp) / len(bp))
    rows.append((ld + st + tx, ld, st, tx, p, ' '.join(f'{slot}:{ser}' + (f'/lv{lv}' if lv else '') for slot, ser, lv, sl, b, act in e['att']), ', '.join(shader[p])[:40]))
tl = sum(r[1] for r in rows) / 1e6; ts = sum(r[2] for r in rows) / 1e6; tt = sum(r[3] for r in rows) / 1e6
print(f'幀 {FR}：通道 {len(rows)} 個；附件 讀入 {tl:.1f} + 寫回 {ts:.1f} = {tl + ts:.1f} MB（預算 {BUDGET:.0f}，超 {tl + ts - BUDGET:+.1f}）；紋理採樣（粗估{"" if ALLD else "，無逐次繪製行，分類偏粗"}）{tt:.1f} MB；總帶寬 {tl + ts + tt:.1f} MB')
print('總 MB | 附件讀入 | 附件寫回 | 紋理採樣 | 通道 | 附件 | 主要片元著色器')
acc = 0
for t, ld, st, tx, p, att, sh in sorted(rows, reverse=True)[:25]:
    acc += t; print(f'{t / 1e6:6.1f} | {ld / 1e6:5.1f} | {st / 1e6:5.1f} | {tx / 1e6:5.1f} | {p} | {att[:52]} | {sh}   （累計 {acc / 1e6:.0f}）')
if TSV:
    import csv
    with open(TSV, 'w', encoding='utf-8', newline='') as out:
        wr = csv.writer(out, delimiter='\t')
        wr.writerow(['frame', 'render_order', 'pass', 'draws', 'draw_source', 'sampling_model', 'target_pixels', 'load_bytes', 'store_bytes', 'texture_est_bytes', 'total_est_bytes', 'unknown_textures', 'unknown_formats', 'texture_views', 'sampled_serials', 'fragment_keys', 'fragment_names', 'attachments', 'attachment_details', 'note'])
        for order, (total, ld, st, tx, p, att, _) in enumerate(rows):
            nd = ndraw[p] if ALLD else sum(shader[p].values())
            source = 'PASSDUMP_ALL' if ndraw[p] else 'clear_only' if not maxvc[p] else 'shader_bindings'
            model = 'small_draw' if maxvc[p] <= 6 and nd <= 3 else 'geometry'
            views = [v for s_ in sampled[p] for v in texinfo[p].get(s_, ())]
            wr.writerow([FR, order, p, nd, source, model, tgt.get(p, 0), int(ld), int(st), int(tx), int(total),
                         len(sampled[p] - texinfo[p].keys()), sum(v[1] not in TBPP for v in views), len(views), ','.join(map(str, sorted(sampled[p]))), ','.join(sorted(fskeys[p])), ','.join(shader[p]), att, '; '.join(att_desc[p]),
                         '綁定模型粗估；含tile綁定，不是外存實測' if p in fused else '綁定模型粗估，不是外存實測'])
