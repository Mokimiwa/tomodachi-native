#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DM_TC_MAX_TOUCH = 16, DM_TC_COUNT = 12 };
enum {
    DM_TC_NONE = -1, DM_TC_JOYSTICK = -2, DM_TC_SWAP = -3, DM_TC_DIFF = -4,   /* DIFF：難度格 / 選擇條佔用的手指，不發按鍵 | DIFF: a finger captured by the difficulty cell / selector bar; sends no key */
    DM_TC_EDGE = -5,   /* 屏幕底邊歸系統的那一條裡起手的手指：不當搖桿，鬆手也不當點地 | A finger that started in the strip along the bottom edge of the screen that belongs to the system: not treated as the stick, and its release is not treated as a ground tap */
    DM_TC_MAIN = 0,
    DM_TC_SKILL1, DM_TC_SKILL2, DM_TC_SKILL3, DM_TC_SKILL4,
    DM_TC_PORTAL, DM_TC_BELT1, DM_TC_BELT2, DM_TC_BELT3, DM_TC_BELT4,
    DM_TC_MENU, DM_TC_MAP
};
typedef struct { float x, y, r; uint64_t button; } dm_tc_target;
typedef struct {
    float width, height;
    dm_tc_target target[DM_TC_COUNT];
    float joy_x, joy_y, joy_radius;
    dm_tc_target swap;   /* 技能組切換鍵：本地開關，不直接發按鍵 | Skill set switch key: a local toggle; does not send a key directly */
    dm_tc_target diff;   /* 底部 UI v4 第 0 格難度：點開 / 收起 1–8 選擇條，不發按鍵 | Bottom UI v4 cell 0 difficulty: tap to open / collapse the 1–8 selector bar; sends no key */
    dm_tc_target pick[8];   /* 選擇條 8 格（第 i 格 = 人數檔 i+1），在七格上方 | The selector bar's 8 cells (cell i = player-count level i+1), above the seven cells */
} dm_tc_layout;
typedef struct {
    int active, target;
    float x, y;
    float aim_origin_x, aim_origin_y;
    uint64_t aim_move_sequence;
    int aim_cancelled;
    int key_down; double key_down_s;   /* 技能鈕：鍵是否已按下過、幾時按下的（按下後先等判定時間，見 update） | Skill button: whether the key has gone down and when (after press, first wait the decision time, see update) */
    double start_time_s;
    int pointer_dragged, pointer_long;
    /* 界面連續手勢（鼠標點擊模式、起手不在物品區也不在面板外、位移過閾值時按起始方向定型，定型後不變）：1 = 縱向拖（滾動），2 = 橫向拖（滑塊）。
       手勢層只定型，前端看到後把起手點與當前點轉給宿主（dm_ui_drag_*），滾動 / 滑塊由宿主按遊戲自己的命中去做
       Continuous UI gesture (mouse click mode, started neither in the item area nor outside the panel; when displacement crosses the threshold the type is fixed by the initial direction and does not change afterwards): 1 = vertical drag (scroll), 2 = horizontal drag (slider).
              The gesture layer only fixes the type; on seeing it the front end forwards the start point and current point to the host (dm_ui_drag_*), and the scroll / slider is done by the host according to the game's own hit testing */
    int ui_drag;
    int shop_drag;   /* 從商人貨架的貨上起拖（不拿起）：鬆手按落點買 / 不買 | Drag starting from goods on the vendor shelf (not picked up): on release, buy / do not buy by the drop point */
    int locked_drag; /* 單件任務槽佔用時從別的物品上起拖：不拿起，鬆手不發任何鍵 | Drag starting from another item while the single-item quest slot is occupied: do not pick up, and send no key on release */
} dm_tc_finger;
typedef struct {
    dm_tc_layout layout;
    dm_tc_finger finger[DM_TC_MAX_TOUCH];
    int joy_slot;
    float joy_origin_x, joy_origin_y;
    uint32_t enabled_targets;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    int aim_slot, navigation_mode, map_cancel_active;
    uint64_t aim_move_sequence;
    int portal_pending, portal_phase; // 0idle / 1modifier-pending / 2armed
    uint64_t gesture_epoch, portal_binding_identity, portal_scene_epoch;
    /* 鬆手施放的短按脈衝：按鍵與右軸方向同時發佈，前端到時調 dm_tc_pulse_end 收回 | Short-press pulse for release-to-cast: the key and the right-axis direction are published together, and the front end calls dm_tc_pulse_end to release it when due */
    uint64_t pulse_button, pulse_epoch;
    int32_t pulse_rx, pulse_ry;
    uint64_t defer_button; double defer_at_s;   /* 第二組裡點得太短的那一下：等 ZL 領先夠了再補發 | The too-short tap in the second set: make it up once ZL has led long enough */
    /* 技能組：0 = 第一套；1 = 第二套（原版 Switch 按住 ZL 用 alt 槽）。是模式，不隨取消/尺寸變化清零 | Skill set: 0 = first set; 1 = second set (on the original Switch game, holding ZL uses the alt slots). It is a mode and is not cleared on cancel/size change */
    int skill_set;
    /* 第二組技能（ZL 修飾）：ZL 只在手指按著主鈕 / 技能鈕、或技能脈衝在發時按著，且先於技能鍵 DM_TC_ZL_LEAD_S 按下。
       鬆手施放按得太短時，這一下脈衝延到 ZL 領先夠了再發（dm_tc_tick 到點起）。
       Second skill set (ZL modifier): ZL is held only while a finger is on the main button / a skill button or a skill pulse is being sent, and goes down DM_TC_ZL_LEAD_S before the skill button.
              When a release-to-cast press is too short, that pulse is delayed until ZL has led long enough (dm_tc_tick starts it when due). */
    int pointer_slot;
    double last_time_s;
    int pointer_pickup_done;
    int pointer_pulse_active, pointer_pulse_pending;
    float pointer_pending_x, pointer_pending_y;
    int pointer_valid;
    float pointer_x, pointer_y;
    /* 物品手勢：pointer_item = 當前指針手指落在物品區（前端在鬆手前按宿主命中結果置位）。物品區單擊只移光標（出說明）、
       雙擊 = 按住 A（遊戲提示：按住 = 打開 / 使用 / 裝備；開著箱子時按住 Y = 移到另一側）；非物品區單擊照舊 = 移光標 + 按 A。
       pulse_hold_s > 0 = 這個脈衝要按住這麼久，前端在「遊戲已取走」之外再等滿它才收
       Item gestures: pointer_item = the current pointer finger landed in the item area (set by the front end before release from the host's hit result). A single tap in the item area only moves the cursor (shows the tooltip),
              double tap = hold A (game hint: hold = open / use / equip; with the stash open, hold Y = move to the other side); a single tap outside the item area is as before = move the cursor + press A.
              pulse_hold_s > 0 = this pulse must be held this long, and the front end waits it out in full in addition to "the game has consumed it" before releasing */
    int pointer_item;
    /* 非物品區的點按走鼠標點擊（暫停菜單按鈕、佈局裡插回的關閉 / 修理等按鈕只認鼠標）：mouse_click_mode 由前端打開；
       每次這樣的點按 mouse_click_seq 加一，前端看到變化就在 (mouse_click_x, mouse_click_y) 發一次界面鼠標點擊，不再按 A
       Taps outside the item area go through mouse clicks (pause menu buttons and the close / repair buttons inserted back into the layout only respond to the mouse): mouse_click_mode is turned on by the front end;
              each such tap increments mouse_click_seq, and on seeing a change the front end sends one UI mouse click at (mouse_click_x, mouse_click_y) and no longer presses A */
    /* 拖放鬆手處（物品抬在手指上方的那個落點）是不是物品區：前端鬆手前置位。不是 = 拖到面板外 = 丟棄（指針回原處、到位後按 Y）。
       holding = 光標上還拿著物品（上一次沒放成）：這時點物品區 = 放下（A），點別處不動
       Whether the drag-and-drop release spot (the drop point where the item is lifted above the finger) is the item area: set by the front end before release. If not = dragged outside the panel = discard (pointer returns to the original spot and presses Y once in place).
              holding = the cursor is still holding an item (the last drop did not succeed): then tapping the item area = put down (A), tapping elsewhere does nothing */
    int pointer_release_item, holding; float release_x, release_y;
    /* 遊戲態（沒開面板）的點按 = 點遊戲世界：手指沒落在任何控件目標上（含落在搖桿區但沒拖動）、按下到抬起不到 0.35 秒、位移不超過點按閾值
       → world_tap_seq 加一，前端看到變化調宿主 dm_world_tap（點物品名拾取 / 點 NPC 對話 / 點箱子，點空地什麼都不發）。
       A tap in game state (no panel open) = tap the game world: the finger did not land on any widget target (including landing in the stick area without dragging), less than 0.35 seconds from press to lift, displacement within the tap threshold
              → world_tap_seq is incremented, and on seeing a change the front end calls the host's dm_world_tap (tap an item name to pick up / tap an NPC to talk / tap a chest; tapping empty ground sends nothing). */
    unsigned world_tap_seq; float world_tap_x, world_tap_y;
    int close_left; double close_next_s;   /* 面板外點按後還要按幾下 B、下一下最早什麼時候（按到面板關掉為止） | After a tap outside the panel, how many more B presses remain and the earliest time of the next one (press until the panel is closed) */
    /* 商人頁：這次按下落在貨架的貨上（前端按宿主命中結果 7 置位）。從貨上起拖 = 想買：不發「拿起」、光標留在那件貨上（宿主買的是光標選中的那件）；
       鬆手時 shop_drop_seq 加一，前端按落點決定——落在背包類物品區 = 買，拖回貨架 / 面板外 / 別處 = 不買
       Vendor page: this press landed on goods on the shelf (set by the front end on host hit result 7). A drag starting from goods = intent to buy: do not send "pick up", and the cursor stays on that item (the host buys the item the cursor has selected);
              on release shop_drop_seq is incremented and the front end decides by the drop point -- landing in an inventory-type item area = buy; dragged back to the shelf / outside the panel / elsewhere = do not buy */
    int pointer_shop; unsigned shop_drop_seq;
    int pointer_lock;   /* 單件任務槽裡有東西時按在別的物品上（宿主命中結果 10）：照物品區移光標出說明，但不拿起、雙擊 / 長按 / 單擊都不動它，也不把槽裡那件放過來 | Pressing on another item while the single-item quest slot holds something (host hit result 10): move the cursor and show the tooltip as in the item area, but do not pick it up; double tap / long press / single tap do not move it, and the item in the slot is not placed here either */
    /* 高級倉庫的成疊格（前端按宿主命中結果 8 置位）：別的都照物品區，只有雙擊不同——stack_take_seq 加一，前端調宿主 dm_stash_take(1) 取 1 件到背包 | Stacked cell of the advanced stash (set by the front end on host hit result 8): everything else follows the item area, only double tap differs -- stack_take_seq is incremented and the front end calls the host's dm_stash_take(1) to take 1 into the inventory */
    int pointer_stack; unsigned stack_take_seq;
    unsigned ident_seq;   /* 物品區雙擊了一件未鑒定物品的次數（只有未鑒定時雙擊 = 免費鑒定，各頁都先判這一條）：前端看到變化調宿主 dm_ui_identify_hovered | Number of double taps on an unidentified item in the item area (only when unidentified does double tap = free identify, and this rule is checked first on every page): on seeing a change the front end calls the host's dm_ui_identify_hovered */
    unsigned use_seq;   /* 物品區雙擊的次數（沒開箱子時）：前端看到變化調宿主 dm_ui_use_item——按光標下物品走原生的使用 / 購買 / 裝備，不等「按住 A」的門檻 | Number of double taps in the item area (when no stash is open): on seeing a change the front end calls the host's dm_ui_use_item -- performs the native use / buy / equip on the item under the cursor without waiting for the "hold A" threshold */
    unsigned item_tap_seq;   /* 物品區單擊的次數（前端看到變化調宿主 dm_ui_item_tap：鑒定態下單擊物品 = 鑒定它） | Number of single taps in the item area (on seeing a change the front end calls the host's dm_ui_item_tap: in identify state, single-tapping an item = identify it) */
    unsigned long_press_seq;   /* 物品區按住不動滿長按時長的次數（前端看到變化調宿主 dm_ui_long_press：背包藥水 → 腰帶） | Number of times the item area was held still for the full long-press duration (on seeing a change the front end calls the host's dm_ui_long_press: inventory potion → belt) */
    unsigned tab_tap_seq;   /* 點按落在頁簽上的次數（前端看到變化調宿主 dm_ui_tab_go） | Number of taps that landed on a tab (on seeing a change the front end calls the host's dm_ui_tab_go) */
    int mouse_click_mode; unsigned mouse_click_seq; float mouse_click_x, mouse_click_y;
    /* 前端臨時改寫「面板內非物品區的點按」：非 0 = 不發鼠標點擊，改成手柄光標走到觸點、到位後按這個鍵（技能綁定流程裡點技能 = 光標到它身上按 Y）。頁簽 / 面板外照舊 | Temporary front-end rewrite of "taps inside a panel outside the item area": non-zero = send no mouse click; instead the controller cursor walks to the touch point and presses this key on arrival (tapping a skill in the skill binding flow = cursor onto it and press Y). Tabs / outside the panel behave as before */
    uint64_t tap_button; float tap_x, tap_y; int tap_move; unsigned tap_move_seq;   /* tap_move：本次點按只把光標移到 tap_x / tap_y，不發點擊（技能圖標 / 屬性「+」：先選中、再確認） | tap_move: this tap only moves the cursor to tap_x / tap_y and sends no click (skill icon / attribute "+": select first, then confirm) */
    double pulse_hold_s;
    double last_tap_s; float last_tap_x, last_tap_y;
    double stash_x_check_s; float stash_x_x, stash_x_y; unsigned stash_x_id;   /* 開著箱子雙擊背包物品先發了 X：到這個時刻那件還在光標下（遊戲沒收這下 X）就退回按住 Y | With the stash open, double-tapping an inventory item sent X first: if at this time the item is still under the cursor (the game did not accept that X), fall back to hold Y */
    uint64_t pointer_pending_button; double pointer_pending_hold;
    /* 難度：diff_open = 選擇條展開；diff_request = 剛選中的人數檔（1–8），前端交給宿主 dm_hud_request_players 後清零 | Difficulty: diff_open = selector bar expanded; diff_request = the player-count level just selected (1–8), cleared after the front end hands it to the host's dm_hud_request_players */
    int diff_open, diff_request;
    /* 腰帶展開（原版鍵鼠「顯示腰帶」的那幾排，手柄版沒有入口）：身上的腰帶多於 1 排時，在腰帶格上向上滑 = 展開、向下滑 / 點別處 = 收起，
       展開後點上面幾排的藥 = 喝。belt_rows / belt_open 由前端每次觸摸前按宿主讀數寫入；belt_request 前端取走後清零：
       1 展開，2 收起，0x100 | 排 << 4 | 列 = 喝那一格（排 1–3 = 底排之上第幾排）
       Belt expansion (the rows of the original game's keyboard-and-mouse "show belt"; the controller version has no entry for it): when the worn belt has more than 1 row, swiping up on a belt cell = expand, swiping down / tapping elsewhere = collapse,
              and after expansion tapping a potion in the upper rows = drink. belt_rows / belt_open are written by the front end from host readings before each touch; belt_request is cleared after the front end takes it:
              1 expand, 2 collapse, 0x100 | row << 4 | column = drink that cell (row 1–3 = which row above the bottom row) */
    int belt_rows, belt_open, belt_request;
    /* 腰帶點一下 = 自己喝，鬆手那一刻就喝，點幾下喝幾瓶；不判雙擊、腰帶上不做餵雇傭兵的手勢 | one tap on the belt = the player drinks, at the moment of release, several taps drink several; no double-tap detection, no mercenary feeding gesture on the belt */
    int belt_gap_cell; double belt_gap_at;   /* 按著第二組技能時要喝藥：先把 ZL 和技能鍵鬆開、空出 DM_TC_ZL_LEAD_S，到 belt_gap_at 才按腰帶鍵（belt_gap_cell = 等著的那一格，0 = 沒有） | to drink a potion while a skill of the second skill set is held: first release ZL and the skill key and leave a gap of DM_TC_ZL_LEAD_S, and only at belt_gap_at press the belt key (belt_gap_cell = the cell that is waiting, 0 = none) */
    double joy_lost_s; float joy_lost_x, joy_lost_y;   /* 搖桿手指被系統取消的時刻與當時的位置（0 = 沒有）：緊接著在附近重新落下的手指接著當搖桿 | time and position at which the joystick finger was cancelled by the system (0 = none): a finger landing again nearby right afterwards continues as the joystick */
    int joy_block;   /* 講話文字框單獨開著（前端不進面板態）：這時落在底座上的手指不起搖桿，照舊只當一次點擊（關講話框）。前端按面板棧寫 | speech text box open on its own (the front end does not enter panel state): a finger on the base starts no stick and is still just one click (closes the speech box). Written by the front end from the panel stack */
    double belt_pulse_s;   /* 最近一次「給玩家喝」的腰帶脈衝還在發的時刻：其間按著的第二組技能連同 ZL 一起讓開，收回後 ZL 重新領先 DM_TC_ZL_LEAD_S 技能鍵才回來 | Time until which the most recent "player drinks" belt pulse is still being sent: during it the held second-set skill gives way together with ZL, and after the release ZL again leads by DM_TC_ZL_LEAD_S before the skill key comes back */
    int belt_q;   /* 此刻有別的脈衝在發 / 別的補發在等：排著，空了再落地（不互相蓋掉）。值 = 格 | another pulse is being sent / another resend is pending: queue and fire when free (they never overwrite each other). Value = cell */
} dm_tc_state;

