#include "dm_touch_controls.h"
#include "../../host/dm_input.h"
#include <math.h>
#include <string.h>

#include <stdatomic.h>
#define DM_TC_POINTER_MOVE_PT 10.0f
#define DM_TC_POINTER_HOLD_S 0.5
#define DM_TC_POINTER_TAP_S 0.35

/* 控件縮放。手機（畫面高 ≤ 440 點，最大的 iPhone 橫屏就是 440）：min(1, 寬/667, 高/375)，與原來一字不差。
   比手機高的畫面（iPad 上居中的 16:9 那塊，13 寸是 1366×768 點）：按畫面高度等比放大（高/440），否則搖桿與技能鈕還是手機的點數大小，
   在大畫面裡只剩一小簇（Mac 上按 iPad 尺寸出圖所見），而底欄與遊戲界面都是按畫面高度縮放的。
   這條放大只在前端顯式打開時生效（dm_tc_set_tall_scale，front_ios.m 建視圖時開）：Mac 宿主的觸屏用例通路（dm/host/touch_mac.c）是把像素數
   （2868×1320）當點數傳進來的，13 個用例的坐標按「縮放 = 1」寫；不加這道開關，1320 > 440 會把它們的控件放大 3 倍、主鈕挪位（skill_main 紅）。
   Widget scale. Phone (picture height ≤ 440 points; the largest iPhone in landscape is exactly 440): min(1, width/667, height/375), identical to before to the letter.
      Pictures taller than the phone's (the centered 16:9 block on iPad; 13-inch is 1366×768 points): scale up proportionally by picture height (height/440), otherwise the stick and skill buttons keep the phone's point size
      and are just a small cluster in the large picture (as seen when rendering at iPad size on Mac), while the bottom bar and the game UI both scale by picture height.
      This enlargement takes effect only when the front end turns it on explicitly (dm_tc_set_tall_scale, turned on when front_ios.m creates the view): the Mac host's touch test-case path (dm/host/touch_mac.c) passes pixel counts
      (2868×1320) in as points, and the coordinates of the 13 test cases are written for "scale = 1"; without this switch, 1320 > 440 would enlarge their widgets 3x and move the main button (skill_main red). */
static int g_tall_scale;
void dm_tc_set_tall_scale(int on) { g_tall_scale = !!on; }
float dm_tc_scale_for_size(float width, float height) { float k = fminf(1.0f, fminf(width / 667.0f, height / 375.0f)); return g_tall_scale && k >= 1.0f && height > 440.0f ? height / 440.0f : k; }
static float scale_for(const dm_tc_layout *l) { return dm_tc_scale_for_size(l->width, l->height); }

/* 安全區（iPhone 橫屏左右各有一條讓給靈動島 / 圓角的區域，17 Pro Max 實讀各 62 點）：右側技能鍵整組、左側搖桿按最小位移平移到全部落在安全區內，
   組內相對排布不變。島在哪一側取決於玩家怎麼拿，兩側都內縮。缺省 0（Mac 宿主、單測不設 = 原佈局）。
   Safe area (in iPhone landscape there is a strip on each of the left and right yielded to the Dynamic Island / rounded corners; read as 62 points each on a 17 Pro Max): the whole right-side skill key group and the left stick are translated by the minimum displacement so that they lie entirely inside the safe area,
      with the relative arrangement within the group unchanged. Which side the island is on depends on how the player holds the device, so both sides are inset. Default 0 (Mac host and unit tests do not set it = original layout). */
static float g_safe_l, g_safe_r;
/* 屏幕底邊歸系統的那一條（點）：從這裡起手往上划會被系統當成底邊手勢（iPad 上連著兩次就拉出程序塢），而移動恰好是同一處反復起手往上推。
   這一條裡起手不當搖桿、鬆手也不當點地；從上面起手的搖桿拖進來照常持有。只有 iPad 的前端會給非零值，手機恆為 0。
   The strip along the bottom edge of the screen that belongs to the system (points): a swipe up starting here is taken by the system as a bottom-edge gesture (on iPad two in a row pull out the Dock), and movement is exactly a repeated push upward starting from the same place.
      A touch starting in this strip is not treated as the stick, and its release is not treated as a ground tap; a stick drag that started above and moves in keeps its hold as usual. Only the iPad front end passes a non-zero value; on phones it is always 0. */
static float g_edge_b;
void dm_tc_set_edge_bottom(float bottom) { g_edge_b = isfinite(bottom) && bottom > 0 ? bottom : 0; }
void dm_tc_set_safe_insets(float left, float right) { g_safe_l = isfinite(left) && left > 0 ? left : 0; g_safe_r = isfinite(right) && right > 0 ? right : 0; }

void dm_tc_layout_make(dm_tc_layout *l, float width, float height) {
    memset(l, 0, sizeof(*l));
    l->width = isfinite(width) && width > 0 ? width : 0;
    l->height = isfinite(height) && height > 0 ? height : 0;
    float k = scale_for(l);
    float main_x = l->width - 56 * k, main_y = l->height - 82 * k;
    { float over = main_x + 40*k - (l->width - g_safe_r); if (over > 0) main_x -= over; }   /* 右側一組裡最靠右的是主技能鍵（半徑 40） | The rightmost one in the right-side group is the main skill key (radius 40) */
    l->target[DM_TC_MAIN] = (dm_tc_target){ main_x, main_y, 40*k, DM_BTN_A };
    /* 四分之一弧的單位向量：180°、150°、120°、90°。 | Unit vectors of the quarter arc: 180°, 150°, 120°, 90°. */
    const float dx[4] = {-1, -0.8660254038f, -0.5f, 0};
    const float dy[4] = {0, -0.5f, -0.8660254038f, -1};
    const uint64_t skill[4] = {DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_R};
    for (int i = 0; i < 4; ++i)
        l->target[DM_TC_SKILL1+i] = (dm_tc_target){main_x+112*k*dx[i], main_y+112*k*dy[i], 26*k, skill[i]};
    /* 底欄 = 底部 UI v4 六格（NativeHUD/v4，遊戲 HUD 按屏高縮放、以屏幕中線居中，所以按屏高換算）：
       效果圖 2868×1320 裡格心 x = 1090 + 120×格號、y = 1241.5、格寬 114；第 0 格難度，1–4 腰帶四列，5 功能入口（PLUS）。
       回城不在底欄（離藥水太近易誤觸）：改為技能弧最上面那顆（技能四）正上方的圓鈕，同半徑、間距 12，點下由宿主施放原版回城技能（dm_freetp.c），不出按鍵。
       Bottom bar = the six cells of bottom UI v4 (NativeHUD/v4; the game HUD scales by screen height and is centered on the screen centerline, so convert by screen height):
              in the 2868×1320 mock, cell center x = 1090 + 120×cell index, y = 1241.5, cell width 114; cell 0 difficulty, 1–4 the four belt columns, 5 the function entry (PLUS).
              Town portal is not in the bottom bar (too close to the potions, easy to hit by mistake): it is instead a round button directly above the topmost one on the skill arc (skill four), same radius, gap 12; on tap the host casts the original game's town portal skill (dm_freetp.c), with no key press. */
    /* 腰帶四列對十字鍵：左 = 第 1 列、上 = 第 2 列、下 = 第 3 列、右 = 第 4 列（原版實測：按「右」第 4 列減、按「下」第 3 列減；首版把後兩個寫反了，真機才撞到） | The four belt columns map to the D-pad: left = column 1, up = column 2, down = column 3, right = column 4 (measured in the original game: pressing "right" decrements column 4, pressing "down" decrements column 3; the first version had the last two swapped, which was only hit on a real device) */
    const uint64_t bottom[5] = {DM_BTN_LEFT, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_RIGHT, DM_BTN_PLUS};
    const int cell[5] = {1, 2, 3, 4, 5};
    float u = l->height / 1320.0f;
    for (int i = 0; i < 5; ++i)
        l->target[DM_TC_BELT1+i] = (dm_tc_target){l->width/2+(1090+120*cell[i]-1434)*u, 1241.5f*u, 57*u, bottom[i]};
    { const dm_tc_target *s4 = &l->target[DM_TC_SKILL4]; l->target[DM_TC_PORTAL] = (dm_tc_target){s4->x, s4->y-s4->r-12*k-26*k, 26*k, 0}; }
    /* 第 0 格難度與選擇條：選擇條 8 格同格寬、格距 120，中線對齊兩球中線（效果圖 1390），格心 y = 1051.5（七格之上、避開數值文字） | Cell 0 difficulty and the selector bar: the selector bar has 8 cells of the same cell width, cell pitch 120, centerline aligned with the centerline between the two orbs (mock 1390), cell center y = 1051.5 (above the seven cells, clear of the value text) */
    l->diff = (dm_tc_target){l->width/2+(1090-1434)*u, 1241.5f*u, 57*u, 0};
    for (int i = 0; i < 8; ++i) l->pick[i] = (dm_tc_target){l->width/2+(1390+(i-3.5f)*120-1434)*u, 1051.5f*u, 57*u, 0};
    l->target[DM_TC_MAP] = (dm_tc_target){l->width-30*k, 30*k, 22*k, DM_BTN_MINUS};
    /* 技能組切換：主技能左下、技能一下方，避開主技能與弧上四鍵 | Skill set switch: lower-left of the main skill, below skill one, clear of the main skill and the four keys on the arc */
    /* 位置與大小：技能一正下方，半徑 22（直徑 44 點 ≈ 7.3 毫米）；上沿離技能一下沿 4 點、下沿離屏幕底 10 點（956×440） | Position and size: directly below skill one, radius 22 (diameter 44 points ≈ 7.3 mm); top edge 4 points from the bottom edge of skill one, bottom edge 10 points from the bottom of the screen (956×440) */
    /* 比手機窄的畫面（16:9：iPad 留邊 / 真全屏的遊戲視圖、667×375）底欄按屏高縮放、右端雕像翼尖（效果圖 x 2075）會伸到換組鍵底下：
       鍵向右讓到離翼尖 8 點以外，大小不變（量：1366×768 下原位重疊 21 點；956×440 下本來就隔著 28 點，不動）
       On pictures narrower than the phone's (16:9: iPad letterboxed / true full-screen game view, 667×375) the bottom bar scales by screen height and the right-end statue wing tip (mock x 2075) would extend under the set-switch key:
              the key moves right to at least 8 points away from the wing tip, size unchanged (measured: at 1366×768 the original position overlaps by 21 points; at 956×440 it is already 28 points apart and is not moved) */
    { float sx = main_x-112*k, bar_r = l->width/2+(2015-1434)*u+8*k+22*k;   /* 右雕像翼尖：七格時 2075，六格石條右端收進 60 | Right statue wing tip: 2075 with seven cells; with six cells the right end of the stone bar is pulled in by 60 */ if (sx < bar_r) sx = bar_r;
      l->swap = (dm_tc_target){sx, main_y+52*k, 22*k, 0}; }
    /* 底欄外側是血 / 藍球；球不是輸入目標。 | Outside the bottom bar are the life / mana orbs; the orbs are not input targets. */
    l->joy_x = fmaxf(90*k, g_safe_l + 60*k);   /* 搖桿底盤（半徑 60）左沿不越過安全區左界 | The left edge of the stick base (radius 60) does not cross the left boundary of the safe area */
    l->joy_y = l->height - 100*k;
    l->joy_radius = 60*k;
}

