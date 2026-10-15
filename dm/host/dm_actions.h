// 「先選中、再確認」的宿主入口：商人買賣、屬性 / 技能加點、數量框設值。實現與依據見 dm_actions.c、dm_textinput.c。
// 動作：任意線程調，遊戲主線程（取手柄處）執行。讀數：主線程每幀發佈快照，任意線程讀。
// Host entry points for "select first, then confirm": vendor buying/selling, stat / skill point allocation, setting the quantity box value. For implementation and rationale see dm_actions.c, dm_textinput.c.
// Actions: callable from any thread, executed on the game main thread (where the controller is fetched). Readings: the main thread publishes a snapshot every frame, readable from any thread.
#pragma once
#include <stdint.h>
typedef struct {
    int open;                 // 商人交易頁開著 | the vendor trade page is open
    int gold, stash_gold;     // 身上金幣、箱內金幣（買東西兩處都能扣，遊戲規則） | gold carried, gold in the stash (purchases can deduct from both, game rule)
    int sel_valid;            // 光標選中了一件貨 | the cursor has selected a piece of goods
    uint32_t sel_id; char sel_code[4]; char sel_name[64]; int buy_price;
    int sel_gx, sel_gy, sel_gw, sel_gh;   // 選中那件貨在貨架上的左上格與佔格（格數；sel_valid 為 0 時無意義）。換成屏幕矩形給拖貨跟手圖用 | top-left cell and footprint of the selected goods on the shelf (in cells; meaningless when sel_valid is 0). Converted to a screen rectangle for the drag-goods follow image
    int can_buy;              // 錢夠（買價 ≤ 身上 + 箱內金幣）；背包放不放得下由遊戲在購買時判 | enough gold (buy price ≤ gold carried + gold in the stash); whether the inventory has room is judged by the game at purchase time
    int held_valid;           // 手上拿著物品（拖著） | an item is held in hand (being dragged)
    uint32_t held_id; char held_name[64]; int sell_price;   // sell_price = 0：這件不能賣 | sell_price = 0: this item cannot be sold
} dm_shop_info;
void dm_shop_get(dm_shop_info *out);
int  dm_shop_open(void);
void dm_shop_buy(void);                   // 買選中的那件（遊戲自己的購買路徑；錢不夠 / 背包放不下由遊戲拒並彈原版提示，這裡不攔） | buy the selected item (the game's own purchase path; not enough gold / no room in the inventory is rejected by the game with the original game's prompt, not blocked here)
void dm_shop_sell_cursor(void);           // 賣手上拿著的那件 | sell the item held in hand

