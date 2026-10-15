// nn::hid（手柄）：真機經 hid 服務讀共享內存；宿主從前端寫入的當前狀態取（Mac 鍵盤 / 手柄，iOS 虛擬手柄 / 手柄）。
// 與遊戲無關的通用版：一台標準手柄。DM_OPMODE=0（掌機）→ 掌機手柄（Npad 號 0x20）；否則 → 1P 全鍵手柄（號 0）。
// 震動、六軸、鼠標、鍵盤：宿主不提供（設備數 0 / 未連接）；手柄小程序直接報「1 人、已選當前手柄」。
// nn::hid (controller): on the real device shared memory is read through the hid service; the host takes the current state written by the front end (Mac keyboard / controller, iOS virtual gamepad / controller).
// Generic, game-independent version: one standard controller. DM_OPMODE=0 (handheld) → handheld controller (Npad id 0x20); otherwise → 1P full-key controller (id 0).
// Vibration, six-axis, mouse, keyboard: not provided by the host (device count 0 / not connected); the controller applet reports "1 player, current controller selected".
#include "hle.h"
#include "dm_input.h"
#include "dm_bind.h"
#include "front.h"
#include <stdatomic.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>

enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };
enum { STYLE_FULLKEY = 1u << 0, STYLE_HANDHELD = 1u << 1, STYLE_JOYDUAL = 1u << 2, STYLE_JOYLEFT = 1u << 3, STYLE_JOYRIGHT = 1u << 4 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1, ATTR_LEFT_CONN = 1u << 2, ATTR_LEFT_WIRED = 1u << 3, ATTR_RIGHT_CONN = 1u << 4, ATTR_RIGHT_WIRED = 1u << 5 };
// 搖桿方向偽鍵（NpadButton 16..23）：由搖桿值導出，閾值約半程
// Stick direction pseudo-buttons (NpadButton 16..23): derived from the stick values, threshold about half travel
enum { STICK_THRESH = 0x4000 };

// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight 佈局相同（0x28）
// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight share one layout (0x28)
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, _pad; } NpadState;
_Static_assert(sizeof(NpadState) == 0x28, "NpadState");

