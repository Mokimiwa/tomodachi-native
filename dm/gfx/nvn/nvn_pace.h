// 出圖節拍器（純函數，Mac / iOS 共用；單測 tests/t_pace.c）。
// 語義同垂直同步：節拍點 = 上一節拍點 + 週期。早到 → 等到節拍點；遲到不足一個週期 → 立即出圖、保持原相位；
// 落後一個週期以上（卡頓）→ 從當前時刻重新起拍，不連發追幀。週期 0 = 不等（節拍點跟隨當前時刻）。
// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now：單調時鐘（ns）；period：本幀週期（ns，0 = 不等）。返回需要等待的 ns（0 = 立即出圖），並更新內部目標時刻。
// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}