typedef struct {
    int stat_points, skill_points;        // 剩餘屬性點 / 技能點 | remaining stat points / skill points
    int tree_open;                        // 技能樹頁開著 | the skill tree page is open
    int char_open;                        // 角色頁開著（且高級屬性頁沒蓋在上面）；手機層據此畫屬性「確認 +1 / +5 / 全部」 | the character page is open (and the advanced stats page is not covering it); the phone layer draws the stat "confirm +1 / +5 / all" based on this
    int sel_skill;                        // 技能樹頁當前選中的技能號，-1 = 沒有 | skill id currently selected on the skill tree page, -1 = none
    int sel_can_add;                      // 還能不能加（= sel_block == 0） | whether a point can still be added (= sel_block == 0)
    int sel_block;                        // 不能加的原因：0 可加，1 沒有技能點，2 等級不夠，3 前置技能沒學，4 其它（已到上限等） | reason a point cannot be added: 0 can add, 1 no skill points, 2 level too low, 3 prerequisite skill not learned, 4 other (already at the cap, etc.)
    char sel_name[64];
} dm_points_info;
void dm_points_get(dm_points_info *out);
void dm_stat_add(int stat, int n);        // stat：0 力量 1 敏捷 2 體力 3 精力；n：1 / 5 / -1 全部（角色頁開著時有效） | stat: 0 strength 1 dexterity 2 vitality 3 energy; n: 1 / 5 / -1 all (valid while the character page is open)
void dm_skill_add_selected(void);         // 給技能樹頁當前選中的技能加 1 點 | add 1 point to the skill currently selected on the skill tree page
void dm_belt_put(uint32_t item_id);        // 把背包裡的這件物品放進腰帶（遊戲自己的「移到腰帶」路徑；不是藥水 / 腰帶滿時不動）。item_id 用 dm_cursor_item 的 id | put this inventory item into the belt (the game's own "move to belt" path; does nothing if it is not a potion / the belt is full). item_id uses the id from dm_cursor_item
void dm_item_use(uint32_t item_id);         // 立即使用這件物品（原版「使用」同一個函數：手柄按住確認鍵 / 鍵鼠右鍵的終點；不等長按門檻）。喝藥、讀捲軸、用書、開方塊。item_id 用 dm_cursor_item 的 id | use this item immediately (the same function as the original game's "use": the endpoint of controller hold-confirm / keyboard-and-mouse right click; does not wait for the long-press threshold). Drinks potions, reads scrolls, uses tomes, opens the cube. item_id uses the id from dm_cursor_item
int  dm_ui_hover_now(int x, int y);        // 把原版手柄光標放到 (x, y)（與 dm_cursor.c 寫的是同一對坐標）並當場讓遊戲重算懸停 / 焦點 / 選中格，返回 1 = 已刷新、0 = 此刻不是手柄光標模式。只能在遊戲主線程（取手柄處）調 | put the original game's controller cursor at (x, y) (the same coordinate pair dm_cursor.c writes) and have the game recompute hover / focus / selected cell on the spot; returns 1 = refreshed, 0 = not in controller cursor mode right now. May only be called on the game main thread (where the controller is fetched)
void dm_item_compare(int on);              // 裝備對比開 / 關：開著時，懸停 / 選中的裝備說明框旁並排出身上同部位那件（原版「按住比較鍵」的同一個狀態位，不經手柄鍵， | equipment comparison on / off: while on, the item worn in the same slot is shown side by side next to the hovered / selected equipment's tooltip (the same state bit as the original game's "hold the compare button", not going through the controller button,
                                           // 所以商人頁、儲藏箱頁裡比較鍵被翻頁簽佔用也能用）。任意線程調；開著期間宿主每幀保持。賭博頁原版不支持對比，開了也不出
                                           // so it also works on the vendor page and the stash page, where the compare button is taken up by tab switching). Callable from any thread; while on, the host maintains it every frame. The original game does not support comparison on the gambling page, so nothing shows there even when on
void dm_belt_expand(int on);               // 展開 / 收起腰帶上面幾排（原版鍵鼠「顯示腰帶」動作開關的同一個面板，面板號 0x1a）。任意線程調 | expand / collapse the upper rows of the belt (the same panel toggled by the original game's keyboard-and-mouse "show belt" action, panel id 0x1a). Callable from any thread
void dm_belt_use(int col, int row);        // 喝腰帶裡第 row 排（0 = 底排）第 col 列（0–3）的那一瓶；那一格是空的就不動。走 dm_item_use 的腰帶分支。任意線程調 | drink the bottle in row `row` (0 = bottom row), column `col` (0–3) of the belt; does nothing if that cell is empty. Goes through the belt branch of dm_item_use. Callable from any thread
void dm_belt_use_merc(int col, int row);   // 同上，但餵給雇傭兵（腰帶雙擊）；雇傭兵不在 / 已死 / 這瓶不能餵就什麼都不做 | same as above, but feeds it to the mercenary (belt double-tap); does nothing if the mercenary is absent / dead / this bottle cannot be fed
int  dm_belt_rows(int *expanded);          // 腰帶排數（1–4，取決於身上的腰帶）；expanded 非空時寫 1 = 現在展開著。讀的是主線程每幀發佈的快照 | belt row count (1–4, depending on the belt worn); when expanded is non-null, writes 1 = currently expanded. Reads the snapshot the main thread publishes every frame
void dm_held_equip(int bodyloc);          // 把手上拿著的物品放到身上這個欄位（原版裝備槽「手上有物品時按確認鍵」的同一個函數 main+0x3bb24c；空槽 = 裝上，有物品 = 交換；原版拒絕的不動）。任意線程調 | put the held item into this body slot (the same function as the original game's equipment slot "press confirm while holding an item", main+0x3bb24c; empty slot = equip, occupied = swap; nothing happens for what the original game rejects). Callable from any thread
void dm_inv_sort(int page);                // 整理（自動排列）：page 0 背包、3 赫拉迪克方塊、4 個人儲藏箱當前頁。原版「按住排序」的終點函數，不等長按。任意線程調 | sort (auto-arrange): page 0 inventory, 3 Horadric Cube, 4 current page of the personal stash. The endpoint function of the original game's "hold to sort", without waiting for the long press. Callable from any thread
void dm_name_random(void);                 // 建角起名頁：給名字框填一個隨機名（原版「隨機名字」鍵的同一條消息）。名字面板開著時有效。任意線程調 | character creation naming page: fill the name box with a random name (the same message as the original game's 「隨機名字」(Random Name) button). Valid while the name panel is open. Callable from any thread
int dm_name_taken(const char *name);       // 已有這個名字的角色（存檔工作目錄下有同名目錄）返回 1 | returns 1 if a character with this name already exists (a same-named directory exists under the save working directory)
int dm_name_gen(char *out, int cap, int (*taken)(const char *));   // 起名頁留空時用的隨機名（dm_namegen.c）：音節拼接，taken 非 0 就再抽，至多 64 次；成功返回 1 | random name used when the naming page is left blank (dm_namegen.c): syllable concatenation, draws again if taken is non-zero, at most 64 times; returns 1 on success
void dm_item_equip(uint32_t item_id);       // 立即裝備背包（或箱子 / 方塊）裡的這件到對應欄位（原版「按住確認鍵 = 裝備」的終點；欄位由遊戲選，已有裝備時遊戲自己交換；雇傭兵面板開著且他能穿時給雇傭兵，同原版）。 | immediately equip this item from the inventory (or stash / cube) into the corresponding slot (the endpoint of the original game's "hold confirm = equip"; the slot is chosen by the game, and the game swaps by itself when something is already equipped; when the mercenary panel is open and he can wear it, it goes to the mercenary, same as the original game).
                                           // 穿不了（等級 / 屬性不夠、未鑒定、不是裝備）時只播原版的錯誤音，不發包
                                           // When it cannot be worn (level / stats too low, unidentified, not equipment) only the original game's error sound is played and no packet is sent