// 前端發佈：鍵盤 / 觸屏一路（dm_input_set）與實體手柄一路（dm_pad_set）按位或；按下沿鎖存到遊戲下一次取，短按不漏
// Front-end publishing: the keyboard / touch path (dm_input_set) and the physical controller path (dm_pad_set) are ORed; press edges are latched until the game's next read so short presses are not lost
static _Atomic uint64_t g_btn, g_axes, g_latch, g_pad_btn, g_pad_axes, g_pad_latch;
static _Atomic int64_t g_sampling;
static uint64_t pack(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    return (uint64_t)(uint16_t)lx | (uint64_t)(uint16_t)ly << 16 | (uint64_t)(uint16_t)rx << 32 | (uint64_t)(uint16_t)ry << 48;
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_btn, buttons); atomic_fetch_or(&g_latch, buttons & ~old); atomic_store(&g_axes, pack(lx, ly, rx, ry));
}
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_pad_btn, buttons); atomic_fetch_or(&g_pad_latch, buttons & ~old); atomic_store(&g_pad_axes, pack(lx, ly, rx, ry));
}
uint64_t dm_input_samples(void) { return (uint64_t)atomic_load(&g_sampling); }
// DM_KEYS 腳本輸入（測試用）："秒:控制項[+控制項][~按住秒],…"（秒寫成 f幀號 = 等到第幾個出圖幀），例 "8:A,12:A,15:Down~0.5,16:A+B,20:StickLUp~2,23:StickRLeft=0.5~1,30:Touch=640/360"。
//   控制項是 Switch 的名字（dm_bind.h：按鍵、搖桿方向帶幅度、Touch=x/y 面板坐標），不經玩家的鍵位表。缺省按住 0.15 s。
//   按鍵與搖桿走手柄那一路（dm_pad_set），與鍵盤 / 觸屏按位或；觸屏點用腳本自己的手指槽（front.h），不抬起前端的手指。時間從宿主啓動算起
// DM_KEYS scripted input (for tests): "seconds:control[+control][~hold seconds],..." (seconds written as f<frame> = wait for that presented frame), e.g. "8:A,12:A,15:Down~0.5,16:A+B,20:StickLUp~2,23:StickRLeft=0.5~1,30:Touch=640/360".
//   Controls are Switch names (dm_bind.h: buttons, stick directions with an amount, Touch=x/y in panel coordinates) and bypass the player's key table. Held 0.15 s by default.
//   Buttons and sticks go through the controller path (dm_pad_set), ORed with keyboard / touch; touch points use the script's own finger slots (front.h) and do not lift the front end's fingers. Times are counted from host start
typedef struct { double at, hold; long frame; dm_ctl ctl; } key_step;   // frame ≥ 0：按出圖幀號等，不看秒（機器忙時遊戲進度跟著幀走） | frame >= 0: wait for that presented frame instead of seconds (on a busy machine the game's progress follows frames)
static key_step g_steps[256]; static int g_nsteps;
static void step_apply(const dm_ctl *c) {
    uint64_t b = 0; int32_t a[4] = {0}; int n = c != NULL;
    dm_ctl_combine(&c, NULL, n, &b, a); dm_pad_set(b, a[0], a[1], a[2], a[3]);
    dm_touch t = {DM_TOUCH_SCRIPT, c ? c->tx : 0, c ? c->ty : 0};
    dm_touch_set_range(&t, c && c->touch, DM_TOUCH_SCRIPT, DM_TOUCH_SCRIPT + DM_TOUCH_SCRIPT_N);
}
static void *keys_main(void *arg) {
    (void)arg; struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < g_nsteps; i++) {
        for (;;) {
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
            if (g_steps[i].frame >= 0 ? (long)dm_nvn_frame_count() >= g_steps[i].frame : el >= g_steps[i].at) break;
            usleep(10000);
        }
        const dm_ctl *c = &g_steps[i].ctl;
        LOG("DM_KEYS：%.2f s / 幀 %llu 按 %#llx 搖桿 (%.2f,%.2f)(%.2f,%.2f)%s（按住 %.2f s）", g_steps[i].frame >= 0 ? (double)g_steps[i].frame : g_steps[i].at, (unsigned long long)dm_nvn_frame_count(), (unsigned long long)c->btn,
            c->ax[0], c->ax[1], c->ax[2], c->ax[3], c->touch ? " 觸屏" : "", g_steps[i].hold);
        step_apply(c); usleep((useconds_t)(g_steps[i].hold * 1e6)); step_apply(NULL);
    }
    return NULL;
}
void hid_keys_init(void) {
    const char *e = getenv("DM_KEYS"); if (!e || !*e) return;
    static char buf[8192]; snprintf(buf, sizeof buf, "%s", e);
    if (strlen(e) >= sizeof buf) LOG("【紅】DM_KEYS 超過 %zu 字節，後面的步驟不收", sizeof buf - 1);
    char *save;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (g_nsteps == (int)(sizeof g_steps / sizeof *g_steps)) { LOG("【紅】DM_KEYS 超過 %d 步，後面的不收", g_nsteps); break; }
        char *c = strchr(tok, ':'); if (!c) { LOG("【紅】DM_KEYS 步驟缺「秒:」：%s", tok); continue; }
        *c = 0; key_step st = {atof(tok), 0.15, tok[0] == 'f' ? atol(tok + 1) : -1, {0}};
        char *h = strchr(c + 1, '~'); if (h) { *h = 0; st.hold = atof(h + 1); }
        if (dm_ctl_parse(c + 1, &st.ctl)) LOG("【紅】DM_KEYS %s 秒那步有不認識的控制項（認識的照按）", tok);
        g_steps[g_nsteps++] = st;
    }
    pthread_t th; if (g_nsteps && !pthread_create(&th, NULL, keys_main, NULL)) { pthread_detach(th); LOG("DM_KEYS：%d 步", g_nsteps); }
}

static uint32_t connected_id(void) { return dm_opmode() == 0 ? NPAD_HANDHELD : NPAD_NO1; }
uint32_t hle_hid_connected_npad(void) { return connected_id(); }   // nfp 的 NFC 設備掛在這個手柄上（hle_sdk16.c） | nfp's NFC device sits on this controller (hle_sdk16.c)
static uint32_t style_of(uint32_t id) { return id != connected_id() ? 0 : id == NPAD_HANDHELD ? STYLE_HANDHELD : STYLE_FULLKEY; }
static int32_t ax(uint64_t v, int i) { return (int16_t)(v >> (16 * i)); }
static int32_t pick(int32_t a, int32_t b) { return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b; }   // 兩路裡偏得多的那個 | whichever of the two paths is deflected more

