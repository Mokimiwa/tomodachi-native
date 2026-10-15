// 不支持 BC 壓縮格式的顯卡（A15 / A16 等，蘋果 GPU 族 Apple8 及以下）：BC 貼圖在 CPU 上解壓成不壓縮格式。
// 遊戲貼圖裡的 BC3 由老機型補充包離線換成 ASTC（dm/tools/astc_pack.py）；剩下的 BC（精靈圖裡的 BC3、少量 BC1 / BC4 / BC5 / BC6H）走這裡。
// GPUs without BC compressed format support (A15 / A16 etc., Apple GPU family Apple8 and below): BC textures are decompressed on the CPU into uncompressed formats.
// BC3 in game textures is converted offline to ASTC by the Legacy Device Pack (dm/tools/astc_pack.py); the remaining BC (BC3 in sprites, a few BC1 / BC4 / BC5 / BC6H) goes through here.
#pragma once
#include <stddef.h>
#include <stdint.h>

// NVN 格式是 BC → 返回 1，*mtl_pf = 解壓後的 Metal 像素格式、*bpp = 解壓後每像素字節；不是 BC 返回 0
// NVN format is BC → returns 1, *mtl_pf = Metal pixel format after decompression, *bpp = bytes per pixel after decompression; returns 0 if not BC
int nvn_bc_unpacked(uint32_t nvn_fmt, uint32_t *mtl_pf, int *bpp);
// 解一片 w×h 像素：src 按塊行排（每塊行 pitch 字節），dst 每像素行 dpitch 字節。成功 0
// Decode a w×h pixel region: src is laid out by block rows (pitch bytes per block row), dst has dpitch bytes per pixel row. Returns 0 on success
int nvn_bc_decode(uint32_t nvn_fmt, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch);