void dm_item_unequip(uint32_t item_id);     // 立即把身上穿著的這件卸到背包（原版裝備欄「按住確認鍵 = 卸下」的終點；背包放不下時遊戲播錯誤音、不動）。商人頁開著時不執行（原版那條路在商人頁裡會變成賣掉） | immediately unequip this worn item into the inventory (the endpoint of the original game's equipment panel "hold confirm = unequip"; when the inventory cannot hold it the game plays the error sound and does nothing). Not executed while the vendor page is open (in the vendor page that original path would turn into selling it)
void dm_item_identify(uint32_t target_id);   // 「用了鑒定捲軸 / 鑒定書、等待選目標」狀態下（dm_item_targeting() 非 0），對這件物品完成使用（原版物品格 / 裝備欄點擊處理在該狀態下的終點：發「對物品使用物品」包；捲軸消耗 / 書扣次數由服務端照原版做）。 | in the "used an identify scroll / identify tome, waiting to pick a target" state (dm_item_targeting() non-zero), complete the use on this item (the endpoint of the original game's item cell / equipment panel click handling in that state: sends the "use item on item" packet; scroll consumption / tome charge deduction is done by the server as in the original game).
                                             // 不在該狀態、目標不是自己身上 / 背包 / 箱子 / 方塊裡的物品、目標有待確認操作時不調（日誌寫原因）
                                             // Not called when not in that state, when the target is not an item on oneself / in the inventory / stash / cube, or when the target has an operation pending confirmation (the log writes the reason)
