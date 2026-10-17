#!/usr/bin/env python3
# 從 NSP 一路出未簽名 IPA：認遊戲（標題號 → dm/games/<名>/ 遊戲檔案）→ 解包（nx_extract）→ 改寫代碼模塊（patch_nso.py）→ 拆出著色器並與檔案裡的表核對（shader_split.py）
#   → iOS 著色器庫（dmsh_build.py）→ build_ipa.sh。沒有對應檔案的遊戲只做粗查（<NSP 文件夾>/<標題號>_survey/survey.md），不出 IPA。
# 兩種跑法：在 Spyder 裡打開本文件按 F5（用下面「設置」裡的值，留空的 NSP 文件夾與 prod.keys 運行時會問）；
#   或在終端裡 python3 scripts/build_ipa_from_nsp.py <NSP 文件夾> -k <prod.keys> [選項]（命令行給的值蓋過「設置」，-h 看全部選項；
#   終端不接鍵盤輸入時缺的值不問、直接報錯；成功退出碼 0，出錯 1，中斷 130）。Spyder 的「運行配置」裡填命令行選項也一樣生效。
# 只用三樣東西：本倉庫、放一個遊戲 NSP 的文件夾（基礎版 + 檔案 shader_versions 列的更新都要；有 DLC NSP 就一並打進去，解鎖遊戲裡已有的追加內容）、prod.keys。
# 所有中間產物都在 NSP 文件夾裡的 <名>_build/：extract/（解出的遊戲文件，出包後默認刪掉）、aoc/（DLC 按序號解出的 RomFS）、tools/、patched_*/、shader/（著色器包與緩存，留著下次出包快）、logs/（每步的完整輸出）、tmp/、ipa/。出的 IPA 缺省也放在 NSP 文件夾裡。
# 著色器翻譯器用倉內的 dm/gfx/tools/dmsh_xlate_bin（與 dmsh_chain.lock 指紋一致的那份；重編出的 dll 指紋不同，dmsh_build 會拒絕）。
# 運行本文件的 Python 要有 lz4、capstone、fonttools；另需 Xcode、xcodegen、.NET SDK 10、zstd、spirv-cross。首次要幾十分鐘（大頭是著色器庫），再跑快很多（著色器庫走緩存）。密鑰值一律不打印。
# Unsigned IPA straight from the NSPs: identify the game (title ID → game profile dm/games/<id>/) → extract (nx_extract) → rewrite the code modules (patch_nso.py) → split out the shaders and check them against the profile's tables (shader_split.py)
#   → iOS shader libraries (dmsh_build.py) → build_ipa.sh. A game with no matching profile is only surveyed (<NSP folder>/<title ID>_survey/survey.md); no IPA.
# Two ways to run it: open this file in Spyder and press F5 (it uses the values under "Settings" below; an empty NSP folder or prod.keys is asked for at run time);
#   or in a terminal: python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> [options] (values given on the command line override "Settings", -h lists every option;
#   without keyboard input a missing value is not asked for but reported as an error; exit code 0 on success, 1 on error, 130 when interrupted). Command-line options entered in Spyder's run configuration work the same way.
# Uses only three things: this repository, a folder holding one game's NSPs (the base game plus the updates listed in the profile's shader_versions are required; DLC NSPs, when present, are packed too and unlock the add-on content already in the game), and prod.keys.
# Every intermediate file is under <id>_build/ in the NSP folder: extract/ (the extracted game files, deleted after the IPA is built by default), aoc/ (each DLC's RomFS by index), tools/, patched_*/, shader/ (shader package and caches, kept so the next build is fast), logs/ (each step's full output), tmp/, ipa/. The finished IPA also goes to the NSP folder by default.
# The shader translator is the in-repo dm/gfx/tools/dmsh_xlate_bin (the build whose fingerprints match dmsh_chain.lock; a rebuilt dll has different fingerprints and dmsh_build refuses it).
# The Python running this file needs lz4, capstone and fonttools; also needed: Xcode, xcodegen, .NET SDK 10, zstd, spirv-cross. The first run takes tens of minutes (mostly the shader libraries); later runs are much faster (the shader libraries come from the cache). Key values are never printed.
import collections, ctypes, getpass, os, re, shutil, struct, subprocess, sys, tempfile, threading, time, zlib
from pathlib import Path

# ---- 設置 | Settings ----
NSP_DIR = ''                            # 放 NSP 的文件夾（留空 = 運行時問） | folder holding the NSPs (empty = ask at run time)
PROD_KEYS = ''                          # prod.keys 的路徑（留空 = 運行時問） | path of prod.keys (empty = ask at run time)
PACKAGE = ''                            # 打進 IPA 的版本（留空 = 遊戲檔案的 package；如 'v0' = 基礎版，'v327680' 之類 = 某個更新） | version packed into the IPA (empty = the profile's package; e.g. 'v0' = the base game, values like 'v327680' = an update)
OUT_IPA = ''                            # 輸出的 IPA（留空 = NSP 文件夾裡的 <檔案的 ipa>） | output IPA (empty = <the profile's ipa> in the NSP folder)
APP_NAME = ''                           # 主屏上的 App 名（留空 = 基礎版 NSP 裡的遊戲名） | app name on the home screen (empty = the game name in the base NSP)
BUNDLE_ID = ''                          # App 的包名（留空 = org.nxnative.<基礎版標題號小寫>） | the app's bundle ID (empty = org.nxnative.<base game title ID in lowercase>)
KEEP_EXTRACTED = False                  # 出包後保留解出的遊戲文件（下次省去解包；與遊戲文件一樣大） | keep the extracted game files after the IPA is built (saves extraction next time; as big as the game files)
DM_ENV_FILE = ''                        # 可選：診斷包的環境開關文件（每行 KEY=VALUE） | optional: environment switches for a diagnostic build (one KEY=VALUE per line)

