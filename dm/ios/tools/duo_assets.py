# 從玩家自己的遊戲檔案取 Duo 外殼要用的圖（建置時跑，產物只放在 Gen 目錄，不進版本庫）：
#   logo_<語言>.png  標題 Logo（Layout/Menu_PhotoStudioTitleLogo_00 的 Title_LogoPhoto_<語言>，ASTC 4x4，sips 解碼）；取不到時用遊戲圖標當 logo.png
#   wave.png         島名頁底部波浪的形狀（Layout/Common_MenuBGPa_00 的 LoopWave_00，BC4 灰度，可左右接續；存成白 = 波浪）
#   doodle.png       波浪裡的塗鴉圖案（同一個包的 CmnFacePartsBG_00，BC4 灰度，可平鋪）
# 只用標準庫 + zstd 命令 + sips（macOS 自帶）。
# 用法：duo_assets.py <RomFS 目錄> <輸出目錄> [後備圖標.jpg]
# Take the pictures the Duo shell uses from the player's own game files (run at build time; the outputs live only in the Gen directory, never in the repository):
#   logo_<lang>.png  the title logo (Title_LogoPhoto_<lang> in Layout/Menu_PhotoStudioTitleLogo_00, ASTC 4x4, decoded by sips); when it cannot be read, the game icon becomes logo.png
#   wave.png         the shape of the wave at the bottom of the island-name page (LoopWave_00 in Layout/Common_MenuBGPa_00, BC4 grey, tiles left to right; saved as white = wave)
#   doodle.png       the doodle pattern inside the wave (CmnFacePartsBG_00 in the same archive, BC4 grey, tiles)
# Uses only the standard library + the zstd command + sips (part of macOS).
# Usage: duo_assets.py <RomFS dir> <output dir> [fallback icon.jpg]
import os, struct, subprocess, sys, tempfile, zlib

def sarc_files(d):   # SARC：名字 → 內容 | SARC: name → contents
    if d[:4] != b'SARC': raise ValueError('not a SARC archive')
    hl = struct.unpack_from('<H', d, 4)[0]; doff = struct.unpack_from('<I', d, 12)[0]
    n = struct.unpack_from('<H', d, hl + 6)[0]
    nodes = [struct.unpack_from('<IIII', d, hl + 12 + 16 * i) for i in range(n)]
    names = hl + 12 + 16 * n + 8; out = {}
    for _, attr, s, e in nodes:
        p = names + (attr & 0xffff) * 4
        out[d[p:d.index(b'\0', p)].decode()] = d[doff + s:doff + e]
    return out

def bntx_textures(b):   # BNTX：名字 → (格式, 寬, 高, 塊高 log2, 數據) | BNTX: name → (format, width, height, block height log2, data)
    nx = b.index(b'NX  '); count, info = struct.unpack_from('<Iq', b, nx + 4); out = {}
    for i in range(count):
        p = struct.unpack_from('<q', b, info + 8 * i)[0]
        if b[p:p + 4] != b'BRTI': raise ValueError('bad BRTI')
        fmt, _, w, h, _, _, layout = struct.unpack_from('<IIIIIII', b, p + 0x1c)
        size = struct.unpack_from('<I', b, p + 0x50)[0]
        name_at = struct.unpack_from('<q', b, p + 0x60)[0]; nl = struct.unpack_from('<H', b, name_at)[0]
        mips = struct.unpack_from('<q', b, p + 0x70)[0]; off = struct.unpack_from('<q', b, mips)[0]   # 第 0 級的數據 | mip level 0's data
        out[b[name_at + 2:name_at + 2 + nl].decode()] = (fmt, w, h, layout & 7, b[off:off + size])
    return out

