// Horizon 內核的線程與同步 SVC，原生實現：每個客體線程 = 一個宿主 pthread，跑在 SDK 給的客體棧上。
// SDK 的 nn::os 原樣運行，只把它下面的「內核」換成宿主：
//   線程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 優先級 0x0E/0x0F 核掩碼
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   鎖     0x1A ArbitrateLock 0x1B ArbitrateUnlock（標籤字 = 持有者句柄 | 0x40000000 有等待者）
//   條件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 語義按公開 Horizon 內核行為（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待掛在一把全局鎖 G 上，每線程一個條件變量；先求正確，性能後量。
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#include "dm.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>
#include <stdatomic.h>

#define R_TIMEOUT     0xEA01u   // 內核模塊 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的時刻與當時的遊戲拍（現場導出用：作業工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // 當前等待：kind 0=無 1=鎖 2=條件 3=地址 4=同步對象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登記等待的先後序號：同優先級先到先醒（與 Horizon 內核 KThread 等待樹同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待鏈 | Wait chain
    struct thr *all;           // 全部線程鏈（現場導出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr;
    int closed;                // 句柄已關：與 exited 同時成立才釋放對象（Horizon：線程對象隨最後一個引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次數（現場導出用） | SVC count (for the state dump)
    mach_port_t mport;         // 宿主線程端口（取 CPU 時間） | Host thread port (to get CPU time)
    uint32_t qos;
    // 暫停（SetThreadActivity，IL2CPP 的垃圾回收停世界用）：pause_req 請求、paused 已停住；停在 SVC 裡時現場取門幀 svc_regs（進 SVC 時登記），
    // 停在客體代碼裡時現場由信號處理存進 ctx_*；pcv 讓請求方等「已停住」、被停線程等「恢復」
    // Pausing (SetThreadActivity, used by IL2CPP's garbage collector to stop the world): pause_req = requested, paused = stopped; when stopped inside an SVC the context comes from the gate frame svc_regs (registered on SVC entry),
    // when stopped in guest code the signal handler saves it into ctx_*; pcv lets the requester wait for "stopped" and the paused thread wait for "resume"
    _Atomic int pause_req; int paused;
    uint64_t *_Atomic svc_regs; uint64_t svc_pc;
    int ctx_ok; uint64_t ctx_x[29], ctx_fp, ctx_lr, ctx_sp, ctx_pc; uint32_t ctx_pstate, ctx_fpcr, ctx_fpsr; __uint128_t ctx_v[32];
    pthread_cond_t pcv;
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的線程（鎖 / 條件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // 調用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表滿");
}
static void *h_peek(uint32_t h, int type) {      // 只查不報 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即報（句柄錯是真錯誤） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄無效 %#x（要類型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("非客體線程調用了線程 SVC");
    return t;
}

