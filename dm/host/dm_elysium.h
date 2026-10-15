#include <stdint.h>
// 「極樂世界」：建角時勾選、之後不可改的按角色開關。該角色在局內時：暗金掉率 ×5、套裝 ×3（品質比例表）。實現與依據見 dm_elysium.c
// 「極樂世界」(Elysium): a per-character switch checked at character creation and unchangeable afterwards. While that character is in a run: unique drop rate ×5, sets ×3 (quality ratio table). For the implementation and its basis see dm_elysium.c
#pragma once
void dm_elysium_arm(int on);     // 建角頁的勾選項調：1 = 這次建出來的角色是極樂世界角色，0 = 不是。只對緊接著的一次建角生效，用掉即清 | Called by the checkbox on the character creation page: 1 = the character created this time is an Elysium character, 0 = it is not. Applies only to the immediately following character creation and is cleared once used
int dm_elysium_active(void);     // 當前這一局是不是極樂世界角色的局（表已改） | Whether the current run is the run of an Elysium character (tables changed)
void dm_elysium_pump(void);      // 遊戲主線程每幀調（dm_actions_pump） | Called every frame on the game main thread (dm_actions_pump)
void dm_elysium_stash_step(uint64_t game, uint64_t player, uint8_t *main_base);   // 遊戲服務端那一側每拍調（diag.c 讀玩家狀態處）：新建極樂世界角色後補齊共享倉庫 | Called every tick on the game server side (where diag.c reads the player state): tops up the shared stash after a new Elysium character is created
