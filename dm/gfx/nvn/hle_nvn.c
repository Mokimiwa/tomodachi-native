// NVN 接口層：接管 main 導入的 nvnBootstrapLoader，sdk 裡的 NVN 驅動整塊不執行。
// 523 個 NVN 名字一律返回計數跳板（nvn_thunks.S），跳板再轉到這裡的實現：
//   · 有語義的（設備 / 內存池 / 貼圖與採樣器 / 隊列 / 窗口 / 同步 / 事件 / 命令緩衝內存）按 NVN 語義實現，對象是真的；
//   · 只改 GPU 狀態、沒有 CPU 可見結果的（狀態對象、綁定、繪製、清除…）錄成命令，提交時回放；
//   · 其餘（main 只加載不調用的 307 個 + 頭文件無原型的）接 nvn_unimpl_entry：被調到即報紅打印名字。
// GPU 地址 = CPU 地址（內存池用遊戲給的真內存，同時建成零拷貝 MTLBuffer）。命令錄進遊戲給的命令內存，提交時回放成
// Metal 命令緩衝（nvn_mtl.m）：緩衝拷貝 / uniform 更新走 blit（GPU 時序），事件寫值 / 計數器在 GPU 完成後寫；
// 同步對象 = 提交柵欄值，等待只在 GPU 完成後放行。
// 每個實現都按 nvn_protos.h 的原型做編譯期簽名檢查（NVNboolean 參數按 uint32_t 收、只看低 8 位）。
// 環境開關：DM_NVN_FRAMES=N 前 N 幀逐幀打印調用表（默認 3）；DM_NVN_TSV=路徑 每幀調用表與計時寫 TSV（DM_NVN_TSV_CALLS=0 只寫計時）；DM_SLOWFRAME=1 只把掉幀的逐幀分項寫進日誌；DM_NVN_LOG=1 記對象創建。
// NVN interface layer: takes over the nvnBootstrapLoader imported by main; the NVN driver in sdk is not executed at all.
// All 523 NVN names return counting thunks (nvn_thunks.S), and the thunks forward to the implementations here:
//   - those with semantics (device / memory pools / textures and samplers / queue / window / sync / events / command buffer memory) are implemented per NVN semantics, and the objects are real;
//   - those that only change GPU state and have no CPU-visible result (state objects, bindings, draws, clears...) are recorded as commands and replayed at submit;
//   - the rest (the 307 that main only loads but never calls + those without a prototype in the header) go to nvn_unimpl_entry: reports a failure and prints the name when called.
// GPU address = CPU address (memory pools use the real memory given by the game, also built as zero-copy MTLBuffer). Commands are recorded into the command memory given by the game and replayed at submit into a
// Metal command buffer (nvn_mtl.m): buffer copies / uniform updates go through blit (GPU ordering), event value writes / counters are written after the GPU completes;
// sync object = submit fence value, and waits are released only after the GPU completes.
// Every implementation gets a compile-time signature check against the prototypes in nvn_protos.h (NVNboolean parameters are received as uint32_t, only the low 8 bits are looked at).
// Environment switches: DM_NVN_FRAMES=N prints the call table per frame for the first N frames (default 3); DM_NVN_TSV=path writes the per-frame call table and timing to a TSV (DM_NVN_TSV_CALLS=0 writes timing only); DM_SLOWFRAME=1 writes only the per-frame breakdown of dropped frames to the log; DM_NVN_LOG=1 logs object creation.
#include "../../host/hle.h"
#include "nvn_protos.h"
#include "nvn_count.h"
#include "nvn_tex.h"
#include "nvn_mtl.h"
#include "nvn_pace.h"
#include "../../host/front.h"
#include "nvn_dirty.h"
#include <mach/mach_time.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#include <unistd.h>
#if TARGET_OS_OSX
#define DM_MEM_CAP_DEFAULT_GB 24
#else
#define DM_MEM_CAP_DEFAULT_GB 0
#endif
#include <pthread/qos.h>
#include <sys/resource.h>
#include <mach/thread_policy.h>
#include <assert.h>
#include <stdatomic.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

// ---------- 槽表、跳板、未實現樁 ----------
// ---------- Slot table, thunks, unimplemented stubs ----------
typedef struct { _Atomic uint64_t cnt; void *fn; } nvn_slot_t;
nvn_slot_t nvn_slot[NVN_NFUNC];                 // 跳板按 i*16 訪問，佈局不可改 | The thunk accesses it by i*16; the layout must not change
_Static_assert(sizeof(nvn_slot_t) == 16, "跳板按 16 字節步長尋址");
extern void *nvn_thunk[NVN_NFUNC];
extern const char *nvn_names[NVN_NFUNC];
extern void nvn_unimpl_entry(void);
extern void nvn_prof_entry(void);
extern void nvn_big_entry(void);
void *nvn_big_real[NVN_NFUNC];            // 走宿主大棧的函數的真實現（nvn_entry.S nvn_big_entry） | the real implementation of functions that run on the big host stack (nvn_entry.S nvn_big_entry)
void *nvn_real[NVN_NFUNC];               // DM_NVN_PROF：計時包裝調用的真實現 | DM_NVN_PROF: the real implementation called by the timing wrapper
_Atomic uint64_t nvn_prof_ticks[NVN_NFUNC]; // DM_NVN_PROF：各函數累計耗時（cntvct 計數） | DM_NVN_PROF: cumulative time of each function (cntvct counts)
static uint8_t g_unimpl_seen[NVN_NFUNC];

uint64_t nvn_unimpl_c(nvn_slot_t *slot, uint64_t x0, uint64_t x1, uint64_t x2) {
    unsigned i = (unsigned)(slot - nvn_slot);
    if (!g_unimpl_seen[i]++) LOG("【紅】NVN 未實現被調用：%s (x0=%#llx x1=%#llx x2=%#llx)", nvn_names[i], x0, x1, x2);
    return 0;
}

typedef struct { const char *name; void *fn; } nvn_impl;
#define NVN(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                             \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)
// 無原型的新版接口：不做簽名檢查
// Newer interfaces without prototypes: no signature check
#define NVN_NOPROTO(name, ret, ...)                                                               \
    static ret h_##name(__VA_ARGS__);                                                             \
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)

static int g_log;
uint64_t nvn_new_serial(void) { static _Atomic uint64_t n; return ++n; }
static unsigned g_nsem;                          // 語義實現個數（nvn_init_slots 填） | Number of semantic implementations (filled by nvn_init_slots)
#define OLOG(...) do { if (g_log) LOG("[nvn] " __VA_ARGS__); } while (0)

static void nvn_init_slots(void) {
    static int done; if (done) return; done = 1;
    g_log = getenv("DM_NVN_LOG") != NULL;
    unsigned long sz;
    const nvn_impl *im = (const nvn_impl *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &sz);
    unsigned nim = im ? (unsigned)(sz / sizeof *im) : 0, hit = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) nvn_slot[i].fn = (void *)nvn_unimpl_entry;
    for (unsigned k = 0; k < nim; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], im[k].name)) i++;
        if (i == NVN_NFUNC) die("NVN 實現 %s 不在 main 的名字表裡", im[k].name);
        nvn_slot[i].fn = im[k].fn; hit++;
    }
    g_nsem = hit;
}

static void *nvn_lookup(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return nvn_thunk[i];
    LOG("【紅】NVN 取址未知名字：%s", name ? name : "(null)");
    return NULL;
}

// nvnBootstrapLoader 由 main 導入（sdk 定義）；強制接管後 sdk 的 NVN 驅動不再被引用
// nvnBootstrapLoader is imported by main (defined in sdk); after it is forcibly taken over, the NVN driver in sdk is no longer referenced
HLE_FORCE("nvnBootstrapLoader", "NVN 圖形接口整塊換成宿主空設備（後續 Metal），sdk 的 NVN 驅動不執行",
          void *, h_nvnBootstrapLoader, const char *name) { return nvn_lookup(name); }

// ---------- 布爾參數：Switch 調用方不擴展，只看低 8 位 ----------
// ---------- Boolean parameters: Switch callers do not extend them, only look at the low 8 bits ----------
#define B(x) ((x) & 0xff)

#include "nvn_obj.h"

// ---------- 設備 ----------
// ---------- Device ----------
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t *)b)->flags;
    OLOG("DeviceInitialize %p flags=%#x", (void *)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)nvn_lookup(name);
}

// 設備屬性：Tegra X1 實測值（nnheaders nvn_DeviceConstantsNX.h，在 SMO 上逐項調 nvnDeviceGetInteger 得到）
// Device properties: values measured on Tegra X1 (nnheaders nvn_DeviceConstantsNX.h, obtained by calling nvnDeviceGetInteger item by item on SMO)
static const int g_devinfo[] = {
    // API 版本取遊戲檔案 device_api（缺省 55.15）：D2R 讀了不比較；SMBW（SDK 16）要求主版本 == 55、次版本 ≥ 15（main 0x5a7b48）；超級馬力歐 3D 世界 + 狂怒世界（SDK 10）要求主版本 == 53、次版本 ≥ 313（main+0x711c88），
    // 不符時不建設備、設備指針留空，nnMain 裡隨即空指針崩潰。DM_NVN_API=主.次 可改（上游 42a50ef）
    // API version from the profile's device_api (default 55.15): D2R reads it without comparing; SMBW (SDK 16) requires major == 55 and minor >= 15 (main 0x5a7b48); Super Mario 3D World + Bowser's Fury (SDK 10) requires major == 53 and minor >= 313 (main+0x711c88),
    // and on a mismatch it creates no device and leaves the device pointer null, which crashes in nnMain right after. DM_NVN_API=major.minor overrides (upstream 42a50ef)
    [0x00] = DM_GAME_NVN_API_MAJOR, [0x01] = DM_GAME_NVN_API_MINOR,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    unsigned w = (unsigned)what;
    // 0x5F 分離採樣器綁定數、0x60/0x61 在 SMO 實測表裡沒有：0x5F 按採樣器池上限給 4096 未核，0x60/0x61 報紅給 0
    // 0x5F separate sampler binding count and 0x60/0x61 are not in the table measured on SMO: 0x5F is given 4096 per the sampler pool limit, unverified; 0x60/0x61 report a failure and return 0
    if (w >= sizeof g_devinfo / sizeof *g_devinfo) { LOG("【紅】nvnDeviceGetInteger 未知項 %#x，給 0", w); *out = 0; return; }
    *out = g_devinfo[w];
    if (w <= 1) { static int api[2] = {-1, -1}; if (api[0] < 0) { const char *e = getenv("DM_NVN_API"); api[0] = g_devinfo[0]; api[1] = g_devinfo[1]; if (e) sscanf(e, "%d.%d", &api[0], &api[1]); } *out = api[w]; }
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
// 分離貼圖 / 採樣器句柄：值 = 描述符號（採樣器號放 20 位以上，與組合句柄的位佈局一致）
// Separate texture / sampler handles: value = descriptor index (the sampler index goes in bit 20 and above, consistent with the bit layout of combined handles)
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = m; LOG("[nvn] 窗口原點 %s", m ? "左上（同 D3D / Metal）" : "左下（GL）"); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = m; LOG("[nvn] 深度模式 %s", m ? "近=0（同 D3D / Metal）" : "近=-w（GL）"); }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; (void)h; }

// ---------- 內存池 ----------
// ---------- Memory pools ----------
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) { memset(b, 0, sizeof(pool_builder_t)); }
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem = m; ((pool_builder_t *)b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags = f; }
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    o->magic = MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = s->mem; o->size = s->size;
    if (o->mem) { mtl_pool_add(o->mem, o->size); nvn_dirty_add_range(o->mem, o->size); }
    if (!o->mem && !(o->flags & 0x200)) LOG("【紅】MemoryPoolInitialize 非虛擬池沒有存儲 flags=%#x size=%#llx", o->flags, o->size);
    if (getenv("DM_NVN_VLOG")) { LOG("[nvn] MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, (unsigned long long)o->size, o->flags); void dm_backtrace(uint64_t fp); dm_backtrace((uint64_t)__builtin_frame_address(0)); }
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags);
    return 1;
}
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) { if (((pool_t *)p)->mem && !(((pool_t *)p)->flags & 0x200)) {   // 虛擬池的存儲借自物理池，不退登記 | A virtual pool's storage is borrowed from a physical pool; do not unregister it
 nvn_dirty_remove_range(((pool_t *)p)->mem); mtl_pool_del(((pool_t *)p)->mem); } mtl_alias_drop_pool(p); ((pool_t *)p)->magic = 0; }
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { return (NVNbufferAddress)((const pool_t *)p)->mem; }

// ---------- 描述符池 ----------
// ---------- Descriptor pools ----------
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n}; OLOG("TexturePool n=%d", n); mtl_set_tex_pool_n(n); return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *tp) { (void)tp; }
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) { (void)tp; mtl_reg_tex(id, (const tex_t *)t, (const view_t *)v); }
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n}; OLOG("SamplerPool n=%d", n); return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *sp) { (void)sp; }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { (void)sp; mtl_reg_samp(id, &((const samp_t *)s)->p); }

// ---------- 貼圖 ----------
// ---------- Textures ----------
#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] 預排布貼圖 %ux%ux%u 級%u 格式%#x 目標%d 佈局字節 %02x%02x%02x%02x%02x%02x%02x%02x → 塊高 log2=%u（取佈局第 0 字節）尺寸 %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("【紅】TextureBuilderGetStorageSize 未知格式 %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
// 預排布貼圖數據拷進存儲（寫前解保護並標髒，讓上傳層看到內容變了）。前 8 例記一行：尺寸、格式、存儲原先是否全零、是否已與數據相同
// Copy packaged texture data into the storage (unprotect and mark dirty before writing so the upload layer sees the change). The first 8 cases log one line: size, format, whether the storage was all zero, whether it already matched the data
static void pkg_copy(tex_t *o, const void *data) {
    static _Atomic uint64_t n;
    uint64_t k = atomic_fetch_add(&n, 1);
    if (!o->mem || !o->L.size) { if (!k) LOG("【紅】預排布貼圖數據：存儲不在宿主可寫的池裡（%ux%u 格式 %#x），沒拷", o->d.width, o->d.height, o->d.format); return; }
    if (k < 8) {
        size_t chk = o->L.size < 4096 ? (size_t)o->L.size : 4096; int zero = 1;
        for (size_t i = 0; i < chk && zero; i++) zero = o->mem[i] == 0;
        LOG("[nvn] 預排布貼圖數據 #%llu：%ux%ux%u 級 %u 格式 %#x 存儲 %#llx 字節；拷前存儲%s、%s數據", (unsigned long long)k, o->d.width, o->d.height, o->d.depth, o->d.levels,
            o->d.format, (unsigned long long)o->L.size, zero ? "全零" : "有內容", memcmp(o->mem, data, chk) ? "不同於" : "已等於");
    }
    nvn_texdesc d = o->d; unsigned top = 1; for (uint32_t m = d.width > d.height ? d.width : d.height; m > 1; m >>= 1) top++;
    if (d.levels > top) d.levels = top;   // 遊戲偶有報超出尺寸的級數（見 nvn_mtl.m 的級數上限）：只拷實際存在的級，免得讀出數據尾 | the game occasionally reports more levels than the size allows (see the level clamp in nvn_mtl.m): copy only the levels that exist, so as not to read past the data
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    size_t sz = (size_t)(L.size < o->L.size ? L.size : o->L.size);
    if (!memcmp(o->mem, data, sz)) return;   // 朋友收集實測：資源加載已把同樣的數據放進池裡，不必再寫（寫會解保護、觸發重傳） | measured in Tomodachi Life: the resource loader already put the same data in the pool, so no write (a write would unprotect and trigger a re-upload)
    static _Atomic int diff; if (!atomic_fetch_add(&diff, 1)) LOG("【紅】預排布貼圖數據與池裡的存儲不同（%ux%u 格式 %#x）：按原版語義拷進存儲", o->d.width, o->d.height, o->d.format);
    dm_guest_write_prepare(o->mem, sz);
    memcpy(o->mem, data, sz);
}
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    // 對仍有效的貼圖對象再初始化（沒先 Finalize）：先放掉舊序號的宿主貼圖。SMBW 的臨時渲染目標每幀在同一個對象上重新初始化、從不 Finalize，
    //   原先每次都新建一張私有 MTLTexture、舊的永遠留在序號表裡，顯卡內存每幀漲約 13 MB（24 GB 時被內存看門狗殺）
    // Re-initializing a texture object that is still live (without a Finalize first): release the host texture of the old serial first. SMBW re-initializes its transient render targets on the same object every frame and never Finalizes them;
    //   previously every init created a new private MTLTexture and the old one stayed in the serial table forever, so GPU memory grew about 13 MB per frame (killed by the memory watchdog at 24 GB)
    if (o->magic == MAGIC('T', 'E', 'X', 0) && o->serial) { static uint64_t n; if (!n++) LOG("[nvn] TextureInitialize 對仍有效的貼圖對象再初始化（無 Finalize）：放掉舊宿主貼圖（首例 ser=%llu）", (unsigned long long)o->serial); mtl_tex_release(o); }
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8); o->set_swz = s->set_swz; o->swz = s->swz;
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    if (s->pkg_data) pkg_copy(o, s->pkg_data);
    o->serial = nvn_new_serial();
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, o->L.size);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) { mtl_tex_release(TX(t)); TX(t)->magic = 0; }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
// 以下幾個查詢 Engage（Unity）啓動就用：它按「貼圖 / 緩衝所在的池」去 MemoryPoolMap，樁返回 0 時它對空池取址崩
// The queries below are used by Engage (Unity) at startup: it calls MemoryPoolMap on "the pool the texture / buffer lives in", and with a stub returning 0 it dereferenced a null pool and crashed
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return (int)CTX(t)->d.levels; }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return CTX(t)->d.samples; }
NVN(nvnTextureGetMemoryPool, NVNmemoryPool *, const NVNtexture *t) { return (NVNmemoryPool *)CTX(t)->pool; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->off; }
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { (void)t; return 0; }   // 宿主不做 ZCull：不需要存儲 | the host does no ZCull: no storage needed
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem;
}
// 頭文件無原型；main 0x174899c 一類調用點把返回值與 1 比，不等則置某狀態位 0x100。含義未核，先給 1
// No prototype in the header; call sites such as main 0x174899c compare the return value with 1 and set some status bit 0x100 if not equal. Meaning unverified; return 1 for now
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { (void)t; return 1; }
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); const view_t *w = (const view_t *)v;
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * o->L.layer_size;
    return (ptrdiff_t)off;
}
// CPU 讀貼圖（NVN ReadTexels）：預排布貼圖以池內存為真值（解排布直讀），GPU 寫出的貼圖從 MTLTexture 讀回（nvn_mtl.m）
// CPU texture read (NVN ReadTexels): for packaged textures the pool memory is the ground truth (de-layout and read directly); GPU-written textures are read back from the MTLTexture (nvn_mtl.m)
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    mtl_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