int dm_tc_hit(const dm_tc_layout *l, float x, float y) {
    if (!isfinite(x) || !isfinite(y) || x < 0 || y < 0 || x > l->width || y > l->height)
        return DM_TC_NONE;
    for (int i = 0; i < DM_TC_COUNT; ++i) {
        const dm_tc_target *t = &l->target[i];
        float dx = x-t->x, dy = y-t->y;
        if (t->r > 0 && dx*dx+dy*dy <= t->r*t->r) return i;
    }
    float sx = x-l->swap.x, sy = y-l->swap.y;
    if (l->swap.r > 0 && sx*sx+sy*sy <= l->swap.r*l->swap.r) return DM_TC_SWAP;
    return DM_TC_NONE;
}

static int skill_target(int target) {
    return target >= DM_TC_MAIN && target <= DM_TC_SKILL4;
}

/* 拖動方向（單位向量，屏幕坐標）：拖動不過 10 點死區 = 沒有方向 | Drag direction (unit vector, screen coordinates): a drag that does not pass the 10-point dead zone = no direction */
static int aim_dir(const dm_tc_state *s, const dm_tc_finger *f, float *ux, float *uy) {
    float dx = f->x-f->aim_origin_x, dy = f->y-f->aim_origin_y, d = hypotf(dx, dy);
    if (!(d > 10 * scale_for(&s->layout))) return 0;
    *ux = dx/d; *uy = dy/d; return 1;
}

static int inside_map_cancel(const dm_tc_state *s, const dm_tc_finger *f) {
    const dm_tc_target *map = &s->layout.target[DM_TC_MAP];
    return map->r > 0 && fabsf(f->x-map->x) <= map->r && fabsf(f->y-map->y) <= map->r;
}

static int g_iso_fix = 1;
void dm_tc_set_iso_fix(int on) { g_iso_fix = !!on; }
static void update_aim(dm_tc_state *s) {
    s->rx = s->ry = 0;
    s->aim_slot = DM_TC_NONE;
    s->map_cancel_active = 0;
    if (s->navigation_mode) return;
    uint64_t latest = 0;
    for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {
        const dm_tc_finger *f = &s->finger[i];
        if (!f->active || !skill_target(f->target)) continue;
        if (inside_map_cancel(s, f)) s->map_cancel_active = 1;
        if (!f->aim_cancelled && f->aim_move_sequence > latest) {
            latest = f->aim_move_sequence;
            s->aim_slot = i;
        }
    }
    if (s->pulse_button) { s->rx = s->pulse_rx; s->ry = s->pulse_ry; return; }
    float radius = 60 * scale_for(&s->layout);
    if (s->aim_slot == DM_TC_NONE || radius <= 0) return;
    const dm_tc_finger *f = &s->finger[s->aim_slot];
    if (f->target != DM_TC_MAIN && hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y) <= 10 * scale_for(&s->layout)) return;   /* 技能鈕：拖動不過 10 點 = 點按，不給方向，原版自動選目標 | Skill button: a drag of no more than 10 points = tap, no direction given, the original game auto-targets */
    float dx = (f->x-f->aim_origin_x)/radius;
    float dy = (f->aim_origin_y-f->y)/radius;
    s->rx = (int32_t)lroundf(fmaxf(-1, fminf(1, dx))*DM_STICK_MAX);
    s->ry = (int32_t)lroundf(fmaxf(-1, fminf(1, dy))*DM_STICK_MAX);
}

static void update(dm_tc_state *s) {
    s->buttons = 0;
    for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {
        dm_tc_finger *f = &s->finger[i];
        if (f->active && !f->aim_cancelled && f->target >= 0 && f->target < DM_TC_COUNT) {
            uint64_t b = f->target==DM_TC_PORTAL && s->portal_pending ? DM_BTN_ZL : s->layout.target[f->target].button;
            /* 拖動定方向（核心手勢）。技能鈕：按下後先等 DM_TC_AIM_SETTLE_S 看是點按還是拖動——拖過死區就帶著右軸方向立刻鍵下，
               沒拖到點也鍵下（點按 = 原版自動選目標 / 朝向）；否則鍵先下、右軸後偏，按下那一髮朝的是舊朝向。
               主鈕：按下即鍵下，不等（它是最高頻的鍵，點按不該多等；按下就拖的第一髮是原生朝向，第二發起跟手）。
               按住期間方向再變由宿主的瞄準適配每拍重取，這裡不做「松—按」；判定時間不為「按下稍停再拖」加長。
               Drag to set direction (the core gesture). Skill button: after press, first wait DM_TC_AIM_SETTLE_S to see whether it is a tap or a drag -- once dragged past the dead zone, key down immediately with the right-axis direction,
                              and key down too if the time is reached without dragging (tap = the original game's auto target / facing); otherwise the key would go down first and the right axis deflect later, and the shot at press would aim at the old facing.
                              Main button: key down on press, no wait (it is the most frequent key and a tap should not wait longer; with press-then-drag the first shot uses the native facing, and from the second shot on it follows the finger).
                              Direction changes while held are re-read every tick by the host's aim adapter, so no "release-press" is done here; the decision time is not lengthened for "press, pause, then drag". */
            if (!s->navigation_mode && f->target >= DM_TC_SKILL1 && f->target <= DM_TC_SKILL4) {
                float ux, uy;
                if (!f->key_down && !aim_dir(s, f, &ux, &uy) && s->last_time_s - f->start_time_s < DM_TC_AIM_SETTLE_S) b = 0;
                else if (!f->key_down) { f->key_down = 1; f->key_down_s = s->last_time_s; }
            }
            /* 多排腰帶：腰帶格上滑 = 展開、下滑 = 收起，所以鍵不在按下時發；一律等鬆手那一刻按位移判（dm_tc_end_at）。不設「按住到點就喝」的計時：
               手慢一點的上滑 / 下滑會先被它判成喝藥（真機與 mini 各撞一次）。只有 1 排時照舊按下即喝
               Multi-row belt: swiping up on a belt cell = expand, swiping down = collapse, so the key is not sent on press; always wait for the moment of release and decide by displacement (dm_tc_end_at). No "drink once held long enough" timer:
                              a slightly slow swipe up / down would first be judged by it as drinking a potion (hit once each on a real device and on the second Mac). With only 1 row, still drink on press as before */
            /* 只有 1 排時也一律鬆手那一刻喝（和多排同一條路） | with only 1 row, also always drink at the moment of release (same path as multiple rows) */
            if (!s->navigation_mode && f->target >= DM_TC_BELT1 && f->target <= DM_TC_BELT4) b = 0;
            s->buttons |= b;
        }
    }
    s->buttons |= s->pulse_button;
    /* 第二套技能：ZL 是原版的換組修飾鍵。只在用技能的那一刻帶上——手指按著主鈕 / 技能鈕、或技能脈衝在發（含等著發）時；
       其餘時候不按：一直按著會讓十字（+）、腰帶、回城都變成「ZL + 鍵」的組合（Ⅱ 態下十字開不了面板）。
       ZL 先下、主鈕的鍵晚 DM_TC_ZL_LEAD_S 再下（同一幀按下遊戲只認主鍵 = 放成第一組的技能）。
       Second skill set: ZL is the original game's set-switch modifier. It is added only at the moment a skill is used -- while a finger is on the main button / a skill button, or a skill pulse is being sent (including waiting to be sent);
              it is not pressed the rest of the time: holding it all the time would turn the cross (+), the belt and town portal all into "ZL + key" combos (in state Ⅱ the cross could not open the panel).
              ZL goes down first, and the main button's key goes down DM_TC_ZL_LEAD_S later (pressed in the same frame, the game only recognizes the main key = casts the first set's skill). */
    if (s->skill_set && !s->navigation_mode) {
        /* 在發的脈衝只有是技能鍵（主鈕 / 技能鈕補的那一下）才帶 ZL：多排腰帶鬆手補的喝藥脈衝帶上 ZL 就成了原版「ZL + 腰帶鍵 = 餵傭兵」
           （本機 實跑：第二組下點腰帶，0x26 包目標字節 = 1、傭兵生命 642 → 762 漲、玩家不變）
           The pulse being sent carries ZL only if it is a skill button (the make-up press of the main button / a skill button): a drink pulse made up on release of a multi-row belt, if it carried ZL, would become the original game's "ZL + belt key = feed the mercenary"
                      (actual run on the local machine: tapping the belt under the second set, 0x26 packet target byte = 1, mercenary life rose 642 → 762, player unchanged) */
        /* 等著補發的技能也要 ZL 領先——但只在沒有別的脈衝在發時：腰帶脈衝在發時帶上 ZL 同樣成了「餵傭兵」（單測探針讀到 0x1100） | A skill waiting for a make-up send also needs ZL to lead -- but only when no other pulse is being sent: adding ZL while a belt pulse is being sent likewise becomes "feed the mercenary" (the unit-test probe read 0x1100) */
        int use = s->defer_button != 0 && !s->pulse_button;
        for (int t = DM_TC_MAIN; t <= DM_TC_SKILL4 && !use; ++t) if (s->pulse_button && s->pulse_button == s->layout.target[t].button) use = 1;
        /* 手指按著第二組技能時發「給玩家喝」的腰帶脈衝：ZL 還按著就成了原版「ZL + 腰帶鍵 = 餵傭兵」。ZL 和按著的技能鍵要一起讓開
           （只松 ZL 不松技能鍵 = 放成第一組的技能），而且要先讓開、隔一段再按腰帶鍵：同一拍裡「松 ZL + 按腰帶鍵」遊戲仍按 ZL 還在算
           （真局實測：發出的還是餵傭兵包——宿主取手柄處對按下沿做鎖存，這一拍採到的是 ZL 與腰帶鍵都在）。所以分三段：
           等著發（belt_gap_cell）→ 脈衝在發 → 收回後 ZL 先回來、領先滿 DM_TC_ZL_LEAD_S 技能鍵再回來（同一幀按下遊戲只認主鍵）
           The "drink for the player" belt pulse sent while a finger holds a skill of the second skill set: with ZL still held it becomes the original game's "ZL + belt key = feed the mercenary". ZL and the held skill key have to get out of the way together
           (releasing only ZL and not the skill key = casting the first set's skill), and they have to get out of the way first, with the belt key pressed after a gap: with "release ZL + press belt key" in the same tick the game still counts ZL as held
           (measured in a real run: what went out was still the feed-the-mercenary packet -- where the host fetches the controller it latches the press edge, and what was sampled this tick was both ZL and the belt key down). So there are three stages:
           waiting to send (belt_gap_cell) → the pulse is being sent → after it is withdrawn ZL comes back first, and the skill key comes back once ZL has led by a full DM_TC_ZL_LEAD_S (pressed in the same frame, the game only recognizes the main key) */
        int belt_pulse = s->belt_gap_cell != 0;
        for (int t = DM_TC_BELT1; t <= DM_TC_BELT4; ++t) if (s->pulse_button && s->pulse_button == s->layout.target[t].button) belt_pulse = 1;
        if (belt_pulse) s->belt_pulse_s = s->last_time_s;
        for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {
            const dm_tc_finger *f = &s->finger[i];
            if (!f->active || f->aim_cancelled || f->target < DM_TC_MAIN || f->target > DM_TC_SKILL4) continue;
            if (!belt_pulse) use = 1;
            if (belt_pulse || s->last_time_s - f->start_time_s < DM_TC_ZL_LEAD_S || (s->belt_pulse_s > 0 && s->last_time_s - s->belt_pulse_s < DM_TC_ZL_LEAD_S)) s->buttons &= ~s->layout.target[f->target].button;
        }
        if (use) s->buttons |= DM_BTN_ZL;
    }
    update_aim(s);
    s->lx = s->ly = 0;
    if (s->joy_slot < 0) return;
    dm_tc_finger *f = &s->finger[s->joy_slot];
    float dx = f->x-s->joy_origin_x, dy = s->joy_origin_y-f->y;
    /* 輸入行程不跟大屏的視覺放大走：拇指不隨屏幕變大。滿偏 60 點、死區 6 點，手機與 iPad 一致
       （原先乘視覺倍數，12.9 寸 iPad 要推 105 點 ≈ 20 mm 才滿偏，是手機的兩倍）。底盤和搖桿頭的大小仍按視覺倍數畫。
       Input travel does not follow the visual enlargement on large screens: the thumb does not grow with the screen. Full deflection 60 points, dead zone 6 points, same on phone and iPad
              (it used to be multiplied by the visual factor, so a 12.9-inch iPad needed a 105-point ≈ 20 mm push for full deflection, twice the phone's). The size of the base and the stick head is still drawn by the visual factor. */
    float in_k = fminf(1.0f, scale_for(&s->layout));
    float distance = hypotf(dx, dy), radius = 60 * in_k;
    float dead = 6 * in_k;
    if (distance <= dead || radius <= dead) return;
    float strength = fminf(1, (distance-dead)/(radius-dead));
    /* 斜向預修正：原版把搖桿做純 45° 旋轉成世界方向、不補償等距畫面的 2:1，人實際走的屏幕角 = atan(tan(搖桿角) / 2)（推 45° 走 26.6°）。
       手柄玩家看不見搖桿和畫面的對應，觸屏上手指畫的就是屏幕方向，所以送值前把縱向放大 2 倍再縮回原幅度：屏幕角 = 手指角，推多深不變。
       只動遊戲態的移動搖桿（導航態沒有搖桿值；技能方向走右軸，不經這裡）
       Diagonal pre-correction: the original game turns the stick into a world direction by a pure 45° rotation and does not compensate for the 2:1 of the isometric view, so the screen angle the character actually walks = atan(tan(stick angle) / 2) (push 45°, walk 26.6°).
              Controller players do not see the mapping between stick and picture, but on a touchscreen what the finger draws is the screen direction, so before sending the value the vertical component is doubled and then scaled back to the original magnitude: screen angle = finger angle, push depth unchanged.
              Only the movement stick in game state is affected (navigation state has no stick value; skill direction goes through the right axis and does not pass here) */
    float ex = dx/distance, ey = dy/distance;
    if (g_iso_fix) { float n = hypotf(ex, 2*ey); ex /= n; ey = 2*ey/n; }
    s->lx = (int32_t)lroundf(ex * strength * DM_STICK_MAX);
    s->ly = (int32_t)lroundf(ey * strength * DM_STICK_MAX);
}