BEAT = 60                               # quiet 步驟的進度間隔（秒） | progress interval of quiet steps (seconds)
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin', '/usr/local/share/dotnet', os.path.expanduser('~/.dotnet')]   # Spyder 等圖形程序拿不到 shell 的 PATH | GUI apps such as Spyder do not get the shell's PATH


class BuildError(Exception):
    pass


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def in_ipython():   # Spyder（與 Jupyter）在 IPython 裡跑本文件：輸入框可用，且不能 sys.exit（會把報錯堆在控制台上） | Spyder (and Jupyter) run this file inside IPython: input works there, and sys.exit must not be called (it piles an error onto the console)
    import builtins
    return hasattr(builtins, '__IPYTHON__')


def ask(prompt, zh, en):   # zh / en：缺的是什麼，報錯用 | zh / en: what is missing, for the error message
    if not in_ipython() and not (sys.stdin and sys.stdin.isatty()):
        raise BuildError(f'沒給{zh}，又沒有鍵盤輸入可問：在命令行上給出（-h 看用法）| {en} not given and there is no keyboard input to ask for it: give it on the command line (-h for usage)')
    s = input(prompt).strip().strip('"').strip("'").replace('\\ ', ' ')   # 拖進來的路徑帶引號或轉義空格 | dragged-in paths come quoted or with escaped spaces
    return os.path.expanduser(s)


def run(cmd, log, env=None, cwd=None, quiet=False):
    """跑一個子進程，全部輸出寫進 log；非零退出即停。quiet：控制台每分鐘報一次進度，結束時只打末尾幾行（著色器出包有上萬行編譯器警告）
    Run a subprocess with all its output written to log; stop on a non-zero exit. quiet: report progress to the console once a minute and print only the last lines at the end (the shader build emits tens of thousands of compiler warnings)"""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    tail, n, done = collections.deque(maxlen=40), [0], threading.Event()
    def beat():   # 子進程不出聲時也報（走緩存的著色器出包幾分鐘沒有輸出） | report even while the subprocess is silent (a cached shader build prints nothing for minutes)
        t0 = time.time()
        while not done.wait(BEAT): say(f'……仍在進行，{(time.time() - t0) / 60:.0f} 分鐘 | still running, {(time.time() - t0) / 60:.0f} min')
    p = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1)
    if quiet: threading.Thread(target=beat, daemon=True).start()
    try:
        with open(log, 'w') as f:
            for line in p.stdout:
                f.write(line); tail.append(line); n[0] += 1
                if not quiet: print(line, end='', flush=True)
        p.wait()
    except KeyboardInterrupt:
        p.terminate(); p.wait(); raise
    finally:
        done.set()
    if quiet: print(''.join(list(tail)[-40 if p.returncode else -12:]), end='', flush=True)
    if p.returncode:
        tool = Path(cmd[1] if cmd[0] in (sys.executable, 'bash') else cmd[0]).name
        raise BuildError(f'{tool} 退出碼 | exit code {p.returncode}（完整輸出 | full output: {log}）')


