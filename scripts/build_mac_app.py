#!/usr/bin/env python3
# 把已經起機好的遊戲工作區打成一個 Mac App（.app）：雙擊開窗口玩、有遊戲自己的圖標、能拖進「應用程序」。
#   App 裡只有：宿主（按遊戲檔案編）、啓動器、遊戲檔案的表、遊戲圖標（約 2 MB）。遊戲資料（改寫後的代碼模塊、Mac 著色器庫（只要 .metallib 與 .json）、字體、RomFS）在 App 外面：
#   缺省指向工作區（玩時那顆盤要接著）；--embed 時整份放到本機 ~/Library/Application Support/nxnative/<標題號>/game/（同一個 APFS 卷上用克隆（cp -c），幾乎不佔額外空間；跨卷照常拷，幾 GB），拔掉放工作區的那顆盤也能玩。
#   資料不放進 App 包：macOS 每次在包改動後第一次打開時，Gatekeeper / XProtect 與 TCC 會把整包掃一遍，幾 GB 的包要等一分多鐘才輪到遊戲開跑（這段沒畫面也沒聲音）。
#   存檔、鍵位表（keymap.txt）放 ~/Library/Application Support/nxnative/<標題號>/，日誌寫 ~/Library/Logs/nxnative/<標題號>.log。
#   遊戲語言缺省跟 Mac 的首選語言（遊戲沒有的語言用美式英文）；想固定一種：打包時 --lang，或 defaults write <App 的 bundle id> GameLanguage en-US（優先於打包時的值）。
# 兩種跑法：在 Spyder 裡打開本文件按 F5（用下面「設置」裡的值）；或在終端裡 python3 scripts/build_mac_app.py <遊戲標題號> [選項]
#   （命令行給的值蓋過「設置」，-h 看全部選項；成功退出碼 0，出錯 1）。需要 Xcode 命令行工具（clang、sips、iconutil、codesign）。
# 工作區（缺省 ~/<標題號>-work）按起機時的佈局：data/<版本>/RomFS 與 icon.jpg、build/patched_<版本>/、build/cache/metal_shader_cache/b-<鏈版本>/、build/font/。
# 遊戲文件只在工作區和本機的 Application Support 裡、密鑰只在工作區，都不進倉庫；App 只給自己用，不要分發。
# Packs an already brought-up game workspace into a Mac app (.app): double-click to play in a window, with the game's own icon, ready to drag into Applications.
#   The app holds only the host (built for the game profile), the launcher, the profile's tables and the game icon (about 2 MB). The game data (the rewritten code modules, the Mac shader libraries (only .metallib and .json), the fonts, the RomFS) stays outside the app:
#   by default it points at the workspace (keep that disk attached to play); with --embed all of it goes to ~/Library/Application Support/nxnative/<title ID>/game/ on this Mac (as clones (cp -c) on the same APFS volume, almost no extra space; a normal copy across volumes, a few GB), playable with the workspace disk unplugged.
#   The data stays out of the app bundle: the first time a changed bundle opens, Gatekeeper / XProtect and TCC scan all of it, and a bundle of several GB keeps the game from starting for over a minute (no picture and no sound meanwhile).
#   Saves and the key table (keymap.txt) live in ~/Library/Application Support/nxnative/<title ID>/, the log goes to ~/Library/Logs/nxnative/<title ID>.log.
#   The game language follows the Mac's preferred language by default (US English when the game lacks it); to pin one: --lang when packing, or defaults write <the app's bundle id> GameLanguage en-US (wins over the packed value).
# Two ways to run it: open this file in Spyder and press F5 (it uses the values under "Settings" below); or in a terminal: python3 scripts/build_mac_app.py <game title ID> [options]
#   (command-line values override "Settings", -h lists every option; exit code 0 on success, 1 on error). Needs the Xcode command-line tools (clang, sips, iconutil, codesign).
# The workspace (default ~/<title ID>-work) follows the bring-up layout: data/<version>/RomFS and icon.jpg, build/patched_<version>/, build/cache/metal_shader_cache/b-<chain version>/, build/font/.
# Game files stay in the workspace and Application Support on this Mac, keys only in the workspace, never in the repository; the app is for your own use, do not distribute it.
import argparse, os, plistlib, re, shutil, subprocess, sys, tempfile
from pathlib import Path

