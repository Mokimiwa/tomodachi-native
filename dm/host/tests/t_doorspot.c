// 秘境結算門落點的單測（dm_doorspot.h，秒級）。期望值都是按圖手算的，不跑被測實現。
// 現場：洞穴 / 地穴類窄圖，人貼牆站，舊規則的理想點（背對守護者 10 格、左右錯 5 格）落在牆裡。
// 紅 → 綠：同一張圖上，舊規則的落點是擋格（old_spot_blocked）；新規則的落點必須是可達格。
// Unit test of the rift completion door landing points (dm_doorspot.h, takes seconds). The expected values are all computed by hand from the maps, without running the implementation under test.
// Situation: narrow maps of the Cave / Pit kind, the player standing against a wall, and the old rule's ideal point (10 cells with the back to the guardian, offset 5 cells left / right) lands inside a wall.
// Fail → pass: on the same map, the old rule's landing point is a blocked cell (old_spot_blocked); the new rule's landing point must be a reachable cell.
#include "dm_doorspot.h"
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct ds_grid g;
static void fill(int v) { memset(g.blk, v, sizeof g.blk); }
static void rect(int x0, int y0, int x1, int y1, int v) { for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (x >= -DS_R && x <= DS_R && y >= -DS_R && y <= DS_R) g.blk[(y + DS_R) * DS_W + x + DS_R] = (uint8_t)v; }
static int blk(int x, int y) { return ds_blk(&g, x + DS_R, y + DS_R); }
static int dist(int x, int y) { return ds_dist(&g, x + DS_R, y + DS_R); }

int main(void) {
    int x, y, t;
    // ① 空地：理想點本身合規 → 原樣取到（野外圖不退化）
    // ① Open ground: the ideal point itself qualifies → taken as is (wilderness maps do not degrade)
    fill(0); ds_flood(&g);
    t = ds_pick(&g, 12, -3, 0, 0, 0, 6, &x, &y); CHECK(t == 0 && x == 12 && y == -3, "空地上應取理想點 12,-3 檔 0，得到 %d,%d 檔 %d", x, y, t);
    t = ds_pick(&g, 2, 12, 1, 12, -3, 6, &x, &y); CHECK(t == 0 && x == 2 && y == 12, "空地上第二扇應取理想點 2,12，得到 %d,%d 檔 %d", x, y, t);
    CHECK(dist(12, -3) == 15, "空地上 12,-3 走 15 步，算出 %d", dist(12, -3));

    // ② 橫向走廊（y −4..4 可走，其餘是牆），人在中間，理想點在正下方牆裡
    // ② Horizontal corridor (y −4..4 walkable, the rest is wall), the player in the middle, the ideal point inside the wall straight below
    fill(1); rect(-DS_R, -4, DS_R, 4, 0); ds_flood(&g);
    CHECK(blk(5, 10) && blk(-5, 10), "對照：舊規則的兩個落點 ±5,10 在牆裡");
    // 十字佔位 → 可達的 y 是 −3..3；周圍 2 格都可達 → y −1..1。離 (5,10) 最近的是 (5,1)
    // Cross footprint → the reachable y is −3..3; the 2 cells around all reachable → y −1..1. The nearest to (5,10) is (5,1)
    t = ds_pick(&g, 5, 10, 0, 0, 0, 6, &x, &y); CHECK(t == 0 && x == 5 && y == 1, "走廊裡應取 5,1 檔 0，得到 %d,%d 檔 %d", x, y, t);
    CHECK(dist(x, y) >= 0 && !blk(x, y), "落點必須可達且不擋");
    int x2, y2; t = ds_pick(&g, -5, 10, 1, x, y, 6, &x2, &y2); CHECK(t == 0 && x2 == -5 && y2 == 1, "走廊裡第二扇應取 -5,1，得到 %d,%d 檔 %d", x2, y2, t);

    // ③ 牆那邊有一片空地但和人不連通：不許選過去
    // ③ There is a patch of open ground beyond the wall but it is not connected to the player: it must not be chosen
    fill(1); rect(-DS_R, -4, DS_R, 4, 0); rect(-DS_R, 8, DS_R, 16, 0); ds_flood(&g);
    CHECK(!blk(5, 10) && dist(5, 10) < 0, "對照：5,10 不擋、但從人這裡走不到");
    t = ds_pick(&g, 5, 10, 0, 0, 0, 6, &x, &y); CHECK(dist(x, y) >= 0 && y <= 1, "隔牆空地不許選，應留在人這一側，得到 %d,%d", x, y);

    // ④ 窄道（3 格寬：y −1..1）：擺不出 5 × 5、3 × 3 → 檔 2，取離人最近的可達格（十字佔位下只有 y = 0 一行可達，離人 1 格）
    // ④ Narrow passage (3 cells wide: y −1..1): neither 5 × 5 nor 3 × 3 fits → tier 2, take the reachable cell nearest to the player (with the cross footprint only the row y = 0 is reachable, 1 cell from the player)
    fill(1); rect(-DS_R, -1, DS_R, 1, 0); ds_flood(&g);
    t = ds_pick(&g, 5, 10, 0, 0, 0, 6, &x, &y); CHECK(t == 2 && y == 0 && abs(x) == 1, "窄道裡應退到腳下最近的可達格（±1,0）檔 2，得到 %d,%d 檔 %d", x, y, t);
    t = ds_pick(&g, -5, 10, 1, x, y, 6, &x2, &y2); CHECK(t == 2 && y2 == 0 && abs(x2 - x) >= 2 && dist(x2, y2) >= 0, "窄道裡第二扇離第一扇至少 2 格且可達，得到 %d,%d 檔 %d", x2, y2, t);

    // ⑤ 人被圍死在一格裡：落在腳下
    // ⑤ The player is boxed into a single cell: land at the feet
    fill(1); rect(0, 0, 0, 0, 0); ds_flood(&g);
    t = ds_pick(&g, 5, 10, 0, 0, 0, 6, &x, &y); CHECK(t == 3 && x == 0 && y == 0 && g.reach == 1, "圍死時落在腳下檔 3，得到 %d,%d 檔 %d（可達 %d 格）", x, y, t, g.reach);

    // ⑥ 要繞遠路才到的格不選：理想點在一道長牆背面，繞過去超過 36 步
    // ⑥ Cells reached only by a long detour are not chosen: the ideal point is behind a long wall, and going around takes more than 36 steps
    fill(0); rect(-20, 6, 20, 7, 1); ds_flood(&g);
    CHECK(dist(0, 12) > DS_FAR, "對照：牆背面 0,12 要走 %d 步（> %d）", dist(0, 12), DS_FAR);
    t = ds_pick(&g, 0, 12, 0, 0, 0, 6, &x, &y); CHECK(t == 0 && abs(x) == 3 && y == 2, "長牆前應取牆這一側的 ±3,2（y = 2 是周圍 2 格都可達的最靠牆一行；離人至少 3 格所以 x = ±3），得到 %d,%d 檔 %d", x, y, t);

    printf("t_doorspot：%d 項，%d 項不通\n", checks, fails); return fails != 0;
}
