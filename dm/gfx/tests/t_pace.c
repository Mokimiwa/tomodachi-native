// 節拍器單測：給定時鐘序列，期望等待時長手算。週期 16.666 ms 記為 P = 16666667 ns。
// Pacer unit tests: given a clock sequence, the expected wait durations are hand-calculated. The period 16.666 ms is written as P = 16666667 ns.
#include "../nvn/nvn_pace.h"
#include <stdio.h>

int main(void) {
    const uint64_t P = 16666667, MS = 1000000;
    struct { uint64_t now, period, want; const char *why; } c[] = {
        {1000 * MS, P, 0, "首幀：起拍，不等"},
        {1005 * MS, P, 1000 * MS + P - 1005 * MS, "早到 5 ms：等到 1000+P"},
        {1000 * MS + 2 * P - 2 * MS, P, 2 * MS, "第三幀早 2 ms：等 2 ms"},
        {1000 * MS + 3 * P + 3 * MS, P, 0, "遲到 3 ms（不足一週期）：立即出圖，節拍點仍為 1000+3P"},
        {1000 * MS + 4 * P - 1 * MS, P, 1 * MS, "下一幀早 1 ms：按原相位等到 1000+4P"},
        {1000 * MS + 7 * P, P, 0, "落後兩個週期（卡頓）：重新起拍於當時，不連發追幀"},
        {1000 * MS + 7 * P + 4 * MS, P, P - 4 * MS, "重新起拍後的下一幀：等到新目標"},
        {1000 * MS + 9 * P, 0, 0, "週期 0（DM_PRESENT_HZ=0）：不等"},
        {1000 * MS + 9 * P + 1 * MS, 2 * P, 2 * P - 1 * MS, "切回間隔 2：節拍點 = 上幀時刻 9P + 2P"},
        {1000 * MS + 11 * P + 3 * MS, 2 * P, 2 * P - 3 * MS, "間隔 2 下一幀：節拍點 13P"},
    };
    nvn_pacer p = {0}; int fail = 0, n = sizeof c / sizeof *c;
    for (int i = 0; i < n; i++) {
        uint64_t got = nvn_pace(&p, c[i].now, c[i].period);
        int ok = got == c[i].want;
        printf("%-52s 期望等 %8.3f ms → 真結果 %8.3f ms %s\n", c[i].why, c[i].want / 1e6, got / 1e6, ok ? "✓" : "✗");
        fail += !ok;
    }
    printf("== %d 項，失敗 %d ==\n", n, fail);
    return fail != 0;
}