def build_id(nso):
    with open(nso, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def rmtree(p):
    if p.is_symlink() or p.is_file(): p.unlink()
    elif p.exists(): shutil.rmtree(p)


def build_nx_extract(repo, work, logs, env):
    """編譯 nx_extract 到 <work>/tools/nx_extract/。先編到臨時目錄再拷過去：輸出路徑帶撇號（如「Bowser's Fury」）時 MSBuild 的條件式被截斷，
    LibHac.dll 與可執行文件都不拷出來，編譯卻報成功
    Build nx_extract into <work>/tools/nx_extract/. It is built in a temporary folder and copied over: with an apostrophe in the output path (e.g. "Bowser's Fury") MSBuild's conditions break,
    LibHac.dll and the executable are not copied, and the build still reports success"""
    nx = work / 'tools/nx_extract/nx_extract'
    with tempfile.TemporaryDirectory(prefix='nx_extract_') as t:
        run(['dotnet', 'build', repo / 'dm/tools/nx_extract', '-c', 'Release', '-o', t, '-v', 'quiet', '-nologo'], logs / 'nx_extract_build.log', env=env)
        if not (Path(t) / 'nx_extract').exists(): raise BuildError(f'nx_extract 編譯後沒有可執行文件（見 {logs / "nx_extract_build.log"}）| nx_extract built without an executable (see {logs / "nx_extract_build.log"})')
        rmtree(nx.parent); shutil.copytree(t, nx.parent)
    return nx


def swap_in(tmp, dst):   # 新產物整個換上（失敗時舊的不受影響） | swap the new output in as a whole (the old one is untouched on failure)
    rmtree(dst); tmp.rename(dst)


def free_gb(p):
    while not p.exists(): p = p.parent
    return shutil.disk_usage(p).free / 2**30


# ---- NSP 識別 | NSP identification ----
def pfs0_names(path):   # NSP = PFS0：列出裡面的文件名 | NSP = PFS0: list the file names inside
    with open(path, 'rb') as f:
        h = f.read(16)
        if h[:4] != b'PFS0': return []
        n, st = struct.unpack_from('<II', h, 4)
        ent, strs = f.read(n * 0x18), f.read(st)
    return [strs[o:strs.index(b'\0', o)].decode(errors='replace') for o in (struct.unpack_from('<I', ent, i * 0x18 + 0x10)[0] for i in range(n))]


def title_of(path):   # 文件名裡的 [標題號]，沒有就取票據名（權利號前 16 位 = 標題號） | [title ID] in the file name, else the ticket name (the first 16 digits of the rights ID = title ID)
    m = re.search(r'\[([0-9A-Fa-f]{16})\]', path.name)
    if m: return int(m.group(1), 16)
    for n in pfs0_names(path):
        if re.match(r'[0-9A-Fa-f]{32}\.tik$', n): return int(n[:16], 16)
    return None


def version_of(path):
    m = re.search(r'\[v(\d+)\]', path.name)
    return int(m.group(1)) if m else None


def scan_nsps(nsp_dir):
    """按遊戲歸組：{應用標題號: {'base': 路徑, 'updates': [(版本, 路徑)], 'dlc': [(標題號, 版本, 路徑)]}}
    Group by game: {application title ID: {'base': path, 'updates': [(version, path)], 'dlc': [(title ID, version, path)]}}"""
    fams, unknown = {}, []
    for p in sorted(nsp_dir.glob('*.nsp')):
        if p.stat().st_size == 0:   # 下載不全留下的空文件（塞爾達傳說 曠野之息的文件夾裡有一個 0 字節的 DLC NSP，旁邊是完整的 (1).nsp）| an empty file left by an incomplete download (the Zelda BotW folder holds a 0-byte DLC NSP next to the complete (1).nsp)
            say(f'跳過空文件 | skipping an empty file: {p.name}'); continue
        tid = title_of(p)
        if tid is None: unknown.append(p); continue
        low = tid & 0xFFF   # 基礎版 …000，更新 …800，DLC = 基礎版 + 0x1000 + 序號 | base …000, update …800, DLC = base + 0x1000 + index
        app = tid if low == 0 else tid & ~0xFFF if low == 0x800 else (tid & ~0xFFF) - 0x1000
        f = fams.setdefault(app, {'base': None, 'updates': [], 'dlc': []})
        if low == 0: f['base'] = p
        elif low == 0x800: f['updates'].append((version_of(p), p))
        else: f['dlc'].append((tid, version_of(p), p))
    if len(fams) == 1 and len(unknown) == 1 and next(iter(fams.values()))['base'] is None:   # 卡帶轉出的基礎版可能既無標籤也無票據；構建號隨後核對 | a base game converted from a cartridge may have neither tags nor a ticket; build IDs are checked afterwards
        next(iter(fams.values()))['base'] = unknown[0]
    if not fams: raise BuildError(f'{nsp_dir} 裡沒有認得出標題號的 NSP | no NSP with a readable title ID in {nsp_dir}')
    if len(fams) > 1: raise BuildError('文件夾裡有不止一個遊戲，一個文件夾放一個遊戲 | more than one game in the folder, keep one game per folder: ' + ', '.join(f'{t:016X}' for t in fams))
    app, fam = next(iter(fams.items()))
    if fam['base'] is None: raise BuildError(f'{nsp_dir} 裡沒找到基礎版 NSP（標題號 {app:016X}）| no base game NSP found (title ID {app:016X})')
    return app, fam


def pick_nsps(fam, versions):
    """每個要的版本對應的（基礎版, 更新或 None）：v0 = 只基礎版，v<N> = 基礎版 + 版本號 N 的更新
    For each wanted version, (base game, update or None): v0 = the base game alone, v<N> = the base game + the update with version number N"""
    say(f'基礎版 | base: {fam["base"].name}')
    out = {}
    for ver in versions:
        if ver == 'v0': out[ver] = (fam['base'], None); continue
        n = int(ver[1:])
        upd = [u for v, u in fam['updates'] if v == n] or [u for v, u in fam['updates'] if v is None and len(fam['updates']) == 1]   # 文件名沒寫版本號時只認唯一的那個，構建號隨後核對 | without a version in the file name only a single update is accepted; build IDs are checked afterwards
        if not upd: raise BuildError(f'沒找到更新 {ver} 的 NSP（遊戲檔案要這些版本：{", ".join(versions)}）| no NSP for update {ver} (the game profile needs these versions: {", ".join(versions)})')
        say(f'更新 {ver} | update {ver}: {upd[0].name}'); out[ver] = (fam['base'], upd[0])
    return out


def can_clone(d):   # 該卷能否做 APFS 克隆（nx_extract 用它省空間，不支持時整份寫出） | whether this volume supports APFS clones (nx_extract uses them to save space; without them it writes full copies)
    a, b = d / '.clone_test', d / '.clone_test2'
    try:
        a.write_bytes(b'x'); rmtree(b)
        return ctypes.CDLL(None, use_errno=True).clonefile(str(a).encode(), str(b).encode(), 0) == 0
    finally:
        rmtree(a); rmtree(b)


# ---- 新遊戲粗查（沒有對應遊戲檔案時） | Survey of a new game (when no game profile matches) ----
MAGICS = [(b'UnityFS\0', 'UnityFS'), (b'UnityWeb', 'UnityWeb'), (b'UnityRaw', 'UnityRaw'), (b'BNSH', 'BNSH'), (b'BAHS', 'BAHS'), (b'SARC', 'SARC'), (b'Yaz0', 'Yaz0'),
          (bytes.fromhex('28b52ffd'), 'zstd'), (b'FRES', 'bfres'), (b'BNTX', 'BNTX'), (b'BWAV', 'BWAV'), (b'FSAR', 'BFSAR'), (b'CPK ', 'CRI CPK'), (b'CRID', 'CRI USM'),
          (b'@UTF', 'CRI UTF'), (b'AKPK', 'Wwise pck'), (b'BKHD', 'Wwise bnk'), (b'FSB5', 'FMOD'), (b'\x89PNG', 'PNG'), (b'RIFF', 'RIFF'), (b'OggS', 'Ogg'), (b'PK\x03\x04', 'zip'), (b'NSO0', 'NSO')]
MARKERS = [(b'nvnBootstrapLoader', 'NVN'), (b'vkCreateInstance', 'Vulkan'), (b'eglGetProcAddress', 'EGL / OpenGL'), (b'il2cpp_', 'Unity IL2CPP'), (b'UnityEngine', 'Unity'),
           (b'sead::', 'Nintendo sead'), (b'agl::', 'Nintendo agl'), (b'gsys::', 'Nintendo gsys'), (b'Unreal', 'Unreal'), (b'UE4', 'Unreal 4'), (b'Havok', 'Havok'), (b'CRIWARE', 'CRIWARE'), (b'Wwise', 'Wwise')]


def nso_image(path):   # 解出 NSO 的三段 | decompress an NSO's three segments
    import lz4.block
    d = path.read_bytes(); flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img), d[0x40:0x50].hex(), [sz for _, _, sz in segs]