static void fill(NpadState *s, uint32_t id, uint32_t style) {
    memset(s, 0, sizeof *s);
    s->sampling = atomic_fetch_add(&g_sampling, 1) + 1;
    if (!(style_of(id) & style)) return;   // 這個號 / 樣式沒接手柄：attributes = 0 | no controller of this id / style: attributes = 0
    uint64_t b = atomic_load(&g_btn) | atomic_exchange(&g_latch, 0) | atomic_load(&g_pad_btn) | atomic_exchange(&g_pad_latch, 0);
    uint64_t k = atomic_load(&g_axes), p = atomic_load(&g_pad_axes);
    s->lx = pick(ax(k, 0), ax(p, 0)); s->ly = pick(ax(k, 1), ax(p, 1)); s->rx = pick(ax(k, 2), ax(p, 2)); s->ry = pick(ax(k, 3), ax(p, 3));
    b &= 0xffffull;
    if (s->lx < -STICK_THRESH) b |= 1u << 16;
    if (s->ly > STICK_THRESH) b |= 1u << 17;
    if (s->lx > STICK_THRESH) b |= 1u << 18;
    if (s->ly < -STICK_THRESH) b |= 1u << 19;
    if (s->rx < -STICK_THRESH) b |= 1u << 20;
    if (s->ry > STICK_THRESH) b |= 1u << 21;
    if (s->rx > STICK_THRESH) b |= 1u << 22;
    if (s->ry < -STICK_THRESH) b |= 1u << 23;
    s->buttons = b;
    s->attributes = ATTR_CONNECTED | (id == NPAD_HANDHELD ? ATTR_WIRED | ATTR_LEFT_CONN | ATTR_LEFT_WIRED | ATTR_RIGHT_CONN | ATTR_RIGHT_WIRED : ATTR_LEFT_CONN | ATTR_RIGHT_CONN);
}
// GetNpadStates(out, n, id)：返回寫入條數，新的在前。宿主不保留歷史：只給 1 條當前狀態（調用方按返回數處理）
// GetNpadStates(out, n, id): returns the number written, newest first. The host keeps no history: only 1 current state (callers handle the returned count)
static int states(NpadState *out, int n, uint32_t id, uint32_t style) { if (n <= 0) return 0; fill(out, id, style); return 1; }

HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "連 hid 服務；宿主從前端狀態取輸入", void, h_hid_InitNpad, void) {}
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "支持的手柄號；宿主固定", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) { (void)ids; (void)n; }
static uint32_t g_styles_supported = 0xffffffffu;
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "支持的手柄樣式；宿主只記下", void, h_hid_SetStyles, uint32_t styles) { g_styles_supported = styles; }
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "手柄樣式查詢；跟 DM_OPMODE 走", uint32_t, h_hid_GetStyle, const uint32_t *id) { return style_of(*id); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", "全鍵手柄狀態", int, h_hid_StatesFullKey, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", "掌機手柄狀態", int, h_hid_StatesHandheld, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", "雙 Joy-Con 狀態（宿主不接）", int, h_hid_StatesJoyDual, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", "左 Joy-Con 狀態（宿主不接）", int, h_hid_StatesJoyLeft, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", "右 Joy-Con 狀態（宿主不接）", int, h_hid_StatesJoyRight, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYRIGHT); }
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "斷開手柄；宿主忽略", void, h_hid_Disconnect, const uint32_t *id) { (void)id; }
static uint32_t g_hold_type;
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Joy-Con 握持方向；宿主只記下", void, h_hid_HoldType, int t) { g_hold_type = (uint32_t)t; }
HLE_FORCE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", "Joy-Con 分配：雙持", int, h_hid_GetJoyAssign, const uint32_t *id) { (void)id; return 0; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", "Joy-Con 單持；宿主忽略", void, h_hid_JoySingle, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", "交換手柄號；宿主忽略", void, h_hid_Swap, const uint32_t *a, const uint32_t *b) { (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid21StartLrAssignmentModeEv", "L/R 分配模式；宿主忽略", void, h_hid_LrStart, void) {}
HLE_FORCE("_ZN2nn3hid20StopLrAssignmentModeEv", "L/R 分配模式；宿主忽略", void, h_hid_LrStop, void) {}
// 手柄小程序（系統 UI）：直接報「1 人、選中當前手柄」。ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; }
// Controller applet (system UI): report "1 player, current controller selected" directly. ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; }
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "手柄小程序；宿主直接報已選好",
          Result, h_hid_ShowCtrl, int32_t *info, const void *arg) { (void)arg; if (info) { info[0] = 1; info[1] = (int32_t)connected_id(); } return RES_OK; }
