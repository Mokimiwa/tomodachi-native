#!/usr/bin/env python3
# 遊戲檔案：dm/games/<基礎版標題號>/game.tsv（鍵 <TAB> 值…，# 開頭為注釋）+ 同目錄的著色器表與 dmsh_chain.h。著色器工具、宿主構建、iOS 打包與 NSP 腳本都從這裡取每個遊戲的常數。
# 用法：game.py list（列出全部檔案）；game.py <名> <鍵>（打印該鍵的值，shell 用）；game.py header <名> <輸出.h>（生成宿主的 game_profile.h）
#   Python 裡：import game; g = game.load()（取 DM_GAME，沒有缺省遊戲：不設就報錯並列出檔案）；game.by_title(標題號)；g.get / g.words / g.int / g.builds() / g.table(文件名)
# Game profiles: dm/games/<base game title ID>/game.tsv (key <TAB> values..., lines starting with # are comments) + the shader tables and dmsh_chain.h in the same folder. The shader tools, the host build, iOS packaging and the NSP script take each game's constants from here.
# Usage: game.py list (list all profiles); game.py <id> <key> (print that key's value, for shell); game.py header <id> <out.h> (generate the host's game_profile.h)
#   From Python: import game; g = game.load() (takes DM_GAME; there is no default game: unset is an error that lists the profiles); game.by_title(title ID); g.get / g.words / g.int / g.builds() / g.table(file name)
import os, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STAGES = {'vs': 0, 'ps': 1, 'gs': 2, 'hs': 3, 'ds': 4, 'cs': 5}   # NVN 著色器階段號 | NVN shader stage numbers


class Game:
    def __init__(self, d):
        self.dir = Path(d).resolve(); self.id = self.dir.name; self.rows = []
        for line in (self.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.lstrip().startswith('#'): continue
            c = line.split('\t'); self.rows.append((c[0].strip(), [v.strip() for v in c[1:]]))

    def values(self, key):   # 該鍵的每一行（build 等可重復的鍵） | every row of this key (keys such as build repeat)
        return [v for k, v in self.rows if k == key]

    def get(self, key, default=None):
        r = self.values(key)
        if r: return '\t'.join(r[0])
        if default is None: raise KeyError(f'{self.dir / "game.tsv"} 沒有鍵 {key} | has no key {key}')
        return default

    def words(self, key, default=None): return self.get(key, default).split()
    def int(self, key): return int(self.get(key), 0)
    def table(self, name): return self.dir / name

    @property
    def title_id(self): return int(self.get('title_id'), 16)

    def builds(self):
        """{版本: {'display': 顯示版本, 模塊名: 構建號, …}} | {version: {'display': display version, module name: build ID, ...}}"""
        out = {}
        for ver, disp, *mods in self.values('build'):
            out[ver] = dict([('display', disp)] + [tuple(m.split('=', 1)) for m in mods])
        return out

    def pairs(self, key):   # 「a=b c=d」→ [(a, b), (c, d)] | "a=b c=d" → [(a, b), (c, d)]
        return [tuple(w.split('=', 1)) for w in self.words(key)]


def load(name=None):
    name = name or os.environ.get('DM_GAME')
    if not name:   # 沒有缺省遊戲 | there is no default game
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'【紅】沒設 DM_GAME：用基礎版標題號選遊戲檔案：{have} | DM_GAME is not set: pick a game profile by base game title ID: {have}')
    d = Path(name) if os.sep in str(name) else HERE / name
    if not (d / 'game.tsv').exists():   # 檔案目錄以基礎版標題號命名（舊名 smbw / fe_engage 已不用）| profile folders are named after the base game's title ID (the old names smbw / fe_engage are gone)
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'【紅】沒有遊戲檔案 {d / "game.tsv"}（DM_GAME / GAME 用基礎版標題號：{have}）| no game profile {d / "game.tsv"} (DM_GAME / GAME take the base game title ID: {have})')
    return Game(d)