static int pointer_finger(const dm_tc_state *s, const dm_tc_finger *f) {
    return s->navigation_mode && f->active && s->pointer_slot >= 0 &&
        &s->finger[s->pointer_slot] == f;
}

static void update_pointer(dm_tc_state *s) {
    if (!s->pointer_valid || s->pointer_slot < 0 || s->pointer_slot >= DM_TC_MAX_TOUCH) return;
    dm_tc_finger *f = &s->finger[s->pointer_slot];
    if (f->pointer_dragged && s->pointer_pickup_done && !f->shop_drag && !f->locked_drag) {
        s->pointer_x = f->x;
        s->pointer_y = f->y - DM_TC_POINTER_LIFT_PT * scale_for(&s->layout);
    }
}

/* 指針脈衝（kind 1 = 拖放的拿起，2 = 放下 / 點按）：指針先落到 (x,y) 再按 A。上一個脈衝還沒收（遊戲還沒取走）時不動指針、只記下，
   等前端在上一個收回且遊戲已看到一次「A 鬆開」之後調 dm_tc_pulse_resume 才開始——否則兩下 A 連成一下、或拿起落在放下的位置
   Pointer pulse (kind 1 = pick-up of a drag-and-drop, 2 = put-down / tap): the pointer first lands at (x,y), then A is pressed. While the previous pulse has not been released (the game has not consumed it yet) the pointer is not moved and the request is only recorded,
      and it starts only when the front end calls dm_tc_pulse_resume, after the previous one is released and the game has seen "A released" once -- otherwise two A presses merge into one, or the pick-up lands at the put-down position */
static void pointer_pulse_btn(dm_tc_state *s, float x, float y, int kind, uint64_t button, double hold_s) {
    if (!s->pulse_button) {
        s->pointer_x = x; s->pointer_y = y;
        s->pulse_button = button;
        s->pulse_hold_s = hold_s;
        s->pulse_rx = s->pulse_ry = 0;
        s->pointer_pulse_active = kind;
        ++s->pulse_epoch;
    } else {
        s->pointer_pulse_pending = kind;
        s->pointer_pending_x = x;
        s->pointer_pending_y = y;
        s->pointer_pending_button = button; s->pointer_pending_hold = hold_s;
    }
}
static void pointer_pulse_start(dm_tc_state *s, float x, float y, int kind) { pointer_pulse_btn(s, x, y, kind, DM_BTN_A, 0); }
void dm_tc_pointer_release(dm_tc_state *s, int kind) { s->pointer_release_item = kind == 1 || kind == 3 || kind == 4 || kind == 8 ? kind : 0; }
void dm_tc_pointer_release_at(dm_tc_state *s, float x, float y) { s->pointer_release_item = 5; s->release_x = x; s->release_y = y; }
void dm_tc_set_holding(dm_tc_state *s, int holding) { s->holding = !!holding; }
void dm_tc_pointer_item(dm_tc_state *s, int kind) {
    if (kind == 7) { s->pointer_shop = 1; kind = 1; }
    s->pointer_lock = kind == 10; if (kind == 10) kind = 1;
    s->pointer_stack = kind == 8; if (kind == 8) kind = 1;   /* 8 = 高級倉庫的成疊格：照物品區，只有雙擊不同 | 8 = stacked cell of the advanced stash: follows the item area, only double tap differs */   /* 7 = 商人貨架上的貨：別的都照物品區，只有起拖不同 | 7 = goods on the vendor shelf: everything else follows the item area, only drag start differs */
    s->pointer_item = (kind >= 1 && kind <= 3) || kind == 6 || kind == 11 ? kind : 0;   /* 11 = 這一下不點（底下是被蓋住的按鈕），但可以起滾動手勢（設置頁列表裡的文字 / 空處） | 11 = this tap does not click (a covered button is underneath), but it may start a scroll gesture (text / empty space in the settings page list) */
    if (s->mouse_click_mode && s->pointer_slot >= 0 && s->pointer_slot < DM_TC_MAX_TOUCH && s->finger[s->pointer_slot].active && !s->finger[s->pointer_slot].pointer_dragged) {
        if (kind == 1) { if (!s->pulse_button) { s->pointer_valid = 1; s->pointer_x = s->finger[s->pointer_slot].aim_origin_x; s->pointer_y = s->finger[s->pointer_slot].aim_origin_y; } }
        else s->pointer_valid = 0;   /* 不在物品區：放開手柄光標（前端據此清掉光標請求，不再推搖桿） | Not in the item area: release the controller cursor (based on this the front end clears the cursor request and stops pushing the stick) */
    }
}   /* 1 物品區 2 頁簽（點按不發鍵也不發鼠標，前端另調宿主切頁） | 1 item area, 2 tab (a tap sends neither a key nor a mouse click; the front end separately calls the host to switch pages) */
static _Atomic int g_dead_modal;
static _Atomic int g_vendor_open;   /* 商人交易頁開著：ZL 是切貨架頁簽，「再點同一處 = 比較」不發 | Vendor trade page open: ZL switches shelf tabs, so "tap the same spot again = compare" is not sent */
static _Atomic int g_close_stop;   /* 「面板外點按 = 按返回到面板關掉」途中彈出了確認框（名字以 Modal 結尾的面板打開）：後面的返回鍵不再按 | A confirmation box popped up (a panel whose name ends in Modal opened) in the middle of "tap outside the panel = press back until the panel closes": stop pressing the remaining back presses */
static _Atomic int g_tap_box;   /* 最近一次單擊的物品是魔盒（前端每拍從宿主同步） | The most recently single-tapped item is the cube (synced by the front end from the host every tick) */
void dm_tc_set_tap_box(int on) { atomic_store(&g_tap_box, on != 0); }
static _Atomic unsigned g_cur_stack;
void dm_tc_set_cur_stack(unsigned id) { atomic_store(&g_cur_stack, id); }
static _Atomic unsigned g_hover_unid;
void dm_tc_set_hover_unid(unsigned id) { atomic_store(&g_hover_unid, id); }
static _Atomic int g_imbue_open;   /* 單件任務槽面板開著（灌魔 / 打孔 / 個性化 / 古墓底座，同一個 ImbueItemsPanel） | Single-item quest slot panel open (imbue / socket / personalize / tomb pedestal, all the same ImbueItemsPanel) */
static _Atomic int g_cube_open;   /* 赫拉迪克方塊頁開著：雙擊 = 按住 Y（原版「移到另一側」：背包 → 方塊 / 方塊 → 背包），不是「使用 / 裝備」 | Horadric Cube page open: double tap = hold Y (the original game's "move to the other side": inventory → cube / cube → inventory), not "use / equip" */
static _Atomic int g_vault_open;   /* 儲藏箱開在第六頁「編年史」：雙擊背包物品 = 存進編年史倉庫（走 use_seq，宿主 ui_use_pump 判頁），不發原版「按住 Y」——那一頁遊戲當魔盒頁 | Stash open on page six 「編年史」(Chronicle): double-tapping an inventory item = store it in the Chronicle vault (goes through use_seq; the host's ui_use_pump determines the page); do not send the original game's "hold Y" -- the game treats that page as the cube page */
static _Atomic int g_bank_open;   /* 儲藏箱開著（前端從宿主讀遊戲面板樹的結果設進來，不靠開關消息）：雙擊 = 按住 Y（移到另一側） | Stash open (set by the front end from the host's reading of the game panel tree, not from open/close messages): double tap = hold Y (move to the other side) */
void dm_tc_set_panel_state(int bits) { atomic_store(&g_bank_open, (bits & 1) != 0); atomic_store(&g_vendor_open, (bits & 2) != 0); atomic_store(&g_dead_modal, (bits & 4) != 0); atomic_store(&g_imbue_open, (bits & 16) != 0); atomic_store(&g_cube_open, (bits & 32) != 0); atomic_store(&g_vault_open, (bits & 64) != 0); }   /* 位同 dm_cursor.h 的 DM_PS_*；宿主讀遊戲狀態得來 | Bits are the same as DM_PS_* in dm_cursor.h; obtained by the host reading the game state */

