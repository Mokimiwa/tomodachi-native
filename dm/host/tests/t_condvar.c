// svc 0x1C（WaitProcessWideKeyAtomic）與 SDK 用戶態快路徑之間的可見性窗口，確定性單測（資源表讀寫鎖丟喚醒卡死）。
// 場景：W 持互斥鎖後調 0x1C 等條件變量；鈎子落在 0x1C「放鎖之後、掛鏈之前」，此刻讓 R 按 SDK 用戶態寫法動作：
//   CAS 0→R 搶鎖 → 讀條件鍵，非 0 才發 svc 0x1D(鍵, 1)（sdk InternalConditionVariable::Signal 0x351140 同此）→ 放鎖：鎖字仍是 R 的標籤則 CAS→0，帶等待位則 svc 0x1B。
// 期望（Horizon 內核語義：等待者先發佈「有等待」再放鎖 ⇒ 任何在其放鎖後改條件的線程必能看到鍵非 0 並喚醒它）：
//   W 的 0x1C 返回 0；返回時鎖字 = W 的標籤（被喚醒者持鎖）；條件鍵 = 0（無剩餘等待者）。
// 構建兩份：t_condvar（現行順序，須全過）與 t_condvar_old（-DDM_TEST_CV_OLD_ORDER 還原舊順序，須判出丟喚醒 = 負對照有效）。
// Deterministic unit test of the visibility window between svc 0x1C (WaitProcessWideKeyAtomic) and the SDK's user-mode fast path (the lost wakeup hang of the resource table read-write lock).
// Scenario: W holds the mutex and calls 0x1C to wait on the condition variable; the hook sits in 0x1C "after releasing the lock, before linking into the wait list", and at this moment R acts the way the SDK's user-mode code does:
//   CAS 0->R to grab the lock -> read the condition key, send svc 0x1D(key, 1) only if non-zero (same as sdk InternalConditionVariable::Signal 0x351140) -> release the lock: CAS->0 if the lock word is still R's tag, svc 0x1B if the waiter bit is set.
// Expected (Horizon kernel semantics: a waiter publishes "there is a waiter" before releasing the lock => any thread that changes the condition after it releases the lock is bound to see a non-zero key and wake it):
//   W's 0x1C returns 0; on return the lock word = W's tag (the woken thread holds the lock); condition key = 0 (no remaining waiters).
// Two builds: t_condvar (current order, must fully pass) and t_condvar_old (-DDM_TEST_CV_OLD_ORDER restores the old order, must detect the lost wakeup = the negative control is valid).
#include "../dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
int svc_thread(int num, uint64_t *regs);
uint32_t svc_thread_main_init(void);
void (*dm_test_cv_after_release)(void);

static _Atomic uint32_t g_mutex, g_key;            // 互斥鎖字、條件鍵（對應 SDK InternalCriticalSection / InternalConditionVariable 的一個字） | mutex word, condition key (corresponding to one word of the SDK's InternalCriticalSection / InternalConditionVariable)
static uint32_t g_wtag, g_rtag = 0x2002;
static _Atomic int g_rstate;                       // 0 未動 1 已搶到鎖 2 將發 0x1D 3 完成 | 0 not started 1 lock acquired 2 about to send 0x1D 3 done
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_c = PTHREAD_COND_INITIALIZER;
static int g_go, g_wdone; static int64_t g_wret = -1; static uint32_t g_wlock_after;
static void state_set(int state) {
    atomic_store(&g_rstate, state);
    pthread_mutex_lock(&g_m); pthread_cond_broadcast(&g_c); pthread_mutex_unlock(&g_m);
}

