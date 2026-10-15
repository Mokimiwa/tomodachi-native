#pragma once
#include <stdint.h>

void dm_cursor_set(int x, int y);
// 前端登記「待寫原版光標」（屏幕點坐標），由宿主在遊戲讀手柄時（hle_hid fill）落地：
// 與按鍵同一次採樣到達遊戲。前端主線程隨時直寫光標全局，遊戲處理 A 之前會用自己的手柄光標狀態把它寫回，A 落在舊光標處
// The front end registers a 'pending write to the original game's cursor' (screen point coordinates), and the host applies it when the game reads the controller (hle_hid fill):
// it reaches the game in the same sample as the keys. If the front-end main thread writes the cursor global directly at an arbitrary time, the game writes it back from its own controller cursor state before processing A, and A lands at the old cursor
void dm_cursor_set_screen(float x, float y, float screen_w, float screen_h);
void dm_cursor_clear(void);
// 搖桿閉環（過渡做法，原版鼠標模式開關找到前用）：真機上原版逐幀鼠標移動那條路不跑，直寫的光標會被手柄光標邏輯蓋回。
// 改為：每次讀手柄時讀原版光標 (main+0x2d128f8/fc)，離目標 > DM_CURSOR_NEAR（9）像素就推左搖桿朝目標走（60 像素內按距離減力）；
// 沒到位時按下的 A 連同當時的目標位置排隊扣住，光標走到該位置才放出（拖放的起點 / 終點各一下）；排隊 1.5 秒沒到位報紅並丟掉。
// 宿主讀手柄處（hle_hid fill）調用，與按鍵同一次採樣到達遊戲
// Stick closed loop (interim approach, used until the original game's mouse mode switch is found): on a real device the original game's per-frame mouse move path does not run, and a directly written cursor gets overwritten by the controller cursor logic.
// Instead: on every controller read, read the original game's cursor (main+0x2d128f8/fc), and if it is > DM_CURSOR_NEAR (9) pixels from the target push the left stick toward the target (force reduced by distance within 60 pixels);
// an A pressed before it is in position is queued and held back together with the target position at that time, and released only when the cursor reaches that position (one each for the start / end of a drag-and-drop); if not in position after 1.5 seconds in the queue, report a failure and drop it.
// Called where the host reads the controller (hle_hid fill), reaching the game in the same sample as the keys
void dm_cursor_apply(uint64_t *buttons, int32_t *lx, int32_t *ly);
int dm_cursor_pending(void);   // 還有幾下 A 扣著沒送到遊戲（光標沒走到位）；前端收脈衝前要等它歸零 | How many A presses are still held back and not delivered to the game (the cursor is not in position yet); the front end must wait for it to return to zero before ending the pulse
// 原生點擊（界面鼠標消息注入，見 dm_cursor.c）：前端把觸點交過來，宿主在遊戲取手柄處逐幀發給界面根控件
// Native click (UI mouse message injection, see dm_cursor.c): the front end hands over the touch points, and the host sends them frame by frame to the UI root widget where the game reads the controller
void dm_ui_send_queue(const char *spec);   // 「類別:動作[:參數]」；下一次取手柄時往界面根控件發（按鍵腳本 MSG 步用） | 'category:action[:argument]'; sent to the UI root widget on the next controller read (used by the MSG step of key scripts)
int dm_ui_fe_drag_ok(void);                      // 前端此刻縱向拖能否當滾動（刪除確認 / 模式頁 / 起名頁 / 確認框開著時不能） | Whether a vertical drag in the front end can count as a scroll right now (not while the delete confirmation / mode page / naming page / a confirmation box is open)
void dm_ui_touch(int phase, float nx, float ny);   // phase：0 按下 1 移動 2 抬起 3 作廢（改成滾動）；坐標 = 出圖畫面歸一化 [0,1] | phase: 0 down, 1 move, 2 up, 3 cancelled (turned into a scroll); coordinates = normalized to the rendered picture [0,1]
void dm_ui_mouse_pump(void);
// 通用指針事件：HOVER 開始懸停保持（每幀頂住光標）、MOVE 更新位置、END 結束保持；其餘排隊每幀一個。arg：SCROLL = 滾輪量（120 一格），KEY = VK 鍵碼
// Generic pointer events: HOVER starts a held hover (pins the cursor every frame), MOVE updates the position, END ends the hold; the rest are queued, one per frame. arg: SCROLL = wheel amount (120 per notch), KEY = VK key code
enum { DM_UP_HOVER = 0, DM_UP_MOVE = 1, DM_UP_END = 2, DM_UP_LDOWN = 3, DM_UP_LUP = 4, DM_UP_RDOWN = 5, DM_UP_RUP = 6, DM_UP_SCROLL = 7, DM_UP_KEYDOWN = 8, DM_UP_KEYUP = 9 };
unsigned dm_ui_item_query(float nx, float ny);   // 問「這點是不是物品區」，返回問題序號；主線程下一幀作答 | Ask 'is this point an item area'; returns the question sequence number; the main thread answers on the next frame
int dm_ui_item_result(unsigned seq);             // 1 物品區 / 2 頁簽 / 0 面板內其它控件 / 3 面板外 / 6 壓著被上層面板蓋住的按鈕（不點）/ 7 貨架上的貨 / 8 成疊格 / 10 單件槽佔用時別的物品（不動）/ -1 還沒答 | 1 item area / 2 tab / 0 other widget inside a panel / 3 outside the panels / 6 pressing on a button covered by an upper panel (not clicked) / 7 goods on the vendor shelf / 8 stacked cell / 10 another item while the single-item slot is occupied (left alone) / -1 not answered yet
unsigned dm_ui_drop_query(float nx, float ny_lifted, float ny_finger);   // 拖放落點怎麼處理：結果 5 放在 dm_ui_drop_point 給的位置（物品區；裝備槽取槽心）/ 3 丟棄 / 0 放回原處（同 dm_ui_item_result 取） | How to handle a drag-and-drop landing point: result 5 put at the position given by dm_ui_drop_point (item area; for an equipment slot the slot center) / 3 drop / 0 put back where it was (fetched with dm_ui_item_result as well)
int dm_ui_drop_at_now(float nx, float ny_lifted, float ny_finger);        // 同步版，只許遊戲主線程調 | Synchronous version, callable only from the game main thread
int dm_ui_origin_point(float *nx, float *ny);   // 最近一次「拿起」的物品原位中心（放回原處用）；0 = 沒有 | Center of the original position of the most recently 'picked up' item (used to put it back where it was); 0 = none
int dm_ui_drop_last(void);                      // 最近一次答上的拖放落點類（鬆手時這次的還沒答上就用它；-1 = 沒有） | Class of the most recently answered drag-and-drop landing point (used on release if this one has not been answered yet; -1 = none)
int dm_ui_drop_point(float *nx, float *ny);   // 落點判定為 5 時的放下位置（歸一化） | Put-down position when the landing verdict is 5 (normalized)
int dm_ui_drop_bag(void);                       // 落點判定為 5 時落點是不是玩家背包格（貨架拖買 / 成疊格拖出只在這裡成交） | When the landing verdict is 5, whether the landing point is a player inventory cell (shelf drag-to-buy / dragging out of a stacked cell only complete here)
int dm_fe_bliss(void);   // 建角模式頁的「極樂世界」勾沒勾（1 = 勾了；玩家離開模式頁後保留最後的值，建角時讀）。任意線程可讀 | Whether 「極樂世界」(Elysium) on the character-creation mode page is checked (1 = checked; the last value is kept after the player leaves the mode page and is read at character creation). Readable from any thread
int dm_ui_shelf_grid(float g[4]);   // 商人貨架格網（歸一化：左上角 x,y、一格寬、高）；0 = 還沒探到 | Vendor shelf grid (normalized: top-left x,y, width and height of one cell); 0 = not probed yet
int dm_ui_item_rect(unsigned seq, float r[4]);   // 商人貨架上被按住的那件貨的屏幕矩形（歸一化；拖貨跟手圖用）；0 = 沒有 / 還沒好，下一幀再問 | Screen rectangle of the goods being held down on the vendor shelf (normalized; used for the finger-following image when dragging goods); 0 = none / not ready yet, ask again next frame
int dm_ui_stack_left(void);                       // 這次按下的成疊格裡那一疊有幾件；-1 = 還沒讀到 | How many items are in the stack of the stacked cell pressed this time; -1 = not read yet
void dm_ui_world_tap(float ux, float uy, float wx, float wy);   // 遊戲態點世界：點在傭兵頭像上 = 開傭兵頁，否則轉給 dm_world_tap(wx, wy)。u = 視圖歸一化，w = 出圖歸一化 | Tap on the world in the game state: a tap on the mercenary portrait = open the mercenary page, otherwise forwarded to dm_world_tap(wx, wy). u = normalized to the view, w = normalized to the rendered picture
void dm_ui_stack_take_at(float nx, float ny);     // 高級倉庫裡的一疊拖到背包鬆手：光標放回那一疊並取 1 件到背包 | A stack in the advanced stash dragged to the inventory and released: put the cursor back on that stack and take 1 item into the inventory
void dm_ui_stack_cube_at(float nx, float ny);     // 高級倉庫裡的一疊拖到這一頁下方的魔盒格上鬆手（落點類 11）：光標放回那一疊並按原版 X = 1 件進魔盒 | A stack in the advanced stash dragged onto the Horadric Cube cell at the bottom of this page and released (landing class 11): put the cursor back on that stack and press the original game's X = 1 item into the Horadric Cube
void dm_ui_shop_buy_at(float nx, float ny);       // 貨架上的貨拖到背包鬆手：把手柄光標放回那件貨上（起拖時挪開了）並買下它 | Goods on the vendor shelf dragged to the inventory and released: put the controller cursor back on those goods (it was moved away when the drag started) and buy them
void dm_ui_item_tap(void);                       // 物品區單擊：處在「用了鑒定捲軸 / 書、等選目標」的狀態時 = 鑒定光標下那件；不在該狀態什麼都不做 | Single tap in an item area: when in the 'identify scroll / tome used, waiting for target selection' state = identify the item under the cursor; does nothing when not in that state
int dm_ui_targeting(void);                       // 此刻是否處在那個狀態（宿主每幀發佈，任意線程讀） | Whether that state is in effect right now (published by the host every frame, read from any thread)
int dm_ui_panel_resident(const char *name);   // 局內常駐層（HUD 各層、傭兵頭像欄、存盤指示、提示條…）：開著不算「有面板開著」 | In-run always-on layers (the HUD layers, mercenary portrait bar, save indicator, button legend...): being open does not count as 'a panel is open'
enum { DM_PS_BANK = 1, DM_PS_VENDOR = 2, DM_PS_DEAD = 4, DM_PS_TEXT = 8, DM_PS_IMBUE = 16, DM_PS_CUBE = 32, DM_PS_VAULT = 64 };   // VAULT = 儲藏箱開在第六頁「編年史」（dm_vault_ui.c）   // CUBE = 赫拉迪克方塊頁開著（HoradricCubeLayout）   // IMBUE = 單件任務槽面板開著（灌魔 / 打孔 / 個性化 / 古墓底座共用的 ImbueItemsPanel） | VAULT = the stash is open on the sixth page 「編年史」(Chronicle) (dm_vault_ui.c)   // CUBE = the Horadric Cube page is open (HoradricCubeLayout)   // IMBUE = the single-item task slot panel is open (ImbueItemsPanel, shared by imbue / socketing / personalize / tomb pedestal)
int dm_ui_panel_state(void);
void dm_ui_lifecycle(int background);   /* 前端進 / 出後台時調：位置守衛自檢（dm_cursor.c mv_audit） | Called when the front end enters / leaves the background: position guard self-check (dm_cursor.c mv_audit) */   // 面板狀態位（每幀讀遊戲面板樹發佈；變化時發合成界面消息 DM:PanelState） | Panel state bits (published by reading the game's panel tree every frame; on change the synthetic UI message DM:PanelState is sent)
int dm_ui_panel_shown(const char *name);   // 這個面板此刻在遊戲面板樹裡可見（遊戲主線程調） | This panel is visible in the game's panel tree right now (called from the game main thread)
int dm_ui_tap_box(void);   // 最近一次單擊的物品是不是魔盒 | Whether the most recently single-tapped item is the Horadric Cube
uint32_t dm_ui_cur_stack_id(void);   // 倉庫開著時光標此刻指著的若是背包裡的物品 = 它的單位 id，否則 0 | While the stash is open, if the cursor is pointing at an item in the inventory right now = its unit id, otherwise 0
int dm_ui_free_cursor(void);   // 當前頁有沒有自由光標 | Whether the current page has a free cursor
void dm_ui_use_item(void);                       // 物品區雙擊：光標下是商人的貨 → 買；能裝備的 → 原版「按住確認鍵」那條（自動穿上）；其餘 → 當場使用（喝藥 / 讀捲軸 / 開魔盒） | Double-tap in an item area: a vendor's goods under the cursor -> buy; equippable -> the original game's 'hold the confirm button' path (auto-equip); everything else -> use on the spot (drink a potion / read a scroll / open the Horadric Cube)
void dm_ui_long_press(void);                     // 物品區長按 = 補充到腰帶：手柄光標指著的若是背包裡能進腰帶的藥水且腰帶有位，放進腰帶；別的不動 | Long-press in an item area = refill the belt: if the controller cursor points at a potion in the inventory that can go into the belt and the belt has room, put it into the belt; anything else is left alone
void dm_ui_tab_go(void);                         // 最近一次命中是頁簽且這次是點按：按夠次數的 ZL / ZR（頂部 L / R）切過去 | The most recent hit was a tab and this time it is a tap: press ZL / ZR (L / R at the top) enough times to switch to it
int dm_ui_item_at_now(float nx, float ny);        // 同上的同步版，只許遊戲主線程調 | Synchronous version of the above; may only be called from the game main thread
void dm_ui_watch(const char *spec);            // 測試量具：盯住界面控件「名字[@面板名]」（NULL 取消）；主線程每幀發佈它的中心 | Test probe: watch the UI widget 'name[@panel name]' (NULL cancels); the main thread publishes its center every frame
int dm_ui_watch_pos(float *nx, float *ny);    // 任意線程：盯住的控件可見返回 1 並給出遊戲視圖歸一化坐標 | Any thread: returns 1 if the watched widget is visible and gives its coordinates normalized to the game view
uint64_t dm_ui_taps_base(void);   // 測試：DM_UI_TAPS_AT 命中時的出圖幀（0 = 還沒命中） | Test: presented frame at which DM_UI_TAPS_AT hit (0 = not hit yet)
void dm_ui_ptr(int kind, float nx, float ny, int arg);
int dm_ui_touch_active(void);
// 前端路徑理順（見 dm_cursor.c）：界面消息回調登記、取手柄處執行、自動按鍵掩碼
// Front-end path streamlining (see dm_cursor.c): UI message callback registration, execution where the controller is read, auto key mask
void dm_ui_on_uimsg(const char *cat, const char *act, const char *arg);
void dm_ui_frontend_pump(void);
uint64_t dm_ui_auto_buttons(void);
void dm_ui_key_pulse(uint64_t mask, int frames);   // 替玩家點一下手柄鍵（按住 frames 個客體幀） | Tap a controller key on the player's behalf (held for frames guest frames)
// 界面連續手勢（前端任意線程調，主線程每幀執行）：kind 1 = 縱向拖 → 滾動，2 = 橫向拖 → 滑塊；坐標 = 出圖畫面歸一化 [0,1]；tol_ny = 認滑塊時上下放寬多少（手指粗細）
// Continuous UI gestures (called by the front end from any thread, executed every frame on the main thread): kind 1 = vertical drag -> scroll, 2 = horizontal drag -> slider; coordinates = normalized to the rendered picture [0,1]; tol_ny = how much vertical slack to allow when identifying a slider (finger thickness)
void dm_ui_drag_begin(int kind, float ox, float oy, float tol_ny);
void dm_ui_drag_move(float x, float y);
void dm_ui_drag_end(void);
void dm_ui_drag_lift(float ny);   // 起拖前報物品圖抬在手指上方多少（屏高比例）：編年史格子拖回背包按物品圖位置判落點（dm_vault_ui.c） | Before a drag starts, report how far the item image is lifted above the finger (as a fraction of screen height): dragging a chronicle cell back to the inventory decides the landing point by the item image position (dm_vault_ui.c)
// 右上角小地圖（遊戲的 AutoMap 面板）在出圖畫面裡的矩形，歸一化 [0,1]：r = 左、上、右、下。返回 1 = 讀到過（面板關著時給最近一次的）；任意線程調
// Rectangle of the top-right minimap (the game's AutoMap panel) in the rendered picture, normalized [0,1]: r = left, top, right, bottom. Returns 1 = has been read (the most recent one is given while the panel is closed); callable from any thread
int dm_ui_minimap_rect(float r[4]);
// 全屏地圖 ↔ 小地圖（運行時切，不落盤）：dm_ui_map_full(1) 切全屏、(0) 回玩家設置的那檔小地圖；dm_ui_map_is_full() = 遊戲當前是不是全屏地圖（任意線程調，主線程每幀刷新）
// 技能綁定的觸摸流程（前端任意線程調，主線程每幀推進）：dm_ui_bind_begin(鍵) = 給這個手柄鍵（可帶 ZL = 第二組）選技能——宿主打開技能樹，
// 玩家點一個技能（前端在流程進行中把點按改成「光標到點 + 按 Y」）→ 彈出原版「重新綁定技能」框 → 宿主替他按目標鍵 → 關面板。dm_ui_bind_active() = 流程進行中（0 無 / 1 正在開技能樹 / 2 等玩家點技能 / 3 收尾）
// Full-screen map ↔ minimap (switched at runtime, not written to disk): dm_ui_map_full(1) switches to full screen, (0) returns to the minimap level set by the player; dm_ui_map_is_full() = whether the game is currently on the full-screen map (callable from any thread, refreshed every frame on the main thread)
// Touch flow for skill binding (called by the front end from any thread, advanced every frame on the main thread): dm_ui_bind_begin(key) = choose a skill for this controller key (may carry ZL = second set) -- the host opens the skill tree,
// the player taps a skill (while the flow is in progress the front end turns the tap into 'cursor to the point + press Y') -> the original game's 「重新綁定技能」(Press button to rebind skill) box pops up -> the host presses the target key for the player -> the panel closes. dm_ui_bind_active() = flow in progress (0 none / 1 opening the skill tree / 2 waiting for the player to tap a skill / 3 wrapping up)
void dm_ui_bind_begin(uint64_t key_mask);
int dm_ui_bind_slot(void);   /* 綁定目標位 1..8（F1–F8），不在綁定態 0 | Binding target slot 1..8 (F1–F8), 0 when not in the binding state */
void dm_ui_safe_left(int units);    /* 左安全區，佈局單位（2160 高）；左上角傭兵 / 隊友頭像欄按它內移 | Left safe area, in layout units (2160 high); the mercenary / party member portrait bar in the top-left corner is moved inward by it */
void dm_ui_safe_right(int units);   /* 右安全區，佈局單位（2160 高）；右上角小地圖與信息欄按它內移 | Right safe area, in layout units (2160 high); the minimap and info bar in the top-right corner are moved inward by it */
uint32_t dm_ui_hit_id(void);   /* 最近一次命中的加點類按鈕：屬性「+」1..4（力量 / 敏捷 / 體力 / 精力），技能圖標 = 最高位置 1 的標識；沒命中 0 | The most recently hit point-spending button: attribute '+' 1..4 (strength / dexterity / vitality / energy), skill icon = an identifier with the top bit set to 1; 0 if nothing was hit */
int dm_ui_bind_active(void); uint64_t dm_ui_auto_release(void);   /* 取手柄處用：此刻要從玩家輸入裡鬆開的鍵 | Used where the controller is read: the keys to release from the player's input right now */ int dm_ui_bind_skill_hit(float *nx, float *ny);   /* 最近一次命中（吸附後）落在加點類按鈕上：1 = 技能樹的技能圖標，2 = 屬性頁的「+」；帶歸一化坐標 | The most recent hit (after snapping) landed on a point-spending button: 1 = a skill icon in the skill tree, 2 = a '+' on the attributes page; comes with normalized coordinates */
void dm_ui_bind_cancel(void);
void dm_ui_map_full(int on);
int dm_ui_map_is_full(void);
void dm_ui_text_entered(const char *text);   // 系統鍵盤返回時調：text = 寫回給遊戲的最終文本（UTF-8）；取消 / 空串傳 NULL | Called when the system keyboard returns: text = final text written back to the game (UTF-8); pass NULL for cancel / empty string
// 延遲讀數（診斷，DM_LAT_LOG=1）：關鍵時刻打一行「LAT <毫秒時刻> <事件>」，量手指落下 / 鬆手到遊戲生效的延遲
// Latency readout (diagnostic, DM_LAT_LOG=1): at key moments print a line 「LAT <毫秒時刻> <事件>」(LAT <millisecond timestamp> <event>), to measure the latency from finger down / release to the game taking effect
int dm_lat_on(void);
#define DM_LAT(fmt, ...) do { if (dm_lat_on()) LOG("LAT %llu " fmt, (unsigned long long)(now_ns() / 1000000), ##__VA_ARGS__); } while (0)
void dm_ui_hand_idle(int idle);                  // 觸摸前端每拍報：沒有手指、沒有在發 / 排著的鍵與光標請求 = 1（手上物品守門用，dm_cursor.c hand_guard） | Reported by the touch front end every tick: no fingers, no keys or cursor requests being sent / queued = 1 (used by the held-item guard, dm_cursor.c hand_guard)
uint32_t dm_ui_hover_unid(void);               // 光標此刻指著的我方未鑒定物品 id（背包 / 箱子 / 方塊 / 身上；沒有 = 0）。宿主每幀發佈，任意線程讀 | id of our own unidentified item the cursor is pointing at right now (inventory / stash / cube / equipped; none = 0). Published by the host every frame, read from any thread
void dm_ui_identify_hovered(void);              // 觸摸層雙擊未鑒定物品：宿主下一幀對它調免費鑒定（dm_item_identify_free） | Touch layer double-tap on an unidentified item: on the next frame the host calls free identify on it (dm_item_identify_free)
int dm_ui_map_geom(float *cell);   // 地圖幾何：返回 0 沒開 / 1 微縮 / 2 全屏，*cell = 每格像素（真實畫面像素，任意線程） | Map geometry: returns 0 not open / 1 minimap / 2 full screen, *cell = pixels per cell (real picture pixels, any thread)
