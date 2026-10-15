// 秘境結算處兩扇門的落點（純算法，不碰遊戲內存；dm_rift.c 用，tests/t_doorspot.c 測）。
// 要保證的事：門落在玩家「走得到」的格上。原先把「人背對守護者走 10 格、左右各錯 5 格」直接交給原版找空地 main+0x7160d0（掩碼 0x400 = 只看物件），
// 牆位（0x1）不在掩碼裡，也沒有任何可達檢查——洞穴 / 地穴這類窄圖裡人貼著牆站，10 格外就是牆裡或圖外（圖：地穴第一層第 26 層結算，兩扇門都在黑區）。
// 做法：以人為中心取一個窗口，blk[] = 這一格擋不擋玩家（調用方按玩家自己的碰撞掩碼填，窗口外一律算擋）；從人腳下那一格按「玩家佔一個十字」的走法做廣搜，
// 搜得到的格才是候選 = 必為可達。候選裡按三檔取離理想點最近的：
//   檔 0：周圍 2 格（5 × 5）全是可達格——門身不貼牆、人能繞著走；檔 1：周圍 1 格（3 × 3）；檔 2：只要可達，改成離人最近（窄到擺不開時退到腳下最近的可走格）。
// 都不成立（人被圍死在一格裡）就落在人腳下那一格。
// Landing points of the two doors at the rift completion (pure algorithm, does not touch game memory; used by dm_rift.c, tested by tests/t_doorspot.c).
// What must be guaranteed: the doors land on cells the player "can walk to". Previously "the player walks 10 cells with their back to the guardian, offset 5 cells to the left and to the right" was handed directly to the original game's free-ground search main+0x7160d0 (mask 0x400 = objects only),
// the wall bit (0x1) is not in the mask and there is no reachability check at all -- in narrow maps such as the Cave / the Pit the player stands right against a wall, and 10 cells away is inside the wall or outside the map (figure: completion of rift level 26 on Pit Level 1, both doors in the black area).
// Approach: take a window centered on the player, blk[] = whether this cell blocks the player (the caller fills it by the player's own collision mask; everything outside the window counts as blocked); from the cell at the player's feet do a breadth-first search with the "player occupies a cross" way of walking,
// and only the cells the search reaches are candidates = necessarily reachable. Among the candidates take the one nearest to the ideal point, in three tiers:
//   tier 0: the 2 cells around it (5 × 5) are all reachable cells -- the door body does not touch a wall and the player can walk around it; tier 1: the 1 cell around it (3 × 3); tier 2: reachable is enough, switched to nearest to the player (when too narrow to fit, fall back to the nearest walkable cell by the feet).
// If none holds (the player is boxed into a single cell), land on the cell at the player's feet.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { DS_R = 24, DS_W = 2 * DS_R + 1, DS_N = DS_W * DS_W };   // 窗口半徑 24 格：理想點在 10 格外再錯開 5 格，留一倍餘量 | window radius 24 cells: the ideal point is 10 cells out and then offset by another 5 cells, leaving a margin of as much again
enum { DS_FAR = 36 };       // 檔 0 / 1 的門離人最多走這麼多步（直線約 11 格；繞過一道牆才到的不要） | doors of tier 0 / 1 are at most this many steps of walking from the player (about 11 cells in a straight line; ones reached only by going around a wall are not wanted)
enum { DS_SELF_GAP = 3 };   // 檔 0 / 1 的門離人至少 3 格（不壓在人和腳下的首通獎勵上） | doors of tier 0 / 1 are at least 3 cells from the player (not on top of the player and the first-clear reward at the player's feet)
struct ds_grid { uint8_t blk[DS_N]; int16_t dist[DS_N]; int reach; };   // blk 調用方填；dist / reach 由 ds_flood 算（-1 = 走不到） | blk is filled by the caller; dist / reach are computed by ds_flood (-1 = unreachable)

