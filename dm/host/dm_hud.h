#pragma once
// 底部 UI v4 的 HUD 數字（宿主每個服務端拍讀一次，前端 UIKit 畫「Lv · %」、角標、難度數字）與難度檔位請求。
// 難度走原版 /players 處理函數（main+0x415db0）同一條鏈：main+0x3102c0 取人數設置對象 → main+0x1199c50(對象, N)；
// 在下一個服務端拍入口（遊戲線程）落地；原版規則對此後新生成的怪生效（實測：8 檔 stat 100 = 8、最大生命 floor(×4.5)）。
// HUD numbers for bottom UI v4 (read by the host once per server tick; the front end UIKit draws 'Lv · %', badges and the difficulty number) and difficulty level requests.
// Difficulty goes through the same chain as the original game's /players handler (main+0x415db0): main+0x3102c0 gets the players setting object -> main+0x1199c50(object, N);
// it is applied at the next server tick entry (game thread); the original game's rules apply to monsters spawned from then on (measured: level 8 stat 100 = 8, max life floor(×4.5)).
typedef struct {
    int valid;          // 局內且讀到受控玩家 | In a run and the controlled player has been read
    int level;          // stat 12
    int exp_permille;   // 本級經驗進度 ‰（原版 experience.txt 門檻） | Experience progress within the current level ‰ (thresholds from the original game's experience.txt)
    int belt[4];        // 腰帶四列各剩幾件（腰帶物品 x % 4 = 列） | How many items remain in each of the four belt columns (belt item x % 4 = column)
    int portal;         // 回城次數：回城書 tbk 的數量（stat 70）+ 回城捲軸 tsc 張數 | Town portal count: quantity in the Tome of Town Portal tbk (stat 70) + number of Scroll of Town Portal tsc
    int players;        // 當前人數檔（人數設置對象 +0xc）；0 = 未讀到 | Current players setting (players setting object +0xc); 0 = not read
    int hp, hp_max, mp, mp_max;   // 生命 / 法力（stat 6–9 ÷ 256，取整） | Life / mana (stat 6–9 ÷ 256, rounded to an integer)
    int area, x, y;     // 所在區域號（levels.txt Id；0 = 未讀到）與玩家坐標（格） | Current area number (levels.txt Id; 0 = not read) and player coordinates (cells)
    int points;         // 未分配屬性點 stat 4 + 技能點 stat 5（紅方十字亮態） | Unassigned attribute points stat 4 + skill points stat 5 (lit state of the red square cross)
    int holding;        // 光標上拿著物品（拖放沒放成、物品還在手上）：前端據此把「點空白處」當取消 | An item is held on the cursor (a drag-and-drop did not complete and the item is still in hand): the front end uses this to treat 'tap on empty space' as cancel
    int quest_unread;   // 有新任務待讀：原版 HUD 任務提示亮著（客戶端界面狀態字節 main+0x2d5ff58）；打開任務面板後清（底欄十字紅態的另一個條件） | A new quest is waiting to be read: the original game's HUD quest hint is lit (client UI state byte main+0x2d5ff58); cleared after the quest panel is opened (the other condition for the red state of the bottom bar cross)
    int dead;           // 玩家死亡（模式 0 / 17）：前端隱藏 UIKit 底欄，免擋死亡畫面 | Player dead (mode 0 / 17): the front end hides the UIKit bottom bar so it does not block the death screen
} dm_hud_info;
// 玩家「已死」判定 = 遊戲自己的定義（main+0x6e5c48 / 0x6e5cc8 客戶端命令分派：玩家單位 [+0xc] 模式為 0 或 0x11 即按已死處理；
// D2 玩家模式 0 = DEATH 死亡動畫中、17 = DEAD 已死，死亡動畫結束時 0 → 17，復活回城後變為城鎮站立 5）
// Player 'dead' test = the game's own definition (main+0x6e5c48 / 0x6e5cc8 client command dispatch: a player unit whose [+0xc] mode is 0 or 0x11 is handled as dead;
// D2 player mode 0 = DEATH, in the death animation, 17 = DEAD, already dead; 0 -> 17 when the death animation ends, and it becomes town standing 5 after reviving back in town)
static inline int dm_hud_player_mode_dead(unsigned mode) { return mode == 0 || mode == 17; }
// UIKit 底欄顯隱：只在局內常態顯示——未讀到玩家、死亡、導航態（面板 / 讀取畫面 / 過場）、被面板棧擋住時都讓開
// 五幕城鎮的區域號（levels.txt Id）：流亡者營地 1、魯高因 40、庫拉斯特海港 75、群魔堡壘 103、哈洛加斯 109
// UIKit bottom bar visibility: shown only in the normal in-run state -- it gets out of the way when the player has not been read, on death, in navigation states (panel / loading screen / cutscene), and when blocked by the panel stack
// Area numbers of the five act towns (levels.txt Id): Rogue Encampment 1, Lut Gholein 40, Kurast Docks 75, Pandemonium Fortress 103, Harrogath 109
static inline int dm_hud_area_is_town(int area) { return area == 1 || area == 40 || area == 75 || area == 103 || area == 109; }
static inline int dm_hud_bar_visible(const dm_hud_info *h, int navigation, int blocked) { return h->valid && !h->dead && !navigation && !blocked; }
void dm_hud_get(dm_hud_info *out);
void dm_hud_request_players(int n);   // 1..8；任意線程調用 | 1..8; callable from any thread
// 宿主內部（diag.c 拍入口）：取最新請求、發佈本拍讀數
// Host internal (diag.c tick entry): take the latest request, publish this tick's readings
int dm_hud_requested_players(void);
void dm_hud_publish(const dm_hud_info *in);
