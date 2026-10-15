// NVN 貼圖存儲佈局（Tegra X1 塊線性），見 nvn_tex.c。
// NVN texture storage layout (Tegra X1 block-linear), see nvn_tex.c.
#pragma once
#include <stdint.h>

typedef struct {
    int target, format, flags, samples;   // NVNtextureTarget / NVNformat / NVNtextureFlags / 採樣數（0 或 1 = 單採樣） | NVNtextureTarget / NVNformat / NVNtextureFlags / sample count (0 or 1 = single-sampled)
    uint32_t width, height, depth, levels, stride;
    int pkg_block_h;                      // 預排布貼圖佈局參數第 0 字節（塊高 log2）+ 1；0 = 按自動算法 | Byte 0 of the packaged texture layout parameters (log2 of block height) + 1; 0 = use the automatic algorithm
} nvn_texdesc;

typedef struct {
    uint64_t size, layer_size;            // 總存儲字節、每層字節 | Total storage bytes, bytes per layer
    uint32_t align, layers, bpb;          // 對齊、層數、每塊字節 | Alignment, layer count, bytes per block
    unsigned tileH, tileD;                // 塊高 / 塊深（log2 GOB 數） | Block height / block depth (log2 of the GOB count)
} nvn_texlayout;

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);   // 每塊字節（0 = 未知格式），塊寬高 | Bytes per block (0 = unknown format), block width and height
// 第 level 級實際使用的塊高 / 塊深（log2 GOB 數）：貼圖級塊高按該級行數（塊行）收縮，解排布逐級用它
// Block height / block depth actually used by level `level` (log2 of the GOB count): the per-level block height shrinks with that level's row count (block rows), and deswizzling uses it level by level
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);