// ---------- 採樣器 ----------
// ---------- Samplers ----------
#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // GL 默認 | GL defaults
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { (void)b; (void)v; }   // Metal 無對應，忽略 | No Metal equivalent, ignored
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; o->magic = MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

// ---------- 程序 ----------
// ---------- Programs ----------
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); return 1;
}
static void pk_drop(const void *p);
NVN(nvnProgramFinalize, void, NVNprogram *p) { mtl_prog_release((prog_t *)p); ((prog_t *)p)->magic = 0; pk_drop(p); }
// 程序的各階段鍵（旁表：客體 NVNprogram 地址 → 宿主對象；ok 第 i 位 = 第 i 階段已算） | a program's per-stage keys (side table: guest NVNprogram address → host object; bit i of ok = stage i computed)
typedef struct { uint8_t key[6][8]; uint8_t ok; } prog_keys_t;
static pthread_mutex_t g_pk_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableDictionaryRef g_pk;   // 專用表（不與別的旁表共用鍵） | a dedicated table (keys not shared with other side tables)
static prog_keys_t *pk_get(const void *p, int make) {
    pthread_mutex_lock(&g_pk_mu);
    if (!g_pk) g_pk = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    prog_keys_t *k = (prog_keys_t *)CFDictionaryGetValue(g_pk, p);
    if (!k && make) { k = calloc(1, sizeof *k); CFDictionarySetValue(g_pk, p, k); }
    pthread_mutex_unlock(&g_pk_mu);
    return k;
}
static void pk_drop(const void *p) {
    pthread_mutex_lock(&g_pk_mu);
    prog_keys_t *k = g_pk ? (prog_keys_t *)CFDictionaryGetValue(g_pk, p) : NULL;
    if (k) { CFDictionaryRemoveValue(g_pk, p); free(k); }
    pthread_mutex_unlock(&g_pk_mu);
}
// mapped = 1：代碼區須在已登記的內存池裡才算（SetShaders 時：馬力歐有程序先拿未映射的地址 SetShaders，首次繪製前才填好）
// mapped = 1: computed only when the code area is in a registered memory pool (at SetShaders: Mario sets some programs with an unmapped address first and fills it in before the first draw)
static int keys_hash(const NVNshaderData *sd, int mapped, uint8_t out[8]) {
    uint32_t ncode = 0; void *mb; uint64_t mo;
    if (!sd->control) return 0;
    memcpy(&ncode, (const uint8_t *)sd->control + 0x704, 4);
    int code = sd->data && ncode && ncode < (1u << 24);
    if (mapped && !(code && mtl_addr(sd->data, ncode, &mb, &mo))) return 0;
    // 控制段 +0x20 是它自己的長度：不短於 0x880 的（馬力歐、Engage、生存恐懼、十三機兵、朋友收集全部）取前 0x880；SDK 10 的 3D 世界有 0x850 字節的，後面緊跟別的數據，只取它自己的長度（shader_split.py ctl_len，上游 42a50ef）
    // Control +0x20 holds the section's own length: sections of at least 0x880 bytes (all of Mario's, Engage's, Dread's, 13 Sentinels' and Tomodachi Life's) use the first 0x880; SDK 10's 3D World has 0x850-byte ones followed by other data, so only their own length is used (shader_split.py ctl_len, upstream 42a50ef)
    uint32_t nctl = 0x880, own; memcpy(&own, (const uint8_t *)sd->control + 0x20, 4);
    if (own >= 0x760 && own < 0x880) nctl = own;
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX hc; CC_SHA256_Init(&hc);
    CC_SHA256_Update(&hc, sd->control, nctl);
    if (code) CC_SHA256_Update(&hc, (const void *)(uintptr_t)sd->data, ncode);
    CC_SHA256_Final(md, &hc); memcpy(out, md, 8);
    return 1;
}
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]) {
    if (st >= p->nstage) return 0;
    prog_keys_t *k = pk_get(p, 1);
    if (!(k->ok >> st & 1)) { keys_hash(&p->sd[st], 0, k->key[st]); k->ok |= (uint8_t)(1u << st); }
    memcpy(key, k->key[st], 8);
    return 1;
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("【紅】ProgramSetShaders 階段數 %d", n); return 0; }
    mtl_prog_release(o); o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = nvn_new_serial();
    // 鍵在這裡算（見 nvn_obj.h prog_key）：控制段只在本調用期間保證有效；代碼區在內存池裡，程序在用期間一直有效
    // The keys are computed here (see prog_key in nvn_obj.h): the control section is only guaranteed valid during this call; the code area lives in a memory pool and stays valid while the program is in use
    static int dump = -1; if (dump < 0) dump = getenv("DM_NVN_SHDUMP") != NULL;
    prog_keys_t *k = pk_get(p, 1);
    k->ok = 0;
    for (int i = 0; i < n; i++) {
        if (keys_hash(&sd[i], 1, k->key[i])) k->ok |= (uint8_t)(1u << i);
        // DM_NVN_SHDUMP（調試）：留一份控制段，供回放層把不在映射表裡的著色器存盤（不釋放）
        // DM_NVN_SHDUMP (debugging): keep a copy of the control section so the replay layer can save shaders missing from the map (never freed)
        if (dump && sd[i].control) { void *c = malloc(0x880); memcpy(c, sd[i].control, 0x880); o->sd[i].control = c; }
    }
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
}

// ---------- 同步 / 事件 ----------
// ---------- Sync / events ----------
NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; return 1; }
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
// 圍欄 = 「此前提交到隊列的命令全部完成」：記下最近一次提交的柵欄值
// Fence = "all commands previously submitted to the queue have completed": record the fence value of the most recent submit
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) { (void)q; (void)c; (void)f; ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = mtl_last_fence(); }
// 沒圍欄過的同步對象按真機行為等到超時；已圍欄的只在 GPU 真正完成到該柵欄值後放行
// A sync object that was never fenced waits until timeout, as on a real device; a fenced one is released only after the GPU has actually completed up to that fence value
// GPU 側等同步：宿主只有一條 Metal 隊列、提交按序執行，圍欄之前的命令必然先完成，不必等。尚未圍欄的同步對象（別的線程之後才圍欄）這裡不等，記一次
// A GPU-side wait on a sync: the host has a single Metal queue that executes submissions in order, so commands before the fence are always done first and nothing needs waiting. A sync not fenced yet (fenced later by another thread) is not waited for here; logged once
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) { (void)q; static int once; if (!((const sync_t *)s)->fenced && !once++) LOG("[nvn] QueueWaitSync：同步對象還沒圍欄，宿主不等"); return 1; }
// ZCull 數據：宿主不做 ZCull，存 / 取都無事可做 | ZCull data: the host does no ZCull, so there is nothing to save / restore
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    const sync_t *o = (const sync_t *)s;
    if (!o->fenced) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    if (mtl_wait(o->fence, 0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    return mtl_wait(o->fence, timeout) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = (ev_builder_t){(const pool_t *)p, off}; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *s = (const ev_builder_t *)b;
    if (!s->pool || !s->pool->mem) { LOG("【紅】EventInitialize 沒有存儲"); return 0; }
    *(ev_t *)e = (ev_t){MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(s->pool->mem + s->off)}; return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
static void ev_signal(const ev_t *e, int mode, uint32_t v) { if (mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v; else *e->val = v; }
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) { ev_signal((const ev_t *)e, m, v); }

// ---------- 隊列 ----------
// ---------- Queue ----------
#define QB(b) ((queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // 設備默認值 | Device defaults
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; }
// 隊列內存：命令 + 計算 + 控制三塊之和按 4 KiB 取整（宿主不使用，只需遊戲按此大小分配）
// Queue memory: the sum of the command + compute + control blocks rounded to 4 KiB (the host does not use it; the game only needs to allocate this size)
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    *(queue_t *)q = (queue_t){MAGIC('Q', 'U', 'E', 'U'), ((const queue_builder_t *)b)->dev, 0};
    OLOG("QueueInitialize %p", (void *)q); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) { ((queue_t *)q)->magic = 0; }
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info); return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; }
NVN(nvnQueueFinish, void, NVNqueue *q) { (void)q; mtl_finish(); }

// ---------- 命令緩衝：錄進遊戲的命令內存，提交時執行 CPU 可見副作用 ----------
// ---------- Command buffer: record into the game's command memory, perform CPU-visible side effects at submit ----------
#define CB(c) ((cmd_t *)(c))
#define OP_RESERVE 24                             // 段尾永遠留夠一條 JUMP / END | Always keep enough room at the segment end for one JUMP / END

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    memset(c, 0, sizeof(cmd_t)); CB(c)->magic = MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) { CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CB(c)->cb_data = d; }
static uint8_t *align8(uint8_t *p) { return (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7); }
// 診斷 DM_NVN_CMDSTAT=1：每 300 幀記一行命令錄製的用量——每幀錄了多少字節 / 多少條、遊戲每次給多大一段命令內存、每幀補內存回調幾次、宿主補段幾次、
// 各類命令的條數與字節（找「我們的記錄比真機命令大多少」）。只計數，不改錄製 / 回放
// Diagnostic DM_NVN_CMDSTAT=1: every 300 frames log one line of command recording usage -- how many bytes / records were recorded per frame, how large a command memory segment the game gives each time, how many out-of-memory callbacks per frame, how many host-supplied segments,
// and the count and bytes of each command type (to find "how much larger our records are than real-device commands"). Counts only; does not change recording / replay
static struct { int on; uint64_t bytes, recs, grants, grant_bytes, grant_min, grant_max, cbs, hostseg, op_n[64], op_b[64], frame0, waste; } g_cs = { .on = -1 };
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t dm_nvn_frame_count(void);
static int cs_on(void) { if (g_cs.on < 0) g_cs.on = getenv("DM_NVN_CMDSTAT") != NULL; return g_cs.on; }
static void cs_op(uint32_t op, uint32_t size) { if (!cs_on() || op >= 64) return; pthread_mutex_lock(&g_cs_mu); g_cs.op_n[op]++; g_cs.op_b[op] += size; pthread_mutex_unlock(&g_cs_mu); }
static void cs_dump(void) {   // 每幀調一次（出圖處） | Called once per frame (at present)
    if (!cs_on()) return; uint64_t f = dm_nvn_frame_count(); if (f - g_cs.frame0 < 300) return;
    pthread_mutex_lock(&g_cs_mu); double n = (double)(f - g_cs.frame0); char top[700]; int k = 0; uint8_t used[64] = {0};
    for (int r = 0; r < 8; r++) { int bi = -1; for (int i = 0; i < 64; i++) if (!used[i] && g_cs.op_b[i] && (bi < 0 || g_cs.op_b[i] > g_cs.op_b[bi])) bi = i; if (bi < 0) break; used[bi] = 1;
        k += snprintf(top + k, sizeof top - k, " 命令%d：%.0f 條×%.0f 字節=%.1f KB；", bi, g_cs.op_n[bi] / n, (double)g_cs.op_b[bi] / (double)g_cs.op_n[bi], g_cs.op_b[bi] / n / 1024.0); }
    LOG("[nvn] 命令內存 至幀 %llu：每幀錄 %.1f KB / %.0f 條；遊戲給段 每幀 %.2f 次，每段 均 %.1f KB（最小 %.1f / 最大 %.1f）；補內存回調 每幀 %.2f 次；宿主補段 每幀 %.3f 次；換段丟棄的段尾 每幀 %.1f KB｜每幀%s",
        (unsigned long long)f, g_cs.bytes / n / 1024.0, g_cs.recs / n, g_cs.grants / n, g_cs.grants ? g_cs.grant_bytes / (double)g_cs.grants / 1024.0 : 0, g_cs.grant_min / 1024.0, g_cs.grant_max / 1024.0, g_cs.cbs / n, g_cs.hostseg / n, g_cs.waste / n / 1024.0, top);
    int on = g_cs.on; memset(&g_cs, 0, sizeof g_cs); g_cs.on = on; g_cs.frame0 = f; pthread_mutex_unlock(&g_cs_mu);
}
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { op_hdr h = {op, size}; memcpy(p, &h, sizeof h); if (op) cs_op(op, size); }
// 新段：錄製中則在舊段尾寫 JUMP 接上
// New segment: if recording is in progress, write a JUMP at the end of the old segment to link it
static void cmd_new_segment(cmd_t *o, uint8_t *mem, size_t n) {
    uint8_t *start = align8(mem), *end = mem + n;
    if (o->recording && o->cur) { if (cs_on() && o->end > o->cur) { pthread_mutex_lock(&g_cs_mu); g_cs.waste += (uint64_t)(o->end - o->cur); pthread_mutex_unlock(&g_cs_mu); } put_hdr(o->cur, OP_JUMP, 16); memcpy(o->cur + 8, &start, 8);
        o->used += 16; }   // JUMP 也佔本次錄製的客體內存：計進 used，遊戲按 used 緊挨著排下一段時不會蓋住它 | the JUMP also occupies guest memory of this recording: count it in used, so the game packing the next recording right behind by used does not overwrite it
    o->cur = start; o->end = end;
}
// 遊戲自己的命令內存用完時的宿主補段：遊戲的補內存回調在它那塊命令內存池分不出來時，會拿空池（p = NULL、n = 0）調 AddCommandMemory
//（88 級法師檔開高級屬性頁：一幀裡列表項很多，原實現在這裡讀 NULL+8 段錯誤）。我們的記錄比真機 NVN 的命令大，遊戲按真機用量留的預算不一定夠，
// 所以這不是遊戲的錯誤狀態而是我們要兜住的：給一段宿主內存接著錄。段在提交時就地回放，留 8 幀後回收（提交在錄製之後同幀或下一幀）。
// Host-supplied segment when the game's own command memory runs out: when the game's out-of-memory callback cannot allocate from its command memory pool, it calls AddCommandMemory with an empty pool (p = NULL, n = 0)
// (opening the advanced stats page on the level 88 sorceress save: many list items in one frame, and the original implementation segfaulted here reading NULL+8). Our records are larger than real-device NVN commands, so the budget the game reserved for real-device usage is not necessarily enough,
// so this is not an error state of the game but something we must catch: provide a segment of host memory to keep recording. The segment is replayed in place at submit and reclaimed after being kept for 8 frames (submit is in the same frame as recording or the next one).
// 回收時機跟著遊戲的命令內存塊走，不按幀數：遊戲會緩存錄好的命令句柄、每幀 CALL 直到自己重錄（SMBW 的物體設置句柄），
// 句柄錄進宿主補段後，原先「借出 8 幀就回收」會在遊戲還在 CALL 時把段改寫掉（Mac 上逼溢出時回放出亂命令：RGBA16F 被當成深度格式建管線）。
// 補段經段尾的 JUMP 接在一次錄製後面：那次錄製在客體內存裡佔 [BEGIN, JUMP 尾)，遊戲只在這些字節作廢後才會把它們重新給出去
// （原地重錄那個材質時；按 used 緊挨著排下一段時不會碰到，used 已含 JUMP）。所以補段記下這段客體範圍，遊戲再給出與之重疊的內存時回收；
// 從補段接著溢出的補段沿用同一範圍。（不能拿整塊遊戲內存當範圍：SMBW 從一大塊裡連續分出很多次錄製，後面在同一塊裡再給內存時會把仍在用的補段收掉）
// (Do not use the whole game chunk as the range: SMBW sub-allocates many recordings out of one big chunk, and a later grant inside that chunk freed segments still in use.)
// Reclaim follows the game's command memory chunks, not a frame count: the game caches recorded command handles and CALLs them every frame until it re-records them itself (SMBW's object setup handles);
// once a handle was recorded into a host segment, the old "reclaim 8 frames after lending" rewrote the segment while the game still CALLed it (with overflow forced on the Mac, the replay read garbage commands: RGBA16F used as a depth format for a pipeline).
// A segment hangs off one recording through the JUMP at the end of its guest part: that recording occupies [BEGIN, end of JUMP) in guest memory, and the game only gives those bytes out again once they are dead
// (when it re-records that material in place; packing the next recording right behind by used never touches them, since used includes the JUMP). So a segment remembers that guest range and is reclaimed when the game gives out memory
// overlapping it; segments overflowing from a host segment keep the same range.
// 段池按需增長（SMBW 開場關：很多小命令緩衝每幀各溢出一次，舊版固定 64 段 × 1 MB 在 8 幀內用完）；總量超過 HOSTSEG_CAP 才中止。
// The segment pool grows on demand (SMBW opening course: many small command buffers each overflow once per frame, and the old fixed 64 segments × 1 MB ran out within 8 frames); abort only beyond HOSTSEG_CAP in total.
#define HOSTSEG_SZ (256u << 10)
#define HOSTSEG_CAP (1u << 30)
uint64_t dm_nvn_frame_count(void);
typedef struct { uint8_t *mem; uint64_t frame; int busy; uintptr_t org_lo, org_hi; } hostseg_t;   // org：它所接的那次錄製在客體內存裡佔的範圍（0 = 未知，不回收） | org: the guest range of the recording it hangs off (0 = unknown, never reclaimed)
static hostseg_t *g_hostseg; static int g_nhostseg, g_caphostseg;
static pthread_mutex_t g_hostseg_mu = PTHREAD_MUTEX_INITIALIZER;
// 取一段：來源 = 當前錄製的客體字節 [BEGIN, JUMP 尾)；寫指針已在補段裡（接著溢出）則沿用那段的來源
// Take a segment: origin = the current recording's guest bytes [BEGIN, end of JUMP); if the write pointer is already in a host segment (overflowing again), keep that segment's origin
static uint8_t *hostseg_take(const cmd_t *o) {
    static unsigned total; uint64_t f = dm_nvn_frame_count(); uint8_t *m = NULL;
    uintptr_t lo = 0, hi = 0, cur = o ? (uintptr_t)o->cur : 0;
    pthread_mutex_lock(&g_hostseg_mu);
    int in_seg = 0;
    for (int i = 0; i < g_nhostseg && cur; i++) if (cur >= (uintptr_t)g_hostseg[i].mem && cur <= (uintptr_t)g_hostseg[i].mem + HOSTSEG_SZ) { lo = g_hostseg[i].org_lo; hi = g_hostseg[i].org_hi; in_seg = 1; break; }
    if (!in_seg && o && o->recording && o->cur) { lo = o->begin && (uintptr_t)o->begin <= cur ? (uintptr_t)o->begin : cur; hi = cur + 16; }
    for (int i = 0; i < g_nhostseg && !m; i++)
        if (!g_hostseg[i].busy) { g_hostseg[i] = (hostseg_t){ g_hostseg[i].mem, f, 1, lo, hi }; m = g_hostseg[i].mem; }
    if (!m && (uint64_t)(g_nhostseg + 1) * HOSTSEG_SZ <= HOSTSEG_CAP) {
        if (g_nhostseg == g_caphostseg) { g_caphostseg = g_caphostseg ? g_caphostseg * 2 : 64; g_hostseg = realloc(g_hostseg, sizeof *g_hostseg * (size_t)g_caphostseg); }
        m = malloc(HOSTSEG_SZ);
        g_hostseg[g_nhostseg++] = (hostseg_t){ m, f, 1, lo, hi };
    }
    unsigned t = ++total; int n = g_nhostseg;
    pthread_mutex_unlock(&g_hostseg_mu);
    if (!m) die("NVN 命令內存：遊戲池已空，宿主補段已達 %u MB 仍未被遊戲的塊回收", HOSTSEG_CAP >> 20);
    if (t == 1 || t % 1024 == 0) LOG("[nvn] 遊戲的命令內存不夠，宿主補段（累計 %u 次，幀 %llu，段池 %d 段 × %u KB）", t, (unsigned long long)f, n, HOSTSEG_SZ >> 10);
    return m;
}
// 遊戲給出 [lo, hi)：所接錄製的客體字節與它重疊的補段都已作廢，收回 | The game gives out [lo, hi): segments whose recording's guest bytes overlap it are dead, reclaim them
static void hostseg_release(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_hostseg_mu);
    for (int i = 0; i < g_nhostseg; i++) if (g_hostseg[i].busy && g_hostseg[i].org_hi > lo && g_hostseg[i].org_lo < hi) g_hostseg[i].busy = 0;
    pthread_mutex_unlock(&g_hostseg_mu);
}
NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    const pool_t *pl = (const pool_t *)p;
    if (!pl || n < 64) { if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.hostseg++; pthread_mutex_unlock(&g_cs_mu); } cmd_new_segment(CB(c), hostseg_take(CB(c)), HOSTSEG_SZ); return; }
    if (!pl->mem) { LOG("【紅】AddCommandMemory 池沒有 CPU 存儲"); return; }
    hostseg_release((uintptr_t)(pl->mem + off), (uintptr_t)(pl->mem + off + n));
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.grants++; g_cs.grant_bytes += n; if (!g_cs.grant_min || n < g_cs.grant_min) g_cs.grant_min = n; if (n > g_cs.grant_max) g_cs.grant_max = n; pthread_mutex_unlock(&g_cs_mu); }
    { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ng; if (cblog && (++ng <= 60 || ng % 500 == 0)) LOG("[nvn] AddCommandMemory #%llu cmd=%p 池 %p +%#lx 長 %#zx（錄製中 %d）", (unsigned long long)ng, (void *)c, (const void *)pl, (long)off, n, CB(c)->recording); }
    cmd_new_segment(CB(c), pl->mem + off, n);
}
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *m, size_t n) { (void)c; (void)m; (void)n; }
// 取一條記錄的空間；不夠則按 NVN 約定回調遊戲補命令內存
// Get space for one record; if not enough, call back into the game per the NVN convention to add command memory
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
    size = (size + 7) & ~7u;
    for (int tries = 0; !o->cur || o->cur + size + OP_RESERVE > o->end; tries++) {
        // 遊戲的回調補不出來（較新 SDK 的遊戲按真機命令大小分小塊、或關了回調自己管）：轉宿主補段接著錄，不中止
        // The game's callback cannot supply enough (newer-SDK games hand out small chunks sized for real-device commands, or disable the callback and manage memory themselves): continue recording in a host segment instead of aborting
        if (!o->cb || tries == 2) {
            static int once; if (!once++) LOG("[nvn] 命令內存：遊戲回調補不夠（cb=%p，需 %u 字節），改用宿主補段", (void *)o->cb, size + OP_RESERVE);
            cmd_new_segment(o, hostseg_take(o), HOSTSEG_SZ); break;
        }
        if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.cbs++; pthread_mutex_unlock(&g_cs_mu); }
        { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ncb;
          if (cblog && (++ncb <= 40 || ncb % 1000 == 0)) LOG("[nvn] 命令內存回調 #%llu cmd=%p 需 %u（本段已用 %ld）", (unsigned long long)ncb, (void *)o, size + OP_RESERVE, o->cur && o->end ? (long)(o->cur - (o->end - 1)) : -1L); }
        o->cb((NVNcommandBuffer *)o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, size + OP_RESERVE, o->cb_data);
    }
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.bytes += size; g_cs.recs++; pthread_mutex_unlock(&g_cs_mu); }
    uint8_t *p = o->cur; o->cur += size; o->used += size; put_hdr(p, 0, size); return p;
}
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (o->recording) LOG("【紅】BeginRecording 重入");
    o->recording = 1; o->used = 0;
    uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_BEGIN, 8); o->begin = p;
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_END, 8);
    o->recording = 0;
    return (NVNcommandHandle)o->begin;
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->recording != 0; }
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    for (int i = 0; i < n; i++) { uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_CALL, 16); memcpy(p + 8, &h[i], 8); }
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_REPORT_COUNTER, 24); uint64_t tt = t; memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 32); put_hdr(p, OP_COPY_B2B, 32); uint64_t nn = n;
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// uniform 更新：數據在錄製時拷進命令內存（NVN 語義：錄製後調用方可改原數據），提交時寫進緩衝
// uniform update: the data is copied into command memory at record time (NVN semantics: the caller may modify the original data after recording) and written into the buffer at submit
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    (void)bufsz;
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf + off, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}