def deswizzle(src, wb, hb, bpp, blh):   # Tegra 塊線性 → 行優先（單位是壓縮塊） | Tegra block-linear → row-major (units are compressed blocks)
    gw = (wb * bpp + 63) // 64; bh = 1 << blh; out = bytearray(wb * hb * bpp)
    for y in range(hb):
        row = (y // (8 * bh)) * 512 * bh * gw + ((y % (8 * bh)) // 8) * 512 + ((y % 8) // 2) * 64 + (y % 2) * 16
        for x in range(wb):
            xb = x * bpp
            a = row + (xb // 64) * 512 * bh + ((xb % 64) // 32) * 256 + ((xb % 32) // 16) * 32 + (xb % 16)
            o = (y * wb + x) * bpp; out[o:o + bpp] = src[a:a + bpp]
    return bytes(out)

def bc4_rows(raw, w, h):   # BC4 → 灰度行（上下翻轉：貼圖原點在左下） | BC4 → grey rows (flipped: the texture origin is bottom left)
    wb, hb = (w + 3) // 4, (h + 3) // 4; px = bytearray(wb * 4 * hb * 4); pw = wb * 4
    for by in range(hb):
        for bx in range(wb):
            o = (by * wb + bx) * 8; a0, a1 = raw[o], raw[o + 1]; bits = int.from_bytes(raw[o + 2:o + 8], 'little')
            pal = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1
                              else [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
            for i in range(16): px[(by * 4 + i // 4) * pw + bx * 4 + i % 4] = pal[(bits >> (3 * i)) & 7]
    return [bytes(px[y * pw:y * pw + w]) for y in reversed(range(h))]

def write_grey_png(path, rows):
    w, h = len(rows[0]), len(rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    data = zlib.compress(b''.join(b'\0' + r for r in rows), 9)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) + chunk(b'IDAT', data) + chunk(b'IEND', b''))

def astc_png(tex, path, tmp):   # ASTC 4x4 → .astc 文件 → sips 解成 PNG → 上下翻轉 | ASTC 4x4 → .astc file → PNG via sips → flip vertically
    fmt, w, h, blh, data = tex
    if fmt >> 8 != 0x2d: raise ValueError(f'not ASTC 4x4: {fmt:#x}')
    raw = deswizzle(data, (w + 3) // 4, (h + 3) // 4, 16, blh)
    src = os.path.join(tmp, 'logo.astc')
    with open(src, 'wb') as f:
        f.write(struct.pack('<I3B', 0x5CA1AB13, 4, 4, 1) + w.to_bytes(3, 'little') + h.to_bytes(3, 'little') + (1).to_bytes(3, 'little') + raw)
    subprocess.run(['sips', '-s', 'format', 'png', src, '--out', path], check=True, capture_output=True)
    subprocess.run(['sips', '-f', 'vertical', path], check=True, capture_output=True)

def archive(romfs, name):
    p = os.path.join(romfs, 'Layout', name + '.Nin_NX_NVN.blarc.zs')
    d = subprocess.run(['zstd', '-dc', p], check=True, capture_output=True).stdout
    return bntx_textures(sarc_files(d)['timg/__Combined.bntx'])

def main():
    if len(sys.argv) < 3: sys.exit('usage: duo_assets.py <RomFS dir> <output dir> [fallback icon.jpg]')
    romfs, out = sys.argv[1], sys.argv[2]; icon = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True); made = []
    with tempfile.TemporaryDirectory() as tmp:
        try:
            for name, tex in archive(romfs, 'Menu_PhotoStudioTitleLogo_00').items():
                lang = name.split('^')[0].rsplit('_', 1)[-1]
                if not name.startswith('Title_LogoPhoto_') or lang == 'Bg': continue   # Bg 只是底下那塊黃色，Logo 已含 | Bg is only the yellow blob, which the logos already include
                astc_png(tex, os.path.join(out, f'logo_{lang}.png'), tmp); made.append(f'logo_{lang}')
        except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as e:
            print(f'duo_assets: no title logo from the game files ({e})', file=sys.stderr)
        if not any(m.startswith('logo_') for m in made) and icon and os.path.isfile(icon):
            subprocess.run(['sips', '-s', 'format', 'png', icon, '--out', os.path.join(out, 'logo.png')], check=True, capture_output=True)
            made.append('logo (game icon)')
        try:
            tex = archive(romfs, 'Common_MenuBGPa_00')
            for key, file in (('LoopWave_00^s', 'wave.png'), ('CmnFacePartsBG_00^r', 'doodle.png')):
                fmt, w, h, blh, data = tex[key]
                if fmt >> 8 != 0x1d: raise ValueError(f'{key} is not BC4: {fmt:#x}')
                rows = bc4_rows(deswizzle(data, (w + 3) // 4, (h + 3) // 4, 8, blh), w, h)
                if file == 'wave.png': rows = [bytes(255 - v for v in r) for r in rows]   # 貼圖裡波浪是黑的 | the wave is black in the texture
                write_grey_png(os.path.join(out, file), rows)
                made.append(file[:-4])
        except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as e:
            print(f'duo_assets: no doodle wave from the game files ({e})', file=sys.stderr)
    print(' '.join(made) if made else 'nothing')
    return 0

if __name__ == '__main__':
    sys.exit(main())
