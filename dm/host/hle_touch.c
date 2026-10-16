// 觸屏：前端整組推觸點（dm_touch_set，front.h），遊戲每幀經 nn::hid::GetTouchScreenStates 讀（hle_hid.c → touch_fill_n）。
// 遊戲的讀法（main 0x1b65260）：清 16 份狀態、要 16 份，只看第 0 份——觸點數與各觸點的 finger_id / x / y；面板坐標系 1280x720
// （它在同一函數裡寫常量 0x2d0_00000500 = 720 << 32 | 1280）。所以：
//   · 前端歸一化坐標 [0,1)（出圖畫面左上為原點）→ x = nx·1280、y = ny·720，夾到 [0,1279] / [0,719]；
//   · 短按鎖存：遊戲只看最新一份，兩次讀之間按下又抬起的手指若不鎖存會整個丟掉——本表每根手指先以 Start 報一次，
//     抬起後再以 End 報一次才釋放（兩次讀之間按下又抬起 = 這次報 Start、下次報 End）。
// TouchState（0x28 字節，= libnx HidTouchState）：+0 delta_time（ns，i64）、+8 屬性（bit0 Start / bit1 End）、+0xc finger_id、
// +0x10 x、+0x14 y、+0x18 直徑 x、+0x1c 直徑 y、+0x20 轉角、+0x24 保留。TouchScreenState<16>：+0 採樣號、+8 觸點數、+0x10 起 16 個 TouchState。
// Touchscreen: the front end pushes the whole set of touch points (dm_touch_set, front.h), and the game reads them every frame through nn::hid::GetTouchScreenStates (hle_hid.c → touch_fill_n).
// How the game reads (main 0x1b65260): it clears 16 states, asks for 16, and looks only at state 0 -- the touch count and each touch point's finger_id / x / y; the panel coordinate system is 1280x720
// (it writes the constant 0x2d0_00000500 = 720 << 32 | 1280 in the same function). Therefore:
//   · front-end normalized coordinates [0,1) (origin at the top left of the output picture) → x = nx·1280, y = ny·720, clamped to [0,1279] / [0,719];
//   · short-press latch: the game only looks at the newest state, so a finger pressed and lifted between two reads would be lost entirely without a latch -- in this table every finger is first reported once as Start,
//     and is released only after being reported once more as End after lifting (pressed and lifted between two reads = Start in this report, End in the next).
// TouchState (0x28 bytes, = libnx HidTouchState): +0 delta_time (ns, i64), +8 attributes (bit0 Start / bit1 End), +0xc finger_id,
// +0x10 x, +0x14 y, +0x18 diameter x, +0x1c diameter y, +0x20 rotation angle, +0x24 reserved. TouchScreenState<16>: +0 sample number, +8 touch count, 16 TouchState from +0x10.
// finger_id 照真機觸控面板給：手指第一次報 Start 時拿「當下沒人佔的最小號」（第一根 = 0），報完 End 才收回。宿主的槽號（前端 0–11、映射 12–13、腳本 14–15，front.h）只是內部分段，不能直接當 finger_id：
//   朋友收集只認 0 號手指——觸屏元件（main+0x44ba40）按 1 << finger_id 建按住 / 按下遮罩，UI 手柄只看第 0 位：main+0x3f7600 見 0 號按下才切到觸屏指針模式，
//   main+0x1d32cb0 再拿 0 號的位置當指針。以前腳本的手指報成 14，遊戲收到了卻永遠不切模式，點按鈕沒反應。
// finger_id follows the real touch panel: a finger gets "the lowest id nobody holds right now" when its Start is first reported (the first finger = 0), and gives it back once its End is reported. The host's slot numbers (front end 0-11, mapping 12-13, script 14-15, front.h) are only internal ranges and cannot be reported as finger_id directly:
//   Tomodachi Life only honours finger 0 -- its touch element (main+0x44ba40) builds the held / pressed masks as 1 << finger_id, and the UI controller looks only at bit 0: main+0x3f7600 switches to touch-pointer mode only when finger 0 is pressed,
//   and main+0x1d32cb0 then takes finger 0's position as the pointer. The script's finger used to be reported as 14, so the game received it but never switched modes and tapping a button did nothing.
#include "dm.h"
#include "front.h"
#include <pthread.h>
enum { F_EMPTY, F_DOWN, F_LIFTED };
static struct { int st, reported, again, hw; float x, y, ax, ay; } g_f[16];   // again/ax/ay：End 還沒報出去時同 id 又按下（下次讀先報 End 再報新 Start）；hw：報出去的 finger_id（reported 時有效） | again/ax/ay: the same id was pressed again before End went out (the next read reports End first, then the new Start); hw: the finger_id reported (valid while reported)
static pthread_mutex_t g_tl = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_tsamp; static uint64_t g_tlast_ns;

