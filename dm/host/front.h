// 宿主前端（Mac：front_mac.m；iOS：dm/ios 另實現同名接口）。
// Host front end (Mac: front_mac.m; iOS: dm/ios separately implements the same-named interface).
#pragma once
// 跑客體：有窗口時客體在獨立線程、主線程歸系統 UI；無窗口時直接在當前線程跑。
// Run the guest: with a window the guest runs on its own thread and the main thread belongs to the system UI; without a window it runs directly on the current thread.
void front_run(void (*guest)(void));
// 圖形後端出圖的 CAMetalLayer（無窗口時為 NULL）
// The CAMetalLayer the graphics backend presents frames to (NULL when there is no window)
void *front_metal_layer(void);
// 遊戲輸出分辨率（改寫器 PARAMS 分辨率參數位）：docked = 1 主機模式 / 0 掌機模式，返回 (u64)高 << 32 | 寬。
// 原版常數 1600x900 / 1280x720（16:9）；屏幕比例不同的前端按同高度放寬，遊戲原生按新寬度渲染並重排界面（寬 ≤ 4096）。
// Game output resolution (the rewriter's PARAMS resolution parameter slot): docked = 1 docked mode / 0 handheld mode, returns (u64)height << 32 | width.
// The original game's constants are 1600x900 / 1280x720 (16:9); front ends with a different screen aspect ratio widen at the same height, and the game natively renders at the new width and re-lays out the UI (width ≤ 4096).
#include <stdint.h>
uint64_t front_game_res(int docked);
// 文本輸入框（軟鍵盤 swkbd 用）：主線程彈原生輸入框，調用線程阻塞到用戶確定 / 取消。
// 文本均為 UTF-8；max_chars 為最大字數（0 = 不限，僅供前端提示；截斷由調用方 hle_swkbd.c 按 UTF-16 單元統一做）。
// 確定返回 1 並把結果寫進 out（含結尾 0），取消返回 0；
// 無窗口（front_metal_layer() == NULL）時不彈，返回 -1。
// Text input box (for the software keyboard swkbd): the main thread shows a native input box and the calling thread blocks until the user confirms / cancels.
// All text is UTF-8; max_chars is the maximum character count (0 = unlimited, only a hint for the front end; truncation is done uniformly by the caller hle_swkbd.c in UTF-16 units).
// Confirm returns 1 and writes the result into out (including the terminating 0); cancel returns 0;
// with no window (front_metal_layer() == NULL) nothing is shown and -1 is returned.
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);

// 宿主裝載提前起跑（main.c）：讀 NSO / 重定位 / 強制接管 / 預取起跑放到後台線程，與前端（UIKit 啓動鏈、Metal 視圖）並行；
// 隨後調用的 dm_main / main 會等它完成。調用時機與前提見 main.c 注釋（先設好 DM_ROMFS 等環境變量與 cwd，參數與隨後 main 的逐項相同）。
// Early start of host loading (main.c): reading the NSO / relocation / forced takeover / prefetch start are moved to a background thread, in parallel with the front end (UIKit launch chain, Metal view);
// the dm_main / main called afterwards waits for it to finish. For call timing and preconditions see the comments in main.c (set DM_ROMFS and the other environment variables and the cwd first; arguments identical item by item to those of the later main).
void dm_host_preload_start(int argc, char **argv);

// 觸屏（宿主 hle_touch.c 實現；iOS / Mac 前端調）：觸點集合一有變化（按下 / 移動 / 抬起 / 取消）就整組推一次，n = 0 = 全抬起。
// id = 手指槽號 0–15（按下取最小空槽、抬起前不變，直接當 finger_id）；x / y = 出圖畫面上的歸一化坐標 [0,1)，左上為原點。
// 宿主換算到遊戲讀的面板坐標 1280x720（x·1280、y·720，main 0x1b65260 的讀法），並鎖存短按（兩次讀之間按下又抬起至少報一次）。可在任意線程調。
// Touchscreen (implemented by the host's hle_touch.c; called by the iOS / Mac front end): whenever the set of touch points changes (down / move / up / cancel) the whole set is pushed once; n = 0 = all lifted.
// id = finger slot number 0–15 (the lowest free slot on down, unchanged until up; used directly as finger_id); x / y = normalized coordinates [0,1) on the output frame, origin at top left.
// The host converts to the panel coordinates the game reads, 1280x720 (x·1280, y·720, the way main 0x1b65260 reads them), and latches short taps (a down and up between two reads is reported at least once). May be called from any thread.
typedef struct { uint32_t id; float x, y; } dm_touch;
void dm_touch_set(const dm_touch *t, int n);
// 手指槽分三段，各推各的、互不抬起對方：前端的真手指 0–11（dm_touch_set）、映射層的 Touch 目標 12–13（dm_bind.c）、DM_KEYS 腳本 14–15（hle_hid.c）。
// dm_touch_set_range 只管 [lo, hi) 裡的槽：這段裡沒出現的手指算抬起，段外的不動
// Finger slots come in three ranges, each pushed separately without lifting the others: the front end's real fingers 0-11 (dm_touch_set), the mapping layer's Touch targets 12-13 (dm_bind.c), the DM_KEYS script 14-15 (hle_hid.c).
// dm_touch_set_range only manages the slots in [lo, hi): fingers in that range that are absent count as lifted, slots outside it are left alone
enum { DM_TOUCH_FRONT_N = 12, DM_TOUCH_BIND = 12, DM_TOUCH_BIND_N = 2, DM_TOUCH_SCRIPT = 14, DM_TOUCH_SCRIPT_N = 2 };
void dm_touch_set_range(const dm_touch *t, int n, uint32_t lo, uint32_t hi);
// 填一份 TouchScreenState<cap>（0x10 + 0x28·cap 字節），nn::hid 的觸屏讀法調（hle_hid.c）；返回寫入的觸點數
// Fill one TouchScreenState<cap> (0x10 + 0x28·cap bytes), called by the nn::hid touchscreen reads (hle_hid.c); returns the number of touch points written
int touch_fill_n(void *out, int cap, uint64_t now_ns);

// 存檔未保存（宿主 hle_fs.c 調；iOS 前端寫同名強實現彈原生提示「存儲空間不足，進度未保存」，缺省弱實現只記日誌）。
// 時機：遊戲一次存檔事務（兩次 Commit 之間）裡有存檔寫失敗（盤滿等），這次 Commit 被拒、已提交的舊檔不變時，每個被拒的事務調一次；
// 下一個事務成功就照常提交，前端不必做恢復動作。err = 首個失敗的 errno（ENOSPC = 28 即空間不足）。在遊戲線程調，前端自己切主線程、不要阻塞
// Save not written (called by the host's hle_fs.c; the iOS front end provides a strong implementation of the same name that shows the native prompt 「存儲空間不足，進度未保存」(Not enough storage. Progress was not saved); the default weak implementation only logs).
// Timing: when a save write fails (disk full etc.) within one of the game's save transactions (between two Commits), so that this Commit is refused and the previously committed save is unchanged, it is called once per refused transaction;
// the next transaction that succeeds commits as usual, and the front end need not do any recovery. err = the first failing errno (ENOSPC = 28 means out of space). Called on the game thread; the front end switches to the main thread itself and must not block
void front_save_failed(int err);