/* 原版UI消息驅動，不根據輸入鍵猜面板。AutoMap是戰鬥疊加，不切導航。 | Driven by the original game's UI messages; panels are not guessed from input keys. AutoMap is a combat overlay and does not switch to navigation. */
typedef struct { int game; uint64_t panels; } dm_tc_scene;
int dm_tc_navigation(dm_tc_scene *scene, const char *cat, const char *act, const char *arg);
int dm_tc_scene_dialog_only(const dm_tc_scene *scene);   /* 只開著 NPC 對話菜單 | Only the NPC dialog menu is open */
int dm_tc_scene_dead(void);   /* 死亡彈窗開著（導航態，但不給手柄圓鈕） | Death popup open (navigation state, but no controller round buttons) */

/* 坐標使用 UIKit 點；佈局本身不畫球、不聲稱遊戲已綁定回城/腰帶。 | Coordinates use UIKit points; the layout itself does not draw the orbs and does not claim the game has bound town portal/belt. */
/* 安全區左右內縮（點）；在 init / resize 之前設，之後的佈局按它把右側技能鍵組與左搖桿移進安全區。缺省 0。 | Left/right safe-area insets (points); set before init / resize, and the layout afterwards uses them to move the right-side skill key group and the left stick into the safe area. Default 0. */
void dm_tc_set_safe_insets(float left, float right);
void dm_tc_set_edge_bottom(float bottom);   /* 屏幕底邊歸系統的那一條的高度（點）；0 = 沒有。只約束移動搖桿的起手 | Height (points) of the strip along the bottom edge of the screen that belongs to the system; 0 = none. Constrains only where the movement stick may start */
void dm_tc_layout_make(dm_tc_layout *layout, float width, float height);
int dm_tc_hit(const dm_tc_layout *layout, float x, float y);
#define DM_TC_POINTER_LIFT_PT 40.0f   // 功能頁拖動時物品抬在手指上方的點數（× 控件縮放） | Number of points the item is lifted above the finger while dragging on function pages (× widget scale)
void dm_tc_set_tall_scale(int on);   // 畫面高 > 440 點時按 高/440 放大的開關；缺省關（Mac 宿主觸屏用例傳的是像素數，不能放大），iOS 前端建視圖時開 | Switch for scaling up by height/440 when picture height > 440 points; default off (the Mac host touch test cases pass pixel counts and must not be scaled up); the iOS front end turns it on when creating the view
float dm_tc_scale_for_size(float width, float height);   // 控件縮放：手機 min(1, 寬/667, 高/375)；畫面高 > 440 點（iPad）時按 高/440 等比放大 | Widget scale: phone min(1, width/667, height/375); when picture height > 440 points (iPad), scale up proportionally by height/440
void dm_tc_init(dm_tc_state *state, float width, float height);
/* 尺寸變更會取消全部觸點，調用者隨後推送歸零狀態。 | A size change cancels all touch points, and the caller then pushes the zeroed state. */
void dm_tc_resize(dm_tc_state *state, float width, float height);
/* 控件開關不改變佈局；切換有效位時取消已有觸點。 | Widget switches do not change the layout; existing touch points are cancelled when the valid bits are toggled. */
void dm_tc_enable_targets(dm_tc_state *state, uint32_t mask);
/* 導航模式（面板 / 對話 / 前端頁開著）下可用的控件：右側五個虛擬手柄圓鈕（確認 / 返回 / 操作 / 綁定 / 下一頁 = A B X Y R）。
   底欄十字位置的「關閉」不再給：面板的關閉入口只留一個（外框右上角的關閉按鈕；圓鈕還在時另有「返回」）。
   pad = 0：五個圓鈕也不給（局內手指直接點界面接上之後用，開關 DM_TOUCH_NAV_PAD=0，缺省 1）。兩個前端共用這一處，不各寫各的。
   Widgets available in navigation mode (a panel / dialog / front-end page is open): the five virtual controller round buttons on the right (confirm / back / action / bind / next page = A B X Y R).
      The "close" at the bottom bar's cross position is no longer offered: only one close entry for panels remains (the close button at the top-right corner of the frame; while the round buttons are present there is also "back").
      pad = 0: the five round buttons are not offered either (used once in-run fingers tapping the UI directly is hooked up; switch DM_TOUCH_NAV_PAD=0, default 1). Both front ends share this one place and do not each write their own. */