static void pointer_drag_begin(dm_tc_state *s, dm_tc_finger *f) {
    f->pointer_dragged = 1;
    if (s->pointer_lock) { f->locked_drag = 1; return; }   /* 單件槽佔用時從別的物品上起拖：不拿起，這次拖動什麼都不做 | Drag starting from another item while the single-item slot is occupied: do not pick up; this drag does nothing */
    if (s->pointer_shop || s->pointer_stack) { f->shop_drag = 1; s->pointer_x = 2; s->pointer_y = 2; update(s); return; }   /* 從貨架上的貨起拖：不拿起；光標挪到屏幕角上，讓貨上的說明框收掉（買的時候宿主再把光標放回這件貨上） | Drag starting from goods on the shelf: do not pick up; move the cursor to a screen corner so the tooltip on the goods closes (at purchase time the host puts the cursor back on that item) */
    s->pointer_pickup_done = 0;
    pointer_pulse_start(s, f->aim_origin_x, f->aim_origin_y, 1);
}

void dm_tc_cancel_all(dm_tc_state *s) {
    ++s->gesture_epoch; s->portal_pending=s->portal_phase=0; s->diff_open=0;
    memset(s->finger, 0, sizeof(s->finger));
    for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) s->finger[i].target = DM_TC_NONE;
    s->joy_slot = DM_TC_NONE;
    s->joy_origin_x = s->joy_origin_y = 0;
    s->buttons = 0;
    s->lx = s->ly = s->rx = s->ry = 0;
    s->aim_slot = DM_TC_NONE;
    s->aim_move_sequence = 0;
    s->map_cancel_active = 0;
    s->pulse_button = 0; s->pulse_rx = s->pulse_ry = 0;
    s->belt_q = 0; s->belt_gap_cell = 0;   /* 排著的、等著發的喝藥一起作廢 | queued and pending drinks are voided as well */   /* 等著判單雙擊的腰帶格、等著發的餵雇傭兵一起作廢 | The belt cell waiting for single/double tap decision and the pending mercenary feed are voided together */
    s->defer_button = 0;   /* 等著補發的那下技能也作廢：原先取消後 0.2 秒照樣放出來 | The skill press waiting for a make-up send is voided as well: previously it was still cast 0.2 seconds after the cancel */
    s->pointer_slot = DM_TC_NONE;
    s->pointer_valid = 0; s->pointer_x = s->pointer_y = 0;
    s->pointer_pickup_done = 0;
    s->pointer_pulse_active = s->pointer_pulse_pending = 0;
    s->pointer_pending_x = s->pointer_pending_y = 0;
}

int dm_tc_pulse_end(dm_tc_state *s, uint64_t epoch) {
    if (!s->pulse_button || s->pulse_epoch != epoch) return 0;
    int ended_pointer = s->pointer_pulse_active;
    if (s->defer_button && s->defer_at_s < s->last_time_s + DM_TC_ZL_LEAD_S) s->defer_at_s = s->last_time_s + DM_TC_ZL_LEAD_S;   /* 等著補發的技能：從這一刻起 ZL 才能領先（見 dm_tc_tick） | Skill waiting for a make-up send: ZL can start leading only from this moment (see dm_tc_tick) */
    s->pulse_button = 0; s->pulse_rx = s->pulse_ry = 0; s->pulse_hold_s = 0;
    s->pointer_pulse_active = 0;
    if (ended_pointer == 1) {
        s->pointer_pickup_done = 1;
        update_pointer(s);
    }
    update(s);
    return 1;
}

int dm_tc_pulse_pending(const dm_tc_state *s) { return !s->pulse_button && s->pointer_pulse_pending; }

int dm_tc_pulse_resume(dm_tc_state *s) {
    if (s->pulse_button || !s->pointer_pulse_pending) return 0;
    int kind = s->pointer_pulse_pending;
    s->pointer_pulse_pending = 0;
    pointer_pulse_btn(s, s->pointer_pending_x, s->pointer_pending_y, kind, s->pointer_pending_button ? s->pointer_pending_button : DM_BTN_A, s->pointer_pending_hold);
    s->pointer_pending_button = 0; s->pointer_pending_hold = 0;
    update(s);
    return 1;
}

void dm_tc_init(dm_tc_state *s, float width, float height) {
    memset(s, 0, sizeof(*s));
    s->pointer_release_item = 1;
    dm_tc_layout_make(&s->layout, width, height);
    s->enabled_targets = (1u << DM_TC_COUNT) - 1;
    dm_tc_cancel_all(s);
}

void dm_tc_resize(dm_tc_state *s, float width, float height) {
    uint64_t portal = s->layout.target[DM_TC_PORTAL].button;
    dm_tc_cancel_all(s);
    dm_tc_layout_make(&s->layout, width, height);
    s->layout.target[DM_TC_PORTAL].button = portal;
}

void dm_tc_enable_targets(dm_tc_state *s, uint32_t mask) {
    mask &= (1u << DM_TC_COUNT) - 1;
    if (mask == s->enabled_targets) return;
    dm_tc_cancel_all(s);
    s->enabled_targets = mask;
}

void dm_tc_set_joy_block(dm_tc_state *s, int block) { s->joy_block = block != 0; }
void dm_tc_set_navigation_mode(dm_tc_state *s, int navigation_mode) {
    navigation_mode = !!navigation_mode;
    if (navigation_mode == s->navigation_mode) return;
    dm_tc_cancel_all(s);
    s->navigation_mode = navigation_mode;
}

/* 系統取消了這根手指（通知橫幅、系統手勢）：不當鬆手——不補脈衝、不點世界、不點界面；只把它按著的鍵、搖桿、光標放掉 | The system cancelled this finger (notification banner, system gesture): do not treat it as a release -- no make-up pulse, no world tap, no UI click; only let go of the keys, stick and cursor it was holding */
void dm_tc_cancel_finger(dm_tc_state *s, int slot) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH) return;
    ++s->gesture_epoch;
    if (s->finger[slot].active && s->finger[slot].target == DM_TC_PORTAL) { dm_tc_cancel_portal(s); }
    if (slot == s->joy_slot && s->finger[slot].active) { s->joy_lost_s = s->last_time_s; s->joy_lost_x = s->finger[slot].x; s->joy_lost_y = s->finger[slot].y; }   /* 系統取消了搖桿那根手指：記下，緊接著在附近重新落下的接著當搖桿 | the system cancelled the joystick finger: record it, and a finger landing again nearby right afterwards continues as the joystick */
    s->finger[slot] = (dm_tc_finger){.target=DM_TC_NONE};
    if (slot == s->joy_slot) { s->joy_slot = DM_TC_NONE; s->joy_origin_x = s->joy_origin_y = 0; }
    if (slot == s->aim_slot) s->aim_slot = DM_TC_NONE;
    if (slot == s->pointer_slot) { s->pointer_item = 0; s->pointer_shop = 0; s->pointer_stack = 0; s->pointer_lock = 0; s->pointer_slot = DM_TC_NONE; s->pointer_valid = 0; }
    update(s);
}
void dm_tc_cancel_portal(dm_tc_state *s) {
    ++s->gesture_epoch; s->portal_pending=s->portal_phase=0;
    for (int i=0;i<DM_TC_MAX_TOUCH;++i)
        if(s->finger[i].active && s->finger[i].target==DM_TC_PORTAL)
            s->finger[i]=(dm_tc_finger){.target=DM_TC_NONE};
    update(s);
}
void dm_tc_set_portal_binding(dm_tc_state *s,uint64_t mask,uint64_t identity,uint64_t scene) {
    if(s->layout.target[DM_TC_PORTAL].button==mask && s->portal_binding_identity==identity && s->portal_scene_epoch==scene)return;
    dm_tc_cancel_portal(s);
    s->portal_binding_identity=identity;s->portal_scene_epoch=scene;
    s->layout.target[DM_TC_PORTAL].button=mask;update(s);
}
void dm_tc_set_portal_button(dm_tc_state *s,uint64_t mask) {
    dm_tc_set_portal_binding(s,mask,s->portal_binding_identity,s->portal_scene_epoch);
}
int dm_tc_portal_ack(dm_tc_state *s,uint64_t epoch,uint64_t identity,uint64_t scene) {
    if(!s->portal_pending || s->gesture_epoch!=epoch || s->portal_binding_identity!=identity || s->portal_scene_epoch!=scene)return 0;
    int active=0;for(int i=0;i<DM_TC_MAX_TOUCH;i++)active|=s->finger[i].active && s->finger[i].target==DM_TC_PORTAL;
    if(!active)return 0;
    s->portal_pending=0;s->portal_phase=2;update(s);return 1;
}

