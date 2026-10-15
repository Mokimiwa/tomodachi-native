// 從 d2r-mobile 拆出來的遊戲專屬模塊（診斷拍門、TACT/Granny、UI 屏、觸屏 UI 交互等）留下的掛鈎點：通用宿主照舊調，這裡給空實現。
// 以後哪個功能要在本作裡重做，就把對應的空實現換成真的。
// Hook points left behind by the game-specific modules split out of d2r-mobile (diagnostic tick gates, TACT/Granny, UI screen, touch UI interaction, ...):
// the generic host still calls them, and they get empty implementations here. When a feature is redone for this game, replace the matching stub with the real thing.
#include "dm.h"
#include "front.h"

void crash_sample_self(void) {}
void inflate_bench_maybe(void) {}
void ticks_install(void) {}
void target_rank_install(void) {}
void dm_padmode_boot(void) {}
void dm_prof_mark(const char *seg) { (void)seg; }
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
void tact_chk_init(void) {}
void tact_prefetch_start(const char *outdir) { (void)outdir; }
void tact_prefetch_stop(void) {}
void granny_prof_dump(void) {}
void uimsg_subscribe(void (*fn)(const char *cat, const char *act, const char *arg)) { (void)fn; }
void dm_ui_text_entered(const char *text) { (void)text; }
int dm_custom_lang_explicit(void) { return 0; }
int dm_ingame_now(void) { return 0; }
void dm_uiscreen_res_reread(void) {}
// 遊戲分辨率：照前端給的（Mac 窗口 / iOS 屏幕），不做 D2R 那套界面屏改寫
// Game resolution: whatever the front end gives (Mac window / iOS screen), without D2R's UI-screen rewriting
uint64_t dm_game_res(int docked) { return front_game_res(docked); }
int dm_screen_real(int *w, int *h) { *w = *h = 0; return 0; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = *y = *w = *h = 0; }
// svc #0x80.. 宿主門：只有 D2R 的改寫規則會造，本作的產物裡不該出現
// svc #0x80.. host gates: only D2R's rewrite rules create them; they should not appear in this game's build output
uint64_t svc_host_call(int num, uint64_t *regs) { (void)regs; die("svc_host_call：本作沒有宿主門 svc #%#x", num); }