static void *reader(void *a) {
    (void)a;
    pthread_mutex_lock(&g_m); while (!g_go) pthread_cond_wait(&g_c, &g_m); pthread_mutex_unlock(&g_m);
    uint32_t z = 0; while (!atomic_compare_exchange_strong(&g_mutex, &z, g_rtag)) { z = 0; sched_yield(); }   // 用戶態搶鎖 | grab the lock in user mode
    state_set(1);
    // （此處 R 改條件：例如把讀者計數減到 0）
    // (here R changes the condition: for example decrementing the reader count to 0)
    if (atomic_load(&g_key)) { state_set(2); uint64_t r[8] = {(uint64_t)&g_key, 1}; svc_thread(0x1D, r); }   // Signal 快路徑：鍵 0 不發 svc | Signal fast path: key 0 sends no svc
    uint32_t v = g_rtag;
    if (!atomic_compare_exchange_strong(&g_mutex, &v, 0)) { uint64_t r[8] = {(uint64_t)&g_mutex}; svc_thread(0x1B, r); }      // 帶等待位 ⇒ ArbitrateUnlock | waiter bit set => ArbitrateUnlock
    state_set(3);
    return NULL;
}
static void hook(void) {   // 在 W 的 0x1C 裡、持宿主全局鎖 G、已放鎖未掛鏈：放 R 去跑，等它「完成」或「將發 0x1D（會卡在 G 上）」 | inside W's 0x1C, holding the host global lock G, lock released but not yet linked into the wait list: let R run and wait until it is "done" or "about to send 0x1D (will block on G)"
    dm_test_cv_after_release = NULL;
    pthread_mutex_lock(&g_m); g_go = 1; pthread_cond_broadcast(&g_c); pthread_mutex_unlock(&g_m);
    pthread_mutex_lock(&g_m); while (atomic_load(&g_rstate) != 2 && atomic_load(&g_rstate) != 3) pthread_cond_wait(&g_c, &g_m); pthread_mutex_unlock(&g_m);
}
static void *waiter(void *a) {
    (void)a;
    g_wtag = svc_thread_main_init();
    uint32_t z = 0; atomic_compare_exchange_strong(&g_mutex, &z, g_wtag);                 // W 持鎖 | W holds the lock
    dm_test_cv_after_release = hook;
    uint64_t r[8] = {(uint64_t)&g_mutex, (uint64_t)&g_key, g_wtag, (uint64_t)-1};
    svc_thread(0x1C, r);
    pthread_mutex_lock(&g_m); g_wret = (int64_t)r[0]; g_wlock_after = atomic_load(&g_mutex); g_wdone = 1; pthread_cond_broadcast(&g_c); pthread_mutex_unlock(&g_m);
    return NULL;
}
int main(void) {
    pthread_t w, rd; pthread_create(&rd, NULL, reader, NULL); pthread_create(&w, NULL, waiter, NULL);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 10;
    pthread_mutex_lock(&g_m); while (!g_wdone) if (pthread_cond_timedwait(&g_c, &g_m, &dl)) break; int done = g_wdone; pthread_mutex_unlock(&g_m);
#ifdef DM_TEST_CV_OLD_ORDER
    // 負對照：舊順序必丟喚醒（R 讀到鍵 0 不發 0x1D，W 掛上去後無人喚醒）
    // Negative control: the old order always loses the wakeup (R reads key 0 and does not send 0x1D, and nobody wakes W after it is queued)
    int ok = !done && atomic_load(&g_rstate) == 3;
    printf("%s 舊順序負對照：W %s，R 狀態 %d（期望 W 不醒、R 未發 0x1D 即完成）\n", ok ? "✓" : "✗", done ? "被喚醒" : "10 秒未醒", atomic_load(&g_rstate));
    printf(ok ? "全過：1/1\n" : "失敗：負對照沒判出丟喚醒（判據無效）\n");
#else
    int ok = done && g_wret == 0 && g_wlock_after == g_wtag && atomic_load(&g_key) == 0;
    printf("%s 現行順序：W %s 返回 %lld，鎖字 %#x（期望 W 標籤 %#x），條件鍵 %u（期望 0），R 狀態 %d\n", ok ? "✓" : "✗",
           done ? "被喚醒" : "2 秒未醒", (long long)g_wret, g_wlock_after, g_wtag, atomic_load(&g_key), atomic_load(&g_rstate));
    printf(ok ? "全過：1/1\n" : "失敗：1/1\n");
#endif
    fflush(stdout); _exit(ok ? 0 : 1);
}