// ---------- 等待原語（持 G 調用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按內核順序挑等待者：優先級數值小者優先，同優先級先登記者優先（持 G）。kind 1 按鎖地址、2 按條件鍵、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }
// 掛起直到被喚醒或超時（ns<0 無限、0 立即超時）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
static int64_t block(thr_t *t, int kind, int64_t ns) {   // 啓動時間線：按等待種類累計（全線程）+ 主線程單列 | Boot timeline: accumulated by wait kind (all threads) + the main thread listed separately
    if (!g_flag.bootprof) return block_inner(t, kind, ns);
    uint64_t t0 = now_ns(); int64_t r = block_inner(t, kind, ns); uint64_t d = now_ns() - t0;
    static const int id[] = {0, PROF_WAIT_LOCK, PROF_WAIT_CV, PROF_WAIT_ADDR, PROF_WAIT_SYNC};
    dm_prof_add(id[kind & 7], d, 0);
    if (t->handle == 0x1001) dm_prof_add(PROF_MAIN_WAIT, d, 0);
    return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = pthread_cond_wait(&t->cv, &G);
        } else rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 鎖 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 釋放 addr 處的鎖：交給等在它上面的第一個線程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 讓 t 去拿 addr 處的鎖：空閒就直接拿到並喚醒，否則置等待者位並轉為鎖等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 裡，改為鎖等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 內核把它新插進鎖持有者的等待隊列：同優先級排在已有鎖等待者之後 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 線程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取當前遊戲拍；diag.c 初始化時登記（單測不登記，拍號記 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每線程 TSD 槽（門、x18、TLS、TLR）唯一設置點 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self());
    crash_thread_init();
    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    dm_thread_enter(t->entry, t->arg, t->stack_top);   // 不返回：客體須經 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客體線程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；從全部線程鏈摘下並釋放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); pthread_cond_destroy(&t->pcv); free(t);
}
static const char *thr_name(thr_t *t, char *buf, size_t n);
static void thread_exit(thr_t *t) {
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 這裡不打日誌：退出發生在客體線程自己的棧上（有的只有十幾 KiB），帶浮點的 printf 會把棧撐爆（真機冒煙：LibcurlResolver 退出時 SIGBUS，回溯 fprintf → __dtoa）。時刻只記下來，現場導出時再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    t->exited = 1;
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
void dm_thread_returned(void) { die("客體線程入口函數返回（Horizon 要求 ExitThread）"); }

// ---------- 暫停線程（SetThreadActivity / GetThreadContext3）----------
// IL2CPP 的 Boehm 垃圾回收停世界：逐個 SetThreadActivity(句柄, 暫停)、GetThreadContext3 讀寄存器找根、掃棧，再恢復。原先兩者都是佔位成功：
// 線程照跑、現場是空的，回收器漏掉只在寄存器裡的指針、提前回收還在用的對象（終結器連帶釋放原生資源）。現在照 Horizon 語義真停：
//   · 在 SVC 裡（門幀已存全寄存器）：立即算停住，現場 = 門幀；SVC 返回前若仍在暫停就停在出口等恢復；
//   · 在跑客體代碼：發 SIGUSR2，處理函數存下 ucontext 後原地等恢復（客體代碼不持宿主鎖，可在信號處理裡取 G）；
//   · 在宿主代碼裡（HLE 函數等）：處理函數不停，請求方隔 100 µs 重發，直到它回到客體代碼或進 SVC。
// IL2CPP's Boehm garbage collector stops the world: SetThreadActivity(handle, paused) on each thread, GetThreadContext3 to read registers for roots, scan the stack, then resume. Both used to be placeholders that reported success:
// threads kept running and the context was empty, so the collector missed pointers held only in registers and freed objects still in use (finalizers releasing native resources with them). They now really stop, as on Horizon:
//   · inside an SVC (the gate frame holds every register): stopped at once, context = the gate frame; if still paused when the SVC returns, the thread waits at the exit for the resume;
//   · running guest code: send SIGUSR2; the handler saves the ucontext and waits there for the resume (guest code holds no host locks, so the handler may take G);
//   · inside host code (HLE functions etc.): the handler does not stop it; the requester resends every 100 µs until the thread is back in guest code or enters an SVC.
module_t *module_of(const void *p);
static void on_pause_sig(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t || !atomic_load(&t->pause_req) || atomic_load(&t->svc_regs)) return;
    ucontext_t *uc = uc_; const struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;
    if (!module_of((void *)__darwin_arm_thread_state64_get_pc(*ss))) return;   // 在宿主代碼裡：不停，請求方重發 | in host code: do not stop; the requester resends
    pthread_mutex_lock(&G);
    if (t->pause_req) {
        for (int i = 0; i < 29; i++) t->ctx_x[i] = ss->__x[i];
        t->ctx_fp = __darwin_arm_thread_state64_get_fp(*ss); t->ctx_lr = __darwin_arm_thread_state64_get_lr(*ss);
        t->ctx_sp = __darwin_arm_thread_state64_get_sp(*ss); t->ctx_pc = __darwin_arm_thread_state64_get_pc(*ss); t->ctx_pstate = ss->__cpsr;
        memcpy(t->ctx_v, uc->uc_mcontext->__ns.__v, sizeof t->ctx_v); t->ctx_fpcr = uc->uc_mcontext->__ns.__fpcr; t->ctx_fpsr = uc->uc_mcontext->__ns.__fpsr;
        t->ctx_ok = 1; t->paused = 1; pthread_cond_broadcast(&t->pcv);
        while (t->pause_req) pthread_cond_wait(&t->pcv, &G);
        t->paused = 0; t->ctx_ok = 0;
    }
    pthread_mutex_unlock(&G);
}
// 門進 SVC / 出 SVC 時調（hle.c gate_dispatch） | called by the gate on SVC entry / exit (gate_dispatch in hle.c)
void thr_svc_enter(uint64_t *regs, uint64_t pc) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    t->svc_pc = pc; atomic_store(&t->svc_regs, regs);
    if (atomic_load(&t->pause_req)) { pthread_mutex_lock(&G); if (t->pause_req && !t->paused) { t->paused = 1; pthread_cond_broadcast(&t->pcv); } pthread_mutex_unlock(&G); }
}
void thr_svc_leave(void) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    pthread_mutex_lock(&G);
    while (t->pause_req) { t->paused = 1; pthread_cond_broadcast(&t->pcv); pthread_cond_wait(&t->pcv, &G); }
    t->paused = 0; atomic_store(&t->svc_regs, NULL);
    pthread_mutex_unlock(&G);
}
static void pause_install(void) {
    struct sigaction sa = {.sa_sigaction = on_pause_sig, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
}
// Horizon svc::ThreadContext（arm64，0x320 字節） | Horizon svc::ThreadContext (arm64, 0x320 bytes)
typedef struct { uint64_t r[29], fp, lr, sp, pc; uint32_t pstate, pad; __uint128_t v[32]; uint32_t fpcr, fpsr; uint64_t tpidr; } hz_ctx_t;
_Static_assert(sizeof(hz_ctx_t) == 0x320, "ThreadContext");
static void ctx_fill(const thr_t *t, hz_ctx_t *o) {   // 持 G、t 已停住 | holding G, t is stopped
    memset(o, 0, sizeof *o);
    const uint64_t *r = atomic_load(&t->svc_regs);
    if (r) {   // 門幀：x0..x30 在 +0、q0..q31 在 +256、nzcv / fpsr / fpcr 在 +768 / +776 / +784；進門前的棧頂 = 門幀 + 800 | gate frame: x0..x30 at +0, q0..q31 at +256, nzcv / fpsr / fpcr at +768 / +776 / +784; the stack top before the gate = frame + 800
        for (int i = 0; i < 29; i++) o->r[i] = r[i];
        o->fp = r[29]; o->lr = r[30]; o->sp = (uint64_t)r + 800; o->pc = t->svc_pc; o->pstate = (uint32_t)r[96];
        memcpy(o->v, r + 32, sizeof o->v); o->fpsr = (uint32_t)r[97]; o->fpcr = (uint32_t)r[98];
    } else if (t->ctx_ok) {
        memcpy(o->r, t->ctx_x, sizeof o->r); o->fp = t->ctx_fp; o->lr = t->ctx_lr; o->sp = t->ctx_sp; o->pc = t->ctx_pc; o->pstate = t->ctx_pstate;
        memcpy(o->v, t->ctx_v, sizeof o->v); o->fpcr = t->ctx_fpcr; o->fpsr = t->ctx_fpsr;
    }
}

// 主線程登記，返回其句柄（交給 nnosInitialize 作主線程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_key_create(&g_keythr, NULL);
    pause_install();
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt);
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    return t->handle;
}

static uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客體主線程寫；診斷行在同一線程讀 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle == 0x1001) continue;
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    pthread_mutex_unlock(&G);
    if (count) *count = n;
    return qos;
}

// ---------- 同步對象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已觸發（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// 處理線程 / 同步類 SVC；返回 1 = 已處理
// Handle thread / synchronization SVCs; returns 1 = handled
static const char *thr_name(thr_t *t, char *nb, size_t n) {   // SDK ThreadType+0x1a0 = 名字指針 | SDK ThreadType+0x1a0 = name pointer
    if (!t->arg) return t->handle == 0x1001 ? "主線程" : "?";
    uint64_t np = 0; vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), t->arg + 0x1a0, 8, (vm_address_t)&np, &got) || !np ||
        vm_read_overwrite(mach_task_self(), np, n - 1, (vm_address_t)nb, &got)) return "?";
    nb[got] = 0; return nb;
}
// 全部活著的客體線程（採樣器 DM_SAMPLE=毫秒:* 用）：把 pthread 填進 out，返回個數
// All live guest threads (used by the sampler DM_SAMPLE=ms:*): fill the pthreads into out, return the count
int thr_list(pthread_t *out, int max) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t && n < max; t = t->all) if (!t->exited && t->started) out[n++] = t->pt;
    pthread_mutex_unlock(&G);
    return n;
}
// 浸泡讀數（DM_SOAK）：句柄表佔用、存活客體線程、線程對象總數（含已退出未釋放）
// Soak readings (DM_SOAK): handle table usage, live guest threads, total thread objects (including exited but not yet freed)
void thr_counts(int *handles, int *live, int *objs) {
    int nh = 0, nl = 0, no = 0;
    pthread_mutex_lock(&G);
    for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    for (thr_t *t = g_all; t; t = t->all) { no++; nl += t->started && !t->exited; }
    pthread_mutex_unlock(&G);
    *handles = nh; *live = nl; *objs = no;
}
// 按 pthread 取客體線程名（按線程 CPU 採樣 DM_THRCPU 用，memstat.m）：不是客體線程返回 0
// Get the guest thread name by pthread (used by per-thread CPU sampling DM_THRCPU, memstat.m): returns 0 if not a guest thread
int thr_name_of(pthread_t pt, char *out, size_t n) {
    int ok = 0; char nb[64];
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (t->pt == pt && !t->exited) { snprintf(out, n, "%#x %s", t->handle, thr_name(t, nb, sizeof nb)); ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
uint32_t thr_cur_handle(void) { thr_t *t = pthread_getspecific(g_keythr); return t ? t->handle : 0; }   // 只讀 TSD，信號處理裡可用 | Only reads TSD, usable in a signal handler
// 按線程名找活著的客體線程（採樣器用）；找不到返回 0
// Find a live guest thread by thread name (used by the sampler); returns 0 if not found
int thr_find_by_name(const char *name, pthread_t *out) {
    char nb[32]; int ok = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited && !strcmp(thr_name(t, nb, sizeof nb), name)) { *out = t->pt; ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
// 現場導出（看門狗 / 崩潰時由 crash.c 調；只讀、不取鎖）：每個客體線程在等什麼、最近一次 SVC 從哪發出
// 現場導出時取別的客體線程的回溯：暫停 → 讀寄存器 → 沿幀指針鏈走（客體與門塊都保持 x29 鏈）→ 恢復；只打 main / sdk 幀
// State dump (called from crash.c on watchdog / crash; read-only, takes no lock): what each guest thread is waiting on and where its most recent SVC was issued from
// During a state dump, take the backtrace of other guest threads: suspend → read registers → walk the frame pointer chain (both guest code and gate blocks keep the x29 chain) → resume; only main / sdk frames are printed
static void thr_bt(thr_t *t) {
    if (!t->mport || t->exited || t->mport == mach_thread_self()) return;
    if (thread_suspend(t->mport) != KERN_SUCCESS) return;
    arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
        // 沒在等系統調用的線程（在跑客體代碼，可能在空轉等一個條件）：打被調用者保存寄存器 x19–x28，以及 x19 / x20 指向的內存各 128 字節。
        // 偶發「進局後不出幀」現場：主線程在作業池的「邊幫忙邊等」（main+0x14f08d8）裡空轉，x19 = 等待對象、x20 = 作業池——有這些才能看出它在等哪一批、未完成計數是幾
        // Threads not waiting in a system call (running guest code, possibly spinning on a condition): print the callee-saved registers x19–x28, plus 128 bytes of the memory pointed to by each of x19 / x20.
        // The state seen in the occasional "no frames after entering a run": the main thread spins in the job pool's "help while waiting" (main+0x14f08d8), x19 = wait object, x20 = job pool -- only with these can one tell which batch it is waiting for and what the unfinished count is
        if (t->wkind == 0) { char rl[400]; int q = snprintf(rl, sizeof rl, "[dm]   %#x 寄存器 pc=", t->handle); uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc);
            q += pm ? snprintf(rl + q, sizeof rl - q, "%s+%#llx", pm->name, pc - (uint64_t)pm->base) : snprintf(rl + q, sizeof rl - q, "%#llx", pc);
            for (int i = 19; i <= 28; i++) q += snprintf(rl + q, sizeof rl - q, " x%d=%#llx", i, st.__x[i]);
            fprintf(stderr, "%s\n", rl);
            for (int i = 19; i <= 20; i++) { uint8_t raw[128]; vm_size_t g = 0; uint64_t a = st.__x[i];
                if (a < 0x100000000ull || vm_read_overwrite(mach_task_self(), a, sizeof raw, (vm_address_t)raw, &g) || g != sizeof raw) continue;
                char hx[420]; int h = 0; for (int k = 0; k < 128; k += 4) h += snprintf(hx + h, sizeof hx - h, "%08x ", *(uint32_t *)(raw + k));
                uint64_t vt = *(uint64_t *)raw; module_t *vm_ = module_of((void *)vt); char vs[64] = ""; if (vm_) snprintf(vs, sizeof vs, "（首 8 字節 = %s+%#llx）", vm_->name, vt - (uint64_t)vm_->base);
                fprintf(stderr, "[dm]   %#x x%d → %#llx 起 128 字節%s: %s\n", t->handle, i, a, vs, hx); } }
        uint64_t fp = __darwin_arm_thread_state64_get_fp(st), lr = __darwin_arm_thread_state64_get_lr(st);
        // 在跑客體代碼的線程：棧頂往上 1 KiB 原樣打出來（外層各幀保存的寄存器在裡面，事後能找回等待對象、資源地址）
        // Threads running guest code: print the 1 KiB above the stack top as is (the registers saved by the outer frames are in there, so the wait object and resource address can be recovered afterwards)
        if (t->wkind == 0 && module_of((void *)__darwin_arm_thread_state64_get_pc(st))) { uint64_t sp = __darwin_arm_thread_state64_get_sp(st), stk[128]; vm_size_t g = 0;   // pc 在客體模塊裡才打：在睡眠 / 讓出裡的線程 pc 在系統庫，棧沒看頭 | Printed only if pc is inside a guest module: a thread in sleep / yield has its pc in a system library, and the stack is not worth looking at
            if (sp > 0x100000000ull && !vm_read_overwrite(mach_task_self(), sp, sizeof stk, (vm_address_t)stk, &g) && g == sizeof stk)
                for (int r = 0; r < 128; r += 8) fprintf(stderr, "[dm]   %#x 棧 sp+%#05x: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n", t->handle, r * 8,
                        stk[r], stk[r + 1], stk[r + 2], stk[r + 3], stk[r + 4], stk[r + 5], stk[r + 6], stk[r + 7]); }
        char line[1024]; int p = snprintf(line, sizeof line, "[dm]   %#x 回溯:", t->handle), nf = 0; uint64_t wait_fp = 0;
        for (int k = 0; k < 64 && fp && nf < 20 && p < (int)sizeof line - 40; k++) {
            module_t *m = module_of((void *)lr);
            if (m) { p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, lr - (uint64_t)m->base); nf++; }
            uint64_t fr[2]; vm_size_t g2 = 0;
            if (vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
            if (m && !strcmp(m->name, "main") && lr - (uint64_t)m->base == 0x132db30) wait_fp = fp;   // 返回點在資源等待函數 main+0x132d988 裡（它調作業池「邊幫忙邊等」之後）：此刻的幀指針就是它自己的 | The return point is inside the resource wait function main+0x132d988 (after it calls the job pool's "help while waiting"): the frame pointer at this moment is its own
            fp = fr[0]; lr = fr[1];
        }
        fprintf(stderr, "%s\n", line);
        // 資源等待函數 main+0x132d988（序言 sub sp,#0x50 … add x29,sp,#0x40）在自己棧底擺等待對象 {虛表, 指向事件指針的指針}（0x132db14 stp x8,x23,[sp]），
        // 所以等待對象 = 它的幀指針 −0x40；所指 −0x10 = 資源等待對象 {+0 在途作業, +8 互斥鎖指針, +0x10 事件指針}。按返回點 0x132db30 定位，與線程此刻停在等待循環的哪一條指令無關。
        // 舊寫法按 0x14f0934 取「上一幀」取錯一層，把資源等待對象當成等待對象讀，「在途作業 為空」「事件 / 鎖 讀不到」都是錯讀
        // The resource wait function main+0x132d988 (prologue sub sp,#0x50 … add x29,sp,#0x40) places the wait object {vtable, pointer to the event pointer} at the bottom of its own stack (0x132db14 stp x8,x23,[sp]),
        // so the wait object = its frame pointer −0x40; what that points to, −0x10 = the resource wait object {+0 in-flight job, +8 mutex pointer, +0x10 event pointer}. It is located by the return point 0x132db30, regardless of which instruction of the wait loop the thread is stopped on right now.
        // The old code took "the previous frame" based on 0x14f0934 and was off by one level, reading the resource wait object as the wait object; 「在途作業 為空」(in-flight job is null) and 「事件 / 鎖 讀不到」(event / lock unreadable) were both misreads
        if (wait_fp) { uint64_t wo[2] = {0, 0}, res[3] = {0, 0, 0}, wjob = 0; vm_size_t g = 0;
            #define RD(addr, buf) ((addr) > 0x100000000ull && !vm_read_overwrite(mach_task_self(), (addr), sizeof(buf), (vm_address_t)(buf), &g) && g == sizeof(buf))
            #define HEX(tag, addr, nbytes) { uint32_t raw[32]; vm_size_t gg = 0; if ((addr) > 0x100000000ull && !vm_read_overwrite(mach_task_self(), (addr), (nbytes), (vm_address_t)raw, &gg) && gg == (nbytes)) { \
                char hx[300]; int h = 0; for (unsigned q = 0; q < (nbytes) / 4; q++) h += snprintf(hx + h, sizeof hx - h, "%08x ", raw[q]); fprintf(stderr, "[dm]   %#x 作業池等待：%s %#llx: %s\n", t->handle, tag, (unsigned long long)(addr), hx); } \
                else fprintf(stderr, "[dm]   %#x 作業池等待：%s %#llx 讀不到\n", t->handle, tag, (unsigned long long)(addr)); }
            if (RD(wait_fp - 0x40, wo)) { module_t *vm_ = module_of((void *)wo[0]);
                fprintf(stderr, "[dm]   %#x 作業池等待：等待對象 %#llx 虛表 %s+%#llx（應為 main+0x20f2408），所指 %#llx\n", t->handle, (unsigned long long)(wait_fp - 0x40), vm_ ? vm_->name : "?", vm_ ? wo[0] - (uint64_t)vm_->base : wo[0], wo[1]);
                if (RD(wo[1] - 0x10, res)) {
                    fprintf(stderr, "[dm]   %#x 作業池等待：資源等待對象 %#llx：在途作業 %#llx（%s）、互斥鎖 %#llx、事件 %#llx。判別：在途作業為空 = 信號已被某次等待消費；非空且事件未置位 = 完成信號沒來；事件已置位 = 等待循環的問題\n",
                            t->handle, (unsigned long long)(wo[1] - 0x10), res[0], res[0] ? "非空" : "為空", res[1], res[2]);
                    HEX("事件本體 64 字節", res[2], 64);
                    HEX("互斥鎖本體 32 字節（+8 重入計數、+0x10 持有線程對象、+0x18 鎖字 = 持有者句柄）", res[1], 32);
                    // 作業對象（提交函數 main+0x147f154 填的）：+0x10 標誌（取作業時按線程類別掩碼過濾，0xeda190 / 0xdf8fac）、+0x20 隊列裡的下一個、+0x30 起依賴表（低 31 位 = 個數）、+0x60 資源、+0x68 指向事件指針的指針
                    // Job object (filled by the submit function main+0x147f154): +0x10 flags (filtered by a per-thread-class mask when taking a job, 0xeda190 / 0xdf8fac), +0x20 next in the queue, dependency table from +0x30 (low 31 bits = count), +0x60 resource, +0x68 pointer to the event pointer
                    if (res[0]) { HEX("在途作業對象 +0 起 128 字節", res[0], 128); HEX("在途作業對象 +0x80 起 128 字節", res[0] + 0x80, 128); } wjob = res[0]; } }
            // 作業池：線程正停在等待循環 main+0x14f08d8 自己的指令上時 x20 就是它；停在它調出去的函數裡時 x20 不可信，不打
            // Job pool: when the thread is stopped on an instruction of the wait loop main+0x14f08d8 itself, x20 is the pool; when it is stopped in a function called from it, x20 cannot be trusted and is not printed
            { uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc); uint64_t off = pm && !strcmp(pm->name, "main") ? pc - (uint64_t)pm->base : 0;
              // 隊列（0xeda190）：{+0 尾, +8 頭, +0x10 個數, +0x18 鎖}，節點 +0x10 標誌、+0x20 下一個。順著頭走最多 8 個，看被等的作業在不在這個池裡
              // Queue (0xeda190): {+0 tail, +8 head, +0x10 count, +0x18 lock}, node +0x10 flags, +0x20 next. Walk from the head for at most 8 nodes to see whether the awaited job is in this pool
              if (off >= 0x14f0900 && off < 0x14f09b8) { uint64_t qa = st.__x[20] + 0x410, nd = 0; HEX("作業隊列頭（池 = x20，+0x410 起 32 字節）", qa, 32); int in = 0, cnt = 0;
                  if (RD(qa + 8, &nd)) for (; nd && cnt < 8; cnt++) { uint64_t nb[5] = {0}; if (!RD(nd, nb)) break; fprintf(stderr, "[dm]   %#x 作業池等待：  隊列第 %d 個 %#llx 標誌 %#x%s\n", t->handle, cnt, (unsigned long long)nd, (uint32_t)nb[2], nd == wjob ? "（就是被等的作業）" : ""); in |= nd == wjob; nd = nb[4]; }
                  fprintf(stderr, "[dm]   %#x 作業池等待：被等的作業%s x20 這個池的隊列前 %d 個裡\n", t->handle, in ? "在" : "不在", cnt); }
              else fprintf(stderr, "[dm]   %#x 作業池等待：線程不在等待循環自己的指令上，作業池沒取\n", t->handle); }
            #undef RD
            #undef HEX
        }
    }
    thread_resume(t->mport);
}
void svc_thread_dump(void) {
    static const char *kinds[] = {"運行/未在等", "鎖", "條件變量", "地址", "同步對象"};
    int nh = 0; for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    fprintf(stderr, "[dm] 句柄表佔用 %d / %d，累計建線程 %llu\n", nh, MAX_H, g_next_id - 0x51);
    for (thr_t *t = g_all; t; t = t->all) {
        module_t *m = module_of((void *)t->last_lr);
        // 線程名：SDK ThreadType+0x1a0 是名字指針（ThreadManager::SetInitialThreadNameUnsafe 0x34a2a4）；CreateThread 的 x2 = ThreadType*
        // Thread name: SDK ThreadType+0x1a0 is the name pointer (ThreadManager::SetInitialThreadNameUnsafe 0x34a2a4); x2 of CreateThread = ThreadType*
        char nb[32]; const char *nm = thr_name(t, nb, sizeof nb);
        double cpu = -1;
        if (t->mport && !t->exited) { thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS)
                cpu = bi.user_time.seconds + bi.user_time.microseconds / 1e6 + bi.system_time.seconds + bi.system_time.microseconds / 1e6; }
        char own[96] = ""; if (t->wkind == 1 && t->waddr) { uint32_t w = 0; vm_size_t g2 = 0;   // 鎖等待：鎖字 = 持有者句柄 | 0x40000000 | Lock wait: lock word = owner handle | 0x40000000
            if (!vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2)) snprintf(own, sizeof own, " 持有者 %#x", w & ~WAITERS_BIT); }
        if (t->wkind == 2 && t->waddr) { uint32_t w = 0, k = 0; vm_size_t g2 = 0;              // 條件等待：關聯鎖字與條件鍵的當前值（卡死現場缺這一項） | Condition wait: current values of the associated lock word and the condition key (the deadlock dump lacked this item)
            vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2); vm_read_overwrite(mach_task_self(), t->wkey, 4, (vm_address_t)&k, &g2);
            snprintf(own, sizeof own, " 鎖字 %#x 條件鍵 %#llx=%#x", w, t->wkey, k); }
        uint8_t raw[64]; vm_size_t g3 = 0;                                                   // 所等對象（讀寫鎖 / 條件變量所在結構）原始 64 字節：看計數與持有者 | Raw 64 bytes of the object being waited on (the structure holding the read-write lock / condition variable): to see the counts and the owner
        int have_raw = (t->wkind == 1 || t->wkind == 2 || t->wkind == 3) && t->waddr && !vm_read_overwrite(mach_task_self(), t->waddr & ~0xfull, 64, (vm_address_t)raw, &g3) && g3 == 64;
        char life[96]; uint64_t nowns = now_ns();
        if (t->exited) snprintf(life, sizeof life, " 建於拍 %u、退於拍 %u（%.1f 秒前）", t->born_tick, t->exit_tick, (nowns - t->exit_ns) / 1e9); else if (!t->born_ns) snprintf(life, sizeof life, " 進程啓動時就在"); else snprintf(life, sizeof life, " 建於拍 %u（%.1f 秒前）", t->born_tick, (nowns - t->born_ns) / 1e9);
        fprintf(stderr, "[dm] 線程 %#x 「%s」優先級 %d 核 %d 掩碼 %#llx CPU %.2fs svc %llu %s%s%s 等:%s addr=%#llx%s 最近 svc %#x lr=%s+%#llx\n", t->handle, nm, t->prio, t->core, (unsigned long long)t->mask,
                cpu, t->nsvc, t->exited ? "已退出" : "", t->started ? "" : "未啓動", life, kinds[t->wkind & 7], t->waddr, own, t->last_svc,
                m ? m->name : "?", m ? t->last_lr - (uint64_t)m->base : t->last_lr);
        if (have_raw) { char hx[200]; int q = 0; for (int i = 0; i < 64; i += 4) q += snprintf(hx + q, sizeof hx - q, "%08x ", *(uint32_t *)(raw + i));
                        fprintf(stderr, "[dm]   %#x 所等對象 %#llx 起 64 字節: %s\n", t->handle, t->waddr & ~0xfull, hx); }
        thr_bt(t);
    }
    // 宿主自建線程（預取、Granny 後台、圖形出圖、音頻等）：不在客體線程表裡，按 mach 線程枚舉，dladdr 符號化前 12 幀
    // Threads created by the host itself (prefetch, Granny background, graphics present, audio, etc.): not in the guest thread table, enumerated as mach threads, the first 12 frames symbolized with dladdr
    thread_act_array_t ths; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &ths, &nth) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < nth; i++) {
            int guest = 0; for (thr_t *t = g_all; t; t = t->all) if (t->mport == ths[i]) { guest = 1; break; }
            if (!guest && ths[i] != mach_thread_self() && thread_suspend(ths[i]) == KERN_SUCCESS) {
                arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(ths[i], ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
                    char line[1200]; int p = snprintf(line, sizeof line, "[dm] 宿主線程 %#x:", ths[i]);
                    uint64_t fp = __darwin_arm_thread_state64_get_fp(st), pc = __darwin_arm_thread_state64_get_pc(st);
                    for (int k = 0; k < 12 && p < (int)sizeof line - 80; k++) {
                        Dl_info di; module_t *m = module_of((void *)pc);
                        if (m) p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, pc - (uint64_t)m->base);
                        else if (dladdr((void *)pc, &di) && di.dli_sname) p += snprintf(line + p, sizeof line - p, " %s+%#lx", di.dli_sname, (unsigned long)(pc - (uint64_t)di.dli_saddr));
                        else p += snprintf(line + p, sizeof line - p, " %#llx", pc);
                        uint64_t fr[2]; vm_size_t g2 = 0;
                        if (!fp || vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
                        fp = fr[0]; pc = fr[1];
                    }
                    fprintf(stderr, "%s\n", line);
                }
                thread_resume(ths[i]);
            }
            mach_port_deallocate(mach_task_self(), ths[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ths, nth * sizeof *ths);
    }
}

