// Tegra 塊線性 ↔ 線性。級 / 層偏移與逐級塊高塊深一律取 nvn/nvn_tex.c（nvn_tex_level_offset / nvn_tex_layout / nvn_tex_level_tiles）。
// Tegra block-linear ↔ linear. Level / layer offsets and per-level block height and block depth always come from nvn/nvn_tex.c (nvn_tex_level_offset / nvn_tex_layout / nvn_tex_level_tiles).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../nvn/nvn_tex.h"

// 把第 level 級、第 layer 層（數組 / 立方體的層；非分層貼圖取 0）從整張貼圖存儲 src_storage 解到線性 dst_linear。
// dst 按塊行排：每行 dst_pitch 字節（≥ 本級塊寬 × 每塊字節），壓縮格式一行 = 一行塊。
// 3D 貼圖：一次解出本級全部深度切片，切片 k 位於 dst_linear + k × dst_pitch × 本級塊行數。
// 不支持：多重採樣、LINEAR 標誌（本就線性）、BUFFER；遇到返回 -1，成功返回 0。
// Decode level number level, layer number layer (layer of an array / cube; 0 for non-layered textures) from the whole-texture storage src_storage into linear dst_linear.
// dst is laid out by block rows: dst_pitch bytes per row (≥ width in blocks of this level × bytes per block); for compressed formats one row = one row of blocks.
// 3D textures: all depth slices of this level are decoded at once; slice k is at dst_linear + k × dst_pitch × block row count of this level.
// Not supported: multisampling, the LINEAR flag (already linear), BUFFER; returns -1 on these, 0 on success.
int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear, size_t dst_pitch);
// 反向（單測往返用）：線性 src_linear → 寫進整張貼圖存儲 dst_storage 的對應級 / 層
// Reverse (for unit test round trips): linear src_linear → written into the corresponding level / layer of the whole-texture storage dst_storage
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear, size_t src_pitch, void *dst_storage);
// 單點地址：本級本層內，線性 (x 字節, y 塊行, z 切片) → 相對本級本層起點的塊線性字節偏移（單測按手算核）
// Single-point address: within this level and layer, linear (x byte, y block row, z slice) → block-linear byte offset relative to the start of this level and layer (unit tests verify against hand calculation)
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned tileH, unsigned tileD);

int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices,
                       uint64_t *base, unsigned *tileH, unsigned *tileD, uint32_t *bpb);