// ---------- 渲染命令：參數按 nvn_obj.h 的載荷格式錄進命令內存，回放見 nvn_mtl.m ----------
// ---------- Render commands: parameters are recorded into command memory in the payload format of nvn_obj.h; for replay see nvn_mtl.m ----------
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
// 帶數組的綁定記錄按實際個數分配：前綴字段 + n 個元素（回放端只讀前 n 個）。原先按最大個數定長記（綁頂點緩衝 272 字節、綁貼圖 / 採樣器 280 字節，
// 實際常只有 1–2 個），空尾佔每幀命令字節的四到五成；88 級法師的高級屬性頁每幀錄 12.5 MB，把遊戲自己的命令內存池（約每幀 105 段 × 128 KB）用空
// Binding records with arrays are allocated by the actual count: prefix fields + n elements (the replay side reads only the first n). Previously they were recorded at fixed length for the maximum count (bind vertex buffers 272 bytes, bind textures / samplers 280 bytes,
// while there are usually only 1–2 in practice), and the empty tail took 40–50% of the command bytes per frame; the advanced stats page of the level 88 sorceress recorded 12.5 MB per frame, using up the game's own command memory pool (about 105 segments x 128 KB per frame)
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) { *has = v != NULL; if (v) *dst = *(const view_t *)v; }
// 帶貼圖的記錄把貼圖對象拍成快照（各 0x80 字節，跟在記錄結構後面），回放讀快照：NVN 的命令錄完就不再依賴 API 對象，遊戲可以在提交前 Finalize、釋放或重用它。
// Unity（Engage）換場景時就這樣做；原先只記對象指針，回放讀到的是別的數據（高 1869367126 的貼圖，Metal 斷言中止）。
// Records with textures snapshot the texture objects (0x80 bytes each, after the record struct) and replay reads the snapshots: an NVN command no longer depends on the API object once recorded, and the game may finalize, free or reuse it before submitting.
// Unity (Engage) does so when it changes scenes; only the object pointer used to be recorded, and replay read other data (a texture 1869367126 high, a Metal assertion abort).
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)
#define REC_SNAP(c, OP, T, K) ((T *)rec((c), (OP), SNAP_OFF(T) + (size_t)(K) * sizeof(tex_t)))
static const tex_t *tex_snap(void *rec_, size_t off, int k, const NVNtexture *t, uint64_t ra) {
    if (!t) return NULL;
    tex_t *s = (tex_t *)((uint8_t *)rec_ + off) + k;
    memcpy(s, t, sizeof *s);
    // 錄製時對象無效（Unity 會在錄製與提交之間 Finalize 再 Initialize 同一個渲染目標對象）：照舊記對象指針、提交時再讀，那時多半又有效了；回放仍按 magic 攔
    // The object is invalid at record time (Unity finalizes and re-initializes the same render target object between recording and submitting): keep the object pointer as before and read it at submit, when it is usually valid again; replay still checks the magic
    if (s->magic != MAGIC('T', 'E', 'X', 0)) {
        static int red;
        if (red++ < 4) {   // 前 4 例記調用點與對象開頭，判斷遊戲傳的是什麼 | the first 4 log the call site and the start of the object, to tell what the game passed
            module_t *m = module_of((void *)ra); const uint64_t *q = (const uint64_t *)t;
            LOG("【紅】錄命令時貼圖對象無效（未初始化或已 Finalize）：%p 第 %d 個，調用點 %s+%#llx，對象開頭 %llx %llx %llx %llx %llx %llx；提交時再讀", (const void *)t, k,
                m ? m->name : "?", m ? (unsigned long long)(ra - (uint64_t)m->base) : (unsigned long long)ra,
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        }
        return (const tex_t *)t;
    }
    return s;
}
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("【紅】SetRenderTargets 數量 %d", n); n = n < 0 ? 0 : 8; }
    p_set_rt *r = REC_SNAP(c, OP_SET_RT, p_set_rt, n + 1); r->n = n;
    for (int i = 0; i < n; i++) { r->color[i] = ct ? tex_snap(r, SNAP_OFF(p_set_rt), i, ct[i], (uint64_t)__builtin_return_address(0)) : NULL; copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL); }
    r->depth = tex_snap(r, SNAP_OFF(p_set_rt), n, dt, (uint64_t)__builtin_return_address(0)); copy_view(&r->dview, &r->has_dview, dv);
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; memcpy(r->rgba, rgba, 16);
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = (p_clear_ds){depth, (int)B(dmask), stencil, smask};
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0 || first + n > 16) { LOG("【紅】SetViewports first=%d n=%d", first, n); return; }
    p_viewports *r = REC(c, OP_VIEWPORTS, p_viewports); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    // 每種階段掩碼第一次出現記一行（只換部分階段的綁定要按階段拼程序，見回放層 OP_BIND_PROGRAM） | log each stage mask the first time it appears (binds that replace only some stages need per-stage program composition, see OP_BIND_PROGRAM in the replay layer)
    static _Atomic uint64_t seen; uint64_t bit = 1ull << (stages & 63);
    if (!(atomic_fetch_or(&seen, bit) & bit)) LOG("[nvn] BindProgram 階段掩碼 %#x 首次出現（程序 %u 個階段）", stages, p ? ((const prog_t *)p)->nstage : 0);
    *REC(c, OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){(const prog_t *)p, stages};
}
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) { p_bind_state *r = REC(c, OP_BIND_STATE, p_bind_state); r->kind = kind; memcpy(r->bytes, st, n); }
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    if (n < 0 || n > 16) { LOG("【紅】BindVertexAttribState n=%d", n); return; }
    p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 4 * n);
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("【紅】BindVertexStreamState n=%d", n); return; }
    p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n);
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first + n > 16) { LOG("【紅】BindVertexBuffers first=%d n=%d", first, n); return; }
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n);
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
static void bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {
    if (n < 0 || n > 32) { LOG("【紅】綁定句柄數 %d", n); return; }
    p_bind_handles *r = RECN(c, op, p_bind_handles, h, n); r->stage = st; r->first = first; r->n = n; memcpy(r->h, h, 8 * n);
}
// 圖像（可寫貼圖）：句柄 = 貼圖池號（GetImageHandle 原樣返回；遊戲不調 RegisterImage，圖像與貼圖共用登記）
// Image (writable texture): handle = texture pool index (GetImageHandle returns it as-is; the game does not call RegisterImage, images and textures share registration)
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateTextureHandle h) { uint64_t v; memcpy(&v, &h, 8); bind_handles(c, OP_BIND_TEX, st, i, 1, &v); }   // 單個（Engage）：同複數版 | single (Engage): as the plural version
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) {
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value);
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, m, 0, 0, 0}; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, m, 0, 1}; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, 0, v, 2}; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { *REC(c, OP_DRAW, p_draw) = (p_draw){pr, first, n, 1, 0}; }
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    *REC(c, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){pr, it, n, bv, bi, inst, ib};
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // 跳板是尾跳（br），這裡的返回地址就是遊戲裡的調用點
    // The thunk is a tail jump (br), so the return address here is the call site in the game
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){x, y, z, (uint64_t)__builtin_return_address(0)};
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(int32_t *)rec(c, OP_BARRIER, 8) = bits; }
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = (const dpool_t *)p; }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = (const dpool_t *)p; }

NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_B2T, p_copy_b2t, 1); o->src = src; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_T2B, p_copy_b2t, 1); o->src = dst; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
// 用給定值清貼圖區域（浮點 / 無符號整數兩版；mask = 通道寫掩碼）
// Clear a texture region with the given value (float / unsigned integer versions; mask = channel write mask)
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 0; memcpy(o->c.f, rgba, 16);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 1; memcpy(o->c.u, rgba, 16);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2); o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, (uint64_t)__builtin_return_address(0)); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, (uint64_t)__builtin_return_address(0));
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
}
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { REC(c, OP_COPY_STRIDE, p_copy_stride)->stride = st; }
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }

// ---------- 狀態對象：就地打包（nvn_obj.h） ----------
// ---------- State objects: packed in place (nvn_obj.h) ----------
#define ST(T, x) ((T *)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)n; (void)v; }
NVN(nvnMultisampleStateGetSamples, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->samples; }
NVN(nvnMultisampleStateGetMultisampleEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->enable; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("【紅】頂點屬性偏移 %td 超出打包範圍", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }

static _Atomic uint64_t g_sub_wall_ns, g_sub_cpu_ns, g_sub_instr;   // 提交（不含出圖）的牆鐘 / 本線程 CPU 時間 / 本線程退休指令數（逐幀取走） | Wall clock / this thread's CPU time / this thread's retired instruction count for submit (excluding present) (taken away per frame)
static _Atomic uint64_t g_cum_sub_instr, g_cum_sub_cpu_ns;   // 同上累計（每 300 幀匯總「翻譯」行取差，真機經日誌口讀：每次繪製指令數 / 每幀提交 CPU） | Cumulative versions of the above (the 「翻譯」(translation) summary row every 300 frames takes the difference; on a real device read through the log port: instructions per draw / submit CPU per frame)
extern int thread_selfcounts(int type, void *buf, size_t nbytes);   // 本線程累計 {指令, 週期}（libSystem 私有接口，Mac / iOS 都有） | This thread's cumulative {instructions, cycles} (libSystem private interface, present on both Mac and iOS)
static uint64_t thr_instr(void) { uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); return c[0]; }
// 提交路徑的補充讀數（真機 Metal 4 指令數 −18% 而提交 CPU 時間 +49%，要分清是內核態、執行效率還是被降頻）：
// 本線程週期數（同一接口的第二項）、內核態 CPU 時間（THREAD_BASIC_INFO）、牆鐘
// Supplementary readings for the submit path (on a real device Metal 4 instruction count is -18% while submit CPU time is +49%; need to tell apart kernel mode, execution efficiency, or frequency throttling):
// this thread's cycle count (the second item of the same interface), kernel-mode CPU time (THREAD_BASIC_INFO), wall clock
static void thr_counts(uint64_t *instr, uint64_t *cyc) { uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); *instr = c[0]; *cyc = c[1]; }
static uint64_t thr_sys_ns(void) {
    thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT; mach_port_t me = mach_thread_self(); uint64_t r = 0;
    if (thread_info(me, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS) r = (uint64_t)bi.system_time.seconds * 1000000000ull + (uint64_t)bi.system_time.microseconds * 1000ull;
    mach_port_deallocate(mach_task_self(), me); return r;
}
static _Atomic uint64_t g_cum_sub_cyc, g_cum_sub_sys_ns, g_cum_sub_wall_ns;
static int frame_rec(void);
static _Atomic uint64_t g_nvn_ns;   // 本幀在提交 / 出圖裡花的宿主時間（回放編碼 + 出圖），計時埋點用 | Host time spent in submit / present this frame (replay encoding + present), used by the timing instrumentation

// ---------- 生命週期：dm_gfx_suspend（iOS 進後台前調用；等效 Switch 掛起） ----------
// on=1：之後的提交 / 出圖阻塞到 on=0；返回前等正在進行的提交 / 出圖調用退出、並等所有在途 GPU 工作完成（iOS 要求後台無 GPU 工作）。
// ---------- Lifecycle: dm_gfx_suspend (called before iOS goes to the background; equivalent to Switch suspend) ----------
// on=1: subsequent submits / presents block until on=0; before returning, wait for in-progress submit / present calls to exit and for all in-flight GPU work to complete (iOS requires no GPU work in the background).
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_susp_cv = PTHREAD_COND_INITIALIZER;
static int g_susp, g_inflight_api;
__thread _Atomic int dm_host_work;   // 見 hle.h（定義在這裡：圖形單測只編本文件，不編 hle_thread.c） | see hle.h (defined here: the graphics unit tests compile this file but not hle_thread.c)
static void gfx_enter(void) { pthread_mutex_lock(&g_susp_mu); while (g_susp) pthread_cond_wait(&g_susp_cv, &g_susp_mu); g_inflight_api++; pthread_mutex_unlock(&g_susp_mu); dm_host_work++; }
static void gfx_leave(void) { dm_host_work--; pthread_mutex_lock(&g_susp_mu); g_inflight_api--; pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu); }
void dm_gfx_suspend(int on) {
    pthread_mutex_lock(&g_susp_mu);
    g_susp = on != 0;
    if (on) while (g_inflight_api) pthread_cond_wait(&g_susp_cv, &g_susp_mu);
    else pthread_cond_broadcast(&g_susp_cv);
    pthread_mutex_unlock(&g_susp_mu);
    if (on) { mtl_drain(); LOG("[nvn] 圖形掛起：在途 GPU 工作已清空"); } else LOG("[nvn] 圖形恢復");
}
// 多線程提交：NVN 允許各線程各用各的隊列同時提交（朋友收集開局就有兩條線程同時提交，回放層報「提交重入」中止）；
// 回放層的狀態（當前命令緩衝、上傳批、提交線程）是全局一份，所以提交整段串行。馬力歐只有一條提交線程，這把鎖從不爭用
// Submissions from several threads: NVN lets threads submit to their own queues at the same time (Tomodachi Life has two threads submitting at once right at start-up, and the replay layer aborted with "submit re-entry");
// the replay layer's state (current command buffer, upload batch, submit thread) is one global copy, so whole submissions are serialized. Mario has a single submit thread, so this lock is never contended
static pthread_mutex_t g_submit_mu = PTHREAD_MUTEX_INITIALIZER;
static void submit_body(NVNqueue *q, int n, const NVNcommandHandle *h);
NVN(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *h) {
    pthread_mutex_lock(&g_submit_mu); submit_body(q, n, h); pthread_mutex_unlock(&g_submit_mu);
}
static void submit_body(NVNqueue *q, int n, const NVNcommandHandle *h) {
    gfx_enter();
    uint64_t t0 = now_ns(), c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), i0, y0, s0 = thr_sys_ns(); thr_counts(&i0, &y0);
    ((queue_t *)q)->submits++;
    mtl_sub_begin();
    mtl_sub_index((const uint64_t *)h, n);
    for (int i = 0; i < n; i++) { mtl_exec_rest((const uint64_t *)h + i + 1, n - i - 1); mtl_exec((const uint8_t *)h[i]); }
    mtl_exec_rest(NULL, 0);
    mtl_sub_commit();
    uint64_t dw = now_ns() - t0;
    atomic_fetch_add(&g_nvn_ns, dw); atomic_fetch_add(&g_sub_wall_ns, dw);
    uint64_t i1, y1; thr_counts(&i1, &y1); uint64_t dc = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0, di = i1 - i0;
    atomic_fetch_add(&g_cum_sub_cyc, y1 - y0); atomic_fetch_add(&g_cum_sub_sys_ns, thr_sys_ns() - s0); atomic_fetch_add(&g_cum_sub_wall_ns, dw);
    atomic_fetch_add(&g_sub_cpu_ns, dc); atomic_fetch_add(&g_cum_sub_cpu_ns, dc);
    atomic_fetch_add(&g_sub_instr, di); atomic_fetch_add(&g_cum_sub_instr, di);
    gfx_leave();
}

