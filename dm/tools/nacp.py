#!/usr/bin/env python3
# NSP 控制數據（control.nacp，nx_extract 解包時存在解出目錄裡）裡的遊戲名：iOS App 名取它（打包腳本與 build_ipa.sh 共用）。
# 用法：nacp.py <control.nacp>（打印遊戲名；讀不出就打印空行、退出碼 1）；Python 裡 import nacp; nacp.name(路徑)
# The game name from the NSP's control data (control.nacp, saved in the extraction folder by nx_extract): the iOS app name comes from it (shared by the packing script and build_ipa.sh).
# Usage: nacp.py <control.nacp> (prints the game name; prints an empty line and exits 1 when none can be read); from Python: import nacp; nacp.name(path)
import struct, sys, zlib
from pathlib import Path


def name(path):
    """名稱區每種語言 0x300 字節（名稱 0x200 + 發行商 0x100），取第一個非空的（0 = 美式英語）；不存在 / 讀不出像樣的名字返回空串
    The title area holds 0x300 bytes per language (name 0x200 + publisher 0x100); take the first non-empty one (0 = American English); a missing file or no sensible name returns an empty string"""
    p = Path(path)
    if not p.is_file(): return ''
    b = p.read_bytes(); t = b[:0x3000]
    try:   # 新 SDK 把名稱區壓縮：u16 長度 + raw deflate，解開是 32 種語言 × 0x300（馬力歐 1.2.1 就是） | newer SDKs compress the title area: a u16 size + raw deflate, inflating to 32 languages × 0x300 (Mario 1.2.1 does)
        z = zlib.decompress(b[2:2 + struct.unpack_from('<H', b)[0]], -15)
        if len(z) == 0x6000: t = z
    except zlib.error: pass   # 未壓縮 | not compressed
    for i in range(len(t) // 0x300):
        n = t[i * 0x300:i * 0x300 + 0x200].split(b'\0', 1)[0]
        if not n: continue
        try: n = n.decode('utf-8')
        except UnicodeDecodeError: return ''
        return n if n.isprintable() else ''
    return ''


if __name__ == '__main__':
    if len(sys.argv) != 2: sys.exit('用法 | usage: nacp.py <control.nacp>')
    n = name(sys.argv[1]); print(n); sys.exit(0 if n else 1)