// 六軸：給出句柄，但讀不到狀態（0 條）
// Six-axis: handles are handed out, but no states can be read (0 entries)
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "六軸句柄",
          int, h_hid_SixHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) { (void)style; int k = n < 2 ? n : 2; for (int i = 0; i < k; i++) h[i] = (*id << 8) | (uint32_t)i; return k; }
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六軸；宿主不提供", void, h_hid_SixStart, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "六軸狀態；宿主不提供（0 條）",
          int, h_hid_SixStates, void *out, int n, const uint32_t *h) { (void)out; (void)n; (void)h; return 0; }
// 震動：沒有設備
// Vibration: no devices
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "震動許可；宿主不支持", unsigned, h_hid_VibPermit, void) { return 0; }
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "震動設備：0 個",
          int, h_hid_VibHandles, void *h, int n, const uint32_t *id, uint32_t style) { (void)h; (void)n; (void)id; (void)style; return 0; }
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "震動；宿主不支持", void, h_hid_InitVib, const void *h) { (void)h; }
// 震動節點（VibrationNode / VibrationPlayer，朋友收集按鍵就震）每次 Update 經 detail::FlushEnqueuedVibrationValues 批量送值，原版經 hid 服務（PermitVibration 先連 sm），宿主沒有服務就中止。
// 這裡收下報成功：隊列照常清空。以後 iOS 可在此接觸感引擎
// Vibration nodes (VibrationNode / VibrationPlayer; Tomodachi Life rumbles on button presses) send values in batches on every Update through detail::FlushEnqueuedVibrationValues; on hardware that goes through the hid service (PermitVibration connects to sm first), which aborts without the service.
// Here they are accepted and reported as sent, so the queue empties as usual. iOS haptics could hook in here later
HLE_FORCE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", "批量送震動值；宿主不支持，報已送", Result, h_hid_SendVibs, const void *h, const void *v, int n) {
    (void)h; (void)v; (void)n; return RES_OK;
}
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "震動；宿主不支持", void, h_hid_SendVib, const void *h, const void *v) { (void)h; (void)v; }
// 鼠標 / 鍵盤：未連接
// Mouse / keyboard: not connected
HLE_FORCE("_ZN2nn3hid15InitializeMouseEv", "鼠標；宿主不提供", void, h_hid_InitMouse, void) {}
HLE_FORCE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", "鼠標狀態：0 條", int, h_hid_MouseStates, void *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn3hid18InitializeKeyboardEv", "鍵盤；宿主不提供", void, h_hid_InitKbd, void) {}
HLE_FORCE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", "鍵盤狀態：未連接（KeyboardState 0x30 字節清零）", void, h_hid_KbdState, void *s) {
    memset(s, 0, 0x30); ((int64_t *)s)[0] = atomic_fetch_add(&g_sampling, 1) + 1;
}

// ---------- SDK 12 的接口（Engage）：單條 GetNpadState、模式查詢 / 設置、觸屏、手勢、六軸細項、手柄顏色等 ----------
// 原版這些都讀 hid 共享內存或走 hid 服務；宿主沒有共享內存，按上面同一份前端狀態作答。只改遊戲自己緩衝的庫函數（震動文件解析 / 生成、ControllerSupportArg 的 SetDefault / SetExplainText）留在 SDK 原樣跑
// ---------- SDK 12 interfaces (Engage): single GetNpadState, mode queries / setters, touch screen, gestures, six-axis details, controller colors, etc. ----------
// In the original all of these read hid shared memory or go through the hid service; the host has no shared memory and answers from the same front-end state as above. Library functions that only modify the game's own buffers (vibration file parse / generate, ControllerSupportArg SetDefault / SetExplainText) keep running in the SDK
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "全鍵手柄當前狀態", void, h_hid_StateFullKey, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "掌機手柄當前狀態", void, h_hid_StateHandheld, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", "雙 Joy-Con 當前狀態（宿主不接）", void, h_hid_StateJoyDual, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", "左 Joy-Con 當前狀態（宿主不接）", void, h_hid_StateJoyLeft, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", "右 Joy-Con 當前狀態（宿主不接）", void, h_hid_StateJoyRight, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYRIGHT); }

