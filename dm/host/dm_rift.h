// 秘境爬塔（dm_rift.c）
// Rift tower climb (dm_rift.c)
#pragma once
#include <stdint.h>
typedef struct { int tier; float progress; int secs_left, done, guardian, inc; } dm_rift_hud_t;
void dm_rift_tick(uint64_t G, uint64_t player, uint8_t *main_base);   // 服務端拍（diag.c） | Server tick (diag.c)
void dm_rift_c2s(uint64_t G, const uint8_t *pkt, uint32_t len);      // 客戶端命令（點門） | Client command (door tap)
void dm_rift_client_pump(uint8_t *main_base);                         // 取手柄處（dm_world.c）：認擊殺、新刷出的怪 | At the controller sampling point (dm_world.c): recognize kills and newly spawned monsters
int dm_rift_hud(dm_rift_hud_t *out);                                  // 前端每幀：秘境進行中 / 剛完成返回 1 | Front end every frame: returns 1 while a rift is in progress / just completed
int dm_rift_active(void);
void dm_rift_gamble_vals(int best, uint32_t *uniq, uint32_t *set);   // 賭博加成：最高通關層 → 地獄暗金 / 套裝值（十萬分之一）；best < 1 = 0（不加） | Gambling bonus: highest cleared level -> Hell unique / set values (in units of 1/100000); best < 1 = 0 (no bonus)
typedef struct { int menu, best, unlock, def; unsigned ms; int clvl, tier, done, secs_left; float progress; int inc; } dm_rift_menu_t;   // menu 1 = 選層頁、2 = 繼續 / 放棄；inc = 0 且 done = 超時打倒守護者（沒過關） | menu 1 = level selection page, 2 = continue / quit; inc = 0 with done set = Guardian defeated after time ran out (not passed)
int dm_rift_cleared_any(void);              // 這個角色打通過任意一層秘境沒有（限時內過關才算：最高通關層 ≥ 1）——技能硬點上限的解鎖條件（dm_actions.c） | has this character cleared any Greater Rift level (only clears within the time limit count: highest cleared level ≥ 1): the unlock condition for the hard skill point cap (dm_actions.c)
int dm_rift_merc_unlocked(void);            // 傭兵新裝備位解沒解鎖（秘境最高通關 ≥ 10 層） | Whether the new mercenary equipment slots are unlocked (highest rift level cleared ≥ 10)
void dm_rift_merc_locked_hint(void);        // 沒解鎖時的提示「通關地獄後打通秘境10層解鎖」 | Hint when not unlocked: 「通關地獄後打通秘境10層解鎖」(Clear Greater Rift Level 10 to unlock)
int dm_rift_tier_level(int tier, int clvl);   // 這一層的怪物等級（9 層以下 0 = 不改） | this level's monster level (below level 9, 0 = unchanged)
int dm_rift_tier_hp_x10(int tier, int clvl);  // 這一層怪物生命總倍率 ×10（相對地獄 /players 1 的原生怪） | total life multiplier of this level's monsters ×10 (relative to a native Hell /players 1 monster)
int dm_rift_menu(dm_rift_menu_t *out);      // 前端每幀：要顯示的頁（0 = 不顯示） | Front end every frame: the page to show (0 = none)
void dm_rift_menu_close(void);              // 關閉（右上角 X） | Close (the X at the top right)
void dm_rift_menu_start(int tier);          // 選層頁「開始」 | Level selection page 「開始」(Enter Rift)
void dm_rift_menu_continue(void);           // 「繼續」 | 「繼續」(Continue)
void dm_rift_menu_abandon(void);
int dm_rift_menu_pending(void); int dm_rift_menu_seq(void); int dm_rift_swept(uint32_t id);            // 「放棄」（放棄後服務端再打開選層頁） | 「放棄」(Quit) (after abandoning, the server opens the level selection page again)
void dm_rift_menu_peek(dm_rift_menu_t *out);   // 沒有要開的頁時也取頁面讀數（編年史「秘境」頁） | Fetch the page readout even when there is no page to open (chronicle 「秘境」(Rift) page)
void dm_rift_ui_pump(void);                     // dm_rift_ui.c：遊戲主線程每幀（dm_actions.c） | dm_rift_ui.c: every frame on the game main thread (dm_actions.c)
void dm_rift_ui_on_uimsg(const char *cat, const char *act, const char *arg);   // 界面消息（diag.c） | UI messages (diag.c)
typedef struct { int best, clears, deaths, tier, unlock; unsigned ms; int fast[101]; } dm_rift_stats_t;
int dm_rift_stats(dm_rift_stats_t *out);    // 積分牌「秘境」頁讀數（側文件），沒有返回 0 | Kill board 「秘境」(Rift) page readout (sidecar file); returns 0 if there is none