void dm_item_repair(uint32_t item_id);        // 商人頁修理態下修這一件（原版修理態點物品的同一個終點）；不在修理態 / 手上有東西 / 原版判定不用修時不調（日誌寫原因） | repair this one item in repair mode on the vendor page (the same endpoint as clicking an item in the original game's repair mode); not called when not in repair mode / something is in hand / the original game judges no repair is needed (the log writes the reason)
void dm_belt_fill(uint64_t belt_widget);     // 物品欄裡的腰帶控件：背包裡能進腰帶的藥一次裝滿空位（原版手柄「焦點在腰帶上按住 R」的同一段）；條件不滿足不動。任意線程可調 | the belt widget in the inventory panel: fill the empty slots in one go with potions from the inventory that can go into the belt (the same code as the original controller "hold R with focus on the belt"); does nothing if the conditions are not met. Callable from any thread
void dm_item_target_cancel(void);            // 退出「等待選目標」狀態，不耗捲軸（原版點到空處 / 無效目標時做的同一段：清待用物品、光標狀態復位）。不在該狀態時什麼都不做 | leave the "waiting to pick a target" state without consuming the scroll (the same code the original game runs when clicking on empty space / an invalid target: clears the pending item, resets the cursor state). Does nothing when not in that state
void dm_stash_take(int count);                   // 高級倉庫（寶石 / 材料 / 符文頁）裡光標此刻指著的那一疊，取 count 件到背包（命令 0x63、去向 = 背包；1 件走格控件方法 main+0x39d824，多件走 main+0x39d9e0 帶件數）。 | from the stack the cursor is pointing at right now in the advanced stash (gems / materials / runes pages), take count pieces into the inventory (command 0x63, destination = inventory; 1 piece goes through the grid widget method main+0x39d824, multiple pieces go through main+0x39d9e0 with a piece count).
                                                 // count 超過疊數時取剩下的全部。光標下沒有物品 / 不是成疊物品 / 箱子沒開時不動，日誌「取多件：」寫原因。背包放不下時由服務端決定取幾件（沒驗）
                                                 // When count exceeds the stack count, takes all that remain. Does nothing when there is no item under the cursor / it is not a stacked item / the stash is not open; the log 「取多件：」(take multiple:) writes the reason. When the inventory cannot hold them, the server decides how many to take (not verified)
void dm_stash_take_id(uint32_t item_id, int count);   // 同上，按物品 id 指定（測試用） | same as above, specified by item id (for testing)
void dm_num_set(int n);                   // 數量框設值：≥0 該數，-1 全部，-2 一半（dm_textinput.c） | set the quantity box value: ≥0 that number, -1 all, -2 half (dm_textinput.c)
void dm_actions_pump(void);               // 取手柄處每幀調（主線程） | called every frame where the controller is fetched (main thread)
void dm_held_stow(void);                     // 把手上（光標上）那件物品收進背包、不關面板：原版「手上有物品時按返回」做的那一步（main+0x3cf4d0：給物品在背包裡找位置併發 0x18）。任意線程調，下一幀在遊戲主線程執行 | put the item in hand (on the cursor) into the inventory without closing the panel: the step the original game does for "press back while holding an item" (main+0x3cf4d0: finds a place in the inventory for the item and sends 0x18). Callable from any thread, executed on the game main thread next frame
int dm_held_stow_result(void);               // 最近一次 dm_held_stow 的結果：-1 還沒出結果（沒放成會逐幀重試約 1.5 秒：剛交換到手上的物品要等服務器回執）；1 已放進背包（已發包）；0 沒放成，手上的物品不動 | result of the most recent dm_held_stow: -1 no result yet (if it was not stowed it retries every frame for about 1.5 seconds: an item just swapped into the hand has to wait for the server's acknowledgement); 1 put into the inventory (packet sent); 0 not stowed, the held item is untouched
void dm_item_identify_free(uint32_t item_id);   // 免費鑒定這件（未鑒定物品雙擊）：服務端拍裡調原版鑒定函數 main+0x78acf0，不消耗捲軸 / 書 | identify this item for free (double-tap on an unidentified item): calls the original game's identify function main+0x78acf0 in the server tick, without consuming a scroll / tome
// 副手組技能一鍵放（dm_actions.c 那一節）
// One-press casting of off-hand set skills (that section of dm_actions.c)
uint64_t dm_swapskill_filter(uint64_t buttons);   // 取手柄狀態處（hle_hid.c）：壓住 / 放行要先換武器的那個鍵 | where the controller state is fetched (hle_hid.c): hold back / let through the button that needs a weapon swap first
void dm_swapskill_srv_wset(int w);                // 服務端拍（diag.c hud_tick）發佈當前武器組 0 / 1 | the server tick (diag.c hud_tick) publishes the current weapon set 0 / 1
int dm_swapskill_slot(int slot, int *charges);    // 技能鈕顯示：1 = 技能在另一組（「II」角標），2 = 充能技能（*charges = 剩餘次數） | skill button display: 1 = the skill is in the other set ("II" badge), 2 = charged skill (*charges = remaining charges)
int dm_swapskill_equipped(int32_t item_id);       // 這件物品身上穿著（含換手那組） | this item is worn on the body (including the weapon-swap set)