# ---- 設置 | Settings ----
GAME = ''                 # 遊戲檔案（基礎版標題號，如 010051F0207B2000） | game profile (base game title ID, e.g. 010051F0207B2000)
WORKSPACE = ''            # 工作區（留空 = ~/<標題號>-work） | workspace (empty = ~/<title ID>-work)
OUT_APP = ''              # 輸出的 App（留空 = 工作區裡的 <檔案 ipa 名>.app） | output app (empty = <the profile's ipa name>.app in the workspace)
APP_NAME = ''             # Dock / 訪達裡的名字（留空 = 檔案的 name） | name in the Dock / Finder (empty = the profile's name)
LANG = ''                 # 遊戲語言（DM_LANG：ja / en-US / en-GB / fr / fr-CA / es / es-419 / de / it / nl / ko / zh-Hans / zh-Hant）；留空 = 跟系統首選語言 | game language (DM_LANG: ...); empty = follow the system's preferred language
OPMODE = 1                # 1 主機模式（1080p）/ 0 掌機模式（720p） | 1 docked (1080p) / 0 handheld (720p)
EMBED = False             # False：遊戲資料指向工作區（玩時那顆盤要接著）；True：資料整份放到本機 Application Support/nxnative/<標題號>/game/（約 7 GB；同卷克隆不佔額外空間）。兩種 App 都約 2 MB
                          # False: game data points at the workspace (keep that disk attached to play); True: all the data goes to Application Support/nxnative/<title ID>/game/ on this Mac (about 7 GB; clones on the same volume take no extra space). Either way the app is about 2 MB
SIGN = True               # ad-hoc 簽名宿主執行檔（不用證書，只為本機能跑） | ad-hoc sign the host executable (no certificate, only so it runs on this Mac)

REPO = Path(__file__).resolve().parent.parent


class BuildError(Exception):
    pass


