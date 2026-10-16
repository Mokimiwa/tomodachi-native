// 客體線程環境：門槽 = 宿主門入口；x18 槽清零；tpidr_el0 槽 = ELF TLS 塊；tpidrro_el0 槽 = Horizon ThreadLocalRegion（0x200）。
// SDK 的內部臨界區從 TLR[0x1f8] 取當前線程對象指針，這裡給每線程分配一個零初始化對象並寫入。
// Guest thread environment: gate slot = host gate entry; x18 slot cleared; tpidr_el0 slot = ELF TLS block; tpidrro_el0 slot = Horizon ThreadLocalRegion (0x200).
// The SDK's internal critical section takes the current thread object pointer from TLR[0x1f8]; here each thread gets a zero-initialized object allocated and written in.
#include "dm.h"
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
extern void dm_gate(void);            // entry.S：門公共入口 | entry.S: common gate entry
static pthread_key_t g_keyown;   // 本線程申請到的 TLS 塊 / 線程本地區的原始指針（見 guest_thread_init 末尾） | raw pointers of the TLS block / thread-local region this thread allocated (see the end of guest_thread_init)
static pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;
static void own_free(void *p) { void **own = p; free(own[0]); free(own[1]); free(own); }
static void own_key_init(void) { if (pthread_key_create(&g_keyown, own_free)) die("創建線程本地塊的釋放鍵失敗"); }
#include "front.h"
#include "dm_uiscreen.h"
void guest_thread_init(void) {
    pthread_setspecific(g_keygate, (void *)dm_gate);
    for (int i = 0; i < 2; i++) if (g_keyres[i]) pthread_setspecific(g_keyres[i], (void *)dm_game_res(i));   // 分辨率參數位：{u32 寬, u32 高} 打包 | resolution parameter slot: {u32 width, u32 height} packed
    if (g_keyresf) pthread_setspecific(g_keyresf, (void *)dm_game_resf());   // 同一尺寸的浮點版：{f32 寬, f32 高} | the same size as floats: {f32 width, f32 height}
    if (g_keyaspect || g_keyaspectui) {   // 寬高比上限參數位：max(19/9, 兩種模式輸出寬 / 高)，float 位模式放低 32 位 | aspect ratio limit parameter slot: max(19/9, output width / height of the two modes), float bit pattern in the low 32 bits
        float a = 19.0f / 9.0f; int wide = 0;
        for (int i = 0; i < 2; i++) { uint64_t r = front_game_res(i); float w = (float)(uint32_t)r, h = (float)(uint32_t)(r >> 32); if (h > 0 && w / h > a) { a = w / h; wide = 1; } }
        // 界面邊柱著色器的上限：屏幕寬於 19:9 時另加 0.15，把邊柱向內的柔邊推出屏幕；16:9 不變。
        // 推導：邊柱內緣 x = 屏高 × 上限 / 2，上限每加 Δ，內緣每側外移 屏高 × Δ / 2。Mac 1952x900 實測柔邊寬度 ≤ 30 像素
        // （上限 = 屏幕比例時右緣第 0–20 列由黑漸亮，上限推到 2.4 即消失）⇒ 需 Δ ≥ 2 × 30 / 900 ≈ 0.067；取 0.15（每側外移約 67 像素）留出一倍以上餘量。
        // 不是魔數：屏高變大時餘量按比例不變（Δ 是相對屏高的量）
        // Limit of the UI side-pillar shader: when the screen is wider than 19:9 add another 0.15, pushing the pillars' inward soft edge off the screen; 16:9 is unchanged.
        // Derivation: pillar inner edge x = screen height * limit / 2, so for every delta added to the limit the inner edge moves outward by screen height * delta / 2 on each side. Measured on Mac 1952x900, the soft edge is <= 30 pixels wide
        // (with limit = screen aspect ratio, columns 0–20 at the right edge fade from black to bright, and it disappears once the limit is pushed to 2.4) => need delta >= 2 * 30 / 900 ~ 0.067; 0.15 is taken (about 67 pixels outward per side), leaving more than double the margin.
        // Not a magic number: when the screen height grows the margin stays proportional (delta is relative to screen height)
        float ui = wide ? a + 0.15f : a;
        uint32_t bits;
        if (g_keyaspect) { memcpy(&bits, &a, 4); pthread_setspecific(g_keyaspect, (void *)(uintptr_t)bits); }
        if (g_keyaspectui) { memcpy(&bits, &ui, 4); pthread_setspecific(g_keyaspectui, (void *)(uintptr_t)bits); }
    }
    pthread_setspecific(g_key18, 0);
    // TLS 塊與線程本地區隨線程退出釋放（取自 d2r-mobile 2eb575877）。客體會改寫 TLS / 線程本地區這兩把鍵的槽（它自己設線程指針），所以不能按鍵裡的當前值去 free：
    // 申請到的原始指針另記在本文件自己的鍵 g_keyown 上，由它的析構放（鍵析構在 pthread_exit 時調用，同 crash.c 的信號備用棧；析構裡只有 free，客體線程的小棧上也安全）。
    // 原先不放：遊戲起一條短命線程就漏 8192 + 0x200 + 0x400 = 9728 字節。線程本地區與當前線程對象並成一塊（TLR[0x1f8] 那一格 SDK 也可能改寫）
    // The TLS block and the thread-local region are freed when the thread exits (from d2r-mobile 2eb575877). The guest overwrites the slots of the TLS / thread-local-region keys (it sets its own thread pointer), so they cannot be freed by the current value in the key:
    // the raw pointers that were allocated are recorded separately on this file's own key g_keyown and freed by its destructor (key destructors run at pthread_exit, like crash.c's signal alternate stack; the destructor only calls free, which is safe on a guest thread's small stack too).
    // They used not to be freed: every short-lived thread the game started leaked 8192 + 0x200 + 0x400 = 9728 bytes. The thread-local region and the current thread object are one block (the SDK may also overwrite the TLR[0x1f8] slot)
    pthread_once(&g_keyown_once, own_key_init);
    void **own = calloc(2, sizeof *own); uint8_t *tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
    if (!own || !tlr) die("客體線程本地塊分配失敗");
    own[0] = calloc(1, TLS_SIZE); own[1] = tlr;
    if (!own[0]) die("客體線程 TLS 塊分配失敗");
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;               // 當前線程對象 | current thread object
    pthread_setspecific(g_keytls, own[0]); pthread_setspecific(g_keytlr, tlr);
    void **prev = pthread_getspecific(g_keyown); if (prev) own_free(prev);   // 同一線程再初始化：先放上一份（同 crash_thread_init 只配一次的道理，這裡按新的為準） | re-initialised on the same thread: free the previous set first (the new one is what the slots now point to)
    pthread_setspecific(g_keyown, own);
}
