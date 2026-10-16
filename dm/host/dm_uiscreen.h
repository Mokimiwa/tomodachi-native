// 比 16:9 窄的屏（iPad 4:3）上讓遊戲界面按 16:9 擺、3D 仍鋪滿整屏。缺省關：不打開時不寫任何東西，下面幾個換算都是恆等。實現與依據見 dm_uiscreen.c
// On screens narrower than 16:9 (iPad 4:3), lay out the game UI at 16:9 while 3D still fills the whole screen. Off by default: when not turned on nothing is written, and the conversions below are all identity. Implementation and basis in dm_uiscreen.c
#pragma once
enum { DM_UISCREEN_OFF, DM_UISCREEN_TOP, DM_UISCREEN_CENTER, DM_UISCREEN_BOTTOM };
void dm_uiscreen_enable(int mode);           // 前端在「窗口比 16:9 窄」時調：DM_UISCREEN_BOTTOM 等 = 打開，界面矩形（寬 = 畫面寬、高 = 寬÷16×9）貼頂 / 居中 / 貼底；DM_UISCREEN_OFF = 關掉並把遊戲的值寫回。任意線程調，下一幀生效 | Called by the front end when "the window is narrower than 16:9": DM_UISCREEN_BOTTOM etc. = turn on, with the UI rectangle (width = frame width, height = width÷16×9) top-aligned / centered / bottom-aligned; DM_UISCREEN_OFF = turn off and write the game's values back. Callable from any thread, takes effect next frame
void dm_uiscreen_pump(void);                 // 遊戲主線程每幀調：發現遊戲重寫了「屏幕寬高」全局量（開機、切主機 / 掌機檔）就記下真實值並改寫成界面用的值——只此一處，不分時機 | Called every frame on the game main thread: on finding that the game rewrote the "screen width / height" globals (boot, switching docked / handheld mode), record the real values and rewrite them to the values the UI uses - this one place only, regardless of occasion
int dm_screen_real(int *w, int *h);          // 真實畫面寬高（3D 出圖、命中框用的那個）。返回 0 = 還沒讀到 | Real frame width and height (the one used by 3D rendering and hit boxes). Returns 0 = not read yet
void dm_ui_rect(int *x, int *y, int *w, int *h);   // 界面那塊矩形在真實畫面裡的位置（像素）；沒打開時 = 整個畫面。圖形層挪界面繪製、前端擺遊戲視圖都讀它，縱向位置只有這一個來源 | Position of the UI rectangle in the real frame (pixels); when off = the whole frame. The graphics layer shifting the UI draw and the front end placing the game view both read it; it is the only source for the vertical position
float dm_ui_ny(float ny_real);               // 真實畫面歸一化縱坐標 → 界面矩形裡的歸一化縱坐標（橫向不變） | Normalized y in the real frame → normalized y within the UI rectangle (horizontal unchanged)
float dm_ui_ny_back(float ny_ui);            // 反向 | Reverse
int dm_ui_proj_dy(void);   // 遊戲「世界點 → 屏幕」（main+0x1237c60 / 0x90d110）的結果被減掉的縱向像素數（界面矩形的 y；沒在減 = 0）。宿主自己調這兩個函數要真實畫面坐標時加回來 | Number of vertical pixels subtracted from the result of the game's "world point → screen" (main+0x1237c60 / 0x90d110) (the UI rectangle's y; not subtracting = 0). Add it back when the host itself calls these two functions and needs real frame coordinates
int dm_uiscreen_ingame(void);                // 1 = 局內（真全屏）；0 = 局外（前端各頁與讀取畫面：16:9 居中留邊）。唯一信號，任意線程讀 | 1 = in a run (true fullscreen); 0 = out of a run (front end pages and loading screen: 16:9 centered with margins). The single signal, readable from any thread
void dm_uiscreen_set_ingame(int on);          // 只許界面每幀那一處（dm_cursor.c）調 | May only be called from the per-frame UI site (dm_cursor.c)
void dm_uiscreen_set_panel(int on);           // 局內開著大面板（功能頁 / 箱子 / 商人 / 魔盒…，= 觸摸前端的面板模式）：界面矩形移到正中；關掉回落前端選的擺法。觸摸前端在界面消息回調裡調 | A large panel open in a run (function pages / stash / vendor / cube..., = the touch front end's panel mode): the UI rectangle moves to the center; when closed it drops back to the placement the front end chose. Called by the touch front end in the UI message callback
int dm_uiscreen_panel(void);                 // 上面那個狀態：局內開著大面板 = 1 | The state above: a large panel open in a run = 1
#include <stdint.h>
#include <string.h>
uint64_t dm_game_res(int docked);            // 遊戲出圖分辨率參數位此刻該給的值（{u32 寬, u32 高} 打包）：前端的原生值，真全屏打開且不在局裡時改成屏寬的 16:9 | The value the game's render resolution parameter slot should give right now (packed {u32 width, u32 height}): the front end's native value, changed to 16:9 of the screen width when true fullscreen is on and not in a run
// KRESF 參數位此刻該給的值：主機模式輸出寬高（dm_game_res(1)）的 float 位模式，低 32 位寬、高 32 位高（改寫器的跳板直接當浮點用，見 dm_aspect.h）
// The value the KRESF parameter slot should give right now: the docked output width and height (dm_game_res(1)) as float bit patterns, width in the low 32 bits and height in the high 32 bits (the rewriter's trampolines use them directly as floats, see dm_aspect.h)
static inline uint64_t dm_game_resf(void) {
    uint64_t r = dm_game_res(1); float w = (float)(uint32_t)r, h = (float)(uint32_t)(r >> 32); uint32_t a, b;
    memcpy(&a, &w, 4); memcpy(&b, &h, 4); return (uint64_t)b << 32 | a;
}
void dm_uiscreen_res_reread(void);            // 只許 hle_oe.c 在遊戲取走「性能模式變化」通知時調 | May only be called by hle_oe.c when the game takes the "performance mode changed" notification
