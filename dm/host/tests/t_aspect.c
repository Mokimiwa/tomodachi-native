// 畫面寬高比（dm_aspect.c）：寫法解析與輸出尺寸改形狀。朋友收集的檔案值：主機 1920x1080、掌機 1280x720、寬度上限 2580、高度上限 1350
// Picture aspect ratio (dm_aspect.c): parsing the syntax and reshaping the output size. Tomodachi Life's profile values: docked 1920x1080, handheld 1280x720, width cap 2580, height cap 1350
#include "dm_aspect.h"
#include <math.h>
#include <stdio.h>

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
static unsigned W(uint64_t r) { return (uint32_t)r; }
static unsigned H(uint64_t r) { return (uint32_t)(r >> 32); }
static int near(double a, double b) { return fabs(a - b) < 1e-9; }

int main(void) {
    const double wide = 2580.0 / 1080, tall = 1920.0 / 1350;
    CHECK(dm_aspect_parse(NULL, 2) == 0 && dm_aspect_parse("", 2) == 0, "沒給 → 0（按 16:9） | none → 0 (16:9)");
    CHECK(near(dm_aspect_parse("21:9", 0), 21.0 / 9), "「21:9」 | \"21:9\"");
    CHECK(near(dm_aspect_parse("19.5:9", 0), 19.5 / 9), "「19.5:9」 | \"19.5:9\"");
    CHECK(near(dm_aspect_parse("2340x1080", 0), 2340.0 / 1080), "「2340x1080」 | \"2340x1080\"");
    CHECK(near(dm_aspect_parse("2.39", 0), 2.39), "「2.39」 | \"2.39\"");
    CHECK(near(dm_aspect_parse("screen", 1.6), 1.6) && dm_aspect_parse("Screen", 0) == 0, "「screen」取前端量到的形狀，量不到 → 0 | \"screen\" takes the measured shape, unknown → 0");
    CHECK(dm_aspect_parse("wide", 2) == 0 && dm_aspect_parse("16:", 2) == 0 && dm_aspect_parse("0:9", 2) == 0 && dm_aspect_parse("21:9x", 2) == 0, "壞值 → 0 | malformed → 0");

    uint64_t r = dm_aspect_res(1920, 1080, wide, tall, 0);
    CHECK(W(r) == 1920 && H(r) == 1080, "比例 0 → 原尺寸 1920x1080 | ratio 0 → the original 1920x1080");
    r = dm_aspect_res(1920, 1080, wide, tall, 16.0 / 9);
    CHECK(W(r) == 1920 && H(r) == 1080, "16:9 → 1920x1080 不變 | 16:9 → 1920x1080 unchanged");
    r = dm_aspect_res(1920, 1080, wide, tall, 19.5 / 9);
    CHECK(W(r) == 2340 && H(r) == 1080, "19.5:9（iPhone 橫放）→ 2340x1080 | 19.5:9 (iPhone landscape) → 2340x1080");
    r = dm_aspect_res(1920, 1080, wide, tall, 2580.0 / 1080);
    CHECK(W(r) == 2580 && H(r) == 1080, "2580/1080 剛好到上限 → 2580（不因浮點誤差掉成 2576） | exactly the cap → 2580 (not 2576 through float error)");
    r = dm_aspect_res(1920, 1080, wide, tall, 32.0 / 9);
    CHECK(W(r) == 2580, "32:9 → 夾到上限 2580 | 32:9 → clamped to the cap 2580");
    r = dm_aspect_res(1920, 1080, wide, tall, 2670.0 / 1878);
    CHECK(W(r) == 1920 && H(r) == 1350, "iPhone Duo 內屏 2670x1878 → 1920x1350（到高度上限） | iPhone Duo inner screen 2670x1878 → 1920x1350 (at the height cap)");
    r = dm_aspect_res(1920, 1080, wide, tall, 16.0 / 10);
    CHECK(W(r) == 1920 && H(r) == 1200, "16:10（Mac）→ 1920x1200 | 16:10 (Mac) → 1920x1200");
    r = dm_aspect_res(1920, 1080, wide, tall, 4.0 / 3);
    CHECK(W(r) == 1920 && H(r) == 1350, "4:3 → 夾到高度上限 1350 | 4:3 → clamped to the height cap 1350");
    r = dm_aspect_res(1920, 1080, wide, 0, 1.42);
    CHECK(W(r) == 1920 && H(r) == 1080, "檔案不加高（上限 0）→ 1920x1080 | profile without growth (cap 0) → 1920x1080");
    r = dm_aspect_res(1280, 720, wide, tall, 2670.0 / 1878);
    CHECK(W(r) == 1280 && H(r) == 900, "掌機 Duo → 1280x900 | handheld Duo → 1280x900");
    r = dm_aspect_res(1920, 1080, 0, 0, 21.0 / 9);
    CHECK(W(r) == 1920, "檔案不放寬（上限 0）→ 1920 | profile without widening (cap 0) → 1920");
    r = dm_aspect_res(1920, 1080, wide, tall, 2532.0 / 1170);
    CHECK(W(r) == 2336 && W(r) % 4 == 0, "iPhone 2532x1170 → 2336（4 的倍數） | iPhone 2532x1170 → 2336 (a multiple of 4)");
    r = dm_aspect_res(1280, 720, wide, tall, 19.5 / 9);
    CHECK(W(r) == 1560 && H(r) == 720, "掌機 19.5:9 → 1560x720 | handheld 19.5:9 → 1560x720");
    r = dm_aspect_res(1280, 720, wide, tall, 3.0);
    CHECK(W(r) == 1720, "掌機上限按同一比例 → 1720 | handheld cap at the same ratio → 1720");
    printf("%d / %d 通過 | passed\n", checks - fails, checks);
    return fails != 0;
}
