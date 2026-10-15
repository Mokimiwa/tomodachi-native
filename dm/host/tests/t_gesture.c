// 手勢識別單測（hle_gesture.c）：按假時鐘送觸點，核對產出的手勢類型 / 方向 / 屬性 / 縮放。期望按 hle_gesture.c 開頭的規則手寫：
// 點按 = Touch → Tap；夠近夠快的第二次點按帶 IsDoubleTap；按住 0.5 s = Press，抬起 Complete；移動過 10 點 = Pan，抬手時快 = Swipe（帶方向）；兩指 = Pinch。
// Gesture recognition unit test (hle_gesture.c): feed touch points on a fake clock and check the produced gesture types / directions / attributes / scale. Expectations are hand-written from the rules at the top of hle_gesture.c:
// tap = Touch → Tap; a second tap close enough in time and place carries IsDoubleTap; held 0.5 s = Press, release Complete; moving past 10 points = Pan, fast at release = Swipe (with a direction); two fingers = Pinch.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint64_t g_clock;
uint64_t now_ns(void) { return g_clock; }
#include "../hle_gesture.c"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (c) printf("  ✓ "); else { fails++; printf("  ✗ "); } printf(__VA_ARGS__); printf("\n"); } while (0)
static const uint64_t MS = 1000000ull;
static void put(int n, int x0, int y0, int x1, int y1, uint64_t t) { int32_t p[2][2] = {{x0, y0}, {x1, y1}}; g_clock = t; gesture_feed((const int32_t (*)[2])p, n, t); }
// 最新的一條 | the newest entry
static gstate last(void) { gstate s[1]; memset(s, 0, sizeof s); h_hid_Gestures(s, 1); return s[0]; }
static int64_t g_seen;
// 自上次以來的新條目（舊的在前）| entries since the last call (oldest first)
static int fresh(gstate *out, int cap) {
    gstate all[RING]; int n = h_hid_Gestures(all, RING), k = 0;
    for (int i = n - 1; i >= 0; i--) if (all[i].sampling > g_seen && k < cap) out[k++] = all[i];
    if (n) g_seen = all[0].sampling;
    return k;
}

int main(void) {
    gstate e[8]; int n;
    put(1, 640, 360, 0, 0, 1000 * MS); put(0, 0, 0, 0, 0, 1080 * MS); n = fresh(e, 8);
    CHECK(n == 2 && e[0].type == G_TOUCH && (e[0].attr & A_NEW) && e[1].type == G_TAP && !(e[1].attr & A_DOUBLE) && e[1].x == 640 && e[1].y == 360,
          "短按：Touch（IsNewTouch）→ Tap，在 (640, 360)");
    int64_t c1 = e[1].context;
    put(1, 650, 365, 0, 0, 1300 * MS); put(0, 0, 0, 0, 0, 1350 * MS); n = fresh(e, 8);
    CHECK(n == 2 && e[1].type == G_TAP && (e[1].attr & A_DOUBLE) && e[1].context == c1 + 1, "0.22 s 內、10 點內再點：Tap 帶 IsDoubleTap，檢測號 +1");
    put(1, 100, 100, 0, 0, 3000 * MS); put(1, 102, 101, 0, 0, 3300 * MS); g_clock = 3600 * MS; (void)last(); put(0, 0, 0, 0, 0, 3700 * MS); n = fresh(e, 8);
    CHECK(n == 3 && e[0].type == G_TOUCH && e[1].type == G_PRESS && e[2].type == G_COMPLETE, "按住 0.6 s（只抖 2 點）：Touch → Press（讀手勢時補判）→ 抬起 Complete");
    put(1, 100, 100, 0, 0, 5000 * MS); put(1, 130, 100, 0, 0, 5050 * MS); put(1, 400, 110, 0, 0, 5100 * MS); put(0, 0, 0, 0, 0, 5120 * MS); n = fresh(e, 8);
    CHECK(n == 4 && e[1].type == G_PAN && e[2].type == G_PAN && e[2].dx == 270 && e[3].type == G_SWIPE && e[3].dir == D_RIGHT, "快速右拖：Pan、Pan（dx 270）→ Swipe 向右");
    put(1, 100, 100, 0, 0, 7000 * MS); put(1, 140, 100, 0, 0, 7500 * MS); put(1, 150, 100, 0, 0, 7900 * MS); put(0, 0, 0, 0, 0, 8200 * MS); n = fresh(e, 8);
    CHECK(n >= 3 && e[n - 1].type == G_COMPLETE, "慢拖、停手後才抬起：收尾 Complete（不算快滑）");
    put(2, 500, 300, 600, 300, 9000 * MS); put(2, 450, 300, 650, 300, 9100 * MS); n = fresh(e, 8);
    CHECK(n == 2 && e[1].type == G_PINCH && e[1].npt == 2 && e[1].scale > 1.99f && e[1].scale < 2.01f, "兩指從相距 100 張到 200：Pinch，scale 2（%.2f）", n == 2 ? e[1].scale : 0.f);
    put(0, 0, 0, 0, 0, 9200 * MS); n = fresh(e, 8);
    CHECK(n == 1 && e[0].type == G_COMPLETE, "兩指抬起：Complete");
    printf("== %d 項，失敗 %d ==\n", checks, fails);
    return fails != 0;
}
