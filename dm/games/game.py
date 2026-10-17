#!/usr/bin/env python3
# 遊戲檔案：dm/games/<基礎版標題號>/game.tsv（鍵 <TAB> 值…，# 開頭為注釋）+ 同目錄的著色器表與 dmsh_chain.h。著色器工具、宿主構建、iOS 打包與 NSP 腳本都從這裡取每個遊戲的常數。
# 用法：game.py list（列出全部檔案）；game.py <名> <鍵>（打印該鍵的值，shell 用）；game.py header <名> <輸出.h>（生成宿主的 game_profile.h）
#   Python 裡：import game; g = game.load()（取 DM_GAME，沒有缺省遊戲：不設就報錯並列出檔案）；game.by_title(標題號)；g.get / g.words / g.int / g.builds() / g.table(文件名)
# Game profiles: dm/games/<base game title ID>/game.tsv (key <TAB> values..., lines starting with # are comments) + the shader tables and dmsh_chain.h in the same folder. The shader tools, the host build, iOS packaging and the NSP script take each game's constants from here.
# Usage: game.py list (list all profiles); game.py <id> <key> (print that key's value, for shell); game.py header <id> <out.h> (generate the host's game_profile.h)
#   From Python: import game; g = game.load() (takes DM_GAME; there is no default game: unset is an error that lists the profiles); game.by_title(title ID); g.get / g.words / g.int / g.builds() / g.table(file name)
import os, re, sys
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
    cp = g.get('core_preempt', 'off')   # 同核搶佔近似（hle_thread.c）：on / off | same-core preemption approximation (hle_thread.c): on / off
    if cp not in ('on', 'off'): sys.exit(f'{g.id}: core_preempt 只能是 on / off | core_preempt must be on or off: {cp}')
    sf = g.get('shared_font', 'bfttf')   # 共享字體地址處放什麼（hle_sdk16.c）：bfttf = 8 字節頭 + TTF，ttf = 只有 TTF | what the shared-font address points at (hle_sdk16.c): bfttf = 8-byte header + TTF, ttf = the TTF alone
    if sf not in ('bfttf', 'ttf'): sys.exit(f'{g.id}: shared_font 只能是 bfttf / ttf | shared_font must be bfttf or ttf: {sf}')
    pl = g.get('pad_layout', 'simple')   # 虛擬手柄首次的佈局（DmGamepad.m，之後按玩家在遊戲裡切的記）：simple = 2D，full = 3D（十字鍵、右搖桿、L3 / R3） | the virtual gamepad's first layout (DmGamepad.m; afterwards whatever the player switched to in game is kept): simple = 2D, full = 3D (D-pad, right stick, L3 / R3)
    if pl not in ('simple', 'full'): sys.exit(f'{g.id}: pad_layout 只能是 simple / full | pad_layout must be simple or full: {pl}')
    api = g.get('device_api', '55.15')   # nvnDeviceGetInteger 報的 NVN API 主.次版本（hle_nvn.c） | the NVN API major.minor version nvnDeviceGetInteger reports (hle_nvn.c)
    if not re.fullmatch(r'\d+\.\d+', api): sys.exit(f'{g.id}: device_api 要寫成 主.次 | device_api must be major.minor: {api}')
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
         f'#define DM_GAME_CORE_PREEMPT {int(cp == "on")}',
         f'#define DM_GAME_SHARED_FONT_TTF {int(sf == "ttf")}',
         f'#define DM_GAME_PAD_FULL {int(pl == "full")}',
         f'#define DM_GAME_NVN_API_MAJOR {api.split(".")[0]}',
         f'#define DM_GAME_NVN_API_MINOR {api.split(".")[1]}',
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