// 只影響 GPU 狀態 / 只在 GPU 上執行的命令：空設備只計數（跳板已計）
// Commands that only affect GPU state / only execute on the GPU: the null device only counts them (the thunk has already counted)
static void nvn_nop(void) {}
static const char *const g_nops[] = {
    "nvnCommandBufferSetPatchSize",
    "nvnCommandBufferMultiDrawArraysIndirectCount",
    "nvnCommandBufferMultiDrawElementsIndirectCount",
    "nvnCommandBufferDispatchComputeIndirect",
    "nvnCommandBufferSetDepthBounds",
    "nvnCommandBufferWaitSync",
    "nvnCommandBufferSetShaderScratchMemory",
    "nvnCommandBufferPushDebugGroupDynamic",
    "nvnCommandBufferPopDebugGroup",
    "nvnCommandBufferInsertDebugMarkerDynamic",
    "nvnCommandBufferInsertDebugMarker",   // 十三機兵防衛圈 | 13 Sentinels
    "nvnCommandBufferPushDebugGroup",      // 超級馬力歐 3D 世界 + 狂怒世界（NVN 53） | Super Mario 3D World + Bowser's Fury (NVN 53)
    "nvnCommandBufferPushDebugGroupStatic",
    "nvnCommandBufferPopDebugGroupId",
    "nvnCommandBufferInsertDebugMarkerStatic",
    "nvnCommandBufferWaitEvent",
    "nvnWindowFinalize",
};
__attribute__((constructor)) static void nvn_nops_register(void) {
    // 空實現在槽表初始化後覆蓋未實現樁；名字寫錯（不在 main 名字表裡）啓動即報紅中止
    // Empty implementations overwrite the unimplemented stubs after the slot table is initialized; a misspelled name (not in main's name table) reports a failure and aborts at startup
    nvn_init_slots();
    for (unsigned k = 0; k < sizeof g_nops / sizeof *g_nops; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], g_nops[k])) i++;
        if (i == NVN_NFUNC) die("NVN 空實現名 %s 不在 main 名字表裡", g_nops[k]);
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry) die("NVN %s 既有實現又列為空實現", g_nops[k]);
        nvn_slot[i].fn = (void *)nvn_nop;
    }
    unsigned nnop = sizeof g_nops / sizeof *g_nops;
    // 錄製命令（nvnCommandBuffer*，每幀上萬次，宿主只追加記錄）以外的已實現函數走宿主大棧：提交 / 出圖 / 建對象會做 Metal 工作，遊戲線程的棧可能太小（見 nvn_entry.S nvn_big_entry，上游 d6cd8a8）
    // Implemented functions other than command recording (nvnCommandBuffer*, tens of thousands per frame, where the host only appends records) run on the big host stack: submit / present / object creation do Metal work, and a game thread's stack may be too small (see nvn_entry.S nvn_big_entry, upstream d6cd8a8)
    unsigned nbig = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && nvn_slot[i].fn != (void *)nvn_nop && strncmp(nvn_names[i], "nvnCommandBuffer", 16)) {
            nvn_big_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_big_entry; nbig++;
        }
    // DM_NVN_PROF=1：已實現的函數（未實現樁與出圖除外：出圖含節拍等待，單獨計）套計時包裝，用於拆「翻譯層錄製期」耗時
    // DM_NVN_PROF=1: implemented functions (except unimplemented stubs and present: present includes the display-link wait and is counted separately) get a timing wrapper, used to break down the time spent in the "translation layer recording phase"
    if (getenv("DM_NVN_PROF"))
        for (unsigned i = 0; i < NVN_NFUNC; i++)
            if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && strcmp(nvn_names[i], "nvnQueuePresentTexture")) { nvn_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_prof_entry; }
    LOG("[nvn] 空設備：名字 %u，已實現 %u（語義 %u + 空實現 %u，其中 %u 個走宿主大棧），其餘 %u 個接報紅樁",
        NVN_NFUNC, g_nsem + nnop, g_nsem, nnop, nbig, NVN_NFUNC - g_nsem - nnop);
}

// ---------- 窗口 / 出圖 ----------
// ---------- Window / present ----------
#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("【紅】WindowBuilderSetTextures 數量 %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    for (int i = 0; i < o->ntex; i++) if (o->tex[i]) mtl_xv_window_tex(CTX(o->tex[i])->serial);
    LOG("[nvn] WindowInitialize 交換鏈 %d 張 間隔 %d 首張 %dx%d 格式 %#x", o->ntex, o->interval,
        o->ntex ? (int)CTX(o->tex[0])->d.width : 0, o->ntex ? (int)CTX(o->tex[0])->d.height : 0, o->ntex ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) { if (((win_t *)w)->interval != v) LOG("[nvn] 遊戲設出圖間隔 %d → %d", ((win_t *)w)->interval, v); ((win_t *)w)->interval = v; }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
}
static void trow(uint64_t f, const char *name, double v);
static void set_time_constraint(uint64_t period_ns);
static void rt_comp_tick(uint64_t period_ns);
static double g_rt_comp_ms;   // 當前聲明的實時計算量（ms；0 = 未聲明） | Currently declared real-time computation amount (ms; 0 = not declared)
static double g_rt_period_ms;
static void sleep_until_ns(uint64_t dl);
static uint64_t present_period(const win_t *o);
__attribute__((weak)) void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = 0; *ns = 0; }   // 宿主 hle_thread.c 提供真值；圖形單測裡為 0 | The host's hle_thread.c provides the ground truth; 0 in graphics unit tests
__attribute__((weak)) uint32_t dm_guest_work_qos(unsigned *count) { if (count) *count = 0; return QOS_CLASS_UNSPECIFIED; }
extern uint32_t mtl_present_qos(void);
static uint64_t g_frame;
static uint64_t g_acq_wait_ns, g_pq_wait_ns;   // 取圖裡節拍等待 / 等出圖隊列歸還的累計，出圖時按幀記（一幀內可有多次取圖） | Accumulated display-link wait / wait for the present queue to return inside acquire, recorded per frame at present (there can be several acquires within one frame)
uint64_t dm_nvn_frame_count(void) { return g_frame; }
extern uint64_t g_t0_ns;   // 宿主進程起點（main.c；UIMSG 時間戳同基） | Host process start point (main.c; same base as the UIMSG timestamps)
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *s, int *idx) {
    win_t *o = (win_t *)w;
    *idx = o->ntex ? o->next++ % o->ntex : 0;
    // 垂直同步阻塞在這裡（真機 NVN：交換鏈裡的貼圖都在等顯示時，等下一張被釋放），不在出圖裡——遊戲常持鎖調出圖
    // Vsync blocks here (real-device NVN: when all textures in the swapchain are waiting for display, wait for the next one to be released), not in present -- the game often calls present while holding a lock
    uint64_t at = *idx < 4 ? o->avail[*idx] : 0, now = dm_detclock_enabled() ? guest_ns() : now_ns(), wait = at > now ? at - now : 0;
    if (!dm_detclock_enabled()) {   // 取圖線程登記為時間約束線程（不再只在「來早了」時才登記：熱態一直落後時也要聲明） | Register the acquire thread as a time-constraint thread (no longer registered only when it "arrived early": it must also declare itself when it is constantly behind in the hot state)
        static uint64_t rt_period; uint64_t period = present_period(o);
        if (period && rt_period != period) { rt_period = period; set_time_constraint(period); }
        rt_comp_tick(period);
        if (mtl_link_active()) { if (!mtl_link_wait_tick(period ? 2 * period : 33333333ull) && wait) sleep_until_ns(at); }   // DM_PRESENT_LINK：節拍完全跟顯示節拍（跑得快就等下一拍，落後立刻走）；自己的 60 Hz 時鐘只作等不到節拍時的兜底 | DM_PRESENT_LINK: pacing fully follows the display link (wait for the next display-link tick when running fast, go immediately when behind); our own 60 Hz clock is only the fallback when no display-link tick arrives
        else if (wait) sleep_until_ns(at);
    } else {   // DM_DETCLOCK_PACE=1（畫面閘門用）：客體時鐘按幀走的同時，每幀牆鐘不短於一個步長——幀數與遊戲內時間確定，後台加載線程每幀至少有一個步長的牆鐘可用； | DM_DETCLOCK_PACE=1 (used by the screenshot gate): while the guest clock advances per frame, each frame's wall clock is no shorter than one step -- frame count and in-game time are deterministic, and the background loading thread has at least one step of wall clock available per frame;
               // 機器滿載時幀只會更慢、後台得到的牆鐘只會更多，所以「消息後第 N 幀」截到的畫面與負載無關
               // When the machine is fully loaded frames only get slower and the background only gets more wall clock, so the picture captured at "the Nth frame after the message" is independent of load
        static int pace = -1; static uint64_t step, last;
        if (pace < 0) { const char *e = getenv("DM_DETCLOCK_PACE"), *d = getenv("DM_DETCLOCK"); step = d ? strtoull(d, 0, 0) : 0; pace = e && atoi(e) && step; }
        if (pace) { if (last && now_ns() < last + step) sleep_until_ns(last + step); last = now_ns(); }
    }
    g_acq_wait_ns += wait; trow(g_frame, "@late_ms", wait ? (now_ns() - at) / 1e6 : 0);
    // 該張上一次出圖還沒被出圖線程處理完（顯示跟不上、落後一整輪交換鏈）：等它歸還，再交給遊戲畫
    // The previous present of this texture has not yet been processed by the present thread (display cannot keep up, a whole swapchain round behind): wait for it to be returned, then hand it to the game to draw
    uint64_t tk = *idx < 4 ? o->ticket[*idx] : 0;
    if (tk) { uint64_t w0 = now_ns(); mtl_present_wait(tk); g_pq_wait_ns += now_ns() - w0; }
    if (s) { ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = mtl_last_fence(); }   // 交換鏈貼圖在此前的提交完成後可用 | The swapchain texture becomes available once the commits before this point have completed
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
// 舊 API（NVN 53，超級馬力歐 3D 世界 + 狂怒世界）：取圖由隊列在 GPU 上等，不交同步對象。宿主按提交順序回放，之後的命令本來就排在此前出圖之後，同 WindowAcquireTexture 不帶同步對象（上游 42a50ef）
// The older API (NVN 53, Super Mario 3D World + Bowser's Fury): the queue waits for the texture on the GPU and no sync object is handed out. The host replays in submission order, so later commands already come after earlier presents; the same as WindowAcquireTexture without a sync object (upstream 42a50ef)
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *q, NVNwindow *w, int *idx) {
    (void)q; return h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS ? (NVNqueueAcquireTextureResult)0 : (NVNqueueAcquireTextureResult)1;
}

