// 單位 → 所在關卡（宿主直接讀遊戲內存，服務端單位與客戶端單位同一套偏移）：
//   單位 +0x38 路徑 → 路徑 +0x20 房間（物件 / 物品類單位的「路徑」是另一種結構，房間在 +0）→ 房間 +0x18 → +0x90 關卡 → 關卡 +0x1f8 關卡號。
// 每一步都可能是空的，必須逐步判空：換幕的讀取畫面期間客戶端玩家有路徑、沒有房間。
// 原先各處一口氣連讀四層 rd(rd(rd(rd(u + 0x38) + 0x20) + 0x18) + 0x90)，rd 只擋地址 0，房間為空時去讀地址 0x18 →
// 真機 / Mac 都是 sig=11 addr=0x18（秘境裡死在別的幕的圖裡點「繼續」、換幕讀取那幾幀）。單測 tests/t_unit_level.c。
// Unit → the level it is in (the host reads game memory directly; server units and client units share the same offsets):
//   unit +0x38 path → path +0x20 room (for object / item class units the "path" is a different struct, with the room at +0) → room +0x18 → +0x90 level → level +0x1f8 level id.
// Every step can be NULL and must be null-checked one by one: during the act-change loading screen the client player has a path but no room.
// Previously various places read four levels in one go, rd(rd(rd(rd(u + 0x38) + 0x20) + 0x18) + 0x90); rd only guards address 0, so with a NULL room it went on to read address 0x18 →
// sig=11 addr=0x18 on both real device and Mac (in a rift, tapping 「繼續」(Continue) after dying in another act's map, during those few act-change loading frames). Unit test tests/t_unit_level.c.
#ifndef DM_UNIT_H
#define DM_UNIT_H
#include <stdint.h>
static inline uint64_t dm_unit_rd64(uint64_t a) { return a ? *(volatile uint64_t *)(uintptr_t)a : 0; }
// 關卡結構指針；任何一步為空返回 0
// Level struct pointer; returns 0 if any step is NULL
static inline uint64_t dm_unit_level_ptr(uint64_t u) {
    uint64_t path = u ? dm_unit_rd64(u + 0x38) : 0, room = path ? dm_unit_rd64(path + 0x20) : 0, ex = room ? dm_unit_rd64(room + 0x18) : 0;
    return ex ? dm_unit_rd64(ex + 0x90) : 0;
}
// 關卡號；讀不到（單位 / 路徑 / 房間任何一步為空）返回 0 = 未知關卡
// Level id; returns 0 = unknown level when it cannot be read (unit / path / room NULL at any step)
static inline int dm_unit_level(uint64_t u) { uint64_t lv = dm_unit_level_ptr(u); return lv ? *(volatile int32_t *)(uintptr_t)(lv + 0x1f8) : 0; }
// 關卡的房間鏈頭（關卡 +0x10，下一間 = 房間 +0x48）；關卡指針為空返回 0——拿 dm_unit_level_ptr 的結果往下遍歷房間一律走這裡，
// 不讓「讀不到關卡」變成去讀地址 0x10
// Head of a level's room chain (level +0x10, next room = room +0x48); returns 0 when the level pointer is NULL - anything that walks rooms from the result of dm_unit_level_ptr must go through here,
// so that "level could not be read" never turns into reading address 0x10
static inline uint64_t dm_level_rooms(uint64_t lv) { return lv ? dm_unit_rd64(lv + 0x10) : 0; }
// 定點傳送：把單位放到 (目標房間, x, y) 旁的空位並給客戶端發重定位 = 原版 main+0x7cc530(遊戲, 單位, 目標房間, x, y, 0, 0)。
// 目標房間必須給：原版按關卡號換關 0x634000 自己就是「0x5596c0 取落點與房間 → 0x7cc530(…, 房間, x, y, 0, 0)」（0x634168–0x634188）。
// 傳 0 時只有目標就在單位所在房間附近才成，目標在另一片房間就返回失敗（秘境打完回城後點「繼續」落在那張圖的小站上）。
// 房間為 0（目標那間沒載入 / 不在這張圖裡）直接返回 0，不去碰運氣
// Fixed-point teleport: put the unit in a free spot next to (target room, x, y) and send the client a relocation = the original main+0x7cc530(game,
// unit, target room, x, y, 0, 0).
// The target room must be given: the original change-level-by-number 0x634000 is itself "0x5596c0 gets the landing point and room → 0x7cc530(..., room,
// x, y, 0, 0)" (0x634168–0x634188).
// Passing 0 only works when the target is near the unit's current room; a target in another cluster of rooms returns failure (after finishing a Rift and
// returning to town, tapping "Continue" landed on that map's waypoint).
// A room of 0 (the target room is not loaded / not in this map) returns 0 directly without taking chances
static inline int dm_teleport(const uint8_t *main_base, uint64_t G, uint64_t unit, uint64_t room, int x, int y) {
    if (!main_base || !G || !unit || !room) return 0;
    return ((int (*)(uint64_t, uint64_t, uint64_t, int, int, int, int))(uintptr_t)(main_base + 0x7cc530))(G, unit, room, x, y, 0, 0);
}
#endif
