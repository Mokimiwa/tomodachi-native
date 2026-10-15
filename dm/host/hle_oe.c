// nn::oe（應用運行環境）：真機上經 am/oe 服務查詢主機狀態；宿主直接給固定狀態。
// oe::Initialize 在 hle_sdkinit.c，不在這裡。
// 環境開關：DM_OPMODE=0 掌機 / 1 主機（默認 1，對應主機模式的渲染檔）；DM_LANG=語言碼（默認 zh-Hans）。
// nn::oe (application operating environment): on the real device the console state is queried through the am/oe services; the host gives fixed state directly.
// oe::Initialize is in hle_sdkinit.c, not here.
// Environment switches: DM_OPMODE=0 handheld / 1 console (default 1, corresponding to the console-mode render profile); DM_LANG=language code (default zh-Hans).
#include "hle.h"
#include "dm_uiscreen.h"

// 運行模式是運行時狀態（前端可改：iOS 過熱降級切掌機）：唯一來源在此，啓動時從 DM_OPMODE 取一次，之後不再讀環境變量
// The operation mode is runtime state (the front end can change it: iOS overheat downgrade switches to handheld): the single source is here, taken once from DM_OPMODE at startup, the environment variable is not read again afterwards
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) { const char *e = getenv("DM_OPMODE"); int v = e ? atoi(e) : 1; int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode; }
    return m;
}
// ---------- 系統通知隊列（AppletMessage）：前端狀態變化 → 遊戲每幀 TryPopNotificationMessage 取 ----------
// 消息號（遊戲 main 0xe10d30 實際處理的）：15 焦點變化、30 運行模式變化、31 性能模式變化
// ---------- System notification queue (AppletMessage): front end state changes → fetched by the game every frame with TryPopNotificationMessage ----------
// Message numbers (those actually handled by the game at main 0xe10d30): 15 focus change, 30 operation mode change, 31 performance mode change
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_mq[MQ]; static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode;          // 遊戲用 Set*NotificationEnabled 打開後才推 | only pushed after the game turns it on with Set*NotificationEnabled
static void mq_push(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15/30/31 是「查詢當前狀態」的失效通知，不是攜帶歷史值的事件。
    // 同類型尚未取走時保留首次排位：D2R 消費循環只讀最新狀態，重復通知的寫回相同，尺寸相同不再 resize。
    // 只合併這三種狀態通知；未來的逐次事件（如 Resume/Exit）不能套這個規則。
    // 合法生產者只發這三種，因此待取最多三項，任意多次失焦/恢復不再撞 32 槽上限；彈出後新變化照常入隊。
    // 15/30/31 are invalidation notifications meaning "query the current state", not events carrying historical values.
    // While one of the same type has not been taken yet, the first queue position is kept: the D2R consumer loop only reads the latest state, repeated notifications write back the same thing, and with the same size there is no further resize.
    // Only these three state notifications are merged; future per-occurrence events (such as Resume/Exit) must not use this rule.
    // Legitimate producers only send these three, so at most three items are pending, and any number of focus losses/restores no longer hits the 32-slot limit; after a pop, new changes are enqueued as usual.
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) { pthread_mutex_unlock(&g_mq_lock); return; }
    if (g_mqn == MQ) die("oe 通知隊列溢出（遊戲沒在取？）");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_mutex_unlock(&g_mq_lock);
}
void dm_oe_res_changed(void) { if (getenv("DM_TEST_OE_FOCUS_FIRST")) mq_push(MSG_FOCUS);   /* 測試：讓 31 排在一條焦點通知後面（iOS 包啓動時的順序） | Test: queue 31 behind a focus notification (the order at iOS app startup) */
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE); else LOG("【紅】分辨率要切，但遊戲沒打開性能模式變化通知"); }   // 出圖分辨率該變了（dm_uiscreen.c）：讓遊戲重讀參數位 | the render resolution should change (dm_uiscreen.c): make the game re-read the parameter bits
// 只對 GetOperationMode 報的運行模式（手柄 / 觸屏佈局切換讓遊戲重載佈局用，dm_padmode.c）：m ≥ 0 報 m 並推一次運行模式通知，m < 0 報回真值再推一次。
// 手柄樣式（hle_hid.c connected_id）與性能模式照真值，所以不彈「重新連接控制器」、出圖分辨率不變
// Operation mode reported to GetOperationMode only (used to make the game reload layouts when switching between controller / touch layouts, dm_padmode.c):
// m ≥ 0 reports m and pushes one operation-mode notification; m < 0 reports the true value again and pushes one more.
// The controller style (hle_hid.c connected_id) and performance mode keep their true values, so no "Reconnect controller" prompt appears and the render
// resolution does not change
static _Atomic int g_ui_opmode = -1;
static _Atomic int g_ui_armed; static _Atomic uint32_t g_ui_reads;   // 推過通知後遊戲第一次來讀運行模式記一次：切換事務拿它回讀「遊戲收到了」 | Counts once when the game first comes to read the operation mode after a notification has been pushed: the switch transaction uses it to read back "the game received it"
uint32_t dm_oe_ui_opmode_reads(void) { return atomic_load(&g_ui_reads); }
void dm_oe_ui_opmode(int m) { g_ui_opmode = m < 0 ? -1 : m; atomic_store(&g_ui_armed, 1); if (g_ntf_opmode) mq_push(MSG_OPMODE); }
void dm_set_opmode(int m) {   // 前端調（過熱降級等）：改狀態並按遊戲打開的開關推送 30 / 31 | called by the front end (overheat downgrade etc.): change the state and push 30 / 31 according to the switches the game has turned on
    int old = dm_opmode(); if (old == m) return;
    g_opmode = m;
    LOG("oe 運行模式 %d → %d", old, m);
    if (g_ntf_opmode) mq_push(MSG_OPMODE);
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE);
}
// FocusState：1 InFocus、2 OutOfFocus、3 Background。前端調 dm_focus_set（iOS 生命週期 / Mac 窗口失焦）
// FocusState: 1 InFocus, 2 OutOfFocus, 3 Background. The front end calls dm_focus_set (iOS lifecycle / Mac window losing focus)
static _Atomic int g_focus = 1; static _Atomic uint64_t g_focus_in_ns;   // 最近一次進入 InFocus 的時刻 | time of the most recent entry into InFocus
int dm_focus_now(void) { return atomic_load(&g_focus); }
uint64_t dm_focus_in_since(void) { return atomic_load(&g_focus_in_ns); }   // 圖形層顯示節拍看門狗：只在激活時算停頓，從激活起算（nvn_mtl.m mtl_present_wait） | graphics layer display link watchdog: stalls are only computed while active, counted from activation (nvn_mtl.m mtl_present_wait)
void dm_focus_set(int st) {
    if (st < 1 || st > 3) die("dm_focus_set 非法狀態 %d", st);
    if (st == 1) atomic_store(&g_focus_in_ns, now_ns());
    if (atomic_exchange(&g_focus, st) == st) return;
    LOG("oe 焦點狀態 → %d", st);
    mq_push(MSG_FOCUS);
}
static int opmode(void) { return dm_opmode(); }