// 幀邊界：每次出圖打印 / 落盤本幀各 NVN 函數調用次數
// Frame boundary: on each present, print / write to disk the call count of each NVN function for this frame
static uint64_t g_prev[NVN_NFUNC];
static FILE *g_tsv;
static void dump_frame(void) {
    static int inited, nframes = 3, calls = 1;
    if (!inited) {
        inited = 1;
        const char *e = getenv("DM_NVN_FRAMES"); if (e) nframes = atoi(e);
        const char *c = getenv("DM_NVN_TSV_CALLS"); if (c) calls = atoi(c);   // 0 = 只寫 @ 計時行（真機拉文件有大小上限，調用計數行佔約 2/3） | 0 = write only the @ timing rows (pulling files from a real device has a size limit, and the call-count rows take about 2/3)
        const char *t = getenv("DM_NVN_TSV"); if (t && !(g_tsv = fopen(t, "w"))) LOG("【紅】打不開 %s", t);
        if (g_tsv) fprintf(g_tsv, "# kind: nvn-per-frame  name 為 NVN 函數名時值 = 調用次數；@ 開頭為計時埋點（ms / 個）\nframe\tname\tvalue\n");
    }
    uint64_t total = 0; unsigned nfn = 0;
    char line[8192]; size_t o = 0; line[0] = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) {
        uint64_t c = atomic_load(&nvn_slot[i].cnt), d = c - g_prev[i];
        g_prev[i] = c;
        if (!d) continue;
        total += d; nfn++;
        if (g_tsv && calls) fprintf(g_tsv, "%llu\t%s\t%llu\n", g_frame, nvn_names[i], d);
        if (g_frame <= (uint64_t)nframes && o < sizeof line - 96) o += snprintf(line + o, sizeof line - o, " %s=%llu", nvn_names[i] + 3, d);
    }
    if (g_tsv) fflush(g_tsv);
    if (g_frame <= (uint64_t)nframes) LOG("[nvn] %s %llu：%u 個函數 %llu 次調用 |%s", g_frame ? "幀" : "初始化", g_frame, nfn, total, line);
    else if (g_frame % 300 == 0) LOG("[nvn] 幀 %llu：%u 個函數 %llu 次調用", g_frame, nfn, total);
    g_frame++; cs_dump();
    // 每 300 幀讀一次常駐內存，交圖形層打內存讀數行（mtl_mem_stat，低頻封頂）。內存看門狗：常駐超過上限就報紅自行退出——只在 Mac 宿主上缺省開（24 GB：宿主內存失控
    // 會把整機拖進內核崩潰，同機別的程序兩次如此），或測試時用 DM_MEM_CAP_GB=<整數> 指定；玩家的 iOS 包不設上限（被系統殺之前自己退沒有意義，反而多一種閃退）
    // Every 300 frames read resident memory and hand it to the graphics layer to print the memory readout line (mtl_mem_stat, low frequency, capped). Memory watchdog: when resident memory exceeds the cap, log a failure and exit on its own. On by default only on the Mac host (24 GB: runaway host memory
    // drags the whole machine into a kernel panic, which happened twice with other programs on the same machine), or set for tests with DM_MEM_CAP_GB=<integer>; the player's iOS app has no cap (exiting before the system kills it is pointless and only adds another kind of crash)
    if (g_frame % 300 == 0) { task_vm_info_data_t vi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) { unsigned long long mb = (unsigned long long)(vi.phys_footprint >> 20); void mtl_mem_stat(unsigned long long, unsigned long long); mtl_mem_stat(g_frame, mb);
            static long cap = -1; if (cap < 0) { const char *e = getenv("DM_MEM_CAP_GB"); cap = e ? atol(e) : DM_MEM_CAP_DEFAULT_GB; }
            if (cap > 0 && mb > (unsigned long long)cap * 1024) { LOG("【紅】內存看門狗：常駐 %llu MB 超過上限 %ld GB（幀 %llu），自行退出", mb, cap, (unsigned long long)g_frame); _exit(86); } } }
    dm_guest_frame_tick();   // 確定性客體時鐘（DM_DETCLOCK，宿主 hle_os.c）：每次出圖推進固定步長；未開為空操作 | Deterministic guest clock (DM_DETCLOCK, host hle_os.c): each present advances a fixed step; a no-op when not enabled
}
// ---------- 計時埋點（逐幀行寫進 DM_NVN_TSV：frame / @指標 / 值；每 300 幀匯總 p50 / p95 / 最大） ----------
// ---------- Timing instrumentation (per-frame rows written to DM_NVN_TSV: frame / @metric / value; every 300 frames a summary of p50 / p95 / max) ----------
#define TWIN 300
static double g_tw[6][TWIN]; static unsigned g_tn[6];
static double g_pres_iv[TWIN]; static unsigned g_presn;   // 上屏間隔（ms；−1 = 被系統丟棄，0 = 無讀數） | Present interval (ms; -1 = dropped by the system, 0 = no reading)
__attribute__((weak)) double dm_brightness_now(void) { return -1; }
__attribute__((weak)) void dm_perf_present(double iv_ms) { (void)iv_ms; }   // 缺省（單測）；宿主 diag.c 的強定義：常開性能記錄 perf.txt | Default (unit tests); strong definition in the host's diag.c: always-on performance record perf.txt
uint64_t dm_nvn_sub_cpu_ns(void) { return atomic_load(&g_cum_sub_cpu_ns); }   // 提交（不含出圖）累計 CPU 納秒 | Cumulative CPU nanoseconds of submit (excluding present)
__attribute__((weak)) int dm_thermal_now(void) { return -1; }   // 缺省（Mac）；iOS front_ios.m 覆蓋   // 缺省（Mac）；iOS 前端 front_ios.m 的強定義覆蓋它 | Default (Mac); overridden by iOS front_ios.m   // default (Mac); the strong definition in the iOS front end front_ios.m overrides it
// 長幀日誌的逐幀環（GPU 時間滯後 8 幀才有，所以先存後判）
// Per-frame ring for the long-frame log (GPU time only arrives 8 frames late, so store first and judge later)
#define LFN 16
typedef struct { uint64_t f; double fr, nv, t_s, make_ms, up_mb, up_ms; unsigned pso, lib, up, rup; int ls; } lf_rec;   // ls：這一幀出圖時讀取畫面是否蓋著 | ls: whether the loading screen was covering the picture when this frame was presented
static lf_rec g_lf[LFN];
__attribute__((weak)) void dm_longframe_note(const char *what) { LOG("[長幀] %s", what); }   // 缺省（圖形單測）；宿主 diag.c 的強定義補上場景上下文並寫 perf.txt | Default (graphics unit tests); the strong definition in the host's diag.c adds scene context and writes perf.txt
extern void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns); extern uint64_t mtl_upload_bytes(void); extern void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
static const char *const g_tname[6] = {"幀時間", "出幀工作", "NVN 提交+出圖", "GPU", "翻譯層錄製期", "翻譯層提交期"};
static void tpush(int k, double v) { g_tw[k][g_tn[k]++ % TWIN] = v; }
// 發熱歸因（iOS DM_HEAT）：每幀 GPU 毫秒另存一圈，取「上次取數以來」的均值 / p95（最多最近 HRN 幀），與計時匯總的 300 幀窗口互不干擾
// Thermal attribution (iOS DM_HEAT): per-frame GPU milliseconds are kept in a separate ring, taking the mean / p95 "since the last read" (at most the last HRN frames), without interfering with the timing summary's 300-frame window
#define HRN 1024
static double g_hr[HRN]; static uint64_t g_hn, g_hn_read;
static int dcmp(const void *a, const void *b);
void nvn_gpu_recent(double *avg, double *p95, unsigned *n) {
    uint64_t hi = g_hn, lo = g_hn_read > hi ? hi : g_hn_read; if (hi - lo > HRN) lo = hi - HRN; g_hn_read = hi;
    unsigned c = (unsigned)(hi - lo); static double v[HRN]; double s = 0;
    for (unsigned i = 0; i < c; i++) { v[i] = g_hr[(lo + i) % HRN]; s += v[i]; }
    qsort(v, c, sizeof *v, dcmp); *n = c; *avg = c ? s / c : -1; *p95 = c ? v[(c * 95) / 100 < c ? (c * 95) / 100 : c - 1] : -1;
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void tsummary(uint64_t frame) {
    char line[1024]; size_t o = 0;
    for (int k = 0; k < 6; k++) {
        unsigned n = g_tn[k] < TWIN ? g_tn[k] : TWIN; if (!n) continue;
        double v[TWIN]; memcpy(v, g_tw[k], n * sizeof *v); qsort(v, n, sizeof *v, dcmp);
        o += snprintf(line + o, sizeof line - o, " %s p50 %.2f / p95 %.2f / 最大 %.2f ms（%u 幀）；", g_tname[k], v[n / 2], v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], v[n - 1], n);
    }
    LOG("[nvn] 計時 至幀 %llu：%s", frame, line);
    double tgt = 1000.0 / mtl_fps_target();   // 一幀該佔的毫秒數：60 幀 16.7、遊戲自己要 30 幀時 33.3（下面的超時 / 掉幀都按它算） | milliseconds one frame should take: 16.7 at 60 fps, 33.3 when the game itself asks for 30 (the over-time / dropped-frame counts below use it)
    { unsigned n = g_tn[0] < TWIN ? g_tn[0] : TWIN, over = 0; double v[TWIN];   // 幀間隔另起一行（M1 基線）：本窗口超過一幀時長的幀數與 p99 | frame interval gets its own line (M1 baseline): number of frames longer than one frame period in this window and p99
      if (n) { memcpy(v, g_tw[0], n * sizeof *v); qsort(v, n, sizeof *v, dcmp); for (unsigned i = 0; i < n; i++) over += v[i] > tgt;
        LOG("[nvn] 幀間隔 至幀 %llu：>%.1fms %u / %u 幀，p99 %.2f ms", frame, tgt, over, n, v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1]); } }
    { unsigned n = g_presn < TWIN ? g_presn : TWIN, k = 0, drop = 0, discard = 0; double v[TWIN];   // 上屏：掉幀 = 間隔 > 一幀時長 × 1.5 或被丟棄 | on-screen present: dropped frame = interval > one frame period × 1.5 or discarded
      for (unsigned i = 0; i < n; i++) { double x = g_pres_iv[i]; if (x < 0) { drop++; discard++; } else if (x > 0) { v[k++] = x; drop += x > tgt * 1.5; } }
      task_vm_info_data_t vi; mach_msg_type_number_t vc = TASK_VM_INFO_COUNT; double fp = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &vc) == KERN_SUCCESS ? vi.phys_footprint / 1048576.0 : -1;
      double br = dm_brightness_now();   // footprint / 亮度隨上屏行帶出：不依賴拍入口觀測門（營地局也有） | footprint / brightness are carried on the on-screen present row: does not depend on the tick-entry observation gate (camp runs have it too)
      if (k) { qsort(v, k, sizeof *v, dcmp); LOG("[nvn] 上屏 至幀 %llu：%u 幀，掉幀 %u（丟棄 %u），p99 %.2f ms；footprint %.0f MiB；亮度 %.2f；溫控 %d", frame, k + discard, drop, discard, v[(k * 99) / 100 < k ? (k * 99) / 100 : k - 1], fp, br, dm_thermal_now()); }
      g_presn = 0; }
    { static uint64_t pi, pc, pd, pf; uint64_t ci = atomic_load(&g_cum_sub_instr), cc = atomic_load(&g_cum_sub_cpu_ns), ps_, dr_; mtl_stats(&ps_, &dr_);   // 翻譯層 | translation layer
      if (pf && frame > pf && dr_ > pd) LOG("[nvn] 翻譯 至幀 %llu：每次繪製 %.0f 條指令，每幀提交 %.2f M 條 / CPU %.3f ms，每幀繪製 %.0f", frame,
          (double)(ci - pi) / (double)(dr_ - pd), (ci - pi) / 1e6 / (double)(frame - pf), (cc - pc) / 1e6 / (double)(frame - pf), (double)(dr_ - pd) / (double)(frame - pf));
      { static uint64_t py, pk, pw; uint64_t cy = atomic_load(&g_cum_sub_cyc), ck = atomic_load(&g_cum_sub_sys_ns), cw = atomic_load(&g_cum_sub_wall_ns);
        if (pf && frame > pf && ci > pi && cy > py) { double nf = (double)(frame - pf), cyc = (double)(cy - py), cpu = (double)(cc - pc);
          qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp);
          uint64_t aw = 0, ag = 0; int an = 0; void mtl_alloc_stats(uint64_t *, uint64_t *, int *); mtl_alloc_stats(&aw, &ag, &an);
          static uint64_t pyn, pyns; uint64_t yn = 0, yns = 0; dm_main_yield_stats(&yn, &yns);
          LOG("[nvn] 翻譯細項 至幀 %llu：每幀提交 週期 %.2f M（每條指令 %.2f 週期）、CPU %.3f ms 其中內核態 %.3f ms、牆鐘 %.3f ms；平均主頻 %.2f GHz（週期 / CPU 時間）；本線程 QoS %#x；分配器池 %d 個、累計等 GPU %llu 次；實時聲明計算 %.2f ms；主線程讓出 每幀 %.0f 次 / %.3f ms", frame,
              cyc / 1e6 / nf, cyc / (double)(ci - pi), cpu / 1e6 / nf, (double)(ck - pk) / 1e6 / nf, (double)(cw - pw) / 1e6 / nf, cpu > 0 ? cyc / cpu : 0, (unsigned)qc, an, (unsigned long long)aw, g_rt_comp_ms, (double)(yn - pyn) / nf, (double)(yns - pyns) / 1e6 / nf); pyn = yn; pyns = yns; }
        if (frame % 300 == 0) { unsigned wn = 0; uint32_t wq = dm_guest_work_qos(&wn); thread_extended_info_data_t xi; mach_msg_type_number_t xc = THREAD_EXTENDED_INFO_COUNT; int okx = thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO, (thread_info_t)&xi, &xc) == KERN_SUCCESS;
            thread_time_constraint_policy_data_t tp; mach_msg_type_number_t tc = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def = 0; int rt_live = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tp, &tc, &def) == KERN_SUCCESS && !def;   // def = 1：實時策略沒在生效（沒設上 / 被撤） | def = 1: the real-time policy is not in effect (never set / revoked)
            LOG("[nvn] 線程策略：主線程 實時 計算 %.2f ms / 週期 %.2f ms，實時策略%s，當前優先級 %d（基礎 %d，調度類 %d）；工作線程 %u 條 QoS %#x；出圖 QoS %#x", g_rt_comp_ms, g_rt_period_ms, rt_live ? "在生效" : "未生效", okx ? xi.pth_curpri : -1, okx ? xi.pth_priority : -1, okx ? xi.pth_policy : -1, wn, wq, mtl_present_qos()); }
        py = cy; pk = ck; pw = cw; }
      pi = ci; pc = cc; pd = dr_; pf = frame; }
    uint64_t up, re, fa, pr; mtl_pkg_stats(&up, &re); nvn_dirty_stats(&fa, &pr);
    LOG("[nvn] 預排布貼圖 首傳 %llu 張、池內容變化重傳 %llu 次；寫保護缺頁 %llu 次、宿主寫前解保護 %llu 頁；漏檢 %llu（累計）", up, re, fa, pr, mtl_pkg_miss());
    memset(g_tn, 0, sizeof g_tn);
}
// DM_SLOWFRAME=1（測試開關，真機經日誌端口讀）：逐幀計時先存近 16 幀環；滯後 8 幀（GPU / 上屏時刻已回）判掉幀，
// 只對掉幀寫一行「[slowframe]\t幀號\t@名=值…」進日誌，另每 300 幀一行分母。不寫逐幀文件：日誌 tee 每行 fsync，逐行寫會擾動讀數
// DM_SLOWFRAME=1 (test switch, read over the log port on a real device): per-frame timings are first stored in a ring of the last 16 frames; dropped frames are judged 8 frames late (GPU / on-screen present times are back by then),
// and only for dropped frames is one line "[slowframe]\t<frame number>\t@<name>=<value>..." written to the log, plus one denominator line every 300 frames. No per-frame file is written: the log tee fsyncs every line, and writing line by line would disturb the readings
#define SRN 16
static struct { uint64_t f; int n; const char *k[64]; double v[64]; } g_sr[SRN];
static int g_slow = -1;
static int slow_on(void) { if (g_slow < 0) g_slow = getenv("DM_SLOWFRAME") != NULL; return g_slow; }
static int frame_rec(void) { return g_tsv || slow_on(); }
static void trow(uint64_t f, const char *name, double v) {
    if (g_tsv) fprintf(g_tsv, "%llu\t%s\t%.3f\n", f, name, v);
    if (slow_on()) { __typeof__(g_sr[0]) *r = &g_sr[f % SRN]; if (r->f != f) { r->f = f; r->n = 0; } if (r->n < 64) { r->k[r->n] = name; r->v[r->n++] = v; } }
}
static double srow(uint64_t f, const char *name, double dflt) {
    __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
    if (r->f == f) for (int i = 0; i < r->n; i++) if (!strcmp(r->k[i], name)) return r->v[i];
    return dflt;
}
// 掉幀口徑：上屏間隔 > 一幀時長（60 幀 16.7 ms、30 幀 33.3 ms）×1.5 或 −1；無窗口（無上屏時刻）退回出圖調用間隔
// Dropped-frame definition: on-screen present interval > one frame period (16.7 ms at 60 fps, 33.3 ms at 30) ×1.5 or −1; without a window (no on-screen present time) fall back to the present call interval
static void slow_emit(uint64_t f) {
    static uint64_t judged, slow;
    double pm = srow(f, "@presented_ms", 0), fr = srow(f, "@frame_ms", 0), lim = 1000.0 / mtl_fps_target() * 1.5;
    int has = mtl_has_window(), bad = has ? (pm > lim || pm < 0) : fr > lim;
    if (has ? pm != 0 : fr != 0) judged++;
    if (bad) {
        slow++;
        char b[4096]; size_t o = snprintf(b, sizeof b, "[slowframe]\t%llu", f);
        __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
        for (int i = 0; i < r->n && o < sizeof b - 64; i++) o += snprintf(b + o, sizeof b - o, "\t%s=%.3f", r->k[i], r->v[i]);
        double m = 0; for (uint64_t p = f >= 3 ? f - 3 : 0; p < f; p++) { double x = srow(p, "@frame_ms", 0); if (x > m) m = x; }
        snprintf(b + o, sizeof b - o, "\t@prev3_max_frame_ms=%.3f", m);
        fprintf(stderr, "%s\n", b);
    }
    if (f % 300 == 0) fprintf(stderr, "[slowframe]\t#\t至幀 %llu\tjudged=%llu\tslow=%llu\t判據=%s\n", f, judged, slow, has ? "上屏間隔" : "出圖調用間隔（無窗口）");
}
// 時間約束（實時）線程的聲明：週期 = 出圖週期，約束 = 週期，計算 = 這條線程每幀實際要用的 CPU 時間。
// 原先計算量寫死 2 ms，而遊戲主線程（遊戲邏輯 + 錄製 + 提交回放）熱態每幀約 10 ms——聲明與實際差 5 倍：
// 實時線程超出自己聲明的計算量會被系統降級處理，性能控制器也按聲明定頻（系統就是靠這份聲明得知幀期限和工作量的）。
// DM_RT_COMP=<毫秒> 固定聲明；缺省 auto = 每 120 幀取本線程每幀 CPU 時間的 p90，與已聲明值差 > 15% 就重新聲明（上限 0.9 × 週期）。
// Declaration for the time-constraint (real-time) thread: period = present period, constraint = period, computation = the CPU time this thread actually needs per frame.
// The computation used to be hard-coded at 2 ms, while the game main thread (game logic + recording + submit replay) takes about 10 ms per frame when hot - a 5× gap between declared and actual:
// a real-time thread that exceeds its declared computation gets demoted by the system, and the performance controller also sets frequency from the declaration (this declaration is how the system learns the frame deadline and the workload).
// DM_RT_COMP=<ms> fixes the declaration; default auto = every 120 frames take the p90 of this thread's per-frame CPU time and re-declare if it differs from the declared value by > 15% (capped at 0.9 × period).
static void set_time_constraint_comp(uint64_t period_ns, uint64_t comp_ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    if (comp_ns < 1000000ull) comp_ns = 1000000ull; if (comp_ns > period_ns * 9 / 10) comp_ns = period_ns * 9 / 10;
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(period_ns * tb.denom / tb.numer), .computation = (uint32_t)(comp_ns * tb.denom / tb.numer),
        .constraint = (uint32_t)(period_ns * tb.denom / tb.numer), .preemptible = 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) { g_rt_comp_ms = comp_ns / 1e6; g_rt_period_ms = period_ns / 1e6; }
    LOG("[nvn] 出圖線程時間約束策略（週期 %.3f ms、計算 %.3f ms）%s", period_ns / 1e6, comp_ns / 1e6, kr == KERN_SUCCESS ? "已設" : "設置失敗");
}
static int rt_comp_fixed_ms(void) { static int v = -2; if (v == -2) { const char *e = getenv("DM_RT_COMP"); v = e && *e && strcmp(e, "auto") ? atoi(e) : -1; } return v; }   // −1 = auto
static void set_time_constraint(uint64_t period_ns) { int f = rt_comp_fixed_ms(); set_time_constraint_comp(period_ns, f > 0 ? (uint64_t)f * 1000000ull : 2000000ull); }   // 首次：固定值，或 auto 的起始值 2 ms | first time: the fixed value, or auto's starting value of 2 ms
// 每幀在取圖處調：記本線程自上次以來的 CPU 時間；auto 模式下按 p90 重新聲明
// Called every frame at acquire: records this thread's CPU time since the last call; in auto mode re-declares based on p90
static void rt_comp_tick(uint64_t period_ns) {
    if (rt_comp_fixed_ms() > 0 || !period_ns || !g_rt_comp_ms) return;
    static uint64_t last; static double win[120]; static int n;
    uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); if (!last) { last = c; return; }
    win[n++] = (double)(c - last) / 1e6; last = c;
    if (n < 120) return;
    double v[120]; memcpy(v, win, sizeof v); qsort(v, 120, sizeof *v, dcmp); n = 0;
    double p90 = v[107], comp = p90 * 1.25, cap = period_ns * 0.9 / 1e6; if (comp > cap) comp = cap; if (comp < 1) comp = 1;
    if (comp > g_rt_comp_ms * 1.15 || comp < g_rt_comp_ms * 0.85) set_time_constraint_comp(period_ns, (uint64_t)(comp * 1e6));
}
static void sleep_until_ns(uint64_t dl) {   // 單調時鐘上的絕對時刻 | absolute time on the monotonic clock
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t now = now_ns(); if (dl <= now) return;
    mach_wait_until(mach_absolute_time() + (dl - now) * tb.denom / tb.numer);
}
// 出圖固定 60 Hz：自適應機制在 24/24 份真機日誌第 60 幀退檔且不再回升，本期先以 60 幀為硬底。
// DM_PRESENT_HZ=0（不限幀率）只作顯式的性能剖析開關：遊戲部分界面邏輯按幀計數（同存檔不限幀率時
// SaveIndicatorPanel 4181 條、限 60 幀時 0 條，與真值一致），不限幀率在語義上是錯的跑法，不能作判據 / 對拍 / 缺省。單測用 nvn_present_config 直接設
// Present is fixed at 60 Hz: the adaptive mechanism stepped down at frame 60 in 24/24 real-device logs and never came back up, so for this phase 60 fps is the hard floor.
// DM_PRESENT_HZ=0 (uncapped frame rate) serves only as an explicit performance profiling switch: some of the game's UI logic counts frames (with the same save, uncapped gives
// 4181 SaveIndicatorPanel entries and capped at 60 fps gives 0, which matches the ground truth), so uncapped is a semantically wrong way to run and must not be used as a criterion / cross-check / default. Unit tests set it directly with nvn_present_config
static int g_hz = -1, g_display_mode;
static _Atomic int g_game_itv = 1;   // 按遊戲間隔出幀時，遊戲此刻設的出圖間隔（game 模式下 2 = 30 幀，nvn_game_fps 報給圖形層定顯示節拍） | when pacing by the game's interval, the interval the game currently sets (in game mode 2 = 30 fps; nvn_game_fps reports it to the graphics layer, which sets the display link from it)
void nvn_present_config(int hz, int display_mode) { g_hz = hz; g_display_mode = display_mode; }
static _Atomic uint64_t g_itv2_since;   // 遊戲這一段連續設間隔 ≥ 2 的起點（now_ns；0 = 此刻是 1） | start of the game's current run of intervals >= 2 (now_ns; 0 = it is 1 now)
// 遊戲自己要的幀數：game 模式且出圖間隔 ≥ 2 持續滿 1 秒時 30（顯示節拍隨之 30 Hz，mtl_fps_target），否則 60；間隔回到 1 立即 60。間隔 3 以上（20 幀以下）沒有對應的節拍檔，按 30 走。
// 要持續 1 秒：馬力歐 1.2.1 按上一幀的耗時動態選間隔（main+0xcfd24），一幀慢了下一幀設 2、再設回 1（iPhone 兩分鐘 123 次）。逐幀跟著切顯示節拍檔會來回抖；
//   單幀的間隔 2 由顯示節拍那邊按這一幀的週期多留一拍（nvn_mtl.m 節拍回調），不靠切檔（上游 c7e5b57）
// The frame rate the game itself asks for: 30 in game mode when the present interval has been >= 2 for a full second (the display link follows at 30 Hz, mtl_fps_target), otherwise 60; back to 60 at once when the interval returns to 1. Intervals of 3 or more (20 fps or less) have no matching display-link mode and run at 30.
// The one-second hold: Mario 1.2.1 picks the interval from the previous frame's time (main+0xcfd24), setting 2 for the frame after a slow one and then 1 again (123 times in two minutes on an iPhone). Following that frame by frame would flap the display-link mode;
//   a single interval-2 frame is instead kept on screen for its own period by the display-link callback (nvn_mtl.m), without changing the mode (upstream c7e5b57)
int nvn_game_fps(void) { uint64_t s = atomic_load(&g_itv2_since); return g_hz > 0 && !g_display_mode && s && now_ns() - s >= 1000000000ull ? 30 : 60; }
static void present_cfg(const win_t *o) {
    if (g_hz >= 0) return;
    // 間隔模式按遊戲檔案（present_interval，缺省 game）：是否按遊戲設的出圖間隔出幀，取決於遊戲邏輯是否按幀計數，各遊戲不同。環境變量可覆蓋，也可覆蓋固定 60 Hz。
    // The interval mode comes from the game profile (present_interval, default game): whether frames follow the present interval the game sets depends on whether the game's logic counts frames, which differs per game. Environment variables override it, and can override the fixed 60 Hz.
    int scr = mtl_display_hz();
    const char *e = getenv("DM_PRESENT_HZ"); g_hz = e ? atoi(e) : 60;
    if (!g_hz) LOG("[nvn] 【警告】DM_PRESENT_HZ=0：不限幀率，幀節奏非真機，界面邏輯可能偏離（僅供性能剖析，不能用於判據 / 對拍）");
    // 剪口（有意偏離原版行為）：DM_PRESENT_INTERVAL=display 時不按遊戲設的出圖間隔，每個節拍出一幀，
    // 固定 60 幀硬底；邏輯 25 拍不變，且客戶端按幀計數的界面邏輯不受影響——
    // 同存檔同輸入下兩模式 UI 消息流逐條一致。證明（選角前）：r9 game / display 60 ×3 / display 120 ×2 共 6 組 uicmp 一致
    // （存檔指示按時序競態豁免），關鍵消息時刻差在 game 自身噪聲帶內；進局後的證明尚缺，待動作腳本覆蓋
    // Cut (intentional deviation from the original game's behavior): with DM_PRESENT_INTERVAL=display the present interval set by the game is not followed and one frame is presented per display-link tick,
    // with a fixed hard floor of 60 fps; the logic stays at 25 ticks, and the client's frame-counted UI logic is unaffected -
    // with the same save and the same input, the UI message streams of the two modes match message by message. Proof (before character select): r9 game / display 60 ×3 / display 120 ×2, 6 groups in total, uicmp consistent
    // (the save indicator is exempted as a timing race), and the timing differences of key messages are within game mode's own noise band; proof after entering a run is still missing, pending action script coverage
    const char *m = getenv("DM_PRESENT_INTERVAL"); g_display_mode = m ? !strcmp(m, "display") : DM_GAME_PRESENT_DISPLAY;
    LOG("[nvn] 出圖節拍 %d Hz，間隔模式 %s（%s；遊戲設的出圖間隔 %d；%s）", g_hz, g_display_mode ? "display（每節拍一幀，剪口）" : "game（原版語義）",
        m ? "DM_PRESENT_INTERVAL" : "遊戲檔案 present_interval", o->interval, scr > 0 ? "有窗口，固定 60 Hz" : "無窗口，固定 60 Hz");
}
static uint64_t present_period(const win_t *o) {
    present_cfg(o);
    int was = atomic_load(&g_game_itv), want = o->interval > 0 ? o->interval : 1;
    if (want != was) {
        atomic_store(&g_game_itv, want);
        if (want < 2) atomic_store(&g_itv2_since, 0);
        else if (was < 2) atomic_store(&g_itv2_since, now_ns() | 1);   // | 1：起點不會是 0 | | 1: the start is never 0
    }
    int itv = g_display_mode ? 1 : o->interval > 0 ? o->interval : 1, hz = g_hz;
    return hz > 0 ? (uint64_t)itv * 1000000000ull / (uint64_t)hz : 0;
}
int (*g_nvn_loadscreen_up)(void);   // 宿主登記：讀取畫面此刻是否開著（diag.c 訂閱界面消息時填；圖形單測裡為空 = 不設閘） | host registration: whether the loading screen is up right now (filled in when diag.c subscribes to UI messages; null in graphics unit tests = no gate)
NVN(nvnQueuePresentTexture, void, NVNqueue *q, NVNwindow *w, int idx) {
    (void)q; win_t *o = (win_t *)w; o->presents++;
    static uint64_t prev_in, prev_out, prev_pass, prev_draw;
    // 第一次進局的讀取畫面：後台管線預熱沒跑完就在這裡等它（讀取畫面多停一會兒），進局後不在提交線程現編管線。預熱早已完成時一次判斷即過
    // Loading screen on first entering a run: if the background pipeline warm-up has not finished, wait for it here (the loading screen stays a bit longer), so that after entering the run no pipelines are compiled on the fly on the submit thread. When warm-up finished long ago this passes with a single check
    { static int gate;
      if (!gate && g_nvn_loadscreen_up && g_nvn_loadscreen_up()) { gate = 1; int ms = mtl_warm_wait(15000); if (ms) LOG("[nvn] 進局讀取畫面等管線預熱 %d ms", ms); } }
    present_cfg(o);
    {   // 測試開關 DM_TEST_PRESENT_LAG=每 N 次出圖,毫秒：在第 N、2N…次出圖前多睡這麼久，模仿真機偶爾慢一幀（馬力歐 1.2.1 隨後設出圖間隔 2，驗顯示節拍按間隔留拍）。產品不設
        // Test switch DM_TEST_PRESENT_LAG=every N presents,milliseconds: sleep that long before the Nth, 2Nth… present, imitating an occasional slow frame on a phone (Mario 1.2.1 then sets present interval 2; checks that the display link holds frames by interval). Not set in the product
        static int lag_n = -1, lag_ms; if (lag_n < 0) { const char *e = getenv("DM_TEST_PRESENT_LAG"); lag_n = 0; if (e && sscanf(e, "%d,%d", &lag_n, &lag_ms) == 2 && lag_n > 0) LOG("DM_TEST_PRESENT_LAG：每 %d 次出圖多睡 %d ms", lag_n, lag_ms); else lag_n = 0; }
        if (lag_n && o->presents % lag_n == 0) usleep((useconds_t)lag_ms * 1000);
    }
    gfx_enter();
    uint64_t t0 = now_ns();
    if (idx >= 0 && idx < o->ntex && idx < 4) o->ticket[idx] = mtl_present(CTX(o->tex[idx]), present_period(o) / 1e9, o->crop);   // 只入隊，立即返回 | enqueue only, return immediately
    gfx_leave();
    atomic_fetch_add(&g_nvn_ns, now_ns() - t0);
    uint64_t f = g_frame, ps, dr; mtl_stats(&ps, &dr);
    if (nvn_real[0] || nvn_real[1]) {   // DM_NVN_PROF：錄製期 = 除提交外全部已實現函數耗時；提交期 = QueueSubmitCommands | DM_NVN_PROF: record phase = time of all implemented functions except submit; submit phase = QueueSubmitCommands
        static unsigned isub = ~0u; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        if (isub == ~0u) for (isub = 0; isub < NVN_NFUNC && strcmp(nvn_names[isub], "nvnQueueSubmitCommands"); isub++) {}
        uint64_t rec = 0, sub = 0;
        static uint64_t acc[NVN_NFUNC];
        for (unsigned i = 0; i < NVN_NFUNC; i++) { uint64_t t = atomic_exchange(&nvn_prof_ticks[i], 0); acc[i] += t; if (i == isub) sub = t; else rec += t; }
        if ((g_frame + 1) % TWIN == 0) {   // 每 300 幀列出耗時最多的 5 個函數 | every 300 frames list the 5 most time-consuming functions
            char line[1024]; size_t o = 0;
            for (int k = 0; k < 5; k++) { unsigned best = 0; for (unsigned i = 1; i < NVN_NFUNC; i++) if (acc[i] > acc[best]) best = i;
                if (!acc[best]) break; o += snprintf(line + o, sizeof line - o, " %s %.2f ms；", nvn_names[best] + 3, acc[best] * tb.numer / tb.denom / 1e6); acc[best] = 0; }
            LOG("[nvn] 翻譯層耗時前 5（近 300 幀合計）：%s", line); memset(acc, 0, sizeof acc);
        }
        trow(g_frame, "@rec_ms", rec * tb.numer / tb.denom / 1e6); trow(g_frame, "@submit_ms", sub * tb.numer / tb.denom / 1e6);
        tpush(4, rec * tb.numer / tb.denom / 1e6); tpush(5, sub * tb.numer / tb.denom / 1e6);
    }
    if (prev_in) {
        double fr = (t0 - prev_in) / 1e6, wk = (t0 - prev_out) / 1e6, nv = atomic_exchange(&g_nvn_ns, 0) / 1e6;
        tpush(0, fr); tpush(1, wk); tpush(2, nv);
        trow(f, "@frame_ms", fr); trow(f, "@work_ms", wk); trow(f, "@nvn_ms", nv);
        // 慢幀歸因（只在開 DM_NVN_TSV 時取）：本幀出圖線程 CPU 時間（換線程記 −1）、進程全部線程 CPU、取圖等待按幀累計、出圖調用時刻（與 GPU / 上屏時刻同基：進程起點 ms）
        // Slow-frame attribution (taken only when DM_NVN_TSV is on): this frame's present-thread CPU time (−1 if the thread changed), CPU of all threads in the process, acquire wait accumulated per frame, present call time (same base as the GPU / on-screen present times: ms since process start)
        trow(f, "@wait_ms", g_acq_wait_ns / 1e6); trow(f, "@pq_wait_ms", g_pq_wait_ns / 1e6);
        if (frame_rec()) {
            static pthread_t pth; static uint64_t pcpu, pproc;
            uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); pthread_t th = pthread_self();
            trow(f, "@cpu_ms", pcpu && pthread_equal(th, pth) ? (c - pcpu) / 1e6 : -1); pth = th; pcpu = c;
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            uint64_t pc = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
            if (pproc) trow(f, "@proc_cpu_ms", (pc - pproc) / 1e6); pproc = pc;
            trow(f, "@call_ms", (t0 - g_t0_ns) / 1e6);
        }
        trow(f, "@passes", (double)(ps - prev_pass)); trow(f, "@draws", (double)(dr - prev_draw));
        static uint64_t pl, pst, pf, pudc; uint64_t l, st, fu; mtl_bw(&l, &st, &fu);
        trow(f, "@rt_load_mb", (l - pl) / 1048576.0); trow(f, "@rt_store_mb", (st - pst) / 1048576.0); trow(f, "@pass_full_rw", (double)(fu - pf));
        uint64_t udc = mtl_udc_saved_bytes(); trow(f, "@unwritten_dc_mb", (udc - pudc) / 1048576.0);
        pl = l; pst = st; pf = fu; pudc = udc;
        static uint64_t pae, par; uint64_t ae_, ar_; mtl_args_stats(&ae_, &ar_);
        trow(f, "@args_enc", (double)(ae_ - pae)); trow(f, "@args_reuse", (double)(ar_ - par)); pae = ae_; par = ar_;
        static uint64_t puc, pus; uint64_t uc_, us_; mtl_use_stats(&uc_, &us_);
        static uint64_t pdo; uint64_t do_ = mtl_ds_omit(); trow(f, "@ds_omit", (double)(do_ - pdo)); pdo = do_;
        trow(f, "@use_call", (double)(uc_ - puc)); trow(f, "@use_skip", (double)(us_ - pus)); puc = uc_; pus = us_;
        static uint64_t pfa, pre_; uint64_t fa_, pr_, up_, re_; nvn_dirty_stats(&fa_, &pr_); mtl_pkg_stats(&up_, &re_);
        trow(f, "@faults", (double)(fa_ - pfa)); trow(f, "@reuploads", (double)(re_ - pre_)); pfa = fa_; pre_ = re_;
        static uint64_t pmc, pmp, pmn; uint64_t mc_, mpg_, mn_; nvn_dirty_mp_stats(&mc_, &mpg_, &mn_);
        trow(f, "@mprotect_calls", (double)(mc_ - pmc)); trow(f, "@mprotect_pages", (double)(mpg_ - pmp)); trow(f, "@mprotect_ms", (mn_ - pmn) / 1e6); pmc = mc_; pmp = mpg_; pmn = mn_;
        double up_ms = mtl_upload_ns_take() / 1e6; trow(f, "@upload_ms", up_ms);
        trow(f, "@upload_gpu_ms", mtl_upload_gpu_ns_take() / 1e6);
        // 分賬：本幀提交線程同步創建數 + 與 UIMSG 同一時鐘的時刻（離線按讀取畫面區間把幀分成 前端 / 讀取 / 局內）
        // Accounting: number of synchronous creations on the submit thread this frame + a timestamp on the same clock as UIMSG (offline, frames are split into front end / loading / in-run by the loading screen intervals)
        static uint64_t plib, ppso; uint64_t lib_, pso_; mtl_create_stats(&lib_, &pso_);
        trow(f, "@lib_new", (double)(lib_ - plib)); trow(f, "@pso_new", (double)(pso_ - ppso));
        {   // 長幀日誌：本幀各項先記進環，等 8 幀後 GPU 時間到了再決定打不打（見下） | long-frame log: this frame's items are first recorded into the ring; whether to print is decided 8 frames later, once the GPU time has arrived (see below)
            static uint64_t plns, ppns, pup, prup, pub; uint64_t lns, pns, up, rup, ub; mtl_create_ns(&lns, &pns); mtl_pkg_stats(&up, &rup); ub = mtl_upload_bytes();
            lf_rec *r = &g_lf[f % LFN]; r->f = f; r->fr = fr; r->nv = nv; r->ls = g_nvn_loadscreen_up ? g_nvn_loadscreen_up() : 0; r->t_s = (now_ns() - g_t0_ns) / 1e9; r->pso = (unsigned)(pso_ - ppso); r->lib = (unsigned)(lib_ - plib);
            r->make_ms = ((lns - plns) + (pns - ppns)) / 1e6; r->up = (unsigned)(up - pup); r->rup = (unsigned)(rup - prup); r->up_mb = (ub - pub) / 1048576.0; r->up_ms = up_ms;
            plns = lns; ppns = pns; pup = up; prup = rup; pub = ub;
        }
        plib = lib_; ppso = pso_;
        trow(f, "@t_s", (now_ns() - g_t0_ns) / 1e9);
        trow(f, "@drawable_ms", mtl_drawable_ns_take() / 1e6);
        trow(f, "@sub_wall_ms", atomic_exchange(&g_sub_wall_ns, 0) / 1e6); trow(f, "@sub_cpu_ms", atomic_exchange(&g_sub_cpu_ns, 0) / 1e6); trow(f, "@sub_instr", (double)atomic_exchange(&g_sub_instr, 0));
    } else atomic_store(&g_nvn_ns, 0);
    g_acq_wait_ns = g_pq_wait_ns = 0;
    if (f >= 8) { double g = mtl_gpu_ms(f - 8); if (g >= 0) { tpush(3, g); trow(f - 8, "@gpu_ms", g); g_hr[g_hn++ % HRN] = g; }   // GPU 時間滯後 8 幀取（已完成） | GPU time is taken 8 frames late (already completed)
        { lf_rec *r = &g_lf[(f - 8) % LFN];   // 長幀日誌（缺省開，DM_LONGFRAME=0 關）：整幀 > 20 ms 或 GPU > 16.7 ms、且明顯高於近況，才打一行 | long-frame log (on by default, DM_LONGFRAME=0 turns it off): a line is printed only when the whole frame > 20 ms or GPU > 16.7 ms, and it is clearly above recent conditions
          static int on = -1; if (on < 0) { const char *e = getenv("DM_LONGFRAME"); on = !(e && !strcmp(e, "0")); }
          // 「長」是相對近況說的：穩定跑在 30 幀（菜單 / 無窗口宿主）或持續過載時每幀都 > 20 ms，那不是長幀，不刷屏——要比近 16 幀的滑動均值高出四成才算
          // "Long" is relative to recent conditions: when running steadily at 30 fps (menus / windowless host) or under sustained overload, every frame is > 20 ms; those are not long frames and must not flood the log - a frame has to be 40% above the sliding mean of the last 16 frames to count
          static double ef = 16.7, eg = 8; int lf = r->fr > 20.0 && r->fr > 1.4 * ef, lg = g > 16.7 && g > 1.3 * eg;
          if (r->f == f - 8) { ef += ((r->fr < 3 * ef ? r->fr : 3 * ef) - ef) / 16; eg += ((g < 3 * eg ? g : 3 * eg) - eg) / 16; }
          if (on && r->f == f - 8 && f - 8 > 0 && (lf || lg)) { char b[512];
              snprintf(b, sizeof b, "幀 %llu（%.2f s）整幀 %.1f ms，提交+出圖 %.1f ms，GPU %.1f ms；新建管線 %u 條 + 著色器庫 %u 個共 %.1f ms；上傳貼圖 首傳 %u 張 + 重傳 %u 次 %.1f MB / %.1f ms",
                       (unsigned long long)r->f, r->t_s, r->fr, r->nv, g, r->pso, r->lib, r->make_ms, r->up, r->rup, r->up_mb, r->up_ms);
              // 可見性：長幀「晚出來」的那段時間屏幕上停著的是上一幀；上一幀若是讀取畫面（或本幀自己就是），玩家看到的只是讀取畫面多停了一下，沒有運動被打斷
              // Visibility: during the time a long frame "comes out late", what stays on screen is the previous frame; if the previous frame was a loading screen (or this frame itself is one), all the player sees is the loading screen staying a bit longer, with no motion interrupted
              const lf_rec *pv = &g_lf[(f - 9) % LFN]; int hidden = r->ls || (f >= 9 && pv->f == f - 9 && pv->ls);
              if (hidden) strlcat(b, "；接讀取畫面，不可見", sizeof b);
              dm_longframe_note(b); } }
        uint64_t g0, g1; if (frame_rec() && mtl_gpu_span(f - 8, &g0, &g1)) { trow(f - 8, "@gpu_beg_ms", (g0 - g_t0_ns) / 1e6); trow(f - 8, "@gpu_end_ms", (g1 - g_t0_ns) / 1e6); } }
    prev_in = t0; prev_pass = ps; prev_draw = dr;
    dump_frame();
    if (g_frame % TWIN == 0) tsummary(g_frame);
    mtl_set_frame(g_frame);
    // 真機語義：出圖只入隊、立即返回；本幀顯示時刻按節拍器排，該貼圖到「顯示時刻 + 一個週期」才可再獲取（阻塞在 AcquireTexture）
    // Real-device semantics: present only enqueues and returns immediately; this frame's display time is scheduled by the pacer, and the texture can be acquired again only at "display time + one period" (blocking in AcquireTexture)
    uint64_t period = present_period(o), now = dm_detclock_enabled() ? guest_ns() : now_ns();
    nvn_pacer pc = {o->pace};
    uint64_t slot = now + nvn_pace(&pc, now, period);
    o->pace = pc.target;
    if (idx >= 0 && idx < 4) o->avail[idx] = period ? slot + period : 0;
    static uint64_t prev_slot; if (prev_slot && period) trow(f, "@display_ms", (slot - prev_slot) / 1e6); prev_slot = slot;   // 屏幕上相鄰兩幀的顯示間隔（掉幀看這個） | display interval between two adjacent frames on screen (this is the one to look at for dropped frames)
    // 實際呈現間隔（窗口模式）：滯後 8 次出圖取（呈現回調已到），第 n 次與第 n−1 次之差；−1 = 被丟棄
    // Actual presentation interval (windowed mode): taken 8 presents late (the presented callback has arrived by then), as the difference between the n-th and the (n−1)-th; −1 = discarded
    if (o->presents > 9) { uint64_t n = o->presents - 8; double a_ = mtl_presented_time(n), b_ = mtl_presented_time(n - 1);
        if (a_ < 0) trow(f - 8, "@presented_ms", -1); else if (a_ > 0 && b_ > 0) trow(f - 8, "@presented_ms", (a_ - b_) * 1e3);
        g_pres_iv[g_presn++ % TWIN] = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0);
        { double iv_ = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0); mtl_ub_present(iv_); }   // 同進程交替測量（DM_NVN_UB）按相統計上屏間隔；沒開時只是一次原子讀 | same-process alternating measurement (DM_NVN_UB) tallies on-screen present intervals per phase; when off it is just one atomic read
        dm_perf_present(a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0));   // 上屏間隔另存一圈，tsummary 每 300 幀報掉幀（M1 判據） | on-screen present intervals are also kept in a separate ring; tsummary reports dropped frames every 300 frames (M1 criterion)
        if (a_ > 0) trow(f - 8, "@pres_at_ms", a_ * 1e3 - g_t0_ns / 1e6); }   // 上屏時刻（進程起點 ms；presentedTime 與 mach_absolute_time 同基） | on-screen present time (ms since process start; presentedTime shares a time base with mach_absolute_time)
    if (f >= 8 && slow_on()) slow_emit(f - 8);
    prev_out = now_ns();
}

