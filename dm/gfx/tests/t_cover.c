// 全覆蓋首繪幾何證明單測（nvn_cover.h）：期望值按幾何手算，不跑被測實現。
// 正例：全屏大三角形（-1,-1)(3,-1)(-1,3)、全屏四邊形條帶；負例：視口只佔附件一部分、剪裁只佔一部分、三角形缺一角、
// 被背面剔除、z 越界、w ≠ 1、兩三角形各包一半（單個三角形都包不住四角，按規則不成立——保守）。
// Unit tests for the geometric proof of a full-coverage first draw (nvn_cover.h): expected values are hand-calculated from geometry, without running the implementation under test.
// Positive cases: fullscreen big triangle (-1,-1)(3,-1)(-1,3), fullscreen quad strip; negative cases: viewport covers only part of the attachment, scissor covers only part, triangle missing a corner,
// back-face culled, z out of range, w ≠ 1, two triangles each covering half (no single triangle covers all four corners, so by the rule it does not hold - conservative).
#include "../nvn/nvn_cover.h"
#include <stdio.h>

static int fail, total;
static void t(const char *name, int want, int got) {
    total++; fail += want != got;
    printf("%-44s 期望 %s → 真結果 %s %s\n", name, want ? "成立" : "不成立", got ? "成立" : "不成立", want == got ? "✓" : "✗");
}
int main(void) {
    const float big[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};        // 逆時針（NDC y 向上） | counterclockwise (NDC y up)
    const float big_cw[3][4] = {{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}};     // 同一三角形，順時針 | same triangle, clockwise
    const float quad[4][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}};   // 條帶：兩三角形各半屏 | strip: two triangles each covering half the screen
    const float small[3][4] = {{-1, -1, 0, 1}, {1.9f, -1, 0, 1}, {-1, 1.9f, 0, 1}}; // 斜邊 x+y=0.9 切掉 (1,1) 角 | hypotenuse x+y=0.9 cuts off the (1,1) corner
    const float zout[3][4] = {{-1, -1, 1.5f, 1}, {3, -1, 1.5f, 1}, {-1, 3, 1.5f, 1}};
    const float w2[3][4] = {{-2, -2, 0, 2}, {6, -2, 0, 2}, {-2, 6, 0, 2}};
    const float vp_full[4] = {0, 0, 1600, 900}, vp_half[4] = {0, 0, 800, 900}, vp_off[4] = {0, 450, 1600, 900};
    const int sc_full[4] = {0, 0, 1600, 900}, sc_part[4] = {0, 0, 1600, 450};
    t("全屏三角形，視口 = 整塊，不剔除", 1, nvn_cover(big, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("全屏三角形，無視口設置（缺省整塊）", 1, nvn_cover(big, 3, 4, 0, 1, 0, NULL, 0, NULL, 1600, 900));
    t("全屏三角形，剔背面，逆時針為正面", 1, nvn_cover(big, 3, 4, 2, 1, 1, vp_full, 1, sc_full, 1600, 900));
    t("負：同一三角形順時針，剔背面（被剔）", 0, nvn_cover(big_cw, 3, 4, 2, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("負：視口只佔附件左半", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_half, 0, NULL, 1600, 900));
    t("負：視口下移（不從 0 起）", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_off, 0, NULL, 1600, 900));
    t("負：視口整塊 1600x900，附件是 1920x1080", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1920, 1080));
    t("負：剪裁只佔上半", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_full, 1, sc_part, 1600, 900));
    t("負：三角形缺 (1,1) 角", 0, nvn_cover(small, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("負：z 超出 [0,1]", 0, nvn_cover(zout, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("負：w ≠ 1", 0, nvn_cover(w2, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("負：全屏四邊形條帶（單個三角形包不住，保守）", 0, nvn_cover(quad, 4, 5, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("負：正背面都剔", 0, nvn_cover(big, 3, 4, 3, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("負：圖元是線（非三角形）", 0, nvn_cover(big, 3, 1, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    printf("== %d 項，失敗 %d ==\n", total, fail);
    return fail != 0;
}