// 截圖 / 錄像開關：宿主無系統截圖服務，空操作
// Screenshot / video recording switches: the host has no system screenshot service, no-op
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "錄像開關；宿主無系統錄像", void, h_oe_EnableRec, void) {}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "錄像開關；宿主無系統錄像", void, h_oe_DisableRec, void) {}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "截圖開關；宿主無系統截圖", void, h_oe_EnableShot, void) {}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "截圖開關；宿主無系統截圖", void, h_oe_DisableShot, void) {}

// OperationMode：0 Handheld、1 Console；PerformanceMode：0 Normal、1 Boost（與運行模式一一對應）
// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost (one-to-one with the operation mode)
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "運行模式查詢；按 DM_OPMODE 給固定值", int, h_oe_GetOpMode, void) { int u = g_ui_opmode; if (atomic_exchange(&g_ui_armed, 0)) atomic_fetch_add(&g_ui_reads, 1); return u >= 0 ? u : opmode(); }
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "性能模式查詢；與運行模式一致", int, h_oe_GetPerfMode, void) { return opmode(); }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "設 CPU/GPU 頻率檔；宿主不調頻", void, h_oe_SetPerfCfg, int mode, int cfg) {
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x（忽略）", mode, cfg);
}

// 腳本化狀態變化（無窗口驗證生命週期 / 過熱降級）：DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30"（第 N 秒）；bg=1 / bg=0 走整套切後台 / 回前台（見 script_bg）
// 時刻從遊戲第一次取通知算起；「@ingame+N」= 讀取畫面第 K 次撤下（K = 前面 @ingame 項數 + 1，首個即進局）後 N 秒（按事件，不受負載影響）。與前端調用並存。
// Scripted state changes (verify lifecycle / overheat downgrade without a window): DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30" (second N); bg=1 / bg=0 run the whole background round trip (see script_bg)
// Time counts from the game's first notification fetch; "@ingame+N" = N seconds after the loading screen is taken down for the K-th time (K = number of preceding @ingame items + 1, the first one is run entry) (event-based, not affected by load). Coexists with front end calls.
#include <unistd.h>
static _Atomic int g_ls_unloads;   // 讀取畫面撤下次數（diag.c 的 UIMSG 處理調 dm_note_loadscreen_unload） | number of times the loading screen was taken down (the UIMSG handler in diag.c calls dm_note_loadscreen_unload)
void dm_note_loadscreen_unload(void) { atomic_fetch_add(&g_ls_unloads, 1); }
// bg=1 / bg=0：整套「切後台 / 回前台」，與 front_ios.m 場景生命週期同序（失活 → 進後台；回前台 → 激活），只少了 iOS 回前台時重設音頻會話。
// Mac 上復現「切後台再回來之後」的問題用（音頻接手與恢復、客體時鐘凍結、圖形掛起）；要連進程掛起一起模仿，在兩項之間對 dmhost 發 SIGSTOP / SIGCONT
// bg=1 / bg=0: the whole "to background / back to foreground" sequence, in the same order as the scene lifecycle in front_ios.m (resign active -> enter background; enter foreground -> become active), minus the audio session reset iOS does on returning.
// Used to reproduce "after going to the background and back" problems on the Mac (audio take-over and resume, guest clock freeze, graphics suspend); to imitate the process being suspended as well, send dmhost SIGSTOP / SIGCONT between the two items
static void script_bg(int on) {
    if (on) { dm_focus_set(2); dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); }
    else { dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); dm_focus_set(1); }
}
static void *oe_script(void *arg) {
    char *buf = arg; uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16]; int v; double at;
        if (sscanf(tok, "%15[a-z]=%d@ingame+%lf", key, &v, &at) == 3) {
            static int need;
            need++; while (atomic_load(&g_ls_unloads) < need) usleep(20000);
            usleep((useconds_t)(at * 1e6));
        } else {
            if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("DM_OE_SCRIPT 項「%s」格式應為 鍵=值@秒 或 鍵=值@ingame+秒", tok);
            uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
            if (due > now) usleep((useconds_t)((due - now) / 1000));
        }
        if (!strcmp(key, "focus")) dm_focus_set(v);
        else if (!strcmp(key, "opmode")) dm_set_opmode(v);
        else if (!strcmp(key, "bg")) script_bg(v);
        else die("DM_OE_SCRIPT 未知鍵「%s」（focus / opmode / bg）", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("DM_OE_SCRIPT"); if (!e) return;
    pthread_t th; if (pthread_create(&th, NULL, oe_script, strdup(e))) die("DM_OE_SCRIPT 線程");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "前後台狀態；由前端 dm_focus_set 驅動", int, h_oe_GetFocus, void) { return g_focus; }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "前後台切換策略；遊戲設 1（收焦點通知），宿主按通知語義實現", void, h_oe_SetFocusMode, int m) { LOG("oe::SetFocusHandlingMode %d", m); }

