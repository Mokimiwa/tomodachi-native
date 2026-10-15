/* mtl_backend.h —— 最小 Metal 離屏後端（純 C 接口，NVN→Metal 後端底座）
 *
 * 調用模型：
 *   ctx = mtlb_init();
 *   rt  = mtlb_target(ctx, w, h, MTLB_FMT_BGRA8);
 *   mtlb_clear(ctx, rt, ...);      // 錄一個 pass 進「當前幀」的命令緩衝
 *   mtlb_triangle(ctx, rt);        // 再錄一個 pass，同一命令緩衝
 *   mtlb_readback(ctx, rt, buf, n);// 提交當前幀、等 GPU 完成、讀回像素
 *
 * 一幀 = 一個 MTLCommandBuffer，在第一次錄 pass 時創建，readback 時提交。
 * 所有函數返回 0 成功 / -1 失敗（失敗原因打到 stderr）。非線程安全。
 * mtl_backend.h - minimal Metal offscreen backend (pure C interface, the base of the NVN→Metal backend)
 * Call model:
 *   ctx = mtlb_init();
 *   rt  = mtlb_target(ctx, w, h, MTLB_FMT_BGRA8);
 *   mtlb_clear(ctx, rt, ...);      // record a pass into the "current frame" command buffer
 *   mtlb_triangle(ctx, rt);        // record another pass, same command buffer
 *   mtlb_readback(ctx, rt, buf, n);// commit the current frame, wait for the GPU to finish, read back pixels
 * One frame = one MTLCommandBuffer, created when the first pass is recorded and committed at readback.
 * All functions return 0 on success / -1 on failure (the failure reason is printed to stderr). Not thread-safe.
 */
#ifndef MTL_BACKEND_H
#define MTL_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtlb_ctx mtlb_ctx; /* 設備 + 命令隊列 + 當前幀命令緩衝 + 管線緩存 | device + command queue + current-frame command buffer + pipeline cache */
typedef struct mtlb_rt  mtlb_rt;  /* 一個離屏渲染目標 | an offscreen render target */

typedef enum {
    MTLB_FMT_BGRA8 = 0, /* 內存字節序 B,G,R,A（unorm） | byte order in memory B,G,R,A (unorm) */
    MTLB_FMT_RGBA8 = 1, /* 內存字節序 R,G,B,A（unorm） | byte order in memory R,G,B,A (unorm) */
} mtlb_fmt;

mtlb_ctx *mtlb_init(void);
void      mtlb_shutdown(mtlb_ctx *ctx);

mtlb_rt  *mtlb_target(mtlb_ctx *ctx, uint32_t w, uint32_t h, mtlb_fmt fmt);
void      mtlb_target_free(mtlb_rt *rt);

int mtlb_clear(mtlb_ctx *ctx, mtlb_rt *rt, float r, float g, float b, float a);
int mtlb_triangle(mtlb_ctx *ctx, mtlb_rt *rt);

/* buf 至少 w*h*4 字節；按行緊排、第 0 行是圖像頂部，字節序同 rt 的格式。 | buf is at least w*h*4 bytes; rows tightly packed, row 0 is the top of the image, byte order same as rt's format. */
int mtlb_readback(mtlb_ctx *ctx, mtlb_rt *rt, void *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
#endif