/* 腰帶展開著、這一點不在腰帶上（底排四格與展開的幾排之外）：前端據此在自己的手勢（小地圖塊、空位十字）之前先收腰帶並吃掉這一下 | Belt expanded and this point is not on the belt (outside the four bottom-row cells and the expanded rows): based on this the front end collapses the belt and consumes this tap before its own gestures (minimap block, empty-slot cross) */
int dm_tc_belt_outside(const dm_tc_state *s, float x, float y) {
    if (s->navigation_mode || !s->belt_open || s->belt_rows <= 1) return 0;
    int hit = dm_tc_hit(&s->layout, x, y); if (hit >= DM_TC_BELT1 && hit <= DM_TC_BELT4) return 0;
    float u = s->layout.height / 1320.0f;
    for (int r = 1; r < s->belt_rows && r < 4; ++r) for (int c = 0; c < 4; ++c) {
        const dm_tc_target *t = &s->layout.target[DM_TC_BELT1+c];
        if (fabsf(x - t->x) <= 60*u && fabsf(y - (t->y - (165.0f + 162.0f*(r-1))*u)) <= 53.75f*u) return 0;
    }
    return 1;
}
/* 腰帶點一下 = 自己喝，點幾下喝幾瓶（原版手感，不判雙擊）。底排 = 補一個腰帶鍵脈衝（不帶 ZL，第二組下也不帶）；上面幾排手柄沒有鍵 = belt_request 交宿主喝那一格。
   餵雇傭兵不在腰帶上做手勢：傭兵面板裡的原版入口和「自動給傭兵喝藥」設置照舊
   One tap on the belt = the player drinks, several taps drink several (original feel, no double-tap detection). Bottom row = one belt key pulse (without ZL, also under the second set); upper rows have no controller key = belt_request asks the host to drink that cell.
      Feeding the mercenary has no gesture on the belt: the game's own entry in the mercenary panel and the "auto-feed mercenary" setting are unchanged */
static void belt_fire(dm_tc_state *s, int cell) {
    if (cell >= 0x100) { s->belt_request = cell; return; }
    uint64_t btn = s->layout.target[DM_TC_BELT1 + (cell & 0xf)].button; if (!btn) return;
    if (s->pulse_button || s->defer_button || s->belt_gap_cell) { s->belt_q = cell; return; }   /* 有脈衝在發 / 有補發在等：排著（dm_tc_tick 空了再發） | A pulse is being sent / a make-up send is waiting: queue it (dm_tc_tick sends it when free) */
    if (s->skill_set && !s->navigation_mode) for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {   /* 按著第二組技能：先松 ZL 和技能鍵，隔 DM_TC_ZL_LEAD_S 再按腰帶鍵（見 update） | a skill of the second skill set is held: first release ZL and the skill key, and press the belt key after DM_TC_ZL_LEAD_S (see update) */
        const dm_tc_finger *f = &s->finger[i]; if (!f->active || f->aim_cancelled || f->target < DM_TC_MAIN || f->target > DM_TC_SKILL4) continue;
        s->belt_gap_cell = 0x1000 | cell; s->belt_gap_at = s->last_time_s + DM_TC_ZL_LEAD_S; update(s); return; }
    s->pulse_rx = s->pulse_ry = 0; s->pulse_button = btn; ++s->pulse_epoch;
    update(s);
}
void dm_tc_begin_at(dm_tc_state *s, int slot, float x, float y, double now_s) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH || s->finger[slot].active || !isfinite(x) || !isfinite(y)) return;
    s->last_time_s = now_s;
    dm_tc_finger *f = &s->finger[slot];
    if (!s->navigation_mode) {   /* 難度：點第 0 格開 / 收選擇條；展開時點某格 = 選檔並收起，點別處只收起、照常處理 | Difficulty: tapping cell 0 opens / collapses the selector bar; when expanded, tapping a cell = select that level and collapse; tapping elsewhere only collapses and is then handled as usual */
        const dm_tc_target *d = &s->layout.diff;
        int on_diff = d->r > 0 && hypotf(x-d->x, y-d->y) <= d->r, pick = -1;
        for (int i = 0; s->diff_open && i < 8; ++i) if (hypotf(x-s->layout.pick[i].x, y-s->layout.pick[i].y) <= s->layout.pick[i].r) pick = i;
        if (on_diff || pick >= 0) {
            if (pick >= 0) s->diff_request = pick + 1;
            s->diff_open = on_diff ? !s->diff_open : 0;
            *f = (dm_tc_finger){.active=1, .target=DM_TC_DIFF, .x=x, .y=y}; update(s); return;
        }
        s->diff_open = 0;
    }
    int hit = dm_tc_hit(&s->layout, x, y);
    if (!s->navigation_mode && s->belt_open && s->belt_rows > 1) {   /* 腰帶展開著：點上面幾排 = 喝那一格；點腰帶以外 = 只收起 | Belt expanded: tapping the upper rows = drink that cell; tapping outside the belt = only collapse */
        float u = s->layout.height / 1320.0f;   /* 展開的幾排是遊戲自己畫的：列與底排對齊，排距 162、第一排比底排高 165（效果圖單位；真機 2868×1320 實量，行距由 gen_hud_v4.py 的 bpy 定，改那邊要一起改） | The expanded rows are drawn by the game itself: columns align with the bottom row, row pitch 162, the first row is 165 above the bottom row (mock units; measured on a real device at 2868×1320; the row spacing is set by bpy in gen_hud_v4.py, and changing it there requires changing it here too) */
        for (int r = 1; r < s->belt_rows && r < 4; ++r) for (int c = 0; c < 4; ++c) {
            const dm_tc_target *t = &s->layout.target[DM_TC_BELT1+c];
            if (fabsf(x - t->x) <= 60*u && fabsf(y - (t->y - (165.0f + 162.0f*(r-1))*u)) <= 53.75f*u) {
                belt_fire(s, 0x100 | r << 4 | c);   /* 按下就喝那一格 | drink that cell on press */
                *f = (dm_tc_finger){.active=1, .target=DM_TC_DIFF, .x=x, .y=y}; update(s); return;
            }
        }
        if (hit < DM_TC_BELT1 || hit > DM_TC_BELT4) {   /* 點別處：只收腰帶，這一下不再做別的（不走路、不點世界、不按鍵） | Tap elsewhere: only collapse the belt; this tap does nothing else (no walking, no world tap, no key press) */
            s->belt_request = 2; *f = (dm_tc_finger){.active=1, .target=DM_TC_DIFF, .x=x, .y=y}; update(s); return;
        }
    }
    if (hit == DM_TC_SWAP) {
        if (s->navigation_mode) hit = DM_TC_NONE;
        else { s->skill_set = !s->skill_set; *f = (dm_tc_finger){.active=1, .target=DM_TC_SWAP, .x=x, .y=y}; update(s); return; }
    }
    int enabled = hit >= 0 && (s->enabled_targets & (1u << hit));
    /* 面板態：被禁用的目標（底欄腰帶 / 功能鍵 / 回城、圓鈕收起後的技能鈕位置）不佔手指——它們壓著的地方可能正是面板的按鈕
       （設置頁「應用」在屏幕底部，落在禁用的底欄目標上，既不判類也不點擊）。遊戲態仍佔著（防隱藏的回城觸點變成搖桿）
       Panel state: disabled targets (bottom-bar belt / function key / town portal, and the skill button positions after the round buttons are hidden) do not capture fingers -- the place they cover may be exactly a panel button
              (「應用」(Apply) on the settings page is at the bottom of the screen and landed on a disabled bottom-bar target, getting neither classified nor clicked). In game state they still capture (preventing a hidden town-portal touch point from turning into the stick) */
    if (s->navigation_mode && hit >= 0 && !enabled) hit = DM_TC_NONE;
    if(enabled && hit==DM_TC_PORTAL && s->layout.target[hit].button && s->portal_phase==0) {
        ++s->gesture_epoch;
        uint64_t mask=s->layout.target[hit].button;
        s->portal_pending=(mask&DM_BTN_ZL) && (mask&~DM_BTN_ZL);
        s->portal_phase=s->portal_pending?1:2;
    }
    *f = (dm_tc_finger){.active=1, .target=enabled ? hit : DM_TC_NONE, .x=x, .y=y,
                           .aim_origin_x=x, .aim_origin_y=y,
                           .start_time_s=now_s};
    /* 禁用圓仍阻止落入左搖桿區，防止隱藏回城觸點變成移動。 | A disabled circle still blocks falling through into the left stick area, preventing a hidden town-portal touch point from turning into movement. */
    if (s->navigation_mode && hit == DM_TC_NONE && s->pointer_slot == DM_TC_NONE) {
        s->pointer_slot = slot;
        s->pointer_item = 0;
        /* 鼠標點擊模式下，手柄光標只跟「落在物品區的手指」走（前端答出命中後調 dm_tc_pointer_item）：
           光標是靠推搖桿走的，在沒有自由光標的頁面（選項、暫停菜單、小站）推搖桿 = 改當前選項 / 挪焦點（點頁簽順帶改了「顯示物品」）
           In mouse click mode the controller cursor follows only "a finger that landed in the item area" (the front end calls dm_tc_pointer_item once it has the hit answer):
                      the cursor moves by pushing the stick, and on pages without a free cursor (options, pause menu, waypoint) pushing the stick = changing the current option / moving the focus (tapping a tab changed 「顯示物品」(Show Items) as a side effect) */
        if (!s->mouse_click_mode) { s->pointer_valid = 1; s->pointer_x = x; s->pointer_y = y; }
    } else if (!s->navigation_mode && hit == DM_TC_NONE && g_edge_b > 0 && x >= 0 && x < s->layout.width*0.43f && y > s->layout.height - g_edge_b) {
        f->target = DM_TC_EDGE;
    } else if (!s->navigation_mode && !s->joy_block && hit == DM_TC_NONE && s->joy_slot == DM_TC_NONE &&
        x >= 0 && y <= s->layout.height && x < s->layout.width*0.43f && y > s->layout.height*0.25f &&
        (hypotf(x - s->layout.joy_x, y - s->layout.joy_y) <= DM_TC_JOY_GRAB_K * s->layout.joy_radius ||
         (s->joy_lost_s > 0 && now_s - s->joy_lost_s <= DM_TC_JOY_RESUME_S && hypotf(x - s->joy_lost_x, y - s->joy_lost_y) <= DM_TC_JOY_GRAB_K * s->layout.joy_radius))) {
        /* 搖桿底座固定在左下角原位：手指落在底座附近（DM_TC_JOY_GRAB_K 倍半徑內）才起搖桿，方向與幅度按「手指相對底座中心」算，
           按住後拖到多遠都照樣識別；落在左側別處不起搖桿（照舊是點地）。原先是浮動搖桿：落在左側區域哪裡，底座就出在哪裡、鬆手回原位。
           例外：搖桿那根手指剛被系統取消（通知橫幅、系統手勢；dm_tc_cancel_finger 記下時刻與位置），DM_TC_JOY_RESUME_S 內在原處附近重新落下的手指接著當搖桿——
           拖遠了的搖桿被系統打斷一下不該就此斷掉；浮動搖桿時這種情形表現為底座追著手指重起
           The joystick base is fixed at its home position in the lower left: the joystick starts only when a finger lands near the base (within DM_TC_JOY_GRAB_K × radius), and direction and magnitude are computed as "finger relative to the base center";
           once held it is still recognized however far it is dragged; landing elsewhere on the left does not start the joystick (it is a ground tap as before). Previously it was a floating joystick: wherever in the left area the finger landed, the base appeared there, returning to its home position on release.
           Exception: when the joystick finger has just been cancelled by the system (notification banner, system gesture; dm_tc_cancel_finger records the time and position), a finger landing again near the same spot within DM_TC_JOY_RESUME_S continues as the joystick --
           a joystick dragged far away should not be cut off for good by a brief system interruption; with the floating joystick this case showed up as the base restarting under the finger */
        s->joy_lost_s = 0;
        f->target = DM_TC_JOYSTICK;
        s->joy_slot = slot;
        s->joy_origin_x = s->layout.joy_x;
        s->joy_origin_y = s->layout.joy_y;
    }
    update(s);
}