def survey(nsp_dir, app, fam, keys, repo, py, env):
    work = nsp_dir / f'{app:016X}_survey'; logs, ex = work / 'logs', work / 'extract'
    say(f'沒有 {app:016X} 的遊戲檔案：只做粗查，不出 IPA | no game profile for {app:016X}: survey only, no IPA')
    nx = build_nx_extract(repo, work, logs, env)
    ups = sorted(fam['updates'], key=lambda u: u[0] or 0)
    upd = ups[-1][1] if ups else None
    tmp = work / 'extract.partial'; rmtree(tmp)
    say(f'解出代碼模塊與 RomFS 清單（不寫遊戲文件）| extracting the code modules and a RomFS manifest (no game files written) → {ex}')
    run([nx, keys, fam['base'], upd or '-', tmp, '--manifest'], logs / 'extract.log')
    swap_in(tmp, ex)
    xlog = (logs / 'extract.log').read_text(errors='replace')
    dv = re.search(r'display version (\S+)', xlog)
    R = [f'# 粗查 | Survey: {nacp.name(ex / "control.nacp") or "?"} ({app:016X})', '', f'- 顯示版本 | display version: {dv.group(1) if dv else "?"}', f'- 基礎版 | base: `{fam["base"].name}` ({fam["base"].stat().st_size / 2**30:.2f} GB)']
    R += [f'- 更新 | update: `{p.name}` (v{v}, {p.stat().st_size / 2**30:.2f} GB){"（粗查用的這一個 | used for this survey）" if p == upd else ""}' for v, p in ups]
    R += [f'- DLC: `{p.name}` ({p.stat().st_size / 1024:.0f} KB{"，只有授權、無內容 | licence only, no content" if p.stat().st_size < 1 << 20 else ""})' for _, _, p in fam['dlc']]

    # 代碼模塊 | Code modules
    R += ['', '## 代碼模塊 | Code modules', '', '| 模塊 module | 構建號 build ID | text / ro / data (MB) |', '|---|---|---|']
    found, sdkver, comps, unity, mods, glslc = {}, set(), set(), set(), [], 0
    for p in sorted((ex / 'ExeFS').iterdir()):
        if p.read_bytes()[:4] != b'NSO0': continue
        img, bid, szs = nso_image(p); mods.append(p)
        R.append(f'| {p.name} | `{bid}` | ' + ' / '.join(f'{z / 2**20:.1f}' for z in szs) + ' |')
        sdkver |= {m.decode() for m in re.findall(rb'(?i)NintendoSDK_nnSdk-([0-9_]+)', img)}   # SDK 12 寫作 NintendoSdk_，16 寫作 NintendoSDK_ | SDK 12 spells it NintendoSdk_, SDK 16 NintendoSDK_
        comps |= {m.decode() for m in re.findall(rb'SDK MW\+Nintendo\+([\w]+-[0-9_]+)', img)}
        unity |= {m.decode() for m in re.findall(rb'20[12][0-9]\.[0-9]+\.[0-9]+f[0-9]', img)} if p.name == 'main' else set()
        if p.name == 'main': glslc = img.count(b'\x91\x68\x86\x19')   # GLSLC 編譯輸出的魔數（shader_split.py glslc_shaders） | magic of GLSLC compiler outputs (shader_split.py glslc_shaders)
        for pat, name in MARKERS:
            if pat in img: found.setdefault(name, []).append(p.name)
    npdm = ex / 'ExeFS/main.npdm'
    if npdm.exists():
        d = npdm.read_bytes(); a = d.find(b'ACI0')
        if a >= 0: R.append(f'\n- main.npdm 程序號 | program ID: `{struct.unpack_from("<Q", d, a + 0x10)[0]:016X}`')
    R.append(f'- SDK: {", ".join(sorted(sdkver)) or "?"}（馬力歐 | Mario: 16_2_0 / 16_2_12；D2R ≈ 12）')
    R.append('- SDK 組件 | SDK components: ' + (', '.join(sorted(comps)) or '—'))
    if unity: R.append('- Unity: ' + ', '.join(sorted(unity)))
    R.append('- 引擎與接口標記 | engine and API markers: ' + ('; '.join(f'{k} ({", ".join(v)})' for k, v in found.items()) or '—'))
    extra = [m.name for m in mods if m.name not in ('main', 'sdk', 'rtld')]

    # RomFS 清單 | RomFS manifest
    tops, exts, mags, nfile, nbytes = collections.Counter(), collections.Counter(), collections.Counter(), 0, 0
    topb, magb = collections.Counter(), collections.Counter()
    for line in (ex / 'romfs_manifest.tsv').read_text(errors='replace').splitlines():
        if line.startswith('#'): continue
        path, size, head = line.split('\t'); size = int(size); h = bytes.fromhex(head)
        nfile += 1; nbytes += size
        parts = path.strip('/').split('/')
        top = '/'.join(parts[:2]) if len(parts) > 2 else parts[0] if len(parts) > 1 else '(根 | root)'   # 兩層：Unity 全在 Data/ 下 | two levels: Unity keeps everything under Data/
        tops[top] += 1; topb[top] += size
        exts[os.path.splitext(path)[1].lower() or '(無 | none)'] += 1
        mag = next((n for m, n in MAGICS if h.startswith(m)), None)
        if mag: mags[mag] += 1; magb[mag] += size
    R += ['', f'## RomFS: {nfile} 個文件 | files, {nbytes / 2**30:.2f} GB', '', '| 頂層 top level | 文件 files | GB |', '|---|---|---|']
    R += [f'| {t} | {n} | {topb[t] / 2**30:.2f} |' for t, n in tops.most_common(20)]
    R += ['', '| 文件頭 header magic | 文件 files | GB |', '|---|---|---|'] + [f'| {m} | {n} | {magb[m] / 2**30:.2f} |' for m, n in mags.most_common()]
    R += ['', '擴展名 | extensions: ' + ', '.join(f'`{e}` {n}' for e, n in exts.most_common(20))]

    # 改寫點統計與 SDK 接口 | Rewrite-site census and SDK surface
    R += ['', '## 改寫點統計 | Rewrite-site census (insn_census.py)', '', '```']
    for m in mods:
        if m.name == 'rtld': continue   # rtld 由宿主代替 | rtld is replaced by the host
        say(f'統計改寫點 | counting rewrite sites: {m.name}')
        run([py, repo / 'dm/tools/insn_census.py', m], logs / f'census_{m.name}.log', env=env, quiet=True)
        R += (logs / f'census_{m.name}.log').read_text(errors='replace').rstrip().splitlines()
    R += ['```', '', '## 導入與宿主鈎子 | Imports vs host hooks (nso_syms.py)', '', '```']
    run([py, repo / 'dm/tools/nso_syms.py', repo / 'dm/host', '-', ex / 'ExeFS'], logs / 'nso_syms.log', env=env, quiet=True)
    R += (logs / 'nso_syms.log').read_text(errors='replace').rstrip().splitlines() + ['```']

    # 對移植意味著什麼 | What it means for a port
    notes = []
    if 'NVN' not in found: notes.append('沒找到 NVN：宿主只實現了 NVN | no NVN found: the host implements only NVN')
    if extra: notes.append(f'除 main / sdk 外還有 {", ".join(extra)}：寫進檔案的 modules（裝載順序 main、subsdk*、sdk），改寫、打包與裝載都按它走（銀河戰士 生存恐懼、十三機兵防衛圈的 subsdk0 是 NintendoSDK_movie） | modules besides main / sdk: {", ".join(extra)}; list them in the profile\'s modules (load order main, subsdk*, sdk), which rewriting, packaging and loading all follow (subsdk0 of Metroid Dread and 13 Sentinels is NintendoSDK_movie)')
    if mags['UnityFS'] or 'Unity IL2CPP' in found: notes.append('Unity：著色器是 UnityFS 包與 data.unity3d 裡的 Shader 對象，shader_split.py 經 unityfs.py 讀出（按 Unity 2020.3 的 Switch 格式寫成，別的 Unity 版本先核對） | Unity: the shaders are Shader objects in UnityFS bundles and data.unity3d, which shader_split.py reads through unityfs.py (written for Unity 2020.3\'s Switch format; check other Unity versions first)')
    if glslc: notes.append(f'main 裡有 {glslc} 處 GLSLC 編譯輸出的魔數（0x19866891）：著色器可能編進了 main（十三機兵防衛圈），檔案開 shader_main_glslc 由 shader_split.py 讀出 | main has {glslc} GLSLC compiler output magics (0x19866891): the shaders may be compiled into main (13 Sentinels); the profile key shader_main_glslc makes shader_split.py read them')
    if not (mags['BNSH'] or mags['BAHS']) and not mags['UnityFS']: notes.append('頂層沒有 BNSH / BAHS：著色器在壓縮包、別的容器或 main 裡，要先找到 | no BNSH / BAHS at the top level: the shaders are inside compressed or other containers or in main, and must be located first')
    major = {v.split('_')[0] for v in sdkver}
    if not major: notes.append('沒找到 SDK 版本串 | no SDK version string found')
    elif major != {'16'}: notes.append(f'SDK {"/".join(sorted(major))}，不是馬力歐的 16：NVN 對象尺寸、API 版本與服務佈局按 SDK 不同（宿主已從 D2R 的 SDK 12 改成 16 的尺寸），按 docs/agents/03 逐項核對 | SDK {"/".join(sorted(major))}, not Mario\'s 16: NVN object sizes, API version and service layouts differ by SDK (the host was converted from D2R\'s SDK 12 to SDK 16 sizes); check them one by one per docs/agents/03')
    if any(l.split()[:1] == ['nn::ro'] for l in R): notes.append('導入 nn::ro（運行時裝 NRO 模塊）：宿主沒有 | imports nn::ro (loads NRO modules at run time): not in the host')
    if fam['dlc']: notes.append('有 DLC：出包時解出打進去，宿主按序號報告已擁有並掛載；查 main 導入的 aoc / fs 追加內容接口宿主都接了沒有 | DLC present: it is extracted into the IPA and the host reports the indices as owned and mounts them; check that the host hooks every aoc / fs add-on content call main imports')
    R += ['', '## 對移植意味著什麼 | What it means for a port', ''] + [f'- {n}' for n in notes or ['—']]
    R += ['', '下一步見 docs/agents（02 粗查、03 起機）。 | Next steps: docs/agents (02 survey, 03 bring-up).']
    (work / 'survey.md').write_text('\n'.join(R) + '\n')
    for n in notes: say(n)
    say(f'粗查報告 | survey report: {work / "survey.md"}')