// ---------- 較新 SDK 遊戲（SMBW）用到、D2R 沒用到的狀態類接口：緩衝對象、池 / 採樣器 / 貼圖查詢、視圖、顏色狀態、設備與窗口 ----------
// 只讀寫對象本身，不錄命令。宿主沒對應能力的（貼圖通道重排、深度 / 模板讀取模式、採樣器歸約、虛擬映射）第一次遇到報一行，照常返回。
// ---------- State-only interfaces used by newer-SDK games (SMBW) that D2R did not use: buffer objects, pool / sampler / texture queries, views, color state, device and window ----------
// They only read and write the object itself and record no commands. Where the host has no matching capability (texture channel swizzle, depth / stencil sampling mode, sampler reduction, virtual mapping) the first occurrence logs one line and the call returns normally.
#define ONCE_LOG(...) do { static int once_; if (!once_++) LOG("[nvn] " __VA_ARGS__); } while (0)

// 緩衝對象 = 池 + 偏移 + 大小；GPU 地址與 CPU 映射都是池的宿主指針 + 偏移（與 MemoryPoolGetBufferAddress 同一口徑）
// Buffer object = pool + offset + size; the GPU address and the CPU mapping are both the pool's host pointer + offset (same convention as MemoryPoolGetBufferAddress)
typedef struct { NVNdevice *dev; const pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; const pool_t *pool; int64_t off; uint64_t size; } buf_t;             FITS(buf_t, NVNbuffer);
#define BB(b) ((buf_builder_t *)(b))
#define BF(b) ((const buf_t *)(b))
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { NVNdevice *d = BB(b)->dev; memset(b, 0, sizeof(buf_builder_t)); BB(b)->dev = d; }
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { BB(b)->dev = d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) { BB(b)->pool = (const pool_t *)p; BB(b)->off = off; BB(b)->size = n; }
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *buf, const NVNbufferBuilder *b) {
    const buf_builder_t *bb = (const buf_builder_t *)b;
    if (!bb->pool || !bb->pool->mem) { LOG("【紅】BufferInitialize：池沒有存儲（池 %p flags %#x 偏移 %#llx 大小 %#llx）", (const void *)bb->pool, bb->pool ? bb->pool->flags : 0, (unsigned long long)bb->off, (unsigned long long)bb->size); return 0; }
    if ((uint64_t)bb->off + bb->size > bb->pool->size) LOG("【紅】BufferInitialize：越過池尾（偏移 %#llx 大小 %#llx 池 %#llx）", (unsigned long long)bb->off, (unsigned long long)bb->size, (unsigned long long)bb->pool->size);
    *(buf_t *)buf = (buf_t){MAGIC('B', 'U', 'F', 0), bb->pool, bb->off, bb->size};
    OLOG("BufferInitialize %p pool=%p off=%#llx size=%#llx", (void *)buf, (const void *)bb->pool, (unsigned long long)bb->off, (unsigned long long)bb->size);
    return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer *buf) { ((buf_t *)buf)->magic = 0; }
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *buf, const char *s) { (void)buf; (void)s; }
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *buf) { return (NVNbufferAddress)(BF(buf)->pool->mem + BF(buf)->off); }
NVN(nvnBufferMap, void *, const NVNbuffer *buf) { return BF(buf)->pool->mem + BF(buf)->off; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *buf) { return (size_t)BF(buf)->size; }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *buf) { return (NVNmemoryPool *)BF(buf)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *buf) { return (ptrdiff_t)BF(buf)->off; }
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) { (void)buf; (void)off; (void)n; }        // 統一內存：無需刷 | unified memory: nothing to flush
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) { (void)buf; (void)off; (void)n; }

