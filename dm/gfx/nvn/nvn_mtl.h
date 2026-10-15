// NVN 空設備的 Metal 執行層（C 接口）：內存池 = 零拷貝 MTLBuffer，提交 = MTLCommandBuffer，同步 = MTLSharedEvent。
// 時序約定（防 x86 路線 GO_MAP_SHARE 花屏的覆轍）：一切「GPU 完成」的可見效果（同步對象就緒、事件值、計數器）
// 只在 Metal 命令緩衝真正完成之後才發生；CPU 側等待只看 MTLSharedEvent 的已完成值。
// Metal execution layer of the NVN null device (C interface): memory pool = zero-copy MTLBuffer, submit = MTLCommandBuffer, sync = MTLSharedEvent.
// Ordering convention (to avoid repeating the GO_MAP_SHARE corrupted-picture mistake of the x86 route): every visible effect of "GPU completion" (sync object ready, event values, counters)
// happens only after the Metal command buffer has actually completed; CPU-side waits look only at the completed value of the MTLSharedEvent.
#pragma once
#include <stdint.h>
#include <stddef.h>

void mtl_init(void);
void bf_want_note(const char *key);

// 內存池：[mem, mem+size) 外擴到頁邊界建零拷貝緩衝；按 GPU 地址（= CPU 地址）反查緩衝與偏移
// Memory pool: [mem, mem+size) is expanded outward to page boundaries to build a zero-copy buffer; look up the buffer and offset by GPU address (= CPU address)
void mtl_pool_add(const void *mem, uint64_t size);
void mtl_pool_del(const void *mem);
// 內存池 Finalize：按存儲共用的貼圖表裡建在這個池（池對象地址）上的貼圖一並淘汰 | memory pool Finalize: evict the textures built on this pool (pool object address) from the storage-sharing table
void mtl_alias_drop_pool(const void *pool);
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // 成功 1；buf 是 (__bridge) id<MTLBuffer> | 1 on success; buf is a (__bridge) id<MTLBuffer>

typedef struct {
    uint32_t pso, cps, shaders, dss;
    uint32_t view_nonnull, view_slots, live_tex, sview_nonnull;
    uint32_t pools;
    uint64_t pool_adds;
    uint32_t c1_bufs; uint64_t c1_alloc, c1_data;   // 著色器 c1 常數段：緩衝個數（4 MiB 塊 + 獨佔 64 KiB）、分配字節、實際字節 | Shader c1 constant segment: buffer count (4 MiB blocks + dedicated 64 KiB), allocated bytes, actual bytes
} mtl_res_count;
void mtl_res_counts(uint32_t tick, const char *why);   // 打一行資源計數（宿主按拍 / 事件調用，見 dm.h dm_res_report_register） | Print one line of resource counts (the host calls it per tick / event, see dm_res_report_register in dm.h)
void mtl_res_snapshot(mtl_res_count *out);

// 一次提交：begin → 若干操作 → commit（返回本次提交完成時的柵欄值）
// One submit: begin -> several operations -> commit (returns the fence value at which this submit completes)
void mtl_sub_begin(void);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n);               // GPU 時序上的緩衝拷貝 | Buffer copy in GPU ordering
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // 完成後寫事件值（0 寫 / 1 加） | Write the event value after completion (0 write / 1 add)
void mtl_sub_counter(uint64_t addr, uint32_t type);                       // 完成後寫計數器記錄 | Write the counter record after completion
uint64_t mtl_sub_commit(void);

uint64_t mtl_last_fence(void);                       // 最近一次提交的柵欄值（0 = 從未提交） | Fence value of the most recent submit (0 = never submitted)
int mtl_wait(uint64_t fence, uint64_t timeout_ns);   // GPU 完成到 fence 返回 1，超時 0 | Returns 1 when the GPU has completed up to fence, 0 on timeout
void mtl_finish(void);                               // 等所有已提交的命令完成 | Wait for all submitted commands to complete
void mtl_drain(void);                                // 等隊列上全部命令（含出圖）完成：提交空命令緩衝並等它 | Wait for all commands on the queue (including presents) to complete: submit an empty command buffer and wait for it
void mtl_xv_window_tex(uint64_t serial);   // 登記窗口（交換鏈）貼圖：跨提交有效性賬本永不收它 | Register a window (swapchain) texture: the cross-submit validity ledger never collects it
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy);   // 拖貨跟手圖：on=1 時把蓋住屏幕點 (px,py)、壓在貨架格網 grid（左上角 x,y、一格寬高）上的那件貨的圖標在界面趟末尾平移到中心 (cx,cy) 再畫一遍（歸一化，左上原點）；任意線程調 | Drag-follow image for shop items: when on=1, the icon of the item that covers screen point (px,py) and sits on the shop shelf grid `grid` (top-left x,y, width and height of one cell) is translated to center (cx,cy) and drawn once more at the end of the UI pass (normalized, top-left origin); callable from any thread
int mtl_ui_ghost_rect(float r[4]);   // 上面認到的那件貨的屏幕矩形（歸一化）；還沒認到返回 0 | Screen rectangle (normalized) of the item recognized above; returns 0 if not recognized yet