// 準時睡到 dl（宿主單調鐘 ns）：macOS 的 nanosleep / mach_wait_until 都帶定時器鬆弛，實測 33 ms 睡成約 36.4 ms、5 ms 睡成 6.1 ms、1 ms 睡成 1.26 ms（任何 QoS 都一樣）。
// 銀河戰士 生存恐懼的音效線程按「33 ms 減本輪耗時」睡（main+0xf95b0），宿主上週期成了 36.9 ms；過場動畫按這條線程更新的音頻播放位置出幀，於是只有約 27 幀（Switch 上 30）。
// 每次只睡餘下時間的 3/4（鬆弛最多約 25%，不會睡過頭），循環到差 20 µs 以內：33 ms 的睡眠約 4 次系統調用、誤差幾十微秒，不空轉（上游 a9c56fd）
// Sleep until dl (host monotonic ns) on time: macOS nanosleep / mach_wait_until carry timer slack; measured, 33 ms sleeps take about 36.4 ms, 5 ms take 6.1 ms, 1 ms take 1.26 ms (the same at any QoS).
// Metroid Dread's sound thread sleeps "33 ms minus this round's work" (main+0xf95b0), and on the host its period became 36.9 ms; cutscenes render a frame whenever the audio play position that thread updates moves, so they ran at about 27 fps (30 on the Switch).
// Each step sleeps only 3/4 of the remaining time (the slack is at most about 25%, so it never oversleeps), looping until within 20 µs: a 33 ms sleep takes about 4 system calls with an error of tens of microseconds, and nothing spins (upstream a9c56fd)
static void sleep_precise(uint64_t dl) {
    for (int i = 0; i < 16; i++) {
        uint64_t now = now_ns(); if (now + 20000 >= dl) return;
        uint64_t rem = dl - now, s = rem < 100000 ? rem : rem - rem / 4;
        struct timespec ts = {(time_t)(s / 1000000000u), (long)(s % 1000000000u)}; nanosleep(&ts, 0);
    }
}
int svc_thread(int num, uint64_t *regs) {
    { thr_t *me = pthread_getspecific(g_keythr); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->nsvc++; } }
    if (g_flag.thr && num != 0x0B) { thr_t *me = pthread_getspecific(g_keythr); LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, regs[0], regs[1], regs[2], regs[3]); }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 參數, x3 棧頂, w4 優先級, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        // 核 -2（IdealCoreUseProcessValue）= 進程缺省理想核（本作 0 號），親和掩碼只含它——同 Horizon 內核。原先記成 -2，GetCurrentProcessorNumber 也答 -2，
        //   SMBW 的字體引擎按當前核號取每核引擎槽，拿 -2 當下標越界崩（main +0x107dbc0，SequenceBackgroundWorker 線程）
        // Core -2 (IdealCoreUseProcessValue) = the process's default ideal core (0 for this game), with an affinity mask of just that core - as in the Horizon kernel. It used to be recorded as -2 and GetCurrentProcessorNumber answered -2;
        //   SMBW's font engine picks its per-core engine slot by the current core number and crashed indexing with -2 (main +0x107dbc0, SequenceBackgroundWorker thread)
        t->prio = (int)regs[4]; t->core = (int)regs[5] >= 0 ? (int)regs[5] : 0; t->mask = 1ull << t->core;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主資源（客體經句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        if (g_flag.thr) LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle, t->entry, t->arg, t->stack_top, t->prio, t->core);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各類讓出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];
        uint64_t t0 = g_flag.bootprof ? now_ns() : 0;
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) sleep_precise(now_ns() + (uint64_t)ns);
        else {   // 讓出（0 / -1 / -2）。主線程的讓出次數與耗時記賬：遊戲渲染任務圖「等全部做完」是純輪詢 + 讓出（main+0x15ee898），量它佔一幀多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = pthread_getspecific(g_keythr);
            if (me && me->handle == 0x1001) { uint64_t y0 = now_ns(); sched_yield(); g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
            else sched_yield();
        }
        if (g_flag.bootprof) { uint64_t d = now_ns() - t0; dm_prof_add(PROF_SLEEP, d, 0); thr_t *me = pthread_getspecific(g_keythr); if (me && me->handle == 0x1001) dm_prof_add(PROF_MAIN_WAIT, d, 0); }
        return 1;
    }
    case 0x0C: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; return 1; }
    case 0x0D: { thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE; return 1; }
    case 0x0E: { thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩碼)：-2 = 用進程缺省（理想核 0、掩碼只含它）、-1 = 不在乎、-3 = 不改理想核（同 Horizon 內核）；宿主只記賬 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -2 = use the process default (ideal core 0, mask of just that core), -1 = don't care, -3 = leave the ideal core unchanged (as in the Horizon kernel); the host only keeps the bookkeeping
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; if (c == -2) { t->core = 0; t->mask = 1; } else if (regs[2]) t->mask = regs[2];
            // 理想核不在新掩碼裡（-2/-3 不改理想核、卻把線程釘到別的核）：同 Horizon，理想核改到掩碼裡最低的核。
            // 不改的話 GetCurrentProcessorNumber 對釘在 1 / 2 號核的線程也報 0 號，按核分隊列的任務系統（SMBW 的 BaseProcExecutor）會去拿 0 號核的活、1 / 2 號核的隊列沒人取
            // The ideal core is not in the new mask (-2/-3 keep the ideal core but pin the thread to other cores): as in Horizon, move the ideal core to the lowest core in the mask.
            // Otherwise GetCurrentProcessorNumber reports core 0 even for threads pinned to cores 1 / 2, and a job system with per-core queues (SMBW's BaseProcExecutor) takes core 0's work while nobody drains the queues of cores 1 / 2
            if (t->mask && (t->core < 0 || !(t->mask >> t->core & 1))) t->core = __builtin_ctzll(t->mask); }
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: { int c = cur()->core; regs[0] = (uint64_t)(uint32_t)(c >= 0 ? c : 0); return 1; }   // 當前核號總在 0..2（核號從不為負） | The current core number is always 0..2 (never negative)
    case 0x32: {  // SetThreadActivity(w0 線程, w1 0 可運行 / 1 暫停)：見上「暫停線程」 | SetThreadActivity(w0 thread, w1 0 runnable / 1 paused): see "Pausing threads" above
        thr_t *me = cur(); int act = (int)regs[1];
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (t == me) r = R_BAD_STATE;
        else if (act == 1) {
            atomic_store(&t->pause_req, 1);
            if (!t->started || t->exited || atomic_load(&t->svc_regs)) t->paused = 1;
            uint64_t t0 = now_ns(); int warned = 0;
            while (!t->paused && !t->exited) {
                pthread_mutex_unlock(&G); pthread_kill(t->pt, SIGUSR2); pthread_mutex_lock(&G);
                if (t->paused || t->exited) break;
                struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 100000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&t->pcv, &G, &dl);
                if (!warned && now_ns() - t0 > 1000000000ull) { warned = 1; LOG("【紅】暫停線程 %#x 超過 1 秒還沒停住（一直在宿主代碼裡？）", t->handle); }
            }
        } else { atomic_store(&t->pause_req, 0); pthread_cond_broadcast(&t->pcv); }
        pthread_mutex_unlock(&G);
        { static int n; if (n++ < 4) LOG("SetThreadActivity %#x → %s（%s）", (uint32_t)regs[0], act ? "暫停" : "恢復", r ? "失敗" : "成功"); }
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x33: {  // GetThreadContext3(x0 出參 ThreadContext*, w1 線程)：只對已停住的線程 | GetThreadContext3(x0 out ThreadContext*, w1 thread): only for stopped threads
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[1], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (!t->paused) r = R_BAD_STATE;
        else ctx_fill(t, (hz_ctx_t *)regs[0]);
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x25: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本線程標籤) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 鎖地址, x1 條件鍵, w2 標籤, x3 超時) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        // 順序要緊：先置條件鍵 = 1，再放互斥鎖。SDK 的 InternalConditionVariable::Signal 在用戶態先讀條件鍵、為 0 就不發 svc 0x1D；
        // 用戶態搶互斥鎖也不經過 G。原先先 lock_handoff 後置鍵：放鎖後、置鍵前，別的線程可在用戶態搶到鎖、改完條件、讀到鍵 0 就不喚醒，
        // 本線程隨後掛上去永遠等不到（並行負載下資源表讀寫鎖 main+0x320e5e0 卡死：讀者計數 0、鎖空閒、寫者條件鍵 1、寫者仍在等）。
        // 掛進等待鏈在 block 裡、仍持 G：svc 0x1D 要取 G，不會落在這之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 單測：緊跟放鎖之後、掛鏈之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 僅單測 t_condvar 的負對照：還原修復前「先放鎖、後置鍵」的順序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超時：不重新拿鎖（用戶態自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 條件鍵, w1 個數，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按內核順序；轉為鎖等待或被喚醒後不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 類型, w2 值, x3 超時)：0 小於則等、1 小於則減一再等、2 相等則等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 類型, w2 值, w3 個數)：0 喚醒、1 相等則加一並喚醒、2 相等則按等待數改值並喚醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // 全部喚醒（個數 ≤ 0）且有人等時減 2，不是減 1（mesosphere KAddressArbiter::SignalAndModifyByWaitingCountIfEqual；Ryujinx、yuzu 同）。
                // 原先減 1：Unity（Engage）的作業柵欄按這個值判斷作業做完沒有，等待方提前放行，粒子幾何作業與主線程算的個數對不上，寫出界（索引寫進貼圖對象、寫空指針崩）。
                // Waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphere KAddressArbiter::SignalAndModifyByWaitingCountIfEqual; Ryujinx and yuzu agree).
                // It used to subtract 1: Unity's (Engage) job fences decide from this value whether jobs are done, waiters were let through early, and the particle geometry job disagreed with the main thread's counts and wrote out of bounds (indices into texture objects, writes to a null pointer).
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 寫端, w2 讀端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(寫端) / ClearEvent(任一端) / ResetSignal(讀端，未觸發報錯) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        // SignalEvent 只認寫端；ClearEvent 兩端都認；ResetSignal 只認讀端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄無效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下標; x1 句柄數組, w2 個數, x3 超時) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 線程) | CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件兩端都關才釋放；線程句柄關閉後槽位回收，對象在線程也退出後釋放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    }
    return 0;
}

// ---------- 服務層的 SystemEvent ----------
// 真機：服務把內核事件的讀端交給客戶端，客體經 SDK nn::os 等待。宿主：在客體線程上調 SDK 自己的
// CreateSystemEvent(跨進程=true)，SDK 經 SVC 0x45 建出內核事件；記下寫端句柄，之後任何線程（含宿主音頻線程）都可觸發。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(void *ev, int autoclear) {      // 必須在客體線程上調（要執行 SDK 代碼） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!pthread_getspecific(g_keythr)) die("hle_bind_system_event 須在客體線程上調用");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent 失敗 %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 綁定表滿");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(void *ev) {   // 未綁定的按手動清除即時綁定（只能在客體線程上發生） | An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) { hle_bind_system_event(ev, 0); pthread_mutex_lock(&g_se_lock); wh = se_find(ev); pthread_mutex_unlock(&g_se_lock); }
    pthread_mutex_lock(&G);
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent 寫端句柄失效 %#x", wh);
    e->signaled = 1; wake_sync_waiters();
    pthread_mutex_unlock(&G);
}