// 內存池查詢
// Memory pool queries
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return (NVNmemoryPoolFlags)((const pool_t *)p)->flags; }
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return ((const pool_t *)p)->mem; }
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return (size_t)((const pool_t *)p)->size; }
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *p, const char *s) { (void)p; (void)s; }
// 調試標籤：只給 NVN 調試工具看，宿主不用 | Debug labels: only for NVN debugging tools, unused by the host
NVN(nvnWindowSetDebugLabel, void, NVNwindow *w, const char *s) { (void)w; (void)s; }
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *p, const char *s) { (void)p; (void)s; }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *p, const char *s) { (void)p; (void)s; }
NVN(nvnQueueSetDebugLabel, void, NVNqueue *q, const char *s) { (void)q; (void)s; }
NVN(nvnProgramSetDebugLabel, void, NVNprogram *p, const char *s) { (void)p; (void)s; }
// 虛擬池（flags 0x200）：只有 GPU 地址區間、沒有存儲，靠 MapVirtual 把物理池的頁映射進來。請求 0x28 字節：{物理池, 物理偏移, 虛擬偏移, 大小, 存儲類}。
// 宿主做法：所有請求若能用同一個「基址」描述（同一物理池、物理偏移 − 虛擬偏移相同），虛擬池的存儲就直接指向物理池那段內存——與真機同一份物理頁的語義一致，
//   緩衝地址 / Metal 緩衝查找都落在物理池已登記的那塊上。SMBW 只有一個 128 MB 虛擬池、一條請求整池映射到一個物理池偏移 0。
//   不一致的映射（真稀疏）未實現：報紅、返回失敗。原先一律返回失敗：遊戲把這個池當不可用，之後從它分出的緩衝拿到空池，MemoryPoolGetFlags 讀 NULL+4 崩
// Virtual pool (flags 0x200): a GPU address range with no storage; MapVirtual maps pages of physical pools into it. A request is 0x28 bytes: {physical pool, physical offset, virtual offset, size, storage class}.
// Host approach: if every request can be described by one "base" (same physical pool, same physical offset - virtual offset), the virtual pool's storage points straight at that memory of the physical pool -- the same semantics as one set of physical pages on the real device,
//   and buffer addresses / Metal buffer lookups land on the block the physical pool already registered. SMBW has a single 128 MB virtual pool, mapped whole by one request onto one physical pool at offset 0.
//   Inconsistent mappings (true sparse) are not implemented: reported red, returns failure. It used to always fail: the game treated the pool as unusable, a buffer later carved from it got a null pool, and MemoryPoolGetFlags read NULL+4 and crashed
typedef struct { NVNmemoryPool *phys; int64_t poff, voff; uint64_t size; int32_t storage_class, _pad; } map_req;
_Static_assert(sizeof(map_req) == 0x28, "NVNmappingRequest");
NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *p, int n, const NVNmappingRequest *rq) {
    pool_t *o = (pool_t *)p; const map_req *r = (const map_req *)rq;
    for (int i = 0; i < n; i++) {
        const pool_t *ph = (const pool_t *)r[i].phys;
        if (!ph) continue;   // 解除映射（物理池為空）：宿主不回收，保持原樣 | Unmapping (null physical pool): the host keeps the storage as is
        if (!ph->mem) { LOG("【紅】MapVirtual：物理池 %p 沒有存儲", (const void *)ph); return 0; }
        uint8_t *base = ph->mem + r[i].poff - r[i].voff;
        if (r[i].voff < 0 || (uint64_t)r[i].voff + r[i].size > o->size || (uint64_t)r[i].poff + r[i].size > ph->size) { LOG("【紅】MapVirtual：請求越界（虛擬 %#llx+%#llx / 池 %#llx，物理 %#llx / 池 %#llx）", (unsigned long long)r[i].voff, (unsigned long long)r[i].size, (unsigned long long)o->size, (unsigned long long)r[i].poff, (unsigned long long)ph->size); return 0; }
        if (o->mem && o->mem != base) { LOG("【紅】MapVirtual：映射不連續（真稀疏）未實現：虛擬池 %p 已對 %p，本條要 %p", (void *)o, (void *)o->mem, (void *)base); return 0; }
        if (!o->mem) LOG("[nvn] MapVirtual：虛擬池 %p（%#llx 字節）的存儲指向物理池 %p + %#llx", (void *)o, (unsigned long long)o->size, (const void *)ph, (unsigned long long)(r[i].poff - r[i].voff));
        o->mem = base;
    }
    return 1;
}

// 採樣器：歸約（min / max）Metal 採樣器沒有，只記一次；取值函數照對象裡的參數
// Samplers: reduction (min / max) does not exist on Metal samplers, logged once; the getters read the parameters in the object
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { (void)b; if (r) ONCE_LOG("SamplerBuilderSetReductionFilter %d：宿主按平均採樣（min/max 歸約不支持）", (int)r); }
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *s, const char *l) { (void)s; (void)l; }
#define SP(s) (&((const samp_t *)(s))->p)
NVN(nvnSamplerGetMinMagFilter, void, const NVNsampler *s, NVNminFilter *mi, NVNmagFilter *ma) { *mi = (NVNminFilter)SP(s)->minf; *ma = (NVNmagFilter)SP(s)->magf; }
NVN(nvnSamplerGetWrapMode, void, const NVNsampler *s, NVNwrapMode *a, NVNwrapMode *b, NVNwrapMode *c) { *a = (NVNwrapMode)SP(s)->ws; *b = (NVNwrapMode)SP(s)->wt; *c = (NVNwrapMode)SP(s)->wr; }
NVN(nvnSamplerGetLodClamp, void, const NVNsampler *s, float *lo, float *hi) { *lo = SP(s)->lod_min; *hi = SP(s)->lod_max; }
NVN(nvnSamplerGetLodBias, float, const NVNsampler *s) { return SP(s)->lod_bias; }
NVN(nvnSamplerGetCompare, void, const NVNsampler *s, NVNcompareMode *m, NVNcompareFunc *f) { *m = (NVNcompareMode)SP(s)->cmp_mode; *f = (NVNcompareFunc)SP(s)->cmp_func; }
NVN(nvnSamplerGetBorderColor, void, const NVNsampler *s, float *c) { memcpy(c, SP(s)->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy, float, const NVNsampler *s) { return SP(s)->aniso; }
NVN(nvnSamplerGetReductionFilter, NVNsamplerReduction, const NVNsampler *s) { (void)s; return (NVNsamplerReduction)0; }
// 兩個採樣器狀態是否相同（遊戲拿它去重採樣器池項） | whether two samplers have the same state (games use it to deduplicate sampler pool entries)
NVN(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) {
    const samp_par *x = SP(a), *y = SP(b);
    return x->minf == y->minf && x->magf == y->magf && x->ws == y->ws && x->wt == y->wt && x->wr == y->wr && x->cmp_mode == y->cmp_mode && x->cmp_func == y->cmp_func &&
           x->lod_min == y->lod_min && x->lod_max == y->lod_max && x->lod_bias == y->lod_bias && x->aniso == y->aniso && !memcmp(x->border, y->border, sizeof x->border);
}

// 貼圖 builder / 貼圖 / 視圖
// Texture builder / texture / view
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int d) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = d; }
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    tex_builder_t *o = (tex_builder_t *)b; o->set_swz = 1; o->swz = (uint16_t)((r & 7) | (g & 7) << 3 | (bl & 7) << 6 | (a & 7) << 9);
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureBuilderSetSwizzle 非恆等（%d %d %d %d）：採樣時用 Metal 視圖重排", (int)r, (int)g, (int)bl, (int)a);
}
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) { (void)b; if (m) ONCE_LOG("TextureBuilderSetDepthStencilMode %d：宿主按深度讀", (int)m); }
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) { (void)b; return 0; }
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { (void)b; return 0; }
// 預排布貼圖的數據（貼圖打包工具的產物，已是硬件佈局）：原版在 TextureInitialize 時由驅動拷進貼圖存儲。
// 朋友收集開機就有貼圖走這條（BC 格式 256x256），但資源加載已把同樣的數據放進池裡：只有內容不同時才拷
// Packaged texture data (output of the texture packager, already in hardware layout): on hardware the driver copies it into the texture storage during TextureInitialize.
// Tomodachi Life sends textures this way from boot (BC formats, 256x256), but its resource loader already put the same data in the pool: copy only when the contents differ
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->pkg_data = data; }
NVN(nvnTextureBuilderGetPackagedTextureData, const void *, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->pkg_data; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)CTX(t)->d.format; }
// 朋友收集讀回貼圖的通道重排與行距（agl 的貼圖數據管理）。重排沒設過 = 恆等（R G B A = 2 3 4 5）；行距是建貼圖時給的（線性貼圖才有意義，塊線性為 0）
// Tomodachi Life reads back a texture's swizzle and stride (agl's texture data manager). An unset swizzle = identity (R G B A = 2 3 4 5); the stride is the one given at creation (meaningful only for linear textures, 0 for block-linear)
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    const tex_t *o = CTX(t); uint16_t s = o->set_swz ? o->swz : (uint16_t)(2 | 3 << 3 | 4 << 6 | 5 << 9);
    if (r) *r = (NVNtextureSwizzle)(s & 7); if (g) *g = (NVNtextureSwizzle)(s >> 3 & 7); if (b) *b = (NVNtextureSwizzle)(s >> 6 & 7); if (a) *a = (NVNtextureSwizzle)(s >> 9 & 7);
}
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->d.stride; }
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) { (void)t; return 0; }   // 宿主不分存儲類，同建造器給 0（狂怒世界用，上游 42a50ef） | the host has no storage classes, 0 as for the builder (used by Bowser's Fury, upstream 42a50ef)
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)CTX(t)->d.target; }
NVN(nvnTextureSetDebugLabel, void, NVNtexture *t, const char *s) { (void)t; (void)s; }
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *base, int *n) { const view_t *w = (const view_t *)v; *base = w->base_level; *n = w->nlevels; return w->set_levels; }
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) { const view_t *w = (const view_t *)v; *t = (NVNtextureTarget)w->target; return w->set_target; }
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) { VW(v)->set_dsmode = 1; VW(v)->dsmode = (uint8_t)m; if (m) ONCE_LOG("TextureViewSetDepthStencilMode %d：宿主按深度讀", (int)m); }
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    view_t *w = VW(v); w->set_swizzle = 1; w->swz[0] = (uint8_t)r; w->swz[1] = (uint8_t)g; w->swz[2] = (uint8_t)bl; w->swz[3] = (uint8_t)a;
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureViewSetSwizzle 非恆等（%d %d %d %d）：採樣時用 Metal 視圖重排", (int)r, (int)g, (int)bl, (int)a);
}

// 顏色狀態：alpha 測試函數、邏輯運算（字段已在 color_st 裡）
// Color state: alpha test function, logic op (the fields already exist in color_st)
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) { ST(color_st, s)->alpha_func = (uint32_t)f; }
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *s, NVNlogicOp op) { ST(color_st, s)->logic_op = (uint32_t)op; }

