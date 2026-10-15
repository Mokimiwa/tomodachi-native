#pragma once
// 首領連戰：哈洛加斯的門 → 亞瑞特之巔打 5 關 → 門進世界之石大殿，強制恐怖地帶打 5 關 → 地獄再加 2 關。實現與依據見 dm_bossrush.c
// Boss rush: the door in Harrogath → 5 stages at the Arreat Summit → a door into the Worldstone Chamber, 5 stages with the terror zone forced → 2 more stages in Hell. For implementation and rationale see dm_bossrush.c
#include <stdint.h>
void dm_bossrush_tick(uint64_t game, uint64_t player, uint8_t *main_base);   // 服務端每拍調（diag.c 讀玩家狀態處） | called every server tick (where diag.c reads the player state)
void dm_bossrush_c2s(uint64_t game, uint64_t player, const uint8_t *pkt, uint32_t len);   // 客戶端命令包分派入口（觀測門 OBS_C2S）每包調 | called per packet at the client command packet dispatch entry (observation gate OBS_C2S)
void dm_game_notice(uint8_t *main_base, const char *text);   // 原版系統提示（聊天欄一行 + 系統提示框），服務端線程調 | original-game system notification (one chat bar line + the system notification box), called from the server thread
int dm_bossrush_owns(uint32_t id);                            // 這個怪物單位是首領連戰本關刷出來的 | this monster unit was spawned by the current boss rush stage
void dm_kills_tick(uint64_t G, uint64_t player, uint8_t *main_base);   // 刷怪積分牌（dm_kills.c），服務端拍裡調 | kill board (dm_kills.c), called in the server tick
void dm_door_label(uint8_t *main_base, uint64_t game, int cls, const char *key, const char *zh, const char *en, int in_town);   // 我們建的門的名牌：城裡把這一類物件的串鍵換成 key（掛進原版串表），離城寫回 | name label of the doors we create: in town the string key of this object class is replaced with key (attached to the original game's string table), and written back on leaving town
