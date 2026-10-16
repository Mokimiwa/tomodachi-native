// 畫面寬高比（啓動時選定，運行中不變）：前端按它給遊戲輸出分辨率（front_game_res），遊戲按這個尺寸原生渲染與排版。
// 寫法（環境變量 DM_ASPECT，或 App 設定 GameAspect）：「16:9」「21:9」這種比、「2.39」這種小數、「2340x1080」這種尺寸、「screen」= 屏幕本身的形狀；
// 沒給、寫錯都按 16:9。只往寬放：比 16:9 窄的屏幕仍出 16:9（上下補黑），比遊戲檔案的寬度上限還寬的夾到上限（兩側補黑）。
// Picture aspect ratio (chosen at launch, fixed while running): the front end gives the game its output resolution from it (front_game_res), and the game renders and lays out natively at that size.
// Syntax (environment variable DM_ASPECT, or the app setting GameAspect): a ratio such as "16:9" / "21:9", a decimal such as "2.39", a size such as "2340x1080", or "screen" = the screen's own shape;
// missing or malformed means 16:9. It only widens: screens narrower than 16:9 still get 16:9 (black bars above and below), and ones wider than the game profile's width cap are clamped to the cap (black bars at the sides).
#pragma once
#include <stdint.h>

// 解析上面的寫法；screen 是前端量到的屏幕寬高比（「screen」時用，0 = 量不到）。返回寬高比，解析不了返回 0
// Parse the syntax above; screen is the screen aspect ratio the front end measured (used for "screen", 0 = unknown). Returns the aspect ratio, or 0 when it cannot be parsed
double dm_aspect_parse(const char *s, double screen);

// 16:9 的輸出尺寸 w x h 按寬高比放寬：高不變，寬 = 高 × 比例取 4 的倍數，夾在 [w, 高 × max_aspect]；max_aspect ≤ w / h 時不放寬。
// 返回 (u64)高 << 32 | 寬（同 front_game_res）
// Widen the 16:9 output size w x h for an aspect ratio: the height stays, the width = height × ratio rounded to a multiple of 4, clamped to [w, height × max_aspect]; no widening when max_aspect ≤ w / h.
// Returns (u64)height << 32 | width (as front_game_res does)
uint64_t dm_aspect_res(unsigned w, unsigned h, double max_aspect, double aspect);

// 遊戲檔案（game.tsv render_size / render_wide_max）的輸出尺寸按寬高比放寬：兩個前端的 front_game_res 共用。要先包含 game_profile.h（dm.h 帶）
// The game profile's output sizes (game.tsv render_size / render_wide_max) widened for an aspect ratio: shared by both front ends' front_game_res. Include game_profile.h first (dm.h brings it)
#ifdef DM_GAME_RES_DOCKED_W
static inline uint64_t dm_aspect_game_res(int docked, double aspect) {
    double wide = DM_GAME_RES_WIDE_MAX ? (double)DM_GAME_RES_WIDE_MAX / DM_GAME_RES_DOCKED_H : 0;
    return docked ? dm_aspect_res(DM_GAME_RES_DOCKED_W, DM_GAME_RES_DOCKED_H, wide, aspect) : dm_aspect_res(DM_GAME_RES_HANDHELD_W, DM_GAME_RES_HANDHELD_H, wide, aspect);
}
#endif
