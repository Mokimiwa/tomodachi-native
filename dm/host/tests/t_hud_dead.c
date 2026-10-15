// 底欄死亡隱藏單測：模式 → 已死、底欄顯隱判定（dm_hud.h）。期望值按遊戲代碼手寫：main+0x6e5c48 玩家模式 0 / 0x11 即已死
// Unit test for hiding the bottom bar on death: mode -> dead, bottom bar show/hide decision (dm_hud.h). Expected values are hand-written from the game code: main+0x6e5c48 player mode 0 / 0x11 means dead
#include <stdio.h>
#include "dm_hud.h"
static int fails, cases;
static void expect(const char *what, int got, int want) {
    cases++; if (got == want) printf("✓ %s\n", what); else { fails++; printf("✗ %s：得 %d，期望 %d\n", what, got, want); }
}
int main(void) {
    char buf[64];
    for (unsigned m = 0; m <= 19; m++) {   // D2 玩家模式 0..19（DEATH … KNOCKBACK） | D2 player modes 0..19 (DEATH ... KNOCKBACK)
        snprintf(buf, sizeof buf, "模式 %u %s", m, (m == 0 || m == 17) ? "→ 已死" : "→ 活著");
        expect(buf, dm_hud_player_mode_dead(m), m == 0 || m == 17);
    }
    expect("模式 0xffffffff（越界）→ 活著", dm_hud_player_mode_dead(0xffffffffu), 0);
    dm_hud_info h = {0}; h.valid = 1;
    for (unsigned m = 0; m <= 19; m++) {
        h.dead = dm_hud_player_mode_dead(m);
        snprintf(buf, sizeof buf, "局內常態 模式 %u → 底欄%s", m, (m == 0 || m == 17) ? "隱藏" : "顯示");
        expect(buf, dm_hud_bar_visible(&h, 0, 0), !(m == 0 || m == 17));
    }
    h.dead = 0;
    expect("活著但導航態 → 隱藏", dm_hud_bar_visible(&h, 1, 0), 0);
    expect("活著但面板擋住 → 隱藏", dm_hud_bar_visible(&h, 0, 1), 0);
    h.valid = 0; expect("未讀到玩家 → 隱藏", dm_hud_bar_visible(&h, 0, 0), 0);
    printf("%s t_hud_dead %d/%d\n", fails ? "FAIL" : "OK", cases - fails, cases);
    return fails != 0;
}