def all_games():
    return [Game(d) for d in sorted(HERE.iterdir()) if (d / 'game.tsv').exists()]


def by_title(tid):
    return next((g for g in all_games() if g.title_id == tid), None)


def header(g):
    """宿主的 game_profile.h：把檔案裡宿主要用的常數寫成宏 | the host's game_profile.h: the profile constants the host uses, as macros"""
    q = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    mods = g.words('modules')
    ssbo = dict(g.pairs('nvn_ssbo_bases'))
    sb = ' : '.join(f'(stage) == {STAGES[k]} ? {v}' for k, v in ssbo.items()) + ' : -1'
    vers = ', '.join('{' + q(b['main']) + ', ' + q(b['display']) + '}' for b in g.builds().values() if 'main' in b)
    pi = g.get('present_interval', 'game')   # 出圖間隔：game = 按遊戲設的（同 Switch），display = 每個顯示節拍一幀 | present interval: game = as the game sets it (as on the Switch), display = one frame per display tick
    if pi not in ('game', 'display'): sys.exit(f'{g.id}: present_interval 只能是 game / display | present_interval must be game or display: {pi}')
    nc = g.get('network_clock', 'offline')   # 網絡時鐘：offline = 不可用（缺省），synced = 與用戶時鐘同值、可用 | network clock: offline = unavailable (default), synced = available, same value as the user clock
    if nc not in ('offline', 'synced'): sys.exit(f'{g.id}: network_clock 只能是 offline / synced | network_clock must be offline or synced: {nc}')
    ml = g.get('mouse_left', 'A')   # Mac 滑鼠左鍵的默認控制項：A = 點畫面鎖定滑鼠、移動推小手（缺省），Pointer = 點哪觸屏哪 | default control of the Mac left mouse button: A = click captures the mouse and moving pushes the hand (default), Pointer = touch where clicked
    if ml not in ('A', 'Pointer'): sys.exit(f'{g.id}: mouse_left 只能是 A / Pointer | mouse_left must be A or Pointer: {ml}')
    # 輸出分辨率（改寫器 PARAMS 的分辨率參數位，dm/host/dm_aspect.h）：主機 / 掌機的 16:9 尺寸，缺省是暗黑 2 的 1600x900 / 1280x720；
    # render_wide_max = 寬屏時主機模式寬度上限（同高放寬），0 = 不放寬
    # Output resolution (the rewriter's PARAMS resolution slots, dm/host/dm_aspect.h): the docked / handheld 16:9 sizes, defaulting to Diablo II's 1600x900 / 1280x720;
    # render_wide_max = the docked width cap when widening (at the same height), 0 = no widening
    rs = [tuple(int(v) for v in r.lower().split('x')) for r in g.words('render_size', '1600x900 1280x720')]
    if len(rs) != 2 or any(len(r) != 2 or min(r) <= 0 for r in rs): sys.exit(f'{g.id}: render_size 要寫「主機寬x高 掌機寬x高」| render_size must be "dockedWxH handheldWxH": {rs}')
    wide = int(g.get('render_wide_max', '0'))
    if wide and wide < rs[0][0]: sys.exit(f'{g.id}: render_wide_max 比主機寬度小 | render_wide_max is below the docked width: {wide}')
    tall = int(g.get('render_tall_max', '0'))
    if tall and tall < rs[0][1]: sys.exit(f'{g.id}: render_tall_max 比主機高度小 | render_tall_max is below the docked height: {tall}')
    # 界面著色器（ui_shaders = shader_map.tsv 第 5 列的來源前綴）：出圖比例不是 16:9 時，回放把用這些頂點著色器的繪製畫進置中的 16:9 帶（nvn_mtl.m ui_band）
    # UI shaders (ui_shaders = source prefixes of column 5 of shader_map.tsv): when the output is not 16:9, the replay draws the draws using these vertex shaders into a centred 16:9 band (nvn_mtl.m ui_band)
    uip = g.words('ui_shaders', '')
    uivs = sorted({int(r[3], 16) for r in (l.split('\t') for l in g.table('shader_map.tsv').read_text(encoding='utf-8').splitlines() if l and not l.startswith('#'))
                   if len(r) >= 5 and r[2] == '頂點' and any(r[4].startswith(p_) for p_ in uip)}) if uip else []
    if uip and not uivs: sys.exit(f'{g.id}: ui_shaders 在 shader_map.tsv 裡一個頂點著色器都沒對上 | ui_shaders matched no vertex shader in shader_map.tsv: {uip}')
    L = [f'// 由 dm/games/game.py 從 dm/games/{g.id}/game.tsv 生成，勿手改 | Generated by dm/games/game.py from dm/games/{g.id}/game.tsv; do not edit',
         '#pragma once',
         f'#define DM_GAME_ID {q(g.id)}',
         f'#define DM_GAME_NAME {q(g.get("name"))}',
         f'#define DM_GAME_PROGRAM_ID 0x{int(g.get("program_id"), 16):016X}ull',
         f'#define DM_GAME_MODULES {{{", ".join(map(q, mods))}}}',
         f'#define DM_GAME_NMOD {len(mods)}',
         f'#define DM_GAME_ROMFS_MARKER {q(g.get("romfs_marker"))}',
         f'#define DM_GAME_MEM_RESERVE {g.int("mem_reserve"):#x}ull',
         f'#define DM_GAME_MEM_ALIAS {g.int("mem_alias"):#x}ull',
         f'#define DM_GAME_MEM_STACK {g.int("mem_stack"):#x}ull',
         f'#define DM_GAME_APP_POOL {g.int("app_pool"):#x}ull',
         f'#define DM_GAME_NVN_SSBO_BASE(stage) ({sb})',
         f'#define DM_GAME_NVN_TEX_BASE {g.int("nvn_tex_base"):#x}',
         f'#define DM_GAME_NVN_TEX_SLOTS {g.int("nvn_tex_slots")}',
         f'#define DM_GAME_NVN_SAMP_BASE {g.int("nvn_samp_base"):#x}',
         f'#define DM_GAME_NVN_IMG_BASE {g.int("nvn_img_base"):#x}',
         f'#define DM_GAME_NVN_IMG_SLOTS {g.int("nvn_img_slots")}',
         f'#define DM_GAME_DISPLAY_VERSIONS {vers}',
         f'#define DM_GAME_PRESENT_DISPLAY {int(pi == "display")}',
         f'#define DM_GAME_NET_CLOCK_SYNCED {int(nc == "synced")}',
         f'#define DM_GAME_MOUSE_LEFT {q(ml)}',
         f'#define DM_GAME_RES_DOCKED_W {rs[0][0]}',
         f'#define DM_GAME_RES_DOCKED_H {rs[0][1]}',
         f'#define DM_GAME_RES_HANDHELD_W {rs[1][0]}',
         f'#define DM_GAME_RES_HANDHELD_H {rs[1][1]}',
         f'#define DM_GAME_RES_WIDE_MAX {wide}',
         f'#define DM_GAME_RES_TALL_MAX {tall}',
         f'#define DM_GAME_UI_VS_N {len(uivs)}',
         f'#define DM_GAME_UI_VS {{{", ".join(f"0x{k:016x}ull" for k in uivs) or "0"}}}',
         '']
    return '\n'.join(L)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['list']:
        for g in all_games(): print(f'{g.id}\t{g.title_id:016X}\t{g.get("name", "")}')
    elif len(a) == 3 and a[0] == 'header':
        out = Path(a[2]); out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_name(out.name + '.tmp'); tmp.write_text(header(load(a[1]))); tmp.replace(out)
    elif len(a) == 2:
        print(load(a[0]).get(a[1]))
    else:
        sys.exit(__doc__ or '用法 | usage: game.py list | game.py <id> <key> | game.py header <id> <out.h>')