// 通知：返回 bool（w0），宿主沒有系統通知
// ---------- 3D 內部渲染比例 ----------
// 遊戲自帶一個靜態的 3D 渲染比例：渲染器對象（全局指針 [main+0x31e0848]）成員 +0x66a4（float，原版恆 1.0）。
// 改分辨率函數 main+0x8582f0(輸出寬, 輸出高) → main+0x1627ef0 按「比例 × 輸出寬高」重建 3D 目標（+0xee78 / +0xee7c），
// 界面層與 Final Result 仍按輸出尺寸（+0xee88 / +0xee8c）建，末端由遊戲自己的放大著色器 ps_upsample_p 放回輸出尺寸。
// 啓動流程不調 0x8582f0，所以要主動調一次；調用點必須是遊戲主線程取系統通知處——遊戲自己處理模式切換的同一上下文
// （在出圖 HLE 裡調會自鎖：渲染鎖已被出圖路徑持有）。
// 缺省：iOS 0.75，Mac 宿主 1.0（畫面閘門對 Switch 真值比）；DM_3D_SCALE=0.5–1.0 覆蓋。
// 視覺尺子真值階段（DM_NVN_RULER=1 的 A / B）強制 1.0，切到被測模式後才生效。
// 動態比例（系統降頻看的是佔用率，溫控檔位只是粗信號 → 按實際掉幀調）：控制器吃出圖線程報來的實際上屏間隔
// （與 perf.txt 同源），三級 配置值(0.75) → 0.72 → 0.70；下限 0.70（實看 0.65 有差別）。規則見 scale3d_ctl_step。
// 開關：DM_3D_SCALE_DYN=0 關；DM_THERMAL_NOFALLBACK=1（功耗 / 幀時間測量）也關——測量時比例必須釘住。
// DM_3D_SCALE_DYN_INJECT=幀起-幀止：診斷，這段幀裡把餵給控制器的間隔一律當 25 ms（Mac 上沒有上屏時刻，用它驗降級 / 回升的整條鏈）。
// Notification: returns bool (w0), the host has no system notifications
// ---------- 3D internal render scale ----------
// The game has a static 3D render scale built in: renderer object (global pointer [main+0x31e0848]) member +0x66a4 (float, always 1.0 in the original game).
// The resolution change function main+0x8582f0(output width, output height) → main+0x1627ef0 rebuilds the 3D target as "scale × output width/height" (+0xee78 / +0xee7c),
// the UI layer and Final Result are still built at the output size (+0xee88 / +0xee8c), and at the end the game's own upscale shader ps_upsample_p brings it back to the output size.
// The boot flow does not call 0x8582f0, so it has to be called once actively; the call site must be where the game main thread fetches system notifications -- the same context in which the game itself handles mode switches
// (calling it in the present HLE self-deadlocks: the render lock is already held by the present path).
// Default: iOS 0.75, Mac host 1.0 (the screenshot gate compares against Switch ground truth); DM_3D_SCALE=0.5–1.0 overrides.
// The visual ruler ground truth phase (A / B of DM_NVN_RULER=1) forces 1.0, it only takes effect after switching to the mode under test.
// Dynamic scale (system throttling looks at utilization, the thermal state is only a coarse signal → adjust by actual dropped frames): the controller consumes the actual present intervals reported by the present thread
// (same source as perf.txt), three levels configured value (0.75) → 0.72 → 0.70; lower limit 0.70 (0.65 is visibly different). For the rules see scale3d_ctl_step.
// Switches: DM_3D_SCALE_DYN=0 off; DM_THERMAL_NOFALLBACK=1 (power / frame time measurement) also turns it off -- the scale must be pinned during measurement.
// DM_3D_SCALE_DYN_INJECT=start frame-end frame: diagnostic, within these frames every interval fed to the controller is taken as 25 ms (the Mac has no present timestamps, this is used to verify the whole lower / raise chain).
#include <TargetConditionals.h>
#define SCALE3D_MIN 0.70f
#define SCALE3D_MID 0.72f
// 純函數控制器（單測 t_oe.c 直接餵序列）：level 0 = 配置值，越大越低。安全閥：只在可感的掉幀時動。
//   窗：每累計 2 秒的上屏間隔一個窗，窗內幀率 = 上屏幀數 ÷ 間隔之和（被系統丟掉的幀不計數，那段時間落在下一張上屏幀的間隔裡）。
//   降：連續兩個窗的幀率都 < 目標 × 0.92（60 幀時 55.2）→ 降一級。
//   升：連續 10 個窗（20 秒）的幀率都 ≥ 目標 × 0.97（60 幀時 58.2）→ 升一級；升上去 10 秒內又被打回，下一次升要連續 30 個窗（60 秒）。
//   換級後停判 2 秒（重建 3D 目標本身會頓一下）。目標幀率 = 1000 ÷ period_ms（0 = 60 幀）。
// 舊規則（2 秒窗慢幀 > 5% 即降、升要連續 60 秒零慢幀、打回 ×3）降得太容易、幾乎升不回：測試 iPhone 進局 582 局降過 33 局、0 局升回，
// 兩份實機日誌在溫控 2、幀率 59.8–60 下也被降到 0.70；慢幀數只留在診斷日誌裡，不再參與決策。
// Pure-function controller (unit test t_oe.c feeds sequences directly): level 0 = configured value, larger is lower. Safety valve: only acts on perceptible dropped frames.
//   Window: one window per 2 seconds of accumulated present intervals, frame rate in the window = presented frames ÷ sum of intervals (frames dropped by the system are not counted, that time falls into the interval of the next presented frame).
//   Lower: frame rate of two consecutive windows both < target × 0.92 (55.2 at 60 fps) → lower one level.
//   Raise: frame rate of 10 consecutive windows (20 seconds) all ≥ target × 0.97 (58.2 at 60 fps) → raise one level; if knocked back within 10 seconds of going up, the next raise needs 30 consecutive windows (60 seconds).
//   Judging is suspended for 2 seconds after a level change (rebuilding the 3D target itself causes a hitch). Target frame rate = 1000 ÷ period_ms (0 = 60 fps).
// The old rule (lower as soon as slow frames > 5% in a 2-second window, raise needs 60 consecutive seconds with zero slow frames, knock-back ×3) lowered too easily and almost never came back up: on the test iPhone, of 582 run entries 33 were lowered and 0 came back up,
// and two real-device logs were also lowered to 0.70 at thermal state 2 with frame rate 59.8–60; the slow frame count is kept only in the diagnostic log and no longer takes part in the decision.
typedef struct { int level, max_level; double period_ms; uint64_t hold_until, last_up; double win_ms, last_fps; unsigned win_n; int bad_run, good_run, need_good; } scale3d_ctl;
#define S3_SEC 1000000000ull
static void scale3d_ctl_hold(scale3d_ctl *c, uint64_t now, uint64_t ns) {   // 停判一段時間（進局 / 回前台 / 換級之後）：窗與連續計數清零 | suspend judging for a while (after entering a run / returning to foreground / changing level): the window and the consecutive counts are cleared
    if (now + ns > c->hold_until) c->hold_until = now + ns;
    c->win_ms = 0; c->win_n = 0; c->bad_run = c->good_run = 0;
}
static int scale3d_ctl_step(scale3d_ctl *c, double iv_ms, uint64_t now) {   // 返回當前級 | returns the current level
    if (now < c->hold_until || iv_ms <= 0) return c->level;   // 停判中 / 無讀數（0）/ 被丟棄（−1） | judging suspended / no reading (0) / dropped (−1)
    c->win_ms += iv_ms; c->win_n++;
    if (c->win_ms < 2000) return c->level;
    double tgt = 1000.0 / (c->period_ms > 0 ? c->period_ms : 1000.0 / 60), fps = c->win_n * 1000.0 / c->win_ms;
    c->last_fps = fps; c->win_ms = 0; c->win_n = 0;
    if (fps < tgt * 0.92) {
        c->good_run = 0;
        if (++c->bad_run >= 2 && c->level < c->max_level) {
            if (c->last_up && now - c->last_up < 10 * S3_SEC) c->need_good = 30;   // 剛升上去 10 秒內又掉：下次升要 60 秒 | dropped again within 10 seconds of going up: the next raise needs 60 seconds
            c->level++; scale3d_ctl_hold(c, now, 2 * S3_SEC);
        }
    } else {
        c->bad_run = 0;
        if (fps >= tgt * 0.97) { if (++c->good_run >= (c->need_good ? c->need_good : 10) && c->level > 0) { c->level--; c->last_up = now; c->need_good = 0; scale3d_ctl_hold(c, now, 2 * S3_SEC); } }
        else c->good_run = 0;
    }
    return c->level;
}
static _Atomic int g_scale3d_level;
static _Atomic int g_scale3d_resumed;
void dm_3d_scale_resumed(void) { atomic_store(&g_scale3d_resumed, 1); }   // 前端：剛回前台。回來頭幾秒的慢幀是恢復（表面重建、緩存變冷），不是 GPU 吃緊：停判 3 秒 | front end: just returned to foreground. Slow frames in the first few seconds back are recovery (surface rebuild, caches gone cold), not GPU pressure: suspend judging for 3 seconds
void dm_3d_scale_feed(double iv_ms) {   // 出圖線程每幀調（diag.c dm_perf_present 轉來）：實際上屏間隔 ms，−1 = 被丟棄，0 = 無讀數 | called by the present thread every frame (forwarded from diag.c dm_perf_present): actual present interval in ms, −1 = dropped, 0 = no reading
    static scale3d_ctl c = {.max_level = 2}; static int on = -1; static long inj0 = -1, inj1 = -1;
    if (on < 0) { const char *e = getenv("DM_3D_SCALE_DYN"), *j = getenv("DM_3D_SCALE_DYN_INJECT"); on = !(e && !strcmp(e, "0")) && !getenv("DM_THERMAL_NOFALLBACK");
        if (j) { inj0 = atol(j); inj1 = strchr(j, '-') ? atol(strchr(j, '-') + 1) : inj0; } }
    if (!on) return;
    if (inj0 >= 0) { long f = (long)dm_nvn_frame_count(); iv_ms = f >= inj0 && f <= inj1 ? 25.0 : 16.7; }
    uint64_t now = now_ns();
    if (atomic_exchange(&g_scale3d_resumed, 0)) scale3d_ctl_hold(&c, now, 3 * S3_SEC);
    { static int was; int in = dm_ingame_now();   // 不在局內（菜單 / 讀取）不判；剛進局的 20 秒也不判（進局頭十幾秒的慢幀是裝載，不是 GPU 吃緊） | not judged outside a run (menu / loading); not judged in the first 20 seconds of a run either (slow frames in the first ten-odd seconds of a run are loading, not GPU pressure)
      if (!in) { was = 0; scale3d_ctl_hold(&c, now, S3_SEC); return; }
      if (!was) { was = 1; if (inj0 < 0) scale3d_ctl_hold(&c, now, 20 * S3_SEC); } }
    { int mtl_fps_target(void); double p = mtl_fps_target() == 30 ? 1000.0 / 30 : 0; if (p != c.period_ms) { c.period_ms = p; scale3d_ctl_hold(&c, now, 2 * S3_SEC); } }   // 省電模式（30 幀）：目標週期跟著換，切換後停判 2 秒 | power-saving mode (30 fps): the target period changes with it, judging is suspended for 2 seconds after the switch
    int was = c.level, lv = scale3d_ctl_step(&c, iv_ms, now);
    if (lv != was) LOG("3D 比例控制：窗口幀率 %.1f（目標 %.0f）→ %s到第 %d 級%s", c.last_fps, c.period_ms > 0 ? 1000.0 / c.period_ms : 60.0, lv > was ? "降" : "升", lv, c.need_good ? "（剛升又掉：下次升檔要 60 秒）" : "");
    atomic_store(&g_scale3d_level, lv);
}
void dm_3d_scale_hot(int hot) { (void)hot; }   // 過熱不再直接降檔（改按實際掉幀）；保留符號給前端 | overheating no longer lowers the level directly (now by actual dropped frames); the symbol is kept for the front end
static _Atomic float g_scale3d_cur = 1.0f;
float dm_3d_scale_cur(void) { return atomic_load(&g_scale3d_cur); }
static void scale3d_tick(void) {
    static float want_cfg = -1, cur = 1.0f; static module_t *main;
    if (want_cfg < 0) {
        const char *e = getenv("DM_3D_SCALE"); want_cfg = TARGET_OS_IPHONE ? 0.75f : 1.0f;
        if (e) { char *end; float v = strtof(e, &end); if (end == e || *end || !(v >= 0.5f && v <= 1.0f)) die("DM_3D_SCALE 非法：%s（要求 0.5–1.0）", e); want_cfg = v; }
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) main = &g_mod[i];
        // 上面的地址（渲染器全局指針、改分辨率函數、成員偏移）都是 D2R 構建 db0507f3… 的；別的構建（馬力歐各版）同一地址是不相干的數據與代碼
        // （馬力歐 v0 的 main+0x8582f0 在一個靜態初始化函數中間），照寫照調會改壞內存。只在這個構建上開
        // The addresses above (renderer global pointer, resolution change function, member offsets) all belong to D2R build db0507f3…; in other builds (every Mario version) the same addresses are unrelated data and code
        // (Mario v0's main+0x8582f0 is in the middle of a static initializer), so writing and calling them would corrupt memory. Only enabled on that build
        if (main && strcmp(main->build_id, "db0507f36b84200ac05f75d76ed5026f")) { LOG("3D 渲染比例：main 構建 %s 沒有對應地址，不調（只認 D2R db0507f3…）", main->build_id[0] ? main->build_id : "（產物無構建號）"); main = NULL; }
    }
    int lv = atomic_load(&g_scale3d_level);   // 動態級：0 = 配置值，1 = 0.72，2 = 0.70（都不高於配置值） | dynamic level: 0 = configured value, 1 = 0.72, 2 = 0.70 (none higher than the configured value)
    float dyn = lv == 0 ? want_cfg : lv == 1 ? SCALE3D_MID : SCALE3D_MIN; if (dyn > want_cfg) dyn = want_cfg;
    float want = dm_nvn_ruler_truth() ? 1.0f : dyn;
    if (want == cur || !main || dm_nvn_frame_count() < 30) return;
    uintptr_t renderer = *(uintptr_t *)(main->base + 0x31e0848); if (!renderer) return;
    uint32_t ow = *(uint32_t *)(renderer + 0xee88), oh = *(uint32_t *)(renderer + 0xee8c); if (!ow || !oh) return;
    { extern void mtl_rt_force_full(int) __attribute__((weak)); if (mtl_rt_force_full) mtl_rt_force_full(4); }   // 運行時外推：換比例前後幾幀一律全畫（主通道換尺寸那幀不能外推） | runtime extrapolation: the few frames before and after a scale change are all drawn in full (the frame where the main pass changes size cannot be extrapolated)
    float *scale = (float *)(renderer + 0x66a4); dm_guest_write_prepare(scale, sizeof *scale); *scale = want;
    ((void (*)(uint32_t, uint32_t))(main->base + 0x8582f0))(ow, oh);
    cur = want; atomic_store(&g_scale3d_cur, want);
    LOG("3D 渲染比例 %.2f：3D 目標 %ux%u，輸出 %ux%u（幀 %llu）", want, *(uint32_t *)(renderer + 0xee78), *(uint32_t *)(renderer + 0xee7c),
        *(uint32_t *)(renderer + 0xee88), *(uint32_t *)(renderer + 0xee8c), (unsigned long long)dm_nvn_frame_count());
}
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "系統通知隊列；前端狀態變化入隊", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    scale3d_tick();
    pthread_once(&g_oe_once, oe_script_start);
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) { *msg = g_mq[g_mqh]; g_mqh = (g_mqh + 1) % MQ; g_mqn--; }
    pthread_mutex_unlock(&g_mq_lock);
    // 分辨率參數位在線程槽裡，遊戲在取通知的線程上讀它們來改出圖尺寸（收到 31「性能模式變化」時，main+0xe10ddc 起；30 只更新一個標誌）。
    // 讀的時機：處理函數每次進來、第一條通知取到之後讀一次（0xe10d5c / 0xe10d60），後面循環裡再取到的通知用的還是這次讀的值。所以每取走一條都把此刻該給的值寫進當前線程的槽——
    // 只在取走 31 時寫不夠：31 排在別的通知（如焦點 15）後面時，參數位在取 15 之後就讀過了（iOS 包：前端沒切成 16:9，Mac 宿主那局 31 正好排第一）。
    // The resolution parameter bits live in the thread slot, and the game reads them on the thread that fetches notifications to change the render size (on receiving 31 "performance mode changed", from main+0xe10ddc; 30 only updates a flag).
    // When they are read: each time the handler is entered, once after the first notification is fetched (0xe10d5c / 0xe10d60), and notifications fetched later in the loop still use the values from that read. So every time one is taken, the value due at this moment is written into the current thread's slot --
    // writing only when 31 is taken is not enough: when 31 is queued behind another notification (e.g. focus 15), the parameter bits have already been read after fetching 15 (iOS app: the front end did not switch to 16:9, in the Mac host run 31 happened to be queued first).
    if (ok) for (int i = 0; i < 2; i++) if (g_keyres[i]) pthread_setspecific(g_keyres[i], (void *)dm_game_res(i));
    if (ok && *msg == MSG_PERFMODE) dm_uiscreen_res_reread();
    if (ok) LOG("oe 通知被遊戲取走：%u", *msg);   // 狀態變化很少，逐條打不刷屏 | state changes are rare, printing each one does not flood the log
    return ok;
}
// bool 參數：Switch 調用方不擴展，聲明成 unsigned 只看低 8 位
// bool arguments: Switch callers do not extend them, declared as unsigned and only the low 8 bits are looked at
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "恢復通知開關；宿主的恢復以焦點通知 15 表達", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "運行模式變化通知開關", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "性能模式變化通知開關", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// nn::settings::LanguageCode = struct { char string[8]; }，8 字節按值返回在 x0
// nn::settings::LanguageCode = struct { char string[8]; }, 8 bytes returned by value in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "系統語言查詢；按 DM_LANG 給值", uint64_t, h_oe_GetLang, void) {
    const char *e = getenv("DM_LANG");
    char code[8] = {0};
    strncpy(code, e ? e : "zh-Hans", sizeof code - 1);
    uint64_t v; memcpy(&v, code, 8);
    return v;
}

// nn::util::Uuid 16 字節：本機固定值（只用於本地區分設備，聯網已剪）
// nn::util::Uuid 16 bytes: a fixed local value (only used to tell devices apart locally, networking is cut)
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "偽設備號；宿主固定值", void, h_oe_GetDevId, uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
}