# ---- 各步 | Steps ----
def check_prereqs(env, xlate):
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools') if not _importable(m)]
    if missing: raise BuildError(f'當前 Python（{sys.executable}）缺模塊 | the current Python is missing modules: {", ".join(missing)}（pip install lz4 capstone fonttools）')
    tools = {'xcodebuild': 'Xcode', 'xcodegen': 'brew install xcodegen', 'dotnet': '.NET SDK 10', 'zstd': 'brew install zstd', 'spirv-cross': 'brew install spirv-cross', 'zip': 'zip', 'make': 'Xcode'}
    gone = [f'{t}（{how}）' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if gone: raise BuildError('找不到工具 | tools not found: ' + '，'.join(gone))
    if not xlate.exists(): raise BuildError(f'倉內沒有著色器翻譯器 | the shader translator is missing from the repository: {xlate}')


def _importable(m):
    try: __import__(m); return True
    except ImportError: return False


def extract(nx, keys, ex, name, base, upd, cmp_dir, logs, g):
    dst, marker = ex / name, g.get('romfs_marker')
    if (dst / 'ExeFS' / 'main').exists() and (dst / 'RomFS' / marker).exists():
        say(f'沿用已解出的 {name} | reusing the extracted {name}: {dst}')
    else:
        tmp = ex / (name + '.partial'); rmtree(tmp)
        say(f'解包 {name} | extracting {name} → {dst}')
        run([nx, keys, base, upd or '-', tmp] + ([cmp_dir] if cmp_dir else []), logs / f'extract_{name}.log')
        check_build(tmp, name, g); swap_in(tmp, dst)
    check_build(dst, name, g)
    return dst


def extract_aoc(nx, keys, work, app, fam, logs):
    """文件夾裡的 DLC NSP → <名>_build/aoc/<序號>/（各自數據 NCA 的 RomFS；序號 = 標題號 − 基礎版 − 0x1000）。宿主按這些序號報告已擁有並掛載（DM_AOC_DIR）。
    沒有 DLC 返回 None。DLC 包很小（Engage 每個 121 KB，內容本體在更新裡），每次重解
    DLC NSPs in the folder → <id>_build/aoc/<index>/ (the RomFS of each one's data NCA; index = title ID − base − 0x1000). The host reports these indices as owned and mounts them (DM_AOC_DIR).
    Returns None without DLC. DLC packages are small (121 KB each for Engage, the content itself is in the update), so they are extracted again every time"""
    if not fam['dlc']: return None
    tmp = work / 'aoc.partial'; rmtree(tmp)
    best = {}   # 同一 DLC 有幾份時取版本最高、同版本取最大的那份 | when one DLC comes in several copies, take the highest version, and the largest of equal versions
    for tid, ver, p in fam['dlc']:
        k = (ver or 0, p.stat().st_size)
        if tid not in best or k > best[tid][0]: best[tid] = (k, p)
    for tid, (_, p) in sorted(best.items()):
        idx = tid - app - 0x1000
        say(f'DLC 序號 {idx} | DLC index {idx}: {p.name}')
        run([nx, keys, '--aoc', p, tmp / str(idx)], logs / f'aoc_{idx}.log')
    swap_in(tmp, work / 'aoc')
    return work / 'aoc'


def check_build(d, name, g):   # 每個模塊的構建號要是檔案 build 行登記的 | every module's build ID must be the one registered in the profile's build row
    want = g.builds()[name]
    for m, bid in want.items():
        if m == 'display': continue
        got = build_id(d / 'ExeFS' / m)
        if got != bid: raise BuildError(f'{m} 構建號 {got} 不是遊戲檔案 {g.id} 登記的 {name}（{bid}）| {m} build ID {got} is not the {name} registered in game profile {g.id} ({bid})')


def rewrite(py, repo, exe_dir, out, env, logs, g):
    tmp = out.with_name(out.name + '.partial'); rmtree(tmp); tmp.mkdir(parents=True)
    for m in g.words('modules'):
        say(f'改寫 {m} | rewriting {m}')
        run([py, repo / 'dm/tools/patch_nso.py', exe_dir / m, tmp, m], logs / f'rewrite_{m}.log', env=dict(env, DM_RULESET='native'))
    swap_in(tmp, out)


def data_rows(b):   # 表的數據行（去掉 # 注釋行：表頭隨 shader_split.py 的版本變，與遊戲文件無關） | a table's data rows (without # comment lines: the header changes with the shader_split.py version, not with the game files)
    return [l for l in b.splitlines() if not l.startswith(b'#')]


def split_shaders(py, repo, roms, mains, pkg, env, logs, g):
    tmp = pkg.with_name(pkg.name + '.partial'); rmtree(tmp); (tmp / 'tables').mkdir(parents=True)
    say('拆出著色器 | splitting out the shaders')
    names = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv')
    run([py, repo / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), tmp / 'pkg'] + [tmp / 'tables' / n for n in names], logs / 'shader_split.log', env=env)
    # 數據行與檔案裡的逐字節相同 = 遊戲文件與建表用的是同一份；不同就別往下出包（檔案裡的表不改）。只比數據行：Engage 檔案的表頭是舊版 shader_split 寫的
    # Data rows byte-identical to the profile's = the same game files the tables were built from; if not, stop (the profile's tables are left alone). Only data rows are compared: the header of Engage's profile tables was written by an older shader_split
    diff = [n for n in names if data_rows((tmp / 'tables' / n).read_bytes()) != data_rows(g.table(n).read_bytes())]
    if diff: raise BuildError(f'拆出的著色器表與遊戲檔案 {g.id} 的不同 | the split shader tables differ from game profile {g.id}\'s: {", ".join(diff)}（遊戲文件不是檔案登記的版本？| game files not the versions the profile registers?）')
    swap_in(tmp / 'pkg', pkg); rmtree(tmp)