static uint32_t g_comm_mode, g_hh_mode;
HLE_FORCE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", "Joy-Con 握持方向：回報設過的值", int, h_hid_GetHold, void) { return (int)g_hold_type; }
HLE_FORCE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", "支持的手柄樣式：全部", uint32_t, h_hid_GetStyles, void) { return g_styles_supported; }
HLE_FORCE("_ZN2nn3hid24GetNpadCommunicationModeEv", "無線通信模式：回報設過的值", int, h_hid_GetComm, void) { return (int)g_comm_mode; }
HLE_FORCE("_ZN2nn3hid24SetNpadCommunicationModeENS0_21NpadCommunicationModeE", "無線通信模式；宿主只記下", void, h_hid_SetComm, int m) { g_comm_mode = (uint32_t)m; }
HLE_FORCE("_ZN2nn3hid29GetNpadHandheldActivationModeEv", "掌機激活模式：回報設過的值", int, h_hid_GetHh, void) { return (int)g_hh_mode; }
HLE_FORCE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", "掌機激活模式；宿主只記下", void, h_hid_SetHh, int m) { g_hh_mode = (uint32_t)m; }
HLE_FORCE("_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", "Joy-Con 雙持；宿主忽略", void, h_hid_JoyDual, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", "Joy-Con 單持（指定左右）；宿主忽略", void, h_hid_JoySingleT, const uint32_t *id, int t) { (void)id; (void)t; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE", "Joy-Con 單持（取回另一半）；宿主忽略，沒有另一半", int, h_hid_JoySingleOut, uint32_t *out, const uint32_t *id, int t) { (void)out; (void)id; (void)t; return 0; }
HLE_FORCE("_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", "合併單 Joy-Con；宿主忽略，報成功", Result, h_hid_Merge, const uint32_t *a, const uint32_t *b) { (void)a; (void)b; return RES_OK; }
HLE_FORCE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", "手柄樣式變化事件：綁定、永不觸發", void, h_hid_BindStyleEv,
          const uint32_t *id, void *ev, int clear) { (void)id; hle_bind_system_event(ev, clear); }
