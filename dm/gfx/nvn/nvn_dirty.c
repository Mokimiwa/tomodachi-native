// 客體內存寫保護髒頁跟蹤：預排布貼圖（CPU 寫、GPU 讀）的池頁上傳後設只讀，遊戲一寫即缺頁 → 標髒、解保護；
// 使用前查覆蓋頁的寫代號，比上傳時新就重傳。NVN 語義：貼圖描述符指向池內存，GPU 每次讀的是當前內容
// （實測遊戲會把流式素材寫進仍被採樣的貼圖存儲，不重新登記）。
// 信號處理約束：缺頁路徑只用原子操作與 mprotect，不分配、不加鎖。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>

// 頁狀態與 mprotect 的次序（取自 d2r-mobile ef9b1ff13）：改保護的一方先把狀態改成 BUSY，mprotect 落地後才改成終態，別的線程見 BUSY 就等。
//   設只讀：NONE → BUSY → mprotect(只讀) → PROT；解保護：PROT → BUSY → mprotect(可寫) → NONE。
// 不變式：狀態是 NONE 的頁一定可寫、是 PROT 的頁一定只讀。原先設只讀是「先記 PROT、再 mprotect」，這中間另一線程的寫前解保護
// （影片解碼線程往貼圖存儲寫一幀、讀盤讀進客體緩衝）會把狀態改回 NONE，隨後 mprotect 落地 ⇒ 頁只讀而狀態 NONE：遊戲線程寫它時缺頁處理只回
// 「重執行」、永遠空轉；宿主用系統調用往裡讀時 prep_write 看 NONE 跳過，系統調用直接 EFAULT（t_dirty_race 復現）。
// BUSY 最長持續一次 mprotect（設只讀時再加幾次原子寫）。等待有上限 BUSY_WAIT_MS，超限報紅：缺頁處理裡超限按「不是我們的頁」返回，交給崩潰處理如實報崩
// Order of page state and mprotect (from d2r-mobile ef9b1ff13): the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY.
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, one whose state is PROT is always read-only. Setting read-only used to be "record PROT, then mprotect"; in between, another thread's unprotect-before-write
// (the movie decoder thread writing a frame into texture storage, a disk read into a guest buffer) could set the state back to NONE, then the mprotect landed => the page read-only with state NONE: a game thread writing it got only
// "re-execute" from the fault handler and spun forever; a host system call reading into it saw NONE in prep_write, skipped, and failed with EFAULT (reproduced by t_dirty_race).
// BUSY lasts at most one mprotect (plus a few atomic writes when setting read-only). The wait is capped at BUSY_WAIT_MS and reports red beyond it: in the fault handler it then returns "not our page" so the crash handler reports truthfully
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// 跟蹤區間 = 各內存池（MemoryPoolInitialize 登記）。池銷毀後復用 live=0 的槽，信號處理裡無鎖掃描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 4096   // 原為 256：內存池多於它時超出的池不跟蹤，裡面的貼圖每次取用都當成髒的重傳（暗黑實測峰值見內存讀數行的「內存池」；塞爾達約 300 個，踩過）。掃描只到已用的槽數，容量大不增加開銷 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing
// 區間按宿主頁（16 KB）取整，客體內存池只按 4 KB 對齊：相鄰的池會共用首尾頁，同一個地址可能落在多個在冊區間裡。
// 所以：判「貼圖在不在跟蹤」要找能整段裝下它的區間；缺頁 / 宿主寫前要把覆蓋該頁的每個區間都解保護並記代號；銷池按池的確切起點 mem 認區間。
// （原先一律取第一個包含該地址的區間：SMBW 兩張 ASTC 6x5 貼圖的池首頁與前一個小池共頁，被判成未跟蹤，每次取用都重傳，每幀約 6 MB）
// Ranges are rounded to host pages (16 KB) but guest memory pools are only 4 KB aligned: adjacent pools share their first / last pages, so one address can fall in several registered ranges.
// So: "is this texture tracked" looks for a range that holds its whole span; a fault / host pre-write unprotects and stamps every range covering that page; pool destruction identifies its range by the pool's exact start mem.
// (Previously the first range containing the address was always taken: two SMBW ASTC 6x5 textures whose pool's first page is shared with a small preceding pool were judged untracked and re-uploaded on every use, about 6 MB per frame)
typedef struct { uintptr_t base, end, mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; } range_t;
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // 在冊的池數與峰值（內存讀數行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 銷池（客體線程，可能併發）互斥選槽；缺頁處理路徑不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模塊的 mprotect：調用次數、覆蓋頁數、耗時（mach 時基，缺頁路徑裡也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static void mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