static inline uint32_t dm_tc_nav_mask(int pad) { return pad ? ((1u<<DM_TC_MAIN)|(1u<<DM_TC_SKILL1)|(1u<<DM_TC_SKILL2)|(1u<<DM_TC_SKILL3)|(1u<<DM_TC_SKILL4)) : 0; }
/* 模式變化釋放全部手勢；導航模式只按鍵，不啓用技能拖拽和地圖取消區。 | A mode change releases all gestures; navigation mode only presses keys and does not enable skill dragging or the map cancel area. */
void dm_tc_set_navigation_mode(dm_tc_state *state, int navigation_mode);
void dm_tc_set_joy_block(dm_tc_state *state, int block);   /* 講話文字框 / 對話菜單單獨開著：落在底座上的手指不起搖桿、只當一次點擊 | speech text box / dialogue menu open on its own: a finger on the base starts no stick and is just one click */
/* 只取消回城控件擁有的core觸點；物理觸點由前端保留。 | Cancel only the core touch point owned by the town-portal widget; the physical touch point is kept by the front end. */
void dm_tc_set_portal_button(dm_tc_state *state, uint64_t mask);
void dm_tc_set_portal_binding(dm_tc_state *state, uint64_t mask, uint64_t binding_identity, uint64_t scene_epoch);
/* 反饋確認僅作用於仍持有且身份/場景/請求一致的pending手勢；返回是否首次armed。 | Feedback confirmation applies only to a pending gesture that is still held and whose identity/scene/request match; returns whether it was armed for the first time. */
int dm_tc_portal_ack(dm_tc_state *state, uint64_t gesture_epoch, uint64_t binding_identity, uint64_t scene_epoch);
void dm_tc_cancel_portal(dm_tc_state *state);
void dm_tc_begin(dm_tc_state *state, int slot, float x, float y);
void dm_tc_move(dm_tc_state *state, int slot, float x, float y);
void dm_tc_end(dm_tc_state *state, int slot);
void dm_tc_begin_at(dm_tc_state *state, int slot, float x, float y, double now_s);
void dm_tc_move_at(dm_tc_state *state, int slot, float x, float y, double now_s);
void dm_tc_end_at(dm_tc_state *state, int slot, double now_s);
void dm_tc_tick(dm_tc_state *state, double now_s);
void dm_tc_cancel_all(dm_tc_state *state);
void dm_tc_cancel_finger(dm_tc_state *state, int slot);   /* 系統取消這根手指：不當鬆手，只放掉它按著的東西 | The system cancels this finger: do not treat it as a release, only let go of what it was holding */
/* 結束 epoch 對應的施放脈衝；返回是否有變化（調用者隨後推送） | End the cast pulse corresponding to epoch; returns whether anything changed (the caller then pushes) */
int dm_tc_pulse_end(dm_tc_state *state, uint64_t pulse_epoch);
// 收回後還有排著的指針脈衝（拖放的「放下」在「拿起」沒收時就到了）：前端等遊戲看到一次鬆開後調 resume 開始它
// After release there is still a queued pointer pulse (the "put-down" of a drag-and-drop arrived before the "pick-up" was released): the front end waits until the game has seen a release once, then calls resume to start it
int dm_tc_pulse_pending(const dm_tc_state *state);
void dm_tc_pointer_press(dm_tc_state *state, float x, float y, uint64_t button);   /* 光標到 (x, y) 再按一下 button；當前指針手指記為「長按已處理」，鬆手不再算點按 | Cursor to (x, y), then press button once; the current pointer finger is marked "long press handled" and its release no longer counts as a tap */
void dm_tc_pointer_item(dm_tc_state *state, int kind);   /* 指針手指的命中結果一出來就調（最晚鬆手前）：1 = 物品區，2 = 頁簽，3 = 面板外（點按 = 關面板），其餘 = 面板內別處（點按 = 鼠標點擊） | Called as soon as the pointer finger's hit result is available (before release at the latest): 1 = item area, 2 = tab, 3 = outside the panel (tap = close the panel), anything else = elsewhere inside the panel (tap = mouse click) */
void dm_tc_pointer_release(dm_tc_state *state, int kind);   /* 拖放鬆手前調：宿主的落點判定（1 放在抬起點 / 4 放在手指點 / 3 丟棄 / 0 放回原處） | Called before a drag-and-drop release: the host's drop decision (1 place at the lifted point / 4 place at the finger point / 3 discard / 0 put back where it was) */
void dm_tc_pointer_release_at(dm_tc_state *state, float x, float y);   /* 拖放鬆手前調：放在這個位置（宿主算的，裝備槽 = 槽心） | Called before a drag-and-drop release: place at this position (computed by the host; equipment slot = slot center) */
void dm_tc_set_holding(dm_tc_state *state, int holding);
void dm_tc_set_cur_stack(unsigned id);   /* 倉庫開著時光標此刻指著的背包物品的 id（指著倉庫一側 / 沒指著 = 0） | id of the inventory item the cursor is pointing at right now while the stash is open (pointing at the stash side / pointing at nothing = 0) */
void dm_tc_set_hover_unid(unsigned id);   /* 光標此刻指著的我方未鑒定物品 id（宿主 dm_ui_hover_unid）：物品區雙擊先判它——非 0 = 鑒定（ident_seq 加一），不走各頁原來的雙擊 | id of our unidentified item the cursor is pointing at right now (host dm_ui_hover_unid): an item-area double tap checks it first -- non-zero = identify (ident_seq incremented), skipping each page's original double tap */
void dm_tc_set_tap_box(int on);   /* 最近一次單擊的物品是不是魔盒（開著箱子時雙擊魔盒 = 打開） | Whether the most recently single-tapped item is the cube (with the stash open, double-tapping the cube = open) */
void dm_tc_set_panel_state(int bits);   /* 宿主讀遊戲面板樹得到的狀態位（1 儲藏箱開著 / 2 商人頁 / 4 死亡彈窗）；界面消息 DM:PanelState 到來時前端調 | State bits the host obtains by reading the game panel tree (1 stash open / 2 vendor page / 4 death popup); the front end calls this when the UI message DM:PanelState arrives */
void dm_tc_set_iso_fix(int on);   /* 移動搖桿斜向預修正開關（缺省開；測量改前改後用） | Switch for the movement stick's diagonal pre-correction (default on; used to measure before and after the change) */
int dm_tc_belt_outside(const dm_tc_state *state, float x, float y);
#define DM_TC_AIM_SETTLE_S 0.08    /* 技能鈕按下後等這麼久判定是點按還是拖動（拖過死區就提前鍵下） | After a skill button is pressed, wait this long to decide tap versus drag (dragging past the dead zone puts the key down early) */
#define DM_TC_BELT_SWIPE_PT 24      /* 腰帶格上鬆手時縱向位移過這麼多點 = 展開 / 收起；不到 = 點按（喝） | On release over a belt cell, vertical displacement beyond this many points = expand / collapse; less = tap (drink) */
#define DM_TC_KEY_MIN_S 0.035       /* 技能鍵至少按住這麼久（約 2 幀）；不夠的鬆手時補成脈衝，保證遊戲讀到 | A skill button is held at least this long (about 2 frames); a shorter one is made up as a pulse on release to guarantee the game reads it */
#define DM_TC_JOY_RESUME_S 0.35   /* 搖桿那根手指被系統取消後，這麼久之內在原處附近重新落下的手指接著當搖桿（不要求落在底座附近） | after the joystick finger is cancelled by the system, a finger landing again near the same spot within this long continues as the joystick (landing near the base is not required) */
#define DM_TC_JOY_GRAB_K 1.5f   /* 手指落在離搖桿底座中心這麼多倍半徑之內才起搖桿（底座固定，不跟落指點） | the joystick starts only when a finger lands within this many radii of the joystick base center (the base is fixed and does not follow the touch-down point) */
#define DM_TC_ZL_LEAD_S 0.17   /* 修飾鍵領先主鍵的時間（約 10 幀；同一幀按下遊戲只認主鍵） | Time the modifier key leads the main key (about 10 frames; pressed in the same frame, the game only recognizes the main key) */
#define DM_TC_DOUBLE_TAP_S 0.40   /* 兩次單擊間隔上限 | Upper limit of the interval between two single taps */
#define DM_TC_HOLD_USE_S 0.60     /* 「按住」動作的按住時長：Mac 宿主實測 0.25 s 只撿起、0.45 s 打開魔盒 | Hold duration of the "hold" action: measured on the Mac host, 0.25 s only picks up, 0.45 s opens the cube */
int dm_tc_pulse_resume(dm_tc_state *state);

#ifdef __cplusplus
}
#endif
