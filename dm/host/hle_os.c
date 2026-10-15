// 宿主公共雜項：客體對象旁表、單調時鐘、modules 都沒定義的 C 庫導入。
// （原 nn::os/mem/lmem/C++ ABI 的 136 條 HLE 已刪：SDK 自帶 nn::os，裝載時這些條目一條都沒被解析用上。）
// Host common miscellany: guest object side table, monotonic clock, C library imports that none of the modules define.
// (The former 136 HLE entries for nn::os/mem/lmem/C++ ABI have been deleted: the SDK brings its own nn::os, and at load time not one of these entries was resolved and used.)
#include <pthread.h>
#include "hle.h"
#include <mach/mach_time.h>
#include <stdatomic.h>

// ---------- 旁表 ----------
// ---------- Side table ----------
#define OM_SIZE (1u << 18)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & (OM_SIZE - 1); }
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1))
        if (g_om[i].k == k || !g_om[i].k || g_om[i].k == (void *)1) { g_om[i].k = k; g_om[i].v = v; break; }
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; g_om[i].k = (void *)1; g_om[i].v = NULL; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
// 客體時鐘：客體能讀到的系統計數器（mrs cntpct 門、svc GetSystemTick）都經這裡。缺省 = now_ns()。
// 凍結（只供測試驅動 DM_TICK_HOLD 用）：凍結期間客體時鐘不走 ⇒ 遊戲不會到期下一拍；解凍後扣掉凍結時長，客體時鐘連續不跳
// DM_DETCLOCK=<步長 ns>（回放 / 測試局用；產品局不設 = 牆鐘）：首次出圖起客體時鐘改為「每次出圖推進固定步長」，不跟牆鐘——
// 客戶端每幀 dt、服務端拍的補拍快照都由出圖次數決定，同一回放兩遍局內第 N 幀逐字節可比（畫面逐字節證明的地基）。
// 首幀之前仍走牆鐘（啓動期有按時刻輪詢的等待，凍住會卡）；切換時從當時的客體時鐘值接續，不跳。凍結期間不推進。
// Guest clock: the system counters the guest can read (mrs cntpct gate, svc GetSystemTick) all go through here. Default = now_ns().
// Freeze (only for the test driver DM_TICK_HOLD): while frozen the guest clock does not advance ⇒ the game's next tick never comes due; after unfreezing the frozen duration is subtracted, the guest clock is continuous with no jump
// DM_DETCLOCK=<step ns> (for replay / test runs; product runs do not set it = wall clock): from the first present on the guest clock becomes "advance a fixed step per present", not following the wall clock --
// the client's per-frame dt and the server tick catch-up snapshots are both determined by the present count, so frame N in a run is byte-for-byte comparable between two passes of the same replay (the foundation of byte-for-byte picture proof).
// Before the first frame the wall clock is still used (the boot period has waits that poll by time, freezing would hang them); at the switch it continues from the guest clock value at that time, no jump. It does not advance while frozen.
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1; if (c < 0) { const char *e = getenv("DM_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0; c = g_gdet_step > 0; }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
// 進局對齊：前端 / 選角階段出圖幀數隨真實時間變（裝載在後台線程按牆鐘推進），首幀起累計的客體時鐘值因此每局不同——
// 依賴絕對時刻的畫面（如生命 / 法力球液面）在同拍仍不同（實測拍 E + 2 幀兩遍差約 1.8 萬像素，集中在兩側球位）。
// 見到 UI 消息 FrontEndNavigation:ToGame（建局、服務端拍時限初始化之前）即把客體時鐘設為固定值 2^52 ns（只許前跳），
// 此後純按出圖推進：拍 E 落在第幾幀、那一刻的絕對時刻都只由步長決定。
// Run entry alignment: the number of presented frames in the front end / character select phase varies with real time (loading advances on a background thread by wall clock), so the guest clock value accumulated since the first frame differs every run --
// pictures that depend on absolute time (such as the liquid level of the life / mana orbs) still differ at the same tick (measured: at tick E + 2 frames two passes differ by about 18,000 pixels, concentrated at the two orb positions).
// On seeing the UI message FrontEndNavigation:ToGame (before the run is created and the server tick deadline is initialized) the guest clock is set to the fixed value 2^52 ns (forward jumps only),
// from then on it advances purely by presents: which frame tick E lands on and the absolute time at that moment are determined by the step alone.
#define DET_TOGAME_NS (1ull << 52)
static void det_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (strcmp(cat, "FrontEndNavigation") || strcmp(act, "ToGame")) return;
    uint64_t cur = atomic_load(&g_gdet);
    if (cur >= DET_TOGAME_NS) { LOG("【紅】DM_DETCLOCK：進局時客體時鐘 %llu ≥ 對齊值 2^52，不回撥（對齊未生效）", (unsigned long long)cur); return; }
    atomic_store(&g_gdet, DET_TOGAME_NS);
    LOG("DM_DETCLOCK：進局（ToGame）客體時鐘對齊到 2^52 ns（前跳 %.3f s）", (DET_TOGAME_NS - cur) / 1e9);
}
void dm_guest_frame_tick(void) {
    crash_watch_frame();
    atomic_fetch_add(&g_guest_frame, 1);
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns()); atomic_store(&g_gdet_on, 1);
        void uimsg_subscribe(void (*)(const char *, const char *, const char *)); uimsg_subscribe(det_on_uimsg);
        LOG("DM_DETCLOCK：首次出圖起客體時鐘按出圖推進，步長 %llu ns；進局時對齊到 2^52 ns", (unsigned long long)g_gdet_step); return;
    }
    if (!atomic_load(&g_gfz)) { atomic_fetch_add(&g_gdet, g_gdet_step); dm_guest_clock_advanced(); }
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); }
}
// 凍結有幾個互不相干的原因（0 測試用的凍結拍 / 讀取畫面，1 App 在後台，2 系統鍵盤彈著）：任一成立就凍結，全部解除才放行——
// 否則「鍵盤彈著時切後台再回來」會被回前台那次解凍提前放開
// Freezing has several unrelated reasons (0 freeze tick for testing / loading screen, 1 App in background, 2 system keyboard up): freeze if any holds, release only when all are lifted --
// otherwise "switch to background and back while the keyboard is up" would be released early by the unfreeze on returning to foreground
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// 熵源：缺省真隨機（arc4random）；DM_DETCLOCK 開時改為固定種子的 splitmix64 流（回放畫面逐字節可比用，營地藍火粒子兩遍不同）。
// 兩處出口（libc getentropy、svc GetInfo RandomEntropy）共用一條流；調用次數在日誌裡報（首次與每 1000 次）
// Entropy source: true random by default (arc4random); with DM_DETCLOCK on it becomes a fixed-seed splitmix64 stream (so that replay pictures are byte-for-byte comparable, the camp blue fire particles differed between two passes).
// The two outlets (libc getentropy, svc GetInfo RandomEntropy) share one stream; the call count is reported in the log (first call and every 1000 calls)
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) return ((uint64_t)arc4random() << 32) | arc4random();
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK：確定熵第 %llu 次", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) { arc4random_buf(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
