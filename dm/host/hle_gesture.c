// nn::hid 手勢（GetGestureStates）：真機由 hid 系統服務從觸屏數據識別點按、長按、拖動、快滑、雙指縮放 / 旋轉，遊戲每幀讀最近幾條。
// 朋友收集的界面靠它：只給觸點、不給手勢時，點按鈕沒反應（實測島名頁點 OK 不前進）。宿主照同樣的規則自己識別：
//   · 觸點從無到有：Touch（屬性 IsNewTouch），檢測號 +1；
//   · 一指移動超過 PAN_PX：Pan（帶位移、速度），之後每次移動一條；
//   · 一指不動按住 PRESS_NS：Press；
//   · 抬起：Touch 階段（沒動、沒到長按）→ Tap（與上次點按夠近夠快再加 IsDoubleTap）；Pan 且抬手時速度夠快 → Swipe（帶方向）；其餘 → Complete；
//   · 兩指以上：Pinch（scale = 指距 / 起始指距，rotation_angle = 轉角，度）。
// 觸點由 hle_touch.c 在每次推送後送來（gesture_feed，前端 / 映射層 / 腳本的手指一起算）；時間類判定（長按）在遊戲讀手勢時補做。
// GestureState（0x60 字節，= libnx HidGestureState）：+0 採樣號、+8 檢測號、+0x10 類型、+0x14 方向、+0x18 x、+0x1c y、+0x20 dx、+0x24 dy、
//   +0x28 vx、+0x2c vy（float）、+0x30 屬性（bit4 IsNewTouch、bit8 IsDoubleTap）、+0x34 scale、+0x38 轉角、+0x3c 點數、+0x40 起 4 個點（x, y）。
// nn::hid gestures (GetGestureStates): on hardware the hid system service recognizes taps, presses, pans, swipes and two-finger pinch / rotate from the touchscreen data, and the game reads the latest few each frame.
// Tomodachi Life's UI depends on it: with touch points but no gestures, tapping a button does nothing (measured: tapping OK on the island name page did not advance). The host recognizes them itself by the same rules:
//   · touch count goes from none to some: Touch (attribute IsNewTouch), detection number +1;
//   · one finger moves more than PAN_PX: Pan (with delta and velocity), then one entry per move;
//   · one finger held still for PRESS_NS: Press;
//   · release: still in the Touch phase (no movement, no long press) → Tap (plus IsDoubleTap when close enough in time and place to the previous tap); Pan released fast enough → Swipe (with a direction); otherwise → Complete;
//   · two or more fingers: Pinch (scale = finger distance / starting distance, rotation_angle = rotation in degrees).
// Touch points come from hle_touch.c after every push (gesture_feed; fingers of the front end / mapping layer / script all count); time-based decisions (long press) are completed when the game reads gestures.
// GestureState (0x60 bytes, = libnx HidGestureState): +0 sampling number, +8 detection number, +0x10 type, +0x14 direction, +0x18 x, +0x1c y, +0x20 dx, +0x24 dy,
//   +0x28 vx, +0x2c vy (float), +0x30 attributes (bit4 IsNewTouch, bit8 IsDoubleTap), +0x34 scale, +0x38 rotation, +0x3c point count, 4 points (x, y) from +0x40.
#include "hle.h"
#include "front.h"
#include <math.h>
#include <pthread.h>

enum { G_IDLE, G_COMPLETE, G_CANCEL, G_TOUCH, G_PRESS, G_TAP, G_PAN, G_SWIPE, G_PINCH, G_ROTATE };
enum { D_NONE, D_LEFT, D_UP, D_RIGHT, D_DOWN };
enum { A_NEW = 1u << 4, A_DOUBLE = 1u << 8 };
enum { RING = 16, PAN_PX = 10, TAP_PX = 40 };
static const uint64_t PRESS_NS = 500000000ull, DOUBLE_NS = 350000000ull;
static const float SWIPE_VEL = 600.f;   // 抬手時速度（面板像素 / 秒）超過它算快滑 | release velocity (panel px / s) above which it is a swipe

typedef struct {
    int64_t sampling, context; uint32_t type, dir; int32_t x, y, dx, dy; float vx, vy; uint32_t attr; float scale, rot; int32_t npt; int32_t pt[4][2];
} gstate;
_Static_assert(sizeof(gstate) == 0x60, "GestureState");

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static gstate g_ring[RING]; static int g_head, g_count; static int64_t g_samp, g_ctx;
static struct {
    int active, npt, phase;               // phase：當前手勢類型（G_TOUCH / G_PRESS / G_PAN / G_PINCH） | phase: the current gesture type
    int32_t pt[4][2], start[2], last[2];  // 當前各點、起點、上一條的位置 | current points, start, position of the previous entry
    uint64_t t0, tlast; float vx, vy;     // 按下時刻、上次移動時刻、速度 | touch-down time, time of the last move, velocity
    float d0, a0;                         // 雙指起始距離、角度 | two-finger starting distance and angle
    uint64_t tap_t; int32_t tap[2];       // 上次點按的時刻與位置（判雙擊） | time and place of the previous tap (double-tap check)
} G;