def build_shaders(py, repo, sh, xlate, env, logs, g):
    say('出 iOS 著色器庫（首次要幾十分鐘，之後走緩存快很多）| building the iOS shader libraries (tens of minutes the first time, much faster with the cache)')
    ios = sh / 'ios'
    run([py, repo / 'dm/gfx/tools/dmsh_build.py', sh / 'pkg', sh / 'cache'], logs / 'dmsh_build.log', env=dict(env, XLATE=str(xlate), IOSOUT=str(ios)), quiet=True)
    chain = re.search(r'#define DMSH_CHAIN "(.*)"', g.table('dmsh_chain.h').read_text()).group(1)
    libs = ios / f'b-{chain}'
    n = sum(1 for _ in libs.glob('*_b.metallib')) if libs.is_dir() else 0
    if n == 0: raise BuildError(f'沒出 iOS 著色器庫 | no iOS shader libraries in {libs}')
    say(f'iOS 著色器庫 {n} 個 | {n} iOS shader libraries ({libs})')
    return ios


def find_repo(here):   # 本文件在倉庫的 scripts/ 下；往上找到有 dm/tools/nx_extract/Program.cs 的那一層 | this file lives in the repository's scripts/; walk up to the level that has dm/tools/nx_extract/Program.cs
    for d in here.parents:
        if (d / 'dm/tools/nx_extract/Program.cs').exists(): return d
    raise BuildError(f'從 {here} 往上找不到倉庫根目錄（沒有 dm/tools/nx_extract）| cannot find the repository root above {here} (no dm/tools/nx_extract)')


