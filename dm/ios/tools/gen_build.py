# 從 dm/host/Makefile 實際展開的編譯命令（make -n -B dmhost，含它依賴的庫的逐文件編譯）取宿主的源文件 / 頭文件路徑 / 框架，生成 iOS 工程的構建輸入，
# 不手抄列表：Makefile 一改，iOS 下次構建自動跟上。
#   Gen/src/<扁平名>   源文件符號鏈接（排除 main.c 與 front_mac.m；main.c 另鏈到 Gen/main/main.c，以 -Dmain=dm_main 編）
#   stdout            xcconfig 行：HEADER_SEARCH_PATHS（-I 目錄 + 每個源文件所在目錄，保證相對 include 照常解析）、
#                     OTHER_CFLAGS（-D）、框架（Cocoa 換成 UIKit 側已有，跳過）
# 用法：gen_build.py <dm/host 目錄> <Gen 目錄>
# Take the host's source files / header paths / frameworks from the actually expanded compile commands of dm/host/Makefile (make -n -B dmhost, including the per-file compiles of the libraries it depends on) and generate the iOS project's build inputs,
# without hand-copying lists: when the Makefile changes, the next iOS build follows automatically.
#   Gen/src/<flat name>   symlinks to source files (excluding main.c and front_mac.m; main.c is linked separately to Gen/main/main.c and compiled with -Dmain=dm_main)
#   stdout            xcconfig lines: HEADER_SEARCH_PATHS (-I directories + the directory of every source file, so relative includes resolve as usual),
#                     OTHER_CFLAGS (-D), frameworks (Cocoa is replaced by what the UIKit side already has, skipped)
# Usage: gen_build.py <dm/host dir> <Gen dir>
import os, shlex, subprocess, sys
host, gen = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
subprocess.run(['make', '-s', '-C', host, 'game_profile'], check=True)   # 先真生成遊戲檔案頭（下面 make -n 只打印不執行；GAME 隨環境） | generate the game profile header for real first (make -n below only prints; GAME comes from the environment)
cmd = subprocess.run(['make', '-s', '-n', '-B', '-C', host, 'dmhost'], capture_output=True, text=True, check=True).stdout
# 宿主自己的鏈接命令（-o dmhost）在前，再收它依賴的庫的逐文件編譯命令（libvpx）：頭文件路徑按這個順序，宿主的頭先找到
# The host's own link command (-o dmhost) first, then the per-file compile commands of the libraries it depends on (libvpx): header paths keep this order, so the host's headers are found first
lines = [shlex.split(l) for l in cmd.splitlines() if l.lstrip().startswith('clang')]
lines.sort(key=lambda t: 'dmhost' not in t)
srcs, incs, defs, fws = [], [], [], []
for toks in lines:
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == '-framework': fws.append(toks[i + 1]); i += 2; continue
        if t.startswith('-I'):
            d = os.path.normpath(os.path.join(host, t[2:]))
            if d not in incs: incs.append(d)
        elif t.startswith('-D'): defs.append(t)
        elif t.endswith(('.c', '.m', '.S')): srcs.append(os.path.normpath(os.path.join(host, t)))
        i += 1
assert srcs, '編譯命令裡沒有源文件'
dup = sorted({s for s in srcs if srcs.count(s) > 1})
if dup: sys.exit('編譯命令裡源文件重復（Makefile 顯式列與通配重疊？）：' + ' '.join(dup))
os.makedirs(f'{gen}/src'); os.makedirs(f'{gen}/main')
dirs = []
for s in srcs:
    d = os.path.dirname(s)
    if d not in dirs: dirs.append(d)
    b = os.path.basename(s)
    if os.path.splitext(b)[0].endswith('_mac'): continue   # Mac 專用源按 _mac 命名約定不進 iOS（front_mac.m 由 front_ios.m 實現 front.h；touch_mac.c 是 Mac 觸點腳本） | Mac-only sources are kept out of iOS by the _mac naming convention (for front_mac.m, front_ios.m implements front.h; touch_mac.c is the Mac touch point script)
    if os.path.dirname(s) == os.path.normpath(os.path.join(host, '../ios/Sources')): continue   # 宿主借用的 iOS 源（如觸屏模塊）iOS 工程本身已編，再收一份會重復鏈接 | iOS sources borrowed by the host (e.g. the touch module) are already compiled by the iOS project itself; collecting another copy would link them twice
    if s == os.path.join(host, 'main.c'): os.symlink(s, f'{gen}/main/main.c'); continue
    flat = os.path.relpath(s, os.path.dirname(host)).replace('/', '__')   # 不同目錄同名文件不衝突 | Same-named files in different directories do not collide
    os.symlink(s, f'{gen}/src/{flat}')
print('HEADER_SEARCH_PATHS = $(inherited) ' + ' '.join(f'"{d}"' for d in incs + dirs))
print('OTHER_CFLAGS = $(inherited) ' + ' '.join(defs))
print('DM_FRAMEWORKS = ' + ' '.join(f'-framework {f}' for f in fws if f != 'Cocoa'))