static int32_t clampi(float v, int hi);
void gesture_feed(const int32_t (*pt)[2], int n, uint64_t now);
void dm_touch_set(const dm_touch *t, int n) { dm_touch_set_range(t, n, 0, DM_TOUCH_FRONT_N); }
// 只動 [lo, hi) 裡的槽（前端 / 映射層 / 腳本各一段，front.h）：段外的手指既不按下也不抬起
// Only touches slots in [lo, hi) (one range each for the front end / mapping layer / script, front.h): fingers outside the range are neither pressed nor lifted
void dm_touch_set_range(const dm_touch *t, int n, uint32_t lo, uint32_t hi) {
    pthread_mutex_lock(&g_tl);
    int seen[16] = {0};
    if (hi > 16) hi = 16;
    for (int i = 0; i < n; i++) {
        uint32_t id = t[i].id; if (id < lo || id >= hi) continue;
        seen[id] = 1;
        if (g_f[id].st == F_LIFTED && g_f[id].reported) { g_f[id].again = 1; g_f[id].ax = t[i].x; g_f[id].ay = t[i].y; continue; }
        if (g_f[id].st == F_EMPTY) { g_f[id].st = F_DOWN; g_f[id].reported = 0; }
        else if (g_f[id].st == F_LIFTED) g_f[id].st = F_DOWN;              // 抬起還沒報過又按下：當作一直按著 | Pressed again before the lift was reported: treat it as held down all along
        g_f[id].x = t[i].x; g_f[id].y = t[i].y;
    }
    for (uint32_t id = lo; id < hi; id++) {
        if (!seen[id] && g_f[id].st == F_DOWN) g_f[id].st = F_LIFTED;
        if (!seen[id] && g_f[id].again) g_f[id].again = 0;                   // 重按後又抬起（End 都還沒報）：重按丟掉，仍按原抬起報 End | Pressed again and then lifted again (End not even reported yet): drop the re-press, still report End for the original lift
    }
    int32_t pts[16][2]; int np = 0;   // 現在按著的手指（所有段）→ 手勢識別（hle_gesture.c） | fingers down now (all ranges) → gesture recognition (hle_gesture.c)
    for (int id = 0; id < 16; id++) if (g_f[id].st == F_DOWN) { pts[np][0] = clampi(g_f[id].x, 1279); pts[np][1] = clampi(g_f[id].y, 719); np++; }
    pthread_mutex_unlock(&g_tl);
    gesture_feed((const int32_t (*)[2])pts, np, now_ns());
}