static inline int ds_blk(const struct ds_grid *g, int ix, int iy) { return ix < 0 || iy < 0 || ix >= DS_W || iy >= DS_W || g->blk[iy * DS_W + ix]; }
// 玩家佔一個十字（中心 + 四鄰），與點地尋路同口徑（dm_world.c wp_free）
// The player occupies a cross (center + four neighbors), same convention as ground-tap pathfinding (dm_world.c wp_free)
static inline int ds_free(const struct ds_grid *g, int ix, int iy) { return !(ds_blk(g, ix, iy) || ds_blk(g, ix + 1, iy) || ds_blk(g, ix - 1, iy) || ds_blk(g, ix, iy + 1) || ds_blk(g, ix, iy - 1)); }
static inline int ds_dist(const struct ds_grid *g, int ix, int iy) { return ix < 0 || iy < 0 || ix >= DS_W || iy >= DS_W ? -1 : g->dist[iy * DS_W + ix]; }
// 從窗口中心（人腳下）廣搜。起點格不要求十字都空：人此刻就站在那裡
// Breadth-first search from the window center (the player's feet). The start cell is not required to have its whole cross free: the player is standing right there at this moment
static inline void ds_flood(struct ds_grid *g) {
    static uint16_t q[DS_N]; int h = 0, t = 0; memset(g->dist, 0xff, sizeof g->dist); g->reach = 0;
    g->dist[DS_R * DS_W + DS_R] = 0; q[t++] = (uint16_t)(DS_R * DS_W + DS_R);
    while (h < t) { int c = q[h++], cx = c % DS_W, cy = c / DS_W; g->reach++;
        static const int d4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) { int nx = cx + d4[k][0], ny = cy + d4[k][1]; if (ds_dist(g, nx, ny) != -1 || nx < 0 || ny < 0 || nx >= DS_W || ny >= DS_W || !ds_free(g, nx, ny)) continue;
            g->dist[ny * DS_W + nx] = (int16_t)(g->dist[c] + 1); q[t++] = (uint16_t)(ny * DS_W + nx); } }
}
static inline int ds_clear(const struct ds_grid *g, int ix, int iy, int k) { for (int dy = -k; dy <= k; dy++) for (int dx = -k; dx <= k; dx++) if (ds_dist(g, ix + dx, iy + dy) < 0) return 0; return 1; }
// 取一扇門的落點。坐標都是相對人的格偏移（人 = 0,0）。(wx, wy) 理想點；have_o 時 (ox, oy) 是已經定下的另一扇門，新門離它至少 gap 格（檔 2 降到 2 格）。
// 返回用了第幾檔（0 / 1 / 2；3 = 落在人腳下），落點寫進 *rx / *ry
// Pick the landing point of one door. All coordinates are cell offsets relative to the player (player = 0,0). (wx, wy) is the ideal point; when have_o, (ox, oy) is the other door already decided, and the new door is at least gap cells away from it (tier 2 lowers this to 2 cells).
// Returns which tier was used (0 / 1 / 2; 3 = landed at the player's feet); the landing point is written to *rx / *ry
static inline int ds_pick(const struct ds_grid *g, int wx, int wy, int have_o, int ox, int oy, int gap, int *rx, int *ry) {
    for (int tier = 0; tier < 3; tier++) { long best = -1; int bx = 0, by = 0, tx = tier == 2 ? 0 : wx, ty = tier == 2 ? 0 : wy;
        for (int y = -DS_R; y <= DS_R; y++) for (int x = -DS_R; x <= DS_R; x++) { int d = ds_dist(g, x + DS_R, y + DS_R); if (d < 0) continue;
            int far = abs(x) > abs(y) ? abs(x) : abs(y), og = !have_o ? 1 << 20 : abs(x - ox) > abs(y - oy) ? abs(x - ox) : abs(y - oy);
            if (tier < 2 ? (far < DS_SELF_GAP || d > DS_FAR || og < gap || !ds_clear(g, x + DS_R, y + DS_R, 2 - tier)) : (far < 1 || og < 2)) continue;
            long s = (long)(x - tx) * (x - tx) + (long)(y - ty) * (y - ty); if (best >= 0 && s >= best) continue; best = s; bx = x; by = y; }
        if (best >= 0) { *rx = bx; *ry = by; return tier; } }
    *rx = 0; *ry = 0; return 3;
}
