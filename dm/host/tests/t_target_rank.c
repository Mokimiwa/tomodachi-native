// 自動選怪單測：在本進程內存裡擺假單位 / 假怪物數據，調觀測門回調，核分數調整（期望值手算，不調被測實現算）。
// 佈局：單位 +0 類型、+4 類號、+0x10 → 怪物數據（+0x1A TypeFlag：0x02 超級獨特 / 0x04 冠軍 / 0x08 獨特 / 0x10 隨從）。
// Automatic monster targeting unit test: lay out fake units / fake monster data in this process's memory, call the observation gate callback, and check the score adjustment (expected values computed by hand, not by calling the implementation under test).
// Layout: unit +0 type, +4 class number, +0x10 -> monster data (+0x1A TypeFlag: 0x02 super unique / 0x04 champion / 0x08 unique / 0x10 minion).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int gate_obs_present(int id) { (void)id; return 1; }
static void (*g_fn)(const uint64_t *);
void gate_obs_set(int id, void (*fn)(const uint64_t *)) { (void)id; g_fn = fn; }
void target_rank_install(void);
static int fails, cases;
static float run(const void *unit, float score) {
    uint64_t frame[100] = {0}; frame[1] = (uint64_t)(uintptr_t)unit; frame[2] = 1;
    memcpy(&frame[32], &score, 4); g_fn(frame);
    float out; memcpy(&out, &frame[32], 4); return out;
}
static void expect(const char *what, float got, float want) {
    cases++; if (got == want) printf("✓ %s（%g）\n", what, got); else { fails++; printf("✗ %s：得 %g，期望 %g\n", what, got, want); }
}
int main(void) {
    target_rank_install();
    uint8_t data[0x40] = {0};
    struct { uint32_t type, cls, id, mode; uint64_t data; } unit = {1, 5, 77, 0, (uint64_t)(uintptr_t)data};
    expect("普通僵屍（類 5、TypeFlag 0）分數不變", run(&unit, 0.5f), 0.5f);
    data[0x1a] = 0x10; expect("隨從（0x10）算普通", run(&unit, 0.5f), 0.5f);
    data[0x1a] = 0x04; expect("冠軍（0x04）+1000", run(&unit, 0.5f), 1000.5f);
    data[0x1a] = 0x08; expect("獨特（0x08）+1000", run(&unit, 0.25f), 1000.25f);
    data[0x1a] = 0x02; expect("超級獨特（0x02）+1000", run(&unit, 0.75f), 1000.75f);
    data[0x1a] = 0; unit.cls = 156; expect("安達利爾（類 156，boss=1）+2000", run(&unit, 0.5f), 2000.5f);
    unit.cls = 745; expect("3.3 新 Boss 巨像（類 745）+2000", run(&unit, 0.125f), 2000.125f);
    unit.cls = 5; unit.type = 0; data[0x1a] = 0x08; expect("玩家單位（類型 0）不動", run(&unit, 0.5f), 0.5f);
    unit.type = 1; unit.data = 0; data[0x1a] = 0; expect("怪物數據指針空：讀不到，不動", run(&unit, 0.5f), 0.5f);
    expect("候選單位空指針不動", run(NULL, 0.5f), 0.5f);
    // 跨檔：精英最差分（0.01+1000）仍高於普通最好分（0.99）；Boss 最差分高於精英最好分
    // Across tiers: the worst elite score (0.01+1000) is still higher than the best normal score (0.99); the worst Boss score is higher than the best elite score
    unit.data = (uint64_t)(uintptr_t)data; data[0x1a] = 0x04; float elite = run(&unit, 0.01f);
    data[0x1a] = 0; unit.cls = 211; float boss = run(&unit, 0.01f);
    cases++; if (elite > 0.99f && boss > 1000.99f) printf("✓ 跨檔優先：精英 %g > 普通 0.99，Boss %g > 精英 1000.99\n", elite, boss); else { fails++; printf("✗ 跨檔優先\n"); }
    printf("%s：%d/%d\n", fails ? "【紅】自動選怪" : "自動選怪 全綠", cases - fails, cases);
    return fails != 0;
}