static int32_t clampi(float v, int hi) { int32_t r = (int32_t)(v * (float)(hi + 1)); return r < 0 ? 0 : r > hi ? hi : r; }
// 分配 finger_id：已報過、還沒報完 End 的手指佔著號；busy 是這一份裡已寫出的號（剛報完 End 的號這一份不重用，免得同一份裡出現兩個同號）
// Allocate a finger_id: fingers already reported and not yet finished with End hold their ids; busy holds the ids already written in this state (an id whose End was just reported is not reused in the same state, so one state never carries the same id twice)
static int hw_alloc(uint32_t busy) {
    for (int i = 0; i < 16; i++) if (g_f[i].st != F_EMPTY && g_f[i].reported) busy |= 1u << g_f[i].hw;
    int h = 0; while (h < 15 && (busy >> h & 1)) h++;
    return h;
}
// 填一份 TouchScreenState<16>（out 按 0x290 字節）；now_ns 用於 delta_time。返回寫入的觸點數
// Fill one TouchScreenState<16> (out is 0x290 bytes); now_ns is used for delta_time. Returns the number of touch points written
int touch_fill(void *out, uint64_t now_ns) { return touch_fill_n(out, 16, now_ns); }
// 同上，TouchScreenState<cap>（0x10 + 0x28·cap 字節）：遊戲模板參數 N 不一定是 16；超出 cap 的手指留到下一次讀再報
// Same as above for TouchScreenState<cap> (0x10 + 0x28·cap bytes): the game's template parameter N is not always 16; fingers beyond cap wait for the next read
int touch_fill_n(void *out, int cap, uint64_t now_ns) {
    if (cap < 0) cap = 0;
    if (cap > 16) cap = 16;
    uint8_t *o = out; memset(o, 0, 0x10 + 0x28 * (size_t)cap);
    pthread_mutex_lock(&g_tl);
    int64_t samp = ++g_tsamp; int64_t dt = g_tlast_ns ? (int64_t)(now_ns - g_tlast_ns) : 0; g_tlast_ns = now_ns;
    int k = 0; uint32_t busy = 0;
    for (int id = 0; id < 16 && k < cap; id++) {
        if (g_f[id].st == F_EMPTY) continue;
        uint32_t attr = 0;
        if (!g_f[id].reported) { attr |= 1; g_f[id].hw = hw_alloc(busy); g_f[id].reported = 1; }   // Start（本次第一次報），分配 finger_id | Start (reported for the first time this time), allocate a finger_id
        else if (g_f[id].st == F_LIFTED) attr |= 2;                         // End（已報過 Start、已抬起） | End (Start already reported, already lifted)
        uint8_t *s = o + 0x10 + 0x28 * k++;
        int32_t x = clampi(g_f[id].x, 1279), y = clampi(g_f[id].y, 719), d = 30, zero = 0, fid = g_f[id].hw;
        busy |= 1u << fid;
        memcpy(s + 0x00, &dt, 8); memcpy(s + 0x08, &attr, 4); memcpy(s + 0x0c, &fid, 4);
        memcpy(s + 0x10, &x, 4); memcpy(s + 0x14, &y, 4); memcpy(s + 0x18, &d, 4); memcpy(s + 0x1c, &d, 4); memcpy(s + 0x20, &zero, 4);
        if (attr & 2) {                                                      // End 報過：釋放；若期間同 id 又按下，轉成新的按下 | End has been reported: release; if the same id was pressed again in the meantime, turn it into a new press
            if (g_f[id].again) { g_f[id].st = F_DOWN; g_f[id].reported = 0; g_f[id].again = 0; g_f[id].x = g_f[id].ax; g_f[id].y = g_f[id].ay; }
            else g_f[id].st = F_EMPTY;
        }
    }
    pthread_mutex_unlock(&g_tl);
    int32_t cnt = k; memcpy(o + 0, &samp, 8); memcpy(o + 8, &cnt, 4);
    // DM_TOUCH_LOG=1（驗收診斷）：非 0 觸點的讀逐次記一行——採樣號、觸點數、各點 finger_id / 面板 x,y / 屬性（1 Start、2 End）；封頂 500 行
    // DM_TOUCH_LOG=1 (acceptance diagnostics): each read with non-zero touch points logs one line -- sample number, touch count, and for each point finger_id / panel x,y / attributes (1 Start, 2 End); capped at 500 lines
    static int lg = -1, nl; if (lg < 0) lg = getenv("DM_TOUCH_LOG") != NULL;
    if (lg && k && nl < 500) {
        char b[256]; int m = snprintf(b, sizeof b, "觸屏讀 採樣 %lld 觸點 %d", (long long)samp, k);
        for (int i = 0; i < k && m < (int)sizeof b - 40; i++) { int32_t v[4]; memcpy(v, o + 0x10 + 0x28 * i + 0x08, 16);
            m += snprintf(b + m, sizeof b - m, " | #%d (%d,%d) 屬性 %d", v[1], v[2], v[3], v[0]); }
        LOG("%s", b); if (++nl == 500) LOG("DM_TOUCH_LOG 滿 500 行，停記");
    }
    return k;
}
