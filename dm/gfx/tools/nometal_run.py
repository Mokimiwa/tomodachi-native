#!/usr/bin/env python3
# 在沒有蘋果 Metal 編譯器的機器上跑著色器出庫鏈（Windows / Linux / 沒裝 Xcode 的 Mac；給 tools/d2rm_pack.py 用）。
# 用法：nometal_run.py <腳本.py> [參數…]   例：nometal_run.py dmsh_build.py <包目錄> <緩存目錄>
# 做法：不改出庫鏈的任何一個文件（它們的內容參與翻譯鏈版本，一改內測的著色器庫就得整體換代），只在本進程裡把它們調外部命令的
# 那一個入口（subprocess.run）接過來，認出下面幾條命令自己答，其餘原樣放行：
#   xcrun -sdk <x> metal --version              → 答「none」（工具指紋裡編譯器記成不用 → 鏈版本自成一支，緩存與正常模式互不相混）
#   xcrun -sdk <x> metal -c <源碼> -o <中間件>    → 把源碼拷成中間件（換行統一成 \n）
#   xcrun -sdk <x> metallib <中間件> -o <庫>      → 把中間件原樣拷成庫
#   which <名字>                                 → 用 Python 自己找
#   cp -c <甲> <乙>                              → 普通拷貝
# 結果：「庫文件」（*_b.metallib / *_bf.metallib）裡放的是著色器源碼。殼（DM_SHELL 構建）的圖形層發現庫不是編好的就在設備上現編
# （gfx/nvn/nvn_mtl.m），系統會把編好的結果緩存，第二次啓動起不再編。
# 少掉的一道：正常模式下蘋果編譯器順帶驗了每份源碼能編過；這裡不驗，編不過的要到設備上用到它時才報紅。
# Runs the shader build chain on a machine without Apple's Metal compiler (Windows / Linux / a Mac without Xcode; for use by tools/d2rm_pack.py).
# Usage: nometal_run.py <script.py> [args...]   Example: nometal_run.py dmsh_build.py <package dir> <cache dir>
# How: not a single file of the build chain is modified (their contents feed into the translation chain version, and any change would force a whole new generation of the beta shader libraries); only, within this process, the one entry through which they call external commands
# (subprocess.run) is taken over: the commands below are recognized and answered here, and everything else is passed through unchanged:
#   xcrun -sdk <x> metal --version              → answers "none" (the compiler is recorded as unused in the tool fingerprint → the chain version forms its own branch, and its cache never mixes with normal mode's)
#   xcrun -sdk <x> metal -c <source> -o <intermediate file>    → copies the source as the intermediate file (line endings normalized to LF)
#   xcrun -sdk <x> metallib <intermediate file> -o <library>      → copies the intermediate file verbatim as the library
#   which <name>                                 → looked up by Python itself
#   cp -c <a> <b>                              → ordinary copy
# Result: the "library files" (*_b.metallib / *_bf.metallib) contain shader source. When the graphics layer of the shell (DM_SHELL build) finds that a library is not a compiled one, it compiles it on the device
# (gfx/nvn/nvn_mtl.m); the system caches the compiled result, and from the second launch on nothing is compiled again.
# The one check that is lost: in normal mode Apple's compiler also verifies along the way that every source compiles; here that is not verified, and one that fails to compile only reports a failure when it is used on the device.
import os, runpy, shutil, subprocess, sys

_real = subprocess.run


def _done(args, out=''):
    return subprocess.CompletedProcess(args, 0, stdout=out, stderr='')


def run(args, *a, **k):
    text = k.get('text') or k.get('universal_newlines')
    enc = (lambda s: s) if text else (lambda s: s.encode())
    if isinstance(args, (list, tuple)) and args:
        c = [str(x) for x in args]
        if c[0] == 'xcrun' and 'metal' in c and '--version' in c: return _done(args, enc('none\n'))
        if c[0] == 'xcrun' and 'metal' in c and '-c' in c and '-o' in c:   # 源碼 = metal 之後第一個不是選項、也不是 -o 的值的參數（iOS 那條命令在 -c 後面還帶一個版本選項） | source = the first argument after metal that is neither an option nor the value of -o (the iOS command also carries a version option after -c)
            o = c.index('-o'); src = [x for i, x in enumerate(c[c.index('metal') + 1:], c.index('metal') + 1) if not x.startswith('-') and i != o + 1]
            if len(src) != 1: raise RuntimeError('nometal_run：認不出源碼參數 %r' % c)
            with open(src[0], 'rb') as f: b = f.read()
            with open(c[o + 1], 'wb') as f: f.write(b.replace(b'\r\n', b'\n'))   # Windows 上 spirv-cross 寫出的源碼是 \r\n 換行：進安裝包前歸一，同樣的輸入各系統出的包才相同 | On Windows spirv-cross writes sources with CRLF line endings: normalize before they go into the install package, so the same input gives the same package on every system
            return _done(args, enc(''))
        if c[0] == 'xcrun' and 'metallib' in c and '-o' in c: shutil.copyfile(c[c.index('metallib') + 1], c[c.index('-o') + 1]); return _done(args, enc(''))
        if c[0] == 'xcrun': raise RuntimeError('nometal_run：沒認出的 xcrun 命令 %r' % c)
        if c[0] == 'which' and len(c) == 2: return _done(args, enc((shutil.which(c[1]) or '') + '\n'))
        if c[0] == 'cp' and len(c) == 4 and c[1] == '-c': shutil.copyfile(c[2], c[3]); return _done(args, enc(''))
    return _real(args, *a, **k)


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__ or '用法：nometal_run.py <腳本.py> [參數…]')
    subprocess.run = run
    script = os.path.abspath(sys.argv[1]); sys.argv = sys.argv[1:]; sys.path.insert(0, os.path.dirname(script))
    runpy.run_path(script, run_name='__main__')
