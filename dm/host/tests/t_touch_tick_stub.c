#include <stdint.h>

uint32_t dm_cur_tick(void) { return 0; }
int dm_loadscreen_down(void) { return 1; }   // 讀取畫面判定在 diag.c；觸點單測當作已撤 | the loading screen check is in diag.c; the touch point unit test treats it as already down
// 點世界（dm_world.c）：這些單測不帶遊戲映像，給空樁
// World tapping (dm_world.c): these unit tests carry no game image, so empty stubs are given
void dm_world_pump(void) {}
int dm_world_has_target(void) { return 1; }
unsigned dm_world_tap(float nx, float ny) { (void)nx; (void)ny; return 0; }
void dm_textinput_pump(void) {}   // 文本框接法（dm_textinput.c）：同上，給空樁 | text box hookup (dm_textinput.c): same as above, an empty stub
void dm_update_pump(void) {}      // 選角頁構建號與新版本提示（dm_update.c）：同上，給空樁 | build number and new-version notice on the character select page (dm_update.c): same as above, an empty stub
void dm_num_set(int n) { (void)n; }
void dm_actions_pump(void) {}
uint64_t dm_swapskill_filter(uint64_t b) { return b; }   // 副手組技能（dm_actions.c）：不壓鍵 | swap-set skill (dm_actions.c): does not suppress keys
uint64_t dm_runeword_pad_filter(uint64_t b) { return b; }   // 手柄模式符文之語清單（dm_runeword.c）：不吃鍵 | Runeword list in controller mode (dm_runeword.c): swallow no buttons
uint64_t dm_padmode_page_filter(uint64_t b) { return b; }   // 手柄模式我們自己的頁（dm_padmode.c）：不吃鍵 | Our own pages in controller mode (dm_padmode.c): swallow no buttons
int dm_padmode_input_gate(uint64_t *b) { (void)b; return 0; }   // 手柄 / 觸屏切換事務的輸入閘（dm_padmode.c）：沒在切 | Input gate of the controller / touch switch transaction (dm_padmode.c): not switching
int dm_padmode_current(void) { return 0; }   // 手柄 / 觸屏模式（dm_padmode.c）：觸屏 | Controller / touch mode (dm_padmode.c): touch
uint64_t dm_padmode_page_keys(uint64_t m) { (void)m; return 0; } void dm_padmode_legend_hide(const char *l, int a) { (void)l; (void)a; } int dm_padmode_busy(void) { return 0; } uint64_t dm_padmode_page_held(void) { return 0; } void dm_padmode_page_pulse(uint64_t m) { (void)m; } void dm_padmode_ident_hint(int on) { (void)on; } void dm_padmode_legend(int a, const char *t) { (void)a; (void)t; } const char *dm_str(uint8_t *b, const char *k) { (void)b; return k; }   // 手柄頁鍵 / 提示條 / 取串的樁（個人倉庫手柄翻頁用，觸屏下不走） | Stubs for controller page keys / button legend / string fetch (used by controller paging of the personal stash, not reached under touch)
int dm_swapskill_wset(void) { return -1; }
int dm_pages_cur(void) { return 0; } int dm_pages_count(void) { return 5; } void dm_pages_request_flip(int d) { (void)d; }   // 個人倉庫多頁（dm_pages.c）樁 | stubs for the multi-page personal stash (dm_pages.c)
void dm_vault_ui_flip(int dir) { (void)dir; }
int dm_vault_ui_pager_hit(int x, int y) { (void)x; (void)y; return 0; }
void dm_vault_request_deposit(uint32_t id) { (void)id; }
uint32_t dm_vault_ui_held(void) { return 0; }
int dm_vault_ui_over(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_tip_gate(int x, int y) { (void)x; (void)y; return 0; }
int dm_pouch_tip_gate(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_blocks(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_tap(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_drag(int phase, int x, int y) { (void)phase; (void)x; (void)y; return 0; }
uint32_t dm_swapskill_itemsig(void) { return 0; }               // 同上：讀不到當前武器組 | same as above: the current weapon set cannot be read
void dm_shop_buy(void) {}
void dm_shop_sell_cursor(void) {}
void dm_stat_add(int s, int n) { (void)s; (void)n; }
void dm_skill_add_selected(void) {}
void dm_test_hold_first_sellable(void) {}
void dm_belt_put(unsigned id) { (void)id; }
void dm_test_sell_first(void) {}
int dm_shop_open(void) { return 0; }
_Atomic int g_dm_frz_want, g_dm_frozen; void dm_freeze_frame(int want) { (void)want; }   // 面板凍結（diag.c）的樁 | stub for the panel freeze (diag.c)
int dm_cursor_item(void *out) { (void)out; return 0; }   // 光標下的物品（dm_cursor_item.c）：沒有 | item under the cursor (dm_cursor_item.c): none
void dm_test_belt_first_potion(void) {}
void dm_shop_get(void *out) { __builtin_memset(out, 0, 320); }   // 商人讀數（dm_actions.c）：全 0（dm_shop_info 不到 320 字節） | vendor readout (dm_actions.c): all 0 (dm_shop_info is under 320 bytes)
void dm_test_use_first(int kind) { (void)kind; }
void dm_test_hover_at(float nx, float ny) { (void)nx; (void)ny; }
int dm_ui_hover_now(int x, int y) { (void)x; (void)y; return 0; }
void dm_item_compare(int on) { (void)on; }
void dm_belt_expand(int on) { (void)on; }
void dm_inv_sort(int page) { (void)page; }
int dm_diag_difficulty(void) { return -1; }
uint64_t dm_fs_commit_count(void) { return 0; }
void dm_held_equip(int bodyloc) { (void)bodyloc; }
int dm_rift_merc_unlocked(void) { return 1; }
int dm_rift_merc_slot_empty(int loc) { (void)loc; return 0; }
void dm_rift_merc_locked_hint(void) {}
void dm_name_random(void) {}
void dm_item_use(unsigned id) { (void)id; }
void dm_test_equip(int what) { (void)what; }
void dm_item_equip(unsigned id) { (void)id; }
void dm_item_unequip(unsigned id) { (void)id; }
int dm_held_size(int *w, int *h) { (void)w; (void)h; return 0; }
int dm_item_targeting(void) { return 0; }
int dm_vendor_repair_mode(void) { return 0; }
void dm_item_repair(uint32_t id) { (void)id; }
void dm_belt_fill(uint64_t w) { (void)w; }
void dm_test_identify(int what) { (void)what; }
uint64_t dm_diag_srv_item(uint32_t id) { (void)id; return 0; }
void dm_belt_use(int col, int row) { (void)col; (void)row; }
void dm_aim_cone_pump(void) {}
void dm_item_identify(unsigned id) { (void)id; }
void dm_item_target_cancel(void) {}
void dm_elysium_arm(int on) { (void)on; }   // 極樂世界勾選（dm_elysium.c）：單測裡空樁 | Elysium checkbox (dm_elysium.c): empty stub in the unit test
void dm_custom_on_uimsg(const char *cat, const char *act, const char *arg) { (void)cat; (void)act; (void)arg; }   // 「自定義」設置（dm_custom.c）的樁 | stub for the "Custom" settings (dm_custom.c)
void dm_test_stash_take(int n) { (void)n; }
void dm_stash_take(int n) { (void)n; }
float dm_ui_ny(float ny) { return ny; }
int dm_screen_real(int *w, int *h) { (void)w; (void)h; return 0; }
void dm_uiscreen_enable(int m) { (void)m; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = *y = *w = *h = 0; }
int dm_uiscreen_ingame(void) { return 0; }
void dm_uiscreen_set_ingame(int on) { (void)on; }
void dm_held_stow(void) {}
int dm_held_stow_result(void) { return -1; }
void dm_test_swap_held(void) {}
int dm_held_pending(void) { return 0; }
void uimsg_emit(const char *cat, const char *act, const char *arg) { (void)cat; (void)act; (void)arg; }
void dm_item_identify_free(unsigned id) { (void)id; }   // 免費鑒定（dm_actions.c）的樁 | stub for free identify (dm_actions.c)
int dm_runeword_long_press(const void *ci) { (void)ci; return 0; }   // 符文之語助手（dm_runeword.c）的樁 | stub for the runeword helper (dm_runeword.c)
int dm_runeword_list_open(void) { return 0; }
void dm_runeword_button(const char *name) { (void)name; }
void dm_runeword_list_close(void) {}