// 設備：組合貼圖句柄 = 貼圖號 | 採樣器號 << 20（與分離句柄同一編碼）
// Device: combined texture handle = texture id | sampler id << 20 (same encoding as the separate handles)
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tex, int samp) { (void)d; return (NVNtextureHandle)((uint64_t)(uint32_t)tex | (uint64_t)(uint32_t)samp << 20); }
// 計數器時戳是 19.2 MHz 的 GPU 計數（nvn_mtl.m gpu_ticks：納秒 × 12 / 625），換回納秒。原先原樣返回，比 GetCurrentTimestampInNanoseconds 小約 52 倍，兩者一減就是亂數
// Counter timestamps are 19.2 MHz GPU ticks (nvn_mtl.m gpu_ticks: ns × 12 / 625); convert back to ns. They used to be returned as is, about 52 times smaller than GetCurrentTimestampInNanoseconds, so subtracting one from the other gave garbage
NVN(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *d, const NVNcounterData *c) { (void)d; return c->timestamp * 625 / 12; }
// 當前 GPU 時刻（納秒）：與計數器時戳同一時鐘——客體時鐘（guest_ns，nvn_mtl.m gpu_ticks） | Current GPU time (ns): the same clock as the counter timestamps -- the guest clock (guest_ns, nvn_mtl.m gpu_ticks)
NVN(nvnDeviceGetCurrentTimestampInNanoseconds, uint64_t, const NVNdevice *d) { (void)d; return guest_ns(); }
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }

// 窗口：活動交換鏈張數（宿主按創建時張數輪轉，只記一次）
// Window: number of active swapchain textures (the host rotates through the count given at creation; logged once)
NVN_NOPROTO(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder *b, int n) { (void)b; ONCE_LOG("WindowBuilderSetNumActiveTextures %d（宿主按 SetTextures 的張數輪轉）", n); }
NVN_NOPROTO(nvnWindowSetNumActiveTextures, void, NVNwindow *w, int n) { (void)w; ONCE_LOG("WindowSetNumActiveTextures %d（宿主按 SetTextures 的張數輪轉）", n); }

// 命令緩衝內存用量：本次錄製累計寫下的記錄字節（跨段累計；控制內存宿主不用，恆 0）；內存回調開關忽略。
// 原先按 cur - begin 算：錄製溢出到宿主補段後 cur 在宿主內存（地址低於客體的 begin），返回 0。SMBW 錄完每個材質設置句柄就問這個值，
// 0（且控制內存也是 0）就當錄了個空的、把句柄標無效（main+0x4bb6c 起清材質項 +0x50 位 0）；之後的繪製不再 CALL 它，沿用上一個物體的頂點緩衝 / 貼圖：
// iPhone 上開場關奇數幀一批繪製越界取頂點，出大三角形和 NaN（黑帶、黑邊光暈、灰條紋）。沒溢出時累計值與 cur - begin 相同
// Command buffer memory usage: record bytes written in this recording (accumulated across segments; the host does not use control memory, always 0); the memory callback switch is ignored.
// It used to be cur - begin: once a recording overflowed into a host segment, cur is in host memory (an address below the guest begin) and it returned 0. SMBW asks for this value after recording each material setup handle,
// and on 0 (with control memory also 0) treats the recording as empty and marks the handle invalid (from main+0x4bb6c it clears bit 0 of the material entry +0x50); later draws no longer CALL it and inherit the previous object's vertex buffers / textures:
// on the iPhone a batch of draws on odd frames of the opening course fetched vertices out of bounds, giving huge triangles and NaN (black band, black-edged glow, grey stripes). Without overflow the accumulated value equals cur - begin
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *m = (const cmd_t *)c;
    return m->begin ? (size_t)m->used : 0; }
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { (void)c; return 0; }
NVN_NOPROTO(nvnCommandBufferSetCommandMemoryCallbackEnabled, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }

// nvnProgramSetShadersExt（NVN 55）：每階段 0x18 字節 = NVNshaderData{data, control} + 一個 8 字節擴展字段（SMBW main 0x1074ff0 起填）。
// 宿主按控制段認階段，擴展字段不用：換成普通 NVNshaderData 交給 SetShaders。
// nvnProgramSetShadersExt (NVN 55): each stage is 0x18 bytes = NVNshaderData{data, control} + one 8-byte extension field (filled from SMBW main 0x1074ff0).
// The host identifies stages by the control section and does not use the extension field: convert to plain NVNshaderData and hand it to SetShaders.
typedef struct { NVNbufferAddress data; const void *control; uint64_t ext; } shader_data_ext;
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const shader_data_ext *sd) {
    if (n < 0 || n > 6) { LOG("【紅】ProgramSetShadersExt 階段數 %d", n); return 0; }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) { plain[i].data = sd[i].data; plain[i].control = sd[i].control; if (sd[i].ext) ONCE_LOG("ProgramSetShadersExt 擴展字段非 0（%#llx）：宿主不用", (unsigned long long)sd[i].ext); }
    return h_nvnProgramSetShaders(p, n, plain);
}

// ---------- 較新 SDK 遊戲（SMBW）用到的命令：多數映射到已有記錄格式，回放層不動 ----------
// 單數 / 組合形式 → 複數 / 分離形式；條件渲染、深度邊界等回放層還沒有的狀態先記一次、照常畫（寧可多畫不漏畫）。
// ---------- Commands used by newer-SDK games (SMBW): most map onto existing record formats, the replay layer is unchanged ----------
// Singular / combined forms → plural / separate forms; state the replay layer does not have yet (conditional render, depth bounds, ...) is logged once and drawing continues (better to over-draw than to drop).
#include "../tex/nvn_deswizzle.h"

NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) { h_nvnCommandBufferCopyCommands(c, n, h); }   // 回放層對 CALL 與 COPY 一視同仁 | the replay layer treats CALL and COPY alike
// 組合貼圖句柄（貼圖號 | 採樣器號 << 20）原樣記進貼圖槽：回放層按低 20 位取貼圖、高位取採樣器（nvn_mtl.m NVN_RB_SAMPT）
// Combined texture handle (texture id | sampler id << 20) is recorded into the texture slot as is: the replay layer takes the texture from the low 20 bits and the sampler from the high bits (nvn_mtl.m NVN_RB_SAMPT)
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNtextureHandle h) { uint64_t v = (uint64_t)h; bind_handles(c, OP_BIND_TEX, st, i, 1, &v); }
NVN_NOPROTO(nvnCommandBufferBindTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h); }
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNimageHandle h) { uint64_t v = (uint64_t)h; bind_handles(c, OP_BIND_IMG, st, i, 1, &v); }
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { NVNbufferRange r = {a, n}; h_nvnCommandBufferBindVertexBuffers(c, i, 1, &r); }

// 繪製
// Draws
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, 0, 0, 1); }
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib, int bv) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, bv, 0, 1); }
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n, int bi, int inst) {
    *REC(c, OP_DRAW, p_draw) = (p_draw){pr, first, n, inst, bi};
}
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind) {
    (void)c; (void)pr; (void)it; (void)ib; (void)ind; ONCE_LOG("【紅】DrawElementsIndirect：宿主未實現（跳過）");
}

// 視口 / 裁剪 / 深度範圍：單數與多個形式
// Viewport / scissor / depth range: single and multiple forms
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) { float v[4] = {(float)x, (float)y, (float)w, (float)h}; h_nvnCommandBufferSetViewports(c, 0, 1, v); }
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int n, const int *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetScissor(c, r[0], r[1], r[2], r[3]);
    if (first + n > 1) ONCE_LOG("SetScissors first=%d n=%d：宿主只用 0 號", first, n);
}
NVN(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int n, const float *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetDepthRange(c, r[0], r[1]);
    if (first + n > 1) ONCE_LOG("SetDepthRanges first=%d n=%d：宿主只用 0 號", first, n);
}

// GPU 寫值：ReportValue = 到這一點時把 32 位值寫到地址（回放層的事件寫，完成時落地）
// GPU value write: ReportValue = write a 32-bit value to the address when this point is reached (the replay layer's event write, applied on completion)
NVN(nvnCommandBufferReportValue, void, NVNcommandBuffer *c, uint32_t v, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    volatile uint32_t *val = (volatile uint32_t *)(uintptr_t)a; uint32_t mm = NVN_EVENT_SIGNAL_MODE_WRITE;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// 緩衝清成 32 位值：一條 OP_CLEAR_BUF 記錄，回放時在提交內按序填（字節一致用 fillBuffer，否則用圖案緩衝拷）
// Clear a buffer to a 32-bit value: one OP_CLEAR_BUF record, filled in order within the submission at replay (fillBuffer when all bytes match, otherwise copied from a pattern buffer)
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n, uint32_t v) { *REC(c, OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){a, n, v, 0}; }

// 回放層還沒有的狀態：只記一次（條件渲染按「恆畫」處理）
// State the replay layer does not have yet: logged once (conditional rendering is treated as "always draw")
NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t en) { *REC(c, OP_RENDER_EN, p_render_en) = (p_render_en){(en & 0xff) != 0, 0, 0}; }
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, NVNconditionalRenderMode m, NVNbufferAddress a) { *REC(c, OP_RENDER_EN, p_render_en) = (p_render_en){2 + ((int)m != 0), 0, a}; }
// 深度夾限：朋友收集畫 Mii 全身像時先清深度為 1、開夾限，再以「深度相等」畫（模型在遠平面外，深度全夾成 1，各部件按繪製先後疊）。
// 原先宿主忽略，Metal 照常裁掉遠平面外的片元，相等測試全不過，Mii 預覽整個不見
// Depth clamp: Tomodachi Life draws a Mii's full-body picture by clearing depth to 1, enabling clamp, then drawing with "depth equal" (the model lies beyond the far plane, all its depth clamps to 1, and parts stack in draw order).
// The host used to ignore it, Metal clipped the fragments beyond the far plane as usual, no equal test passed, and the Mii preview vanished
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t en) { *REC(c, OP_DEPTH_CLAMP, p_depth_clamp) = (p_depth_clamp){B(en) != 0}; }
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) { (void)c; if (w != 1.0f) ONCE_LOG("SetLineWidth %.2f：Metal 只有 1 像素線", w); }
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c, float s) { (void)c; (void)s; ONCE_LOG("SetPointSize：點大小由著色器給"); }
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c, uint32_t en, int idx) { (void)c; if ((en & 0xff) && idx != -1 && idx != 0xffff) ONCE_LOG("【紅】SetPrimitiveRestart 索引 %#x：Metal 只認全 1", idx); }
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c, uint32_t en) { (void)c; if (en & 0xff) ONCE_LOG("【紅】SetRasterizerDiscard 開：宿主暫忽略"); }
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c, int m) { (void)c; if ((m & 0xffff) != 0xffff) ONCE_LOG("SetSampleMask %#x：宿主暫忽略", m); }
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; if (en & 0xff) ONCE_LOG("SetConservativeRasterEnable 開：宿主暫忽略"); }
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }        // Tegra 分塊緩存提示，Metal 無對應 | Tegra tiled cache hint, no Metal equivalent
NVN(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c, int w, int h) { (void)c; (void)w; (void)h; }
NVN_NOPROTO(nvnCommandBufferSetColorReductionEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; (void)en; }   // 顏色壓縮提示 | color compression hints
NVN_NOPROTO(nvnCommandBufferSetColorReductionThresholds, void, NVNcommandBuffer *c, int cls, const int *t) { (void)c; (void)cls; (void)t; }
// 多重採樣貼圖解析到單採樣貼圖（整張）：按貼圖拷貝錄、帶 DM_COPY_RESOLVE，回放用 Metal 的多重採樣解析（nvn_mtl.m copy_t2t）
// Resolve a multisample texture into a single-sample one (whole texture): recorded as a texture copy with DM_COPY_RESOLVE; replay uses Metal's multisample resolve (copy_t2t in nvn_mtl.m)
static void rec_downsample(NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv, uint64_t ra) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2);
    o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, ra); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, ra);
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv);
    if (o->src) o->sr = (NVNcopyRegion){0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = (NVNcopyRegion){0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = DM_COPY_RESOLVE;
}
NVN(nvnCommandBufferDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { rec_downsample(c, s, NULL, d, NULL, (uint64_t)__builtin_return_address(0)); }
NVN(nvnCommandBufferDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    rec_downsample(c, s, sv, d, dv, (uint64_t)__builtin_return_address(0));
}

// CPU 寫貼圖（NVN WriteTexels）：貼圖存儲在池內存裡、按 Tegra 塊線性排布；整級解出 → 改區域 → 排回。寫前解寫保護並標髒，回放層下次用到時重傳
// CPU texture write (NVN WriteTexels): texture storage lives in pool memory in Tegra block-linear layout; decode the whole level → patch the region → re-encode.
// Write protection is lifted and the range marked dirty first, so the replay layer re-uploads it the next time it is used
// 非 3D 貼圖：按塊線性地址只寫區域內的字節，不解整級。銀河戰士 生存恐懼的小地圖（main+0x160e80）每個紋素調一次 WriteTexels（1×1 區域），
// 原先每次都解出、改寫、排回整級，主線程幾分鐘後整幀耗在這裡（175 s 起幀數停住）。區域越界或格式不支持返回 −1，退回整級做法（上游 a9c56fd）
// Non-3D textures: only the bytes inside the region are written, by block-linear address, without decoding the whole level. Metroid Dread's minimap (main+0x160e80) calls WriteTexels once per texel (a 1×1 region);
// previously every call decoded, patched and re-encoded the whole level, and after a few minutes the main thread spent whole frames there (frames stopped from 175 s). Returns −1 when the region is out of range or the format unsupported, falling back to the whole-level path (upstream a9c56fd)
static int write_region_bl(const tex_t *t, unsigned level, unsigned layer, const NVNcopyRegion *r, const uint8_t *src, size_t row, int bw, int bh, int bpb) {
    uint32_t row_bytes, rows, slices, gbpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &gbpb) || (int)gbpb != bpb || r->xoffset < 0 || r->yoffset < 0) return -1;
    uint32_t x0 = (uint32_t)(r->xoffset / bw) * (uint32_t)bpb, y0 = (uint32_t)(r->yoffset / bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh);
    if (x0 + wb > row_bytes || y0 + hb > rows) return -1;
    for (size_t y = 0; y < hb; y++)
        for (size_t x = 0; x < wb;) {   // GOB 內 16 字節一段連續 | 16-byte runs are contiguous within a GOB
            uint32_t bx = x0 + (uint32_t)x; size_t n = 16 - bx % 16; if (n > wb - x) n = wb - x;
            memcpy(t->mem + base + nvn_bl_offset(bx, y0 + (uint32_t)y, 0, row_bytes, rows, th, td), src + y * row + x, n); x += n;
        }
    return 0;
}
static void write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    if (!t->mem) { LOG("【紅】WriteTexels：貼圖沒有存儲"); return; }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { ONCE_LOG("【紅】WriteTexels：未知格式 %#x", t->d.format); return; }
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = r->depth ? r->depth : 1;
    if (!is3d) {
        size_t wb0 = (size_t)((r->width + bw - 1) / bw) * bpb, row0 = row ? row : wb0, img0 = img ? img : row0 * (size_t)((r->height + bh - 1) / bh);
        dm_guest_write_prepare(t->mem, t->L.size);
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) ok = !write_region_bl(t, level, layer0 + (unsigned)(r->zoffset + z), r, src + (size_t)z * img0, row0, bw, bh, bpb);
        if (ok) { mtl_tex_cpu_written(t); return; }
    }
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    uint8_t *lin = malloc(lp * lr * lz); if (!lin) die("WriteTexels 分配失敗");
    dm_guest_write_prepare(t->mem, t->L.size);
    // 區域正好是整級（二維）時每個紋素都被覆蓋，不必先解出舊內容（影片每幀整張寫，1080p 省一次解排布） | when the region is exactly the whole level (2D) every texel is overwritten, so the old content need not be decoded first (movies write whole frames; saves one deswizzle per 1080p frame)
    int whole = !is3d && !r->xoffset && !r->yoffset && (uint32_t)r->width == lw && (uint32_t)r->height == lh;
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + (unsigned)(r->zoffset + z);
        if (((is3d && z == 0) || !is3d) && !whole) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { ONCE_LOG("【紅】WriteTexels 解排布失敗（格式 %#x 目標 %d）", t->d.format, t->d.target); free(lin); return; }
        uint8_t *dst = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0) + (size_t)(r->yoffset / bh) * lp + (size_t)(r->xoffset / bw) * bpb;
        for (size_t y = 0; y < hb; y++) memcpy(dst + y * lp, src + (size_t)z * img + y * row, wb);
        if (!is3d || z == nz - 1) nvn_swizzle(&t->d, level, layer, lin, lp, t->mem);
    }
    free(lin);
    mtl_tex_cpu_written(t);
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) { write_texels(CTX(t), (const view_t *)v, r, src, 0, 0); }
// 影片解碼器的原生貼圖輸出（DecoderMode 2，hle_movie.c）：整幀寫進客戶端貼圖第 0 級，同 WriteTexels；返回貼圖格式，對象不是貼圖返回 −1
// The movie decoder's native-texture output (DecoderMode 2, hle_movie.c): a whole frame into level 0 of the client texture, as WriteTexels does; returns the texture format, or −1 when the object is not a texture
int nvn_host_write_texture(const void *nvntex, const void *src, int w, int h, size_t row, int *tw, int *th) {
    const tex_t *t = nvntex;
    uint32_t mg = 0; vm_size_t got = 0;   // 客戶端給的值未必是指針：先試讀 | the value the client gives is not necessarily a pointer: test-read first
    if (!t || (uintptr_t)t & 7 || vm_read_overwrite(mach_task_self(), (vm_address_t)t, 4, (vm_address_t)&mg, &got) != KERN_SUCCESS || got != 4) return -1;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) return -1;
    if (tw) *tw = (int)t->d.width;
    if (th) *th = (int)t->d.height;
    if (src) { NVNcopyRegion r = {0, 0, 0, w, h, 1}; write_texels(t, NULL, &r, src, row, 0); }
    return (int)t->d.format;
}
// 宿主在 CPU 上做完的活：同步對象直接算已發信號（柵欄 0 恆已完成） | work the host finished on the CPU: the sync object counts as signalled at once (fence 0 is always complete)
int nvn_host_signal_sync(void *nvnsync) {
    sync_t *o = nvnsync;
    if (!o || o->magic != MAGIC('S', 'Y', 'N', 'C')) return -1;
    o->fence = 0; o->fenced = 1; return 0;
}
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }   // 統一內存 + 髒頁跟蹤：無需刷 | unified memory + dirty page tracking: nothing to flush