static mach_timebase_info_data_t g_tb;   // 首次登記池時取好（缺頁路徑裡只做乘除） | obtained when the first pool is registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// 等這一頁離開 BUSY。返回 1 = 已離開；0 = 等過了上限（報紅一次）。信號處理裡可用：只有原子讀、mach_absolute_time、sched_yield
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("【紅】髒頁跟蹤：頁 %p 停在「忙」超過 %d 毫秒（改保護的線程沒走完），不再等", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&g_r[i].live, memory_order_acquire); }
// 能整段裝下 [a, a+len) 的在冊區間（貼圖的跟蹤 / 保護 / 查髒都用它） | The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (live_at(i) && a >= g_r[i].base && a + len <= g_r[i].end) return &g_r[i];
    return NULL;
}
static void unprotect_page(range_t *r, uintptr_t p) {   // 調用方已把狀態從 PROT 改成 BUSY | The caller has already changed the state from PROT to BUSY
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    atomic_store(&r->gen[p], atomic_fetch_add(&g_now, 1) + 1);
    atomic_store(&r->state[p], PG_NONE);
}
// 解開一個區間裡的一頁：受保護就解並記代號；別的線程正在改它的保護就等它改完再看——對方若是在設只讀，等完這頁是 PROT，照樣得解
// （寫前解保護走不到缺頁處理，必須在這裡解掉）。返回 1 = 這一頁是本模塊管的；0 = 不是，或等「忙」超時
// Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is changing its protection, wait for it to finish and look again -- if the other side was setting read-only, the page is PROT after the wait and still has to be unprotected
// (an unprotect-before-write never reaches the fault handler, so it must be done here). Returns 1 = this page is managed by this module; 0 = it is not, or the wait on BUSY timed out
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st = PG_PROT;
        if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); if (count) atomic_fetch_add(count, 1); return 1; }
        if (st != PG_BUSY) return atomic_load(&r->gen[p]) != 0;   // 已是 NONE：剛被別的線程解掉（缺頁發生在解之前）就重執行；從未保護過的頁不是我們的 | already NONE: just unprotected by another thread (the fault happened before the unprotect) -> re-execute; pages never protected are not ours
        if (!busy_wait(r, p)) return 0;
    }
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr; int ours = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++)   // 共頁的每個區間都要解：只解一個的話，另一個區間的狀態仍是「受保護」，之後的寫不再缺頁、被漏掉 | Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
        if (live_at(i) && a >= g_r[i].base && a < g_r[i].end) ours |= release_page(&g_r[i], (a - g_r[i].base) >> g_shift, &g_faults);
    return ours;
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len; int n = atomic_load(&g_nr);
    if (!len) return;
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!live_at(i) || e <= r->base || a >= r->end) continue;
        uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
        for (uintptr_t p = (lo - r->base) >> g_shift, q = (hi - 1 - r->base) >> g_shift; p <= q; p++) release_page(r, p, &g_preps);
    }
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【紅】髒頁跟蹤區間已滿（%d），池 %p 不跟蹤", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("髒頁表分配失敗（%zu 頁）", np);
        // 舊數組不 free：信號處理裡可能仍有極短暫的陳舊讀者，洩漏量 = 每頁 9 字節。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->mem = (uintptr_t)mem; r->state = state; r->gen = gen;
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// 池銷毀：先把仍受保護的頁還原成可寫（池內存歸還遊戲後可能另作他用），再標失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && g_r[i].mem == (uintptr_t)mem) r = &g_r[i];   // 按池的確切起點認：共頁的鄰池取整後的 base 可能相同 | Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// 設只讀：先把頁標成 BUSY，mprotect 落地後才標 PROT（次序與不變式見文件頭的狀態說明）
// Set read-only: mark the pages BUSY first and mark them PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r) return;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // 連續置成 PROT 的頁合成一次 mprotect（逐頁調用在首傳大貼圖時一幀上千次；選角頁啓動幀 5：4888 次）。
    // 已是 PROT / BUSY 的頁斷開連續段：不重復保護，也不碰別的線程正在改的頁
    // Consecutive pages are merged into one mprotect (per-page calls reach thousands per frame on the first upload of large textures; character select page startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break the consecutive run: do not protect again, and do not touch pages another thread is changing
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ); for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT); open = 0; }
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = find_span((uintptr_t)addr, len);
    if (!r) return 1;   // 未跟蹤時保守視為髒，寧可多傳也不顯示舊內容 | When untracked, conservatively treat as dirty: better to upload more than to show stale content
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // 上傳前最後一次寫的代號 ≤ 戳；之後的寫必 > 戳 | The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