void dm_tc_move_at(dm_tc_state *s, int slot, float x, float y, double now_s) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH || !s->finger[slot].active || !isfinite(x) || !isfinite(y)) return;
    s->last_time_s = now_s;
    dm_tc_finger *f = &s->finger[slot];
    f->x = x;
    f->y = y;
    if (pointer_finger(s, f)) {
        float distance = hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y);
        if (!f->pointer_dragged && distance > DM_TC_POINTER_MOVE_PT * scale_for(&s->layout) && (!s->mouse_click_mode || s->pointer_item == 1))
            pointer_drag_begin(s, f);   /* 鼠標點擊模式：只有從物品區起手才是拖放；別處的滑動不發鍵。按住不動超過長按時長再拖也算拖放（真人「按住停一下再拖」，z4：0.5 秒後才動的拖放拿不起來） | Mouse click mode: only a gesture starting in the item area is a drag-and-drop; swipes elsewhere send no key. Holding still longer than the long-press duration and then dragging also counts as drag-and-drop (a real person "holds for a moment and then drags"; z4: a drag that started moving only after 0.5 seconds could not pick the item up) */
        /* 起手在模態框外 / 被上層蓋住（類 6 = 這一下不點）也不起滾動 / 滑塊手勢：原先模態框開著時框外上下滑照樣給底下發滾輪 | A gesture starting outside the modal box / covered by an upper layer (class 6 = this tap does not click) does not start a scroll / slider gesture either: previously, with a modal box open, swiping up and down outside it still sent wheel events to what was underneath */
        else if (s->mouse_click_mode && s->pointer_item != 1 && s->pointer_item != 3 && s->pointer_item != 6 && !f->ui_drag && !f->pointer_dragged && distance > DM_TC_POINTER_MOVE_PT * scale_for(&s->layout))
            f->ui_drag = fabsf(f->x - f->aim_origin_x) > fabsf(f->y - f->aim_origin_y) ? 2 : 1;   /* 界面連續手勢：過閾值那一刻按方向定型（縱 = 滾動，橫 = 滑塊） | Continuous UI gesture: the type is fixed by direction at the moment the threshold is crossed (vertical = scroll, horizontal = slider) */
        update_pointer(s);
    } else if (!s->navigation_mode && skill_target(f->target)) {
        if (inside_map_cancel(s, f)) f->aim_cancelled = 1;
        if (!f->aim_cancelled) f->aim_move_sequence = ++s->aim_move_sequence;
    }
    update(s);
}

