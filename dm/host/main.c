// 引導：裝模塊後，照 SDK 裡 nn::init::Start（sdk+0x2b0470）的真實順序初始化 SDK 運行時，再進 nnMain。
// Bootstrap: after loading the modules, initialize the SDK runtime in the real order of nn::init::Start (sdk+0x2b0470) in the SDK, then enter nnMain.
#include "dm.h"
#include "front.h"
#include <stdatomic.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
void probe_install(void); void ticks_install(void); void hid_keys_init(void); void tact_prefetch_start(const char *outdir); void tact_chk_init(void); void stubs_report(void); void crash_install(void); void guest_thread_init(void); uint32_t svc_thread_main_init(void);
typedef void (*fn0)(void); typedef void (*fn2)(uint64_t, uint64_t); typedef void (*fnp)(void *);
static void *req(const char *n){ void *p=guest_sym(n); if(!p) die("缺符號 %s", n); return p; }

// 遊戲參數：argv[0] 固定為 "game"，"--" 之後的原樣追加（真機由 nn::init 從進程參數區取，經 SetHostArgc/Argv 交給 SDK）
// Game arguments: argv[0] is fixed to "game", and whatever follows "--" is appended as is (on a real device nn::init takes them from the process argument area and hands them to the SDK through SetHostArgc/Argv)
static char *g_argv[64] = {"game"}; static int g_argc = 1;

void crash_sample_self(void);
static void guest_main(void) {
    guest_thread_init();
    { void inflate_bench_maybe(void); inflate_bench_maybe(); }   // DM_INFLATE_BENCH 測試模式（診斷用，跑完退出） | DM_INFLATE_BENCH test mode (for diagnostics, exits when done)
    crash_sample_self();   // DM_SAMPLE 採樣（診斷用） | DM_SAMPLE sampling (for diagnostics)
    uint32_t hmain = svc_thread_main_init();   // 主線程句柄：nnosInitialize 的 x0（存入 sdk 全局，ThreadManagerImplByHorizon 取用）；x1 = 參數區指針 | Main thread handle: x0 of nnosInitialize (stored in an sdk global, used by ThreadManagerImplByHorizon); x1 = pointer to the argument area
    // 逐條照抄 Start：_init_libc1 = __nnmusl_init_locks（把 stdio 等鎖初始化成遞歸鎖，略過會在 putc 嵌套加鎖時自鎖死）+ __init_ssp。
    // rtld 傳入的三個回調：A 只置 rtld 標誌（略）、B = 各模塊 DT_INIT（nnMain 前）、C = 析構（不做）
    // Copy Start step by step: _init_libc1 = __nnmusl_init_locks (initializes the stdio etc. locks as recursive locks; skipping it self-deadlocks on nested locking in putc) + __init_ssp.
    // The three callbacks passed in by rtld: A only sets an rtld flag (skipped), B = each module's DT_INIT (before nnMain), C = destructors (not done)
    ((fn0)req("_init_libc0"))();
    LOG(">>> nnosInitialize"); ((fn2)req("nnosInitialize"))(hmain, 0);
    ((fn0)req("_init_libc1"))();
    LOG(">>> nninitInitializeSdkModule"); ((fn0)req("nninitInitializeSdkModule"))();
    LOG(">>> nninitStartup"); ((fn0)req("nninitStartup"))();
    ((fn0)req("_init_libc2"))();
    dm_prof_mark("② SDK 初始化（nnosInitialize → nninitStartup）");
    run_module_inits();   // rtld 回調 B：各模塊 DT_INIT（全局構造函數） | rtld callback B: each module's DT_INIT (global constructors)
    dm_prof_mark("③ 各模塊 init_array");
    ((fnp)req("_ZN2nn2os11SetHostArgcEi"))((void *)(uint64_t)g_argc);
    ((fnp)req("_ZN2nn2os11SetHostArgvEPPc"))(g_argv);
    { void dm_guest_spawn_selftest(void); dm_guest_spawn_selftest(); }   // DM_GUEST_SPAWN_TEST 宿主客體線程自檢（hle_guestthread.c） | DM_GUEST_SPAWN_TEST self-test of host-created guest threads (hle_guestthread.c)
    { void dm_padmode_boot(void); dm_padmode_boot(); }   // 觸屏覆蓋層在：佈局前綴改指觸屏版（dm_padmode.c），要在遊戲建面板之前 | Touch overlay present: repoint the layout prefixes to the touch version (dm_padmode.c); must happen before the game builds its panels
    LOG(">>> nnMain"); ((fn0)req("nnMain"))();
    LOG("nnMain returned");
}

struct dm_flags g_flag;
uint64_t g_t0_ns;
static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.movie = !!getenv("DM_MOVIE"); g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}