def parse_args(argv):
    """命令行選項：給了就蓋過「設置」；Spyder 裡 F5 不帶選項，全用「設置」。-h 看說明
    Command-line options: when given they override "Settings"; F5 in Spyder passes none, so "Settings" is used as is. -h shows the help"""
    import argparse
    ap = argparse.ArgumentParser(prog='build_ipa_from_nsp.py',
        description='從 NSP 出未簽名 IPA；沒有遊戲檔案的遊戲只寫粗查報告 | Build an unsigned IPA from a game\'s NSPs; a game with no profile only gets a survey.',
        epilog='例 | example: python3 scripts/build_ipa_from_nsp.py ~/nsp/"Tomodachi Life" -k ~/keys/prod.keys')
    ap.add_argument('nsp_dir', nargs='?', help='放一個遊戲 NSP 的文件夾（缺省 = 設置 NSP_DIR，再沒有就問）| folder holding one game\'s NSPs (default: the NSP_DIR setting, else asked)')
    ap.add_argument('-k', '--keys', help='prod.keys 的路徑（缺省 = 設置 PROD_KEYS，再沒有就問）| path of prod.keys (default: the PROD_KEYS setting, else asked)')
    ap.add_argument('-p', '--package', help='打進 IPA 的版本，如 v327680 / v0（缺省 = 遊戲檔案的 package）| version packed into the IPA, e.g. v327680 / v0 (default: the profile\'s package)')
    ap.add_argument('-o', '--out', help='輸出的 IPA（缺省 = NSP 文件夾裡的 <檔案的 ipa>）| output IPA (default: <the profile\'s ipa> in the NSP folder)')
    ap.add_argument('--app-name', help='主屏上的 App 名（缺省 = 基礎版 NSP 裡的遊戲名）| app name on the home screen (default: the game name in the base NSP)')
    ap.add_argument('--bundle-id', help='App 的包名（缺省 = org.nxnative.<基礎版標題號小寫>）| the app\'s bundle ID (default: org.nxnative.<base game title ID in lowercase>)')
    ap.add_argument('--keep-extracted', action='store_true', help='出包後保留解出的遊戲文件 | keep the extracted game files after the IPA is built')
    ap.add_argument('--env-file', help='診斷包的環境開關文件（每行 KEY=VALUE）| environment switches for a diagnostic build (one KEY=VALUE per line)')
    return ap.parse_args(argv)


