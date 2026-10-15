// 點遊戲世界裡的目標 = 與它交互（實現與依據見 dm_world.c）
// Tapping a target in the game world = interacting with it (implementation and rationale in dm_world.c)
#pragma once
unsigned dm_world_tap(float nx, float ny);   // 任意線程：排一筆點按（出圖畫面歸一化坐標 [0,1]）；主線程取手柄處解析目標併發令，沒有目標什麼都不發。返回序號 | any thread: queue one tap (coordinates normalized to the rendered picture, [0,1]); the main thread resolves the target and sends the command at the controller-poll point, and sends nothing when there is no target. Returns the sequence number
int dm_world_tap_result(unsigned seq);       // -1 還沒執行；0 沒有目標；1 物品（拾取）2 NPC（對話）3 物件（使用）4 出口 5 屍體（取回） | -1 not executed yet; 0 no target; 1 item (pick up) 2 NPC (talk) 3 object (use) 4 exit 5 corpse (retrieve)
int dm_world_has_target(void);               // 任意線程：按主鍵會去交互的目標是否存在（遊戲手柄版自己的選擇） | any thread: whether a target that the primary button would interact with exists (the game's own controller-version selection)
void dm_world_pump(void);                    // 取手柄處每幀調（主線程） | called every frame where the controller is fetched (main thread)
void dm_world_watch(int type, uint32_t cls);     // 測試量具：盯住某類單位（type < 0 取消）；主線程每幀發佈它身體框中心的屏幕位置 | test probe: watch one kind of unit (type < 0 cancels); the main thread publishes the screen position of its body box center every frame
int dm_world_watch_pos(float *nx, float *ny);    // 任意線程：盯住的單位在畫面內返回 1 並給出歸一化坐標 | any thread: returns 1 and gives normalized coordinates when the watched unit is in the picture
// 小地圖任務終點（dm_world.c）：主線程每幀發佈，前端任意線程取。x / y = 世界格；art ≥ 64 = 塔拉夏符號第 art−64 幀，其餘 = 名牌號（tools/gen_map_goal_art.py）
// Minimap quest goals (dm_world.c): published by the main thread every frame, fetched by the front end from any thread. x / y = world cell; art >= 64 = frame art-64 of the Tal Rasha symbols, otherwise = nameplate number (tools/gen_map_goal_art.py)
typedef struct { int n, px, py; float sx, sy; int x[12], y[12]; unsigned char art[12]; } dm_map_goal_view;   // px / py 玩家世界格；sx / sy 玩家腳底在畫面上的位置（歸一化） | px / py player world cell; sx / sy position of the player's feet on screen (normalized)
int dm_map_goals(dm_map_goal_view *v);      // 返回終點個數（地圖標注開關關著、本關沒有終點、還沒掃完 = 0） | returns the number of goals (map marking switch off, no goal in this level, scan not finished = 0)