def say(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode: raise BuildError(f'命令失敗 | command failed: {" ".join(map(str, cmd))}\n{r.stdout[-2000:]}{r.stderr[-2000:]}')
    return r.stdout


def profile(game, key):
    return run([sys.executable, REPO / 'dm/games/game.py', game, key]).strip()


def same_volume(a, b):
    return os.stat(a).st_dev == os.stat(b).st_dev


def clone(src, dst, skip=()):
    """整棵一次放：同卷用 APFS 克隆（cp -cR），跨卷照常拷；再刪掉 skip 列的擴展名（刪克隆幾乎不花時間）
    Place a whole tree at once: APFS clone (cp -cR) on the same volume, a normal copy across volumes; then delete the extensions listed in skip (deleting clones costs almost nothing)"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    run(['cp', '-cR' if same_volume(src, dst.parent) else '-R', src, dst])
    if skip:
        for root, _, files in os.walk(dst):
            for f in files:
                if os.path.splitext(f)[1] in skip: os.remove(os.path.join(root, f))


# 圓角圖標（macOS 的圖標網格：1024 畫布裡 824 見方、圓角 185.4、下方一層淺陰影）：用系統自帶的 JavaScript for Automation 調 AppKit 畫，不另裝東西
# Rounded icon (the macOS icon grid: 824 square on a 1024 canvas, corner radius 185.4, a light shadow below): drawn through AppKit from the system's JavaScript for Automation, nothing extra to install
ROUND_JS = r"""
ObjC.import('AppKit');
function run(argv) {
  const src = $.NSImage.alloc.initWithContentsOfFile(argv[0]);
  if (src.isNil()) throw new Error('cannot read ' + argv[0]);
  const S = 1024, side = 824, inset = (S - side) / 2, r = 185.4;
  const rep = $.NSBitmapImageRep.alloc.initWithBitmapDataPlanesPixelsWidePixelsHighBitsPerSampleSamplesPerPixelHasAlphaIsPlanarColorSpaceNameBytesPerRowBitsPerPixel(null, S, S, 8, 4, true, false, 'NSDeviceRGBColorSpace', 0, 0);
  const ctx = $.NSGraphicsContext.graphicsContextWithBitmapImageRep(rep);
  $.NSGraphicsContext.saveGraphicsState;
  $.NSGraphicsContext.setCurrentContext(ctx);
  const rect = $.NSMakeRect(inset, inset + 6, side, side);
  const path = $.NSBezierPath.bezierPathWithRoundedRectXRadiusYRadius(rect, r, r);
  ctx.saveGraphicsState;
  const sh = $.NSShadow.alloc.init;
  sh.setShadowColor($.NSColor.colorWithWhiteAlpha(0, 0.28)); sh.setShadowBlurRadius(18); sh.setShadowOffset($.NSMakeSize(0, -8)); sh.set;
  $.NSColor.whiteColor.setFill; path.fill;
  ctx.restoreGraphicsState;
  path.addClip;
  src.drawInRectFromRectOperationFraction(rect, $.NSMakeRect(0, 0, 0, 0), 2, 1);
  ctx.flushGraphics;
  $.NSGraphicsContext.restoreGraphicsState;
  const png = rep.representationUsingTypeProperties(4, $.NSDictionary.dictionary);
  if (!png.writeToFileAtomically(argv[1], true)) throw new Error('cannot write ' + argv[1]);
}
"""


def make_icns(jpg, icns):
    """遊戲圖標（256×256 JPEG）→ 圓角 .icns：JXA 畫 1024 的圓角圖，sips 出各尺寸 PNG，iconutil 打包
    game icon (256×256 JPEG) → rounded .icns: JXA draws the 1024 rounded image, sips makes the PNG sizes, iconutil packs them"""
    with tempfile.TemporaryDirectory() as td:
        js, big = Path(td) / 'round.js', Path(td) / 'round.png'
        js.write_text(ROUND_JS)
        run(['osascript', '-l', 'JavaScript', js, jpg, big])
        ics = Path(td) / 'AppIcon.iconset'; ics.mkdir()
        for s in (16, 32, 128, 256, 512):
            for k, name in ((1, f'icon_{s}x{s}.png'), (2, f'icon_{s}x{s}@2x.png')):
                run(['sips', '-z', s * k, s * k, big, '--out', ics / name])
        run(['iconutil', '-c', 'icns', ics, '-o', icns])


LAUNCHER = '''#!/bin/bash
# {name}：設好路徑與語言後啓動宿主（由 scripts/build_mac_app.py 生成） | {name}: set the paths and language, then start the host (generated by scripts/build_mac_app.py)
C="$(cd "$(dirname "$0")/.." && pwd)"
SUP="$HOME/Library/Application Support/nxnative/{game}"; LOGD="$HOME/Library/Logs/nxnative"
G="{data}"; [ -n "$G" ] || G="$SUP/game"
mkdir -p "$SUP/save" "$SUP/temp" "$LOGD"
[ -f "$SUP/keymap.txt" ] || cp "$C/Resources/keymap.txt" "$SUP/keymap.txt"
# 語言：defaults 裡的 GameLanguage > 打包時定的 > Mac 首選語言（遊戲有的 13 種之一，沒有的用美式英文） | language: GameLanguage in defaults > the packed value > the Mac's preferred language (one of the game's 13, US English otherwise)
L="$(defaults read {bundle_id} GameLanguage 2>/dev/null)"; [ -n "$L" ] || L="{lang}"
if [ -z "$L" ]; then
  case "$(defaults read -g AppleLanguages 2>/dev/null | sed -n '2{{s/[[:space:]",]//g;p;}}')" in
    zh-Hant*|zh-TW*|zh-HK*|zh-MO*|yue*) L=zh-Hant;; zh*) L=zh-Hans;; ja*) L=ja;; ko*) L=ko;;
    en-GB*|en-AU*|en-NZ*|en-IE*|en-IN*|en-ZA*) L=en-GB;; fr-CA*) L=fr-CA;; fr*) L=fr;;
    es-ES*|es) L=es;; es*) L=es-419;; de*) L=de;; it*) L=it;; nl*) L=nl;; *) L=en-US;;
  esac
fi
# 幀率：defaults 裡的 FrameRate 是 60 就開可選剪口 fps60（遊戲沒有這個剪口時宿主略過） | frame rate: FrameRate 60 in defaults turns on the optional cut fps60 (the host skips it when the game has no such cut)
O=""; [ "$(defaults read {bundle_id} FrameRate 2>/dev/null)" = 60 ] && O=fps60
if [ ! -d "$G/{romfs}/" ]; then
  case "$L" in zh*) M="找不到遊戲資料（RomFS）：請接上放遊戲的那顆磁碟。";; *) M="The game data (RomFS) is missing. Connect the disk that holds the game.";; esac
  osascript -e "display alert \\"{name}\\" message \\"$M\\"" ; exit 1
fi
exec env DM_NVN_SHMAP="$C/Resources/profile/shader_map.tsv" DM_ROOT="$SUP" DM_ROMFS="$G/{romfs}" DM_SAVE="$SUP/save" DM_TEMP="$SUP/temp" DM_MSL_DIR="$G/{msl}" DM_FONT_DIR="$G/{font}" \\
    DM_LANG="$L" DM_OPTIONS="$O" DM_OPMODE={opmode} DM_WINDOW=1 DM_AUDIO=1 "$C/MacOS/dmhost" "$G/{patched}" >"$LOGD/{game}.log" 2>&1
'''


def build(game, ws, out, name, lang, opmode, sign, embed, keep_data=False):
    game = game.upper()
    if not re.fullmatch(r'[0-9A-F]{16}', game): raise BuildError(f'標題號不對 | not a title ID: {game}')
    ws = Path(ws or Path.home() / f'{game}-work').expanduser()
    pkg = profile(game, 'package'); name = name or profile(game, 'name')
    chain = re.search(r'#define DMSH_CHAIN "([0-9a-f]+)"', (REPO / f'dm/games/{game}/dmsh_chain.h').read_text()).group(1)
    src = {'RomFS': ws / f'data/{pkg}/RomFS', 'icon': ws / f'data/{pkg}/icon.jpg', 'patched': ws / f'build/patched_{pkg}',
           'msl': ws / f'build/cache/metal_shader_cache/b-{chain}', 'font': ws / 'build/font'}
    for k, p in src.items():
        if not p.exists(): raise BuildError(f'工作區缺 {k} | the workspace lacks {k}: {p}')
    short = name.split(':')[0].strip() or name   # 冒號前那段當檔名與選單名（檔名不能有冒號），完整名稱經在地化顯示名給訪達與 Dock | the part before the colon is the file and menu name (file names cannot hold a colon); the full name reaches Finder and the Dock as the localized display name
    out = Path(out or ws / f'{short}.app').expanduser()
    say(f'遊戲 | game {game}「{name}」 版本 | version {pkg} 著色器鏈 | shader chain {chain}\n輸出 | output {out}')

    say('編宿主 | building the host')
    run(['make', '-s', '-C', REPO / 'dm/host', f'GAME={game}'])
    host = REPO / 'dm/host/dmhost'

    # --embed 的資料放在 App 外面（見文件開頭），存檔旁邊的 game/；重打 App 不動它，只換跟程式碼一起變的部分
    # --embed data lives outside the app (see the top of this file), in game/ next to the saves; rebuilding the app leaves it in place and only replaces the parts that change with the code
    g = Path.home() / f'Library/Application Support/nxnative/{game}/game'
    if embed:
        g.mkdir(parents=True, exist_ok=True)
        old = out / 'Contents/Resources/game/RomFS'   # 舊版打包把 RomFS 放在 App 裡 | older builds put the RomFS inside the app
        keep = keep_data and (g / 'RomFS').is_dir()
        for k in ('patched', 'msl', 'font') + (() if keep else ('RomFS',)):
            if (g / k).exists(): shutil.rmtree(g / k)
        # --keep-data：沿用已有的 RomFS（幾 GB、跟程式碼無關）：先找 game/ 裡的，再找舊 App 裡的（同卷直接搬出來）
        # --keep-data: reuse the RomFS already there (gigabytes, independent of the code): first the one in game/, then the one inside an old app (moved out directly on the same volume)
        if keep: say('沿用已放好的 RomFS | reusing the RomFS already in place')
        elif keep_data and old.is_dir() and same_volume(old, g):
            old.rename(g / 'RomFS'); say('把舊 App 裡的 RomFS 搬出來 | moving the RomFS out of the old app')
        else:
            if not same_volume(src['RomFS'], g): say('跨卷：RomFS 整份拷過來（幾 GB，要幾分鐘）| another volume: copying the whole RomFS (a few GB, takes minutes)')
            clone(src['RomFS'], g / 'RomFS')
        say(f'放遊戲資料（同卷克隆、跨卷拷貝）| placing the game data (clones on the same volume, copies across volumes): {g}')
        clone(src['patched'], g / 'patched')
        clone(src['msl'], g / 'msl' / f'b-{chain}', skip=('.metal',))
        clone(src['font'], g / 'font')
        paths = dict(data='', romfs='RomFS', msl='msl', font='font', patched='patched')
    else:   # 指向工作區：路徑相對工作區寫進啓動器 | point at the workspace: paths relative to it are written into the launcher
        paths = dict(data=str(ws), romfs=src['RomFS'].relative_to(ws), msl=src['msl'].parent.relative_to(ws), font=src['font'].relative_to(ws), patched=src['patched'].relative_to(ws))

    if out.exists(): shutil.rmtree(out)
    c = out / 'Contents'
    (c / 'MacOS').mkdir(parents=True); (c / 'Resources').mkdir()
    shutil.copy2(host, c / 'MacOS/dmhost')
    exe = c / 'MacOS' / re.sub(r'[^A-Za-z0-9]', '', Path(profile(game, 'ipa')).stem)
    # 遊戲檔案的表（著色器映射、c1 清單、預熱清單等，都在 shader_map.tsv 同目錄）：宿主默認按執行檔位置找倉裡的 dm/games/<名>/，進了 App 就找不到，改由 DM_NVN_SHMAP 指過來
    # The profile's tables (shader map, c1 list, prewarm lists etc., all in the same folder as shader_map.tsv): the host looks for the repository's dm/games/<id>/ relative to its executable, which fails inside an app, so DM_NVN_SHMAP points here instead
    (c / 'Resources/profile').mkdir()
    for t in (REPO / f'dm/games/{game}').glob('*.tsv'): shutil.copy2(t, c / 'Resources/profile' / t.name)
    shutil.copy2(REPO / 'dm/host/keymap_template.txt', c / 'Resources/keymap.txt')   # 首次開啟時複製到 Application Support | copied to Application Support on first launch
    bundle_id = f'org.nxnative.{game.lower()}.mac'
    exe.write_text(LAUNCHER.format(name=name, game=game, lang=lang, opmode=opmode, bundle_id=bundle_id, **paths)); exe.chmod(0o755)
    make_icns(src['icon'], c / 'Resources/AppIcon.icns')
    try: category = profile(game, 'app_category') or 'public.app-category.games'
    except BuildError: category = 'public.app-category.games'   # 檔案沒寫 | the profile does not set it
    # 訪達只在檔名與 Info.plist 的 CFBundleDisplayName 相同時才改顯示在地化名稱，所以這裡填檔名，完整名稱放進 InfoPlist.strings
    # Finder shows the localized name only when the file name equals CFBundleDisplayName in Info.plist, so the file name goes here and the full name into InfoPlist.strings
    localized = name != out.stem
    for lp in ('en', 'zh-Hant'):
        (c / f'Resources/{lp}.lproj').mkdir()
        (c / f'Resources/{lp}.lproj/InfoPlist.strings').write_text(f'"CFBundleDisplayName" = "{name}";\n"CFBundleName" = "{short}";\n', encoding='utf-8')
    plistlib.dump({
        'CFBundleName': short, 'CFBundleDisplayName': out.stem, 'LSHasLocalizedDisplayName': localized, 'CFBundleExecutable': exe.name,
        'CFBundleIdentifier': bundle_id, 'CFBundlePackageType': 'APPL',
        # 菜單文字自帶英文與繁體中文兩套（front_mac.m），系統給的菜單項（全屏、聽寫等）跟著用同一種 | the menus carry English and Traditional Chinese text (front_mac.m); system-supplied items (full screen, dictation...) follow the same language
        'CFBundleDevelopmentRegion': 'en', 'CFBundleLocalizations': ['en', 'zh-Hant'], 'CFBundleAllowMixedLocalizations': True,
        'CFBundleShortVersionString': '1.0', 'CFBundleVersion': '1', 'CFBundleIconFile': 'AppIcon',
        'LSApplicationCategoryType': category,
        'LSMinimumSystemVersion': '26.0', 'NSHighResolutionCapable': True,
    }, open(c / 'Info.plist', 'wb'))
    if sign:   # 只簽宿主執行檔：本機自用，不需要整包封簽 | sign only the host executable: for use on this Mac, sealing the whole bundle is not needed
        say('ad-hoc 簽名宿主 | ad-hoc signing the host')
        run(['codesign', '--force', '--sign', '-', c / 'MacOS/dmhost'])
    say(f'完成 | done: {out}')
    return out


def main(argv):
    ap = argparse.ArgumentParser(description='把遊戲工作區打成 Mac App | pack a game workspace into a Mac app')
    ap.add_argument('game', nargs='?', default=GAME, help='基礎版標題號 | base game title ID')
    ap.add_argument('-w', '--workspace', default=WORKSPACE, help='工作區 | workspace')
    ap.add_argument('-o', '--out', default=OUT_APP, help='輸出的 .app | output .app')
    ap.add_argument('-n', '--name', default=APP_NAME, help='App 名 | app name')
    ap.add_argument('--lang', default=LANG, help='遊戲語言（留空 = 跟系統）| game language (empty = follow the system)')
    ap.add_argument('--opmode', type=int, default=OPMODE, choices=(0, 1), help='1 主機 / 0 掌機 | 1 docked / 0 handheld')
    ap.add_argument('--no-sign', dest='sign', action='store_false', default=SIGN, help='不簽名 | do not sign')
    ap.add_argument('--embed', action='store_true', default=EMBED, help='遊戲資料整份放到本機 Application Support（不再需要工作區那顆盤）| place all the game data in Application Support on this Mac (the workspace disk is no longer needed)')
    ap.add_argument('--keep-data', action='store_true', help='與 --embed 並用：沿用已放好的 RomFS（或把舊 App 裡的搬出來），不再複製 | with --embed: reuse the RomFS already in place (or move it out of an old app) instead of copying it again')
    a = ap.parse_args(argv)
    if not a.game: raise BuildError('沒給遊戲標題號（-h 看用法）| no game title ID given (see -h)')
    build(a.game, a.workspace, a.out, a.name, a.lang, a.opmode, a.sign, a.embed, a.keep_data)


if __name__ == '__main__':
    try: main(sys.argv[1:])
    except BuildError as e:
        print(f'【紅】{e}', file=sys.stderr)
        if 'IPython' not in sys.modules: sys.exit(1)
