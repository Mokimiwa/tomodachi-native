// 物品面板裡「手柄光標 / 指針當前指著的物品」：按遊戲自己的懸停對象取（底部按鍵提示用的那個函數），不按格子坐標反算。
// 只許遊戲主線程調（取手柄處 / 界面消息回調）：讀的是客戶端界面控件和客戶端單位表，同步、只讀、不加鎖。
// 'The item the controller cursor / pointer is currently pointing at' in an item panel: taken from the game's own hover object (the function used by the bottom key hints), not back-computed from cell coordinates.
// May only be called from the game main thread (where the controller is read / UI message callbacks): it reads client UI widgets and the client unit table, synchronously, read-only, without locking.
#pragma once
#include <stdint.h>
enum { DM_ITEM_AT_NONE, DM_ITEM_AT_INVENTORY, DM_ITEM_AT_BELT, DM_ITEM_AT_EQUIP, DM_ITEM_AT_STASH, DM_ITEM_AT_CUBE, DM_ITEM_AT_SHOP, DM_ITEM_AT_TRADE };
typedef struct {
    uint32_t id, cls;       // 物品單位 id、物品表行號 | Item unit id, item table row number
    char code[4];           // 物品碼（去尾空格，如 "hp1"、"box"），以 0 結尾 | Item code (trailing spaces removed, e.g. "hp1", "box"), 0-terminated
    int container;          // DM_ITEM_AT_*
    int potion, scroll, book;   // 物品類型（遊戲的類型歸屬函數）：藥水 / 捲軸 / 書 | Item type (the game's type membership function): potion / scroll / book
    int beltable;           // 類型上可放腰帶（物品類型表的腰帶標誌） | Beltable by type (the belt flag in the item type table)
    int belt_room;          // 可放腰帶且此刻腰帶有位（遊戲「移到腰帶」提示前的同一個檢查） | Beltable and the belt has room right now (the same check that precedes the game's 「移到腰帶」(Move to Belt) hint)
    int equippable;         // 此刻能裝備到身上（遊戲「裝備」提示前的同一個檢查） | Can be equipped right now (the same check that precedes the game's 「裝備」(Equip) hint)
    int stack;              // 高級倉庫（寶石 / 材料 / 符文頁）裡這一格的疊數；不是成疊物品 = 0（物品數據 +0x9c，原版長按取多件前判的同一個字節） | Stack count of this cell in the advanced stash (gem / material / rune pages); not a stacked item = 0 (item data +0x9c, the same byte the original game checks before a long-press takes multiple items)
    int unid;               // 未鑒定：物品數據（[物品+0x10]）+0x18 位 0x10 為 0（原版「已鑒定」標誌；測試代碼 dm_actions.c 反著改過兩端覈實） | Unidentified: bit 0x10 of item data ([item+0x10]) +0x18 is 0 (the original game's 'identified' flag; verified at both ends by flipping it in the test code dm_actions.c)
    int gx, gy, gw, gh;     // 物品在容器裡的左上格與佔格（背包 / 箱子 / 方塊 / 商店 / 交易）；腰帶給 (格號, 0, 1, 1)，裝備位給 (0, 0, 1, 1) | Top-left cell and cells occupied of the item in its container (inventory / stash / cube / shop / trade); for the belt gives (cell number, 0, 1, 1), for an equipment slot gives (0, 0, 1, 1)
} dm_cursor_item_info;
int dm_cursor_item(dm_cursor_item_info *out);   // 1 = 指著物品並已填 out；0 = 沒有（面板沒開 / 指著空格 / 焦點不在物品格上） | 1 = pointing at an item and out has been filled; 0 = none (panel not open / pointing at an empty cell / focus not on an item cell)
int dm_held_size(int *w, int *h);              // 手上拿著的那件的佔格（寬、高）；沒拿著返回 0。「用了鑒定捲軸等目標」那種不算拿著 | Cells occupied by the item held in hand (width, height); returns 0 if nothing is held. The 'identify scroll used, waiting for a target' case does not count as held
int dm_held_pending(void);                     // 手上那件此刻有沒有待確認的操作（上一個指令還沒答完）；沒拿著返回 0 | Whether the item in hand has an operation pending confirmation right now (the previous command has not been answered yet); returns 0 if nothing is held
int dm_vendor_repair_mode(void);                // 商人頁此刻是否處在「修理」光標模式（點了修理鈕、錘子光標）。只許遊戲主線程調 | Whether the vendor page is in the 'repair' cursor mode right now (repair button tapped, hammer cursor). May only be called from the game main thread
int dm_item_targeting(void);                    // 此刻是否處在「用了一件物品、等待選目標物品」的狀態：0 無；非 0 = 物品表裡該物品的目標類型 + 1（鑒定捲軸 / 鑒定書都走這個狀態） | Whether the 'an item was used, waiting to select a target item' state is in effect right now: 0 none; non-zero = the target type of that item in the item table + 1 (identify scroll / identify tome both go through this state)
