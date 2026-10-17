// 宿主發起的客體線程（Granny 預解碼等宿主後台活要調遊戲自己的分配器 / 鎖，需要完整的 SDK 線程上下文）。
// 做法：在客體線程上（例如 guest_main 進 nnMain 之前）直接調 SDK 的 nn::os::CreateThread / StartThread，入口填宿主函數——
// 線程由 SDK 建，ThreadType、線程句柄、TLS 槽全齊，遊戲的 malloc / nn::os 鎖照常可用（鎖標籤字寫的是該線程的客體句柄）。
// 約束：必須在客體線程上調用（SDK 建線程走 SVC 門）；不暴露給遊戲。
// 自檢 DM_GUEST_SPAWN_TEST=1：guest_main 進 nnMain 前起一個線程，做「SDK malloc/free + nn::os 互斥鎖加鎖、核持有者是自己」，WaitThread 收尾後報結果。
// Host-initiated guest threads (host background work such as Granny pre-decoding needs to call the game's own allocator / locks and needs a complete SDK thread context).
// Approach: on a guest thread (e.g. before guest_main enters nnMain) call the SDK's nn::os::CreateThread / StartThread directly, with a host function as the entry --
// the thread is created by the SDK, so ThreadType, thread handle and TLS slots are all in place, and the game's malloc / nn::os locks work as usual (the lock tag word holds this thread's guest handle).
// Constraints: must be called on a guest thread (SDK thread creation goes through the SVC gate); not exposed to the game.
// Self-test DM_GUEST_SPAWN_TEST=1: before guest_main enters nnMain, start one thread that does "SDK malloc/free + lock an nn::os mutex and check that the owner is itself", and report the result after WaitThread finishes.
#include "hle.h"
#include <stdatomic.h>
#include <unistd.h>
typedef int (*create_fn)(void *tt, void (*fn)(void *), void *arg, void *stack, size_t size, int prio);
typedef void (*tt_fn)(void *tt);
static void *need(const char *n) { void *p = guest_sym(n); if (!p) die("宿主客體線程：缺符號 %s", n); return p; }
// 線程入口墊一層：先登記為宿主自建（hle_thread.c dm_yield_to_core_peers 不等它們），再進 fn | an entry shim: first register the thread as host-created (hle_thread.c dm_yield_to_core_peers does not wait for them), then run fn
typedef struct { void (*fn)(void *); void *arg; } spawn_arg;
void dm_thread_mark_host(void);
static void spawn_tramp(void *p) { spawn_arg a = *(spawn_arg *)p; free(p); dm_thread_mark_host(); a.fn(a.arg); }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    spawn_arg *sa = malloc(sizeof *sa); if (!sa) die("宿主客體線程：分配失敗"); sa->fn = fn; sa->arg = arg; fn = spawn_tramp; arg = sa;
    stack_size = (stack_size + 0xfff) & ~(size_t)0xfff;
    void *tt = calloc(1, 0x200);                                 // nn::os::ThreadType（0x1c0 字節，留餘量） | nn::os::ThreadType (0x1c0 bytes, with headroom)
    void *stack = aligned_alloc(0x1000, stack_size);              // SDK 要求棧 4 KiB 對齊、大小 4 KiB 整數倍 | the SDK requires the stack to be 4 KiB aligned and its size a multiple of 4 KiB
    if (!tt || !stack) die("宿主客體線程：分配失敗");
    int rc = ((create_fn)need("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"))(tt, fn, arg, stack, stack_size, prio);
    if (rc) die("宿主客體線程：CreateThread 失敗 %#x", rc);
    ((tt_fn)need("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"))(tt);
    return tt;
}
void dm_guest_thread_join(void *tt) { ((tt_fn)need("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"))(tt); ((tt_fn)need("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"))(tt); }

static struct { int malloc_ok, lock_owned, handle; } g_st;
static void selftest_body(void *arg) {
    (void)arg;
    void *(*gmalloc)(size_t) = need("malloc"); void (*gfree)(void *) = need("free");
    void *p = gmalloc(4096); if (p) { memset(p, 0x5a, 4096); gfree(p); g_st.malloc_ok = 1; }
    uint8_t mtx[0x40] __attribute__((aligned(16))) = {0};           // nn::os::MutexType（0x20 字節，留餘量） | nn::os::MutexType (0x20 bytes, with headroom)
    ((void (*)(void *, int, int))need("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi"))(mtx, 0, 0);
    ((void (*)(void *))need("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"))(mtx);
    g_st.lock_owned = ((int (*)(const void *))need("_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE"))(mtx) & 1;
    ((void (*)(void *))need("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"))(mtx);
    uint32_t thr_cur_handle(void); g_st.handle = (int)thr_cur_handle();
}
static void park_body(void *arg) { (void)arg; for (;;) sleep(3600); }   // 常駐（=2）：模擬預解碼線程整局存活，佔住句柄 | resident (=2): simulates the pre-decode thread staying alive for the whole run, occupying the handle
void dm_guest_spawn_selftest(void) {
    const char *e = getenv("DM_GUEST_SPAWN_TEST"); if (!e) return;
    if (atoi(e) == 2) { dm_guest_thread_spawn(park_body, NULL, 16, 16 << 10); LOG("宿主客體線程自檢：另起常駐線程（之後遊戲線程句柄順延）"); }
    void *tt = dm_guest_thread_spawn(selftest_body, NULL, 16, 64 << 10);
    dm_guest_thread_join(tt);
    uint32_t thr_cur_handle(void); int mine = (int)thr_cur_handle();
    int ok = g_st.malloc_ok && g_st.lock_owned && g_st.handle && g_st.handle != mine;
    LOG("%s宿主客體線程自檢：SDK malloc/free %s；互斥鎖持有者是本線程 %s；線程句柄 %#x（主線程 %#x）", ok ? "" : "【紅】",
        g_st.malloc_ok ? "通" : "不通", g_st.lock_owned ? "是" : "否", g_st.handle, mine);
}