def main(args=None):
    args = args or parse_args([])
    here = Path(globals().get('__file__') or sys.argv[0]).resolve()
    repo = find_repo(here)
    sys.path.insert(0, str(repo / 'dm/games')); import game as gamemod   # 遊戲檔案 | game profiles
    sys.path.insert(0, str(repo / 'dm/tools')); global nacp; import nacp   # NSP 控制數據裡的遊戲名 | the game name in the NSP's control data
    nsp_dir = Path(os.path.expanduser(args.nsp_dir or NSP_DIR or ask('NSP 文件夾 | NSP folder: ', ' NSP 文件夾', 'the NSP folder'))).resolve()
    keys = Path(os.path.expanduser(args.keys or PROD_KEYS or ask('prod.keys 路徑 | path of prod.keys: ', ' prod.keys（-k）', 'prod.keys (-k)'))).resolve()
    if not nsp_dir.is_dir(): raise BuildError(f'不是文件夾 | not a folder: {nsp_dir}')
    if not keys.is_file(): raise BuildError(f'沒有這個文件 | no such file: {keys}')
    env = dict(os.environ, PATH=os.pathsep.join(TOOL_PATHS + [os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')]))
    env.setdefault('USER', getpass.getuser())   # xcodegen 沒有 USER 就失敗（Couldn't find current username） | xcodegen fails without USER (Couldn't find current username)
    py = sys.executable
    app, fam = scan_nsps(nsp_dir)
    g = gamemod.by_title(app)
    if g is None:   # 沒有遊戲檔案：只粗查 | no game profile: survey only
        missing = [m for m in ('lz4.block', 'capstone') if not _importable(m)]
        if missing: raise BuildError(f'當前 Python（{sys.executable}）缺模塊 | the current Python is missing modules: {", ".join(missing)}')
        if not shutil.which('dotnet', path=env['PATH']): raise BuildError('找不到 dotnet（.NET SDK 10）| dotnet not found (.NET SDK 10)')
        return survey(nsp_dir, app, fam, keys, repo, py, env)
    say(f'遊戲檔案 | game profile: {g.id}')
    builds, need = g.builds(), g.words('shader_versions')
    pkg_ver = args.package or PACKAGE or g.get('package')
    if pkg_ver not in builds: raise BuildError(f"打進 IPA 的版本（-p / PACKAGE）只能是遊戲檔案登記的 | the version to pack (-p / PACKAGE) must be one the game profile registers: {', '.join(builds)}")
    versions = need + ([pkg_ver] if pkg_ver not in need else [])
    out_ipa = Path(os.path.expanduser(args.out or OUT_IPA)).resolve() if (args.out or OUT_IPA) else nsp_dir / g.get('ipa')   # 缺省放在 NSP 文件夾裡，與 NSP 和 <名>_build/ 同處 | by default in the NSP folder, next to the NSPs and <id>_build/
    say(f'輸出 IPA | output IPA: {out_ipa}')
    work = nsp_dir / f'{g.id}_build'
    say(f'工作目錄（解出的遊戲文件在 extract/，出包後刪掉，--keep-extracted 保留）| work folder (extracted game files in extract/, deleted after the IPA is built unless --keep-extracted): {work}')
    xlate = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    # DM_GAME 選檔案；DM_ROOT / WORK 指到 <名>_build：各工具的缺省路徑與臨時目錄都不出它 | DM_GAME picks the profile; DM_ROOT / WORK point at <id>_build: the tools' default paths and temporary directories all stay inside it
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'))
    logs = work / 'logs'
    check_prereqs(env, xlate)
    picks = pick_nsps(fam, versions)

    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    gb = sum({p.stat().st_size for b, u in picks.values() for p in (b, u) if p}) / 2**30 + 6   # 解包 + 著色器緩存 + 餘量 | extraction + shader cache + slack
    if free_gb(work) < gb and not all((ex / v).exists() for v in versions): raise BuildError(f'{work} 所在的卷剩餘不足 {gb:.0f} GB | less than {gb:.0f} GB free on the volume of {work}')
    say('編譯 nx_extract | building nx_extract')
    nx = build_nx_extract(repo, work, logs, env)
    dirs, clone = {}, None
    for ver in versions:   # 基礎版先單獨解，後面的更新拿它的 RomFS 做克隆比對 | the base game is extracted alone first; later updates clone against its RomFS
        b, u = picks[ver]
        dirs[ver] = extract(nx, keys, ex, ver, b, u, clone, logs, g)
        if u is None and clone is None and can_clone(ex): clone = dirs[ver] / 'RomFS'
    game = dirs[pkg_ver]
    aoc = extract_aoc(nx, keys, work, app, fam, logs)
    base = next((dirs[v] for v in versions if picks[v][1] is None), dirs[versions[0]])   # 基礎版單獨解出的目錄 | the folder the base game was extracted to alone
    # App 名與包名都取自基礎版 NSP：名字是它控制數據裡的遊戲名，包名按它的標題號；設置 / 命令行可改 | the app name and bundle ID both come from the base NSP: the game name in its control data, and a bundle ID from its title ID; a setting / the command line overrides them
    app_name = args.app_name or APP_NAME or nacp.name(base / 'control.nacp')
    if not app_name: raise BuildError(f'基礎版解包裡讀不出遊戲名（{base / "control.nacp"}；舊的解包沒存它：刪掉 extract/ 重跑，或用 --app-name 指定）| no game name in the base extraction ({base / "control.nacp"}; an older extraction did not save it: delete extract/ and run again, or give --app-name)')
    bundle_id = args.bundle_id or BUNDLE_ID or f'org.nxnative.{app:016x}'
    say(f'App 名 | app name: {app_name}；包名 | bundle ID: {bundle_id}')

    patched = work / f'patched_{pkg_ver}'
    rewrite(py, repo, game / 'ExeFS', patched, env, logs, g)
    sh = work / 'shader'
    split_shaders(py, repo, [dirs[v] / 'RomFS' for v in need], [dirs[v] / 'ExeFS/main' for v in need], sh / 'pkg', env, logs, g)
    ios = build_shaders(py, repo, sh, xlate, env, logs, g)

    if free_gb(out_ipa.parent) < 5: raise BuildError(f'{out_ipa.parent} 所在的卷剩餘不足 5 GB | less than 5 GB free on the volume of {out_ipa.parent}')
    say(f'出 IPA | building the IPA → {out_ipa}')
    ipa_env = dict(env, GAME=g.id, W=str(work / 'ipa'), PATCHED=str(patched), ROMFS=str(game / 'RomFS'), MSL_IOS=str(ios), FONT_PY=py, DM_DISPLAY_NAME=app_name, DM_BUNDLE_ID=bundle_id, AOC=str(aoc or ''))
    # App 圖標：nx_extract 從控制數據存下的遊戲圖標（英文版，icon.jpg）；打包版本的優先，其次基礎版 | app icon: the game icon nx_extract saved from the control data (English, icon.jpg); the packed version's first, then the base game's
    icon = next((d / 'icon.jpg' for d in (game, base) if (d / 'icon.jpg').exists()), None)
    if icon: ipa_env['DM_ICON'] = str(icon); say(f'App 圖標 | app icon: {icon}')
    else: say('【紅】解包裡沒有遊戲圖標（之前的解包沒存圖標：刪掉 extract/ 重跑即可補上）；主屏圖標會是空白 | no game icon in the extraction (an older extraction did not save it: delete extract/ and run again); the home-screen icon will be blank')
    env_file = args.env_file or DM_ENV_FILE
    if env_file: ipa_env['DM_ENV_FILE'] = str(Path(os.path.expanduser(env_file)).resolve())
    run(['bash', repo / 'dm/ios/tools/build_ipa.sh', out_ipa], logs / 'build_ipa.log', env=ipa_env)
    if not out_ipa.is_file(): raise BuildError(f'沒出 IPA | no IPA at {out_ipa}')
    rmtree(work / 'tmp')
    if not (args.keep_extracted or KEEP_EXTRACTED):
        say(f'刪掉解出的遊戲文件 | deleting the extracted game files: {ex}'); rmtree(ex)
    say(f'完成 | done: {out_ipa}（{out_ipa.stat().st_size / 2**30:.1f} GB）')


def cli(argv):   # 返回退出碼：0 完成，1 出錯，130 中斷 | returns the exit code: 0 done, 1 error, 130 interrupted
    try:
        main(parse_args(argv))
    except BuildError as e:
        print(f'【紅】{e}', flush=True); return 1
    except KeyboardInterrupt:
        print('【紅】中斷 | interrupted', flush=True); return 130
    return 0


if __name__ == '__main__':
    code = cli(sys.argv[1:])
    if not in_ipython(): sys.exit(code)   # 終端裡給出退出碼；Spyder 裡不退（控制台保持乾淨） | give an exit code in a terminal; do not exit in Spyder (keeps the console clean)