void dm_tc_end_at(dm_tc_state *s, int slot, double now_s) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH) return;
    s->last_time_s = now_s;
    if(s->finger[slot].active && s->finger[slot].target==DM_TC_PORTAL) {
        s->finger[slot]=(dm_tc_finger){.target=DM_TC_NONE};
        for(int i=0;i<DM_TC_MAX_TOUCH;i++)if(s->finger[i].active && s->finger[i].target==DM_TC_PORTAL) {update(s);return;}
        dm_tc_cancel_portal(s);return;
    }
    dm_tc_finger done = s->finger[slot];
    if (s->navigation_mode && slot == s->pointer_slot) {
        float distance = hypotf(done.x-done.aim_origin_x, done.y-done.aim_origin_y);
        double elapsed = now_s - done.start_time_s;
        /* 商人貨架上的貨長按 = 原版的「按住 Y 大量購買」（提示條原文；宿主讀數：約 1.3 秒）：背包裡有沒滿的捲軸書 / 同類疊時買滿，沒有就和買 1 份一樣；買多少、錢夠不夠都由遊戲定，這裡只替玩家按鍵 | Long press on goods on the vendor shelf = the original game's 「按住 Y 大量購買」(Hold Y to buy in bulk) (button legend text; host reading: about 1.3 seconds): when the inventory has a non-full scroll tome / stack of the same kind it buys until full, otherwise it is the same as buying 1; how many to buy and whether there is enough gold are decided by the game, and here we only press the key for the player */
        if (done.pointer_long && !done.pointer_dragged && !done.shop_drag && s->pointer_item == 1 && s->pointer_shop) pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, DM_BTN_Y, 1.3);
        else if (done.pointer_long && !done.pointer_dragged && !done.shop_drag && s->pointer_item == 1 && !s->pointer_lock) ++s->long_press_seq;   /* 長按在鬆手時才算（按住沒動）：按住到點就算的話，「按住停一下再拖」的藥水會先被放進腰帶（mini 藥拖腰帶腳本裡多出 2 次） | Long press counts only at release (held without moving): if it counted as soon as the hold time was reached, a potion that is "held for a moment and then dragged" would first be put into the belt (2 extra occurrences in the potion-drag-to-belt script on the second Mac) */
        if (done.locked_drag) { }   /* 單件槽佔用時拖別的物品：不發任何鍵 | Dragging another item while the single-item slot is occupied: send no key */
        else if (done.shop_drag) { ++s->shop_drop_seq; }   /* 買不買由前端按落點定；這裡不發任何鍵 | Whether to buy is decided by the front end from the drop point; no key is sent here */
        else if (done.pointer_dragged) {
            if (s->pointer_release_item == 1) pointer_pulse_start(s, done.x, done.y - DM_TC_POINTER_LIFT_PT * scale_for(&s->layout), 2);   /* 放在抬起點（物品顯示的位置） | Place at the lifted point (where the item is displayed) */
            else if (s->pointer_release_item == 5) pointer_pulse_start(s, s->release_x, s->release_y, 2);                                    /* 宿主給的放下位置（裝備槽 = 槽心） | Drop position given by the host (equipment slot = slot center) */
            else if (s->pointer_release_item == 4) pointer_pulse_start(s, done.x, done.y, 2);                                              /* 放在手指點（玩家對著手指放） | Place at the finger point (the player aims with the finger) */
            else if (s->pointer_release_item == 3) pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, DM_BTN_Y, 0.15);          /* 手指在面板外 = 丟棄：指針回原處、到位後按 Y | Finger outside the panel = discard: pointer returns to the original spot and presses Y once in place */
            else if (s->pointer_release_item == 8) {   /* 松在單件任務槽上：按原版走法重做——先放回原格，再對它按住 Y（原版的「放進槽」）。只把物品拿在手上停在槽上方不算進槽：槽會顯示它，但點確認不成交（本機 實測沒有成交包） | Released on the single-item quest slot: redo it the original game's way -- first put it back in its original cell, then hold Y on it (the original game's "put into slot"). Merely holding the item and hovering over the slot does not count as entering the slot: the slot shows it, but tapping confirm does not complete the transaction (measured on the local machine: no transaction packet) */
                pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2); pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, DM_BTN_Y, 1.3); }
            else pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);                                                          /* 面板裡的空白處：放回原處 | Blank area inside the panel: put it back where it was */
        } else if (!done.ui_drag && !done.pointer_long && elapsed < DM_TC_POINTER_TAP_S && distance <= DM_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {   /* 拖出去又拖回原處不算點按 | Dragging out and back to the original spot does not count as a tap */
            /* 單件任務槽面板開著時，手上那件就是「槽裡那件」：點背包格仍是放回去，點別處按沒拿著處理——確認 / 關閉按鈕要能點，點面板外 = 關面板（不是把槽裡那件丟到地上） | While the single-item quest slot panel is open, the held item is "the one in the slot": tapping an inventory cell still puts it back, tapping elsewhere is handled as if nothing were held -- the confirm / close buttons must be tappable, and tapping outside the panel = close the panel (not drop the slot's item on the ground) */
            if (s->pointer_lock) { s->pointer_x = done.aim_origin_x; s->pointer_y = done.aim_origin_y; }   /* 單件槽佔用時點別的物品：只移光標出說明，單擊 / 雙擊都不動它 | Tapping another item while the single-item slot is occupied: only move the cursor to show the tooltip; neither single nor double tap moves it */
            else if (s->holding && s->pointer_item != 0 && !(atomic_load(&g_imbue_open) && s->pointer_item != 1)) { if (s->pointer_item == 1) pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);
                              else if (s->pointer_item == 3) pointer_pulse_btn(s, s->pointer_x, s->pointer_y, 2, DM_BTN_Y, 0.15); }   /* 手上還拿著物品（上次沒放成）：點物品區 = 放下；點面板外 / 兩側黑網格 = 丟（Y）；點頁簽等別處不動。
                              點按鈕（類 0）不在這裡吞掉、照常發鼠標點擊：原先連合成鈕也吞，手上有東西時點合成什麼都不發；合成鈕由宿主先收手上那件再按，見 dm_cursor.c
                              Still holding an item (the last drop did not succeed): tap the item area = put down; tap outside the panel / the black grid on either side = drop (Y); tapping tabs or elsewhere does nothing.
                                                            Tapping a button (class 0) is not swallowed here and sends a mouse click as usual: previously even the transmute button was swallowed, and tapping transmute with something in hand sent nothing; for the transmute button the host first stows the held item and then presses, see dm_cursor.c */
            else if (s->pointer_item == 2) { ++s->tab_tap_seq; }
            else if (s->pointer_item == 6 || s->pointer_item == 11) { }   /* 壓著被上層面板蓋住的按鈕：不點、不發鍵 | Pressing on a button covered by an upper panel: no click, no key */
            else if (s->pointer_item == 3) { if (!atomic_load(&g_dead_modal)) { s->close_left = 3; s->close_next_s = now_s; atomic_store(&g_close_stop, 0); } }   /* 面板外：不發鼠標點擊（會落到遊戲世界 = 點地走路），按 B 關面板——子菜單裡一下 B 只退一級，所以按到面板關掉為止（至多 3 下，見 dm_tc_tick）；死亡彈窗開著時不動 | Outside the panel: do not send a mouse click (it would land in the game world = tap ground to walk); press B to close the panel -- in a submenu one B only goes back one level, so press until the panel is closed (at most 3 presses, see dm_tc_tick); do nothing while the death popup is open */
            else if (s->pointer_item) {   /* 物品區：單擊只移光標（遊戲自己出說明）；緊接著在同一處再點一下 = 按住 A / Y | Item area: a single tap only moves the cursor (the game shows the tooltip itself); another tap at the same spot right after = hold A / Y */
                float ox = done.aim_origin_x, oy = done.aim_origin_y;
                int dbl = now_s - s->last_tap_s < DM_TC_DOUBLE_TAP_S && hypotf(ox - s->last_tap_x, oy - s->last_tap_y) <= 2 * DM_TC_POINTER_MOVE_PT * scale_for(&s->layout);
                if (dbl) { if (atomic_load(&g_hover_unid) && !s->pointer_stack && !s->pointer_shop) ++s->ident_seq;   /* 未鑒定物品：雙擊 = 免費鑒定（各頁都先判；商人貨架上的貨不算我方物品，宿主也不發佈） | Unidentified item: double tap = free identify (checked first on every page; goods on the vendor shelf are not our items, and the host does not publish them either) */
                    else if (s->pointer_stack) ++s->stack_take_seq;   /* 成疊格：取 1 件（原版短按 Y；這裡不按鍵，按住 0.6 秒會變成「移動 3 個」） | Stacked cell: take 1 (short press Y in the original game; no key is pressed here, since holding for 0.6 seconds would become "move 3") */
                    else if (atomic_load(&g_imbue_open)) pointer_pulse_btn(s, ox, oy, 2, DM_BTN_Y, 1.3);   /* 單件任務槽面板：原版對背包物品按住 Y = 放進槽（宿主讀數：約 1.3 秒）；不能當「使用」——否則在古墓底座前雙擊法杖會被裝到手上 | Single-item quest slot panel: in the original game holding Y on an inventory item = put it into the slot (host reading: about 1.3 seconds); it must not be treated as "use" -- otherwise double-tapping the staff in front of the tomb pedestal would equip it in hand */
                    else if (atomic_load(&g_bank_open) && !atomic_load(&g_tap_box) && !atomic_load(&g_vault_open)) {
                        /* 背包裡的物品：先發原版的 X（移動到堆疊欄）。能堆疊的（符文 / 寶石 / 材料）遊戲收下、進堆疊格；不能堆疊的遊戲對 X 不做任何事，0.4 秒後它還在光標下就退回按住 Y。
                           不直接按住 Y：那是「到倉庫當前這一頁」，在符文 / 寶石頁且背包帶著魔盒時會進頁裡嵌的魔盒格。倉庫一側的物品不走這裡，仍是按住 Y 取回
                           Item in the inventory: first send the original game's X (move to the stacking tab). Stackable ones (runes / gems / materials) are accepted by the game and go into a stack cell; for non-stackable ones the game does nothing on X, and if it is still under the cursor after 0.4 seconds, fall back to hold Y.
                                                      Do not hold Y directly: that means "to the stash's current page", and on the rune / gem page with a cube in the inventory it would go into the cube cell embedded in the page. Items on the stash side do not go through here; they are still retrieved by holding Y */
                        if (atomic_load(&g_cur_stack)) { pointer_pulse_btn(s, ox, oy, 2, DM_BTN_X, 0.15); s->stash_x_check_s = now_s + 0.4; s->stash_x_x = ox; s->stash_x_y = oy; s->stash_x_id = atomic_load(&g_cur_stack); }
                        else pointer_pulse_btn(s, ox, oy, 2, DM_BTN_Y, DM_TC_HOLD_USE_S); }
                    else if (atomic_load(&g_cube_open) && !atomic_load(&g_bank_open)) pointer_pulse_btn(s, ox, oy, 2, DM_BTN_Y, DM_TC_HOLD_USE_S);   /* 方塊頁（不是箱子裡嵌的那格）：原版按住 Y 一步送進 / 送出方塊；與箱子頁雙擊同一條規則 | Cube page (not the cell embedded in the stash): in the original game holding Y sends the item into / out of the cube in one step; same rule as double tap on the stash page */
                    else ++s->use_seq;   /* 開著箱子：按住 Y = 移到另一側（魔盒除外：原版開箱時按住 A 仍是打開魔盒，讀碼）；否則交給宿主當場使用（原先按住 A 0.6 秒，約 0.4 秒後才生效） | Stash open: hold Y = move to the other side (except the cube: in the original game holding A with the stash open still opens the cube, per the code); otherwise hand it to the host to use on the spot (it used to hold A for 0.6 seconds and took effect only after about 0.4 seconds) */
                    s->last_tap_s = -1e9; }
                else {   /* 單擊：移光標。隔了一會兒（不算雙擊）又點同一處 = 與另一件已裝備的比較（原版按 ZL 循環；說明框文案「再點一次與另一件比較」） | Single tap: move the cursor. Tapping the same spot again after a while (not a double tap) = compare with the other equipped item (the original game cycles with ZL; tooltip text 「再點一次與另一件比較」(Tap again to compare with the other one)) */
                    int again = now_s - s->last_tap_s < 8.0 && hypotf(ox - s->last_tap_x, oy - s->last_tap_y) <= 2 * DM_TC_POINTER_MOVE_PT * scale_for(&s->layout);
                    if (again && !atomic_load(&g_bank_open) && !atomic_load(&g_vendor_open)) pointer_pulse_btn(s, ox, oy, 2, DM_BTN_ZL, 0);   /* 開著箱子 / 商人頁時 ZL 是切頁簽，不發（貨上點第二下把貨架翻到了下一頁） | With the stash / vendor page open ZL switches tabs, so do not send it (a second tap on goods flipped the shelf to the next page) */
                    else if (!s->pulse_button) { s->pointer_x = ox; s->pointer_y = oy; }
                    ++s->item_tap_seq;                     s->last_tap_s = now_s; s->last_tap_x = ox; s->last_tap_y = oy; }
            } else if (s->tap_button) {
                s->pointer_valid = 1; pointer_pulse_btn(s, s->tap_x, s->tap_y, 2, s->tap_button, 0);   /* 前端改寫：光標到點 + 按鍵（技能綁定裡點技能） | Front-end rewrite: cursor to the point + key press (tapping a skill in skill binding) */
            } else if (s->tap_move) {
                s->pointer_valid = 1; s->pointer_x = s->tap_x; s->pointer_y = s->tap_y; ++s->tap_move_seq;
            } else if (s->mouse_click_mode) {
                s->mouse_click_x = done.aim_origin_x; s->mouse_click_y = done.aim_origin_y; ++s->mouse_click_seq;
            } else pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);
        }
        s->pointer_item = 0; s->pointer_release_item = 1; s->pointer_shop = 0; s->pointer_stack = 0; s->pointer_lock = 0;
        s->pointer_slot = DM_TC_NONE;
        s->finger[slot] = (dm_tc_finger){.target=DM_TC_NONE};
        update(s);
        return;
    }
    /* 技能鈕：按住 = 持續施放、拖動定方向、鬆手鍵起，不再是「按住瞄準、鬆手放一下」。
       唯一要補的：點得太短，鍵還沒來得及按下手就鬆了（不到點按 / 拖動的判定時間，或第二組裡 ZL 還沒領先夠）——補成一個帶方向的脈衝；
       第二組裡 ZL 還沒領先夠時，這個脈衝延到夠了再發（dm_tc_tick）。
       Skill button: hold = continuous cast, drag sets the direction, release lifts the key; no longer "hold to aim, release to cast once".
              The only thing to make up: the tap was too short and the finger lifted before the key could go down (shorter than the tap / drag decision time, or in the second set ZL has not led long enough) -- make it up as one pulse with direction;
              in the second set, when ZL has not led long enough, this pulse is delayed until it has (dm_tc_tick). */
    if (done.active && done.target >= DM_TC_SKILL1 && done.target <= DM_TC_SKILL4 && !done.aim_cancelled && !s->navigation_mode
        && (!done.key_down || now_s - done.key_down_s < DM_TC_KEY_MIN_S || (s->skill_set && now_s - done.start_time_s < DM_TC_ZL_LEAD_S))) {   /* 鍵沒按下過，或按下不到 2 幀（遊戲可能漏讀）：補成脈衝 | The key was never pressed, or was pressed for less than 2 frames (the game may miss it): make it up as a pulse */
        float ux = 0, uy = 0; int has = aim_dir(s, &done, &ux, &uy);
        s->pulse_rx = has ? (int32_t)lroundf(ux*DM_STICK_MAX) : 0; s->pulse_ry = has ? (int32_t)lroundf(-uy*DM_STICK_MAX) : 0;
        if (s->skill_set && now_s - done.start_time_s < DM_TC_ZL_LEAD_S) { s->defer_button = s->layout.target[done.target].button; s->defer_at_s = done.start_time_s + DM_TC_ZL_LEAD_S; }
        else { s->pulse_button = s->layout.target[done.target].button; ++s->pulse_epoch; }
    }
    if (done.active && !s->navigation_mode && done.target >= DM_TC_BELT1 && done.target <= DM_TC_BELT4) {
        /* 腰帶格鬆手這一刻判：多排時上移過閾值 = 展開，下移過閾值 = 收起；幾乎沒動 = 點了一下，這一刻就喝；橫著滑走了什麼都不做 | decided when a belt cell is released: with multiple rows, moved up past the threshold = expand, down past the threshold = collapse; barely moved = a tap, drink right then; slid away sideways = do nothing */
        float th = DM_TC_BELT_SWIPE_PT * scale_for(&s->layout), up = done.aim_origin_y - done.y, dx = done.x - done.aim_origin_x;
        if (s->belt_rows > 1 && up > th) s->belt_request = 1;
        else if (s->belt_rows > 1 && up < -th) s->belt_request = 2;
        else if (fabsf(dx) <= th && fabsf(up) <= th) belt_fire(s, 0x10 | (done.target - DM_TC_BELT1));
    }
    if (done.active && !s->navigation_mode && done.target == DM_TC_NONE && now_s - done.start_time_s < DM_TC_POINTER_TAP_S &&   /* 落在搖桿底座上的那一下是搖桿（一按就有方向），不再兼作點地 | the tap that lands on the joystick base is the joystick (direction as soon as pressed) and no longer doubles as a ground tap */
        hypotf(done.x - done.aim_origin_x, done.y - done.aim_origin_y) <= DM_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {
        s->world_tap_x = done.aim_origin_x; s->world_tap_y = done.aim_origin_y; ++s->world_tap_seq;   /* 點遊戲世界 | Tap the game world */
    }
    s->finger[slot] = (dm_tc_finger){.target=DM_TC_NONE};
    if (slot == s->joy_slot) {
        s->joy_slot = DM_TC_NONE;
        s->joy_origin_x = s->joy_origin_y = 0;
    }
    update(s);
}

