// 輸入映射層單測（dm_bind.c + hle_touch.c）：控制項串的解析、玩家表覆蓋默認表、按下來源合成手柄狀態、Touch 目標走映射層自己的手指槽。
// 期望值按 dm_bind.h 的規則手算：推滿 = DM_STICK_MAX，相對兩鍵抵消，同軸相加夾到 ±1。
// Input mapping layer unit test (dm_bind.c + hle_touch.c): parsing control strings, the player's table overriding the defaults, combining held sources into controller state, Touch targets using the mapping layer's own finger slots.
// Expected values are hand-computed from the rules in dm_bind.h: full tilt = DM_STICK_MAX, opposite keys cancel, the same axis sums and clamps to ±1.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../dm_bind.h"
#include "../dm_input.h"
#include "../front.h"
static int checks, fails;
void gesture_feed(const int32_t (*pt)[2], int n, uint64_t now) { (void)pt; (void)n; (void)now; }   // 手勢識別另有單測 | gesture recognition is tested separately
uint64_t now_ns(void) { return 0; }
uint64_t dm_game_res(int docked) { (void)docked; return (uint64_t)1080 << 32 | 1920; }   // 出圖 16:9：界面帶不換算觸點 | 16:9 output: the UI band leaves touch points as they are
#define CHECK(c, ...) do { checks++; if (c) printf("  ✓ "); else { fails++; printf("  ✗ "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static uint64_t s_btn; static int32_t s_ax[4];
static void cap(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { s_btn = b; s_ax[0] = lx; s_ax[1] = ly; s_ax[2] = rx; s_ax[3] = ry; }
static int ax_is(int32_t lx, int32_t ly, int32_t rx, int32_t ry) { return s_ax[0] == lx && s_ax[1] == ly && s_ax[2] == rx && s_ax[3] == ry; }
// 讀一次觸屏，找面板坐標 (x, y) 上的手指；返回觸點數，*fid 是那根手指的 finger_id（沒有 = -1）。finger_id 是宿主按真機規則分配的（hle_touch.c），不等於槽號
// Read the touchscreen once and look for the finger at panel coordinates (x, y); returns the touch count, *fid is that finger's finger_id (-1 = absent). finger_id is allocated by the host following the hardware rule (hle_touch.c) and is not the slot number
static int touch_at(int32_t x, int32_t y, int32_t *fid) {
    static uint8_t st[0x290]; int k = touch_fill_n(st, 16, 1000); *fid = -1;
    for (int i = 0; i < k; i++) { int32_t v[3]; memcpy(v, st + 0x10 + 0x28 * i + 0x0c, 12); if (v[1] == x && v[2] == y) *fid = v[0]; }
    return k;
}

int main(void) {
    const int M = DM_STICK_MAX;
    dm_ctl c;
    CHECK(!dm_ctl_parse("A+b+Plus", &c) && c.btn == (DM_BTN_A | DM_BTN_B | DM_BTN_PLUS), "按鍵名大小寫不論、+ 連接");
    CHECK(!dm_ctl_parse("StickLUp", &c) && c.ax[0] == 0 && c.ax[1] == 1 && !c.btn, "StickLUp = 左搖桿 y +1");
    CHECK(!dm_ctl_parse("StickRLeft=0.5", &c) && c.ax[2] == -0.5f && c.ax[3] == 0, "StickRLeft=0.5 = 右搖桿 x −0.5");
    CHECK(!dm_ctl_parse("StickR", &c) && c.analog == 2, "StickR = 收右搖桿模擬量");
    CHECK(!dm_ctl_parse("Touch=640/360", &c) && c.touch && c.tx == 0.5f && c.ty == 0.5f, "Touch=640/360 = 面板正中");
    CHECK(dm_ctl_parse("A+Jump", &c) == 1 && c.btn == DM_BTN_A, "不認識的名字計 1，認識的照填");
    CHECK(dm_ctl_parse("StickLUp=2", &c) == 1 && dm_ctl_parse("A=1", &c) == 1 && dm_ctl_parse("Touch=1280/0", &c) == 1 && dm_ctl_parse("StickLUp=", &c) == 1,
          "幅度超出 0–1、按鍵帶值、觸點出面板、空幅度都不收");

    const char *def = "key:W StickLUp\nkey:S StickLDown\nkey:D StickLRight\nkey:J A\nkey:K B   # 注釋\n\nkey:P StickRUp\nkey:T Touch=320/180\npad:LeftStick StickL\n";
    char path[] = "/tmp/t_bind_XXXXXX"; int fd = mkstemp(path); FILE *f = fdopen(fd, "w");
    fputs("# 玩家的表\nkey:J B\nkey:K -\nkey:X X+Y\nkey:Bad Jump\nkey:Two A B\n", f); fclose(f);
    int n = dm_bind_load(def, path); unlink(path);
    CHECK(n == 8, "默認 8 條，玩家改 J、刪 K、加 X，兩條壞行跳過：共 %d 條", n);
    CHECK(dm_bind_find("key:K") < 0 && dm_bind_find("key:Bad") < 0 && dm_bind_find("key:Two") < 0, "刪掉的與壞行都查不到");

    int w = dm_bind_find("key:W"), s = dm_bind_find("KEY:s"), d = dm_bind_find("key:D"), j = dm_bind_find("key:J"), x = dm_bind_find("key:X"), p = dm_bind_find("key:P");
    dm_bind_press(j, 1); dm_bind_publish(cap);
    CHECK(s_btn == DM_BTN_B && ax_is(0, 0, 0, 0), "J 被玩家改成 B");
    dm_bind_press(j, 0); dm_bind_press(x, 1); dm_bind_publish(cap);
    CHECK(s_btn == (DM_BTN_X | DM_BTN_Y), "一個來源映射到兩個鍵");
    dm_bind_press(x, 0); dm_bind_press(w, 1); dm_bind_press(d, 1); dm_bind_publish(cap);
    CHECK(s_btn == 0 && ax_is(M, M, 0, 0), "W+D：左搖桿右上，兩軸都推滿（與原來的 WASD 相同）");
    dm_bind_press(s, 1); dm_bind_publish(cap);
    CHECK(ax_is(M, 0, 0, 0), "再按 S：上下抵消");
    dm_bind_press(w, 0); dm_bind_press(s, 0); dm_bind_press(d, 0); dm_bind_press(p, 1); dm_bind_publish(cap);
    CHECK(ax_is(0, 0, 0, M), "P：右搖桿上");
    dm_bind_press(p, 0);

    int ls = dm_bind_find("pad:LeftStick");
    dm_bind_analog(ls, 0.5f, -0.25f); dm_bind_publish(cap);
    CHECK(ax_is((int32_t)(0.5f * M), (int32_t)(-0.25f * M), 0, 0), "模擬量來源不必按下：左搖桿 (0.5, −0.25)");
    dm_bind_press(d, 1); dm_bind_publish(cap);
    CHECK(ax_is(M, (int32_t)(-0.25f * M), 0, 0), "模擬量加鍵盤方向：x 相加夾到 1");
    dm_bind_press(d, 0); dm_bind_analog(ls, 0, 0); dm_bind_publish(cap);
    CHECK(ax_is(0, 0, 0, 0) && s_btn == 0, "全放開：歸零");

    int32_t fid;
    dm_touch front = {0, 0.75f, 0.5f}; dm_touch_set(&front, 1);
    int t = dm_bind_find("key:T"); dm_bind_press(t, 1); dm_bind_publish(cap);
    int k = touch_at(320, 180, &fid);
    CHECK(k == 2 && fid == 1, "T：映射層的手指（槽 %d）在 (320, 180)、finger_id 1（前端手指佔 0），共 %d 根", DM_TOUCH_BIND, k);
    dm_bind_press(t, 0); dm_bind_publish(cap); touch_at(320, 180, &fid); touch_at(320, 180, &fid);
    k = touch_at(960, 360, &fid);
    CHECK(k == 1 && fid == 0, "放開 T：映射層手指報完 End 後消失，前端手指（finger_id 0）不受影響");
    dm_touch scr = {DM_TOUCH_SCRIPT, 0.1f, 0.1f}; dm_touch_set_range(&scr, 1, DM_TOUCH_SCRIPT, DM_TOUCH_SCRIPT + DM_TOUCH_SCRIPT_N);
    dm_touch_set(NULL, 0); touch_at(128, 72, &fid);
    k = touch_at(128, 72, &fid);
    CHECK(k == 1 && fid == 1, "前端全抬起不會抬起腳本的手指（它按下那次 0 號還在報 End，拿 1）");
    uint8_t small[0x10 + 0x28 * 2]; dm_touch two[3] = {{1, 0.1f, 0.1f}, {2, 0.2f, 0.2f}, {3, 0.3f, 0.3f}}; dm_touch_set(two, 3);
    k = touch_fill_n(small, 2, 2000);
    CHECK(k == 2, "TouchScreenState<2> 只寫 2 根（其餘留到下次）");

    dm_ctl pc; CHECK(!dm_ctl_parse("Pointer", &pc) && pc.touch == 2 && dm_ctl_parse("Pointer=1", &pc) == 1, "Pointer = 跟著前端指針觸屏，不帶值");
    dm_bind_load("mouse:Left Pointer\nmouse:Right B\npad:LeftStick StickL\n", NULL);
    int ml = dm_bind_find("mouse:Left"), mr = dm_bind_find("mouse:Right"), ls2 = dm_bind_find("pad:LeftStick");
    CHECK(dm_bind_is_pointer(ml) && !dm_bind_is_pointer(mr) && !dm_bind_is_pointer(-1), "mouse:Left 是 Pointer、mouse:Right 不是");
    int k0 = touch_at(-1, -1, &fid);
    dm_bind_press(ml, 1); dm_bind_publish(cap); k = touch_at(-1, -1, &fid);
    CHECK(s_btn == 0 && k == k0, "Pointer 不由映射層推觸點（前端按指針位置推），也不按鍵");
    dm_bind_press(mr, 1); dm_bind_analog(ls2, 1, 0); dm_bind_publish(cap);
    CHECK(s_btn == DM_BTN_B && s_ax[0] == M, "放開前：B 按著、左搖桿推滿");
    dm_bind_reset(); dm_bind_publish(cap);
    CHECK(s_btn == 0 && ax_is(0, 0, 0, 0), "dm_bind_reset：全部放開、模擬量歸零（窗口失焦用）");
    printf("== %d 項，失敗 %d ==\n", checks, fails);
    return fails != 0;
}