// 啓動時間線零點 = 進程創建時刻（sysctl KERN_PROC_PID 的 p_starttime，牆鐘）換算到 now_ns 單調時基；Mac / iOS 同口徑
// Zero point of the boot timeline = process creation time (p_starttime from sysctl KERN_PROC_PID, wall clock) converted to the now_ns monotonic time base; same definition on Mac / iOS
static uint64_t proc_start_mono_ns(void) {
    struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    uint64_t mono = now_ns(); struct timeval tv; gettimeofday(&tv, NULL);
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || !n) return mono;
    int64_t age = (int64_t)(tv.tv_sec - kp.kp_proc.p_starttime.tv_sec) * 1000000000ll + (tv.tv_usec - kp.kp_proc.p_starttime.tv_usec) * 1000ll;
    return age > 0 ? mono - (uint64_t)age : mono;
}

// 宿主裝載（與 UIKit / Metal 無關）：讀 NSO、重定位、強制接管、探針 / 拍門 / 按鍵、預取起跑。缺省在 main 裡同步跑；
// iOS 前端可在 App 的 main() 一開頭調 dm_host_preload_start(同一組參數) 讓它在後台線程先跑，與 UIKit 啓動鏈並行（⓪ + ① ≈ max(UIKit 鏈, 宿主裝載)）。
// Host loading (independent of UIKit / Metal): read the NSO, relocate, forced take-over, probes / tick gate / keys, start prefetch. By default it runs synchronously in main;
// the iOS front end can call dm_host_preload_start(same arguments) at the very start of the App's main() so that it runs first on a background thread, in parallel with the UIKit startup chain (⓪ + ① ≈ max(UIKit chain, host loading)).
static pthread_key_t g_race_keys[512];
static unsigned g_race_n, g_race_want;
static void *keyrace_thread(void *unused);
static void host_preload(int argc, char **argv) {
    g_t0_ns = proc_start_mono_ns();
    flags_init();
    dm_prof_mark("⓪ 進程創建 → 宿主 main（dyld）");
    if (argc < 2) die("usage: dmhost <dir> [mod...] [-- 遊戲參數...]");
    // 時區：遊戲把絕對時刻換成本地時刻走兩條路，都落到宿主 libc——nn::time::ToCalendarTime（hle_time.c，localtime_r）和客體 libc 的
    // localtime_r / mktime（hle_table.c 直連宿主）。所以宿主跟系統時區走，原版界面（編年史發現時刻等）才是本機時間。
    // 只有確定性對拍（DM_FIXED_TIME）釘成 UTC：同一份回放在本機（東八區）和 mini（東九區）上要換出同樣的日曆時刻
    // Time zone: the game converts absolute time to local time along two paths, both ending in the host libc -- nn::time::ToCalendarTime (hle_time.c, localtime_r) and the guest libc's
    // localtime_r / mktime (hle_table.c wires them straight to the host). So only when the host follows the system time zone do the original game's screens (chronicle discovery time etc.) show this machine's time.
    // Only deterministic comparison (DM_FIXED_TIME) pins it to UTC: the same replay must convert to the same calendar time on this machine (UTC+8) and on the second Mac (UTC+9)
    if (getenv("DM_FIXED_TIME")) setenv("TZ", "UTC", 1);
    crash_install();
    const char *race = getenv("DM_TEST_KEYRACE");
    if (race && *race) {
        g_race_want = (unsigned)strtoul(race, NULL, 10);
        if (g_race_want > 512) g_race_want = 512;
        pthread_t th;
        if (pthread_create(&th, NULL, keyrace_thread, NULL)) die("DM_TEST_KEYRACE 起線程失敗");
        pthread_detach(th);
    }
    { void dm_term_install(void); dm_term_install(); }   // SIGTERM 優雅退出（hle_lifecycle.c） | SIGTERM graceful exit (hle_lifecycle.c)
    { void crash_set_dump_hook(void (*)(void)); void svc_thread_dump(void); crash_set_dump_hook(svc_thread_dump); }   // 崩潰時導出客體線程現場 | Dump guest thread state on a crash
    const char *names[MAX_MOD]; int n = 0;
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--") && n < MAX_MOD; i++) names[n++] = argv[i];
    if (i < argc && !strcmp(argv[i], "--")) for (i++; i < argc && g_argc < 63; i++) g_argv[g_argc++] = argv[i];
    if (!n) { static const char *def[] = DM_GAME_MODULES; for (int k = 0; k < DM_GAME_NMOD && n < MAX_MOD; k++) names[n++] = def[k]; }   // 缺省模塊表取自遊戲檔案 modules（馬力歐 main sdk） | default module list from the game profile's modules (Mario: main sdk)
    load_modules(argv[1], names, n);
    uint64_t ta = now_ns();
    probe_install();   // DM_PROBE 探針（診斷用） | DM_PROBE probes (for diagnostics)
    ticks_install();   // DM_TICKS 逐拍狀態哈希（拍入口觀測門） | DM_TICKS per-tick state hash (tick-entry observation gate)
    { void target_rank_install(void); target_rank_install(); }   // 觸屏版自動選怪 Boss > 精英 > 普通（hle_target.c） | Touch version auto-targeting Boss > elite > normal (hle_target.c)
    hid_keys_init();   // DM_KEYS 腳本輸入（按 UI 消息驅動的步驟要在消息出現前訂閱） | DM_KEYS scripted input (steps driven by UI messages must subscribe before the message appears)
    uint64_t tb = now_ns();
    tact_chk_init();                // DM_TACTCHK 逐塊記錄（到選角停記） | DM_TACTCHK per-block log (logging stops at character select)
    tact_prefetch_start(argv[1]);   // 啓動塊後台預解碼（缺省開：產物目錄 bootlist.tsv；DM_TACT_PREFETCH=0 關） | Background pre-decode of boot blocks (on by default: bootlist.tsv in the build output directory; DM_TACT_PREFETCH=0 turns it off)
    uint64_t tc = now_ns();
    stubs_report();
    if (g_flag.bootprof) LOG("① 細分（裝載後）：探針 / 拍門 / 按鍵 %.1f ms、預取啓動（讀清單）%.1f ms、樁報告 %.1f ms", (tb - ta) / 1e6, (tc - tb) / 1e6, (now_ns() - tc) / 1e6);
    dm_prof_mark("① 宿主裝載（讀 NSO / 重定位 / 強制接管）");
    { void loader_hash_log(void); loader_hash_log(); }   // DM_LOADHASH=1：裝載結果的地址無關哈希（loader.c） | DM_LOADHASH=1: address-independent hash of the load result (loader.c)
}
static struct { int argc; char **argv; pthread_t th; _Atomic int started; } g_pre;
static void *preload_thread(void *a) { (void)a; host_preload(g_pre.argc, g_pre.argv); return NULL; }
static void *keyrace_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < g_race_want; i++) if (pthread_key_create(&g_race_keys[g_race_n++], NULL)) break;
    return NULL;
}
// 提前起跑宿主裝載。調用時機：進程裡第一件事之一（iOS：App 的 main() 進 UIApplicationMain 之前），須先設好裝載要用的環境變量
// （DM_ROMFS / DM_SAVE / DM_TEMP 等與 cwd——裝載與預取起跑讀它們）；參數須與隨後 dm_main 的參數逐項相同（不同即報紅退出）。
// 只能調一次；不調則 main 內同步裝載，行為與調用前完全一致。不依賴 UIKit / Metal。
// Start host loading early. When to call: as one of the first things in the process (iOS: in the App's main() before entering UIApplicationMain); the environment variables loading needs must already be set
// (DM_ROMFS / DM_SAVE / DM_TEMP etc. and the cwd -- loading and the prefetch start read them); the arguments must be identical, item by item, to those of the later dm_main (a difference reports a failure and exits).
// May be called only once; if not called, loading is synchronous inside main and behavior is exactly as before this call existed. Does not depend on UIKit / Metal.
void dm_host_preload_start(int argc, char **argv) {
    if (atomic_exchange(&g_pre.started, 1)) return;
    g_pre.argc = argc; g_pre.argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) g_pre.argv[i] = strdup(argv[i]);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    if (pthread_create(&g_pre.th, &a, preload_thread, NULL)) die("dm_host_preload_start：起線程失敗");
}
int main(int argc, char **argv) {
    if (getenv("DM_PRELOAD")) dm_host_preload_start(argc, argv);   // Mac 上驗證提前起跑的路徑（iOS 由 App 的 main() 調） | Exercise the early-start path on Mac (on iOS it is called from the App's main())
    if (atomic_load(&g_pre.started)) {
        pthread_join(g_pre.th, NULL);
        int same = argc == g_pre.argc; for (int i = 0; same && i < argc; i++) same = !strcmp(argv[i], g_pre.argv[i]);
        if (!same) die("【紅】dm_host_preload_start 的參數與 main 不同");
        void crash_thread_init(void); crash_thread_init();   // 信號備用棧是每線程的：裝載在別的線程上裝過，本線程補上 | The signal alternate stack is per thread: loading installed it on another thread, so add it for this thread
        LOG("宿主裝載已在後台提前完成（dm_host_preload_start）");
    } else host_preload(argc, argv);
    front_run(guest_main);
    return 0;
}