void dm_tc_begin(dm_tc_state *s, int slot, float x, float y) { dm_tc_begin_at(s, slot, x, y, s->last_time_s); }
void dm_tc_move(dm_tc_state *s, int slot, float x, float y) { dm_tc_move_at(s, slot, x, y, s->last_time_s); }
void dm_tc_end(dm_tc_state *s, int slot) { dm_tc_end_at(s, slot, s->last_time_s); }

void dm_tc_pointer_press(dm_tc_state *s, float x, float y, uint64_t button) {
    if (s->pointer_slot >= 0 && s->pointer_slot < DM_TC_MAX_TOUCH) s->finger[s->pointer_slot].pointer_long = 1;
    s->pointer_valid = 1; pointer_pulse_btn(s, x, y, 2, button, 0); update(s);
}
void dm_tc_tick(dm_tc_state *s, double now_s) {
    s->last_time_s = now_s;
    /* 補發不許蓋掉在發的脈衝（腰帶喝藥等）：在發就等；那個脈衝收回時（dm_tc_pulse_end）把補發時刻推到 ZL 再領先滿 DM_TC_ZL_LEAD_S 之後（原先到點直接覆蓋，腰帶那下沒了） | The make-up send must not overwrite a pulse being sent (belt potion, etc.): wait while one is being sent; when that pulse is released (dm_tc_pulse_end), push the make-up time to after ZL has again led for a full DM_TC_ZL_LEAD_S (it used to overwrite directly when due, and the belt press was lost) */
    if (s->belt_gap_cell && now_s >= s->belt_gap_at && !s->pulse_button) { uint64_t btn = s->layout.target[DM_TC_BELT1 + (s->belt_gap_cell & 0xf)].button; s->belt_gap_cell = 0;
        if (btn && !s->navigation_mode) { s->pulse_rx = s->pulse_ry = 0; s->pulse_button = btn; ++s->pulse_epoch; } update(s); }
    if (s->belt_q && !s->pulse_button && !s->defer_button && !s->belt_gap_cell) { int q = s->belt_q; s->belt_q = 0; if (!s->navigation_mode) belt_fire(s, q); }
    if (s->defer_button && !s->pulse_button && now_s >= s->defer_at_s) { s->pulse_button = s->defer_button; s->defer_button = 0; ++s->pulse_epoch; update(s); }
    else if (!s->navigation_mode) {   /* 主鈕 / 技能鈕上有手指：判定時間、ZL 領先這兩個時刻要按時刷新鍵值 | A finger is on the main button / a skill button: the key values must be refreshed on time at two moments, the decision time and the ZL lead */
        for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) if (s->finger[i].active && (s->finger[i].target >= DM_TC_MAIN && s->finger[i].target <= DM_TC_SKILL4)) { update(s); break; }
    }
    if (s->stash_x_check_s > 0 && now_s >= s->stash_x_check_s) {   /* 發過 X 的那件還在光標下（不能堆疊 / 這個存檔沒有堆疊欄 / 那一疊滿了）：退回按住 Y，移到倉庫當前頁。核的是同一個物品 id，X 已經把它移走就什麼都不做 | The item that X was sent for is still under the cursor (not stackable / this save has no stacking tab / that stack is full): fall back to hold Y, moving it to the stash's current page. The check is on the same item id; if X has already moved it away, do nothing */
        s->stash_x_check_s = 0;
        if (atomic_load(&g_bank_open) && atomic_load(&g_cur_stack) == s->stash_x_id && !s->holding && !s->pulse_button) { pointer_pulse_btn(s, s->stash_x_x, s->stash_x_y, 2, DM_BTN_Y, DM_TC_HOLD_USE_S); update(s); }
    }
    if (s->close_left > 0 && atomic_exchange(&g_close_stop, 0)) s->close_left = 0;   /* 返回鍵引出了確認框（設置頁有未應用的改動）：停手，讓玩家自己答——再按一下就成了替他選「放棄」（改動丟失） | The back button brought up a confirmation box (the settings page has unapplied changes): stop and let the player answer -- one more press would choose 「放棄」(Discard) on their behalf (changes lost) */
    if (s->close_left > 0) {   /* 面板外點按 = 關到底：面板還開著、上一下 B 已收回，就隔 0.3 秒再按一下 | Tap outside the panel = close all the way: if the panel is still open and the previous B has been released, press once more after 0.3 seconds */
        if (!s->navigation_mode) s->close_left = 0;
        else if (!s->pulse_button && now_s >= s->close_next_s) { s->pulse_button = DM_BTN_B; s->pulse_hold_s = 0; s->pulse_rx = s->pulse_ry = 0; s->pointer_pulse_active = 2; ++s->pulse_epoch; --s->close_left; s->close_next_s = now_s + 0.3; update(s); }
    }
    if (s->pointer_slot < 0 || s->pointer_slot >= DM_TC_MAX_TOUCH) return;
    dm_tc_finger *f = &s->finger[s->pointer_slot];
    if (!f->active || f->pointer_dragged || f->pointer_long) return;
    if (s->mouse_click_mode && s->pointer_item != 1) return;
    if (now_s - f->start_time_s >= DM_TC_POINTER_HOLD_S &&
        hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y) <= DM_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {
        f->pointer_long = 1;
        s->pointer_x = f->aim_origin_x;
        s->pointer_y = f->aim_origin_y;
        update(s);
    }
}

int dm_tc_navigation(dm_tc_scene *s, const char *cat, const char *act, const char *arg) {
    static const char *names[]={"PlayerInventoryExpansionLayout","SkillsTreePanel","PauseLayout","PauseLayoutGarden","CharacterStatsPanel","QuestLogPanelExpansion","BankExpansionLayout","HoradricCubeLayout","WaypointsPanelExpansion","NpcDialogPanel","NpcTradePanel","ButtonBindingModal","SkillSelect","YouDiedModal"};
    if(!strcmp(cat,"FrontEndNavigation")&&!strcmp(act,"ToFrontEnd")) { s->game=0;s->panels=0; }
    if(!strcmp(cat,"PanelManager")) {
        if(!strcmp(arg,"LoadScreenPanel")&&!strcmp(act,"OpenPanel"))s->game=0;
        if(!strcmp(arg,"LoadScreenPanel")&&!strcmp(act,"UnloadPanel"))s->game=1;
        if(!strcmp(act,"UnloadAll"))s->panels=0;
        { size_t al=strlen(arg); if((!strcmp(act,"OpenPanel")||!strcmp(act,"OpenExclusivePanel"))&&al>5&&!strcmp(arg+al-5,"Modal"))atomic_store(&g_close_stop,1); }
        for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)if(!strcmp(arg,names[i])) {
            if(!strcmp(act,"OpenPanel")||!strcmp(act,"OpenExclusivePanel"))s->panels|=1ull<<i;
            if(!strcmp(act,"ClosePanel")||!strcmp(act,"UnloadPanel"))s->panels&=~(1ull<<i);
        }
    }
    return !s->game||s->panels!=0;
}
/* 死亡彈窗開著：也算導航態（觸點走界面點擊，能點到「繼續」），但五個手柄圓鈕不給——死後點技能鈕位置會觸發復活，容易誤觸 | Death popup open: also counts as navigation state (touch points go through UI clicks, so 「繼續」(Continue) can be tapped), but the five controller round buttons are not offered -- after death a tap at a skill button position triggers resurrection, which is easy to hit by mistake */
int dm_tc_scene_dead(void) { return atomic_load(&g_dead_modal); }
int dm_tc_scene_dialog_only(const dm_tc_scene *s) { return s->game && s->panels == (1ull << 9); }   /* 面板表第 9 項 = NpcDialogPanel（見 dm_tc_navigation 的 names） | Entry 9 of the panel table = NpcDialogPanel (see names in dm_tc_navigation) */

