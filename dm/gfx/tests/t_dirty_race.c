// 髒頁跟蹤「設只讀」與「寫前解保護」的次序競態（取自 d2r-mobile ef9b1ff13 的 t_dirty_race.c，按本倉庫的跟蹤表改了樁）。
// 現場：線程甲 nvn_dirty_protect 已把頁的狀態記成「受保護」、mprotect 還沒落地；線程乙這時來 prep_write（宿主往客體內存寫之前的解保護，
// 例如影片解碼線程把一幀寫進貼圖存儲、讀盤把文件讀進客體緩衝）。舊實現：乙把狀態改回「無」，隨後甲的 mprotect 落地 ⇒ 頁只讀而狀態「無」——
//   宿主再往裡讀文件：prep_write 看狀態「無」跳過，系統調用不走缺頁處理，直接 EFAULT；遊戲線程直接寫：缺頁處理只回「重執行」，永遠空轉。
// 造法：把 nvn_dirty.c 裡的 mprotect 換成這裡的 t_mprotect——甲設只讀的那一次調用落地之前，先放乙進來並等 100 毫秒，次序是確定的。
// 期望值不靠被測實現：乙的 prep_write 返回之後，這一頁必須真的可寫（往裡 read 16 字節得 16；直接寫一個字節不卡、讀回是寫進去的值），
// 且這次寫要被記成髒（nvn_dirty_since 對設只讀之前取的戳返回 1）。
// Ordering race between "set read-only" and "unprotect before write" in dirty-page tracking (from d2r-mobile ef9b1ff13's t_dirty_race.c, stubs adapted to this repository's tracking table).
// Scene: thread A, in nvn_dirty_protect, has already recorded the page state as "protected" while its mprotect has not landed yet; thread B now arrives with prep_write (the unprotect done before the host writes into guest memory,
// for example the movie decoder thread writing a frame into texture storage, or a disk read into a guest buffer). Old implementation: B sets the state back to "none", then A's mprotect lands => the page is read-only while its state is "none" --
//   when the host reads a file into it again: prep_write sees state "none" and skips, the system call does not go through the page-fault handler and fails with EFAULT; when a game thread writes directly: the page-fault handler only returns "re-execute" and spins forever.
// How it is staged: the mprotect in nvn_dirty.c is replaced by t_mprotect here -- before the call with which A sets read-only lands, B is let in and given 100 ms, so the order is deterministic.
// Expected values do not depend on the implementation under test: after B's prep_write returns, this page must really be writable (reading 16 bytes into it gives 16; writing one byte directly does not hang and reads back as written),
// and this write must be recorded as dirty (nvn_dirty_since returns 1 for a stamp taken before the page was set read-only).
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-58s 期望 %-4lld → 真結果 %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static uint8_t *g_page; static _Atomic int g_armed, g_go, g_b_done;
static void *thread_b(void *arg) { (void)arg; while (!atomic_load(&g_go)) usleep(200); prep_write(g_page, 16); atomic_store(&g_b_done, 1); return NULL; }
int t_mprotect(void *a, size_t n, int prot) {
    if (prot == PROT_READ && atomic_exchange(&g_armed, 0)) { atomic_store(&g_go, 1); usleep(100000); }   // 甲的 mprotect 落地前：放乙進來，等它跑到頭或卡在等待上 | Before A's mprotect lands: let B in and wait until it runs to the end or gets stuck waiting
    return mprotect(a, n, prot);
}

int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize();
    g_page = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(g_page, 2 * pg);
    uint64_t stamp = nvn_dirty_now();
    pthread_t tb; pthread_create(&tb, NULL, thread_b, NULL);
    atomic_store(&g_armed, 1);
    nvn_dirty_protect(g_page, 2 * pg);                       // 甲：置狀態 → （乙插進來）→ mprotect 落地 | A: set the state -> (B cuts in) -> mprotect lands
    for (int i = 0; i < 2000 && !atomic_load(&g_b_done); i++) usleep(1000);
    EXPECT("乙的寫前解保護在 2 秒內返回", 1, atomic_load(&g_b_done));
    if (!atomic_load(&g_b_done)) { printf("t_dirty_race：乙沒返回，後面的項不測\n"); return 1; }
    pthread_join(tb, NULL);
    int fd = open("/dev/zero", O_RDONLY); g_page[pg] = 0;     // 第二頁沒人解過保護，這一寫走缺頁處理（對照：正常路徑是通的） | Nobody has unprotected the second page, so this write goes through the page-fault handler (control: the normal path works)
    ssize_t r = read(fd, g_page, 16);                        // 乙接下來要做的那次宿主寫：系統調用直接寫進頁裡 | The host write that B is about to make: a system call writing straight into the page
    EXPECT("乙解完保護後往這頁讀文件：讀到的字節數 = 請求的 16", 16, r);
    if (r == 16) { ((volatile uint8_t *)g_page)[1] = 0x5a; EXPECT("直接寫這頁不卡、讀回是寫進去的值", 0x5a, g_page[1]); }
    else { printf("  （read 失敗 errno=%d；頁只讀而狀態為「無」，直接寫會在缺頁處理裡空轉，這一項不執行）\n", errno); g_fail++; }
    EXPECT("這次寫被記成髒（對設只讀前的戳）", 1, nvn_dirty_since(g_page, 16, stamp));
    close(fd);
    printf("t_dirty_race：%s\n", g_fail ? "不通" : "全通"); return g_fail != 0;
}