HLE_FORCE("_ZN2nn3hid19GetPlayerLedPatternERKj", "玩家燈：1 號燈", uint8_t, h_hid_Led, const uint32_t *id) { (void)id; return 1; }
// 手柄顏色 NpadControllerColor = { 主色 RGBA, 按鍵色 RGBA }：給灰底黑鍵 | NpadControllerColor = { main RGBA, button RGBA }: grey body, black buttons
static void ctrl_color(uint32_t *c) { c[0] = 0xff828282u; c[1] = 0xff0f0f0fu; }
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", "手柄顏色", Result, h_hid_Color, uint32_t *c, const uint32_t *id) { (void)id; ctrl_color(c); return RES_OK; }
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", "左右 Joy-Con 顏色", Result, h_hid_Color2, uint32_t *l, uint32_t *r, const uint32_t *id) { (void)id; ctrl_color(l); ctrl_color(r); return RES_OK; }
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", "手柄小程序（不要結果）；宿主直接報已選好", Result, h_hid_ShowCtrl0, const void *arg) { (void)arg; return RES_OK; }
HLE_FORCE("_ZN2nn3hid24ShowControllerStrapGuideEv", "腕帶說明小程序；宿主不顯示", Result, h_hid_Strap, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3hid28ShowControllerFirmwareUpdateERKNS0_27ControllerFirmwareUpdateArgE", "手柄固件更新小程序；宿主不顯示", Result, h_hid_FwUpd, const void *arg) { (void)arg; return RES_OK; }

// 觸屏：TouchScreenState<N> = { int64 採樣號; int32 點數; int32 保留; TouchState[N] }，從前端 / 映射層 / 腳本推來的觸點取（hle_touch.c）。
// 宿主不留歷史，GetTouchScreenStates 只給 1 條當前狀態
// Touch screen: TouchScreenState<N> = { int64 sampling number; int32 count; int32 reserved; TouchState[N] }, taken from the touch points pushed by the front end / mapping layer / script (hle_touch.c).
// The host keeps no history, so GetTouchScreenStates gives only 1 current state
HLE_FORCE("_ZN2nn3hid21InitializeTouchScreenEv", "連 hid 觸屏；宿主從前端觸點取", void, h_hid_InitTouch, void) {}
#define TOUCH(N) \
    HLE_FORCE("_ZN2nn3hid19GetTouchScreenStateILm" #N "EEEvPNS0_16TouchScreenStateIXT_EEE", "觸屏當前狀態", void, h_hid_Touch1_##N, void *s) { touch_fill_n(s, N, now_ns()); } \
    HLE_FORCE("_ZN2nn3hid20GetTouchScreenStatesILm" #N "EEEiPNS0_16TouchScreenStateIXT_EEEi", "觸屏狀態：1 條", int, h_hid_TouchN_##N, void *s, int n) { if (n <= 0) return 0; touch_fill_n(s, N, now_ns()); return 1; }
TOUCH(1) TOUCH(2) TOUCH(3) TOUCH(4) TOUCH(5) TOUCH(6) TOUCH(7) TOUCH(8) TOUCH(9) TOUCH(10) TOUCH(11) TOUCH(12) TOUCH(13) TOUCH(14) TOUCH(15) TOUCH(16)
// 手勢（InitializeGesture / GetGestureStates）在 hle_gesture.c：從觸點識別 | Gestures (InitializeGesture / GetGestureStates) live in hle_gesture.c: recognized from touch points

// 六軸細項：靜止、未開融合；SixAxisSensorState 0x60 字節清零（attributes = 0：未連接） | Six-axis details: at rest, fusion off; SixAxisSensorState 0x60 bytes zeroed (attributes = 0: not connected)
HLE_FORCE("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六軸；宿主不提供", void, h_hid_SixStop, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", "六軸當前狀態：未連接", void, h_hid_SixState, void *s, const uint32_t *h) { (void)h; memset(s, 0, 0x60); }
HLE_FORCE("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", "六軸靜止：是", unsigned, h_hid_SixRest, const uint32_t *h) { (void)h; return 1; }
HLE_FORCE("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", "六軸融合；宿主忽略", void, h_hid_SixFusionOn, const uint32_t *h, uint32_t on) { (void)h; (void)on; }
HLE_FORCE("_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", "六軸融合：未開", unsigned, h_hid_SixFusionQ, const uint32_t *h) { (void)h; return 0; }
HLE_FORCE("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", "陀螺零漂模式：標準", int, h_hid_GyroGet, const uint32_t *h) { (void)h; return 1; }
HLE_FORCE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", "陀螺零漂模式；宿主忽略", void, h_hid_GyroSet, const uint32_t *h, const int *m) { (void)h; (void)m; }
HLE_FORCE("_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", "六軸融合參數：0", void, h_hid_FusionGet, float *a, float *b, const uint32_t *h) { (void)h; *a = 0; *b = 0; }
HLE_FORCE("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", "六軸融合參數；宿主忽略", void, h_hid_FusionSet, const uint32_t *h, float a, float b) { (void)h; (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid34ResetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleE", "六軸融合參數；宿主忽略", void, h_hid_FusionReset, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid41IsFirmwareUpdateAvailableForSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六軸固件更新：無", unsigned, h_hid_SixFw, const uint32_t *h) { (void)h; return 0; }
// 震動設備信息 / 實際值：沒有設備，清零 | Vibration device info / actual value: no device, zeroed
HLE_FORCE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", "震動設備信息：無", void, h_hid_VibInfo, uint32_t *info, const void *h) { (void)h; info[0] = 0; info[1] = 0; }
HLE_FORCE("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", "實際震動值：0", void, h_hid_VibActual, float *v, const void *h) { (void)h; memset(v, 0, 16); }
