// DM_HLECOUNT=<輸出文件，可含 %d = 進程號>（Mac 診斷，HLE 覆蓋審計用）：統計每條 HLE 被客體調用的次數，退出時寫 TSV「類別 名字 次數」（含 0 次）。
// 做法：裝載時把客體對宿主實現的引用（強制接管 / HLE 表 / libc 直通 / 未實現樁）解析到一個計數樁：
//   ldr x16, =&計數; movz x17, #1; stadd x17, [x16]; ldr x16, =實現; br x16
// 只動調用間可破壞的 x16 / x17，參數與返回值原樣。樁寫在可寫頁裡，裝載結束封成只讀可執行（hlecount_seal）。
// svc 按號單獨計（hle_svc.c 分派入口調 hlecount_svc）。iOS 不可用（運行時寫出的代碼不能執行）。
// DM_HLECOUNT=<output file, may contain %d = process id> (Mac diagnostic, for HLE coverage audit): count how many times each HLE is called by the guest and write a TSV "category name count" on exit (including 0 counts).
// Method: at load time, resolve guest references to host implementations (forced takeover / HLE table / libc passthrough / unimplemented stubs) to a counting stub:
//   ldr x16, =&count; movz x17, #1; stadd x17, [x16]; ldr x16, =implementation; br x16
// Touches only x16 / x17, which may be clobbered between calls; arguments and return values pass through unchanged. Stubs are written in a writable page and sealed read-only executable when loading ends (hlecount_seal).
// svc is counted separately by number (the hle_svc.c dispatch entry calls hlecount_svc). Unavailable on iOS (code written at runtime cannot execute).
#include "dm.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#if !TARGET_OS_IPHONE
#define HC_MAX 8192
static struct { const char *kind, *name; void *fn; _Atomic uint64_t n; } g_hc[HC_MAX];
static unsigned g_nhc; static uint32_t *g_hcode; static int g_hon = -1, g_hsealed; static const char *g_hpath;
static _Atomic uint64_t g_hsvc[256];
static int hc_on(void) { if (g_hon < 0) { g_hpath = getenv("DM_HLECOUNT"); g_hon = g_hpath != NULL; } return g_hon; }
static void hc_write(void) {
    char pth[1024]; snprintf(pth, sizeof pth, g_hpath, (int)getpid());   // 路徑裡的 %d 換成進程號（並行多局各寫一份） | %d in the path is replaced with the process id (parallel runs each write their own copy)
    FILE *f = fopen(pth, "w"); if (!f) { LOG("【紅】DM_HLECOUNT 寫不了 %s", pth); return; }
    fprintf(f, "類別\t名字\t次數\n");
    for (unsigned i = 0; i < g_nhc; i++) fprintf(f, "%s\t%s\t%llu\n", g_hc[i].kind, g_hc[i].name, (unsigned long long)atomic_load(&g_hc[i].n));
    for (int k = 0; k < 256; k++) if (atomic_load(&g_hsvc[k])) fprintf(f, "SVC\t%#x\t%llu\n", k, (unsigned long long)atomic_load(&g_hsvc[k]));
    fclose(f); LOG("DM_HLECOUNT：%u 條入口 → %s", g_nhc, pth);
}
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    if (!hc_on() || !fn) return fn;
    if (g_hsealed) { static int once; if (!once++) LOG("【紅】DM_HLECOUNT：封頁後還有解析（%s），此後的入口不計數", name); return fn; }
    for (unsigned i = 0; i < g_nhc; i++) if (g_hc[i].fn == fn && !strcmp(g_hc[i].name, name)) return g_hcode + 12 * i;   // 同名同實現復用一個樁 | Same name and same implementation reuse one stub
    if (g_nhc == HC_MAX) die("DM_HLECOUNT 入口超過 %d", HC_MAX);
    if (!g_hcode) { g_hcode = mmap(NULL, HC_MAX * 48, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (g_hcode == MAP_FAILED) die("DM_HLECOUNT mmap"); atexit(hc_write); }
    unsigned i = g_nhc++; g_hc[i].kind = kind; g_hc[i].name = name; g_hc[i].fn = fn;
    uint32_t *w = g_hcode + 12 * i;                                  // 48 字節：5 條指令 + 1 空 + 兩個 8 字節字面量（偏移 0x18 / 0x20） | 48 bytes: 5 instructions + 1 empty + two 8-byte literals (offsets 0x18 / 0x20)
    w[0] = 0x58000000 | ((0x18 / 4) << 5) | 16;                      // ldr x16, [pc+0x18]   → &計數 | ldr x16, [pc+0x18]   → &count
    w[1] = 0xD2800031;                                                // movz x17, #1
    w[2] = 0xF8200000 | (17u << 16) | (16u << 5) | 31;               // stadd x17, [x16]（ldadd 目標 xzr） | stadd x17, [x16] (ldadd with destination xzr)
    w[3] = 0x58000000 | (((0x20 - 0xc) / 4) << 5) | 16;              // ldr x16, [pc+0x14]   → 實現 | ldr x16, [pc+0x14]   → implementation
    w[4] = 0xD61F0200;                                                // br x16
    w[5] = 0xD503201F;                                                // nop
    uint64_t a = (uint64_t)&g_hc[i].n, b = (uint64_t)fn; memcpy(w + 6, &a, 8); memcpy(w + 8, &b, 8);
    return w;
}
void hlecount_seal(void) {
    if (!hc_on() || !g_hcode) return;
    if (mprotect(g_hcode, HC_MAX * 48, PROT_READ | PROT_EXEC)) die("DM_HLECOUNT mprotect");
    g_hsealed = 1;
    sys_icache_invalidate(g_hcode, HC_MAX * 48);
    LOG("DM_HLECOUNT：計數樁 %u 個已封", g_nhc);
}
void hlecount_svc(int num) { if (g_hon > 0 && num >= 0 && num < 256) atomic_fetch_add(&g_hsvc[num], 1); }
void hlecount_flush(void) { if (hc_on() && g_hcode) hc_write(); }   // 優雅退出路徑（SIGTERM / 場景斷開）也寫一份 | Also write a copy on the graceful exit path (SIGTERM / scene disconnect)
#else
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
void hlecount_seal(void) {}
void hlecount_svc(int num) { (void)num; }
void hlecount_flush(void) {}
#endif