static void emit(uint32_t type, uint32_t dir, uint32_t attr, float scale, float rot) {
    gstate *s = &g_ring[g_head]; memset(s, 0, sizeof *s);
    s->sampling = ++g_samp; s->context = g_ctx; s->type = type; s->dir = dir; s->attr = attr;
    s->x = G.pt[0][0]; s->y = G.pt[0][1]; s->dx = G.pt[0][0] - G.last[0]; s->dy = G.pt[0][1] - G.last[1];
    s->vx = G.vx; s->vy = G.vy; s->scale = scale; s->rot = rot; s->npt = G.npt > 4 ? 4 : G.npt;
    memcpy(s->pt, G.pt, sizeof s->pt);
    G.last[0] = G.pt[0][0]; G.last[1] = G.pt[0][1];
    g_head = (g_head + 1) % RING; if (g_count < RING) g_count++;
    static int lg = -1, nl; if (lg < 0) lg = getenv("DM_TOUCH_LOG") != NULL;
    if (lg && nl < 300) { nl++; LOG("手勢：類型 %u 方向 %u 位置 (%d,%d) 點數 %d 屬性 %#x 縮放 %.2f", type, dir, s->x, s->y, s->npt, attr, scale); }
}
static float dist2(const int32_t (*p)[2]) { float dx = (float)(p[1][0] - p[0][0]), dy = (float)(p[1][1] - p[0][1]); return sqrtf(dx * dx + dy * dy); }
static float angle2(const int32_t (*p)[2]) { return atan2f((float)(p[1][1] - p[0][1]), (float)(p[1][0] - p[0][0])) * 57.29578f; }

// 長按的時間判定：沒有新觸點事件也要能出 Press | the time-based long-press decision: Press must come out even without new touch events
static void tick_locked(uint64_t now) {
    if (G.active && G.npt == 1 && G.phase == G_TOUCH && now - G.t0 >= PRESS_NS) { G.phase = G_PRESS; emit(G_PRESS, D_NONE, 0, 1, 0); }
}
void gesture_feed(const int32_t (*pt)[2], int n, uint64_t now) {
    pthread_mutex_lock(&g_mu);
    tick_locked(now);
    if (n > 0 && !G.active) {   // 新的一次觸摸 | a new touch
        G.active = 1; g_ctx++; G.npt = n > 4 ? 4 : n; memcpy(G.pt, pt, sizeof(int32_t[2]) * (size_t)G.npt);
        G.start[0] = G.last[0] = pt[0][0]; G.start[1] = G.last[1] = pt[0][1]; G.t0 = G.tlast = now; G.vx = G.vy = 0; G.phase = G_TOUCH;
        if (G.npt >= 2) { G.d0 = dist2(G.pt); G.a0 = angle2(G.pt); }
        emit(G_TOUCH, D_NONE, A_NEW, 1, 0);
    } else if (n > 0) {         // 觸摸進行中：移動 / 指數變化 | touch in progress: movement / finger count change
        int was = G.npt; G.npt = n > 4 ? 4 : n; memcpy(G.pt, pt, sizeof(int32_t[2]) * (size_t)G.npt);
        if (G.npt >= 2) {
            if (was < 2 || G.d0 <= 0) { G.d0 = dist2(G.pt); G.a0 = angle2(G.pt); }
            float sc = G.d0 > 0 ? dist2(G.pt) / G.d0 : 1, rot = angle2(G.pt) - G.a0;
            G.phase = G_PINCH; emit(G_PINCH, D_NONE, 0, sc, rot);
        } else {
            int32_t mx = G.pt[0][0] - G.start[0], my = G.pt[0][1] - G.start[1];
            if (G.phase != G_PAN && G.phase != G_PINCH && mx * mx + my * my >= PAN_PX * PAN_PX) G.phase = G_PAN;
            if (G.phase == G_PAN && (G.pt[0][0] != G.last[0] || G.pt[0][1] != G.last[1])) {
                float dt = (float)(now - G.tlast) / 1e9f; if (dt < 1e-3f) dt = 1e-3f;
                G.vx = (float)(G.pt[0][0] - G.last[0]) / dt; G.vy = (float)(G.pt[0][1] - G.last[1]) / dt; G.tlast = now;
                emit(G_PAN, D_NONE, 0, 1, 0);
            }
        }
    } else if (G.active) {      // 全部抬起：收尾 | all lifted: finish
        if (G.phase == G_TOUCH) {
            int dbl = G.tap_t && now - G.tap_t <= DOUBLE_NS && abs(G.pt[0][0] - G.tap[0]) <= TAP_PX && abs(G.pt[0][1] - G.tap[1]) <= TAP_PX;
            emit(G_TAP, D_NONE, dbl ? A_DOUBLE : 0, 1, 0);
            G.tap_t = dbl ? 0 : now; G.tap[0] = G.pt[0][0]; G.tap[1] = G.pt[0][1];
        } else if (G.phase == G_PAN && now - G.tlast < 100000000ull && (fabsf(G.vx) >= SWIPE_VEL || fabsf(G.vy) >= SWIPE_VEL)) {
            uint32_t d = fabsf(G.vx) >= fabsf(G.vy) ? (G.vx < 0 ? D_LEFT : D_RIGHT) : (G.vy < 0 ? D_UP : D_DOWN);
            emit(G_SWIPE, d, 0, 1, 0);
        } else emit(G_COMPLETE, D_NONE, 0, 1, 0);
        G.active = 0; G.npt = 0; G.phase = G_IDLE;
    }
    pthread_mutex_unlock(&g_mu);
}

HLE_FORCE("_ZN2nn3hid17InitializeGestureEv", "手勢：宿主從觸點自己識別", void, h_hid_InitGesture, void) {}
// GetGestureStates(out, n)：新的在前，最多 n 條 | GetGestureStates(out, n): newest first, at most n entries
HLE_FORCE("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", "手勢狀態：宿主識別的最近幾條（新的在前）", int, h_hid_Gestures, void *out, int n) {
    pthread_mutex_lock(&g_mu);
    tick_locked(now_ns());
    int k = n < g_count ? n : g_count;
    for (int i = 0; i < k; i++) memcpy((gstate *)out + i, &g_ring[(g_head - 1 - i + RING) % RING], sizeof(gstate));
    pthread_mutex_unlock(&g_mu);
    return k < 0 ? 0 : k;
}
