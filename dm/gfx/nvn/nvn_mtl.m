// NVN 空設備的 Metal 執行層，見 nvn_mtl.h。
// Metal execution layer of the NVN null device, see nvn_mtl.h.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>   // os_proc_available_memory：內存讀數行用 | os_proc_available_memory: for the memory readout line
#endif
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include "../tex/nvn_fmt_mtl.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_deswizzle_gpu.h"
#include "../tex/nvn_bcdec.h"
#include "nvn_builtin.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "nvn_cover.h"
#include "dmsh_chain.h"   // DMSH_CHAIN：本源碼樹期望的 B 著色器庫翻譯鏈版本（在遊戲檔案目錄，Makefile 給 -I） | DMSH_CHAIN: the translation chain version of the B shader library expected by this source tree (in the game profile folder, -I from the Makefile)
#include "game_profile.h"   // 遊戲檔案常數（NVN 驅動常量段基址、檔案名） | game profile constants (NVN driver constant segment bases, profile id)
_Static_assert(DM_GAME_NVN_TEX_SLOTS <= 128 && DM_GAME_NVN_IMG_SLOTS <= 32 && DM_GAME_NVN_TEX_BASE + 8 * DM_GAME_NVN_TEX_SLOTS <= 0xA00 && DM_GAME_NVN_IMG_BASE + 8 * DM_GAME_NVN_IMG_SLOTS <= 0xA00 && DM_GAME_NVN_SAMP_BASE + 8 * 128 <= 0xA00, "遊戲檔案的 NVN 句柄表超出驅動段緩衝（0xA00）或綁定表 | the profile's NVN handle tables exceed the driver segment buffer (0xA00) or the binding tables");
#include "nvn_hmap.h"
#include "../../host/dm.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <execinfo.h>
#include <os/lock.h>
// 本文件的函數參數一律不在被調方重復持有（調用方 / 全局表已持有，生命期由 defer_obj_release 保證）：熱路徑每次調用省一對 retain / release
// Function parameters in this file are never retained again in the callee (the caller / global tables already hold them, and lifetime is guaranteed by defer_obj_release): saves a retain / release pair per call on the hot path
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)

#define MLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define MDIE(...) do { MLOG("FATAL " __VA_ARGS__); abort(); } while (0)

static void warm_start(void);
static void warm_claim(void);

static id<MTLDevice> g_dev;
static NSMutableArray *g_l3_gpu_leaks;
static int g_nvn_deswz_gpu;
static id<MTLCommandQueue> g_q;
static id<MTL4CommandQueue> g_q4;
static dispatch_queue_t g_feedback_q;
static _Atomic uint32_t g_present_qos;
#define ALLOC_MAX 8
static id<MTL4CommandAllocator> g_alloc4[ALLOC_MAX]; static int g_alloc_n, g_alloc_cur;   // 主提交的命令分配器池：開工時挑一個 GPU 已用完的，沒有就新建（到上限才等） | Command allocator pool of the main submission: at start pick one the GPU has finished with, create a new one if there is none (wait only at the cap)
static id<MTL4CommandAllocator> g_up_alloc4[3];
static id<MTL4CommandAllocator> g_present_alloc4[3];
static id<MTL4CommandBuffer> g_present_cb[3];
static id<MTL4ArgumentTable> g_vt, g_ft, g_ct, g_up_ct;
static id<MTL4ArgumentTable> g_present_args;
static id<MTLBuffer> g_present_uv[3];
int g_bloom_q = -1;   // 泛光 1/4 起點（非 static：真機同進程交替直接改 0 / 1） | Bloom 1/4 starting point (non-static: real-device same-process alternation sets it to 0 / 1 directly)
static id<MTL4ArgumentTable> g_bloomq_args;
static id<MTLBuffer> g_bloomq_const[3];
static id<MTLSamplerState> g_bloomq_samp;
static id<MTLRenderPipelineState> g_bloomq_pso;
static MTLPixelFormat g_bloomq_fmt;
static id<MTLTexture> g_bloomq_view;
static int g_bloomq_i;
static struct { int state; uint64_t s_ser, b0_ser, b1_ser, s_h, samp_h, b1_h; const tex_t *s_tex; float threshold, coeff_g, coeff_b; } g_bloomq;
static uint64_t g_bloomq_last_b0, g_bloomq_last_b1h, g_bloomq_last_b1s;   // 最近一次泛光 1/4 疊加改讀：B0 序號 → B1 句柄（跨幀保留，見 2e2d6700a2569795 那支） | Most recent bloom 1/4 overlay read redirection: B0 serial → B1 handle (kept across frames, see the 2e2d6700a2569795 branch)
static uint64_t g_present_alloc_fence[3];
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_link_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_link_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_link_tick_ns, g_link_tick_n;
static uint64_t g_link_dropped;
static int g_pq_inflight;   // 持 g_pq_mu：已提交給 GPU、完成回調還沒來的上屏數 | Under g_pq_mu: number of presents committed to the GPU whose completion callback has not arrived yet
int g_dm_present_link;   // 出圖節拍模式（DM_PRESENT_LINK 的運行時值，同進程交替測量時直接改它）：0 = 自己取 drawable；1 = 顯示節拍 60 Hz；2 = 120 Hz 節拍、遊戲隔拍出一幀 | Present pacing mode (runtime value of DM_PRESENT_LINK, modified directly during same-process alternating measurement): 0 = acquire the drawable itself; 1 = display link at 60 Hz; 2 = display link at 120 Hz, the game presents a frame every other display-link tick
static int g_present_latency = 1;
static uint64_t g_pq_done;
static uint64_t g_pq_read;
static id<MTLResidencySet> g_res;
static int g_res_dirty;
static id<MTLSharedEvent> g_ev;
static uint64_t g_fence;                        // 已提交的最大柵欄值（提交線程獨佔遞增） | Highest committed fence value (incremented exclusively by the submit thread)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_done;                         // 已完成的最大柵欄值（完成回調寫） | Highest completed fence value (written by the completion callback)
static uint64_t g_arena_made;
static int g_allocprof = -1; static uint64_t g_ap_bytes[64], g_ap_fn[8];
#define AP_B uint64_t _ap0 = g_allocprof > 0 ? g_dev.currentAllocatedSize : 0
#define AP_E(i) do { if (g_allocprof > 0) { uint64_t _n = g_dev.currentAllocatedSize; if (_n > _ap0) g_ap_fn[i] += _n - _ap0; } } while (0)   // DM_NVN_ALLOCPROF：按操作種類累計的顯卡分配增長 | DM_NVN_ALLOCPROF: GPU allocation growth accumulated per operation type                   // 累計新建的每幀競技場（4 MiB 一塊；內存報告用） | Per-frame arenas created so far (4 MiB each; for the memory report)
static _Atomic int g_last_commit_error;
static _Atomic int g_up_pending;

static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;   // 駐留集專用鎖：res_add 會在持 g_mu 的路徑裡調用（arena_new），不能共用 g_mu | Dedicated lock for the residency set: res_add is called on paths that hold g_mu (arena_new), so it cannot share g_mu
static NSHashTable *g_res_objects;
static void res_add(id<MTLResource> r) {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    [g_res addAllocation:(id<MTLAllocation>)r];
    if (!g_res_objects) g_res_objects = [NSHashTable weakObjectsHashTable];
    [g_res_objects addObject:r];
    g_res_dirty = 1;
    pthread_mutex_unlock(&g_res_mu);
}
static void res_remove(id<MTLResource> r) {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    [g_res removeAllocation:(id<MTLAllocation>)r];
    [g_res_objects removeObject:r];
    g_res_dirty = 1;
    pthread_mutex_unlock(&g_res_mu);
}
// DM_RES_CHECK=1（診斷）：著色器經參數塊訪問的貼圖 / 緩衝若不在駐留集裡，打出前 40 個（貼圖按父貼圖 / 底層緩衝歸到實際分配）
// DM_RES_CHECK=1 (diagnostic): if textures / buffers the shader accesses through the argument block are not in the residency set, print the first 40 (textures are attributed to the actual allocation via parent texture / underlying buffer)
static int g_res_check_on;
static void res_check(id r, const char *what, int reg) {
    static int nbad;
    if (!g_res_check_on || !r || nbad >= 40) return;
    int is_tex = [r conformsToProtocol:@protocol(MTLTexture)], is_buf = [r conformsToProtocol:@protocol(MTLBuffer)];
    if (!is_tex && !is_buf) return;   // 採樣器等不是分配 | Samplers etc. are not allocations
    id<MTLResource> a = r; int hops = 0;
    if (is_tex) { id<MTLTexture> t = (id<MTLTexture>)a; while (t.parentTexture) { t = t.parentTexture; hops++; } a = t.buffer ? (id<MTLResource>)t.buffer : (id<MTLResource>)t; }
    pthread_mutex_lock(&g_res_mu); int ok = [g_res_objects containsObject:a]; pthread_mutex_unlock(&g_res_mu);
    if (!ok) { nbad++; id<MTLTexture> t = is_tex ? (id<MTLTexture>)r : nil;
        MLOG("【紅】駐留缺失 #%d：%s 槽 %d %s %p（父鏈 %d 層）分配 %lu B 存儲 %lu%s", nbad, what, reg, is_tex ? "貼圖" : "緩衝", (__bridge void *)a, hops, (unsigned long)a.allocatedSize, (unsigned long)a.storageMode,
             t ? [NSString stringWithFormat:@" 格式 %lu %lux%lu 類型 %lu 用途 %#lx", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.textureType, (unsigned long)t.usage].UTF8String : ""); }
}
static void res_commit(void) {
    pthread_mutex_lock(&g_res_mu);
    if (g_res_dirty) { [g_res commit]; g_res_dirty = 0; }
    pthread_mutex_unlock(&g_res_mu);
}

@interface NvDeferredRelease : NSObject { @public id object; uint64_t fence; int keep_resident; } @end
@implementation NvDeferredRelease @end
static NSMutableArray<NvDeferredRelease *> *g_deferred;
static pthread_mutex_t g_deferred_mu = PTHREAD_MUTEX_INITIALIZER;
// 延遲釋放有兩種意思，必須分開（進崔凡克首幀的 GPU 內存故障的根）：
//   defer_obj_release = 這個對象退役了：等用過它的提交完成後放掉引用，並把它移出駐留集；
//   defer_obj_unref   = 只是我手裡這一份引用不要了，對象本身可能還在用（貼圖號的視圖緩存裡存的可能就是基貼圖本身）：只放引用，不動駐留集。
// 原先只有前一種：視圖緩存作廢時把「仍是某個序號當前存儲」的貼圖也移出了駐留集。以前作廢只發生在基貼圖真被換掉時，碰不到；
// 透明鏈原地合成對調兩張貼圖的存儲後，兩張活著的貼圖各被這樣移出一次，之後每幀照常當附件、被採樣——GPU 訪問非駐留內存。
// Deferred release has two meanings that must be kept apart (the root of the GPU memory fault on the first frame on entering Travincal):
//   defer_obj_release = this object is retired: drop the reference after the submissions that used it complete, and remove it from the residency set;
//   defer_obj_unref   = only the reference I hold is no longer wanted, the object itself may still be in use (the view cache of a texture ID may hold the base texture itself): only drop the reference, leave the residency set alone.
// Previously only the former existed: when the view cache was invalidated, a texture that 'is still the current storage of some serial' was also removed from the residency set. Invalidation used to happen only when the base texture was really replaced, so it was never hit;
// after the transparency chain's in-place composite swapped the storage of two textures, each of the two live textures was removed once this way, and afterwards they are used as attachments and sampled every frame as usual - the GPU accesses non-resident memory.
static void defer_obj_release_ex(id obj, int keep_resident) {
    if (!obj) return;
    NvDeferredRelease *d = [NvDeferredRelease new]; d->object = obj; d->fence = g_fence + 1; d->keep_resident = keep_resident;
    pthread_mutex_lock(&g_deferred_mu);
    if (!g_deferred) g_deferred = [NSMutableArray array];
    [g_deferred addObject:d];
    pthread_mutex_unlock(&g_deferred_mu);
}
static void defer_obj_release(id obj) { defer_obj_release_ex(obj, 0); }
static void defer_obj_unref(id obj) { defer_obj_release_ex(obj, 1); }
static void defer_drain(void) {
    NSMutableArray<NvDeferredRelease *> *ready = [NSMutableArray array];
    pthread_mutex_lock(&g_deferred_mu);
    for (NSInteger i = g_deferred.count - 1; i >= 0; i--) {
        NvDeferredRelease *d = g_deferred[(NSUInteger)i];
        if (d->fence <= g_done) { [ready addObject:d]; [g_deferred removeObjectAtIndex:(NSUInteger)i]; }
    }
    pthread_mutex_unlock(&g_deferred_mu);
    for (NvDeferredRelease *d in ready) {
        id obj = d->object;
        if (!d->keep_resident && [obj conformsToProtocol:@protocol(MTLResource)]) {
            pthread_mutex_lock(&g_res_mu);
            int registered = [g_res_objects containsObject:obj];
            pthread_mutex_unlock(&g_res_mu);
            if (registered) res_remove((id<MTLResource>)obj);
        }
        d->object = nil;
    }
}

static pthread_mutex_t g_bf_want_mu = PTHREAD_MUTEX_INITIALIZER;
static char **g_bf_want_keys; static size_t g_bf_want_n, g_bf_want_cap; static int g_bf_want_loaded;
void bf_want_note(const char *key) {
    const char *path = getenv("DM_NVN_BF_WANT");
    if (!path || !key || strlen(key) != 16) return;
    for (int i = 0; i < 16; i++) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f') || (key[i] >= 'A' && key[i] <= 'F'))) return;
    pthread_mutex_lock(&g_bf_want_mu);
    if (!g_bf_want_loaded) {
        FILE *f = fopen(path, "r"); char line[64];
        if (f) { while (fgets(line, sizeof line, f)) { char *e = strpbrk(line, "\r\n"); if (e) *e = 0; if (strlen(line) != 16) continue;
                if (g_bf_want_n == g_bf_want_cap) { g_bf_want_cap = g_bf_want_cap ? g_bf_want_cap * 2 : 64; g_bf_want_keys = realloc(g_bf_want_keys, g_bf_want_cap * sizeof *g_bf_want_keys); }
                g_bf_want_keys[g_bf_want_n++] = strdup(line); } fclose(f); }
        g_bf_want_loaded = 1;
    }
    for (size_t i = 0; i < g_bf_want_n; i++) if (!strcmp(g_bf_want_keys[i], key)) { pthread_mutex_unlock(&g_bf_want_mu); return; }
    if (g_bf_want_n == g_bf_want_cap) { g_bf_want_cap = g_bf_want_cap ? g_bf_want_cap * 2 : 64; g_bf_want_keys = realloc(g_bf_want_keys, g_bf_want_cap * sizeof *g_bf_want_keys); }
    g_bf_want_keys[g_bf_want_n++] = strdup(key);
    FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", key); fclose(f); }
    pthread_mutex_unlock(&g_bf_want_mu);
}

// ---------- 內存池 ----------
// ---------- Memory pools ----------
typedef struct { uintptr_t lo, hi; uintptr_t base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; const void *key; int res; } pool_ent;   // res：已放進駐留集（按需，見 pool_res） | res: already in the residency set (on demand, see pool_res)
static pool_ent *g_pools; static int g_npool, g_cap;
static NSMutableArray *g_keep;                  // 持有緩衝引用（pool_ent 裡是不持有的指針） | Holds the buffer references (pool_ent has non-owning pointers)
static uint64_t g_pool_adds;
static int g_pool_res_n;   // 在冊池裡已放進駐留集的個數（內存讀數行用；持 g_mu） | registered pools already in the residency set (for the memory readout; under g_mu)

#define C1_BANK_SIZE (4u << 20)
#define C1_DECL_SIZE (64u << 10)
static NSMutableArray *g_c1_banks;
static size_t g_c1_bank_off, g_c1_alloc_bytes, g_c1_data_bytes;
static uint64_t g_c1_count, g_c1_exclusive;
static NSMutableSet<NSString *> *g_c1_full;
static int g_c1_full_valid;
static os_unfair_lock g_c1_lock = OS_UNFAIR_LOCK_INIT;

// DM_UDC_SELFTEST=1（診斷，A19 真機核 DontCare 行為）：私有存儲 Depth32Float_Stencil8 / RGBA8（1952x900）先清除寫入已知值，
// 再 Load + DontCare、不畫，讀回逐像素比對，打一行結果
// DM_UDC_SELFTEST=1 (diagnostic, checks DontCare behavior on a real A19 device): private-storage Depth32Float_Stencil8 / RGBA8 (1952x900) are first cleared to write known values,
// then Load + DontCare without drawing, read back and compared pixel by pixel, printing one result line
static void udc_selftest(void) {
    if (!getenv("DM_UDC_SELFTEST")) return;
    id<MTLCommandQueue> q = [g_dev newCommandQueue];
    for (int depth = 1; depth >= 0; depth--) {
        NSUInteger w = 1952, h = 900;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (int pass = 0; pass < 2; pass++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            MTLLoadAction la = pass ? MTLLoadActionLoad : MTLLoadActionClear; MTLStoreAction sa = pass ? MTLStoreActionDontCare : MTLStoreActionStore;
            if (depth) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = la; rp.depthAttachment.storeAction = sa; rp.depthAttachment.clearDepth = 0.3125;
                         rp.stencilAttachment.texture = t; rp.stencilAttachment.loadAction = la; rp.stencilAttachment.storeAction = sa; rp.stencilAttachment.clearStencil = 0x5a; }
            else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = la; rp.colorAttachments[0].storeAction = sa; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 0.8); }
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        id<MTLBuffer> out = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0
            destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4 options:depth ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *px = out.contents; NSUInteger bad = 0; for (NSUInteger i = 0; i < w * h; i++) if (px[i] != px[0]) bad++;
        MLOG("UDC自檢 %s %s %lux%lu 首值 %08x 與首值不同 %lu / %lu", g_dev.name.UTF8String, depth ? "Depth32F_S8" : "RGBA8", (unsigned long)w, (unsigned long)h, px[0], (unsigned long)bad, (unsigned long)(w * h));
    }
}
void mtl_init(void) {
    if (g_dev) return;
    const char *link = getenv("DM_PRESENT_LINK");
    g_dm_present_link = link ? atoi(link) : 1; if (g_dm_present_link < 0 || g_dm_present_link > 2) g_dm_present_link = 1;   // 缺省 1（真機同進程交替：負載超一拍時長幀 15% → 0、幀間隔變勻；穩 60 時與 0 無差別）；DM_PRESENT_LINK=0 關   // 1 = 60 Hz 節拍每拍出圖；2 = 120 Hz 節拍、遊戲隔拍出一幀（落後時下一個 8.33 ms 拍就能上屏） | Default 1 (real-device same-process alternation: when load exceeds one display-link period, long frames 15% → 0 and frame intervals even out; no difference from 0 at a steady 60); DM_PRESENT_LINK=0 turns it off   // 1 = present on every tick of a 60 Hz display link; 2 = 120 Hz display link, the game presents a frame every other display-link tick (when behind, it can present on the next 8.33 ms display-link tick)
    const char *latency = getenv("DM_PRESENT_LATENCY");
    if (latency && atoi(latency) > 0) g_present_latency = atoi(latency);
    g_res_check_on = getenv("DM_RES_CHECK") != NULL;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) MDIE("沒有 Metal 設備");
    if (!g_dev.hasUnifiedMemory) MDIE("需要統一內存設備（零拷貝內存池）");
    g_q = [g_dev newCommandQueue];
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
    rd.initialCapacity = 4096;
    NSError *re = nil;
    g_res = [g_dev newResidencySetWithDescriptor:rd error:&re];
    if (!g_res) MDIE("建立 MTLResidencySet 失敗");
    [g_q addResidencySet:g_res];
    { void crash_set_wait_dump(void (*fn)(void)); void mtl_wait_state_dump(void); crash_set_wait_dump(mtl_wait_state_dump); }
    dispatch_queue_attr_t feedback_attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    g_feedback_q = dispatch_queue_create("dm.nvn.mtl4-feedback", feedback_attr);
    MTL4CommandQueueDescriptor *q4d = [MTL4CommandQueueDescriptor new]; q4d.feedbackQueue = g_feedback_q;
    g_q4 = [g_dev newMTL4CommandQueueWithDescriptor:q4d error:&re];
    if (!g_q4) MDIE("建立 MTL4CommandQueue 失敗");
    [g_q4 addResidencySet:g_res];
    g_alloc_n = 3;
    for (int i = 0; i < 3; i++) {
        MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        ad.label = [NSString stringWithFormat:@"nvn-mtl allocator %d", i];
        NSError *ae = nil;
        g_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_alloc4[i]) MDIE("建立 MTL4CommandAllocator[%d] 失敗：%s", i, ae.localizedDescription.UTF8String);
        g_up_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_up_alloc4[i]) MDIE("建立上傳 MTL4CommandAllocator[%d] 失敗：%s", i, ae.localizedDescription.UTF8String);
        MTL4CommandAllocatorDescriptor *pad = [MTL4CommandAllocatorDescriptor new];
        pad.label = [NSString stringWithFormat:@"nvn-mtl present allocator %d", i];
        g_present_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:pad error:&ae];
        if (!g_present_alloc4[i]) MDIE("建立出圖 MTL4CommandAllocator[%d] 失敗：%s", i, ae.localizedDescription.UTF8String);
        g_present_cb[i] = [g_dev newCommandBuffer];
        if (!g_present_cb[i]) MDIE("建立出圖 MTL4CommandBuffer[%d] 失敗", i);
    }
    MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new];
    atd.maxBufferBindCount = 31; atd.maxTextureBindCount = 1;
    NSError *ate = nil;
    g_vt = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_vt) MDIE("建立 vertex argument table 失敗：%s", ate.localizedDescription.UTF8String);
    g_ft = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_ft) MDIE("建立 fragment argument table 失敗：%s", ate.localizedDescription.UTF8String);
    g_ct = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_ct) MDIE("建立 compute argument table 失敗：%s", ate.localizedDescription.UTF8String);
    g_up_ct = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_up_ct) MDIE("建立上傳 compute argument table 失敗：%s", ate.localizedDescription.UTF8String);
    MTL4ArgumentTableDescriptor *pad = [MTL4ArgumentTableDescriptor new];
    pad.maxTextureBindCount = 1; pad.maxSamplerStateBindCount = 1; pad.maxBufferBindCount = 1;
    g_present_args = [g_dev newArgumentTableWithDescriptor:pad error:&ate];
    if (!g_present_args) MDIE("建立出圖 argument table 失敗：%s", ate.localizedDescription.UTF8String);
    g_bloomq_args = [g_dev newArgumentTableWithDescriptor:pad error:&ate];
    if (!g_bloomq_args) MDIE("建立泛光 1/4 argument table 失敗：%s", ate.localizedDescription.UTF8String);
    for (int i = 0; i < 3; i++) {
        g_present_uv[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_present_uv[i]) MDIE("建立出圖 UV 緩衝[%d] 失敗", i);
        res_add(g_present_uv[i]);
        g_bloomq_const[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_bloomq_const[i]) MDIE("建立泛光 1/4 常量緩衝[%d] 失敗", i);
        res_add(g_bloomq_const[i]);
    }
    g_ev = [g_dev newSharedEvent];
    g_keep = [NSMutableArray array];
    g_nvn_deswz_gpu = nvn_deswz_gpu_init(g_dev) == 0;
    dm_res_report_register(mtl_res_counts);
    if (!g_nvn_deswz_gpu) MLOG("【紅】顯卡解排布初始化失敗，預排布貼圖回退 CPU");
    MLOG("Metal 設備 %s", g_dev.name.UTF8String);
    udc_selftest();
}

void dm_mtl_l3_tick(uint32_t tick) {
    static int leak = -1;
    if (leak < 0) leak = getenv("DM_TEST_LEAK_GPU") != NULL;
    if (leak && tick % 100 == 0) {
        if (!g_l3_gpu_leaks) g_l3_gpu_leaks = [NSMutableArray array];
        id<MTLBuffer> leak = [g_dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
        if (leak) [g_l3_gpu_leaks addObject:leak];
    }
}

uint64_t dm_mtl_l3_sample(uint32_t tick) {
    (void)tick;
    return g_dev ? g_dev.currentAllocatedSize : 0;
}

// 已 Finalize 的池的零拷貝緩衝（弱引用，持 g_mu）：內存報告數還活著的個數與長度——池放了緩衝卻沒死，就是還有別的對象（池上直建的貼圖等）強引用著它
// No-copy buffers of finalized pools (weak references, under g_mu): the memory report counts how many are still alive and their length -- a buffer that outlives its pool is still strongly referenced by something else (a texture built directly on the pool, etc.)
static NSHashTable *g_dead_pools;
static _Atomic uint64_t g_reg_gen = 1;   // 內存池 / 貼圖 / 採樣器登記的變更代號：參數緩衝復用以它為失效條件之一 | Change generation of memory pool / texture / sampler registration: one of the invalidation conditions for argument buffer reuse
// 內存池按需駐留（取自 d2r-mobile e2a0c9a9b）：零拷貝緩衝放進駐留集，iOS 會把整個池讀進內存並釘成 wired，遊戲從沒用過的池也一樣
// （他們在 17 Pro Max 上量到一個 512 MB 的渲染目標池一局取址 0 次、整機 wired 多出約 530 MB；4 GB 的 A14 機因此啓動幾秒就被系統結束）。
// 所以池建好時不放進駐留集，GPU 第一次經 mtl_addr / mtl_gpu_addr 取到它的地址時才放進去並當場提交駐留集——GPU 拿池地址只有這兩個口子，
// 當場提交早於用到它的任何命令緩衝提交。CPU 側經 mtl_addr 讀池內容也會觸發（保守，不漏）。調用方持 g_mu
// On-demand residency of memory pools (from d2r-mobile e2a0c9a9b): putting a zero-copy buffer into the residency set makes iOS read the whole pool into memory and pin it as wired, even a pool the game never uses
// (they measured on a 17 Pro Max a 512 MB render-target pool addressed 0 times in a run, with about 530 MB more device wired memory; a 4 GB A14 was ended by the system within seconds of launch because of it).
// So a pool is not put into the residency set when created; it is put in, and the residency set committed on the spot, the first time the GPU gets its address through mtl_addr / mtl_gpu_addr -- these are the only two ways the GPU gets pool addresses,
// and committing on the spot is earlier than any command buffer commit that uses it. CPU-side reads of pool contents through mtl_addr trigger it too (conservative, never misses). Caller holds g_mu
static void pool_res(int i) {
    if (g_pools[i].res) return;
    res_add(g_pools[i].buf); res_commit(); g_pools[i].res = 1; g_pool_res_n++;
}
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init(); g_reg_gen++;
    uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)mem, hi = lo + size;
    uintptr_t a = lo & ~(pg - 1), b = (hi + pg - 1) & ~(pg - 1);
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    if (!buf) MDIE("內存池 %p+%#llx 建零拷貝緩衝失敗", mem, (unsigned long long)size);
    pthread_mutex_lock(&g_mu);
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, g_cap * sizeof *g_pools); }
    g_pools[g_npool++] = (pool_ent){lo, hi, a, buf, buf.gpuAddress, mem, 0};   // 先不進駐留集：第一次被取址時才放（pool_res） | not in the residency set yet: put in the first time it is addressed (pool_res)
    g_pool_adds++;
    [g_keep addObject:buf];
    pthread_mutex_unlock(&g_mu);
}

void mtl_pool_del(const void *mem) {
    g_reg_gen++;
    pthread_mutex_lock(&g_mu);
    // 零拷貝緩衝走延遲釋放（提交過的命令跑完後出駐留集、放引用）。原先只從 g_keep 拿掉，駐留集還強引用著它：池釋放後緩衝一直活著、包著那段客體內存，
    // 客體再在原地址建池又包一份。Engage 的標題演示循環換場景，顯卡已分配 850 s 裡漲到 5.4 GB、常駐漲到 3.6 GB
    // The no-copy buffer goes through deferred release (out of the residency set and unreferenced after submitted commands complete). It used to be removed only from g_keep while the residency set still held a strong reference: after the pool was freed the buffer stayed alive wrapping that guest memory,
    // and a new pool at the same address wrapped it again. Engage's title demo cycles scenes; GPU allocated grew to 5.4 GB and resident memory to 3.6 GB in 850 s
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i].key == mem) { id<MTLBuffer> b = g_pools[i].buf; [g_keep removeObject:b]; defer_obj_release(b); if (g_pools[i].res) g_pool_res_n--;
            if (!g_dead_pools) g_dead_pools = [NSHashTable weakObjectsHashTable];
            [g_dead_pools addObject:b]; g_pools[i] = g_pools[--g_npool]; break; }
    pthread_mutex_unlock(&g_mu);
}

int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++)
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
            pool_res(i); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; ok = 1; break;
        }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) {
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++)
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
            pool_res(i); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; *gpu = g_pools[i].gpu + *off; ok = 1; break;
        }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---------- 提交 ----------
// ---------- Submission ----------
static void defer_reset(void); static void defer_fallback(void);
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; void *vis; } post_op;   // vis：kind 3 的可見性槽表 | vis: the visibility slot table for kind 3
// ---------- 遮擋查詢（NVN_COUNTER_TYPE_SAMPLES_PASSED）----------
// NVN 語義：通過採樣數是隊列上的累計計數器——ResetCounter 清零，此後每個繪製累加，ReportCounter 在 GPU 完成時寫出當時的累計值（+ 時間戳）。
// Metal：每個渲染通道掛可見性結果緩衝；計數期間的繪製用計數模式，每個通道一個新槽（不依賴跨通道累加）；清零時清空「自清零以來的槽表」，
// 報告時把槽表連同所在緩衝（持有）交給完成回調求和寫回；報告之後換新槽，後續繪製不計入已報告的值。
// （原先一律寫 0：遮擋查詢 = 「完全被擋」，篝火 / 光源被剔——選角頁每幀 ResetCounter 21 次）
// ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
// NVN semantics: samples passed is a cumulative counter on the queue - ResetCounter zeroes it, every draw afterwards adds to it, and ReportCounter writes out the cumulative value at that moment (+ timestamp) when the GPU completes.
// Metal: every render pass attaches a visibility result buffer; draws during counting use counting mode, with a new slot per pass (no reliance on accumulation across passes); a reset empties the 'slot table since the reset',
// and a report hands the slot table together with its buffer (retained) to the completion callback to sum and write back; after a report a new slot is used, so later draws are not counted into the reported value.
// (Previously always wrote 0: occlusion query = 'fully occluded', campfires / light sources got culled - the character select screen calls ResetCounter 21 times per frame)
typedef struct { void *buf; uint32_t slot; } vis_slot;
typedef struct { int n; vis_slot s[]; } vis_list;
#define VIS_SLOTS 8192
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence; } vis_buf;
static vis_buf g_vis_ring[4]; static int g_vis_ring_n, g_vis_ring_i = -1;
static id<MTLBuffer> g_vis; static uint32_t g_vis_n;   // 本次提交的可見性緩衝與已用槽數 | This submission's visibility buffer and number of slots used
static int vis_in_ring(id<MTLBuffer> b) { for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].buf == b) return 1; return 0; }
static int g_vis_on, g_vis_counting, g_vis_cur = -1; static uint64_t g_vis_cur_pass;
static vis_slot *g_vs; static int g_nvs, g_vscap;      // 自上次清零以來的槽 | Slots since the last reset
static uint64_t g_cnt_report[32], g_cnt_reset[32];       // 各計數器類型的報告 / 清零次數（診斷） | Report / reset counts per counter type (diagnostics)
static void vis_clear(void) { for (int i = 0; i < g_nvs; i++) CFRelease(g_vs[i].buf); g_nvs = 0; g_vis_cur = -1; }
static vis_list *vis_snapshot(void) {
    vis_list *l = malloc(sizeof *l + g_nvs * sizeof(vis_slot)); l->n = g_nvs;
    for (int i = 0; i < g_nvs; i++) { l->s[i] = g_vs[i]; CFRetain(g_vs[i].buf); }
    return l;
}
// 本次提交裡已編碼、尚未在 GPU 上執行的緩衝寫（拷貝 / uniform 內聯更新）的目標區間：編碼期在 CPU 側讀這些地址讀到的是舊值（uv_proof 讀 uniform 時避開）
// Target ranges of buffer writes (copies / inline uniform updates) already encoded in this submission but not yet executed on the GPU: reading these addresses on the CPU side at encode time yields old values (uv_proof avoids them when reading uniforms)
#define PEND_MAX 256
static uint64_t g_pend_lo[PEND_MAX], g_pend_hi[PEND_MAX]; static int g_npend, g_pend_over;
static const uint8_t *g_pend_src[PEND_MAX];   // 這次寫入的數據在宿主側的副本（內聯 uniform 更新：共享環裡的那份；拷貝：源的 CPU 視圖；填充等 = NULL） | host-side copy of the data this write puts there (inline uniform update: the copy in the shared ring; copies: the source's CPU view; fills etc. = NULL)
static int pend_hits(uint64_t a, uint64_t n) { if (g_pend_over) return 1; for (int i = 0; i < g_npend; i++) if (a < g_pend_hi[i] && a + n > g_pend_lo[i]) return 1; return 0; }
// 編碼時要讀的位置若在本提交裡還排著 GPU 側寫入：取最後一筆整段蓋住它、且有數據副本的寫入（就是 GPU 執行到此處時那裡的值）；找不到返回 NULL
// When a location read at encode time still has GPU-side writes queued in this submission: take the last write that fully covers it and has a data copy (that is the value there when the GPU gets here); NULL if none
static const uint8_t *pend_value(uint64_t a, uint64_t n) {
    if (g_pend_over) return NULL;
    for (int i = g_npend - 1; i >= 0; i--) if (a < g_pend_hi[i] && a + n > g_pend_lo[i]) return g_pend_src[i] && a >= g_pend_lo[i] && a + n <= g_pend_hi[i] ? g_pend_src[i] + (a - g_pend_lo[i]) : NULL;
    return NULL;
}
typedef struct { uint64_t ser; uint16_t level, slice; } zskip_zero;
static zskip_zero g_zskip_zero[4096]; static int g_zskip_n, g_zskip_count;
static zskip_zero g_zskip_cand[8]; static uint8_t g_zskip_cand_ok[8];
static void zskip_clear(void);
static int tbdr_on(void);
static id g_cb;
static id<MTL4ComputeCommandEncoder> g_blit;
static NSMutableArray<dispatch_block_t> *g_commit_blocks;
static uint64_t g_alloc_fence[ALLOC_MAX]; static uint64_t g_alloc_waits, g_alloc_grown;
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static void up_flush(void);
// 上傳批只屬於提交線程（發起 mtl_sub_begin 的線程）：批的編碼器不能跨線程用。每個入口斷言當前線程 == 提交線程，
// 不符當場打棧中止——讓「別的線程碰批」在 Mac 上也確定性暴露，不靠時序（iOS 真機卡主菜單：出圖線程經 q_cb 衝刷了提交線程的批）。
// The upload batch belongs only to the submit thread (the thread that called mtl_sub_begin): the batch's encoder cannot be used across threads. Every entry point asserts current thread == submit thread,
// and on mismatch prints the stack and aborts on the spot - so that 'another thread touched the batch' is exposed deterministically on Mac too, not by timing (real iOS device stuck at the main menu: the present thread flushed the submit thread's batch via q_cb).
static pthread_t g_sub_thread; static _Atomic int g_sub_thread_set;
static int on_sub_thread(void) { return atomic_load(&g_sub_thread_set) && pthread_equal(pthread_self(), g_sub_thread); }
static void up_owner_check(const char *where) {
    if (!atomic_load(&g_sub_thread_set) || on_sub_thread()) return;
    char name[64] = ""; pthread_getname_np(pthread_self(), name, sizeof name);
    MLOG("【紅】上傳批只屬於提交線程：%s 在線程「%s」上被調用", where, name);
    void *bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
    abort();
}
// 回放 / 上傳批之外另建命令緩衝一律經它。提交線程上：先衝刷本批上傳，保證隨後讀寫這些貼圖的命令排在上傳之後；
// 別的線程（出圖線程等）另建命令緩衝不碰批：它們不讀寫本批上傳的貼圖；需要這些貼圖的命令都在本次回放提交之後（批在其前衝刷）
// 舊隊列命令緩衝（讀回 / 診斷 / 上屏，M4-4、M4-5 遷走後刪）：上傳已在 Metal 4 隊列，兩隊列之間沒有先後保證——
// 衝刷上傳後等它完成，舊隊列才讀得到上傳結果（mtl_read_texels 先等後衝刷，讀回搶在上傳之前，t_nvn 上傳內容 4 紅）
// Every command buffer created outside the replay / upload batch goes through this. On the submit thread: flush this batch's uploads first, so that commands that then read or write these textures are ordered after the uploads;
// other threads (the present thread etc.) creating a command buffer do not touch the batch: they do not read or write textures uploaded in this batch; commands that need these textures all come after this replay submission (the batch is flushed before it)
// Old-queue command buffers (readback / diagnostics / present; to be deleted once M4-4 and M4-5 are migrated away): uploads are already on the Metal 4 queue, and there is no ordering guarantee between the two queues -
// wait for the upload to complete after flushing it, and only then can the old queue read the upload result (mtl_read_texels waited first and flushed afterwards, the readback beat the upload, 4 failures on t_nvn upload content)
static id<MTLCommandBuffer> q_cb(void) { if (on_sub_thread()) { up_flush(); while (atomic_load(&g_up_pending)) usleep(200); } return [g_q commandBuffer]; }
static post_op *g_post; static int g_npost, g_postcap;
// 每次提交一個自動釋放池：提交跑在客體線程上（長駐、無運行循環、從不排空隱式池），Metal 回放裡自動釋放的對象
// （命令緩衝、編碼器、通道描述等）不放池就永遠不釋放。iOS 真機選角頁實測宿主堆每幀漲約 47 個對象、0.22 MiB/s。
// One autorelease pool per submission: submissions run on a guest thread (long-lived, no run loop, never drains the implicit pool), so objects autoreleased in the Metal replay
// (command buffers, encoders, pass descriptors, etc.) are never released unless put in a pool. Measured on the character select screen of a real iOS device: the host heap grows by about 47 objects per frame, 0.22 MiB/s.
void *objc_autoreleasePoolPush(void); void objc_autoreleasePoolPop(void *);
static void *g_sub_pool;
static int g_ruler_truth = -1, g_ruler_pending;
static int tbdr_on(void);

static uint64_t g_sub_serial;   // 第幾次提交（並行預編「每次提交只預測一次」用） | Submission index (used by the parallel precompile's 'predict only once per submission')
void mtl_sub_begin(void) {
    mtl_init(); g_sub_serial++;
    if (tbdr_on() == 0 && g_ruler_pending) { g_ruler_truth = 0; g_ruler_pending = 0; MLOG("DM_NVN_RULER：切到被測模式"); }
    defer_drain();
    defer_reset();
    warm_claim();
    if (g_cb) MDIE("提交重入");
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    atomic_store(&g_last_commit_error, 0);
    g_sub_pool = objc_autoreleasePoolPush();
    g_cb = [g_dev newCommandBuffer];
    // 不在幀中間等 GPU（真機熱態：固定 3 個輪轉時 GPU 跟不上就每幀在這裡阻塞，提交線程睡醒後按低頻跑，指令數 −18% 而 CPU 時間 +49%）：
    // 挑一個 GPU 已用完的分配器；都在用就新建；到上限才等最老的那個。超前量由遊戲取交換鏈貼圖處的等待約束，與舊接口相同。
    // Do not wait for the GPU mid-frame (hot real device: with a fixed rotation of 3, when the GPU cannot keep up every frame blocks here, and the submit thread runs at low frequency after waking, instruction count −18% but CPU time +49%):
    // pick an allocator the GPU has finished with; if all are in use create a new one; only at the cap wait for the oldest one. How far ahead it runs is bounded by the wait where the game acquires the swapchain texture, same as the old interface.
    int alloc_i = -1; uint64_t done = g_done;
    for (int i = 0; i < g_alloc_n; i++) if (g_alloc_fence[i] <= done) { alloc_i = i; break; }
    if (alloc_i < 0 && g_alloc_n < ALLOC_MAX) { NSError *ae = nil; MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        g_alloc4[g_alloc_n] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae]; if (!g_alloc4[g_alloc_n]) MDIE("新建 MTL4CommandAllocator 失敗：%s", ae.localizedDescription.UTF8String);
        alloc_i = g_alloc_n++; g_alloc_grown++; }
    if (alloc_i < 0) { alloc_i = 0; for (int i = 1; i < g_alloc_n; i++) if (g_alloc_fence[i] < g_alloc_fence[alloc_i]) alloc_i = i; g_alloc_waits++; mtl_wait(g_alloc_fence[alloc_i], UINT64_MAX); }
    g_alloc_cur = alloc_i;
    [g_alloc4[alloc_i] reset];
    [g_cb beginCommandBufferWithAllocator:g_alloc4[alloc_i]];
    g_commit_blocks = [NSMutableArray array];
    // 上次提交槽用滿時另建的溢出緩衝（見 g_vis_n >= VIS_SLOTS 那一處）：退役。原先這裡直接置空，它留在駐留集裡永遠不放（取自 d2r-mobile 2eb575877）
    // The overflow buffer created when the previous submission ran out of slots (see g_vis_n >= VIS_SLOTS): retire it. It used to be just cleared here and stayed in the residency set for good (from d2r-mobile 2eb575877)
    if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis);
    g_npost = 0; g_vis = nil; g_vis_n = 0; g_vis_cur = -1; g_npend = 0; g_pend_over = 0;
    zskip_clear(); g_zskip_count = 0;
    int pick = -1;
    for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].fence <= g_done) { pick = i; break; }
    if (pick < 0 && g_vis_ring_n < 4) pick = g_vis_ring_n++;
    if (pick < 0) { uint64_t oldest = UINT64_MAX; for (int i = 0; i < 4; i++) if (g_vis_ring[i].fence < oldest) { oldest = g_vis_ring[i].fence; pick = i; } if (oldest > g_done) mtl_wait(oldest, UINT64_MAX); }
    g_vis_ring_i = pick; g_vis_ring[pick].fence = UINT64_MAX;
    if (!g_vis_ring[pick].buf) { g_vis_ring[pick].buf = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis_ring[pick].buf); }
    g_vis = g_vis_ring[pick].buf;
    void pg_sub_begin_ext(uint64_t fence); pg_sub_begin_ext(g_fence + 1);
    void arena_begin(void); arena_begin();
}

static void rp_end(void);
extern int g_frz_on; static int g_frz_state;   // 世界凍結：本幀鎖存的狀態（0 正常 / 1 完整幀 / 2 凍結幀），見 frz_op 上方的說明 | World freeze: the state latched for this frame (0 normal / 1 full frame / 2 frozen frame), see the note above frz_op
// DM_NVN_PASSGPU_SERIAL=1（測量用）：主命令緩衝上每建一個編碼器前編「發信號 n+1 → 等信號 n+1」——信號要等此前全部工作完成才發出，
// 後面的編碼器因此等前一個徹底跑完才開始；TBDR 上通道不再流水重疊，DM_NVN_PASSGPU 的逐通道起止跨度即該通道獨佔耗時（整幀總時長偏大，只看相對開銷）
// DM_NVN_PASSGPU_SERIAL=1 (for measurement): before creating each encoder on the main command buffer, encode 'signal n+1 → wait for signal n+1' - the signal is only raised once all prior work has completed,
// so the following encoder starts only after the previous one has fully finished; on TBDR passes no longer overlap in the pipeline, and the per-pass start-to-end span of DM_NVN_PASSGPU is that pass's exclusive time (the whole-frame total is inflated; look only at relative cost)
static void ser_gate(void) {
    static int on = -1; static id<MTLEvent> ev; static uint64_t n;
    if (on < 0) { on = getenv("DM_NVN_PASSGPU_SERIAL") != NULL; if (on) { ev = [g_dev newEvent]; MLOG("DM_NVN_PASSGPU_SERIAL：通道串行化（測量用，耗時偏大）"); } }
    if (!on) return;
    (void)ev; (void)n;
}
// Metal 4 保守屏障（第一版）：編碼器開頭一道消費者屏障（之前全部工作 → 本編碼器），結束處一道生產者屏障（本編碼器及之前 → 後續編碼器）。
// 只放開頭不夠：t_rt_range「通道 1 → 通道 2 → 拷貝」讀回黑色，補上結束處屏障後 19/19。舊隊列的編碼器（上傳 / 診斷 / 上屏）不走這裡的屏障。
// Metal 4 conservative barriers (first version): a consumer barrier at the start of an encoder (all prior work → this encoder) and a producer barrier at the end (this encoder and earlier → later encoders).
// Only the one at the start is not enough: t_rt_range 'pass 1 → pass 2 → copy' read back black, and after adding the end barrier it is 19/19. Old-queue encoders (upload / diagnostics / present) do not go through these barriers.
static inline void m4_end(id e) {
    if ([e conformsToProtocol:@protocol(MTL4CommandEncoder)]) [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [e endEncoding];
}
static inline id<MTL4RenderCommandEncoder> enc_r_begin(MTL4RenderPassDescriptor *d) { id<MTL4RenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d]; [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [e setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [e setArgumentTable:g_ft atStages:MTLRenderStageFragment]; return e; }
static inline void enc_r_end(id e) { m4_end(e); }
static inline void enc_r_pso(id<MTL4RenderCommandEncoder> e, id<MTLRenderPipelineState> p) { [e setRenderPipelineState:p]; }
static inline void enc_r_dss(id<MTL4RenderCommandEncoder> e, id<MTLDepthStencilState> s) { [e setDepthStencilState:s]; }
static inline void enc_r_viewport(id<MTL4RenderCommandEncoder> e, MTLViewport v) { [e setViewport:v]; }
static inline void enc_r_scissor(id<MTL4RenderCommandEncoder> e, MTLScissorRect r) { [e setScissorRect:r]; }
static inline void enc_r_cull(id<MTL4RenderCommandEncoder> e, MTLCullMode m) { [e setCullMode:m]; }
static inline void enc_r_winding(id<MTL4RenderCommandEncoder> e, MTLWinding w) { [e setFrontFacingWinding:w]; }
static inline void enc_r_fill(id<MTL4RenderCommandEncoder> e, MTLTriangleFillMode m) { [e setTriangleFillMode:m]; }
static inline void enc_r_depth_bias(id<MTL4RenderCommandEncoder> e, float u, float s, float c) { [e setDepthBias:u slopeScale:s clamp:c]; }
static inline void enc_r_depth_clip(id<MTL4RenderCommandEncoder> e, BOOL v) { [e setDepthClipMode:v ? MTLDepthClipModeClip : MTLDepthClipModeClamp]; }
static inline void enc_r_stencil_ref(id<MTL4RenderCommandEncoder> e, uint32_t f, uint32_t b) { (void)b; [e setStencilReferenceValue:f]; }
static inline void enc_r_blend_color(id<MTL4RenderCommandEncoder> e, float r, float g, float b, float a) { [e setBlendColorRed:r green:g blue:b alpha:a]; }
static inline void enc_r_visibility(id<MTL4RenderCommandEncoder> e, MTLVisibilityResultMode m, NSUInteger o) { [e setVisibilityResultMode:m offset:o]; }
static inline void enc_r_vbuf(id<MTL4RenderCommandEncoder> e, NSUInteger i, uint64_t gpu) { (void)e; [g_vt setAddress:gpu atIndex:i]; }
static inline void enc_r_args(id<MTL4RenderCommandEncoder> e, MTLRenderStages s, uint64_t gpu, NSUInteger i) { (void)e; [(s == MTLRenderStageVertex ? g_vt : g_ft) setAddress:gpu atIndex:i]; }
static inline void enc_r_draw(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n) { [e drawPrimitives:p vertexStart:v vertexCount:n]; }
static inline void enc_r_draw_instanced(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n, NSUInteger i, NSUInteger b) { [e drawPrimitives:p vertexStart:v vertexCount:n instanceCount:i baseInstance:b]; }
static inline void enc_r_draw_indexed(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger n, MTLIndexType t, uint64_t gpu, NSUInteger len, NSUInteger i, NSInteger bv, NSUInteger bi) { [e drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:gpu indexBufferLength:len instanceCount:i baseVertex:bv baseInstance:bi]; }
static inline id<MTL4ComputeCommandEncoder> enc_c_begin(MTLComputePassDescriptor *d) { (void)d; id<MTL4ComputeCommandEncoder> e = [g_cb computeCommandEncoder]; [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [e setArgumentTable:g_ct]; return e; }
static inline id<MTLComputeCommandEncoder> enc_c_begin_concurrent(id<MTLCommandBuffer> cb) { return [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent]; }
static inline void enc_c_end(id e) { m4_end(e); }
static inline void enc_c_pso(id e, id<MTLComputePipelineState> p) { [e setComputePipelineState:p]; }
static inline void enc_c_args(id e, uint64_t gpu, NSUInteger i) { (void)e; [g_ct setAddress:gpu atIndex:i]; }
static inline void enc_c_dispatch(id e, MTLSize n, MTLSize t) { [e dispatchThreadgroups:n threadsPerThreadgroup:t]; }
static inline id enc_b_begin(id cb) { return [cb respondsToSelector:@selector(beginCommandBufferWithAllocator:)] ? [cb computeCommandEncoder] : [cb blitCommandEncoder]; }
static inline void enc_b_end(id e) { m4_end(e); }
static inline void enc_b_copy_buffer(id e, id<MTLBuffer> s, NSUInteger so, id<MTLBuffer> d, NSUInteger doff, NSUInteger n) { [e copyFromBuffer:s sourceOffset:so toBuffer:d destinationOffset:doff size:n]; }
static inline void enc_b_copy_buffer_texture(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o]; }
static inline void enc_b_copy_texture_buffer(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img]; }
static inline void enc_b_copy_texture(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin so, MTLSize z, id<MTLTexture> d, NSUInteger dl, NSUInteger dv, MTLOrigin doo) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:so sourceSize:z toTexture:d destinationSlice:dl destinationLevel:dv destinationOrigin:doo]; }
static void blit_note(void);   // 逐通道轉儲：拷貝 / 緩衝更新結束了渲染通道（定義在 PD 之後） | Per-pass dump: a copy / buffer update ended the render pass (defined after PD)
// 拷貝編碼器內的先後：Metal 4 同一編碼器裡的拷貝 / 填充互不排序（無自動冒險跟蹤），後一條讀寫前一條寫過的範圍要編碼器內屏障。
// 缺了它，SMBW 每幀對同一 uniform 槽「先寫 4 字節 −1、再寫 16 字節系數」兩次更新偶爾反序落地，噴泉水面的波形模擬拿 −1 當自身系數、發散，水面頂點被推到屏幕頂。
// 緩衝操作按客體地址範圍判衝突（讀寫前面寫過的範圍才加屏障）；貼圖操作只要編碼器裡已有操作就加。
// Ordering inside the copy encoder: Metal 4 does not order copies / fills within one encoder (no automatic hazard tracking); an operation reading or writing a range an earlier one wrote needs an intra-encoder barrier.
// Without it, SMBW's two updates per frame to the same uniform slot ("write 4 bytes −1, then 16 bytes of coefficients") occasionally landed in reverse order; the fountain water wave simulation took −1 as its self coefficient, diverged, and the water vertices were pushed to the top of the screen.
// Buffer operations check conflicts by guest address range (barrier only when reading or writing a range written earlier); texture operations get one whenever the encoder already holds an operation.
enum { BW_MAX = 64 };
static uint64_t g_bw_lo[BW_MAX], g_bw_hi[BW_MAX]; static int g_bw_n, g_blit_ops; static uint64_t g_blit_barriers;
static id<MTL4ComputeCommandEncoder> blit_raw(void) { blit_note(); rp_end(); if (!g_blit) { ser_gate(); g_blit = enc_b_begin(g_cb); [g_blit barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; g_bw_n = 0; g_blit_ops = 0; } return g_blit; }
static void blit_barrier(void) {
    static int off = -1; if (off < 0) { const char *e = getenv("DM_NVN_BLIT_ORDER"); off = e && !strcmp(e, "0"); if (off) MLOG("DM_NVN_BLIT_ORDER=0：拷貝編碼器內不加屏障（診斷，會復現噴泉竪條）"); }
    if (!off) [g_blit barrierAfterEncoderStages:MTLStageAll beforeEncoderStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    g_bw_n = 0; g_blit_ops = 0; g_blit_barriers++;
}
uint64_t mtl_blit_barriers(void) { return g_blit_barriers; }
static int bw_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_bw_n; i++) if (lo < g_bw_hi[i] && g_bw_lo[i] < hi) return 1; return 0; }
// 緩衝 → 緩衝 / 填充：讀 [rlo, rhi)（無讀給 0, 0）、寫 [wlo, whi) | buffer → buffer / fill: reads [rlo, rhi) (0, 0 when nothing is read), writes [wlo, whi)
static id<MTL4ComputeCommandEncoder> blit_buf(uint64_t rlo, uint64_t rhi, uint64_t wlo, uint64_t whi) {
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    if (g_blit_ops && (g_bw_n == BW_MAX || (rhi > rlo && bw_hit(rlo, rhi)) || bw_hit(wlo, whi))) blit_barrier();
    g_bw_lo[g_bw_n] = wlo; g_bw_hi[g_bw_n++] = whi; g_blit_ops++;
    return e;
}
// 貼圖相關的拷貝 / 讀回：前面有操作就先屏障，之後的操作也要等它 | texture copies / readbacks: barrier if anything came before, and later operations must wait for it too
static id<MTL4ComputeCommandEncoder> blit(void) {
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    if (g_blit_ops) blit_barrier();
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_blit_ops = 1;   // 記成「全範圍已寫」：下一條一律加屏障 | record as "everything written": the next operation always gets a barrier
    return e;
}

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    void *sb, *db; uint64_t so, dof;
    if (!mtl_addr(src, n, &sb, &so) || !mtl_addr(dst, n, &db, &dof))
        MDIE("緩衝拷貝地址不在任何內存池內 src=%#llx dst=%#llx n=%#llx", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    const uint8_t *sp = [(__bridge id<MTLBuffer>)sb contents];
    if (g_npend < PEND_MAX) { g_pend_lo[g_npend] = dst; g_pend_hi[g_npend] = dst + n; g_pend_src[g_npend++] = sp && !pend_hits(src, n) ? sp + so : NULL; } else g_pend_over = 1;
    enc_b_copy_buffer(blit_buf(src, src + n, dst, dst + n), (__bridge id<MTLBuffer>)sb, so, (__bridge id<MTLBuffer>)db, dof, n);
}

// 內聯 uniform 更新（UpdateUniformBuffer）：數據在編碼時就拷進宿主自己的共享環，再按序拷到目標。
// 原先 GPU 執行時才從命令內存讀——遊戲回收命令內存若早於 GPU 執行，讀到的是下一批命令的字節（SMBW 噴泉水面的波形模擬系數被換掉，模擬發散到 ±65504）。
// Inline uniform update (UpdateUniformBuffer): the data is copied into the host's own shared ring at encode time, then copied in order to the destination.
// Previously it was read from command memory only when the GPU executed - if the game recycles command memory before the GPU runs, it reads the bytes of the next batch of commands (SMBW's fountain water wave simulation got the wrong coefficients and diverged to ±65504).
static id<MTLBuffer> g_ubs_buf; static size_t g_ubs_off, g_ubs_size;
void mtl_sub_update(const void *data, uint64_t dst, uint64_t n) {
    if (!n) return;
    int pi = g_npend < PEND_MAX ? g_npend : -1;
    if (pi >= 0) { g_pend_lo[g_npend] = dst; g_pend_hi[g_npend] = dst + n; g_pend_src[g_npend++] = NULL; } else g_pend_over = 1;
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("uniform 更新目標不在任何內存池內 dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    size_t need = (n + 15u) & ~(size_t)15u;
    if (!g_ubs_buf || g_ubs_off + need > g_ubs_size) {
        if (g_ubs_buf) defer_obj_release(g_ubs_buf);   // 已編的拷貝還引用著舊塊：本次提交跑完再放 | copies already encoded still reference the old block: release after this submission completes
        g_ubs_size = MAX((size_t)1 << 20, need);
        g_ubs_buf = [g_dev newBufferWithLength:g_ubs_size options:MTLResourceStorageModeShared];
        if (!g_ubs_buf) MDIE("uniform 更新環分配失敗 %#zx", g_ubs_size);
        res_add(g_ubs_buf); g_ubs_off = 0;
    }
    memcpy((uint8_t *)g_ubs_buf.contents + g_ubs_off, data, n);
    if (pi >= 0) g_pend_src[pi] = (const uint8_t *)g_ubs_buf.contents + g_ubs_off;   // 環裡這份留到本提交跑完 | this copy in the ring lives until the submission completes
    enc_b_copy_buffer(blit_buf(0, 0, dst, dst + n), g_ubs_buf, g_ubs_off, (__bridge id<MTLBuffer>)db, dof, n);
    g_ubs_off += need;
}

// 提交內按序填充緩衝：四個字節一致 → fillBuffer；否則從一塊 64 KiB 圖案緩衝分段拷
// In-order buffer fill within the submission: all four bytes equal → fillBuffer; otherwise copied in chunks from a 64 KiB pattern buffer
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!n) return;
    if (g_npend < PEND_MAX) { g_pend_lo[g_npend] = dst; g_pend_hi[g_npend] = dst + n; g_pend_src[g_npend++] = NULL; } else g_pend_over = 1;
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("緩衝填充地址不在任何內存池內 dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    uint8_t b0 = (uint8_t)value;
    if (value == (uint32_t)b0 * 0x01010101u) { [blit_buf(0, 0, dst, dst + n) fillBuffer:(__bridge id<MTLBuffer>)db range:NSMakeRange(dof, n) value:b0]; return; }
    enum { PAT = 0x10000 }; static __strong id<MTLBuffer> pat; static uint32_t pat_v; static int pat_ok;
    if (!pat_ok || pat_v != value) { uint32_t *tmp = malloc(PAT); for (unsigned i = 0; i < PAT / 4; i++) tmp[i] = value;
        pat = [g_dev newBufferWithBytes:tmp length:PAT options:MTLResourceStorageModeShared]; free(tmp); pat_v = value; pat_ok = 1; res_add(pat); }
    for (uint64_t off = 0; off < n; off += PAT) { uint64_t k = n - off < PAT ? n - off : PAT; enc_b_copy_buffer(blit_buf(0, 0, dst + off, dst + off + k), pat, 0, (__bridge id<MTLBuffer>)db, dof + off, k); }
}

static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, g_postcap * sizeof *g_post); }
    g_post[g_npost++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, NULL}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type, NULL}); }
void mtl_counter_stats(uint64_t rep[32], uint64_t rst[32]) { memcpy(rep, g_cnt_report, sizeof g_cnt_report); memcpy(rst, g_cnt_reset, sizeof g_cnt_reset); }

// 計數器時戳（19.2 MHz）取客體時鐘（hle_os.c guest_ns，與 svc GetSystemTick / cntpct 同源）：真機上 GPU 時戳與 CPU 計數同一時基，遊戲會拿兩者相比。
// 原先取 CLOCK_MONOTONIC：它在 Mac 睡眠時照走，而客體時鐘（mach_absolute_time）不走，兩者差出睡眠時長；時戳換成納秒後馬力歐把幀率減半。圖形單測裡 guest_ns 是返回 0 的弱定義，退回單調時鐘
// Counter timestamps (19.2 MHz) are taken from the guest clock (hle_os.c guest_ns, the same source as svc GetSystemTick / cntpct): on hardware GPU timestamps and the CPU counter share one timebase, and games compare them.
// They used to come from CLOCK_MONOTONIC, which keeps running while the Mac sleeps while the guest clock (mach_absolute_time) does not, so the two differed by the sleep time; once timestamps were converted to ns, Mario halved its frame rate. In graphics unit tests guest_ns is a weak definition returning 0; fall back to the monotonic clock
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; } return g * 12 / 625; }

// ---------- 啓動埋點（DM_BOOTPROF=1）：metallib 載入、管線創建（均在提交線程回放中同步發生）、貼圖上傳、首幀出圖 ----------
// ---------- Boot instrumentation (DM_BOOTPROF=1): metallib loading, pipeline creation (both happen synchronously during replay on the submit thread), texture upload, first-frame present ----------
static uint64_t bp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static uint64_t g_bp_t0;   // 進程啓動（本模塊裝入）時刻 | Time of process start (when this module is loaded)
__attribute__((constructor)) static void bp_t0(void) { g_bp_t0 = bp_now(); }
static int bootprof(void) { static int v = -1; if (v < 0) v = getenv("DM_BOOTPROF") != NULL; return v; }
static uint64_t g_bp_lib_n, g_bp_lib_ns, g_bp_pso_n, g_bp_pso_ns, g_bp_up_bytes, g_bp_first_present;
static uint64_t g_up_total_ns;   // 上傳累計耗時（與逐幀 @upload_ms 同源） | Cumulative upload time (same source as the per-frame @upload_ms)
// ---------- 按幀歸集 GPU 時間 ----------
// ---------- Per-frame GPU time aggregation ----------
#define GRING 1024
static struct { _Atomic uint64_t frame, ns, t0, t1; _Atomic int pending; } g_gpu[GRING];   // t0 / t1：該幀最早 GPUStart / 最晚 GPUEnd（mach 絕對時間 ns） | t0 / t1: the frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns)
static _Atomic uint64_t g_cur_frame;
// ---------- 同進程交替測量（DM_NVN_UB，缺省無：不設環境變量時下面只多一次「是否啓用」的判斷） ----------
// 真機熱態下系統持續調功率 / 頻率，分段對比（A 段、B 段各跑一分鐘）分不清 <10% 的差；在同一局裡按相輪流開關、相鄰相配對取差，漂移對消。
// DM_NVN_UB="名字=項,項;名字=項"，每 DM_NVN_UB_N 幀（缺省 180，下限 60）一相：偶數相 = 基準（什麼都不改），奇數相輪流取一類。一類裡的每一項：
//   @set:<全局變量名>=<值>   進相時把這個 int 全局變量設成該值、出相時還原（按符號名找地址；真機上要用的開關須在下面的登記表裡加一行）
//   <片元著色器鍵前綴> 或 *   該相丟掉這些片元著色器的全部繪製（畫面會錯，只量「這類繪製值多少」；近乎空載的類別要單獨成輪）
// 讀數：每相丟掉前 45 幀（過渡 + 上屏滯後），其餘幀的實際上屏間隔（與 perf.txt 同源）均值與 16.7 / 25 / ≥33 ms 三檔幀數，日誌一行「UB 上屏」。
// 出表：$DM_ROOT/run/ruler/ub_table.py；真機腳本：$DM_ROOT/run/ruler/ub_alt.sh。
// ---------- Same-process alternating measurement (DM_NVN_UB, off by default: without the environment variable the code below only adds one 'is it enabled' check) ----------
// On a hot real device the system keeps adjusting power / frequency, and segment comparison (segments A and B each run for a minute) cannot resolve differences of <10%; within the same run, toggle by phase in turn and take differences of adjacent phase pairs, so the drift cancels.
// DM_NVN_UB="name=item,item;name=item", one phase every DM_NVN_UB_N frames (default 180, minimum 60): even phases = baseline (nothing changed), odd phases take one category in turn. Each item of a category:
//   @set:<global variable name>=<value>   on entering the phase set this int global variable to the value, on leaving restore it (address found by symbol name; switches to be used on a real device must have a line added to the registration table below)
//   <fragment shader key prefix> or *   the phase drops all draws of these fragment shaders (the picture will be wrong; it only measures 'how much this kind of draw is worth'; a nearly idle category needs a round of its own)
// Readings: each phase discards its first 45 frames (transition + present lag); for the remaining frames, the mean of the actual present interval (same source as perf.txt) and the frame counts in the three buckets 16.7 / 25 / ≥33 ms, one log line 「UB 上屏」(UB present).
// Table output: $DM_ROOT/run/ruler/ub_table.py; real-device script: $DM_ROOT/run/ruler/ub_alt.sh.
static int g_ub_n = -1, g_ub_N = 180; static _Atomic int g_ub_cur = -1; static char g_ub_name[16][16], g_ub_keys[16][16][48]; static int g_ub_nk[16];
static _Atomic int g_ubp_on, g_ubp_c[4]; static _Atomic uint64_t g_ubp_sum_us;
void mtl_ub_present(double iv) {   // 出圖線程每幀調一次：實際上屏間隔 ms（−1 = 被丟棄，0 = 無讀數） | Called once per frame by the present thread: actual present interval in ms (−1 = dropped, 0 = no reading)
    if (!atomic_load(&g_ubp_on) || iv == 0) return;
    if (iv < 0) { atomic_fetch_add(&g_ubp_c[3], 1); return; }
    atomic_fetch_add(&g_ubp_c[iv <= 20.8 ? 0 : iv <= 29.2 ? 1 : 2], 1); atomic_fetch_add(&g_ubp_sum_us, (uint64_t)(iv * 1000));
}
// 登記過的運行時開關：名字 → 地址。iOS 發佈包的主程序符號不進動態符號表，dlsym 找不到（真機首輪四類全部「找不到全局變量」、整輪白跑），
// 所以真機要量的開關必須在這裡加一行；Mac / 單測裡沒登記的仍按符號名找。
// Registered runtime switches: name → address. Main-program symbols of an iOS release build are not in the dynamic symbol table and dlsym cannot find them (in the first real-device round all four categories were 「找不到全局變量」(global variable not found) and the whole round was wasted),
// so a switch to be measured on a real device must have a line added here; on Mac / in unit tests unregistered ones are still looked up by symbol name.
static int *ub_var(const char *nm) {
    extern int g_bloom_q, g_dm_present_link;
    static const struct { const char *n; int *p; } t[] = { {"g_bloom_q", &g_bloom_q}, {"g_dm_present_link", &g_dm_present_link}, {"g_frz_on", &g_frz_on} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(nm, t[i].n)) return t[i].p;
    return dlsym(RTLD_DEFAULT, nm);
}
static void ub_frame(uint64_t f) {
    if (g_ub_n < 0) { g_ub_n = 0; const char *e = getenv("DM_NVN_UB"), *n = getenv("DM_NVN_UB_N"); if (n && atoi(n) >= 60) g_ub_N = atoi(n);
        for (const char *q = e; q && *q && g_ub_n < 16; ) { const char *eq = strchr(q, '='), *end = strchr(q, ';'); if (!end) end = q + strlen(q); if (!eq || eq > end) break;
            int c = g_ub_n++; size_t l = (size_t)(eq - q); if (l > 15) l = 15; memcpy(g_ub_name[c], q, l);
            for (const char *k = eq + 1; k < end && g_ub_nk[c] < 16; ) { size_t kl = 0; while (k + kl < end && k[kl] != ',') kl++; size_t cl = kl > 47 ? 47 : kl; memcpy(g_ub_keys[c][g_ub_nk[c]++], k, cl); k += kl; if (k < end) k++; }
            q = *end ? end + 1 : end; }
        if (g_ub_n) MLOG("DM_NVN_UB：%d 類，每相 %d 幀（同進程交替測量；丟繪製的相畫面不對）", g_ub_n, g_ub_N); }
    if (!g_ub_n) return;
    uint64_t ph = f / (uint64_t)g_ub_N, r = f % (uint64_t)g_ub_N;
    if (r == 45) { for (int i = 0; i < 4; i++) atomic_store(&g_ubp_c[i], 0); atomic_store(&g_ubp_sum_us, 0); atomic_store(&g_ubp_on, 1); }
    if (r != 0) return;
    if (f && atomic_load(&g_ubp_on)) { atomic_store(&g_ubp_on, 0); uint64_t pp = ph - 1; int pc = (pp & 1) ? (int)((pp / 2) % (uint64_t)g_ub_n) : -1;
        int c0 = atomic_load(&g_ubp_c[0]), c1 = atomic_load(&g_ubp_c[1]), c2 = atomic_load(&g_ubp_c[2]), c3 = atomic_load(&g_ubp_c[3]), n_ = c0 + c1 + c2;
        MLOG("UB 上屏 %llu %s 幀 %d 均 %.3f ms 2拍 %d 3拍 %d 4拍以上 %d 丟棄 %d", (unsigned long long)pp, pc < 0 ? "base" : g_ub_name[pc], n_, n_ ? atomic_load(&g_ubp_sum_us) / 1000.0 / n_ : 0.0, c0, c1, c2, c3); }
    int nc = (ph & 1) ? (int)((ph / 2) % (uint64_t)g_ub_n) : -1;
    static int *sp[16]; static int sv[16], ns;   // 上一相設過的變量：先還原 | Variables set by the previous phase: restore first
    for (int i = 0; i < ns; i++) *sp[i] = sv[i]; ns = 0;
    for (int i = 0; nc >= 0 && i < g_ub_nk[nc] && ns < 16; i++) { const char *k = g_ub_keys[nc][i]; if (strncmp(k, "@set:", 5)) continue;
        char nm[48]; const char *eq = strchr(k, '='); if (!eq) continue; size_t l = (size_t)(eq - (k + 5)); if (l >= sizeof nm) continue; memcpy(nm, k + 5, l); nm[l] = 0;
        int *a_ = ub_var(nm); static int bad, ok;
        if (!a_) { if (bad++ < 8) MLOG("【紅】DM_NVN_UB：找不到全局變量 %s（真機上要先在 ub_var 的表裡登記）", nm); continue; }
        sp[ns] = a_; sv[ns] = *a_; ns++; *a_ = atoi(eq + 1); if (ok++ < 8) MLOG("DM_NVN_UB：%s = %d（原值 %d）", nm, atoi(eq + 1), sv[ns - 1]); }
    atomic_store(&g_ub_cur, nc);
}
static int ub_skip(const char *key) {   // 本相要不要丟掉這個片元著色器的繪製 | Whether this phase should drop draws of this fragment shader
    int uc = atomic_load(&g_ub_cur); if (uc < 0) return 0;
    for (int i = 0; i < g_ub_nk[uc]; i++) { const char *k = g_ub_keys[uc][i]; if (k[0] == '@') continue; if (k[0] == '*' || !strncmp(key, k, strlen(k))) return 1; }
    return 0;
}
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); if (g_ub_n) ub_frame(f); }
// GPU 計時登記：提交前 gpu_tag_begin(幀)，完成時 gpu_tag_done(幀, GPU 起, GPU 止)（CACurrentMediaTime 基準秒）。
// 舊隊列命令緩衝走 gpu_tag_f；Metal 4 主提交在 mtl_sub_commit 的 feedback 裡回填（換 Metal 4 時調用點全丟，「計時」行的 GPU 一欄成了空）
// GPU timing registration: gpu_tag_begin(frame) before commit, gpu_tag_done(frame, GPU start, GPU end) on completion (seconds on the CACurrentMediaTime base).
// Old-queue command buffers go through gpu_tag_f; the Metal 4 main submission backfills in the feedback of mtl_sub_commit (when switching to Metal 4 all the call sites were lost and the GPU column of the 「計時」(timing) line became empty)
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) { atomic_store(&g_gpu[k].frame, f); atomic_store(&g_gpu[k].ns, 0); atomic_store(&g_gpu[k].pending, 0); atomic_store(&g_gpu[k].t0, UINT64_MAX); atomic_store(&g_gpu[k].t1, 0); }
    atomic_fetch_add(&g_gpu[k].pending, 1);
}
static void gpu_tag_done(uint64_t f, double g0, double g1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) return;
    double d = g1 - g0;
    if (d > 0) {
        atomic_fetch_add(&g_gpu[k].ns, (uint64_t)(d * 1e9));
        uint64_t s0 = (uint64_t)(g0 * 1e9), s1 = (uint64_t)(g1 * 1e9), v;
        for (v = atomic_load(&g_gpu[k].t0); s0 < v && !atomic_compare_exchange_weak(&g_gpu[k].t0, &v, s0);) {}
        for (v = atomic_load(&g_gpu[k].t1); s1 > v && !atomic_compare_exchange_weak(&g_gpu[k].t1, &v, s1);) {}
    }
    atomic_fetch_sub(&g_gpu[k].pending, 1);
}
static void gpu_tag_f(id<MTLCommandBuffer> cb, uint64_t f) { gpu_tag_begin(f); [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { gpu_tag_done(f, c.GPUStartTime, c.GPUEndTime); }]; }
void gpu_tag(id<MTLCommandBuffer> cb) { gpu_tag_f(cb, atomic_load(&g_cur_frame)); }
double mtl_gpu_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return -1;
    return atomic_load(&g_gpu[k].ns) / 1e6;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return 0;
    *t0 = atomic_load(&g_gpu[k].t0); *t1 = atomic_load(&g_gpu[k].t1);
    return *t1 >= *t0 && *t1;
}
static void rp_flush(void);
static const char *cctx_snap(void);   // 這次提交裡有什麼（出錯時打進紅行；定義在 mtl_exec 之前） | What is in this submission (printed into the failure line on error; defined before mtl_exec)
uint64_t mtl_sub_commit(void) {
    defer_fallback();   // 推遲合併通道：提交結束仍有未回放的 P 繪製（正常路徑不會到這裡） | Deferred merged pass: at the end of the submission there are still P draws not replayed (the normal path never gets here)
    rp_flush();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    uint64_t fence = ++g_fence;
    if (g_vis_ring_i >= 0) g_vis_ring[g_vis_ring_i].fence = fence;
    // 完成後的 CPU 可見效果：拷貝一份操作表進回調
    // CPU-visible effects after completion: copy the op table into the callback
    int n = g_npost; post_op *ops = n ? malloc(n * sizeof *ops) : NULL;
    if (n) memcpy(ops, g_post, n * sizeof *ops);
    NSArray<dispatch_block_t> *commit_blocks = [g_commit_blocks copy];
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)f; for (dispatch_block_t block in commit_blocks) block(); }];
    uint64_t gpu_f = atomic_load(&g_cur_frame); gpu_tag_begin(gpu_f);
    NSString *cctx = @(cctx_snap());
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> cb) {
        gpu_tag_done(gpu_f, cb.GPUStartTime, cb.GPUEndTime);
        if (cb.error) { atomic_store(&g_last_commit_error, 1); static int n, nv; int victim = [cb.error.description containsString:@"InnocentVictim"]; if (victim ? nv++ < 1 : n++ < 8) MLOG("【紅】命令緩衝錯誤詳情 fence=%llu：%s | GPU %.3f→%.3f", (unsigned long long)fence, cb.error.description.UTF8String, cb.GPUStartTime, cb.GPUEndTime); if (!victim) MLOG("【紅】出錯那次提交的現場 fence=%llu：%s", (unsigned long long)fence, cctx.UTF8String); MLOG("【紅】命令緩衝執行出錯：%s", cb.error.localizedDescription.UTF8String); }
        for (int i = 0; i < n; i++) {
            if (ops[i].kind == 1) { if (ops[i].mode == 1) *ops[i].val += ops[i].v; else *ops[i].val = ops[i].v; }
            else if (ops[i].kind == 3) {   // 通過採樣數：自清零以來各槽之和 | Samples passed: the sum of all slots since the reset
                vis_list *l = ops[i].vis; uint64_t sum = 0;
                for (int k = 0; k < l->n; k++) { sum += ((const uint64_t *)((__bridge id<MTLBuffer>)l->s[k].buf).contents)[l->s[k].slot]; CFRelease(l->s[k].buf); }
                free(l); uint64_t rec[2] = {sum, gpu_ticks()}; memcpy((void *)ops[i].addr, rec, sizeof rec);
            }
            else { uint64_t t = gpu_ticks(), rec[2] = {ops[i].type == 0 || ops[i].type == 0x10 ? t : 0, t}; memcpy((void *)ops[i].addr, rec, sizeof rec); }
        }
        free(ops);
        pthread_mutex_lock(&g_mu); if (fence > g_done) g_done = fence; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        defer_drain();
    }];
    void arena_commit(uint64_t fence); arena_commit(fence);
    res_commit();
    [g_cb endCommandBuffer];
    id<MTL4CommandBuffer> buffers[] = {g_cb};
    [g_q4 commit:buffers count:1 options:o];
    [g_q4 signalEvent:g_ev value:fence];
    g_alloc_fence[g_alloc_cur] = fence;
    g_cb = nil;
    g_commit_blocks = nil;
    objc_autoreleasePoolPop(g_sub_pool); g_sub_pool = NULL;
    return fence;
}

uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_last_commit_error(void) { return atomic_load(&g_last_commit_error); }

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += ns / 1000000000ull; dl.tv_nsec = ns % 1000000000ull;
    pthread_mutex_lock(&g_mu);
    int r = 0;
    while (g_done < fence) if ((r = pthread_cond_timedwait(&g_cv, &g_mu, &dl))) break;
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static void mtl_finish_gpu(void) {
    if (g_fence) mtl_wait(g_fence, UINT64_MAX);
    while (atomic_load(&g_up_pending)) usleep(1000);
}
void mtl_present_flush(void);
void mtl_finish(void) { mtl_present_flush(); mtl_finish_gpu(); }   // 隊列語義：已入隊的出圖也算隊列工作 | Queue semantics: already-enqueued presents also count as queue work
static void present_drop_for_suspend(void);
void mtl_drain(void) { present_drop_for_suspend(); mtl_present_flush(); mtl_init(); id<MTLCommandBuffer> cb = q_cb(); [cb commit]; [cb waitUntilCompleted]; mtl_finish_gpu(); }

// ================= 渲染回放 =================
// ================= Render replay =================
#include "../../host/front.h"
static int g_red_once[80];
#define RED_ONCE(k, ...) do { if (!g_red_once[k]++) MLOG("【紅】" __VA_ARGS__); } while (0)

// ---------- 貼圖 → MTLTexture ----------
// ---------- Texture → MTLTexture ----------
static int fmt_compressed(int f) { return (f >= 0x41 && f <= 0x50) || (f >= 0x79 && f <= 0x94); }
static int fmt_depth(int f) { return f >= 0x31 && f <= 0x36; }
typedef struct { MTLPixelFormat a, b; } pfv_pair;
static const pfv_pair g_pfv_srgb[] = {
    {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB},
    {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB},
    {MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB},
    {MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB},
    {MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB},
    {MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB},
};
static int tex_need_pfv(MTLPixelFormat base, MTLPixelFormat view) {
    if (base == view) return 0;
    for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++)
        if ((base == g_pfv_srgb[i].a && view == g_pfv_srgb[i].b) || (base == g_pfv_srgb[i].b && view == g_pfv_srgb[i].a)) return 0;
    return 1;
}
// 回放熱路徑的緩存表一律用 nvn_hmap（C 哈希，鍵不裝箱）；表裡存 CFBridgingRetain 過的對象，替換 / 刪除時 CFRelease
// Cache tables on the replay hot path always use nvn_hmap (C hash, keys not boxed); the table stores CFBridgingRetain'ed objects, CFRelease'd on replace / delete
static void hm_rel(void *v) { CFRelease(v); }
static __unsafe_unretained id hm_get_id(const nvn_hmap *m, const void *k) { return (__bridge id)nvn_hm_get(m, k); }
static void hm_put_id(nvn_hmap *m, const void *k, id o) { void *old = nvn_hm_put(m, k, (void *)CFBridgingRetain(o)); if (old) CFRelease(old); }
static void hm_del_id(nvn_hmap *m, const void *k) { void *old = nvn_hm_del(m, k); if (old) CFRelease(old); }
static nvn_hmap g_by_serial = {.klen = 8};     // 序號 → MTLTexture / NvPkg / NvProg | Serial → MTLTexture / NvPkg / NvProg
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
// 序號表代號：表項增刪、預排布貼圖換新時 +1。下游緩存（貼圖視圖、程序）記下取用時的代號，代號未變即可直接用、不必再查表
// Serial table generation: +1 when entries are added / removed or a packaged texture is replaced. Downstream caches (texture views, programs) record the generation at fetch time; if it is unchanged they can be used directly without another table lookup
static _Atomic uint64_t g_ser_gen = 1;
// 按存儲共用的貼圖（非預排布）：鍵 = 內存池 + 偏移 + 貼圖描述。NVN 的貼圖對象只是描述符，同一段存儲上按同樣描述再建的對象就是同一張貼圖；
// SMBW 的臨時渲染目標每幀在新的對象上重建、從不 Finalize，按序號各建一張會每幀漏約 13 MB 顯卡內存。這裡的貼圖由共用表持有駐留，
// 序號表更替 / 刪除時只放引用（defer_obj_unref），不移出駐留集。共用表不淘汰（條目數 = 不同的存儲 + 描述組合）。
// Textures shared by storage (non-packaged): key = memory pool + offset + texture description. An NVN texture object is only a descriptor; an object rebuilt over the same storage with the same description is the same texture;
// SMBW rebuilds its transient render targets on new objects every frame and never Finalizes them, so building one per serial leaked about 13 MB of GPU memory per frame. Textures here are kept resident by the sharing table,
// and when the serial table replaces / deletes them it only drops its reference (defer_obj_unref), never removing them from the residency set. The sharing table does not evict (entries = distinct storage + description combinations).
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};
static CFMutableSetRef g_alias_set;   // 共用表裡的貼圖（不持有；查「是否共用」用） | Textures in the sharing table (non-owning; used to ask "is this shared")
static uint64_t g_alias_evicted;   // 池 Finalize 時淘汰的共用貼圖累計（內存報告用） | shared textures evicted on pool Finalize so far (for the memory report)
static int alias_has(id o) { return g_alias_set && o && CFSetContainsValue(g_alias_set, (__bridge const void *)o); }
// 內存池 Finalize 時淘汰共用表裡建在這個池上的貼圖（先收鍵再刪：刪除會回移表項）。共用表原先不淘汰：馬力歐的池常駐，條目有限；
// Engage 換場景就放池、在新池上重建，舊池的渲染目標等私有貼圖永遠留在表裡（標題演示 850 s 顯卡已分配多漲約 0.8 GB），池上直建的線性貼圖還拖著整塊池緩衝不放。
// 另外池對象地址被復用時，舊條目會把舊存儲上的貼圖錯給新池的同偏移同描述貼圖。淘汰的貼圖走延遲釋放；序號表若還掛著它，只是多一份引用
// On memory pool Finalize, evict the textures built on that pool from the sharing table (collect the keys first, then delete: deletion shifts entries back). The sharing table used not to evict: Mario's pools stay for good, so entries are bounded;
// Engage frees pools on scene changes and rebuilds on new ones, so the old pools' render targets and other private textures stayed in the table forever (about 0.8 GB more GPU memory over 850 s of the title demo), and linear textures built directly on a pool also kept the whole pool buffer alive.
// Also, when a pool object's address is reused, old entries would hand a texture on the old storage to the new pool's texture at the same offset and description. Evicted textures go through deferred release; if the serial table still holds one, that is just one more reference
static void treg_drop_pool_views(const void *pool);
void mtl_alias_drop_pool(const void *pool) {
    if (!pool) return;
    pthread_mutex_lock(&g_smu);
    uint32_t n = 0; alias_key *ks = NULL;
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)pool) continue;
        alias_key *nk = realloc(ks, (n + 1) * sizeof *ks); if (!nk) break; ks = nk; ks[n++] = k;
    }
    for (uint32_t j = 0; j < n; j++) {
        void *old = nvn_hm_del(&g_alias, &ks[j]);
        if (!old) continue;
        id o = CFBridgingRelease(old);
        if (g_alias_set) CFSetRemoveValue(g_alias_set, (__bridge const void *)o);
        defer_obj_release(o);
    }
    if (n) { g_ser_gen++; g_alias_evicted += n; }
    pthread_mutex_unlock(&g_smu);
    free(ks);
    treg_drop_pool_views(pool);   // 在 g_smu 外：貼圖號登記表的鎖 g_rmu 在 g_smu 之前取 | outside g_smu: the texture ID registry lock g_rmu is taken before g_smu
}
static __unsafe_unretained id serial_get(uint64_t k) { pthread_mutex_lock(&g_smu); __unsafe_unretained id o = hm_get_id(&g_by_serial, &k); pthread_mutex_unlock(&g_smu); return o; }
static void serial_put(uint64_t k, id o) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { if (alias_has(old)) defer_obj_unref(old); else defer_obj_release(old); } hm_put_id(&g_by_serial, &k, o); g_ser_gen++; pthread_mutex_unlock(&g_smu); }
static void serial_del(uint64_t k) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { if (alias_has(old)) defer_obj_unref(old); else defer_obj_release(old); } hm_del_id(&g_by_serial, &k); g_ser_gen++; pthread_mutex_unlock(&g_smu); }
// 預排布貼圖的宿主記錄：當前 MTLTexture、上傳戳、上次檢查時的全局寫代號
// Host record of a packaged texture: the current MTLTexture, the upload stamp, and the global write generation at the last check
@interface NvPkg : NSObject { @public id<MTLTexture> tex; uint64_t stamp, seen, hash; } @end   // hash：DM_NVN_DIRTYCHECK 時上傳內容的全量哈希 | hash: full hash of the uploaded content under DM_NVN_DIRTYCHECK
@implementation NvPkg
- (void)dealloc { res_remove(tex); }
@end
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_miss;
static uint64_t g_pfv_upgrades;
// DM_NVN_DIRTYCHECK=1（診斷，對照真值）：每次使用都全量哈希池內容，變了卻沒被髒頁跟蹤檢出 = 漏檢
// DM_NVN_DIRTYCHECK=1 (diagnostic, compared against ground truth): fully hash the pool content on every use; changed but not detected by dirty-page tracking = a missed detection
static int dirtycheck(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_DIRTYCHECK") != NULL; return v; }
static uint64_t full_hash(const uint8_t *m, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ m[i]) * 1099511628211ull; return h; }
// 顯卡不支持 BC 壓縮格式（A15 / A16 等）：BC 貼圖建成解壓後的格式，上傳 / 緩衝→貼圖拷貝時在 CPU 上解壓（nvn_bcdec.c）。
// 遊戲貼圖裡的 BC3 由老機型補充包離線換成了 ASTC（dm/tools/astc_pack.py），走到這裡的是剩下的少量 BC。測試開關 DM_TEST_NO_BC=1 在任何機器上強制走這條路
// The GPU does not support BC compressed formats (A15 / A16 etc.): BC textures are created in the decompressed format and decompressed on the CPU at upload / buffer→texture copy time (nvn_bcdec.c).
// BC3 among the game textures was converted to ASTC offline by the Legacy Device Pack (dm/tools/astc_pack.py); what reaches here is the small remainder of BC. The test switch DM_TEST_NO_BC=1 forces this path on any machine
static int no_bc(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_TEST_NO_BC"); v = (e && *e && strcmp(e, "0")) || ![g_dev supportsBCTextureCompression];
        if (v) MLOG("顯卡不支持 BC 壓縮格式%s：BC 貼圖建成解壓格式（BC1–5 在 GPU 上解壓，BC6H / BC7 / 3D 在 CPU 上）", e && *e && strcmp(e, "0") ? "（測試開關 DM_TEST_NO_BC）" : ""); }
    return v;
}
static int bc_unpack(uint32_t nvn_fmt, uint32_t *pf, int *bpp) { return no_bc() && nvn_bc_unpacked(nvn_fmt, pf, bpp); }
static MTLTextureDescriptor *tex_desc(const tex_t *t, int packaged) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf) { RED_ONCE(0, "貼圖格式 %#x Metal 無對應（首例）", t->d.format); return nil; }
    int unp = 0; { uint32_t upf; int ubpp; if (bc_unpack(t->d.format, &upf, &ubpp)) { pf = upf; unp = 1; } }
    // Metal 的上限：二維 16384、三維 2048、數組 2048 層；超出時 Metal 斷言中止進程，這裡先攔下記紅 | Metal's limits: 16384 in 2D, 2048 in 3D, 2048 array layers; beyond them Metal aborts the process on an assertion, so they are caught here with a red line
    if (t->d.width > 16384 || t->d.height > 16384 || t->d.depth > 2048 || t->d.levels > 15) { RED_ONCE(71, "貼圖尺寸超出 Metal 上限：%u×%u×%u 級 %u 格式 %#x 目標 %d（首例 ser=%llu）", t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, (unsigned long long)t->serial); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = (MTLPixelFormat)pf;
    d.width = t->d.width ? t->d.width : 1; d.height = t->d.height ? t->d.height : 1; d.depth = 1; d.arrayLength = 1;
    d.mipmapLevelCount = t->d.levels ? t->d.levels : 1;
    switch (t->d.target) {
        case 0: d.textureType = MTLTextureType1D; d.height = 1; break;
        case 2: d.textureType = MTLTextureType3D; d.depth = t->d.depth; break;
        case 3: d.textureType = MTLTextureType1DArray; d.arrayLength = t->d.height; d.height = 1; break;
        case 4: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 5: d.textureType = MTLTextureType2DMultisample; d.sampleCount = t->d.samples; d.mipmapLevelCount = 1; break;
        case 6: d.textureType = MTLTextureType2DMultisampleArray; d.sampleCount = t->d.samples; d.arrayLength = t->d.depth; d.mipmapLevelCount = 1; break;
        case 8: d.textureType = MTLTextureTypeCube; break;
        case 9: d.textureType = MTLTextureTypeCubeArray; d.arrayLength = t->d.depth; break;
        case 10: RED_ONCE(1, "緩衝貼圖（TARGET_BUFFER）未實現"); return nil;
        default: d.textureType = MTLTextureType2D; break;
    }
    // 級數夾到尺寸允許的上限：朋友收集給 128×256 的貼圖設 15 級，Metal 斷言中止（最多 9 級）。上傳按 Metal 的級數循環，多出的級（真機上都是 1×1）不建
    // Clamp the level count to what the size allows: Tomodachi Life gives a 128×256 texture 15 levels, and Metal aborts on an assertion (9 at most). Uploads loop over Metal's level count, so the extra levels (all 1×1 on hardware) are not created
    { NSUInteger m = MAX(d.width, d.height); if (d.textureType == MTLTextureType3D) m = MAX(m, d.depth); NSUInteger mx = 1; while (m >>= 1) mx++;
      if (d.mipmapLevelCount > mx) { RED_ONCE(58, "貼圖級數 %lu 超出 %lux%lux%lu 能有的 %lu 級，按 %lu 級建（首例 ser=%llu）", (unsigned long)d.mipmapLevelCount, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)mx, (unsigned long)mx, (unsigned long long)t->serial); d.mipmapLevelCount = mx; } }
    d.storageMode = MTLStorageModePrivate;   // 渲染目標與素材都放 GPU 私有存儲（可用無損壓縮） | Render targets and assets both go in GPU private storage (lossless compression available)
    d.usage = MTLTextureUsageShaderRead | (fmt_compressed(t->d.format) || packaged ? 0 : MTLTextureUsageRenderTarget)
            | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0)   // 圖像用途：著色器可寫（計算核生成 3D 調色表等） | Image usage: shader-writable (compute kernels generating the 3D color lookup table, etc.)
            | (unp ? MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView : 0);   // 無 BC：解壓格式貼圖由 GPU 解壓核寫（up_dec）/ 接 ASTC 源的整塊拷貝（copy_t2t_astc） | No BC: textures in the decompressed format are written by the GPU decompression kernel (up_dec) / receive whole-block copies from ASTC sources (copy_t2t_astc)
    return d;
}
// 就地升級：原貼圖沒開 PixelFormatView（缺省不開，保住蘋果顯卡無損壓縮）卻要建換格式視圖時，按原規格 + PixelFormatView 新建、逐 slice × level 拷過去並替換登記。
// 計數：選角 / 營地 / 野外 / 戰鬥 4 頁 17473 張貼圖換格式視圖 0 次，所以這裡是兜底，不靠預測哪些貼圖需要
// In-place upgrade: when the original texture was not created with PixelFormatView (off by default, to keep Apple GPU lossless compression) but a different-format view is needed, create a new one with the original spec + PixelFormatView, copy over every slice × level, and replace the registration.
// Count: across the 4 screens character select / camp / wilderness / combat, 17473 textures needed a different-format view 0 times, so this is a fallback and does not rely on predicting which textures need it
static id<MTLTexture> tex_upgrade_pfv(const tex_t *t, id<MTLTexture> base, MTLPixelFormat view_pf) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = base.textureType; d.pixelFormat = base.pixelFormat; d.width = base.width; d.height = base.height; d.depth = base.depth;   // 保留原格式，只加換格式視圖用途 | Keep the original format, only add the format-view usage
    d.arrayLength = base.arrayLength; d.mipmapLevelCount = base.mipmapLevelCount; d.sampleCount = base.sampleCount;
    d.storageMode = base.storageMode; d.cpuCacheMode = base.cpuCacheMode; d.hazardTrackingMode = base.hazardTrackingMode;
    d.resourceOptions = base.resourceOptions; d.usage = base.usage | MTLTextureUsagePixelFormatView;
    id<MTLTexture> upgraded = [g_dev newTextureWithDescriptor:d];
    if (!upgraded) { RED_ONCE(45, "貼圖升級失敗 ser=%llu 格式 %lu → %lu", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf); return base; }
    res_add(upgraded);
    id<MTL4ComputeCommandEncoder> b = blit();
    NSUInteger slices = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    for (NSUInteger l = 0; l < base.mipmapLevelCount; l++) {
        NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = base.textureType == MTLTextureType3D ? MAX(base.depth >> l, 1u) : 1;
        if (base.textureType == MTLTextureType3D)
            enc_b_copy_texture(b, base, 0, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, z), upgraded, 0, l, MTLOriginMake(0, 0, 0));
        else for (NSUInteger s = 0; s < slices; s++)
            enc_b_copy_texture(b, base, s, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), upgraded, s, l, MTLOriginMake(0, 0, 0));
    }
    id holder = serial_get(t->serial);
    if ([holder isKindOfClass:[NvPkg class]]) { id<MTLTexture> old = ((NvPkg *)holder)->tex; ((NvPkg *)holder)->tex = upgraded; defer_obj_release(old); g_ser_gen++; }
    else {
        serial_put(t->serial, upgraded);
        alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d;   // 共用表同步換成升級後的那張，後來同存儲的對象直接拿到它 | Switch the sharing table to the upgraded one too, so later objects on the same storage get it directly
        pthread_mutex_lock(&g_smu); if (hm_get_id(&g_alias, &ak) == base) { hm_put_id(&g_alias, &ak, upgraded); CFSetAddValue(g_alias_set, (__bridge const void *)upgraded); } pthread_mutex_unlock(&g_smu);
    }
    MLOG("貼圖升級為可換格式視圖 ser=%llu 基格式 %lu → 視圖格式 %lu（升級次數=%llu）", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, ++g_pfv_upgrades);
    return upgraded;
}
// 預排布貼圖上傳：先設只讀、再取戳、再讀池內存解排布進暫存緩衝，blit 進新的 private 貼圖。
// 批量：一次回放提交內的上傳共用一個上傳命令緩衝（計算編碼器解排布 + 統一 blit），在回放命令緩衝提交前衝刷；
// 同一隊列按提交順序執行 ⇒ 首次採樣前必已上傳完。提交之外的上傳當場衝刷。暫存區按 32 MB 私有塊切分，命令緩衝持有到完成。
// Packaged texture upload: first set read-only, then take the stamp, then read the pool memory and deswizzle into a staging buffer, and blit into a new private texture.
// Batching: uploads within one replay submission share one upload command buffer (compute encoder deswizzle + a unified blit), flushed before the replay command buffer is committed;
// the same queue executes in commit order ⇒ uploads are always finished before the first sample. Uploads outside a submission are flushed on the spot. The staging area is cut from 32 MB private blocks, held by the command buffer until completion.
static _Atomic uint64_t g_upload_ns;   // 提交線程上解排布 + 上傳編碼的累計耗時（計時埋點） | Cumulative time of deswizzle + upload encoding on the submit thread (timing instrumentation)
static _Atomic uint64_t g_upload_gpu_ns;
static id<MTL4CommandBuffer> g_up_cb;
static id<MTL4ComputeCommandEncoder> g_up_ce;
static _Atomic int g_up_alloc_busy[3];
static unsigned g_up_seq;
#define UP_BLOCK_SIZE (32u * 1024u * 1024u)
typedef struct {
    __unsafe_unretained id<MTLBuffer> src;
    size_t off, row, image;
    MTLSize size;
    __unsafe_unretained id<MTLTexture> dst;   // 由 g_up_texs 強引用到衝刷編碼完 | Strongly referenced by g_up_texs until the flush is encoded
    NSUInteger slice, level;
} up_blit;
static up_blit *g_up_blits; static int g_nup_blits, g_up_blitcap;
// 無 BC 時 BC1–5 在 GPU 上解壓（A16 日誌：CPU 解排布 + CPU 解壓單幀 300 ms 以上）：GPU 解排布把壓縮塊寫進暫存區，衝刷時過了屏障
// 由計算核 bcd 一線程一塊解進貼圖（目標是解壓格式的 2D 視圖，sRGB 底圖用 RGBA8Unorm 視圖寫原值）。BC6H / BC7 / 3D 貼圖量極少，仍走 CPU（nvn_bcdec.c）
// Without BC, BC1–5 are decompressed on the GPU (A16 logs: CPU deswizzle + CPU decompression took over 300 ms in a single frame): the GPU deswizzle writes the compressed blocks into the staging area, and at flush time, after the barrier,
// the compute kernel bcd decodes them into the texture one thread per block (the target is a 2D view in the decompressed format; an sRGB base texture is written with raw values through an RGBA8Unorm view). BC6H / BC7 / 3D textures are very few and still go through the CPU (nvn_bcdec.c)
typedef struct { __unsafe_unretained id<MTLBuffer> src, params; size_t off, poff; uint32_t bw, bh; __unsafe_unretained id<MTLTexture> view; } up_dec;   // view：批內由 g_up_texs、之後由 defer_obj_unref 持有到 GPU 跑完 | view: held by g_up_texs within the batch, and afterwards by defer_obj_unref until the GPU finishes
static up_dec *g_up_decs; static int g_nup_decs, g_up_deccap;
static const char k_bcd_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct P { uint off, pitch, w, h, fmt; };\n"   // fmt：1 BC1、2 BC2、3 BC3、4 BC4、5 BC4 有符號、6 BC5、7 BC5 有符號 | fmt: 1 BC1, 2 BC2, 3 BC3, 4 BC4, 5 BC4 signed, 6 BC5, 7 BC5 signed
"static float3 c565(uint c) { return float3((c >> 11) & 31, (c >> 5) & 63, c & 31) / float3(31, 63, 31); }\n"
"static void col(device const uchar *b, thread float4 *o, bool four) {\n"
"  uint c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8), ix = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24); float3 e0 = c565(c0), e1 = c565(c1); float4 p[4];\n"
"  p[0] = float4(e0, 1); p[1] = float4(e1, 1);\n"
"  if (four || c0 > c1) { p[2] = float4((2 * e0 + e1) / 3, 1); p[3] = float4((e0 + 2 * e1) / 3, 1); } else { p[2] = float4((e0 + e1) / 2, 1); p[3] = float4(0); }\n"
"  for (int i = 0; i < 16; i++) o[i] = p[(ix >> (2 * i)) & 3]; }\n"
"static void a8(device const uchar *b, thread float *o, bool sn) {\n"   // BC4 / BC3 alpha / BC5 的單通道塊 | Single-channel block of BC4 / BC3 alpha / BC5
"  float a0 = sn ? max(float(as_type<char>(b[0])), -127.0) / 127 : b[0] / 255.0, a1 = sn ? max(float(as_type<char>(b[1])), -127.0) / 127 : b[1] / 255.0;\n"
"  bool eight = sn ? as_type<char>(b[0]) > as_type<char>(b[1]) : b[0] > b[1]; float lo = sn ? -1 : 0; float p[8]; p[0] = a0; p[1] = a1;\n"
"  for (int k = 2; k < 8; k++) p[k] = eight ? ((8 - k) * a0 + (k - 1) * a1) / 7 : (k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : (k == 6 ? lo : 1));\n"
"  uint lo24 = b[2] | (b[3] << 8) | (b[4] << 16), hi24 = b[5] | (b[6] << 8) | (b[7] << 16);\n"
"  for (int i = 0; i < 16; i++) o[i] = p[i < 8 ? (lo24 >> (3 * i)) & 7 : (hi24 >> (3 * (i - 8))) & 7]; }\n"
"kernel void bcd(device const uchar *src [[buffer(0)]], device const P &p [[buffer(1)]], texture2d<float, access::write> dst [[texture(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  uint nbx = (p.w + 3) / 4, nby = (p.h + 3) / 4; if (g.x >= nbx || g.y >= nby) return;\n"
"  uint bb = (p.fmt == 1 || p.fmt == 4 || p.fmt == 5) ? 8 : 16; device const uchar *b = src + p.off + g.y * p.pitch + g.x * bb; float4 o[16]; float a[16], c[16];\n"
"  if (p.fmt == 1) col(b, o, false);\n"
"  else if (p.fmt == 2) { col(b + 8, o, true); for (int i = 0; i < 16; i++) o[i].a = ((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0; }\n"
"  else if (p.fmt == 3) { col(b + 8, o, true); a8(b, a, false); for (int i = 0; i < 16; i++) o[i].a = a[i]; }\n"
"  else if (p.fmt == 4 || p.fmt == 5) { a8(b, a, p.fmt == 5); for (int i = 0; i < 16; i++) o[i] = float4(a[i], 0, 0, 1); }\n"
"  else { a8(b, a, p.fmt == 7); a8(b + 8, c, p.fmt == 7); for (int i = 0; i < 16; i++) o[i] = float4(a[i], c[i], 0, 1); }\n"
"  for (int i = 0; i < 16; i++) { uint2 q = g * 4 + uint2(i & 3, i >> 2); if (q.x < p.w && q.y < p.h) dst.write(o[i], q); } }\n";
static int bcd_fmt(uint32_t f) {   // NVN 格式 → bcd 的 fmt；0 = GPU 不解（BC6H / BC7 / 非 BC） | NVN format → bcd fmt; 0 = not decoded on the GPU (BC6H / BC7 / non-BC)
    switch (f) { case 0x41: case 0x42: case 0x45: case 0x46: return 1; case 0x43: case 0x47: return 2; case 0x44: case 0x48: return 3;
                 case 0x49: return 4; case 0x4a: return 5; case 0x4b: return 6; case 0x4c: return 7; default: return 0; }
}
static id<MTLComputePipelineState> bcd_pso(void) {
    static id<MTLComputePipelineState> ps; static int failed;
    if (!ps && !failed) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_bcd_msl) options:nil error:&e]; id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"bcd"] : nil;
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!ps) { failed = 1; MLOG("【紅】無 BC：GPU 解壓管線建不出 %s（退回 CPU 解壓）", e.localizedDescription.UTF8String); } else MLOG("無 BC：BC1–5 改在 GPU 上解壓"); }
    return ps;
}
static NSMutableArray *g_up_texs;   // 批內目標貼圖：衝刷前同次提交裡貼圖可能被換掉 / 釋放 | Target textures in the batch: before the flush, textures may be replaced / released within the same submission
static id<MTLBuffer> g_up_block;
static size_t g_up_block_off, g_up_block_size;
static NSMutableArray *g_up_keep;


// 上傳命令緩衝被系統丟棄（GPU 出錯恢復時連帶丟，進崔凡克首幀那次每局丟一條）：這批貼圖的對象已建好、時間戳已更新，內容卻沒寫進去，
// 遊戲那邊池內存不再變就永遠不會重傳。記下這批目標貼圖，下次取用到其中哪張就當它髒了重傳一次。
// DM_TEST_UP_LOST=<第幾批>（測試用）：把那一批上傳當成被丟，驗證重傳路徑。
// The upload command buffer was discarded by the system (dropped along during GPU error recovery; the first frame on entering Travincal drops one every run): the objects of this batch of textures are built and the timestamps updated, but the content was never written in,
// and if the pool memory on the game side no longer changes it will never be re-uploaded. Record this batch of target textures, and the next time any of them is fetched treat it as dirty and re-upload once.
// DM_TEST_UP_LOST=<batch index> (for testing): treat that batch of uploads as dropped, to verify the re-upload path.
static NSHashTable *g_up_lost; static os_unfair_lock g_up_lost_lk = OS_UNFAIR_LOCK_INIT; static _Atomic int g_up_lost_n; static _Atomic uint64_t g_up_lost_redo;   // 弱引用表：記錄裡的貼圖若之後再沒被取用、被正常釋放，不由這張表拖著 | Weak-reference table: if a texture in the record is never fetched again and is released normally, this table does not keep it alive
static int up_lost_take(id tex) {   // 這張貼圖的上傳被丟過：從記錄裡取走並返回 1 | This texture's upload was dropped: take it out of the record and return 1
    if (!tex) return 0; os_unfair_lock_lock(&g_up_lost_lk); int hit = [g_up_lost containsObject:tex]; if (hit) { [g_up_lost removeObject:tex]; atomic_store(&g_up_lost_n, (int)g_up_lost.allObjects.count); atomic_fetch_add(&g_up_lost_redo, 1); } os_unfair_lock_unlock(&g_up_lost_lk); return hit;
}
static void up_diag(void);
static void up_flush(void) {
    up_owner_check("up_flush");
    if (!g_up_cb) return;
    if (g_up_ce) {
        m4_end(g_up_ce); g_up_ce = nil;
    }
    id<MTL4ComputeCommandEncoder> b = g_nup_blits || g_nup_decs ? g_up_ce = [g_up_cb computeCommandEncoder] : nil;
    if (b) [b barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    for (int i = 0; b && i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i];
        enc_b_copy_buffer_texture(b, x->src, x->off, x->row, x->image, x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    if (b && g_nup_decs) {   // 無 BC 的 GPU 解壓（見 up_dec）：參數塊裡的偏移相對暫存塊起點 | GPU decompression without BC (see up_dec): offsets in the parameter block are relative to the start of the staging block
        [b setArgumentTable:g_up_ct]; [b setComputePipelineState:bcd_pso()];
        for (int i = 0; i < g_nup_decs; i++) { up_dec *x = &g_up_decs[i];
            [g_up_ct setAddress:x->src.gpuAddress atIndex:0]; [g_up_ct setAddress:x->params.gpuAddress + x->poff atIndex:1]; [g_up_ct setTexture:x->view.gpuResourceID atIndex:0];
            [b dispatchThreadgroups:MTLSizeMake((x->bw + 7) / 8, (x->bh + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; }
    }
    if (b) { m4_end(b); g_up_ce = nil; }
    id<MTL4CommandBuffer> cb = g_up_cb;
    unsigned slot = (g_up_seq - 1) % 3;
    NSMutableArray *dsts = [NSMutableArray arrayWithCapacity:(NSUInteger)g_nup_blits]; for (int i = 0; i < g_nup_blits; i++) if (g_up_blits[i].dst) [dsts addObject:g_up_blits[i].dst];
    static int lost_at = -2; static uint64_t batch_n; if (lost_at == -2) { const char *e = getenv("DM_TEST_UP_LOST"); lost_at = e ? atoi(e) : -1; } int fake_lost = dsts.count && lost_at > 0 && (int)++batch_n == lost_at;
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    atomic_fetch_add(&g_up_pending, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) MLOG("【紅】上傳命令緩衝執行出錯：%s", f.error.description.UTF8String);
        if (f.error || fake_lost) {
            os_unfair_lock_lock(&g_up_lost_lk); if (!g_up_lost) g_up_lost = [NSHashTable weakObjectsHashTable]; for (id d in dsts) [g_up_lost addObject:d]; int n = (int)g_up_lost.allObjects.count; atomic_store(&g_up_lost_n, n); os_unfair_lock_unlock(&g_up_lost_lk);
            pthread_mutex_lock(&g_smu); g_ser_gen++; pthread_mutex_unlock(&g_smu);   // 讓各貼圖號的視圖緩存走一次慢路（緩存命中時不會經過 tex_mtl，看不到這份記錄） | Make the view cache of every texture ID take the slow path once (on a cache hit tex_mtl is not passed through and this record is not seen)
            MLOG("%s上傳命令緩衝被丟：這一批 %lu 張貼圖記為內容未寫，下次取用時重傳（待重傳共 %d 張）", fake_lost ? "DM_TEST_UP_LOST：" : "", (unsigned long)dsts.count, n);
        }
        double d = f.GPUEndTime - f.GPUStartTime;
        if (d > 0) atomic_fetch_add(&g_upload_gpu_ns, (uint64_t)(d * 1e9));
        atomic_store(&g_up_alloc_busy[slot], 0);
        atomic_fetch_sub(&g_up_pending, 1);
    }];
    res_commit();
    [cb endCommandBuffer];
    id<MTL4CommandBuffer> buffers[] = {cb};
    [g_q4 commit:buffers count:1 options:o];
    g_up_cb = nil; if (g_up_block) { defer_obj_release(g_up_block); g_up_block = nil; } g_up_block_off = g_up_block_size = 0;
    for (id obj in g_up_keep) defer_obj_release(obj);
    [g_up_keep removeAllObjects];
    up_diag();
    g_nup_blits = 0; g_nup_decs = 0;
    [g_up_texs removeAllObjects];
}

static void up_begin(void) {
    up_owner_check("up_begin");
    if (g_up_cb) return;
    unsigned slot = g_up_seq++ % 3;
    while (atomic_load(&g_up_alloc_busy[slot])) usleep(1000);
    atomic_store(&g_up_alloc_busy[slot], 1);
    [g_up_alloc4[slot] reset];
    g_up_cb = [g_dev newCommandBuffer];
    [g_up_cb beginCommandBufferWithAllocator:g_up_alloc4[slot]];
    g_up_ce = [g_up_cb computeCommandEncoder];
    [g_up_ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [g_up_ce setArgumentTable:g_up_ct];
    if (!g_up_texs) g_up_texs = [NSMutableArray array];
    if (!g_up_keep) g_up_keep = [NSMutableArray array];
}

// 診斷讀數（低頻，換塊次數有變化且距上一行 ≥ 1 秒才打，一局最多 200 行）：一批上傳裡暫存塊放不下、中途換了幾次塊；顯卡已分配、駐留集對象數、延遲釋放隊列長度。
// 換塊時舊塊若沒退役，它留在駐留集裡永遠放不掉，「顯卡已分配」會隨換塊次數一塊一塊往上漲
// Diagnostic readout (low frequency; printed only when the block switch count changed and ≥ 1 second since the last line, at most 200 lines per game): how many times an upload batch ran out of staging block and switched blocks midway; GPU allocated, residency set object count, deferred-release queue length.
// If the old block is not retired on a switch it stays in the residency set and can never be freed, and "GPU allocated" climbs one block at a time with the switch count
static uint64_t g_up_blocks_new, g_up_swaps, g_up_swap_bytes; static NSHashTable *g_up_old;   // g_up_old：換下的舊塊（弱引用），診斷行數它們還活著幾塊 | g_up_old: old blocks switched out (weak references); the diagnostic line counts how many are still alive
static void up_diag(void) {
    // 換塊次數變了打一行；之後過 5 秒再補一行（那一批提交早已跑完、延遲退役也排空了，「顯卡已分配」是落定後的數）
    // Print a line when the block switch count changes; then one more line 5 seconds later (that batch's commit is long done and deferred retirement drained, so "GPU allocated" is the settled number)
    static uint64_t said_swaps = ~0ull, last_ns; static int lines, follow; uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    int chg = g_up_swaps != said_swaps, fol = follow && now - last_ns >= 5000000000ull; if ((!chg && !fol) || lines >= 200) return;
    if (chg && last_ns && now - last_ns < 1000000000ull) return; last_ns = now; said_swaps = g_up_swaps; lines++; follow = chg;
    unsigned long alive = 0, inres = 0; uint64_t abytes = 0; NSArray *olds = g_up_old.allObjects;
    pthread_mutex_lock(&g_res_mu); unsigned long nres = (unsigned long)g_res_objects.allObjects.count; for (id<MTLBuffer> o in olds) { alive++; abytes += o.length; inres += [g_res_objects containsObject:o]; } pthread_mutex_unlock(&g_res_mu);
    pthread_mutex_lock(&g_deferred_mu); unsigned long ndef = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    MLOG("上傳暫存塊%s：累計新建 %llu 塊、其中一批內換塊 %llu 次（換下的舊塊共 %llu MB）；換下的舊塊此刻還活著 %lu 塊 %llu MB、仍在駐留集 %lu 塊；顯卡已分配 %llu MB、駐留集 %lu 個對象、延遲釋放隊列 %lu", fol && !chg ? "（落定後）" : "",
         (unsigned long long)g_up_blocks_new, (unsigned long long)g_up_swaps, (unsigned long long)(g_up_swap_bytes >> 20), alive, (unsigned long long)(abytes >> 20), inres, (unsigned long long)(g_dev.currentAllocatedSize >> 20), nres, ndef);
}
static id<MTLBuffer> up_alloc(size_t n, size_t *off) {
    size_t need = (n + 255u) & ~((size_t)255u);
    if (!g_up_block || g_up_block_off + need > g_up_block_size) {
        // 換塊：舊塊還被這一批已經登記的拷貝 / 解壓命令引用著，不能當場放；交給 g_up_keep，衝刷提交後和別的暫存一起延遲退役（退役時撤出駐留集）。
        // 原先這裡直接把 g_up_block 換成新塊：舊塊沒人退役、卻一直留在駐留集裡，一批上傳每多換一次塊就漏掉一塊（通常 32 MB）顯卡內存。
        // 舊塊與衝刷時退役的「當前塊」是兩個對象，不會重復退役
        // Block switch: the old block is still referenced by copy / decompress commands already recorded in this batch and cannot be freed on the spot; hand it to g_up_keep, to be retired later with the other staging after the flush commits (removed from the residency set on retire).
        // Previously g_up_block was simply replaced with the new block here: nobody retired the old block, yet it stayed in the residency set, so each extra block switch in an upload batch leaked one block (usually 32 MB) of GPU memory.
        // The old block and the "current block" retired at flush are two objects, so nothing is retired twice
        g_up_blocks_new++; if (g_up_block) { g_up_swaps++; g_up_swap_bytes += g_up_block_size; if (!g_up_old) g_up_old = [NSHashTable weakObjectsHashTable]; [g_up_old addObject:g_up_block]; static int leak = -1; if (leak < 0) leak = getenv("DM_TEST_UP_LEAK") != NULL;   // 測試開關 DM_TEST_UP_LEAK=1：照修前的做法不退役舊塊（對照用，量漏多少） | test switch DM_TEST_UP_LEAK=1: do not retire the old block, as before the fix (for comparison, to measure the leak)
            if (!leak) { if (!g_up_keep) g_up_keep = [NSMutableArray array]; [g_up_keep addObject:g_up_block]; } }
        g_up_block_size = n > UP_BLOCK_SIZE ? need : UP_BLOCK_SIZE;
        g_up_block = [g_dev newBufferWithLength:g_up_block_size options:MTLResourceStorageModePrivate];
        if (!g_up_block) MDIE("建預排布上傳暫存塊失敗 %#zx", g_up_block_size);
        res_add(g_up_block);
        g_up_block_off = 0;
    }
    *off = g_up_block_off;
    g_up_block_off += need;
    return g_up_block;
}

// 解排布參數塊：64 字節一格，從 64 KB 共享存儲塊裡順序切；塊滿換新，舊塊按柵欄延遲釋放
// Deswizzle parameter blocks: 64 bytes per cell, cut sequentially from a 64 KB shared-storage block; a full block is replaced by a new one, and the old block is released lazily by fence
static id<MTLBuffer> g_up_pblock; static size_t g_up_pblock_off;
static id<MTLBuffer> up_param_alloc(size_t *off) {
    if (!g_up_pblock || g_up_pblock_off + 64 > g_up_pblock.length) {
        if (g_up_pblock) defer_obj_release(g_up_pblock);
        g_up_pblock = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared];
        if (!g_up_pblock) MDIE("建解排布參數塊失敗");
        res_add(g_up_pblock); g_up_pblock_off = 0;
    }
    *off = g_up_pblock_off; g_up_pblock_off += 64;
    return g_up_pblock;
}
static void up_blit_add(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                        id<MTLTexture> dst, NSUInteger slice, NSUInteger level) {
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 256;
        g_up_blits = realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits);
    }
    g_up_blits[g_nup_blits++] = (up_blit){src, off, row, image, size, dst, slice, level};
}

static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp);
static id<MTLTexture> pkg_upload(const tex_t *t, uint64_t *stamp) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    id<MTLTexture> r = pkg_upload_(t, stamp);
    clock_gettime(CLOCK_MONOTONIC, &b); uint64_t d_ = (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + b.tv_nsec - a.tv_nsec);
    atomic_fetch_add(&g_upload_ns, d_); g_up_total_ns += d_;
    return r;
}
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_upload_ns, 0); }
uint64_t mtl_upload_gpu_ns_take(void) { return atomic_exchange(&g_upload_gpu_ns, 0); }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = g_bp_lib_n; *pso = g_bp_pso_n; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = g_bp_lib_ns; *pso_ns = g_bp_pso_ns; }   // 同步現建著色器庫 / 管線的累計耗時（長幀日誌按幀取差） | Cumulative time spent synchronously creating shader libraries / pipelines on the spot (the long-frame log takes the per-frame difference)
uint64_t mtl_upload_bytes(void) { return g_bp_up_bytes; }   // 貼圖上傳累計字節 | Cumulative texture upload bytes
static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp) {
    MTLTextureDescriptor *d = tex_desc(t, 1);
    if (!d) return nil;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(2, "建 MTLTexture 失敗 %ux%u 格式 %#x 目標 %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("新建貼圖（%s）#%d ser=%llu %ux%ux%u 格式 %#x 目標 %d 級 %u 存儲 %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    res_add(mt);   // 首傳與重傳都在這裡登記駐留（Metal 4 不自動駐留；漏登 = GPU 地址頁錯誤） | Both first upload and re-upload register residency here (Metal 4 does not make resources resident automatically; a missed registration = GPU address page fault)
    nvn_dirty_protect(t->mem, t->L.size);
    *stamp = nvn_dirty_now();
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned layers = (t->d.target == 4 || t->d.target == 9) ? t->d.depth : t->d.target == 8 ? 6 : t->d.target == 3 ? t->d.height : 1;
    if (t->d.target == 9) layers *= 6;
    uint32_t upf; int ubpp = 0, unp = bc_unpack(t->d.format, &upf, &ubpp);   // 無 BC：解排布後在 CPU 上解壓，暫存區按解壓後的大小 | No BC: decompress on the CPU after deswizzling, with the staging area sized for the decompressed data
    size_t total = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        total += unp ? (size_t)w * ubpp * h * z * layers : (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z * layers;
    }
    g_bp_up_bytes += total;
    void *mb; uint64_t mo;
    int gdec = unp && t->d.target != 2 && bcd_fmt(t->d.format) && bcd_pso();   // 無 BC 的 BC1–5：GPU 解排布 + GPU 解壓 | BC1–5 without BC: GPU deswizzle + GPU decompression
    size_t ctotal = 0;   // 壓縮塊的暫存大小（GPU 解壓時暫存區放的是壓縮塊） | Staging size of the compressed blocks (with GPU decompression the staging area holds compressed blocks)
    if (gdec) for (unsigned l = 0; l < d.mipmapLevelCount; l++) { uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u); ctotal += (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * layers; }
    int use_gpu = (!unp || gdec) && g_nvn_deswz_gpu && mtl_addr((uint64_t)t->mem, t->L.size, &mb, &mo);
    if (g_nvn_deswz_gpu && !use_gpu && !unp) RED_ONCE(40, "預排布貼圖存儲不在內存池裡，回退 CPU");
    if (use_gpu) {
        // GPU 執行時讀內存池，比 CPU 上傳時刻快照更接近 Switch 真機；髒頁戳仍在編碼前取，之後寫入仍會觸發重傳。
        // The GPU reads the memory pool at execution time, which is closer to a real Switch than a snapshot at CPU upload time; the dirty-page stamp is still taken before encoding, and later writes still trigger a re-upload.
        up_begin();
        size_t stg_off;
        id<MTLBuffer> stg = up_alloc(gdec ? ctotal : total ? total : 16, &stg_off);
        size_t off = 0; int enc_ok = g_up_ce != nil, nb0 = g_nup_blits, nd0 = g_nup_decs;
        for (unsigned l = 0; enc_ok && l < d.mipmapLevelCount; l++) {
            uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            for (unsigned y = 0; y < layers; y++) {
                size_t poff; id<MTLBuffer> params = up_param_alloc(&poff);   // Metal 4 無 setBytes：每次解排布的參數塊從共享塊裡切（不逐次新建 GPU 緩衝） | Metal 4 has no setBytes: the parameter block of each deswizzle is cut from a shared block (no new GPU buffer each time)
                if (nvn_deswz_gpu_encode_m4(g_up_ce, g_up_ct, params, poff, (__bridge id<MTLBuffer>)mb, mo, &t->d, l, y, stg, stg_off + off, pitch)) { enc_ok = 0; break; }
                if (gdec) {   // 解壓任務：衝刷時過了屏障再派發（見 up_flush） | Decompression job: dispatched at flush time after the barrier (see up_flush)
                    MTLPixelFormat vf = mt.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : mt.pixelFormat;
                    id<MTLTexture> v = [mt newTextureViewWithPixelFormat:vf textureType:MTLTextureType2D levels:NSMakeRange(l, 1) slices:NSMakeRange(y, 1)];
                    size_t qoff; id<MTLBuffer> q = up_param_alloc(&qoff);
                    if (!v || !q) { enc_ok = 0; break; }
                    uint32_t *pp = (uint32_t *)((uint8_t *)q.contents + qoff); pp[0] = (uint32_t)(stg_off + off); pp[1] = (uint32_t)pitch; pp[2] = w; pp[3] = h; pp[4] = (uint32_t)bcd_fmt(t->d.format);
                    if (g_nup_decs == g_up_deccap) { g_up_deccap = g_up_deccap ? g_up_deccap * 2 : 256; g_up_decs = realloc(g_up_decs, g_up_deccap * sizeof *g_up_decs); }
                    g_up_decs[g_nup_decs++] = (up_dec){stg, q, 0, qoff, (w + 3) / 4, (h + 3) / 4, v}; [g_up_texs addObject:v]; defer_obj_unref(v);   // 視圖：本次提交跑完再放，不撤駐留（駐留的是底圖） | View: released after this submission finishes running, residency not revoked (what is resident is the base texture)
                } else
                up_blit_add(stg, stg_off + off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
                off += pitch * rows * z;
            }
        }
        if (enc_ok) {
            [g_up_texs addObject:mt];
            if (!g_cb) up_flush();
            return mt;
        }
        g_nup_blits = nb0; g_nup_decs = nd0;   // 撤回本張已記的拷貝 / 解壓：已編的解排布只寫暫存區，無害；本張改走下面 CPU 路徑 | Roll back the copies / decompressions already recorded for this texture: the already-encoded deswizzle only writes the staging area and is harmless; this texture takes the CPU path below instead
        if (!g_cb) up_flush();
        RED_ONCE(41, "預排布貼圖 GPU 解排布不支持格式 %#x 目標 %d，回退 CPU", t->d.format, t->d.target);
    }

    up_begin();
    id<MTLBuffer> stg = [g_dev newBufferWithLength:total ? total : 16 options:MTLResourceStorageModeShared];
    res_add(stg); [g_up_keep addObject:stg];
    size_t off = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < layers; y++) {
            if (unp) {   // 無 BC：先解排布到臨時區（壓縮塊），再逐層解壓進暫存區 | No BC: first deswizzle into a temporary area (compressed blocks), then decompress layer by layer into the staging area
                uint8_t *cb = malloc(pitch * rows * z); size_t up = (size_t)w * ubpp;
                if (!cb || nvn_deswizzle(&t->d, l, y, t->mem, cb, pitch)) { free(cb); RED_ONCE(9, "解排布失敗 格式 %#x 目標 %d", t->d.format, t->d.target); break; }
                for (uint32_t zi = 0; zi < z; zi++) nvn_bc_decode(t->d.format, cb + zi * pitch * rows, pitch, w, h, (uint8_t *)stg.contents + off + zi * up * h, up);
                free(cb);
                up_blit_add(stg, off, up, up * h, MTLSizeMake(w, h, z), mt, y, l);
                off += up * h * z;
                continue;
            }
            if (nvn_deswizzle(&t->d, l, y, t->mem, (uint8_t *)stg.contents + off, pitch)) { RED_ONCE(9, "解排布失敗 格式 %#x 目標 %d", t->d.format, t->d.target); break; }
            up_blit_add(stg, off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
            off += pitch * rows * z;
        }
    }
    up_flush();
    return mt;
}
// 新建的非預排布貼圖清零：Metal 新貼圖的內容未定義，Switch 上新映射的內存池讀出 0。遊戲有依賴：SMBW 噴泉水面的 512×1 RGBA16F 波形貼圖
// 每幀「讀上一幀 → 畫進渲染目標 → 拷回」，從沒清過；未定義的初值在反饋裡一直蕩，水面頂點被推到屏幕頂（噴泉上方的竪條）。
// 從一塊共享全零緩衝按級 / 層拷進去，隨上傳批執行（排在本次回放之前）。多重採樣、深度 / 模板、塊壓縮格式不清（遊戲用前必清，也拷不進去）。
// Zero newly created non-packaged textures: a new Metal texture's content is undefined, while a freshly mapped memory pool on the Switch reads as 0. Games depend on it: SMBW's fountain water surface uses a 512×1 RGBA16F wave texture
// that every frame is "read last frame → render into a render target → copy back" and is never cleared; the undefined initial value keeps swinging through the feedback and pushes the water vertices to the top of the screen (the tall panels above the fountain).
// Copied level by level / slice by slice from one shared all-zero buffer, executed with the upload batch (ordered before this replay). Multisample, depth / stencil and block-compressed formats are not cleared (games always clear them before use, and they cannot be copied into anyway).
static size_t mtl_pf_bytes(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 10: case 11: case 12: case 13: case 14: return 1;                                   // R8
        case 20: case 22: case 23: case 24: case 25: case 30: case 31: case 32: case 33: case 34: return 2;   // R16 / RG8
        case 40: case 41: case 42: case 43: return 2;                                            // 16 位壓縮色 | packed 16-bit colour
        case 53: case 54: case 55: case 60: case 62: case 63: case 64: case 65:
        case 70: case 71: case 72: case 73: case 74: case 80: case 81:
        case 90: case 91: case 92: case 93: case 94: return 4;                                   // R32 / RG16 / RGBA8 / BGRA8 / 10-10-10-2 / 11-11-10 / 9-9-9-5
        case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115: return 8;   // RG32 / RGBA16
        case 123: case 124: case 125: return 16;                                                 // RGBA32
    }
    return 0;
}
// 共用的全零緩衝（新貼圖清零、剪裁清零用）：不夠大就換一塊更大的，舊塊等在途提交跑完再放
// Shared all-zero buffer (for zeroing new textures and scissored zero clears): replaced by a bigger one when too small; the old one is released after in-flight submissions finish
static id<MTLBuffer> zero_buf(size_t need) {
    static id<MTLBuffer> zb; static size_t zn;
    if (need > zn) {
        size_t n = MAX(need, (size_t)4 << 20);
        id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
        if (!b) return nil;
        memset(b.contents, 0, n); res_add(b);
        if (zb) defer_obj_release(zb);
        zb = b; zn = n;
    }
    return zb;
}
static uint64_t g_tex_zeroed;
static void tex_zero_fill(id<MTLTexture> mt) {
    static int warned;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread()) return;   // 上傳批只屬於提交線程 | the upload batch belongs to the submit thread only
    MTLTextureType tt = mt.textureType;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray || tt == MTLTextureTypeTextureBuffer) return;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat);
    if (!bpp) { if (!warned++) MLOG("新建貼圖清零：像素格式 %lu 不清（深度 / 模板 / 壓縮等）", (unsigned long)mt.pixelFormat); return; }
    NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
    size_t need = (size_t)mt.width * bpp * mt.height * mt.depth;   // 第 0 級一層最大 | level 0, one slice is the largest
    if (need > (64u << 20)) { static int big; if (!big++) MLOG("新建貼圖清零：%lux%lux%lu 超過 64 MB，不清", (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)mt.depth); return; }
    id<MTLBuffer> zb = zero_buf(need); if (!zb) return;
    up_begin();
    for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
        NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), z = tt == MTLTextureType3D ? MAX(mt.depth >> l, 1u) : 1;
        for (NSUInteger y = 0; y < slices; y++) up_blit_add(zb, 0, w * bpp, w * bpp * h, MTLSizeMake(w, h, z), mt, y, l);
    }
    [g_up_texs addObject:mt];
    if (!g_cb) up_flush();
    if (!g_tex_zeroed++) MLOG("新建貼圖清零：開（Metal 新貼圖內容未定義，Switch 新內存讀 0）");
}

static int clear_rect_zero(int i);
// 線性貼圖（LINEAR）放在 CPU 可見池裡：遊戲用 CPU 直接往池內存寫（SMBW 的字形緩存：2048×8192 R8，IMAGE|LINEAR，池 CPU_UNCACHED|GPU_CACHED，
// 字體引擎在 CPU 上把字形畫進去）。GPU 私有貼圖看不到這些寫入 → 菜單 / 對話框文字全空。統一內存下直接在池緩衝上建線性貼圖（零拷貝），GPU 執行時讀到當前內容，同 NVN 語義。
// Linear textures (LINEAR) in a CPU-visible pool: the game writes the pool memory directly from the CPU (SMBW's glyph cache: 2048×8192 R8, IMAGE|LINEAR, pool CPU_UNCACHED|GPU_CACHED,
// the font engine rasterizes glyphs into it on the CPU). A GPU-private texture never sees those writes → all menu / dialog text is blank. On unified memory, create a linear texture directly on the pool buffer (zero copy): the GPU reads the current content at execution time, same as NVN.
static id<MTLTexture> tex_linear(const tex_t *t) {
    // LINEAR_RENDER_TARGET 也是線性排布（SMBW 的天空色帶：4 張 64×1 RGBA16F，CPU 每幀寫）| LINEAR_RENDER_TARGET is a linear layout too (SMBW's sky colour ramps: four 64×1 RGBA16F, written by the CPU every frame)
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return nil;
    if (t->d.target != NVN_TEXTURE_TARGET_2D || t->d.levels > 1 || t->d.samples > 1) return nil;
    uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb || bw != 1 || bh != 1) return nil;
    size_t row = t->d.stride ? t->d.stride : (size_t)t->d.width * bpb;
    void *mb; uint64_t mo;
    if (!mtl_addr((uint64_t)t->mem, (uint64_t)row * (t->d.height ? t->d.height : 1), &mb, &mo)) { RED_ONCE(64, "線性貼圖的存儲不在內存池裡（%ux%u 格式 %#x）", t->d.width, t->d.height, t->d.format); return nil; }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
    NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
    if (mo % al || row % al) { RED_ONCE(65, "線性貼圖偏移 %#llx / 行距 %zu 不滿足 %lu 字節對齊，按私有貼圖建（CPU 寫入不可見）", (unsigned long long)mo, row, (unsigned long)al); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width ? t->d.width : 1 height:t->d.height ? t->d.height : 1 mipmapped:NO];
    d.resourceOptions = b.resourceOptions;
    d.usage = MTLTextureUsageShaderRead | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0) | (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET ? MTLTextureUsageRenderTarget : 0);
    return [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
}
// CPU 寫過的塊線性貼圖（WriteTexels）：遊戲沒給預排布、每幀用 CPU 改內容（Engage 的影片幀：Y 平面 R8 + UV 平面 RG8，每幀 WriteTexels）。
// 原先只有預排布貼圖從客體內存上傳，這類貼圖建成純 GPU 貼圖、內容永遠是 0。記下序號，解析時走預排布路徑（上傳 + 髒頁重傳）
// CPU-written block-linear textures (WriteTexels): the game gave no packaged layout and changes the content from the CPU each frame (Engage's movie frames: a Y plane R8 + a UV plane RG8, WriteTexels every frame).
// Only packaged textures used to be uploaded from guest memory, so such a texture became a GPU-only texture whose content stayed 0. Their serials are recorded and they resolve through the packaged path (upload + re-upload on dirty pages)
static pthread_mutex_t g_cpu_tex_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableSetRef g_cpu_tex;
void mtl_tex_cpu_written(const tex_t *t) {
    uint64_t serial = t->serial;
    if (!serial) return;
    pthread_mutex_lock(&g_cpu_tex_mu);
    if (!g_cpu_tex) g_cpu_tex = CFSetCreateMutable(NULL, 0, NULL);
    if (!CFSetContainsValue(g_cpu_tex, (const void *)(uintptr_t)serial)) {
        CFSetAddValue(g_cpu_tex, (const void *)(uintptr_t)serial);
        static int n; if (n++ < 16) MLOG("CPU 寫貼圖 ser=%llu %ux%ux%u 格式 %#x 目標 %d 級 %u：按預排布貼圖上傳 / 重傳", (unsigned long long)serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels);
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
}
static int tex_cpu_written(uint64_t serial) {
    pthread_mutex_lock(&g_cpu_tex_mu); int r = g_cpu_tex && CFSetContainsValue(g_cpu_tex, (const void *)(uintptr_t)serial); pthread_mutex_unlock(&g_cpu_tex_mu);
    return r;
}
static id<MTLTexture> tex_mtl_(const tex_t *ct);
static id<MTLTexture> tex_mtl(const tex_t *ct) { AP_B; id<MTLTexture> r = tex_mtl_(ct); AP_E(4); return r; }
static uint64_t g_texinfo_ser[8]; static int g_texinfo_n;
static id<MTLTexture> tex_mtl_(const tex_t *ct) {
    tex_t *t = (tex_t *)ct;
    if (!t || !t->serial) return nil;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) { RED_ONCE(70, "貼圖對象已失效（magic %#x，首例 ser=%llu）：不建 MTLTexture", t->magic, (unsigned long long)t->serial); return nil; }
    int packaged = (t->has_layout || tex_cpu_written(t->serial)) && t->mem;
    {   // 診斷 DM_NVN_TEXINFO=寬x高：該尺寸的貼圖首次解析時記下存儲與描述（判兩張貼圖是否同一塊存儲、為何沒合用） | Diagnostic DM_NVN_TEXINFO=WxH: log storage and description of textures of that size on first resolution (to tell whether two textures share storage and why they were not merged)
        static int ti = -1; static uint32_t tw, th; static uint64_t last[64]; static int nl;
        if (ti > 0 && t->d.width == tw && t->d.height == th && g_texinfo_n < 8) { int dup = 0; for (int i = 0; i < g_texinfo_n; i++) dup |= g_texinfo_ser[i] == t->serial; if (!dup && !(t->d.flags & 8)) g_texinfo_ser[g_texinfo_n++] = t->serial; }   // 非渲染目標（無 COMPRESSIBLE）的記下，供 DM_NVN_TEXRAW=info | remember the non-render-target ones (no COMPRESSIBLE) for DM_NVN_TEXRAW=info
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && t->d.width == tw && t->d.height == th) {
            int seen = 0; for (int i = 0; i < nl; i++) if (last[i] == t->serial) seen = 1;
            static uint64_t n; if (!seen && (n++ < 40 || n % 600 == 0)) { if (nl < 64) last[nl++] = t->serial; else last[n % 64] = t->serial;
                MLOG("TEXINFO ser=%llu 預排布=%d has_layout=%d mem=%p pool=%p(%#x) off=%#llx 目標=%d 格式=%#x 標誌=%#x 採樣=%d %ux%ux%u 級=%u 跨距=%u 塊高=%d",
                     (unsigned long long)t->serial, packaged, t->has_layout, t->mem, t->pool, t->pool ? t->pool->flags : 0, (unsigned long long)t->off, t->d.target, t->d.format, t->d.flags, t->d.samples,
                     t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.stride, t->d.pkg_block_h); }
        }
    }
    id have = serial_get(t->serial);
    if (packaged) {
        NvPkg *pk = [have isKindOfClass:[NvPkg class]] ? have : nil;   // 先按純 GPU 貼圖解析過、後來才被 CPU 寫的：丟掉舊的，按預排布重建 | resolved as GPU-only before the CPU wrote it: dropped and rebuilt as packaged
        if (pk) {   // 已上傳：自上次檢查後有過客體寫才查覆蓋頁 | Already uploaded: check the covered pages only if there has been a guest write since the last check
            uint64_t now = nvn_dirty_now();
            int tracked = nvn_dirty_tracked(t->mem, t->L.size);
            int clean = tracked && (now == pk->seen || !nvn_dirty_since(t->mem, t->L.size, pk->stamp));
            pk->seen = now;
            if (clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) { clean = 0; uint64_t k = atomic_load(&g_up_lost_redo); if (k <= 3 || !atomic_load(&g_up_lost_n)) MLOG("上傳被丟的貼圖 ser=%llu 重傳（累計 %llu 張，記錄裡還剩 %d 張）", (unsigned long long)t->serial, (unsigned long long)k, atomic_load(&g_up_lost_n)); }
            if (clean) {
                if (dirtycheck() && full_hash(t->mem, t->L.size) != pk->hash && !g_pkg_miss++)
                    MLOG("【紅】髒頁漏檢：貼圖 ser=%llu 池內容已變但未檢出", t->serial);
                return pk->tex;
            }
            uint64_t st; id<MTLTexture> nt = pkg_upload(t, &st);   // 池內容變了（流式素材寫進同一存儲）：換新貼圖，在途繪製仍讀舊的 | Pool content changed (streamed assets written into the same storage): switch to a new texture; in-flight draws still read the old one
            if (nt) { id<MTLTexture> old = pk->tex; pk->tex = nt; defer_obj_release(old); res_add(nt); g_ser_gen++; pk->stamp = st; pk->seen = nvn_dirty_now(); g_pkg_reuploads++; if (dirtycheck()) pk->hash = full_hash(t->mem, t->L.size); }
            return pk->tex;
        }
        pk = [NvPkg new];
        pk->tex = pkg_upload(t, &pk->stamp);
        if (!pk->tex) return nil;
        pk->seen = nvn_dirty_now(); g_pkg_uploads++; if (dirtycheck()) pk->hash = full_hash(t->mem, t->L.size);
        serial_put(t->serial, pk);
        return pk->tex;
    }
    if (have) return have;
    if (t->d.target == 10) {   // 緩衝貼圖（TARGET_BUFFER）：直接在內存池緩衝上建貼圖緩衝視圖（零拷貝，GPU 執行時讀池裡的當前內容，同 NVN 語義） | Buffer texture (TARGET_BUFFER): create the texture-buffer view directly on the memory pool buffer (zero copy; the GPU reads the pool's current content at execution time, same as NVN semantics)
        uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        void *mb; uint64_t mo;
        if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb) { RED_ONCE(36, "緩衝貼圖格式 %#x 未映射", t->d.format); return nil; }
        if (!t->mem || !mtl_addr((uint64_t)t->mem, (uint64_t)t->d.width * bpb, &mb, &mo)) { RED_ONCE(37, "緩衝貼圖的存儲不在內存池裡"); return nil; }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
        NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
        if (mo % al) { RED_ONCE(38, "緩衝貼圖偏移 %#llx 不滿足 %lu 字節對齊", (unsigned long long)mo, (unsigned long)al); return nil; }
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width resourceOptions:b.resourceOptions
                                                                                         usage:MTLTextureUsageShaderRead | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0)];
        size_t row = ((size_t)t->d.width * bpb + al - 1) / al * al;
        id<MTLTexture> mt = [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
        if (!mt) { RED_ONCE(39, "緩衝貼圖建不出：%u 元素 格式 %#x", t->d.width, t->d.format); return nil; }
        res_add(mt);
        serial_put(t->serial, mt);
        return mt;
    }
    alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d;
    pthread_mutex_lock(&g_smu); __unsafe_unretained id shared = hm_get_id(&g_alias, &ak); pthread_mutex_unlock(&g_smu);
    if (shared) { serial_put(t->serial, shared); return shared; }
    id<MTLTexture> mt = tex_linear(t);
    if (!mt) {
        MTLTextureDescriptor *d = tex_desc(t, 0);
        if (!d) return nil;
        mt = [g_dev newTextureWithDescriptor:d];
        if (!mt) { RED_ONCE(2, "建 MTLTexture 失敗 %ux%u 格式 %#x 目標 %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
        res_add(mt); tex_zero_fill(mt);   // 先登記駐留再清零（拷貝目標也要駐留） | register residency before zeroing (the copy destination must be resident too)
    }
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("新建貼圖（%s）#%d ser=%llu %ux%ux%u 格式 %#x 目標 %d 級 %u 存儲 %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    if (getenv("DM_NVN_TEXCPU") && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) MLOG("CPU 可見池上的非預排布貼圖 ser=%llu %ux%ux%u 格式 %#x 目標 %d 級 %u 貼圖標誌 %#x 池標誌 %#x 存儲 %p", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->d.flags, t->pool->flags, (void *)t->mem);
    res_add(mt);
    pthread_mutex_lock(&g_smu);
    if (!g_alias_set) g_alias_set = CFSetCreateMutable(NULL, 0, NULL);
    hm_put_id(&g_alias, &ak, mt); CFSetAddValue(g_alias_set, (__bridge const void *)mt);
    pthread_mutex_unlock(&g_smu);
    serial_put(t->serial, mt);
    return mt;
}
// 內存讀數行（常開、低頻、封頂）：常駐內存、顯卡已分配、延遲釋放隊列、駐留集對象數、在冊內存池數 / 峰值 / 跟蹤表容量、貼圖重傳累計。
// 用途：定「沒有日誌的閃退」——系統按內存殺進程前不給任何通知，只有事先留下的讀數能說明是哪個數一路往上漲。
// 每 300 幀由幀鈎子調一次；常駐或顯卡已分配比上一行變了 ≥ 64 MB 才打，否則每 3000 幀打一行；一局封頂 400 行。全是整數（幀鈎子在客體線程的小棧上，不用浮點格式）
// Memory readout line (always on, low frequency, capped): resident memory, GPU allocated, deferred-release queue, residency set object count, registered memory pools / peak / tracking table capacity, cumulative texture re-uploads.
// Purpose: pin down "crashes with no log". The system gives no notice before killing a process for memory, so only readouts left beforehand can show which number kept climbing.
// Called by the frame hook every 300 frames; printed only when resident or GPU allocated changed by ≥ 64 MB since the last line, otherwise one line every 3000 frames; capped at 400 lines per game. All integers (the frame hook runs on the guest thread's small stack, so no floating-point formatting)
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) {
    static unsigned long long last_rss, last_gpu, last_frame; static int lines; unsigned long long gpu = (unsigned long long)(g_dev.currentAllocatedSize >> 20);
    unsigned long long dr = rss_mb > last_rss ? rss_mb - last_rss : last_rss - rss_mb, dg = gpu > last_gpu ? gpu - last_gpu : last_gpu - gpu;
    if (lines >= 400 || (lines && dr < 64 && dg < 64 && frame - last_frame < 3000)) return; lines++; last_rss = rss_mb; last_gpu = gpu; last_frame = frame;
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    pthread_mutex_lock(&g_res_mu); unsigned long nr = (unsigned long)g_res_objects.count; pthread_mutex_unlock(&g_res_mu);
    int live = 0, peak = 0, cap = 0; nvn_dirty_stat(&live, &peak, &cap);
    pthread_mutex_lock(&g_smu); unsigned ns = g_by_serial.n, ashared = g_alias.n; pthread_mutex_unlock(&g_smu);
    unsigned long dead_n = 0; unsigned long long dead_mb = 0;
    // 弱引用表取出的對象是「保留 + 自動釋放」：幀鈎子在客體線程上，沒有會排空的自動釋放池，不包一層就把數到的緩衝永遠釘住（讀數自己造出洩漏）
    // Objects read from a weak table come back retained + autoreleased: the frame hook runs on a guest thread with no autorelease pool that drains, so without this wrapper every buffer counted would be pinned for good (the readout creating the leak itself)
    int pres = 0; @autoreleasepool { pthread_mutex_lock(&g_mu); pres = g_pool_res_n; for (id<MTLBuffer> b in g_dead_pools) { dead_n++; dead_mb += b.length; } pthread_mutex_unlock(&g_mu); } dead_mb >>= 20;   // 按序號掛著的宿主對象（貼圖 / 預排布貼圖 / 程序）：顯卡已分配一路漲而它也一路漲，就是貼圖沒放 | host objects held by serial (textures / packaged textures / programs): if it climbs along with GPU allocated, textures are not being released
    // os_proc_available_memory：本進程離被系統按內存終止還差多少，只有 iOS 有（取自 d2r-mobile 704147218）；Mac 寫「無」。手機上沒有日誌的閃退多半是這個數到了 0
    // os_proc_available_memory: how far this process is from being terminated by the system for memory, iOS only (from d2r-mobile 704147218); the Mac writes 「無」(none). On a phone, a crash with no log is most likely this number reaching 0
    char avail[32] = "無";
#if TARGET_OS_IPHONE
    snprintf(avail, sizeof avail, "%llu MB", (unsigned long long)(os_proc_available_memory() >> 20));
#endif
    MLOG("內存 至幀 %llu：常駐 %llu MB，顯卡已分配 %llu MB，延遲釋放隊列 %lu，駐留集 %lu 個，序號對象 %u 個，內存池 %d 個（其中已駐留 %d；峰值 %d / 跟蹤表容量 %d；已釋放仍活 %lu 個 %llu MB），共用貼圖 %u 張（隨池淘汰累計 %llu），貼圖重傳累計 %llu 次，每幀競技場累計新建 %llu 塊；離系統內存上限還有 %s", frame, rss_mb, gpu, nd, nr, ns, live, pres, peak, cap, dead_n, dead_mb, ashared, (unsigned long long)g_alias_evicted, (unsigned long long)g_pkg_reuploads, (unsigned long long)g_arena_made, avail);
    if (g_allocprof > 0) { char ab[256]; int o = 0; for (int i = 0; i < g_alloc_n && o < 200; i++) o += snprintf(ab + o, sizeof ab - o, " #%d %llu MB", i, (unsigned long long)(g_alloc4[i].allocatedSize >> 20));
        for (int i = 0; i < 3 && o < 240; i++) o += snprintf(ab + o, sizeof ab - o, " 傳%d %llu 出%d %llu", i, (unsigned long long)(g_up_alloc4[i].allocatedSize >> 20), i, (unsigned long long)(g_present_alloc4[i].allocatedSize >> 20));
        MLOG("命令分配器佔用：%s", ab);
        MLOG("顯卡分配增長按函數：結束通道 %llu MB，開通道 %llu MB，取管線 %llu MB，參數緩衝 %llu MB，取貼圖 %llu MB", g_ap_fn[0] >> 20, g_ap_fn[1] >> 20, g_ap_fn[2] >> 20, g_ap_fn[3] >> 20, g_ap_fn[4] >> 20); }
    if (g_allocprof > 0) { int top[4] = {-1, -1, -1, -1}; for (int o = 0; o < 64; o++) for (int k = 0; k < 4; k++) if (top[k] < 0 || g_ap_bytes[o] > g_ap_bytes[top[k]]) { for (int j = 3; j > k; j--) top[j] = top[j - 1]; top[k] = o; break; }
        MLOG("顯卡分配增長按操作：op%d %llu MB，op%d %llu MB，op%d %llu MB，op%d %llu MB", top[0], g_ap_bytes[top[0]] >> 20, top[1], g_ap_bytes[top[1]] >> 20, top[2], g_ap_bytes[top[2]] >> 20, top[3], g_ap_bytes[top[3]] >> 20); }
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
uint64_t mtl_pkg_miss(void) { return g_pkg_miss; }
void mtl_tex_release(tex_t *t) { if (t->serial) serial_del(t->serial); }

// ---------- 渲染通道：SetRenderTargets 開新通道；通道裡第一條命令之前的清除並進 load 動作 ----------
// ---------- Render passes: SetRenderTargets opens a new pass; clears before the first command in the pass are folded into the load action ----------
static struct {
    p_set_rt rt; int has_rt;
    uint8_t clear_c[8]; MTLClearColor cc[8];
    uint8_t clear_d, clear_s; double cd; uint32_t cs;
    p_scissor sc; int has_sc;
} R;

// ---------- 零源跳過 ----------
// 著色器事實表（靜態證明過的鍵）：「它採樣的貼圖全 0 且系數有限 ⇒ 輸出全 0」。系數 = 片元階段常量緩衝寄存器 ubo_slot 的第 vec 個向量第 comp 分量。
//   4a819818b8e489c5 ps_outline_blur_p：輸出 = (同一張貼圖沿行 / 列若干次採樣之和) × fp_c7._m0[1].x（fp_c7 = cbuf reg 4），對採樣值齊次線性、無常數項
//   （證明出處：dmsh_b/<鏈版本>/4a819818b8e489c5_b.metal，逐行讀過）。以後別的著色器證過了往裡加。
// ---------- Zero-source skip ----------
// Shader fact table (statically proven keys): 'the textures it samples are all 0 and the coefficient is finite ⇒ the output is all 0'. Coefficient = component comp of vector vec of fragment-stage constant buffer register ubo_slot.
//   4a819818b8e489c5 ps_outline_blur_p: output = (sum of several samples of the same texture along a row / column) × fp_c7._m0[1].x (fp_c7 = cbuf reg 4), homogeneous linear in the sampled values, no constant term
//   (source of the proof: dmsh_b/<chain version>/4a819818b8e489c5_b.metal, read line by line). Add other shaders here once they are proven.
typedef struct { char key[17]; int ubo_slot, vec, comp; } zskip_fact;
static zskip_fact g_zskip_facts[16] = {{"4a819818b8e489c5", 4, 1, 0}};
static int g_zskip_nf = 1;
void zskip_test_add(const char *key, int ubo_slot, int vec, int comp) {
    if (!key || g_zskip_nf >= 16) return;
    snprintf(g_zskip_facts[g_zskip_nf].key, sizeof g_zskip_facts[0].key, "%s", key);
    g_zskip_facts[g_zskip_nf].ubo_slot = ubo_slot; g_zskip_facts[g_zskip_nf].vec = vec; g_zskip_facts[g_zskip_nf].comp = comp; g_zskip_nf++;
}
int zskip_test_count(void) { return g_zskip_count; }
static int g_zskip_env = -1;   // DM_NVN_ZSKIP 的緩存（−1 = 未讀）。本函數每次繪製都調，getenv 在提交路徑佔過約 8%；單測運行中切開關後把它置回 −1 | Cache of DM_NVN_ZSKIP (−1 = not read). This function is called on every draw, and getenv once took about 8% of the submit path; unit tests set it back to −1 after toggling the switch mid-run
static int zskip_on(void) { if (g_zskip_env < 0) { const char *e = getenv("DM_NVN_ZSKIP"); g_zskip_env = !e || strcmp(e, "0"); } return g_zskip_env && tbdr_on(); }
static void zskip_clear(void) { g_zskip_n = 0; memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok); }
static void zskip_del(uint64_t ser) { for (int i = 0; i < g_zskip_n;) if (g_zskip_zero[i].ser == ser) g_zskip_zero[i] = g_zskip_zero[--g_zskip_n]; else i++; }
static int zskip_has(uint64_t ser, int lv, int sl) { for (int i = 0; i < g_zskip_n; i++) if (g_zskip_zero[i].ser == ser && g_zskip_zero[i].level == lv && g_zskip_zero[i].slice == sl) return 1; return 0; }
static void zskip_add(uint64_t ser, int lv, int sl) { if (!ser || zskip_has(ser, lv, sl) || g_zskip_n >= 4096) return; g_zskip_zero[g_zskip_n++] = (zskip_zero){ser, (uint16_t)lv, (uint16_t)sl}; }
static int zskip_zero_clear(int i) { return R.clear_c[i] && R.cc[i].red == 0 && R.cc[i].green == 0 && R.cc[i].blue == 0 && R.cc[i].alpha == 0; }
static void zskip_commit_clears(void) { for (int i = 0; i < 8; i++) if (g_zskip_cand_ok[i]) zskip_add(g_zskip_cand[i].ser, g_zskip_cand[i].level, g_zskip_cand[i].slice); }
static int zskip_target_zero(const tex_t *t, const view_t *v, int hv) { int lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0; return t && zskip_has(t->serial, lv, sl); }
#ifdef NVN_RT_RANGE_TEST
static int g_rr_checking;
static void rr_begin(void), rr_finish(int ranged);
static void rr_snap(const p_draw *, const p_draw_idx *);
static void rr_reset(void);
#endif
static id<MTL4RenderCommandEncoder> g_enc;
// 當前渲染通道的目標信息：rp_begin 時由附件定一次，通道內各繪製共用（管線鍵、深度模板、視口 / 剪裁都只依賴它）
// Target info of the current render pass: determined once from the attachments at rp_begin and shared by all draws in the pass (pipeline key, depth-stencil, viewport / scissor all depend only on it)
static struct { uint32_t cfmt[8], dfmt, sfmt, samples; int hd, hs; NSUInteger tw, th; uint64_t serial, reg_gen, ser_gen, range_dirty; int range_external, range_depth_nonro; __unsafe_unretained id<MTLTexture> range_color, range_depth; } g_pass;
static id<MTLRenderPipelineState> g_enc_pso; static id<MTLDepthStencilState> g_enc_dss; static int g_enc_clamp;   // 本編碼器上的深度夾限（Metal 新編碼器缺省裁切 = 0） | depth clamp on this encoder (a new Metal encoder defaults to clipping = 0)   // 本編碼器上已設的（相同不重設） | Already set on this encoder (not set again if identical)
static int g_enc_vis, g_enc_has_vis;   // 本編碼器當前的可見性計數模式（0 關 / 槽號 + 1）、是否掛了可見性緩衝 | This encoder's current visibility count mode (0 off / slot number + 1), and whether a visibility buffer is attached
static int g_pso_dirty = 1, g_dss_dirty = 1;   // 回放中改了管線 / 深度模板相關狀態 | Pipeline / depth-stencil related state was changed during replay
static uint64_t g_stat_args_enc, g_stat_args_reuse;   // 參數緩衝：新編 / 復用次數 | Argument buffer: new-encode / reuse counts
static uint64_t g_stat_draw, g_stat_pass, g_bw_load, g_bw_store, g_pass_full_rw, g_udc_saved_bytes;   // 帶寬賬：附件讀入 / 寫出字節、全量讀寫通道數 | Bandwidth ledger: attachment load / store bytes, number of full load-store passes
// DM_NVN_PASSDUMP：逐條記通道附件 / 採樣 / 出圖（貼圖按序號），供離線做數據流分析；P / D / R / S 行的通道號一致（= 開通道後的 g_stat_pass）
// DM_NVN_PASSDUMP=幀號 或 起-止（含兩端）
// DM_NVN_PASSDUMP: records pass attachments / samples / presents one by one (textures by serial) for offline data-flow analysis; the pass numbers of P / D / R / S lines agree (= g_stat_pass after the pass is opened)
// DM_NVN_PASSDUMP=frame number, or start-end (inclusive at both ends)
// DM_NVN_PASSDUMP=@寬x高[+延後幀數][:幀數]：頭一回出現這個尺寸的顏色附件後，延後若干幀（缺省 30）轉儲那一幀——進度跟幀號對不上時（機器忙）照樣抓得到那一頁
// DM_NVN_PASSDUMP=@WxH[+delay frames][:frame count]: once a color attachment of that size first appears, dump the frame that many frames later (default 30) -- catches the page even when progress and frame numbers drift apart (a busy machine)
static long g_pd_a0 = -2, g_pd_a1; static unsigned g_pd_tw, g_pd_th, g_pd_delay, g_pd_count = 1;
static void passdump_init(void) {
    const char *e = getenv("DM_NVN_PASSDUMP"); g_pd_a0 = -1;
    if (e && e[0] == '@') { const char *d = strchr(e, '+'), *k = strchr(e, ':'); if (sscanf(e + 1, "%ux%u", &g_pd_tw, &g_pd_th) != 2) g_pd_tw = g_pd_th = 0;
        g_pd_delay = d ? (unsigned)atoi(d + 1) : 30; g_pd_count = k ? (unsigned)atoi(k + 1) : 1; if (!g_pd_count) g_pd_count = 1; return; }
    if (e) { g_pd_a0 = atol(e); g_pd_a1 = strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_pd_a0; }
}
static void passdump_arm(unsigned w, unsigned h) {
    if (g_pd_a0 == -2) passdump_init();
    if (!g_pd_tw || g_pd_a0 >= 0 || w != g_pd_tw || h != g_pd_th) return;
    g_pd_a0 = (long)atomic_load(&g_cur_frame) + g_pd_delay; g_pd_a1 = g_pd_a0 + g_pd_count - 1; MLOG("DM_NVN_PASSDUMP：%ux%u 首次出現，轉儲第 %ld–%ld 幀", w, h, g_pd_a0, g_pd_a1);
}
static int passdump(void) {
#ifdef NVN_RT_RANGE_TEST
    if (g_rr_checking) return 0;
#endif
    if (g_pd_a0 == -2) passdump_init();
    long f = (long)atomic_load(&g_cur_frame); return g_pd_a0 >= 0 && f >= g_pd_a0 && f <= g_pd_a1;
}
#define PD(...) do { if (passdump()) { fprintf(stderr, "[pass] F%llu ", (unsigned long long)atomic_load(&g_cur_frame)); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static void blit_note(void) { if (g_enc) PD("E %llu 拷貝 / 緩衝更新結束渲染通道", g_stat_pass); }
// DM_GFXUSE=<文件>：選角頁穩定後（首次 OpenPanel MainMenuButtonRibbon 之後 1 秒起，需 DM_BOOTPROF=1）連續 30 幀裡
// GPU 實際讀取的資源在客體內存中的區間（貼圖存儲、UBO / SSBO、頂點 / 索引緩衝），按（種類, 地址, 長度）去重，首見時記一行。
// 供與 inflate 輸出區間求交（哪些選角前解壓的數據選角頁真用到）。列：kind  guest_addr  len  first_frame  tex_id  fmt  w×h
// DM_GFXUSE=<file>: once the character select screen is stable (starting 1 second after the first OpenPanel MainMenuButtonRibbon, requires DM_BOOTPROF=1), over 30 consecutive frames,
// the ranges in guest memory of the resources the GPU actually reads (texture storage, UBO / SSBO, vertex / index buffers), deduplicated by (kind, address, length), one line recorded on first sight.
// For intersecting with the inflate output ranges (which data decompressed before character select is really used by the character select screen). Columns: kind  guest_addr  len  first_frame  tex_id  fmt  w×h
__attribute__((weak)) uint64_t dm_prof_ribbon_ns(void) { return 0; }   // diag.c 提供強定義；未合入時恆 0 → 時間窗不開，結束時報紅 | diag.c provides the strong definition; when not merged it is always 0 → the time window does not open, and a failure is reported at the end
__attribute__((weak)) void dm_shot_written(const char *path, int ok) { (void)path; (void)ok; }
__attribute__((weak)) void dm_guest_frame_tick(void) {}   // hle_os.c 提供強定義（DM_DETCLOCK 確定性客體時鐘）；圖形單測不鏈宿主時為空操作   // diag.c 提供強定義（DM_TICK_HOLD=E:SHOT 解凍）；圖形單測不鏈宿主時為空操作 | hle_os.c provides the strong definition (DM_DETCLOCK deterministic guest clock); a no-op when graphics unit tests do not link the host   // diag.c provides the strong definition (DM_TICK_HOLD=E:SHOT unfreeze); a no-op when graphics unit tests do not link the host
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }   // 同上：宿主 hle_os.c 提供強定義；圖形單測不鏈宿主時確定性時鐘視為關閉 | Same as above: the host's hle_os.c provides the strong definition; when graphics unit tests do not link the host, the deterministic clock is treated as off
__attribute__((weak)) uint64_t guest_ns(void) { return 0; }          // 同上：僅在確定性時鐘開啓時被讀，圖形單測裡不會走到 | Same as above: read only when the deterministic clock is on; never reached in graphics unit tests
enum { USE_OFF, USE_WAIT, USE_REC, USE_DONE };
static int g_use = -1; static FILE *g_use_f; static long g_use_f0, g_use_rows; static nvn_hmap g_use_seen = {.klen = 24};
#define USE_FRAMES 30
static void use_exit(void) {
    if (g_use == USE_WAIT) fprintf(stderr, "[dm] 【紅】DM_GFXUSE 時間窗始終沒開：dm_prof_ribbon_ns() = %llu（需 DM_BOOTPROF=1 且宿主含面板打點）\n", (unsigned long long)dm_prof_ribbon_ns());
    else if (g_use == USE_REC) fprintf(stderr, "[dm] 【紅】DM_GFXUSE 只記了 %ld 幀（不足 %d）即退出，文件未完整\n", (long)atomic_load(&g_cur_frame) - g_use_f0, USE_FRAMES);
    if (g_use_f) fflush(g_use_f);
}
static void use_tick(void) {   // 每次出圖調用 | Called on every present
    if (g_use < 0) { const char *e = getenv("DM_GFXUSE"); g_use = USE_OFF;
        if (e) { g_use_f = fopen(e, "w"); if (!g_use_f) { MLOG("【紅】DM_GFXUSE 打不開 %s", e); return; }
            fprintf(g_use_f, "kind\tguest_addr\tlen\tfirst_frame\ttex_id\tfmt\twxh\n"); fflush(g_use_f); g_use = USE_WAIT; atexit(use_exit);
            MLOG("[gfxuse] 等面板事件：首次 OpenPanel MainMenuButtonRibbon 後 1 秒起記 %d 幀 → %s", USE_FRAMES, e); } }
    long f = (long)atomic_load(&g_cur_frame);
    if (g_use == USE_WAIT) {
        uint64_t t = dm_prof_ribbon_ns();
        if (t && now_ns() - t >= 1000000000ull) { g_use = USE_REC; g_use_f0 = f; MLOG("[gfxuse] 開始記錄：幀 %ld 起 %d 幀", f, USE_FRAMES); }
    } else if (g_use == USE_REC && f - g_use_f0 >= USE_FRAMES) {
        g_use = USE_DONE; fclose(g_use_f); g_use_f = NULL;
        MLOG("[gfxuse] 記錄完成：幀 %ld–%ld，去重後 %ld 個區間", g_use_f0, f - 1, g_use_rows);
    }
}
static __attribute__((always_inline)) inline void use_rec(const char *kind, uint64_t addr, uint64_t len, int tid, const tex_t *t) {
    if (g_use != USE_REC || !addr || !len) return;
    uint64_t k[3] = {addr, len, (uint64_t)(uint8_t)kind[0]};
    if (nvn_hm_get(&g_use_seen, k)) return;
    nvn_hm_put(&g_use_seen, k, (void *)1);
    g_use_rows++;
    if (t) fprintf(g_use_f, "%s\t%#llx\t%llu\t%lld\t%d\t%#x\t%ux%u\n", kind, (unsigned long long)addr, (unsigned long long)len, (long long)atomic_load(&g_cur_frame), tid, t->d.format, t->d.width, t->d.height);
    else fprintf(g_use_f, "%s\t%#llx\t%llu\t%lld\t\t\t\n", kind, (unsigned long long)addr, (unsigned long long)len, (long long)atomic_load(&g_cur_frame));
}
static uint64_t att_bytes(id<MTLTexture> t, NSUInteger level) {
    if (!t) return 0;
    NSUInteger w = MAX(t.width >> level, 1), h = MAX(t.height >> level, 1);
    NSUInteger bpp = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? 5 : t.pixelFormat == MTLPixelFormatRGBA16Float || t.pixelFormat == MTLPixelFormatRG32Float ? 8 :
                     t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR8Unorm || t.pixelFormat == MTLPixelFormatStencil8 ? 1 :
                     t.pixelFormat == MTLPixelFormatRG8Unorm || t.pixelFormat == MTLPixelFormatR16Float ? 2 : 4;
    return (uint64_t)w * h * bpp * t.sampleCount;
}
// 渲染附件選中的是某個mip，MTLTexture.width/height仍表示基級。空附件不參與邊界。
// The render attachment selects a particular mip, while MTLTexture.width/height still describe the base level. Empty attachments do not take part in the bounds.
static NSUInteger mip_extent(NSUInteger size, NSUInteger level) {
    return level < sizeof(size) * 8 ? MAX(size >> level, 1) : 1;
}
static void pass_dims(MTL4RenderPassDescriptor *pd, NSUInteger nc, NSUInteger *w, NSUInteger *h) {
    *w = *h = 0;
    for (NSUInteger i = 0; i < nc + 2; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < nc ? pd.colorAttachments[i] : i == nc ? pd.depthAttachment : pd.stencilAttachment;
        if (!a.texture) continue;
        NSUInteger aw = mip_extent(a.texture.width, a.level), ah = mip_extent(a.texture.height, a.level);
        *w = *w ? MIN(*w, aw) : aw; *h = *h ? MIN(*h, ah) : ah;
    }
    if (!*w) *w = *h = 1;
}

// ---------- DM_NVN_PASSGPU=<文件>：逐通道 GPU 時間（階段邊界時間戳計數器） ----------
// 每個渲染通道採 4 個點（頂點起止、片元起止），計算通道採 2 個點；命令緩衝完成後解析，按「幀內第幾個通道」累計。
// 每行：frame idx kind gpu_ms inc_ms w h fmt0 dfmt load draws t0_ns t1_ns（kind：R 渲染 / C 計算；load：各顏色附件 + 深度的 L 讀入 / C 清除）
// TBDR 上通道間流水重疊（後一通道的頂點階段早於前一通道片元結束）：gpu_ms = 本通道起止跨度（含排隊），不可相加；
// 歸因用時間線增量 inc_ms = t1 − max(t0, 此前已完成通道的最晚 t1)，各通道增量之和 = GPU 忙的總時長（命令緩衝按提交序完成）
// ---------- DM_NVN_PASSGPU=<file>: per-pass GPU time (stage-boundary timestamp counters) ----------
// Each render pass samples 4 points (vertex start/end, fragment start/end), a compute pass 2 points; resolved after the command buffer completes and accumulated by 'index of the pass within the frame'.
// Each line: frame idx kind gpu_ms inc_ms w h fmt0 dfmt load draws t0_ns t1_ns (kind: R render / C compute; load: L load / C clear for each color attachment + depth)
// On TBDR passes overlap in the pipeline (the later pass's vertex stage is earlier than the end of the earlier pass's fragment stage): gpu_ms = this pass's start-to-end span (including queueing) and cannot be summed;
// attribution uses the timeline increment inc_ms = t1 − max(t0, latest t1 of the passes completed so far); the sum of the increments of all passes = total time the GPU is busy (command buffers complete in submission order)
typedef struct { uint64_t frame, pass; int idx, kind, w, h, draws, slot; uint32_t cfmt0, dfmt; char load[12]; } pg_pass;   // pass：全局通道號（同 DM_NVN_PASSDUMP 各行的通道號，供兩份記錄對接） | pass: global pass number (same as the pass number in the DM_NVN_PASSDUMP lines, for joining the two records)
#define PG_SLOTS 4096   // 計數採樣緩衝上限 32768 字節 = 4096 個時間戳（本機報錯實測） | Counter sample buffer limit 32768 bytes = 4096 timestamps (measured from the error on this machine)
#define PG_RING 8
static int g_pg = -1; static FILE *g_pg_f;
static id<MTLCounterSampleBuffer> g_pg_buf[PG_RING]; static int g_pg_b; static uint32_t g_pg_used;
static pg_pass *g_pg_pass; static int g_pg_np, g_pg_cap, g_pg_open = -1;
static uint64_t g_pg_frame = ~0ull; static int g_pg_idx;
static pthread_mutex_t g_pg_mu = PTHREAD_MUTEX_INITIALIZER;
static inline void args_dirty_all(void);
typedef struct { double sum, last; uint64_t n; pg_pass meta; } pg_stat;
static pg_stat g_pg_st[256]; static uint64_t g_pg_rep_frame;
static int pg_on(void) {
    if (g_pg >= 0) return g_pg;
    const char *e = getenv("DM_NVN_PASSGPU"); g_pg = 0;
    if (!e) return 0;
    if (![g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) { MLOG("【紅】DM_NVN_PASSGPU：本設備不支持階段邊界計數採樣"); return 0; }
    id<MTLCounterSet> ts = nil; for (id<MTLCounterSet> c in g_dev.counterSets) if ([c.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = c;
    if (!ts) { MLOG("【紅】DM_NVN_PASSGPU：沒有時間戳計數器集"); return 0; }
    MTLCounterSampleBufferDescriptor *d = [MTLCounterSampleBufferDescriptor new];
    d.counterSet = ts; d.storageMode = MTLStorageModeShared; d.sampleCount = PG_SLOTS;
    for (int i = 0; i < PG_RING; i++) { NSError *er = nil; g_pg_buf[i] = [g_dev newCounterSampleBufferWithDescriptor:d error:&er]; if (!g_pg_buf[i]) { MLOG("【紅】計數採樣緩衝建不出：%s", er.localizedDescription.UTF8String); return 0; } }
    if (!(g_pg_f = fopen(e, "w"))) { MLOG("【紅】DM_NVN_PASSGPU 打不開 %s", e); return 0; }
    fprintf(g_pg_f, "frame\tidx\tkind\tgpu_ms\tinc_ms\tw\th\tfmt0\tdfmt\tload\tdraws\tt0_ns\tt1_ns\tpass\n");
    MLOG("[passgpu] 逐通道 GPU 計時 → %s", e);
    return g_pg = 1;
}
static void pg_sub_begin(uint64_t fence) {   // 本次提交用環裡第 fence % PG_RING 塊；先等 PG_RING 次之前用它的提交完成 | This submission uses block fence % PG_RING of the ring; first wait for the submission that used it PG_RING submissions ago to complete
    if (!pg_on()) return;
    if (fence > PG_RING) mtl_wait(fence - PG_RING, UINT64_MAX);
    g_pg_b = (int)(fence % PG_RING); g_pg_used = 0; g_pg_np = 0; g_pg_open = -1;
}
static pg_pass *pg_new(int kind, int nslot) {
    if (!pg_on() || g_pg_used + nslot > PG_SLOTS) return NULL;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_pg_frame) { g_pg_frame = f; g_pg_idx = 0; }
    if (g_pg_np == g_pg_cap) { g_pg_cap = g_pg_cap ? g_pg_cap * 2 : 128; g_pg_pass = realloc(g_pg_pass, g_pg_cap * sizeof *g_pg_pass); }
    pg_pass *p = &g_pg_pass[g_pg_np++]; memset(p, 0, sizeof *p);
    p->frame = f; p->pass = g_stat_pass; p->idx = g_pg_idx++; p->kind = kind; p->slot = (int)g_pg_used; p->draws = -(int)g_stat_draw;
    g_pg_used += nslot;
    return p;
}
static void pg_render(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    (void)pd; (void)nc;
    return;
#if 0
    pg_pass *p = pg_new(0, 4); if (!p) return;
    MTLRenderPassSampleBufferAttachmentDescriptor *a = pd.sampleBufferAttachments[0];
    a.sampleBuffer = g_pg_buf[g_pg_b];
    a.startOfVertexSampleIndex = p->slot; a.endOfVertexSampleIndex = p->slot + 1; a.startOfFragmentSampleIndex = p->slot + 2; a.endOfFragmentSampleIndex = p->slot + 3;
    id<MTLTexture> t0 = nil; int k = 0;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        if (!t0) t0 = t; if (k < 10) p->load[k++] = pd.colorAttachments[i].loadAction == MTLLoadActionClear ? 'C' : 'L'; }
    id<MTLTexture> dt = pd.depthAttachment.texture;
    if (dt) { if (k < 10) { p->load[k++] = '|'; p->load[k++] = pd.depthAttachment.loadAction == MTLLoadActionClear ? 'C' : 'L'; } p->dfmt = (uint32_t)dt.pixelFormat; }
    if (!t0) t0 = dt;
    NSUInteger pw, ph; pass_dims(pd, nc, &pw, &ph);
    p->w = (int)pw; p->h = (int)ph; p->cfmt0 = t0 && t0 != dt ? (uint32_t)t0.pixelFormat : 0;
    g_pg_open = g_pg_np - 1;
#endif
}
static id<MTLComputeCommandEncoder> pg_compute(void) {
    pg_pass *p = pg_new(1, 2);
    ser_gate();
    if (!p) { args_dirty_all(); return enc_c_begin(NULL); }
    MTLComputePassDescriptor *cd = [MTLComputePassDescriptor computePassDescriptor];
    cd.sampleBufferAttachments[0].sampleBuffer = g_pg_buf[g_pg_b];
    cd.sampleBufferAttachments[0].startOfEncoderSampleIndex = p->slot; cd.sampleBufferAttachments[0].endOfEncoderSampleIndex = p->slot + 1;
    p->draws = 0;
    args_dirty_all();
    return enc_c_begin(cd);
}
static void pg_end_render(void) { if (g_pg_open >= 0) { g_pg_pass[g_pg_open].draws += (int)g_stat_draw; g_pg_open = -1; } }
static void pg_commit(id<MTLCommandBuffer> cb) {   // 把本次提交的通道表交給完成回調解析 | Hand this submission's pass table to the completion callback for resolving
    if (!g_pg || !g_pg_np) return;
    int n = g_pg_np; pg_pass *ps = malloc(n * sizeof *ps); memcpy(ps, g_pg_pass, n * sizeof *ps);
    id<MTLCounterSampleBuffer> buf = g_pg_buf[g_pg_b]; uint32_t used = g_pg_used;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        NSData *d = [buf resolveCounterRange:NSMakeRange(0, used)];
        const MTLCounterResultTimestamp *ts = d.bytes;
        pthread_mutex_lock(&g_pg_mu);
        for (int i = 0; d && i < n; i++) {
            pg_pass *p = &ps[i]; uint64_t a, b;
            #define OK(x) ((x) != MTLCounterErrorValue && (x) != 0)
            if (p->kind == 0) { a = OK(ts[p->slot].timestamp) ? ts[p->slot].timestamp : ts[p->slot + 2].timestamp; b = OK(ts[p->slot + 3].timestamp) ? ts[p->slot + 3].timestamp : ts[p->slot + 1].timestamp; }
            else { a = ts[p->slot].timestamp; b = ts[p->slot + 1].timestamp; }
            if (!OK(a) || !OK(b) || b < a) continue;
            #undef OK
            static uint64_t prev_end;   // 此前已完成通道的最晚結束時刻（完成回調按提交序串行） | Latest end time of the passes completed so far (completion callbacks are serialized in submission order)
            double ms = (b - a) / 1e6;   // 時間戳與 CPU 同為 ns（本機 sampleTimestamps 實測 1:1） | Timestamps are in ns, same as the CPU (measured 1:1 via sampleTimestamps on this machine)
            double inc = b > prev_end ? (b - (a > prev_end ? a : prev_end)) / 1e6 : 0;
            if (b > prev_end) prev_end = b;
            fprintf(g_pg_f, "%llu\t%d\t%c\t%.4f\t%.4f\t%d\t%d\t%u\t%u\t%s\t%d\t%llu\t%llu\t%llu\n", (unsigned long long)p->frame, p->idx, p->kind ? 'C' : 'R', ms, inc,
                    p->w, p->h, p->cfmt0, p->dfmt, p->load, p->draws, (unsigned long long)a, (unsigned long long)b, (unsigned long long)p->pass);
            if (p->idx < 256 && p->frame > 300) { pg_stat *st = &g_pg_st[p->idx]; st->sum += inc; st->n++; st->last = inc; st->meta = *p; }
            if (p->frame >= g_pg_rep_frame + 300 && p->frame > 300) {   // 每 300 幀報一次前五（第 300 幀起累計） | Report the top five every 300 frames (accumulated from frame 300)
                g_pg_rep_frame = p->frame; double tot = 0; int top[5] = {-1, -1, -1, -1, -1};
                for (int k = 0; k < 256; k++) if (g_pg_st[k].n) { double m = g_pg_st[k].sum / g_pg_st[k].n; tot += m;
                    for (int j = 0; j < 5; j++) if (top[j] < 0 || m > g_pg_st[top[j]].sum / g_pg_st[top[j]].n) { for (int q = 4; q > j; q--) top[q] = top[q - 1]; top[j] = k; break; } }
                char line[1024]; size_t o = 0;
                for (int j = 0; j < 5 && top[j] >= 0; j++) { pg_stat *t = &g_pg_st[top[j]];
                    o += snprintf(line + o, sizeof line - o, " #%d %.2f ms %dx%d fmt%u d%u %s %d 繪製；", top[j], t->sum / t->n, t->meta.w, t->meta.h, t->meta.cfmt0, t->meta.dfmt, t->meta.load, t->meta.draws); }
                MLOG("[passgpu] 至幀 %llu 每幀通道均值合計 %.2f ms；前五：%s", (unsigned long long)p->frame, tot, line);
                fflush(g_pg_f);
            }
        }
        pthread_mutex_unlock(&g_pg_mu);
        free(ps);
    }];
}
void ts_after(void);
// ---------- 寫回前瞻：通道結束時按同一次提交的後續命令判各附件寫回是否多餘 ----------
// TBDR 上每個附件結束時整塊寫回內存；Switch（Maxwell 直接渲染）沒有這筆開銷。附件若在被讀之前先被整塊清除，寫回即多餘（DontCare）。
// 判定見 mtl_sub_index / sdx_decide；只認同一次提交內能證明的，到提交末尾仍未定論一律寫回。
// ---------- Store lookahead: at pass end, decide from the later commands of the same submission whether each attachment's write-back is redundant ----------
// On TBDR every attachment is written back to memory in full when the pass ends; Switch (Maxwell immediate-mode rendering) has no such cost. If an attachment is cleared in full before being read, the write-back is redundant (DontCare).
// For the decision see mtl_sub_index / sdx_decide; only what can be proven within the same submission is accepted, and anything still undecided at the end of the submission is always written back.
typedef struct { uint64_t ser, bytes; uint32_t lv, sl; int idx; uint8_t depth, state, ridx, drawn, why, load, wrote, wwhy; const char *wkey; int wslot; } sd_att;   // depth：0 顏色 1 深度 2 模板（深度模板合一的貼圖兩部分分開判）   // state 0 未定 1 需要 2 多餘；idx：顏色附件序號，深度為 -1 | depth: 0 color 1 depth 2 stencil (the two parts of a combined depth-stencil texture are decided separately)   // state 0 undecided 1 needed 2 redundant; idx: color attachment index, -1 for depth
static sd_att g_sd[9]; static int g_nsd;
static int g_udc_ab = -1, g_udc_ab_on;
// 本通道沒寫的附件（只做深度 / 模板測試、顏色寫掩碼為 0 等）寫回改 DontCare：內容本就在內存裡，免一次整塊寫回。
// 缺省開：Mac 同進程開 / 關截圖與空對照同分布；A19 Pro 私有 Depth32F_S8 / RGBA8 DontCare 後讀回 0 處不同。
// DM_NVN_UNWRITTEN_DC=0 關閉（等價對照用）
// Attachments this pass did not write (depth / stencil test only, color write mask 0, etc.) get their write-back changed to DontCare: the content is already in memory, saving one full write-back.
// On by default: on Mac, same-process on / off screenshots follow the same distribution as the null control; on A19 Pro, private Depth32F_S8 / RGBA8 read back after DontCare differ in 0 places.
// DM_NVN_UNWRITTEN_DC=0 turns it off (for equivalence comparison)
static int udc_on(void) {
    if (g_udc_ab < 0) { const char *e = getenv("DM_NVN_UNWRITTEN_DC"); g_udc_ab = !(e && !strcmp(e, "0"));
        const char *a = getenv("DM_NVN_UDC_AB"); if (a && !strcmp(a, "1")) { g_udc_ab = 0; g_udc_ab_on = 0; } }
    return g_udc_ab || g_udc_ab_on;
}
// 診斷 DM_NVN_TBDR=0：關掉全部 TBDR 優化（寫回前瞻、讀入可省、同目標續用、純清除通道前提、深度模板按需掛），回放走最樸素的「全掛、全讀、全寫」路徑。
// 只用於等價對照（同一回放局開 / 關逐字節比畫面），不是產品配置
// Diagnostic DM_NVN_TBDR=0: turns off all TBDR optimizations (store lookahead, load elision, same-target continuation, clear-only pass hoist, on-demand depth-stencil attach); replay takes the plainest 'attach all, load all, store all' path.
// Only for equivalence comparison (the same replay run on / off, comparing the picture byte by byte), not a product configuration
// 馬力歐：缺省關（DM_NVN_TBDR=1 才開）。這些優化按「貼圖槽裡綁了哪些貼圖」推斷讀寫關係，而本作 38% 的著色器用無綁定貼圖（句柄在 uniform 緩衝裡，NVN_RB_TEXU），
//   推斷會漏掉讀者——例如判定某附件之後沒人讀而不寫回。等它們認得無綁定讀者再開。
// Mario: off by default (only DM_NVN_TBDR=1 turns it on). These optimizations infer read / write relations from "which textures are bound in texture slots", while 38% of this game's shaders use bindless textures (handles in uniform buffers, NVN_RB_TEXU),
//   so the inference would miss readers - e.g. deciding an attachment is never read afterwards and not storing it. Turn them back on once they recognize bindless readers.
static int tbdr_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_TBDR"), *r = getenv("DM_NVN_RULER"); v = e && !strcmp(e, "1"); g_ruler_truth = r && !strcmp(r, "1"); if (!v) MLOG("TBDR 優化缺省關（本作有無綁定貼圖；DM_NVN_TBDR=1 開）"); } return v && !g_ruler_truth; }
int dm_nvn_ruler_truth(void) { tbdr_on(); return g_ruler_truth; }   // 視覺尺子真值階段（宿主側的被測設置據此讓開，如 3D 渲染比例） | Visual ruler ground-truth phase (host-side settings under test yield accordingly, e.g. the 3D render scale)
static int rt_range_on(void) { static int v = -1; if (v < 0) { const char *e=getenv("DM_NVN_RT_RANGE"); v=e && !strcmp(e,"1"); } return v; }   // 窄候選，缺省關 | Narrow candidate, off by default
static uint64_t g_sd_dead_bytes, g_sd_n_dead, g_sd_n_all;
// ---------- 跨提交附件有效性賬本 ----------
// 條目 = 子資源（貼圖序號, 級, 層）+ 狀態，跨提交存活：
//   XV_INVALID       內容沒寫回 / 沒生成，不許被讀。下一次觸碰必須是整張 Clear，或作附件且讀入動作 DontCare / Clear；否則違規。
//   XV_PENDING_CLEAR 預留給「純清除跨提交掛起」（未實現）：只收整張、無剪裁、寫掩碼全開的清除；已掛起再清 = 換清除值不落地；
//                    作附件且讀入 DontCare / Clear → 作廢掛起，讀入 Load → 改成 Clear 加載；深度模板按平面分開記。
// 違規 = 報紅 + 該穩定鍵（格式 / 寬 / 高 / 級）永久禁用（內存 + DM_XV_FILE 落盤，下次啓動讀入）+ 同貼圖條目全刪。兜底只能發現不能輓回（那一幀該附件是舊內容）。
// 用它的第一刀：提交末尾「掃到底未定」的顏色附件不寫回（sd_finish）——條件：本次提交裡它首次觸碰是整張清 / DontCare 讀入（每幀先重置再用）、不是窗口貼圖、鍵未禁用。
// 開關 DM_NVN_XV=0 關；tbdr_on()==0（視覺尺子真值模式）不生效。
// 試用期（XV_PROBE）：某個鍵頭 XV_TRIAL 次只「影子記賬」——照常寫回，但同樣跟蹤它的下一次觸碰；連續 XV_TRIAL 次都是合規觸碰才開始真的不寫回。
//   試用期裡遇到讀 / Load：只把該鍵禁用（本進程內，不落盤、不報紅），畫面無損—— 實測啓動 / 菜單頁就有讀者（放大目標 36），不該讓每次全新安裝都錯一幀。
// ---------- Cross-submission attachment validity ledger ----------
// Entry = subresource (texture serial, level, layer) + state, surviving across submissions:
//   XV_INVALID       content was not written back / not generated and must not be read. The next touch must be a full Clear, or use as an attachment with load action DontCare / Clear; otherwise it is a violation.
//   XV_PENDING_CLEAR reserved for 'clear-only pending across submissions' (not implemented): accepts only full, unscissored clears with the write mask fully open; clearing again while pending = replace the clear value without landing it;
//                    as an attachment with load DontCare / Clear → the pending clear is voided, with load Load → changed to a Clear load; depth-stencil is recorded per plane.
// Violation = report a failure + that stable key (format / width / height / level) permanently disabled (in memory + persisted to DM_XV_FILE, read at the next launch) + all entries of the same texture deleted. The fallback can only detect, not undo (in that frame the attachment holds old content).
// Its first use: color attachments 'scanned to the end undecided' at the end of a submission are not written back (sd_finish) - conditions: its first touch in this submission is a full clear / DontCare load (reset every frame before use), it is not a window texture, and the key is not disabled.
// Switch DM_NVN_XV=0 turns it off; has no effect when tbdr_on()==0 (visual ruler ground-truth mode).
// Trial period (XV_PROBE): for a key's first XV_TRIAL times it is only 'shadow-accounted' - written back as usual, but its next touch is tracked all the same; only after XV_TRIAL consecutive compliant touches does the write-back really start being skipped.
//   On a read / Load during the trial period: only disable that key (within this process, not persisted, no failure), the picture is intact - in practice there are readers as early as the launch / menu screens (upscale target 36), and not every fresh install should get one wrong frame.
enum { XV_INVALID = 1, XV_PENDING_CLEAR = 2, XV_PROBE = 3 };
#define XV_TRIAL 3
typedef struct { uint64_t ser, tag; uint32_t lv, sl; uint8_t state; } xv_ent;
static xv_ent g_xv[64]; static int g_xv_n, g_xv_block, g_xv_init;
static uint64_t g_xv_disabled[128]; static int g_xv_ndis;
static struct { uint64_t tag; int good; } g_xv_trial[128]; static int g_xv_ntrial;   // 各鍵已連續合規的次數 | Number of consecutive compliant touches for each key
static int xv_trial_get(uint64_t tag, int add) { for (int i = 0; i < g_xv_ntrial; i++) if (g_xv_trial[i].tag == tag) return g_xv_trial[i].good += add; if (g_xv_ntrial < 128) { g_xv_trial[g_xv_ntrial].tag = tag; return g_xv_trial[g_xv_ntrial++].good = add; } return 0; }
static uint64_t g_xv_present[128]; static int g_xv_npresent;
static uint64_t g_xv_dc, g_xv_viol;
typedef struct { uint64_t ser; uint32_t lv, sl; uint8_t good; } xv_touch;
static xv_touch g_xv_touch[128]; static int g_xv_ntouch;
static uint64_t xv_tag(const tex_t *t, uint32_t lv) { return ((uint64_t)t->d.format << 40) ^ ((uint64_t)t->d.width << 24) ^ ((uint64_t)t->d.height << 8) ^ lv; }
// 穩定鍵（跨啓動不變，不用貼圖序號）：格式 / 寬 / 高 / 級，再混入不寫回時所在通道的各顏色附件格式與本附件序號——同格式同尺寸的不同用途貼圖（如透光度 27 與放大目標 36）不共用一個鍵
// Stable key (unchanged across launches, does not use the texture serial): format / width / height / level, then mixed with the formats of the color attachments of the pass where the write-back is skipped and this attachment's index - textures of the same format and size but different purpose (e.g. transmittance 27 and upscale target 36) do not share a key
static uint64_t xv_tag_pass(const tex_t *t, uint32_t lv, int idx, const p_set_rt *rt) { uint64_t h = xv_tag(t, lv) ^ ((uint64_t)(idx + 1) << 60); for (int i = 0; i < rt->n && i < 8; i++) h = (h ^ (rt->color[i] ? (uint64_t)rt->color[i]->d.format + 1 : 0)) * 0x100000001b3ull; return h; }
static void xv_init(void) {
    if (g_xv_init) return; g_xv_init = 1;
    const char *p = getenv("DM_XV_FILE"); if (!p || !*p) return;
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256], why[160]; unsigned long long tag; int ch, n = 0;
    while (fgets(line, sizeof line, f) && g_xv_ndis < 128) if (sscanf(line, "%llx\t%159[^\t]\t%d", &tag, why, &ch) >= 1) g_xv_disabled[g_xv_ndis++] = tag, n++;
    fclose(f); MLOG("讀到 %d 條禁用記錄", n);
}
static int xv_disabled(uint64_t tag) { xv_init(); for (int i = 0; i < g_xv_ndis; i++) if (g_xv_disabled[i] == tag) return 1; return 0; }
static xv_ent *xv_find(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) return &g_xv[i]; return NULL; }
static void xv_drop(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) { g_xv[i] = g_xv[--g_xv_n]; return; } }
static void xv_drop_all_of(uint64_t ser) { for (int i = 0; i < g_xv_n;) if (g_xv[i].ser == ser) g_xv[i] = g_xv[--g_xv_n]; else i++; }
static void xv_set(uint64_t ser, uint32_t lv, uint32_t sl, uint8_t state, uint64_t tag) {
    xv_init(); if (g_xv_block || xv_disabled(tag)) return;
    xv_ent *e = xv_find(ser, lv, sl); if (e) { e->state = state; e->tag = tag; return; }
    if (g_xv_n < 64) g_xv[g_xv_n++] = (xv_ent){ser, tag, lv, sl, state};
}
static void xv_violation(const xv_ent *e, const char *why) {
    xv_init();
    if (e->state == XV_PROBE) {   // 試用期：內容還在，畫面無損——只把鍵禁用（本進程） | Trial period: the content is still there and the picture is intact - only disable the key (this process)
        if (!xv_disabled(e->tag) && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
        PD("V xv 試用期見讀者：鍵=%016llx 貼圖=%llu 級=%u 原因=%s → 該鍵不做", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, why);
        xv_drop_all_of(e->ser); return; }
    g_xv_viol++;
    int seen = xv_disabled(e->tag); if (!seen && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
    MLOG("【紅】xv 標籤=%016llx 貼圖=%llu 級=%u 層=%u 原因=%s 通道=%llu", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, e->sl, why, (unsigned long long)g_stat_pass);
    const char *p = getenv("DM_XV_FILE"); if (p && *p) { FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%016llx\t%s\t%llu\n", (unsigned long long)e->tag, why, (unsigned long long)g_stat_pass); fclose(f); } }
    xv_drop_all_of(e->ser);
}
static void xv_check(uint64_t ser, uint32_t lv, uint32_t sl, const char *why) { xv_ent *e = xv_find(ser, lv, sl); if (e && (e->state == XV_INVALID || e->state == XV_PROBE)) { xv_ent copy = *e; xv_violation(&copy, why); } }
static void xv_good(xv_ent *e) { if (e->state == XV_PROBE) xv_trial_get(e->tag, 1); xv_drop(e->ser, e->lv, e->sl); }   // 合規觸碰：試用期計一次，條目回到有效 | Compliant touch: counts once toward the trial period, and the entry returns to valid
static void xv_touch_add(uint64_t ser, uint32_t lv, uint32_t sl, int good) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return; if (g_xv_ntouch < 128) g_xv_touch[g_xv_ntouch++] = (xv_touch){ser, lv, sl, (uint8_t)good}; }
static int xv_first_good(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return g_xv_touch[i].good; return 0; }
static int xv_present(uint64_t ser) { for (int i = 0; i < g_xv_npresent; i++) if (g_xv_present[i] == ser) return 1; return 0; }
static void xv_forward(void);
static int tbdr_on(void);
static int xv_on(void) { static int off = -1; if (off < 0) { const char *e = getenv("DM_NVN_XV"); off = e && !strcmp(e, "0"); } return !off && tbdr_on(); }
void mtl_xv_window_tex(uint64_t serial) { if (!xv_present(serial) && g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = serial; }   // 窗口（交換鏈）貼圖：永不進賬本 | Window (swapchain) texture: never enters the ledger
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = (int)g_xv_dc; if (viol) *viol = (int)g_xv_viol; }
static void sd_finish(void);
// 讀入可省（全覆蓋首繪）：開通道的這次繪製若可證覆蓋某顏色附件整塊（first_draw_covers），該附件讀入改 DontCare——
// 首繪不混合、寫滿通道，覆蓋到的每個像素都被它重寫，原內容不可見（Switch 直接渲染無此開銷；TBDR 上讀入 = 整塊從內存讀進 tile）
// Load elision (fully covering first draw): if the draw that opens the pass can be proven to cover a color attachment in full (first_draw_covers), that attachment's load becomes DontCare -
// the first draw does not blend and writes all channels, every covered pixel is rewritten by it, and the original content is not visible (Switch immediate-mode rendering has no such cost; on TBDR load = reading the whole thing from memory into the tile)
static const p_draw *g_fd_dw; static const p_draw_idx *g_fd_di;   // 開通道的這次繪製（非索引 / 索引其一）；僅 draw() 開通道期間非空 | The draw that opens the pass (one of non-indexed / indexed); non-null only while draw() is opening a pass
static uint64_t g_fd_n;         // 累計判可省的附件數 | Cumulative number of attachments judged elidable
static int g_fd_why;            // 最近一次不成立的原因（PASSDUMP 的 Q 行：1 無程序 2 著色器 3 附件 4 混合/掩碼/光柵 5 深度模板測試 6 頂點數 7 位置屬性 8 格式/流 9 越界 10 地址 11 幾何） | Reason for the most recent failure (Q line of PASSDUMP: 1 no program 2 shader 3 attachment 4 blend/mask/raster 5 depth-stencil test 6 vertex count 7 position attribute 8 format/stream 9 out of bounds 10 address 11 geometry)
static int first_draw_covers(int i, id<MTLTexture> t, NSUInteger level);
static int pf_is_float(MTLPixelFormat f) {   // 能存 NaN 的浮點顏色格式（讀入可省探針的哨兵選擇；RGB9E5 存不了 NaN，不算） | Float color formats that can store NaN (sentinel choice for the load-elision probe; RGB9E5 cannot store NaN and does not count)
    switch (f) { case MTLPixelFormatR16Float: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA16Float: case MTLPixelFormatR32Float:
        case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA32Float: case MTLPixelFormatRG11B10Float: return 1; default: return 0; }
}
static int fz_load_covers(int i, id<MTLTexture> t, NSUInteger level);
static NSMutableArray *g_lp;    // DM_NVN_LOADPROBE=1 / 2 / 3（哨兵值兩種常數 / NaN——浮點格式下著色器正常輸出碰不到 NaN，排除數值巧合）：判可省的附件改清成哨兵色，通道結束回讀數剩餘哨兵像素（診斷：應為 0） | DM_NVN_LOADPROBE=1 / 2 / 3 (two constant sentinel values / NaN - under float formats normal shader output cannot reach NaN, ruling out numeric coincidence): attachments judged elidable are cleared to the sentinel color instead, and at pass end the remaining sentinel pixels are read back and counted (diagnostic: should be 0)
static void lp_finish(void);
// 同目標續用：遊戲對同一組目標再次 SetRenderTargets（中間無拷貝 / 派發 / 清除）時不結束通道——TBDR 上省一次整塊寫回 + 讀入。
// 等價依據：NVN 在兩個通道間的 Barrier 要求後者看到前者的全部寫入。同一 Metal 渲染編碼器內，後續繪製對附件本身的讀寫（混合、深度 / 模板測試）
// 與先前繪製按提交順序有序；但「採樣」附件貼圖不在此列（同一通道內讀寫同一附件未定義）。故只在後續繪製不採樣這些附件時續用：
// 續用期間某次繪製要採樣它們，或來了清除，就在它之前結束通道，回到分開的語義。拷貝 / 派發本來就會結束渲染編碼器
// Same-target continuation: when the game calls SetRenderTargets again on the same set of targets (with no copy / dispatch / clear in between) the pass is not ended - on TBDR this saves one full write-back + load.
// Basis for equivalence: NVN's Barrier between two passes requires the latter to see all writes of the former. Within one Metal render encoder, later draws' reads and writes of the attachment itself (blend, depth / stencil test)
// are ordered with earlier draws in submission order; but 'sampling' the attachment texture is not covered (reading and writing the same attachment within one pass is undefined). So continuation happens only when later draws do not sample these attachments:
// if during continuation a draw needs to sample them, or a clear arrives, the pass is ended before it, returning to the separate semantics. Copies / dispatches end the render encoder anyway
static int g_enc_mode;          // 當前編碼器的開法（rp_begin_ex 參數：0 全部附件 1 只為清除 2 省深度） | How the current encoder was opened (rp_begin_ex argument: 0 all attachments 1 only for clears 2 depth omitted)
static int g_rt_range_active, g_rt_range_pending;
static int g_rt_merged;         // 當前通道已續用過（至少一次同目標 SetRenderTargets 被併入） | The current pass has already been continued (at least one same-target SetRenderTargets was merged in)
static uint64_t g_rt_merge_n;   // 累計續用次數 | Cumulative continuation count
static int g_defer_replaying;
static void fz_end(void);   // 可讀 tile 通道合併的收尾（定義在 draw 之前） | Wrap-up of the readable-tile pass merge (defined before draw)
static void defer_fallback(void);
// DM_NVN_ENC_LABEL=1：給每個渲染編碼器打標籤「p<幀內第幾個渲染通道> <寬>x<高> <首次繪製的片元鍵前 8 位> d<繪製數>」，Instruments（xctrace Game Performance / Metal System Trace）
// 的編碼器軌與 GPU 區間表直接帶出這個名字，按 shader_map.tsv 就能對回著色器；缺省關（每通道一次字符串分配）
// DM_NVN_ENC_LABEL=1: label every render encoder with 'p<index of the render pass within the frame> <width>x<height> <first 8 digits of the first draw's fragment key> d<draw count>'; in Instruments (xctrace Game Performance / Metal System Trace)
// the encoder track and the GPU interval table carry this name directly, and shader_map.tsv maps it back to the shader; off by default (one string allocation per pass)
static int g_el_w, g_el_h, g_el_draws; static char g_el_key[12];
static int enc_label_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_ENC_LABEL"); v = e && atoi(e); } return v; }
static void ghost_flush(void);
// ---- 壓縮貼圖的塊視圖渲染目標 | Block views of compressed textures as render targets ----
// 遊戲在 GPU 上把貼圖編成 BC（朋友收集生成 Mii 的臉部貼圖）：把 BC 貼圖的存儲當成 RGBA32UI（16 字節塊）或 RG32UI / RGBA16（8 字節塊）
// 的貼圖來畫，一個像素 = 一個 4x4 塊。Metal 不能畫進壓縮格式（原先掛底圖，建管線時斷言中止）：改畫到同尺寸（以塊計）的代理貼圖，
// 通道結束後經緩衝把塊原樣拷進 BC 貼圖；本通道要讀入舊內容時，開通道前先反向拷一次。
// The game encodes textures to BC on the GPU (Tomodachi Life building a Mii's face textures): it draws into the storage of a BC texture as an RGBA32UI (16-byte blocks) or RG32UI / RGBA16 (8-byte blocks)
// texture, one pixel = one 4x4 block. Metal cannot render into compressed formats (attaching the base texture aborted on a pipeline assertion): draw into a proxy texture of the same size in blocks instead,
// and after the pass copy the blocks unchanged into the BC texture through a buffer; when the pass loads the old contents, copy the other way once before the pass opens.
enum { BV_MAX = 16 };
static id<MTLTexture> g_bv_base[BV_MAX], g_bv_proxy[BV_MAX]; static id<MTLBuffer> g_bv_buf[BV_MAX];
static uint32_t g_bv_lv[BV_MAX], g_bv_sl[BV_MAX], g_bv_bytes[BV_MAX]; static uint64_t g_bv_use[BV_MAX], g_bv_tick;
static int g_bv_pend[8], g_bv_npend;
static unsigned bc_block_bytes(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB: case MTLPixelFormatBC4_RUnorm: case MTLPixelFormatBC4_RSnorm: return 8;
        case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC2_RGBA_sRGB: case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC3_RGBA_sRGB:
        case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm: case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat:
        case MTLPixelFormatBC7_RGBAUnorm: case MTLPixelFormatBC7_RGBAUnorm_sRGB: return 16;
        default: return 0;
    }
}
// 代理用浮點格式：翻譯出的片元著色器輸出一律是 float4（Maxwell 輸出寄存器不分類型，編碼器把整數位經 as_type<float> 寫出），
// 整數格式的附件建管線會失敗；同寬的 32 位浮點存的就是原樣的位。RGBA16 類視圖（每分量 16 位）沒有保位的浮點對應，不接
// Proxies use float formats: translated fragment shaders always output float4 (Maxwell output registers are untyped, and the encoder writes integer bits through as_type<float>),
// so an integer attachment fails pipeline creation; a 32-bit float format of the same width stores the bits unchanged. RGBA16-style views (16 bits per component) have no bit-preserving float counterpart and are not handled
static MTLPixelFormat bv_proxy_fmt(MTLPixelFormat view) {
    switch (view) {
        case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float: return MTLPixelFormatRGBA32Float;
        case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: return MTLPixelFormatRG32Float;
        default: return MTLPixelFormatInvalid;
    }
}
static unsigned plain_bytes(MTLPixelFormat f) { return f == MTLPixelFormatRGBA32Float ? 16 : f == MTLPixelFormatRG32Float ? 8 : 0; }
// 底圖是 BC、視圖是同塊大小的 32 位分量格式 → 塊視圖 | BC base and a view of 32-bit components with the same block size → a block view
static int bv_is_block_view(MTLPixelFormat base, MTLPixelFormat view) { unsigned b = bc_block_bytes(base); return b && b == plain_bytes(bv_proxy_fmt(view)); }
static int bv_is_proxy(id<MTLTexture> t) { for (int i = 0; i < BV_MAX; i++) if (t && g_bv_proxy[i] == t) return 1; return 0; }
static void bv_level_dims(int i, NSUInteger *lw, NSUInteger *lh, NSUInteger *bw, NSUInteger *bh) {
    *lw = MAX((NSUInteger)1, g_bv_base[i].width >> g_bv_lv[i]); *lh = MAX((NSUInteger)1, g_bv_base[i].height >> g_bv_lv[i]);
    *bw = (*lw + 3) / 4; *bh = (*lh + 3) / 4;
}
// 取（或建）底圖某級某層的代理；返回表項號，-1 = 建不出 | Get (or create) the proxy for one level and layer of a base texture; returns the entry index, -1 = could not be created
static int bv_proxy(id<MTLTexture> base, uint32_t lv, uint32_t sl, MTLPixelFormat pf) {
    int hit = -1, lru = 0;
    for (int i = 0; i < BV_MAX; i++) {
        if (g_bv_base[i] == base && g_bv_lv[i] == lv && g_bv_sl[i] == sl && g_bv_proxy[i].pixelFormat == pf) { hit = i; break; }
        if (g_bv_use[i] < g_bv_use[lru]) lru = i;
    }
    if (hit < 0) {
        for (int k = 0; k < g_bv_npend; k++) if (g_bv_pend[k] == lru) return -1;   // 本通道還要用的表項不能擠掉 | an entry this pass still needs cannot be evicted
        if (g_bv_proxy[lru]) { defer_obj_release(g_bv_proxy[lru]); defer_obj_release(g_bv_buf[lru]); }
        g_bv_base[lru] = base; g_bv_lv[lru] = lv; g_bv_sl[lru] = sl; g_bv_bytes[lru] = plain_bytes(pf);
        NSUInteger lw, lh, bw, bh; bv_level_dims(lru, &lw, &lh, &bw, &bh);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:bw height:bh mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        g_bv_proxy[lru] = [g_dev newTextureWithDescriptor:td];
        g_bv_buf[lru] = [g_dev newBufferWithLength:bw * bh * g_bv_bytes[lru] options:MTLResourceStorageModePrivate];
        if (!g_bv_proxy[lru] || !g_bv_buf[lru]) { g_bv_base[lru] = nil; g_bv_proxy[lru] = nil; g_bv_buf[lru] = nil; return -1; }
        res_add(g_bv_proxy[lru]); res_add(g_bv_buf[lru]);
        static int logged; if (logged++ < 4) MLOG("壓縮貼圖塊視圖：BC 底圖 %lux%lu 級 %u 層 %u（Metal 格式 %lu）→ 代理 %lux%lu 格式 %lu，通道後經緩衝拷回", (unsigned long)base.width,
            (unsigned long)base.height, lv, sl, (unsigned long)base.pixelFormat, (unsigned long)bw, (unsigned long)bh, (unsigned long)pf);
        hit = lru;
    }
    g_bv_use[hit] = ++g_bv_tick;
    return hit;
}
// dir 0：代理 → 緩衝 → 底圖（通道後）；dir 1：底圖 → 緩衝 → 代理（開通道前讀入舊內容） | dir 0: proxy → buffer → base (after the pass); dir 1: base → buffer → proxy (before a pass that loads the old contents)
static void bv_copy(int i, int dir) {
    NSUInteger lw, lh, bw, bh; bv_level_dims(i, &lw, &lh, &bw, &bh);
    NSUInteger row = bw * g_bv_bytes[i], img = row * bh;
    if (!dir) {
        enc_b_copy_texture_buffer(blit(), g_bv_proxy[i], 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(bw, bh, 1), g_bv_buf[i], 0, row, img);
        enc_b_copy_buffer_texture(blit(), g_bv_buf[i], 0, row, img, MTLSizeMake(lw, lh, 1), g_bv_base[i], g_bv_sl[i], g_bv_lv[i], MTLOriginMake(0, 0, 0));
    } else {
        enc_b_copy_texture_buffer(blit(), g_bv_base[i], g_bv_sl[i], g_bv_lv[i], MTLOriginMake(0, 0, 0), MTLSizeMake(lw, lh, 1), g_bv_buf[i], 0, row, img);
        enc_b_copy_buffer_texture(blit(), g_bv_buf[i], 0, row, img, MTLSizeMake(bw, bh, 1), g_bv_proxy[i], 0, 0, MTLOriginMake(0, 0, 0));
    }
}
static void bv_flush(void) {
    int n = g_bv_npend; g_bv_npend = 0;
    for (int k = 0; k < n; k++) bv_copy(g_bv_pend[k], 0);
}
// 附件回授（同一通道裡取樣自己的顏色附件）：Switch 靠紋理屏障，每個像素讀自己那一點是合法的；Metal 同一渲染通道裡讀寫同一張貼圖是未定義行為，
// 分塊 GPU 會讀到別塊正在寫回的資料（朋友收集的全屏後製讀 HDR 畫面又寫回它：128 行邊界上冒出 NaN，打完名字跳出的確認框把畫面模糊後疊回，NaN 擴散成綠色方塊、從此留在畫布上）。
// 做法：開通道前把被取樣的附件整張拷到一張重用的副本（fbx_prepare），本通道裡取樣這張附件的綁定改讀副本（fbx_bind），通道結束即失效
// Attachment feedback (sampling one's own colour attachment in the same pass): on the Switch a texture barrier makes it legal for each pixel to read its own spot; in Metal reading and writing the same texture in one render pass is undefined,
// and a tiled GPU reads data other tiles are still storing (Tomodachi Life's full-screen post-process reads the HDR picture and writes it back: NaN appears on 128-row boundaries, the confirmation box after typing a name blurs the picture and blends it back, and the NaN spreads into green blocks that stay on the canvas).
// Fix: before the pass, copy the sampled attachment into a reused copy (fbx_prepare); bindings in this pass that sample that attachment read the copy instead (fbx_bind); it lapses when the pass ends
enum { FBX_MAX = 4 };
static __strong id<MTLTexture> g_fbx_pool[FBX_MAX]; static uint64_t g_fbx_use[FBX_MAX], g_fbx_tick;
static __strong id<MTLTexture> g_fbx_copy[FBX_MAX]; static uint64_t g_fbx_ser[FBX_MAX]; static int g_fbx_n;
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *g_fbx_views; static uint64_t g_fbx_count;   // g_fbx_count：累計拷了幾次 | g_fbx_count: copies made so far
static void rp_end(void) { if (g_enc) { AP_B;
    ghost_flush();
    if (enc_label_on()) { static uint64_t lf = ~0ull, base; uint64_t f = atomic_load(&g_cur_frame); if (f != lf) { lf = f; base = g_stat_pass - 1; }   // 幀內序號：同一趟每幀同名，表裡才能按名歸並 | Intra-frame serial: the same pass has the same name every frame, so the table can aggregate by name
        [(id)g_enc setLabel:[NSString stringWithFormat:@"p%02llu %dx%d %s d%d", (unsigned long long)(g_stat_pass - base), g_el_w, g_el_h, g_el_key[0] ? g_el_key : "-", g_el_draws]]; }
#ifdef NVN_RT_RANGE_TEST
    int ranged=g_rt_range_active && !g_rt_range_pending;
#endif
    sd_finish(); enc_r_end(g_enc); zskip_commit_clears(); g_enc = nil; g_rt_merged = 0; if (g_fbx_n) { g_fbx_n = 0; [g_fbx_views removeAllObjects]; } g_rt_range_active = g_rt_range_pending = 0; pg_end_render(); ts_after(); if (g_lp.count) lp_finish(); fz_end();
#ifdef NVN_RT_RANGE_TEST
    rr_finish(ranged);
#endif
    if (g_bv_npend) bv_flush();   // 塊視圖代理畫完了：把塊拷回 BC 貼圖 | block-view proxies are done: copy the blocks back into the BC textures
    AP_E(0);
} }
void pg_sub_begin_ext(uint64_t fence) { pg_sub_begin(fence); }
void pg_commit_ext(id<MTLCommandBuffer> cb) { pg_commit(cb); }
// 附件實際掛上的格式：視圖換了格式且掛得上就是視圖格式，否則底圖格式。掛附件（attach）與管線預測（pso_predict_rest）共用，口徑一致——
// 原先預測按底圖格式，經 sRGB 視圖寫的通道（馬力歐末道）預測出的管線格式不對，真管線到首次繪製才現編（取自 d2r-mobile e2a0c9a9b）
// The format an attachment is actually attached with: the view's format if the view changes the format and can be attached, otherwise the base texture's. Shared by attaching (attach) and pipeline prediction (pso_predict_rest) so both agree --
// prediction used the base format, so for a pass written through an sRGB view (Mario's last pass) the predicted pipeline had the wrong format and the real one was compiled at the first draw (from d2r-mobile e2a0c9a9b)
static MTLPixelFormat rt_view_fmt(id<MTLTexture> base, const view_t *v, int has_v) {
    if (!base || !has_v || !v->set_format) return base.pixelFormat;
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(v->format, &pf, swz) || !pf || pf == (uint32_t)base.pixelFormat) return base.pixelFormat;
    if (bv_is_block_view(base.pixelFormat, (MTLPixelFormat)pf)) return bv_proxy_fmt((MTLPixelFormat)pf);   // 畫到代理（見上） | drawn into a proxy (see above)
    if (tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) return base.pixelFormat;
    return (MTLPixelFormat)pf;
}
static void attach(MTLRenderPassAttachmentDescriptor *a, const tex_t *t, const view_t *v, int has_v) {
    id<MTLTexture> base = tex_mtl(t); a.texture = base;
    if (has_v && v->set_levels) a.level = v->base_level;
    if (has_v && v->set_layers) { if (t->d.target == 2) a.depthPlane = v->min_layer; else a.slice = v->min_layer; }
    // 換格式的視圖掛同格式的 Metal 視圖（管線格式跟著附件走）。SMBW 末道經 RGBA8 交換鏈貼圖的 RGBA8_SRGB 視圖寫：寫入時由硬件編成 sRGB；
    // 原先掛底圖，線性值原樣落進貼圖，整幅偏暗（每個值都多解了一次 sRGB，Mac 與 iPhone 都是）。視圖只活到本次提交跑完
    // A view that changes the format is attached as a Metal view of that format (pipeline formats follow the attachment). SMBW's last pass writes the RGBA8 swapchain texture through an RGBA8_SRGB view, so the hardware encodes sRGB on write;
    // attaching the base texture instead stored the linear values as they were and the whole picture came out dark (every value decoded from sRGB once more, on the Mac and the iPhone). The view lives until this submission completes
    if (base && has_v && v->set_format) {
        MTLPixelFormat pf = rt_view_fmt(base, v, has_v);
        if (pf != base.pixelFormat && bc_block_bytes(base.pixelFormat) && plain_bytes(pf)) {   // rt_view_fmt 已把塊視圖換成代理格式 | rt_view_fmt has already turned a block view into the proxy format
            uint32_t lv = v->set_levels ? v->base_level : 0, sl = v->set_layers ? v->min_layer : 0;
            int i = g_bv_npend < 8 ? bv_proxy(base, lv, sl, pf) : -1;
            if (i < 0) { a.texture = nil; RED_ONCE(69, "壓縮貼圖塊視圖的代理建不出（%lux%lu 格式 %lu）：本附件不畫", (unsigned long)base.width, (unsigned long)base.height, (unsigned long)pf); return; }
            a.texture = g_bv_proxy[i]; a.level = 0; a.slice = 0; a.depthPlane = 0; g_bv_pend[g_bv_npend++] = i;
            return;
        }
        if (pf != base.pixelFormat) {
            id<MTLTexture> vw = [base newTextureViewWithPixelFormat:pf];
            if (vw) { a.texture = vw; defer_obj_unref(vw); }
            else RED_ONCE(67, "渲染目標視圖建不出：格式 %#x（底圖 Metal 格式 %lu）", v->format, (unsigned long)base.pixelFormat);
        } else { uint32_t want = 0; uint8_t swz[4];
            if (nvn_fmt_to_mtl(v->format, &want, swz) && want && want != (uint32_t)base.pixelFormat)
                RED_ONCE(68, "渲染目標視圖格式 %#x 掛不上（底圖 Metal 格式 %lu 建時沒有換格式用途）：按底圖格式畫", v->format, (unsigned long)base.pixelFormat); }
    }
}
// mode 1（clear_only）：只為落地掛起的清除而開的通道，只掛有清除的附件；mode 2：前瞻證明本通道不用深度模板，不掛深度。
// clear_only：只為落地掛起的清除而開的通道，只掛有清除的附件——其餘附件本通道不碰，掛上去在 TBDR 上平白一讀一寫
// （Mac 選角頁：清色通道連帶掛上 400x225 深度讀入寫出）
// mode 1 (clear_only): a pass opened only to land pending clears, attaching only the attachments that have clears; mode 2: the lookahead proves this pass does not use depth-stencil, so depth is not attached.
// clear_only: a pass opened only to land pending clears, attaching only the attachments that have clears - the other attachments are not touched by this pass, and attaching them costs a pointless load and store on TBDR
// (Mac character select screen: the color-clear pass drags along a 400x225 depth load and store)
static id<MTL4RenderCommandEncoder> rp_begin_ex(int clear_only);
static id<MTL4RenderCommandEncoder> rp_begin(void) { return rp_begin_ex(0); }
static id<MTL4RenderCommandEncoder> rp_begin_ex_(int clear_only);
static id<MTL4RenderCommandEncoder> rp_begin_ex(int clear_only) { AP_B; id<MTL4RenderCommandEncoder> e = rp_begin_ex_(clear_only); AP_E(1); return e; }
static id<MTL4RenderCommandEncoder> rp_begin_ex_(int clear_only) {
    if (g_enc) return g_enc;
    memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok);
    g_enc_mode = clear_only;
    if (!R.has_rt) { RED_ONCE(3, "繪製前沒有設置渲染目標"); return nil; }
    // 附件範圍候選的版本在捕獲任何附件之前取；建立途中有變化則不續用。
    // The version of the attachment-range candidate is taken before capturing any attachment; if it changes during setup, there is no continuation.
    uint64_t reg0 = 0, ser0 = 0, dirty0 = 0;
    if (rt_range_on()) { reg0 = g_reg_gen; ser0 = g_ser_gen; }
    if (rt_range_on()) dirty0=nvn_dirty_now();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
    int any = 0;
    for (int i = 0; i < R.rt.n; i++) {
        if (!R.rt.color[i] || (clear_only == 1 && !R.clear_c[i])) continue;
        attach(pd.colorAttachments[i], R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]);
        if (!pd.colorAttachments[i].texture) continue;
        any = 1;
        pd.colorAttachments[i].loadAction = R.clear_c[i] ? MTLLoadActionClear : MTLLoadActionLoad;
        pd.colorAttachments[i].clearColor = R.cc[i];
        // 診斷 DM_NVN_CLEAR_RT=寬x高：這個尺寸的顏色附件一律清成黑色再畫（看畫面上的東西是當幀畫的，還是前幾幀留下來的）
        // Diagnostic DM_NVN_CLEAR_RT=WxH: colour attachments of this size are always cleared to black before drawing (tells what is drawn this frame from what was left by earlier frames)
        { static int cw = -1, ch; if (cw < 0) { const char *e = getenv("DM_NVN_CLEAR_RT"); if (!e || sscanf(e, "%dx%d", &cw, &ch) != 2) cw = 0; else MLOG("DM_NVN_CLEAR_RT：%dx%d 的顏色附件一律先清黑（診斷）", cw, ch); }
          id<MTLTexture> ct = pd.colorAttachments[i].texture;
          if (cw && (int)ct.width == cw && (int)ct.height == ch) { pd.colorAttachments[i].loadAction = MTLLoadActionClear; pd.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 1); } }
        int cov = tbdr_on() && !R.clear_c[i] && (g_fd_dw || g_fd_di) && fz_load_covers(i, pd.colorAttachments[i].texture, pd.colorAttachments[i].level);
        if (!R.clear_c[i] && (g_fd_dw || g_fd_di) && !cov) PD("Q %llu c%d 讀入不可省 why=%d", g_stat_pass + 1, i, g_fd_why);
        if (cov) {
            static int probe = -1; if (probe < 0) { probe = getenv("DM_NVN_LOADPROBE") ? atoi(getenv("DM_NVN_LOADPROBE")) : 0; if (probe) MLOG("DM_NVN_LOADPROBE：讀入可省的附件改清成哨兵色並回讀核對"); }
            if (probe) { pd.colorAttachments[i].loadAction = MTLLoadActionClear; int pm = probe == 1 && pf_is_float(pd.colorAttachments[i].texture.pixelFormat) ? 3 : probe;   // 浮點格式缺省用 NaN 哨兵：固定哨兵值會被真實輸出撞上（實測撞上過） | Float formats use a NaN sentinel by default: a fixed sentinel value can be hit by real output (it has been hit in practice)
                pd.colorAttachments[i].clearColor = pm == 3 ? MTLClearColorMake(NAN, NAN, NAN, NAN) : probe == 2 ? MTLClearColorMake(0.777, 0.222, 0.555, 0.888) : MTLClearColorMake(0.123, 0.456, 0.789, 0.321);
                if (!g_lp) g_lp = [NSMutableArray array];
                [g_lp addObject:@[pd.colorAttachments[i].texture, @(pd.colorAttachments[i].level), @(pd.colorAttachments[i].slice), @(g_stat_pass + 1), @(i), @(R.rt.color[i]->serial), @(pm)]]; }
            else pd.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            g_fd_n++;
        }
        xv_touch_add(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice,
                     pd.colorAttachments[i].loadAction != MTLLoadActionLoad);
        if (g_xv_n) { xv_ent *xe = xv_find(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice);
            if (xe && (xe->state == XV_INVALID || xe->state == XV_PROBE)) { if (pd.colorAttachments[i].loadAction == MTLLoadActionLoad) { xv_ent copy = *xe; xv_violation(&copy, "作附件讀入舊內容"); } else xv_good(xe); } }
        pd.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    // clear_only：0 全掛 1 只為清除 2 深度模板都不掛 3 只掛模板 4 只掛深度（3 / 4 由 pass_ds_need 前瞻證明整通道只用那一項）
    // clear_only: 0 attach all 1 only for clears 2 neither depth nor stencil attached 3 stencil only 4 depth only (3 / 4: the pass_ds_need lookahead proves the whole pass uses only that one)
    if (R.rt.depth && clear_only == 3) {   // 只掛模板：合一格式的深度平面不進 tile、不讀不寫（Metal 允許深度附件空、模板附件掛合一貼圖） | Stencil only: the depth plane of the combined format does not enter the tile and is neither read nor written (Metal allows an empty depth attachment with the combined texture on the stencil attachment)
        attach(pd.stencilAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview);
        MTLPixelFormat pf = pd.stencilAttachment.texture.pixelFormat;
        if (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8) {
            any = 1; pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
        } else pd.stencilAttachment.texture = nil;   // 沒有模板平面：模板測試恆過，什麼都不掛 | No stencil plane: the stencil test always passes, attach nothing
    } else if (R.rt.depth && clear_only != 2 && !(clear_only == 1 && !R.clear_d && !R.clear_s)) {
        attach(pd.depthAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview);
        if (pd.depthAttachment.texture) {
            any = 1;
            pd.depthAttachment.loadAction = R.clear_d ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.depthAttachment.clearDepth = R.cd; pd.depthAttachment.storeAction = MTLStoreActionStore;
            MTLPixelFormat pf = pd.depthAttachment.texture.pixelFormat;
            if (clear_only != 4 && (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8)) {   // 4：只掛深度 | 4: depth only
                pd.stencilAttachment.texture = pd.depthAttachment.texture; pd.stencilAttachment.level = pd.depthAttachment.level;
                pd.stencilAttachment.slice = pd.depthAttachment.slice;
                pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
                pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }
    }
    if (!any) { g_bv_npend = 0; RED_ONCE(4, "渲染目標都建不出 MTLTexture"); return nil; }
    for (int i = 0, k = 0; i < R.rt.n && k < g_bv_npend; i++) {   // 塊視圖代理要讀入舊內容：開通道前把 BC 塊拷進代理 | block-view proxies that load old contents: copy the BC blocks into the proxy before the pass opens
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
        if (!c.texture || !bv_is_proxy(c.texture)) continue;
        int e = g_bv_pend[k++];
        if (c.loadAction == MTLLoadActionLoad) bv_copy(e, 1);
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i] && zskip_zero_clear(i)) {
        g_zskip_cand_ok[i] = 1; g_zskip_cand[i] = (zskip_zero){R.rt.color[i]->serial, (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0), (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_layers ? R.rt.cview[i].min_layer : 0)};
    }
    // 帶寬賬：Load 的附件計讀入，全部附件計寫出（當前一律 Store）
    // Bandwidth ledger: attachments with Load count as load, all attachments count as store (currently always Store)
    int nload = 0, natt = 0;
    for (int i = 0; i < R.rt.n; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
        if (!c.texture) continue;
        uint64_t b = att_bytes(c.texture, c.level); natt++; g_bw_store += b; if (c.loadAction == MTLLoadActionLoad) { g_bw_load += b; nload++; }
        if (g_pd_tw) passdump_arm((unsigned)c.texture.width, (unsigned)c.texture.height);
        if (R.rt.has_cview[i] && R.rt.cview[i].set_format) PD("V %llu c%d 視圖格式 %#x（基貼圖 NVN 格式 %#x）", g_stat_pass + 1, i, R.rt.cview[i].format, R.rt.color[i]->d.format);
        PD("P %llu c%d ser=%llu fmt=%lu %lux%lu ms%lu lv%lu sl%lu %s", g_stat_pass + 1, i, R.rt.color[i]->serial, (unsigned long)c.texture.pixelFormat,
           (unsigned long)c.texture.width, (unsigned long)c.texture.height, (unsigned long)c.texture.sampleCount, (unsigned long)c.level, (unsigned long)c.slice,
           c.loadAction == MTLLoadActionClear ? "clear" : c.loadAction == MTLLoadActionDontCare ? "dontcare" : "load");
    }
    {   // 深度與模板平面分開記（合一格式 5 字節 = 深度 4 + 模板 1；只掛一項時只記那一項） | Depth and stencil planes are counted separately (combined format 5 bytes = depth 4 + stencil 1; when only one is attached only that one is counted)
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t whole = att_bytes(dt ? dt : stt, dt ? pd.depthAttachment.level : pd.stencilAttachment.level);
        int comb = (dt ? dt : stt).pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        uint64_t db = dt ? (comb ? whole / 5 * 4 : whole) : 0, sb = stt && stt != dt ? (comb ? whole / 5 : whole) : stt ? whole - db : 0;
        if (dt) { natt++; g_bw_store += db; if (pd.depthAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += db; nload++; } }
        if (stt) { natt += !dt; g_bw_store += sb; if (pd.stencilAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += sb; nload += !dt; } }
        if (dt) PD("P %llu d ser=%llu fmt=%lu %lux%lu %s lv%lu sl%lu 視圖(層 %d+%d 級 %d+%d %s) 貼圖 %ux%ux%u 級%u 目標%d", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)dt.pixelFormat,
                   (unsigned long)dt.width, (unsigned long)dt.height, pd.depthAttachment.loadAction == MTLLoadActionClear ? "clear" : "load",
                   (unsigned long)pd.depthAttachment.level, (unsigned long)pd.depthAttachment.slice, R.rt.dview.min_layer, R.rt.dview.nlayers, R.rt.dview.base_level, R.rt.dview.nlevels,
                   R.rt.has_dview ? "有" : "無", R.rt.depth->d.width, R.rt.depth->d.height, R.rt.depth->d.depth, R.rt.depth->d.levels, R.rt.depth->d.target);
        else if (stt) PD("P %llu s ser=%llu fmt=%lu %lux%lu %s（只掛模板）", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)stt.pixelFormat,
                   (unsigned long)stt.width, (unsigned long)stt.height, pd.stencilAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
    }
    if (natt && nload == natt) g_pass_full_rw++;
    g_nsd = 0;
    {
        for (int i = 0; i < R.rt.n && i < 8; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i]; if (!c.texture || bv_is_proxy(c.texture)) continue;   // 代理一律存：通道後要拷回 | proxies are always stored: they are copied back after the pass
            g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.color[i]->serial; g_sd[g_nsd].bytes = att_bytes(c.texture, c.level); g_sd[g_nsd].lv = (uint32_t)c.level; g_sd[g_nsd].sl = (uint32_t)(c.texture.textureType == MTLTextureType3D ? c.depthPlane : c.slice); g_sd[g_nsd].idx = i; g_sd[g_nsd].load = c.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_c[i] != 0; g_nsd++;
            c.storeAction = MTLStoreActionUnknown; }
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t db = att_bytes(dt, pd.depthAttachment.level), sb = 0;
        if (stt) { int comb = stt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8; sb = att_bytes(stt, pd.stencilAttachment.level) / (comb ? 5 : 1); if (stt == dt) db -= sb; }   // 合一格式 5 字節 = 深度 4 + 模板 1（只掛模板時也只記 1） | Combined format 5 bytes = depth 4 + stencil 1 (when only stencil is attached, only 1 is counted)
        if (dt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = db; g_sd[g_nsd].lv = (uint32_t)pd.depthAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.depthAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 1; g_sd[g_nsd].load = pd.depthAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_d != 0; g_nsd++;
                  pd.depthAttachment.storeAction = MTLStoreActionUnknown; }
        if (stt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = sb; g_sd[g_nsd].lv = (uint32_t)pd.stencilAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.stencilAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 2; g_sd[g_nsd].load = pd.stencilAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_s != 0; g_nsd++;
                   pd.stencilAttachment.storeAction = MTLStoreActionUnknown; }
    }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    {   // 通道信息（口徑同原先每次繪製現取：顏色取各附件基貼圖格式，採樣數取最後一個顏色附件、有深度則取深度） | Pass info (same convention as the former per-draw fetch: colors take each attachment's base texture format; sample count takes the last color attachment, or depth if there is depth)
        uint64_t ser = g_pass.serial + 1; memset(&g_pass, 0, sizeof g_pass); g_pass.serial = ser;
        for (int i = 0; i < R.rt.n && i < 8; i++) { id<MTLTexture> t = R.rt.color[i] ? pd.colorAttachments[i].texture : nil;
            if (t) { g_pass.cfmt[i] = (uint32_t)t.pixelFormat; g_pass.samples = (uint32_t)t.sampleCount; } }
        id<MTLTexture> dt = R.rt.depth ? pd.depthAttachment.texture : nil, stt = R.rt.depth ? pd.stencilAttachment.texture : nil;   // 深度 / 模板附件分開取（可能只掛其一） | Depth / stencil attachments are fetched separately (possibly only one of them is attached)
        g_pass.hd = dt && dt.pixelFormat != MTLPixelFormatStencil8; g_pass.hs = stt != nil;
        g_pass.dfmt = g_pass.hd ? (uint32_t)dt.pixelFormat : 0; g_pass.sfmt = stt ? (uint32_t)stt.pixelFormat : 0;
        if (dt || stt) g_pass.samples = (uint32_t)(dt ? dt : stt).sampleCount;
        pass_dims(pd, (NSUInteger)R.rt.n, &g_pass.tw, &g_pass.th);
        if (rt_range_on()) { g_pass.reg_gen = reg0; g_pass.ser_gen = ser0; }
        if (rt_range_on()) { g_pass.range_dirty=dirty0; g_pass.range_color=pd.colorAttachments[0].texture; g_pass.range_depth=pd.depthAttachment.texture; }
        g_enc_pso = nil; g_enc_dss = nil; g_enc_clamp = 0;
    }
    if (g_vis_on) {
        // 槽用滿（一次提交超過 VIS_SLOTS 個計數通道）：另建溢出緩衝接著用。環裡的緩衝不能退役（還要輪著用，退役會把它移出駐留集），只退上一塊溢出緩衝
        // Slots used up (one submission exceeds VIS_SLOTS counting passes): continue in a new overflow buffer. A ring buffer must not be retired (it is still used in rotation, and retiring removes it from the residency set); only a previous overflow buffer is
        if (!g_vis || g_vis_n >= VIS_SLOTS) { if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis); g_vis = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis); g_vis_n = 0; }
        pd.visibilityResultBuffer = g_vis;
    }
    g_enc_vis = 0; g_enc_has_vis = g_vis_on;
    pg_render(pd, (NSUInteger)R.rt.n);
    { void td_note(id pd, NSUInteger nc); td_note(pd, (NSUInteger)R.rt.n);
      void ts_note(id pd, NSUInteger nc); ts_note(pd, (NSUInteger)R.rt.n); }
    ser_gate();
    g_enc = enc_r_begin(pd);
    args_dirty_all();
    g_stat_pass++;
    if (enc_label_on()) { id<MTLTexture> lt = pd.colorAttachments[0].texture ?: pd.depthAttachment.texture ?: pd.stencilAttachment.texture; g_el_w = (int)lt.width; g_el_h = (int)lt.height; g_el_draws = 0; g_el_key[0] = 0; }
    return g_enc;
}
// 開通道前的空剪裁預檢查：第一個有效目標的選中級尺寸是實際pass最小邊界的上界。
// Empty-scissor precheck before opening a pass: the size of the selected level of the first valid target is an upper bound on the minimum bounds of the actual pass.
static void rt_dims(NSUInteger *tw, NSUInteger *th) {
    id<MTLTexture> t0 = nil; NSUInteger level = 0;
    for (int i = 0; i < R.rt.n && i < 8 && !t0; i++) if (R.rt.color[i]) {
        t0 = tex_mtl(R.rt.color[i]); level = R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0;
    }
    if (!t0 && R.rt.depth) { t0 = tex_mtl(R.rt.depth); level = R.rt.has_dview && R.rt.dview.set_levels ? R.rt.dview.base_level : 0; }
    *tw = t0 ? mip_extent(t0.width, level) : 1; *th = t0 ? mip_extent(t0.height, level) : 1;
}
// 有掛起的清除但通道裡沒有繪製：開一次通道把清除落地
// There are pending clears but no draw in the pass: open a pass once to land the clears
static void rp_flush(void) {
    int pend = R.clear_d || R.clear_s;
    for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
    if (pend && !g_enc && g_cb) rp_begin_ex(1);
    rp_end();
}
static int scissor_full(const tex_t *t, const view_t *v, int has_v) {
    NSUInteger level = has_v && v->set_levels ? v->base_level : 0;
    NSUInteger w = mip_extent(t->d.width, level), h = mip_extent(t->d.target == 0 || t->d.target == 3 ? 1 : t->d.height, level);
    return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h);
}
// 剪裁清零（見 OP_CLEAR_COLOR）：先落地掛起的清除，再在回放流的拷貝編碼器裡從全零緩衝拷進附件那一級 / 層的剪裁矩形。多重採樣、3D、深度 / 壓縮格式不做（返回 0 走原路）
// Scissored zero clear (see OP_CLEAR_COLOR): land pending clears first, then copy from the zero buffer into the scissor rectangle of the attachment's level / slice in the replay stream's copy encoder. Multisample, 3D, depth / compressed formats are not handled (returns 0 and takes the old path)
static int clear_rect_zero(int i) {
    const tex_t *t = R.rt.color[i]; id<MTLTexture> mt = tex_mtl(t); if (!mt) return 0;
    MTLTextureType tt = mt.textureType;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray || tt == MTLTextureType3D || tt == MTLTextureTypeTextureBuffer) return 0;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat); if (!bpp) return 0;
    const view_t *v = &R.rt.cview[i]; int hv = R.rt.has_cview[i];
    NSUInteger lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0;
    long lw = (long)MAX(mt.width >> lv, 1u), lh = (long)MAX(mt.height >> lv, 1u);
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > lw) x1 = lw; if (y1 > lh) y1 = lh;
    if (x1 <= x0 || y1 <= y0) return 1;
    size_t row = (size_t)(x1 - x0) * bpp, img = row * (size_t)(y1 - y0);
    id<MTLBuffer> zb = zero_buf(img); if (!zb) return 0;
    rp_flush();   // 同一附件上掛起的整面清除要先落地，否則會蓋掉這次 | a pending full clear on the same attachment must land first, or it would overwrite this one
    enc_b_copy_buffer_texture(blit(), zb, 0, row, img, MTLSizeMake((NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0), 1), mt, sl, lv, MTLOriginMake((NSUInteger)x0, (NSUInteger)y0, 0));
    static uint64_t n; if (!n++) MLOG("剪裁清零：首例 %ldx%ld @%ld,%ld（目標 %lux%lu 級 %lu）", x1 - x0, y1 - y0, x0, y0, (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)lv);
    return 1;
}


// ================= 著色器 / 描述符池 / 管線 / 繪製 =================
// ================= Shaders / descriptor pool / pipelines / draws =================
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
// ---------- 著色器映射表：Switch 控制段哈希 → PC metallib（dm/gfx/tools/shader_map.py 產出） ----------
// ---------- Shader mapping table: Switch control section hash → PC metallib (produced by dm/gfx/tools/shader_map.py) ----------
static NSMutableDictionary<NSString *, NSArray *> *g_shmap;
static NSString *g_shmap_path, *g_msl_dir;
static NSString *exe_rel(const char *rel) {
    char buf[4096]; uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n)) return nil;
    char *dir = dirname(buf);
    return [NSString stringWithFormat:@"%s/%s", dir, rel];
}
// 著色器映射表（tools/shader_map.py 出）：運行時只用「Switch 控制段鍵 → 階段」；著色器一律是 Switch 原版翻譯（tools/dmsh_build.py 離線出 <鍵>_b.metallib + 反射）
// Shader mapping table (output of tools/shader_map.py): at runtime only 'Switch control section key → stage' is used; shaders are always translations of the Switch originals (tools/dmsh_build.py produces <key>_b.metallib + reflection offline)
static void shmap_load(void) {
    if (g_shmap) return;
    const char *e = getenv("DM_NVN_SHMAP");
    NSString *path = e ? @(e) : exe_rel("../games/" DM_GAME_ID "/shader_map.tsv");   // 缺省：當前遊戲檔案目錄（Mac：dm/games/<名>；iOS 由 main.m 設 DM_NVN_SHMAP） | default: the current game profile folder (Mac: dm/games/<id>; on iOS main.m sets DM_NVN_SHMAP)
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!txt) MDIE("讀不到著色器映射表 %s（DM_NVN_SHMAP 可指定）", path.UTF8String);
    g_shmap = [NSMutableDictionary dictionary];
    for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) {
        if (!ln.length || [ln hasPrefix:@"#"] || [ln hasPrefix:@"package"]) continue;
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count > 3) g_shmap[c[3]] = c[2];
    }
    g_shmap_path = path;
    const char *m = getenv("DM_MSL_DIR");
    // 庫目錄 = <DM_MSL_DIR 或缺省緩存>/b-<翻譯鏈版本>：版本取自遊戲檔案的 dmsh_chain.h（tools/dmsh_chain.py），舊源碼樹編的宿主只認舊版本的庫
    // Library directory = <DM_MSL_DIR or default cache>/b-<translation chain version>: the version comes from the game profile's dmsh_chain.h (tools/dmsh_chain.py); a host built from an old source tree only accepts libraries of the old version
    g_msl_dir = [NSString stringWithFormat:@"%@/b-%s", m ? @(m) : [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work"], DMSH_CHAIN];
    if (![NSFileManager.defaultManager fileExistsAtPath:g_msl_dir]) MLOG("【紅】著色器庫目錄 %s 不存在：本源碼樹的翻譯鏈版本 %s 還沒出包（dm/gfx/tools/dmsh_build.py）", g_msl_dir.UTF8String, DMSH_CHAIN);
    NSString *cp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"c1_full.tsv"];   // 與映射表同目錄（Mac：dm/games/<名>；iOS：App 內 dm/，build_ipa.sh 一起拷） | Same directory as the mapping table (Mac: dm/games/<id>; iOS: dm/ inside the App, copied along by build_ipa.sh)
    NSString *ct = cp ? [NSString stringWithContentsOfFile:cp encoding:NSUTF8StringEncoding error:nil] : nil;
    NSString *head = ct ? [[ct componentsSeparatedByString:@"\n"] firstObject] : nil;
    NSString *want = [NSString stringWithFormat:@"# chain b-%s", DMSH_CHAIN];
    if (!head || ![head hasPrefix:want]) {
        MLOG("【紅】c1_full.tsv 清單過期或缺失，全部 c1 獨佔 64 KiB");
    } else {
        g_c1_full_valid = 1; g_c1_full = [NSMutableSet set];
        for (NSString *line in [ct componentsSeparatedByString:@"\n"]) if (line.length && ![line hasPrefix:@"#"]) [g_c1_full addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    }
    MLOG("著色器路線 B（Switch 原版翻譯）：映射表 %lu 條（%s），metallib 目錄 %s", (unsigned long)g_shmap.count, path.UTF8String, g_msl_dir.UTF8String);
    warm_start();
}

@interface NvShader : NSObject { @public id<MTLFunction> fn; nvn_refl rf; id<MTLArgumentEncoder> ae; NSUInteger arg_len; int stage, range_ro; const char *builtin; uint64_t logged; char key[24]; id<MTLBuffer> c1buf; uint64_t c1_gpu; size_t c1off;
    // 上次編好的參數緩衝
    // The argument buffer encoded last time
    id<MTLBuffer> ab_buf; uint64_t ab_gpu; size_t ab_off; uint64_t ab_pass;   // ab_pass：編這塊參數的渲染通道序號——只在同一通道內復用（跨通道 / 跨提交的塊所在 arena 可能已回收重寫） | ab_pass: serial of the render pass that encoded this argument block - reused only within the same pass (the arena holding a block from another pass / another submission may already have been reclaimed and rewritten)
    id<MTLLibrary> bflib; int bflib_tried; NSMutableDictionary<NSString *, id<MTLFunction>> *bffn; } @end   // bflib：可讀 tile 版本 <鍵>_bf（合併 B 第 2 步） | bflib: readable-tile version <key>_bf (merge B step 2)
@implementation NvShader @end
#include "nvn_pso_skey.h"
@interface NvProg : NSObject { @public NvShader *sh[6]; } @end
@implementation NvProg @end
// 預熱項：後台建好、等提交線程領用。vs / fs 強引用（pso 裡只是裸指針，後台字典隨任務結束釋放）
// Warm-up entry: built in the background, waiting to be taken by the submit thread. vs / fs are strong references (in pso they are only raw pointers, and the background dictionary is released when the task ends)
@interface WarmItem : NSObject { @public int kind; NSString *key, *vs_key, *fs_key; NvShader *shader, *vs, *fs; pso_key pso; id<MTLFunction> fsfn; id object; } @end
@implementation WarmItem @end
static NSMutableDictionary<NSString *, NvShader *> *g_shcache;

static void c1_store(NvShader *o) {
    size_t len = o->rf.c1_len;
    NSString *guardKey = [NSString stringWithUTF8String:o->key];
    if ([guardKey hasSuffix:@"_b"]) guardKey = [guardKey substringToIndex:guardKey.length - 2];
    os_unfair_lock_lock(&g_c1_lock);   // 預熱線程與提交線程都會載入著色器 | Both the warm-up thread and the submit thread load shaders
    if (!g_c1_full_valid || [g_c1_full containsObject:guardKey]) {   // 運行時下標未證有界：獨佔 64 KiB 全零（同原實現） | Runtime index not proven bounded: exclusive 64 KiB of all zeros (same as the original implementation)
        o->c1buf = [g_dev newBufferWithLength:C1_DECL_SIZE options:MTLResourceStorageModeShared]; o->c1_gpu = o->c1buf.gpuAddress;
        if (!o->c1buf) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 獨佔緩衝分配失敗"); }
        res_add(o->c1buf);
        memset(o->c1buf.contents, 0, C1_DECL_SIZE); memcpy(o->c1buf.contents, o->rf.c1, len);
        g_c1_alloc_bytes += C1_DECL_SIZE; g_c1_data_bytes += len; g_c1_count++; g_c1_exclusive++;
        os_unfair_lock_unlock(&g_c1_lock);
        return;
    }
    size_t need = (len + 255u) & ~((size_t)255u);
    if (!g_c1_banks) g_c1_banks = [NSMutableArray array];
    if (!g_c1_banks.count || g_c1_bank_off + need + 256u + C1_DECL_SIZE > C1_BANK_SIZE) {
        id<MTLBuffer> b = [g_dev newBufferWithLength:C1_BANK_SIZE options:MTLResourceStorageModeShared];
        if (!b) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 緩衝塊分配失敗"); }
        res_add(b);
        memset(b.contents, 0, C1_BANK_SIZE);
        [g_c1_banks addObject:b];
        g_c1_bank_off = 0;
        g_c1_alloc_bytes += C1_BANK_SIZE;
    }
    id<MTLBuffer> b = g_c1_banks.lastObject;
    size_t off = g_c1_bank_off;
    memcpy((uint8_t *)b.contents + off, o->rf.c1, len);
    g_c1_bank_off += need + 256u;
    g_c1_data_bytes += len;
    g_c1_count++;
    o->c1buf = b;
    o->c1_gpu = b.gpuAddress;
    o->c1off = off;
    os_unfair_lock_unlock(&g_c1_lock);
}

static NSMutableArray<WarmItem *> *g_warm_items;
static os_unfair_lock g_warm_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_warm_ready;
static int g_warm_started;
static uint64_t g_warm_t0;
static _Atomic int g_warm_running;   // 後台預熱還沒跑完（進局讀取畫面據此多等一會兒，見 mtl_warm_wait） | Background warm-up has not finished yet (the loading screen on entering a run waits a little longer based on this, see mtl_warm_wait)
static uint64_t g_warm_shaders, g_warm_rps, g_warm_cps, g_warm_skip_fz;
static uint64_t g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps;
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap);
static int rt_range_bindings_ro(NSDictionary *j);
static FILE *g_pso_record; static int g_pso_record_checked;
static FILE *pso_record_file(void) {
    if (g_pso_record_checked) return g_pso_record;
    g_pso_record_checked = 1;
    const char *path = getenv("DM_PSO_RECORD");
    if (path && *path) g_pso_record = fopen(path, "a");
    return g_pso_record;
}
static void pso_record(const char *fmt, ...) {
    FILE *f = pso_record_file();
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fflush(f);
}
// 用戶清單：本機運行中提交線程同步新建的著色器 / 管線，下次啓動與種子清單一起預熱。
// 路徑 = DM_PSO_USER，或存檔目錄（DM_SAVE，iOS 為 Documents/save）的父目錄下 pso_user.tsv；測試每局用新存檔副本，互不污染
// User manifest: shaders / pipelines created synchronously on the submit thread while running on this machine, warmed up at the next launch together with the seed manifest.
// Path = DM_PSO_USER, or pso_user.tsv under the parent of the save directory (DM_SAVE, Documents/save on iOS); tests use a fresh save copy per run, so they do not pollute each other
static NSString *pso_user_path(void) {
    const char *u = getenv("DM_PSO_USER"), *save = getenv("DM_SAVE");
    if (u && *u) return @(u);
    return save && *save ? [[@(save) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pso_user.tsv"] : nil;
}
static FILE *g_pso_user; static int g_pso_user_checked;
static NSMutableSet<NSString *> *g_pso_user_seen;
static FILE *pso_user_file(void) {
    if (g_pso_user_checked) return g_pso_user;
    g_pso_user_checked = 1;
    NSString *path = pso_user_path();
    if (path) g_pso_user = fopen(path.fileSystemRepresentation, "a");
    return g_pso_user;
}
static void pso_user_record(const char *fmt, ...) {
    FILE *f = pso_user_file();
    if (!f) return;
    char line[8192]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (!g_pso_user_seen) {
        g_pso_user_seen = [NSMutableSet set];
        NSString *seed = g_shmap_path ? [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"] : nil;
        NSMutableArray<NSString *> *paths = [NSMutableArray array]; if (seed) [paths addObject:seed]; NSString *user = pso_user_path(); if (user) [paths addObject:user];
        for (NSString *path in paths) { NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            for (NSString *old in [txt componentsSeparatedByString:@"\n"]) if (old.length && ![old hasPrefix:@"#"]) [g_pso_user_seen addObject:old]; }
    }
    size_t ll = strlen(line); NSString *s = [[NSString alloc] initWithBytes:line length:ll && line[ll - 1] == '\n' ? ll - 1 : ll encoding:NSUTF8StringEncoding];   // 與清單文件按行（不含換行）比 | Compared with the manifest file line by line (without the newline)
    if (!s || [g_pso_user_seen containsObject:s]) return;
    [g_pso_user_seen addObject:s]; fputs(line, f); fflush(f);
}
// 從 .metallib 文件建庫，不常駐佔文件描述符：newLibraryWithURL 在庫對象活著期間一直開著那個文件（每個遊戲著色器一個——iOS 包進局常駐 258 個描述符裡 213 個是它，
// 而 iOS 進程缺省軟上限 256；別的幕新建著色器還會漲）。這裡把文件映射進內存後立刻關掉，用內存數據建庫；映射隨庫的數據對象釋放時解除
// Build the library from a .metallib file without permanently occupying a file descriptor: newLibraryWithURL keeps that file open for as long as the library object lives (one per game shader - of the 258 descriptors the iOS app permanently holds in a run, 213 are these,
// while the default soft limit of an iOS process is 256; new shaders created in other acts raise it further). Here the file is mapped into memory and closed immediately, and the library is built from the in-memory data; the mapping is released when the library's data object is freed
static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) { if (fd >= 0) close(fd); if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"打不開 %@", path]}]; return nil; }
    size_t len = (size_t)st.st_size; void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"映射失敗 %@", path]}]; return nil; }
#ifdef DM_SHELL
    // 殼：使用者的電腦上沒有蘋果的著色器編譯器時（tools/d2rm_pack.py 的不調編譯器模式，gfx/tools/nometal_run.py），「庫文件」裡放的是
    // 著色器源碼而不是編好的庫（編好的庫以 MTLB 開頭）。這裡就地現編；系統會把編好的結果緩存，同一個著色器第二次啓動起不再花這個時間。
    // 真機實測（冷）：平均約 80 毫秒一個、單個最長約 450 毫秒；一局用到幾百個。
    // Shell: when the user's computer has no Apple shader compiler (the no-compiler mode of tools/d2rm_pack.py, gfx/tools/nometal_run.py), the "library files" contain
    // shader source rather than compiled libraries (a compiled library starts with MTLB). They are compiled here on the spot; the system caches the compiled result, and from the second launch on the same shader no longer costs this time.
    // Measured on a real device (cold): about 80 ms each on average, about 450 ms for the longest single one; one session uses several hundred.
    if (len < 4 || memcmp(m, "MTLB", 4)) { NSString *src = [[NSString alloc] initWithBytes:m length:len encoding:NSUTF8StringEncoding]; munmap(m, len);
        if (!src) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:EILSEQ userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"既不是編好的庫也不是源碼 %@", path]}]; return nil; }
        static _Atomic int n; static _Atomic long long us; CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        id<MTLLibrary> l = [g_dev newLibraryWithSource:src options:nil error:err]; long long dt = (long long)((CFAbsoluteTimeGetCurrent() - t0) * 1e6);
        int k = atomic_fetch_add(&n, 1) + 1; long long tot = atomic_fetch_add(&us, dt) + dt;
        if (k == 1 || k % 100 == 0 || dt >= 300000) MLOG("著色器現編：第 %d 個 %.0f ms，累計 %.1f 秒", k, dt / 1e3, tot / 1e6);
        return l; }
#endif
    dispatch_data_t d = dispatch_data_create(m, len, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(m, len); });
    return [g_dev newLibraryWithData:d error:err];
}
static NvShader *shader_load(NSString *pc, int stage) {
    NSError *e = nil;
    NSString *lp = [NSString stringWithFormat:@"%@/%@.metallib", g_msl_dir, pc];
    id<MTLLibrary> lib = lib_from_file(lp, &e);
    NSData *jd = [NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%@/%@.json", g_msl_dir, pc]];
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (!lib || !j) { MLOG("【紅】加載著色器失敗 %s：%s", pc.UTF8String, e ? e.localizedDescription.UTF8String : "無反射 json"); return nil; }
    if (![j[@"chain"] isEqual:@DMSH_CHAIN]) { MLOG("【紅】著色器 %s 的翻譯鏈版本 %s 與本宿主期望的 %s 不符", pc.UTF8String, [[j[@"chain"] description] UTF8String], DMSH_CHAIN); return nil; }
    NvShader *o = [NvShader new];
    char err[160];
    if (nvn_refl_from_json(j, &o->rf, err, sizeof err)) { MLOG("【紅】著色器 %s 反射轉換失敗：%s", pc.UTF8String, err); return nil; }
    if (rt_range_on()) o->range_ro=rt_range_bindings_ro(j);
    o->fn = [lib newFunctionWithName:j[@"entry"]]; o->stage = stage; strlcpy(o->key, pc.UTF8String, sizeof o->key);
    if (o->rf.nb) {
        o->ae = [o->fn newArgumentEncoderWithBufferIndex:o->rf.abi];
        int end = 0;   // 末槽之後：數組綁定佔 aid … aid + cnt − 1 | one past the last slot: an array binding occupies aid … aid + cnt − 1
        for (int i = 0; i < o->rf.nb; i++) end = MAX(end, o->rf.b[i].aid + o->rf.b[i].cnt);
        o->arg_len = o->ae.encodedLength;
        if (o->arg_len != (NSUInteger)(8 * end) || (o->arg_len & 7))
            MDIE("著色器 %s 參數塊佈局異常：encodedLength=%lu 末槽 %d", o->key, (unsigned long)o->arg_len, end);
    }
    if (o->rf.has_c1) c1_store(o);
    return o;
}

static int stage_of(NSString *zh) {
    if ([zh isEqualToString:@"頂點"]) return 0; if ([zh isEqualToString:@"像素"]) return 1; if ([zh isEqualToString:@"計算"]) return 5;
    if ([zh isEqualToString:@"外殼"]) return 3; if ([zh isEqualToString:@"域"]) return 4; return -1;
}
#define NVN_RT_RANGE_RO_IMPL 1
// 私有續用只接受明確RO的B反射，sbuf缺rw不把默認0當證明；不改變既有反射/翻譯鏈。
// Private continuation accepts only B reflection that is explicitly RO; a missing rw on sbuf does not let the default 0 count as proof; does not change the existing reflection / translation chain.
static int rt_range_bindings_ro(NSDictionary *j) {
    if (![j[@"route"] isEqual:@"B"] || ![j[@"bindings"] isKindOfClass:[NSArray class]] ||
        ![j[@"drv_reads"] isKindOfClass:[NSDictionary class]] || [j[@"drv_reads"] count]) return 0;
    for (NSDictionary *b in j[@"bindings"]) {
        if (![b isKindOfClass:[NSDictionary class]] || ![b[@"kind"] isKindOfClass:[NSString class]] || ![b[@"msl_type"] isKindOfClass:[NSString class]]) return 0;
        NSString *k=b[@"kind"], *t=b[@"msl_type"];
        if (b[@"rw"] && (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue])) return 0;
        if ([k isEqual:@"cbuf"] || [k isEqual:@"c1"]) { if (![t hasPrefix:@"constant "]) return 0; }
        else if ([k isEqual:@"sbuf"]) { if (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue] || ![t hasPrefix:@"const device "]) return 0; }
        else if ([k isEqual:@"samp"] || [k isEqual:@"sampt"]) { if (![t isEqual:@"sampler"]) return 0; }
        else if ([k isEqual:@"tex"] || [k isEqual:@"tbuf"]) {
            if (![t hasPrefix:@"texture"] || ([t containsString:@"access::"] && ![t containsString:@"access::sample>"] && ![t containsString:@"access::read>"])) return 0; }
        else return 0;
    }
    return 1;
}
static int rt_range_pr_ro(NvProg *p) {
    if (!p || !p->sh[0] || !p->sh[1]) return 0;
    for (int k=0;k<6;k++) if (p->sh[k] && (k>1 || p->sh[k]->stage != k || !p->sh[k]->range_ro || p->sh[k]->rf.drv_read[0] || p->sh[k]->rf.drv_read[2])) return 0;
    return 1;
}
static NvShader *shader_get(NSString *pc, int stage) {
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    NvShader *o = g_shcache[pc];
    if (o) return o;
    uint64_t bt = bp_now(); o = shader_load(pc, stage);
    if (!o) return nil;
    g_shcache[pc] = o;
    pso_record("S\t%s\t%d\n", pc.UTF8String, stage);
    pso_user_record("S\t%s\t%d\n", pc.UTF8String, stage);
    g_bp_lib_n++; g_bp_lib_ns += bp_now() - bt;
    return o;
}
static id<MTLLibrary> g_builtin_lib;
static NSMutableDictionary<NSString *, NvShader *> *g_builtin;
static NvShader *builtin_get(const char *ctl) {
    for (unsigned i = 0; i < sizeof k_nvn_builtin_map / sizeof *k_nvn_builtin_map; i++) {
        if (strcmp(ctl, k_nvn_builtin_map[i].ctl)) continue;
        if (!g_builtin) g_builtin = [NSMutableDictionary dictionary];
        NvShader *o = g_builtin[@(ctl)];
        if (o) return o;
        if (!g_builtin_lib) {
            NSError *e = nil; g_builtin_lib = [g_dev newLibraryWithSource:@(k_nvn_builtin_msl) options:nil error:&e];
            if (!g_builtin_lib) MDIE("內嵌計算核 MSL 編譯失敗：%s", e.localizedDescription.UTF8String);
        }
        o = [NvShader new]; o->fn = [g_builtin_lib newFunctionWithName:@(k_nvn_builtin_map[i].fn)]; o->stage = 5; o->builtin = k_nvn_builtin_map[i].fn;
        g_builtin[@(ctl)] = o;
        return o;
    }
    return nil;
}
static __unsafe_unretained NvProg *prog_get(const prog_t *cp) {   // 只在提交線程調用 | Called only on the submit thread
    prog_t *p = (prog_t *)cp;
    static const prog_t *lp; static uint64_t lser, lgen; static NvProg *lpr;   // 上次結果：同一對象、同一序號且序號表未變即直接用 | Last result: same object, same serial and unchanged serial table means it is used directly
    uint64_t gen = g_ser_gen;
    if (p == lp && p->serial == lser && gen == lgen && lpr) return lpr;
    __unsafe_unretained NvProg *have = p->serial ? serial_get(p->serial) : nil;
    if (have) { lp = p; lser = p->serial; lgen = gen; lpr = have; return have; }
    shmap_load();
    NvProg *o = [NvProg new];
    for (unsigned i = 0; i < p->nstage; i++) {
        // 鍵 = sha256(控制段前 0x880 字節 + 代碼區)：馬力歐大量著色器控制段相同、代碼不同（dm/gfx/tools/shader_split.py）。SetShaders 時已算好（hle_nvn.c）：這時控制段可能已被遊戲釋放
        // Key = sha256(first 0x880 bytes of the control section + code area): many of Mario's shaders share a control section but differ in code (dm/gfx/tools/shader_split.py). It was computed at SetShaders (hle_nvn.c): by now the game may have freed the control section
        uint8_t md[8]; prog_key(p, i, md);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *zh = g_shmap[k];
        if (!zh) {   // 不在包裡：查 main 本體內嵌的計算核（nvn_builtin.h） | Not in the package: look up the compute kernels embedded in the main body (nvn_builtin.h)
            NvShader *b = builtin_get(k.UTF8String);
            if (b) { o->sh[5] = b; continue; }
            MLOG("【紅】著色器 %s 不在映射表裡（DM_NVN_SHDUMP=目錄 可把控制段與代碼區存盤）", k.UTF8String);
            // DM_NVN_SHDUMP=目錄：不在表裡的著色器存成 <鍵>.ctl（控制段 0x880 字節）與 <鍵>.code（代碼區，長度取控制段 +0x704），供離線查來源 / 補進映射表
            // DM_NVN_SHDUMP=directory: shaders missing from the map are saved as <key>.ctl (0x880-byte control section) and <key>.code (code area, length from control +0x704), to trace their source / add them to the map offline
            static const char *shd; static int shd_init; if (!shd_init) { shd = getenv("DM_NVN_SHDUMP"); shd_init = 1; }
            if (shd) { char path[1024]; FILE *f; uint32_t ncode; memcpy(&ncode, (const uint8_t *)p->sd[i].control + 0x704, 4);   // 設了 SHDUMP 時控制段是宿主留的副本 | with SHDUMP set the control section is the host's copy
                snprintf(path, sizeof path, "%s/%s.ctl", shd, k.UTF8String); if ((f = fopen(path, "wb"))) { fwrite(p->sd[i].control, 1, 0x880, f); fclose(f); }
                snprintf(path, sizeof path, "%s/%s.code", shd, k.UTF8String);
                if (p->sd[i].data && ncode && ncode < (1u << 24) && (f = fopen(path, "wb"))) { fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f); } }
            continue;
        }
        int st = stage_of(zh);
        NvShader *sh = st >= 0 ? shader_get([k stringByAppendingString:@"_b"], st) : nil;
        if (sh) o->sh[st] = sh; else MLOG("【紅】著色器 %s（%s）加載失敗：先跑 dm/gfx/tools/dmsh_build.py 出包", k.UTF8String, zh.UTF8String);
    }
    if (p->serial) serial_put(p->serial, o);
    return o;
}
void mtl_prog_release(prog_t *p) { if (p->serial) serial_del(p->serial); }

// ---------- 描述符池：貼圖號 / 採樣器號 → 對象 ----------
// 登記時快照整個貼圖對象（NVN 語義：描述符已寫進池）；view：該號對應的 MTLTexture（視圖或基貼圖，CFBridgingRetain 持有）及取得時的序號表 / 客體寫代號
// ---------- Descriptor pool: texture ID / sampler ID → object ----------
// At registration the whole texture object is snapshotted (NVN semantics: the descriptor has been written into the pool); view: the MTLTexture for that ID (a view or the base texture, held via CFBridgingRetain) and the serial-table / guest-write generations at the time it was obtained
typedef struct { tex_t t; int ok; view_t v; int has_v; void *view; uint64_t view_id, view_rid, view_ser, view_dirty;
                 void *tview, *tview_src; uint64_t tview_rid; uint32_t tview_type; } treg_t;   // tview：按著色器聲明類型改建的視圖（tex_type_fix）| tview: the view re-made for the type the shader declares (tex_type_fix)
static treg_t *g_treg; static int g_ntreg;
// 貼圖號 → 序號的無鎖副本（回放熱路徑只要序號：寫回前瞻索引、純清除前提的綁定集合）：貼圖號 20 位，固定 1M 項（BSS，只佔碰過的頁），
// 登記時 release 寫、讀端 relaxed 原子讀，不進 g_rmu（原 h_ser 每次查表加解鎖，營地每幀數千次）
// Lock-free copy of texture ID → serial (the replay hot path only needs the serial: store lookahead index, binding set of the clear-only hoist): texture IDs are 20 bits, fixed 1M entries (BSS, only touched pages are used),
// release write at registration, relaxed atomic read on the reader side, without entering g_rmu (the former h_ser locked and unlocked on every lookup, thousands of times per frame in the camp)
static _Atomic uint64_t g_tser[1 << 20];
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static void **g_sview; static uint64_t *g_srid; static int g_nsreg;   // g_sview：採樣器號 → MTLSamplerState（持有） | g_sview: sampler ID → MTLSamplerState (retained)
// 內存池 Finalize：貼圖號登記表裡貼圖建在這個池上的號，丟掉緩存的 MTLTexture（視圖或基貼圖）。緩存原先要等這個號被重新登記才放，
// 池上直建的線性貼圖就一直拖著整塊池緩衝（Engage 標題演示裡常有 6–17 塊 16 MB 的已釋放池緩衝活著）。號若還在用，下次取用走慢路重建
// Memory pool Finalize: for texture IDs whose registered texture lives on this pool, drop the cached MTLTexture (view or base texture). The cache used to be released only when the ID was registered again,
// so a linear texture built directly on a pool kept the whole pool buffer alive (6 to 17 finalized 16 MB pool buffers stayed alive in Engage's title demo). If the ID is still used, the next lookup rebuilds it on the slow path
static void treg_drop_pool_views(const void *pool) {
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < g_ntreg; i++) {
        if (!g_treg[i].ok || (const void *)g_treg[i].t.pool != pool) continue;
        if (g_treg[i].view) { defer_obj_unref((__bridge id)g_treg[i].view); CFRelease(g_treg[i].view); g_treg[i].view = NULL; g_treg[i].view_id = g_treg[i].view_rid = 0; }
        if (g_treg[i].tview) { defer_obj_unref((__bridge id)g_treg[i].tview); CFRelease(g_treg[i].tview); g_treg[i].tview = g_treg[i].tview_src = NULL; }
    }
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    int id_ = tid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_ntreg) { int n = id_ + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_reg_gen++;
    if (g_treg[id_].view) { defer_obj_unref((__bridge id)g_treg[id_].view); CFRelease(g_treg[id_].view); g_treg[id_].view = NULL; g_treg[id_].view_id = g_treg[id_].view_rid = 0; }
    if (g_treg[id_].tview) { defer_obj_unref((__bridge id)g_treg[id_].tview); CFRelease(g_treg[id_].tview); g_treg[id_].tview = g_treg[id_].tview_src = NULL; }
    g_treg[id_].ok = t != NULL; if (t) g_treg[id_].t = *t; g_treg[id_].has_v = v != NULL; if (v) g_treg[id_].v = *v;
    if (id_ < (1 << 20)) atomic_store_explicit(&g_tser[id_], t ? t->serial : 0, memory_order_release);
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    int id_ = sid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_nsreg) { int n = id_ + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); g_sview = realloc(g_sview, n * sizeof *g_sview); g_srid = realloc(g_srid, n * sizeof *g_srid);
        memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); memset(g_sview + g_nsreg, 0, (n - g_nsreg) * sizeof *g_sview); memset(g_srid + g_nsreg, 0, (n - g_nsreg) * sizeof *g_srid); g_nsreg = n; }
    if (g_sview[id_]) { CFRelease(g_sview[id_]); g_sview[id_] = NULL; g_srid[id_] = 0; }
    g_reg_gen++;
    g_sreg[id_] = *sp; g_sreg_ok[id_] = 1;
    pthread_mutex_unlock(&g_rmu);
}
static MTLTextureType ttype(int target, int fallback) {
    switch (target) { case 0: return MTLTextureType1D; case 1: case 7: return MTLTextureType2D; case 2: return MTLTextureType3D;
        case 3: return MTLTextureType1DArray; case 4: return MTLTextureType2DArray; case 5: return MTLTextureType2DMultisample;
        case 6: return MTLTextureType2DMultisampleArray; case 8: return MTLTextureTypeCube; case 9: return MTLTextureTypeCubeArray; }
    return (MTLTextureType)fallback;
}
// 池外貼圖號 = 空槽：遊戲自己用 GetSeparateTextureHandle(0xFFFFF) 取句柄綁到不用的槽（實測調用點 main+0x1746820，
// 貼圖池容量 15256），從不登記該號。按 D3D / 空槽語義讀出 0（與未綁同一套全零啞貼圖），不報紅；池內未登記的號仍報紅
// Out-of-pool texture ID = empty slot: the game itself uses GetSeparateTextureHandle(0xFFFFF) to get a handle and bind it to unused slots (measured call site main+0x1746820,
// texture pool capacity 15256) and never registers that ID. Read 0 per D3D / empty-slot semantics (the same all-zero dummy textures as unbound), no failure; unregistered IDs inside the pool are still reported a failure
static int g_tpool_n; static uint64_t g_stat_tex_oob;
void mtl_set_tex_pool_n(int n) { g_tpool_n = n; }
// 著色器聲明的貼圖類型與綁到的貼圖類型不同（2D ↔ 2D 數組、立方 ↔ 立方數組）：Maxwell 用數組指令採樣 2D 貼圖照常讀第 0 層，Metal 在 Mac 上類型不符就讀出 0。
// 朋友收集的文字著色器把字形緩存（4096×4096 R8 線性貼圖，建在池緩衝上，只能是 2D）聲明成 texture2d_array：讀出 0 → 透明度 0 → 整段字被丟棄（馬力歐在 Mac 上文字空白應是同一原因）。
// 換成同一份存儲、該類型的單層視圖（類型視圖不需要 PixelFormatView，線性貼圖也能建），按貼圖號緩存；視圖持有原貼圖，原貼圖在時指針不會被復用
// The type the shader declares differs from the bound texture's (2D ↔ 2D array, cube ↔ cube array): Maxwell sampling a 2D texture with an array instruction reads layer 0 as usual, while Metal on the Mac reads 0 on a type mismatch.
// Tomodachi Life's text shader declares the glyph cache (a 4096×4096 R8 linear texture on a pool buffer, which can only be 2D) as texture2d_array: it read 0 → alpha 0 → every glyph discarded (Mario's blank text on the Mac is most likely the same cause).
// Switch to a single-layer view of that type over the same storage (a type-only view needs no PixelFormatView and works on linear textures too), cached per texture ID; the view holds the original texture, so its pointer cannot be reused while it exists
static uint64_t tex_type_fix(int tid, __unsafe_unretained id<MTLTexture> *t, MTLTextureType want) {   // 調用方持 g_rmu | Caller holds g_rmu
    MTLTextureType have = (*t).textureType;
    int ok = (have == MTLTextureType2D && want == MTLTextureType2DArray) || (have == MTLTextureType2DArray && want == MTLTextureType2D) ||
             (have == MTLTextureTypeCube && want == MTLTextureTypeCubeArray) || (have == MTLTextureTypeCubeArray && want == MTLTextureTypeCube);
    if (!ok) { RED_ONCE(56, "貼圖類型對不上：著色器要 %lu，綁的是 %lu（貼圖號 %d），照綁", (unsigned long)want, (unsigned long)have, tid); return 0; }
    if (tid <= 0 || tid >= g_ntreg) return 0;
    treg_t *rp = &g_treg[tid];
    if (rp->tview && rp->tview_src == (__bridge void *)*t && rp->tview_type == (uint32_t)want) { *t = (__bridge id<MTLTexture>)rp->tview; return rp->tview_rid; }
    NSUInteger ns = want == MTLTextureTypeCube || want == MTLTextureTypeCubeArray ? 6 : 1;
    // 從底圖建、帶上原視圖的通道重排與級 / 層位置：原先在（可能已重排的）視圖上用不帶重排的方法建，重排丟了——朋友收集字形緩存是 R8 配 (1 1 1 R)，
    // 讀透明度的文字陰影著色器拿到恆為 1 的透明度，整個字形方塊被填滿，成了島名頁上的綠色方塊
    // Build from the base texture, carrying the original view's swizzle and level / layer position: it used to be built on the (possibly swizzled) view with the method without a swizzle, and the swizzle was lost -- Tomodachi Life's glyph cache is R8 with (1 1 1 R),
    // so the text shadow shader reading alpha got a constant alpha of 1 and filled whole glyph boxes, the green squares on the island name page
    id<MTLTexture> src = *t, from = src.parentTexture ? src.parentTexture : src;
    NSUInteger l0 = src.parentTexture ? src.parentRelativeLevel : 0, s0 = src.parentTexture ? src.parentRelativeSlice : 0;
    id<MTLTexture> v = [from newTextureViewWithPixelFormat:src.pixelFormat textureType:want levels:NSMakeRange(l0, src.mipmapLevelCount) slices:NSMakeRange(s0, ns) swizzle:src.swizzle];
    if (!v) { RED_ONCE(57, "按著色器類型 %lu 建不出視圖（貼圖號 %d，原類型 %lu）", (unsigned long)want, tid, (unsigned long)have); return 0; }
    static int n; if (n++ < 4) MLOG("貼圖號 %d：著色器要類型 %lu、綁的是 %lu，改用同存儲的類型視圖", tid, (unsigned long)want, (unsigned long)have);
    if (rp->tview) { defer_obj_unref((__bridge id)rp->tview); CFRelease(rp->tview); }
    rp->tview = (void *)CFBridgingRetain(v); rp->tview_src = (__bridge void *)*t; rp->tview_type = (uint32_t)want; rp->tview_rid = v.gpuResourceID._impl;
    *t = (__bridge id<MTLTexture>)rp->tview; return rp->tview_rid;
}
static id<MTLTexture> tex_by_id(int tid) {   // 調用方持 g_rmu | Caller holds g_rmu
    if (g_tpool_n && tid >= g_tpool_n) { if (!g_stat_tex_oob++) MLOG("池外貼圖號 %d（池容量 %d）按空槽處理：讀出 0", tid, g_tpool_n); return nil; }
    if (tid < 0 || tid >= g_ntreg || !g_treg[tid].ok) { RED_ONCE(10, "貼圖號 %d 未登記", tid); return nil; }
    treg_t *rp = &g_treg[tid];
    uint64_t sg = g_ser_gen, dg = nvn_dirty_now();   // 先取代號再查：期間若有變更，下次必走慢路 | Take the generations first and then look up: if a change happens in between, the next time must take the slow path
    if (rp->view && rp->view_ser == sg && rp->view_dirty == dg) return (__bridge id<MTLTexture>)rp->view;
    id<MTLTexture> v = (__bridge id<MTLTexture>)rp->view;
    if (v) {   // 序號表或客體內存有過變動：核基貼圖是否被重傳換新，換了則視圖作廢 | The serial table or guest memory has changed: check whether the base texture was re-uploaded and replaced; if so the view is invalidated
        id<MTLTexture> cur = tex_mtl(&rp->t);
        if (cur == v || v.parentTexture == cur) { rp->view_ser = sg; rp->view_dirty = dg; return v; }
        defer_obj_unref(v); CFRelease(rp->view); rp->view = NULL; rp->view_rid = 0; v = nil;
    }
    treg_t r = *rp;
    id<MTLTexture> base = tex_mtl(&rp->t);
    if (!base) return nil;
    v = base;
    // 通道重排：視圖設了用視圖的，否則用貼圖建造時的；恆等不建視圖（SMBW 的字體緩存是 (1 1 1 R)：不重排時字形成白塊）
    // Channel swizzle: the view's if set, otherwise the one from texture building; identity builds no view (SMBW's font cache is (1 1 1 R): without the swizzle glyphs become white blocks)
    uint8_t sw[4] = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha}; int need_sw = 0;
    if (r.has_v && r.v.set_swizzle) memcpy(sw, r.v.swz, 4);
    else if (r.t.set_swz) for (int c = 0; c < 4; c++) sw[c] = (uint8_t)(r.t.swz >> (3 * c) & 7);
    for (int c = 0; c < 4; c++) { if (sw[c] > MTLTextureSwizzleAlpha) sw[c] = (uint8_t)(MTLTextureSwizzleRed + c); if (sw[c] != MTLTextureSwizzleRed + c) need_sw = 1; }
    if ((r.has_v && (r.v.set_format || r.v.set_levels || r.v.set_layers || r.v.set_target)) || need_sw) {
        if (!r.has_v) memset(&r.v, 0, sizeof r.v);
        uint32_t pf = (uint32_t)base.pixelFormat; uint8_t swz[4];
        if (r.v.set_format && !(nvn_fmt_to_mtl(r.v.format, &pf, swz) && pf)) pf = (uint32_t)base.pixelFormat;
        { uint32_t upf; int ubpp; if (r.v.set_format && bc_unpack(r.v.format, &upf, &ubpp)) pf = upf; }   // 無 BC：BC 視圖跟底圖一樣換成解壓後的格式 | No BC: a BC view is switched to the decompressed format just like the base texture
        if (r.v.set_format && tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) {
            base = tex_upgrade_pfv(&rp->t, base, (MTLPixelFormat)pf);
            sg = g_ser_gen;
        }
        NSUInteger l0 = r.v.set_levels ? r.v.base_level : 0, nl = r.v.set_levels && r.v.nlevels ? r.v.nlevels : base.mipmapLevelCount - l0;
        if (l0 >= base.mipmapLevelCount) l0 = base.mipmapLevelCount - 1; if (nl > base.mipmapLevelCount - l0) nl = base.mipmapLevelCount - l0;   // 視圖級數同樣夾到底圖實有的級數（底圖級數可能已被夾過）| clamp the view's levels to what the base texture has too (its count may have been clamped)
        NSUInteger s0 = r.v.set_layers ? r.v.min_layer : 0;
        NSUInteger ns = base.textureType == MTLTextureType3D ? 1 : (r.v.set_layers && r.v.nlayers ? r.v.nlayers : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1) - s0);
        MTLTextureType tt = r.v.set_target ? ttype(r.v.target, base.textureType) : base.textureType;
        if (tt == MTLTextureType2D && ns > 1) ns = 1;
        v = need_sw ? [base newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns) swizzle:MTLTextureSwizzleChannelsMake(sw[0], sw[1], sw[2], sw[3])]
                    : [base newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns)];
        if (!v) { RED_ONCE(11, "貼圖視圖建不出 id %d", tid); v = base; }
    }
    rp->view = (void *)CFBridgingRetain(v); rp->view_rid = v.gpuResourceID._impl; rp->view_ser = sg; rp->view_dirty = dg;
    return v;
}
// 熱路徑取對象：表裡有效就直接返回表持有的指針（不經 ARC 的 retain / autorelease），否則走慢路建 / 校驗。調用方持 g_rmu。
// Hot-path object fetch: if the table entry is valid, return the table-held pointer directly (without ARC retain / autorelease), otherwise take the slow path to build / validate. Caller holds g_rmu.
static inline __attribute__((always_inline)) void *tex_fast(int tid) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && g_treg[tid].view_dirty == nvn_dirty_now()) return g_treg[tid].view;
    return (__bridge void *)tex_by_id(tid);
}
static inline __attribute__((always_inline)) uint64_t tex_fast_rid(int tid, void **out) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && g_treg[tid].view_dirty == nvn_dirty_now()) { if (out) *out = g_treg[tid].view; return g_treg[tid].view_rid; }
    id<MTLTexture> t = tex_by_id(tid); if (out) *out = (__bridge void *)t; return t ? t.gpuResourceID._impl : 0;
}
// 綁定改讀副本：按原綁定的格式 / 類型 / 級 / 層 / 通道重排建同樣的視圖（每通道每貼圖號建一次）；調用方持 g_rmu
// Bindings read the copy: build the same view as the original binding's format / type / levels / slices / swizzle (once per pass per texture ID); the caller holds g_rmu
static id<MTLTexture> fbx_bind(int tid, id<MTLTexture> t) {
    uint64_t ser = g_treg[tid].t.serial;
    for (int i = 0; i < g_fbx_n; i++) {
        if (g_fbx_ser[i] != ser) continue;
        NSNumber *key = @(tid); id<MTLTexture> v = g_fbx_views[key]; if (v) return v;
        id<MTLTexture> c = g_fbx_copy[i];
        if (t.parentTexture || t.pixelFormat != c.pixelFormat || t.textureType != c.textureType)
            v = [c newTextureViewWithPixelFormat:t.pixelFormat textureType:t.textureType levels:NSMakeRange(t.parentTexture ? t.parentRelativeLevel : 0, t.mipmapLevelCount)
                                          slices:NSMakeRange(0, 1) swizzle:t.swizzle];
        else v = c;
        if (!v) return nil;
        if (!g_fbx_views) g_fbx_views = [NSMutableDictionary dictionary];
        g_fbx_views[key] = v; return v;
    }
    return nil;
}
static MTLSamplerAddressMode wrapm(int w) {
    switch (w) { case 0: case 7: return MTLSamplerAddressModeClampToEdge; case 1: return MTLSamplerAddressModeRepeat;
        case 3: case 2: return MTLSamplerAddressModeMirrorClampToEdge; case 5: case 4: return MTLSamplerAddressModeClampToBorderColor;
        case 6: return MTLSamplerAddressModeMirrorRepeat; }
    return MTLSamplerAddressModeRepeat;
}
static id<MTLSamplerState> samp_by_id(int sid) {   // 調用方持 g_rmu | Caller holds g_rmu
    if (sid < 0 || sid >= g_nsreg || !g_sreg_ok[sid]) { RED_ONCE(12, "採樣器號 %d 未登記", sid); return nil; }
    if (g_sview[sid]) return (__bridge id<MTLSamplerState>)g_sview[sid];
    samp_par p = g_sreg[sid];
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = p.magf ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (p.minf == 1 || p.minf == 3 || p.minf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = p.minf <= 1 ? MTLSamplerMipFilterNotMipmapped : (p.minf == 2 || p.minf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.sAddressMode = wrapm(p.ws); d.tAddressMode = wrapm(p.wt); d.rAddressMode = wrapm(p.wr);
    d.lodMinClamp = p.lod_min < 0 ? 0 : p.lod_min; d.lodMaxClamp = p.lod_max;
    d.maxAnisotropy = p.aniso < 1 ? 1 : p.aniso > 16 ? 16 : (NSUInteger)p.aniso;
    if (p.cmp_mode) d.compareFunction = (MTLCompareFunction)(p.cmp_func - 1);
    d.borderColor = p.border[3] > 0.5f ? (p.border[0] > 0.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack) : MTLSamplerBorderColorTransparentBlack;
    d.supportArgumentBuffers = YES;
    id<MTLSamplerState> s = [g_dev newSamplerStateWithDescriptor:d];
    if (s) { g_sview[sid] = (void *)CFBridgingRetain(s); g_srid[sid] = s.gpuResourceID._impl; }
    return s;
}
static inline __attribute__((always_inline)) void *samp_fast(int sid) {
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid] && g_sview[sid]) return g_sview[sid];
    return (__bridge void *)samp_by_id(sid);
}

// ---------- 參數緩衝區塊：每次提交一塊，GPU 完成後回收 ----------
// ---------- Argument buffer blocks: one block per submission, reclaimed after the GPU completes ----------
@interface NvArena : NSObject { @public id<MTLBuffer> buf; uint64_t gpu, fence; size_t used; } @end
@implementation NvArena @end
static NSMutableArray<NvArena *> *g_arenas; static NvArena *g_cur_arena; static NSMutableArray<NvArena *> *g_sub_arenas;
static NvArena *arena_new(void) {
    // for-in 循環變量不持有對象：先取強引用再從數組刪，否則數組釋放後返回的是野指針
    // The for-in loop variable does not retain the object: take a strong reference first and then remove it from the array, otherwise what is returned after the array releases it is a dangling pointer
    for (NvArena *it in g_arenas) if (it->fence && it->fence <= g_done) { NvArena *r = it; [g_arenas removeObject:r]; r->fence = 0; r->used = 0; return r; }
    NvArena *a = [NvArena new]; a->buf = [g_dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared]; a->gpu = a->buf.gpuAddress; res_add(a->buf); g_arena_made++; return a;
}
void arena_begin(void) { if (!g_arenas) { g_arenas = [NSMutableArray array]; g_sub_arenas = [NSMutableArray array]; } g_cur_arena = nil; }
void arena_commit(uint64_t fence) { for (NvArena *a in g_sub_arenas) { a->fence = fence; [g_arenas addObject:a]; } [g_sub_arenas removeAllObjects]; g_cur_arena = nil; }
static size_t arena_alloc(size_t n, id<MTLBuffer> *buf) {
    n = (n + 255) & ~(size_t)255;
    if (!g_cur_arena || g_cur_arena->used + n > g_cur_arena->buf.length) {
        pthread_mutex_lock(&g_mu); g_cur_arena = arena_new(); pthread_mutex_unlock(&g_mu);
        [g_sub_arenas addObject:g_cur_arena];
    }
    size_t off = g_cur_arena->used; g_cur_arena->used += n; *buf = g_cur_arena->buf; return off;
}

// ---------- 繪製狀態 ----------
// ---------- Draw state ----------
static struct {
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][128], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; int depth_clamp;
    float blend_color[4]; int64_t copy_stride;
} S;
static void udc_note_draw(NvProg *pr) {
    if (R.has_rt) for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) { zskip_del(R.rt.color[i]->serial); g_zskip_cand_ok[i] = 0; }   // 零源集合：真畫了就不再是全零 | Zero-source set: once really drawn it is no longer all zeros
    if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue; for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG) { zskip_clear(); break; } }   // 圖像槽可寫：保守清空 | Image slots are writable: conservatively clear
    int image_write = 0;
    uint32_t shader_color_mask = 0;
    if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        if (st == 1) shader_color_mask = sh->rf.color_mask;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG && sh->rf.b[i].rw) { image_write = 1; break; }
        if (image_write) break;
    }
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i];
        if (image_write) { t->wrote = 1; t->wwhy = 1; }
        else if (t->depth == 0 && t->idx >= 0 && (((S.chmask.mask >> (4 * t->idx)) & 0xf) || (shader_color_mask >> t->idx & 1))) { t->wrote = 1; t->wwhy = (S.chmask.mask >> (4 * t->idx)) & 0xf ? 2 : 3; }
        else if (t->depth == 1 && S.ds.dw) { t->wrote = 1; t->wwhy = 4; }
        else if (t->depth == 2 && S.ds.st && (S.st_wmask[0] || S.st_wmask[1])
                && (S.ds.fail_f != NVN_STENCIL_OP_KEEP || S.ds.zfail_f != NVN_STENCIL_OP_KEEP || S.ds.zpass_f != NVN_STENCIL_OP_KEEP
                    || S.ds.fail_b != NVN_STENCIL_OP_KEEP || S.ds.zfail_b != NVN_STENCIL_OP_KEEP || S.ds.zpass_b != NVN_STENCIL_OP_KEEP)) { t->wrote = 1; t->wwhy = 5; }
    }
}
static void state_reset(void) {
    memset(&S, 0, sizeof S);
    g_pso_dirty = g_dss_dirty = 1;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}

// 頂點格式按著色器輸入的分量類型校正：Metal 要求整數輸入配同符號的整數格式、浮點輸入配浮點 / 歸一化格式。
// 整數輸入（翻譯時按「只按整數用」推斷，dmsh_build.py int_inputs）配整數格式：符號不同則換同寬的另一符號（位模式不變）；配 32 位浮點格式則按同寬整數讀（位模式不變）。
// 其餘不匹配（整數輸入配歸一化 / 半精度格式，浮點輸入配整數格式）位模式保不住，報紅一次照原格式。
// Correct the vertex format by the shader input's component type: Metal requires integer inputs to get integer formats of the same signedness, and float inputs float / normalized formats.
// Integer input (inferred at translation time as "only used as an integer", dmsh_build.py int_inputs) with an integer format: on a signedness mismatch switch to the same width with the other sign (bit pattern unchanged); with a 32-bit float format read it as an integer of the same width (bit pattern unchanged).
// Other mismatches (integer input with a normalized / half format, float input with an integer format) cannot keep the bit pattern: report once and keep the format.
static MTLVertexFormat vfmt_for_ct(MTLVertexFormat f, int ct, int nvn_fmt) {
    static const MTLVertexFormat U[] = { MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4, MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4, MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4 };
    static const MTLVertexFormat I[] = { MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4, MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4, MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4 };
    static const MTLVertexFormat F[] = { MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4 };
    int iu = -1, ii = -1, iff = -1;
    for (int i = 0; i < 12; i++) { if (U[i] == f) iu = i; if (I[i] == f) ii = i; }
    for (int i = 0; i < 4; i++) if (F[i] == f) iff = i;
    if (ct == NVN_CT_UINT) { if (iu >= 0) return f; if (ii >= 0) return U[ii]; if (iff >= 0) return U[8 + iff]; }
    else if (ct == NVN_CT_INT) { if (ii >= 0) return f; if (iu >= 0) return I[iu]; if (iff >= 0) return I[8 + iff]; }
    else { if (iu < 0 && ii < 0) return f; }
    RED_ONCE(66, "頂點屬性格式 %#x 與著色器輸入類型 %s 對不上，位模式保不住（照原格式）", nvn_fmt, ct == NVN_CT_UINT ? "uint" : ct == NVN_CT_INT ? "int" : "float");
    return f;
}
static MTLVertexFormat vfmt(int f) {
    switch (f) {
        case 0x01: return MTLVertexFormatUCharNormalized; case 0x02: return MTLVertexFormatCharNormalized; case 0x03: return MTLVertexFormatUChar; case 0x04: return MTLVertexFormatChar;
        case 0x05: return MTLVertexFormatHalf; case 0x06: return MTLVertexFormatUShortNormalized; case 0x07: return MTLVertexFormatShortNormalized;
        case 0x08: return MTLVertexFormatUShort; case 0x09: return MTLVertexFormatShort; case 0x0A: return MTLVertexFormatFloat; case 0x0B: return MTLVertexFormatUInt; case 0x0C: return MTLVertexFormatInt;
        case 0x0D: return MTLVertexFormatUChar2Normalized; case 0x0E: return MTLVertexFormatChar2Normalized; case 0x0F: return MTLVertexFormatUChar2; case 0x10: return MTLVertexFormatChar2;
        case 0x11: return MTLVertexFormatHalf2; case 0x12: return MTLVertexFormatUShort2Normalized; case 0x13: return MTLVertexFormatShort2Normalized; case 0x14: return MTLVertexFormatUShort2;
        case 0x15: return MTLVertexFormatShort2; case 0x16: return MTLVertexFormatFloat2; case 0x17: return MTLVertexFormatUInt2; case 0x18: return MTLVertexFormatInt2;
        case 0x19: return MTLVertexFormatUChar3Normalized; case 0x1A: return MTLVertexFormatChar3Normalized; case 0x1B: return MTLVertexFormatUChar3; case 0x1C: return MTLVertexFormatChar3;
        case 0x1D: return MTLVertexFormatHalf3; case 0x1E: return MTLVertexFormatUShort3Normalized; case 0x1F: return MTLVertexFormatShort3Normalized; case 0x20: return MTLVertexFormatUShort3;
        case 0x21: return MTLVertexFormatShort3; case 0x22: return MTLVertexFormatFloat3; case 0x23: return MTLVertexFormatUInt3; case 0x24: return MTLVertexFormatInt3;
        case 0x25: return MTLVertexFormatUChar4Normalized; case 0x26: return MTLVertexFormatChar4Normalized; case 0x27: return MTLVertexFormatUChar4; case 0x28: return MTLVertexFormatChar4;
        case 0x29: return MTLVertexFormatHalf4; case 0x2A: return MTLVertexFormatUShort4Normalized; case 0x2B: return MTLVertexFormatShort4Normalized; case 0x2C: return MTLVertexFormatUShort4;
        case 0x2D: return MTLVertexFormatShort4; case 0x2E: return MTLVertexFormatFloat4; case 0x2F: return MTLVertexFormatUInt4; case 0x30: return MTLVertexFormatInt4;
        case 0x3D: return MTLVertexFormatUInt1010102Normalized; case 0x69: return MTLVertexFormatInt1010102Normalized; case 0x3F: return MTLVertexFormatFloatRG11B10; case 0x9A: return MTLVertexFormatUChar4Normalized_BGRA;
    }
    return MTLVertexFormatInvalid;
}
static MTLBlendFactor bfac(int f) {
    switch (f) { case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color; case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
        case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor; case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha; }
    return MTLBlendFactorOne;
}
static MTLBlendOperation bop(int e) { switch (e) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract; case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; } return MTLBlendOperationAdd; }

// 管線：按（程序, 目標格式, 混合 / 寫掩碼, 頂點佈局, 採樣數）緩存
// Pipeline: cached by (program, target formats, blend / write mask, vertex layout, sample count)
static const uint8_t *g_pso_lmap; static id<MTLFunction> g_pso_fsfn; static pso_fz_desc g_pso_fz, g_fz_desc;
static os_unfair_lock g_fz_fn_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSString *, id<MTLFunction>> *g_bf_fn_cache;
static NSMutableDictionary<NSString *, id<MTLLibrary>> *g_bf_lib_cache;
static nvn_hmap g_pso = {.klen = sizeof(pso_key)}, g_dss;
static nvn_hmap g_pso_fail = {.klen = sizeof(pso_key)};   // 建失敗的管線：記下不再重試（原先每次繪製都重編一遍，iPhone 上一局裡 1 萬多次） | pipelines that failed to build: remembered and never retried (previously every draw recompiled it, over 10,000 times in one iPhone session)
static MTLRenderPipelineDescriptor *pso_desc(const pso_key *k, NvShader *vs, NvShader *fs, id<MTLFunction> fsfn, int merged) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs->fn; d.fragmentFunction = fsfn ? fsfn : fs ? fs->fn : nil;
    // 診斷 DM_NVN_HILITE=片元著色器鍵,…：這些繪製改用純色片元（第 1–6 個鍵依次 品紅 / 綠 / 青 / 黃 / 藍 / 白），看幾何落在哪裡
    // Diagnostic DM_NVN_HILITE=fragment shader key,...: these draws use a solid-color fragment instead (keys 1–6 in order magenta / green / cyan / yellow / blue / white), to see where the geometry lands
    if (fs) { static char keys[6][24]; static int nk = -1; static id<MTLLibrary> hl;
        if (nk < 0) { nk = 0; const char *e = getenv("DM_NVN_HILITE");
            for (const char *q = e; q && *q && nk < 6; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 23) n = 23; memcpy(keys[nk], q, n); keys[nk++][n] = 0; } }
        for (int i = 0; i < nk; i++) if (!strncmp(fs->key, keys[i], strlen(keys[i]))) {
            if (!hl) { NSError *e = nil; hl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };\n"
                "fragment O hl0() { return {float4(1,0,1,1),float4(1,0,1,1),float4(1,0,1,1)}; } fragment O hl1() { return {float4(0,1,0,1),float4(0,1,0,1),float4(0,1,0,1)}; }\n"
                "fragment O hl2() { return {float4(0,1,1,1),float4(0,1,1,1),float4(0,1,1,1)}; } fragment O hl3() { return {float4(1,1,0,1),float4(1,1,0,1),float4(1,1,0,1)}; }\n"
                "fragment O hl4() { return {float4(0,0,1,1),float4(0,0,1,1),float4(0,0,1,1)}; } fragment O hl5() { return {float4(1,1,1,1),float4(1,1,1,1),float4(1,1,1,1)}; }\n" options:nil error:&e];
                if (!hl) MLOG("【紅】HILITE 著色器編譯失敗 %s", e.localizedDescription.UTF8String); }
            if (hl) d.fragmentFunction = [hl newFunctionWithName:[NSString stringWithFormat:@"hl%d", i]];
            break; } }
    d.rasterSampleCount = k->samples ? k->samples : 1;
    d.alphaToCoverageEnabled = k->a2c;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)k->cfmt[i];
        int l = merged ? k->lmap[i] : i;
        uint32_t m = l == 0xff ? 0 : (k->chmask >> (4 * l)) & 0xf;
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        if (l != 0xff && k->blend_en >> l & 1) {
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = bfac(k->blend[l].src_rgb); c.destinationRGBBlendFactor = bfac(k->blend[l].dst_rgb);
            c.sourceAlphaBlendFactor = bfac(k->blend[l].src_a); c.destinationAlphaBlendFactor = bfac(k->blend[l].dst_a);
            c.rgbBlendOperation = bop(k->blend[l].eq_rgb); c.alphaBlendOperation = bop(k->blend[l].eq_a);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) {
        int loc = rf->v[v].loc, a = rf->v[v].nvn;   // Metal 槽 loc ← NVN 屬性 a | Metal slot loc ← NVN attribute a
        MTLVertexFormat vf = a < 16 ? vfmt(k->attr[a].fmt) : MTLVertexFormatInvalid;
        // a >= S.nattr 時 k.attr[a] 保持 memset 的 0，vfmt(0) 為 Invalid，走同一回退分支且 fmt 非 0 的 RED_ONCE 條件不成立。
        // When a >= S.nattr, k.attr[a] keeps the 0 from memset, vfmt(0) is Invalid, the same fallback branch is taken and the RED_ONCE condition of fmt being non-0 does not hold.
        if (a < 16 && k->attr[a].fmt && vf == MTLVertexFormatInvalid) RED_ONCE(15, "頂點屬性格式 %#x 未映射", k->attr[a].fmt);
        if (vf == MTLVertexFormatInvalid) {
            int ct = rf->v[v].ct;
            vd.attributes[loc].format = ct == NVN_CT_UINT ? MTLVertexFormatUInt4 : ct == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4;
            vd.attributes[loc].offset = 0; vd.attributes[loc].bufferIndex = 30;
            vd.layouts[30].stride = 16; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0;
            continue;
        }
        int st = k->attr[a].stream;
        vf = vfmt_for_ct(vf, rf->v[v].ct, k->attr[a].fmt);
        vd.attributes[loc].format = vf; vd.attributes[loc].offset = k->attr[a].offset; vd.attributes[loc].bufferIndex = 16 + st;
        vd.layouts[16 + st].stride = k->stream[st].stride ? k->stream[st].stride : 4;
        vd.layouts[16 + st].stepFunction = k->stream[st].stride == 0 ? MTLVertexStepFunctionConstant : k->stream[st].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vd.layouts[16 + st].stepRate = k->stream[st].stride == 0 ? 0 : k->stream[st].divisor ? k->stream[st].divisor : 1;
    }
    d.vertexDescriptor = vd;
    return d;
}
// 管線鍵：當前狀態 S + 通道格式（普通通道；合併通道另帶 lmap / 特化函數，由 pso_get 填）
// Pipeline key: current state S + pass formats (normal pass; a merged pass additionally carries lmap / specialized function, filled in by pso_get)
static void pso_key_fill(pso_key *k, NvShader *vs, NvShader *fs, const uint32_t cfmt[8], uint32_t dfmt, uint32_t sfmt, uint32_t samples, int lmap) {
    memset(k, 0, sizeof *k);
    k->vs = (__bridge void *)vs; k->fs = (__bridge void *)fs;
    memcpy(k->cfmt, cfmt, sizeof k->cfmt); k->dfmt = dfmt; k->sfmt = sfmt; k->samples = samples;
    k->blend_en = S.color.blend_en; k->chmask = S.chmask.mask; k->a2c = S.ms.a2c;
    for (int i = 0; i < 8; i++) if (k->cfmt[i] || lmap) k->blend[i] = S.blend[i];
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) { int a = rf->v[v].nvn; if (a < 16 && a < S.nattr) { k->attr[a] = S.attr[a]; int st = S.attr[a].stream; if (st < 16) k->stream[st] = S.stream[st]; } }
}
// ---- 並行預編：首次遇到的管線原先在提交線程一條條同步編，一幀裡連著缺 9 條 = 284 ms（真機重場面）。
// 現在：一次提交裡第一次缺管線時，順著餘下的命令往前掃一遍（只重放狀態，不畫），把後面還會缺的普通通道管線一並丟給後台並行編；
// 提交線程走到那次繪製時只等「還沒編完的剩餘時間」。編出來的管線與同步編的完全相同（同一份描述符）。合併通道 / 特化片元的管線鍵依賴回放時的規劃結果，不預測，照舊同步編。
// ---- Parallel precompile: pipelines met for the first time used to be compiled synchronously one by one on the submit thread; 9 misses in a row in one frame = 284 ms (heavy scene on a real device).
// Now: on the first pipeline miss in a submission, scan ahead once along the remaining commands (replaying state only, no drawing) and kick all the normal-pass pipelines that will also miss later to the background to compile in parallel;
// when the submit thread reaches that draw it only waits for 'the remaining time not yet compiled'. The compiled pipelines are identical to synchronously compiled ones (same descriptor). Pipeline keys of merged passes / specialized fragments depend on the planning result at replay time, are not predicted, and are compiled synchronously as before.
#define PSO_PEND_MAX 384
static struct { pso_key k; id<MTLRenderPipelineState> ps; int used, done; } g_pp[PSO_PEND_MAX];
static pthread_mutex_t g_pp_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pp_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_pp_submit_mark, g_pp_kicked, g_pp_taken;   // 本次提交是否已預測過（按提交序號）；累計丟出 / 領用條數 | Whether this submission has already been predicted (by submission serial); cumulative kicked / taken counts
static void pso_predict_rest(void);
static int pso_pend_find(const pso_key *k) { for (int i = 0; i < PSO_PEND_MAX; i++) if (g_pp[i].used && !memcmp(&g_pp[i].k, k, sizeof *k)) return i; return -1; }
static void pso_pend_kick(const pso_key *k, NvShader *vs, NvShader *fs) {   // 提交線程調：沒在緩存、沒在表裡才丟 | Called by the submit thread: kick only if it is neither in the cache nor in the table
    if (hm_get_id(&g_pso, k)) return;
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k), slot = -1;
    if (at < 0) for (int i = 0; i < PSO_PEND_MAX; i++) if (!g_pp[i].used) { slot = i; break; }
    if (at < 0 && slot >= 0) { g_pp[slot].k = *k; g_pp[slot].ps = nil; g_pp[slot].done = 0; g_pp[slot].used = 1; }
    pthread_mutex_unlock(&g_pp_mu);
    if (at >= 0 || slot < 0) return;
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, nil, 0); g_pp_kicked++;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        pthread_mutex_lock(&g_pp_mu); g_pp[slot].ps = ps; g_pp[slot].done = 1; pthread_cond_broadcast(&g_pp_cv); pthread_mutex_unlock(&g_pp_mu);
    });
}
static id<MTLRenderPipelineState> pso_pend_take(const pso_key *k) {   // 提交線程調：表裡有就等它編完並取走；沒有返回 nil | Called by the submit thread: if it is in the table, wait for it to finish compiling and take it; otherwise return nil
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k); id<MTLRenderPipelineState> ps = nil;
    if (at >= 0) { while (!g_pp[at].done) pthread_cond_wait(&g_pp_cv, &g_pp_mu); ps = g_pp[at].ps; g_pp[at].ps = nil; g_pp[at].used = 0; }
    pthread_mutex_unlock(&g_pp_mu);
    return ps;
}
static __unsafe_unretained id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused);
static __unsafe_unretained id<MTLRenderPipelineState> pso_get(NvProg *pr, MTLRenderPassDescriptor *unused) { AP_B; __unsafe_unretained id<MTLRenderPipelineState> r = pso_get_(pr, unused); AP_E(2); return r; }
static __unsafe_unretained id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused) {
    (void)unused;
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs) { RED_ONCE(13, "程序沒有頂點著色器（或未映射）"); return nil; }
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, g_pso_lmap != NULL);
    if (g_pso_lmap) { memcpy(k.lmap, g_pso_lmap, 8); k.fsfn = (__bridge void *)g_pso_fsfn; }
    __unsafe_unretained id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    if (cached) return cached;
    if (hm_get_id(&g_pso_fail, &k)) return nil;
    id<MTLRenderPipelineState> ps;
    NSError *e = nil; uint64_t bt = bp_now();
    ps = g_pso_lmap ? nil : pso_pend_take(&k);
    if (ps) g_pp_taken++;
    else {
        if (g_pp_submit_mark != g_sub_serial) { g_pp_submit_mark = g_sub_serial; pso_predict_rest(); }   // 本次提交第一次缺：先把後面的丟出去並行編，再同步編眼前這條 | First miss in this submission: first kick the later ones out to compile in parallel, then synchronously compile the one at hand
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, g_pso_fsfn, g_pso_lmap != NULL);
        ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    }
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
    if (!ps) { MLOG("【紅】建渲染管線失敗（頂點 %s 片元 %s）：%s", vs->key, fs ? fs->key : "-", e.localizedDescription.UTF8String); hm_put_id(&g_pso_fail, &k, NSNull.null); return nil; }
    pso_skey sk; char shex[sizeof sk * 2 + 1]; pso_skey_from(&k, vs, fs, g_pso_lmap != NULL, g_pso_lmap ? &g_pso_fz : NULL, &sk); pso_skey_hex(&sk, shex);
    pso_record("R\t%s\n", shex);
    pso_user_record("R\t%s\n", shex);
    hm_put_id(&g_pso, &k, ps);
    return ps;
}
static NSString *warm_key(const char *s, size_t n) {
    size_t z = strnlen(s, n);
    return [[NSString alloc] initWithBytes:s length:z encoding:NSUTF8StringEncoding];
}
static NvShader *warm_shader(NSMutableDictionary<NSString *, NvShader *> *dict, NSString *key, int stage) {
    NvShader *sh = dict[key];
    if (!sh) { sh = shader_load(key, stage); if (sh) dict[key] = sh; }
    return sh;
}
static void warm_enqueue(WarmItem *item) {
    os_unfair_lock_lock(&g_warm_lock);
    [g_warm_items addObject:item];
    atomic_store_explicit(&g_warm_ready, 1, memory_order_release);
    os_unfair_lock_unlock(&g_warm_lock);
}
static void warm_worker(NSArray<NSString *> *paths) {
    @autoreleasepool {
        NSMutableDictionary<NSString *, NvShader *> *dict = [NSMutableDictionary dictionary];
        NSMutableSet<NSString *> *seen = [NSMutableSet set];
        for (NSString *path in paths) {
            NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            if (!txt) { MLOG("【紅】預熱清單讀取失敗：%s", path.UTF8String); continue; }
            { unsigned long nl = 0; for (NSString *l in [txt componentsSeparatedByString:@"\n"]) nl += l.length && ![l hasPrefix:@"#"];
              MLOG("管線預熱清單 %s：%lu 行", path.UTF8String, nl); }   // 診斷：每份清單讀到多少行（真機預熱 0 個時看是哪份沒讀到） | Diagnostics: how many lines each manifest gave (when a device warms up 0, shows which one was missing)
            for (NSString *line in [txt componentsSeparatedByString:@"\n"]) {
                if (!line.length || [line hasPrefix:@"#"] || [seen containsObject:line]) continue;
                [seen addObject:line];
                NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
                if (!c.count) continue;
                if ([c[0] isEqualToString:@"S"] && c.count >= 3) {
                    NSString *key = c[1]; NvShader *sh = warm_shader(dict, key, c[2].intValue);
                    if (!sh) continue;
                    WarmItem *item = [WarmItem new]; item->kind = 0; item->key = key; item->shader = sh;
                    warm_enqueue(item); g_warm_shaders++;
                } else if ([c[0] isEqualToString:@"R"] && c.count >= 2) {
                    pso_skey sk; if (pso_skey_parse(c[1].UTF8String, &sk)) continue;
                    NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
                    NvShader *vs = vkey.length ? warm_shader(dict, vkey, sk.vs_stage) : nil;
                    NvShader *fs = fkey.length ? warm_shader(dict, fkey, sk.fs_stage) : nil;
                    if (!vs || (fkey.length && !fs)) continue;
                    id<MTLFunction> fsfn = nil;
                    if (sk.fz) {
                        if (sk.fz_desc.kind == 1 && fs) { int fx[32]; for (int i = 0; i < 32; i++) fx[i] = sk.fz_desc.fx[i]; fsfn = bf_fn(fs, fx, sk.fz_desc.fbmask, sk.k.lmap); }
                                                if (!fsfn) { g_warm_skip_fz++; continue; }
                    }
                    pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; k.fsfn = (__bridge void *)fsfn;
                    NSError *e = nil; id ps = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&k, vs, fs, fsfn, sk.merged) error:&e];
                    if (!ps) { MLOG("【紅】後台建渲染管線失敗：%s", e.localizedDescription.UTF8String); continue; }
                    WarmItem *item = [WarmItem new]; item->kind = 1; item->vs_key = vkey; item->fs_key = fkey; item->vs = vs; item->fs = fs; item->fsfn = fsfn; item->pso = k; item->object = ps;
                    warm_enqueue(item); g_warm_rps++;
                } else if ([c[0] isEqualToString:@"C"] && c.count >= 2) {
                    NSString *key = c[1]; if (!key.length) continue;
                    NvShader *cs = warm_shader(dict, key, 5);
                    if (!cs) continue;
                    NSError *e = nil; id ps = [g_dev newComputePipelineStateWithFunction:cs->fn error:&e];
                    if (!ps) { MLOG("【紅】後台建計算管線失敗：%s", e.localizedDescription.UTF8String); continue; }
                    WarmItem *item = [WarmItem new]; item->kind = 2; item->key = key; item->shader = cs; item->object = ps;
                    warm_enqueue(item); g_warm_cps++;
                }
            }
        }
        MLOG("管線預熱完成：著色器 %llu 個 / 渲染管線 %llu 個（跳過特化片元 %llu）/ 計算管線 %llu 個，後台用時 %.1f ms",
             g_warm_shaders, g_warm_rps, g_warm_skip_fz, g_warm_cps, (bp_now() - g_warm_t0) / 1e6);
    }
    atomic_store(&g_warm_running, 0);
}
// 等後台預熱跑完（最多 timeout_ms）：返回實際等了多少毫秒，沒在跑返回 0。首裝時系統著色器緩存全冷，預熱慢於玩家進局的話，
// 沒預熱到的管線會在提交線程現編 = 局內卡頓；進局讀取畫面本來就擋著玩家，讓它多讀一會兒（hle_nvn.c 出圖處調）。
// Wait for the background warm-up to finish (at most timeout_ms): returns how many milliseconds were actually waited, 0 if it is not running. On first install the system shader cache is fully cold; if warm-up is slower than the player entering a run,
// pipelines not yet warmed are compiled on the spot on the submit thread = in-run stutter; the loading screen on entering a run blocks the player anyway, so let it load a little longer (called where hle_nvn.c presents).
int mtl_warm_wait(int timeout_ms) {
    if (!atomic_load(&g_warm_running)) return 0;
    uint64_t t0 = bp_now();
    while (atomic_load(&g_warm_running) && (bp_now() - t0) / 1000000ull < (uint64_t)timeout_ms) usleep(2000);
    return (int)((bp_now() - t0) / 1000000ull) + 1;
}
static void warm_start(void) {
    if (g_warm_started++) return;
    const char *e = getenv("DM_PSO_MANIFEST");
    if (e && !strcmp(e, "off")) return;
    NSArray<NSString *> *paths;
    if (e && *e) {
        paths = @[@(e)];
    } else {
        NSString *seed = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"];
        NSMutableArray<NSString *> *all = [NSMutableArray arrayWithObject:seed];
        NSString *user = pso_user_path();
        if (user && [NSFileManager.defaultManager fileExistsAtPath:user]) [all addObject:user];
        else MLOG("管線預熱：本機用戶清單%s%s", user ? "還沒有：" : "沒有路徑（DM_PSO_USER / DM_SAVE 都沒設）", user ? user.UTF8String : "");
        if (![NSFileManager.defaultManager fileExistsAtPath:seed]) { MLOG("【紅】管線預熱：種子清單不存在：%s", seed.UTF8String); return; }
        paths = all;
    }
    if (!g_warm_items) g_warm_items = [NSMutableArray array];
    g_warm_t0 = bp_now();
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);   // 不是可有可無的後台活：它決定玩家進局前管線是否就緒 | Not optional background work: it decides whether the pipelines are ready before the player enters a run
    dispatch_queue_t q = dispatch_queue_create("dm.nvn.pso-warm", attr);
    atomic_store(&g_warm_running, 1);
    dispatch_async(q, ^{ warm_worker(paths); });
}
static int g_rt_ovr;   // 運行時外推的模板覆蓋：0 不覆蓋；1 只在單位 / 特效 / 外推洞位（0xD0）上通過、不寫模板；3 照常深度測試，通過處寫特效位 0x40 | Stencil override for runtime extrapolation: 0 no override; 1 pass only on the unit / effect / extrapolation-hole bits (0xD0), no stencil write; 3 depth test as usual, write the effect bit 0x40 where it passes
static id<MTLDepthStencilState> dss_get(int has_depth, int has_stencil) {
    struct { ds_st ds; uint32_t vm[2], wm[2]; int hd, hs, ovr; } k;
    memset(&k, 0, sizeof k); k.ds = S.ds; memcpy(k.vm, S.st_vmask, 8); memcpy(k.wm, S.st_wmask, 8); k.hd = has_depth; k.hs = has_stencil; k.ovr = has_stencil ? g_rt_ovr : 0;
    if (!g_dss.klen) g_dss.klen = sizeof k;
    id<MTLDepthStencilState> s = hm_get_id(&g_dss, &k);
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (has_depth && S.ds.dt) { d.depthCompareFunction = (MTLCompareFunction)(S.ds.df - 1); d.depthWriteEnabled = S.ds.dw; }
    if (has_depth && S.ds.dt && S.ds.df == 3) {   // 診斷：相等測試的深度範圍；DM_NVN_EQ_LE=1 把相等當小於等於（判斷「相等測不過」） | diagnostics: depth range of equal tests; DM_NVN_EQ_LE=1 treats equal as less-equal (tells whether "equal fails")
        static int eqle = -1; if (eqle < 0) { const char *e = getenv("DM_NVN_EQ_LE"); eqle = e && atoi(e); }
        static int lg; if (lg++ < 4) MLOG("相等深度測試的狀態：深度範圍 %g–%g 寫深度 %d%s", S.dr[0], S.dr[1], S.ds.dw, eqle ? "（DM_NVN_EQ_LE：按小於等於建）" : "");
        if (eqle) d.depthCompareFunction = MTLCompareFunctionLessEqual;
    }
    if (has_stencil && S.ds.st) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b = [MTLStencilDescriptor new];
        f.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_f - 1); f.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_f - 1);
        f.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_f - 1); f.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_f - 1);
        f.readMask = S.st_vmask[0]; f.writeMask = S.st_wmask[0];
        b.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_b - 1); b.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_b - 1);
        b.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_b - 1); b.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_b - 1);
        b.readMask = S.st_vmask[1]; b.writeMask = S.st_wmask[1];
        d.frontFaceStencil = f; d.backFaceStencil = b;
    }
    if (has_stencil && k.ovr) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new];
        if (k.ovr == 1) { f.stencilCompareFunction = MTLCompareFunctionNotEqual; f.readMask = 0xD0; f.writeMask = 0; }   // 參考值 0：(s & 0xD0) != 0 才通過（單位 0x10、特效 0x40、外推洞 / 屏邊 0x80） | Reference value 0: passes only if (s & 0xD0) != 0 (unit 0x10, effect 0x40, extrapolation hole / screen edge 0x80)
        else { f.stencilCompareFunction = MTLCompareFunctionAlways; f.depthStencilPassOperation = MTLStencilOperationReplace; f.readMask = 0; f.writeMask = 0x40; }   // 參考值 0x40 | Reference value 0x40
        d.frontFaceStencil = f; d.backFaceStencil = f;
    }
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    hm_put_id(&g_dss, &k, s);
    return s;
}
// 參數緩衝：按 B 反射逐項填（uniform 槽 r、分離貼圖 / 採樣器槽 r、存儲緩衝槽 r、驅動段 c0 / c2、c1、support_buffer）
// 沒綁的資源讀出 0：給全零啞資源（NVN 的 0..255 號描述符為驅動保留，遊戲綁 0 號句柄即「此槽為空」）
// Argument buffer: filled item by item per the B reflection (uniform slot r, separate texture / sampler slot r, storage buffer slot r, driver segments c0 / c2, c1, support_buffer)
// Unbound resources read 0: give all-zero dummy resources (NVN descriptors 0..255 are reserved for the driver; the game binding handle 0 means 'this slot is empty')
static uint64_t g_dummy_tex_rid[8][4], g_dummy_img_rid[8][4], g_dummy_tbuf_rid[4];
static id<MTLTexture> dummy_tex(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    MTLTextureType tt = (MTLTextureType)dtt;
    MTLPixelFormat pf = dpf == NVN_DPF_DEPTH ? MTLPixelFormatDepth32Float : dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> t = cache[dtt & 7][dpf & 3];
    if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt; d.pixelFormat = pf; d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    t = [g_dev newTextureWithDescriptor:d];   // 共享存儲新建即全零 | Shared storage is all zeros when newly created
    if (pf != MTLPixelFormatDepth32Float) { uint8_t z[64] = {0}; NSUInteger faces = tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1;
        for (NSUInteger f = 0; f < faces; f++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:z bytesPerRow:16 bytesPerImage:16]; }
    cache[dtt & 7][dpf & 3] = t; g_dummy_tex_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t);
    return t;
}
// 未綁圖像槽的寫入匯：與讀出 0 的啞貼圖分開（寫入不得污染它們），同類型同像素類共用一張
// Write sink for unbound image slots: kept separate from the dummy textures that read 0 (writes must not pollute them); one is shared per same type and same pixel class
static id<MTLTexture> dummy_img(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    id<MTLTexture> t = cache[dtt & 7][dpf & 3]; if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = (MTLTextureType)dtt; d.pixelFormat = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA16Float;
    d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [g_dev newTextureWithDescriptor:d]; cache[dtt & 7][dpf & 3] = t; g_dummy_img_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t); return t;
}
static id<MTLBuffer> dummy_buf(void);
static id<MTLTexture> dummy_tbuf(int dpf) {   // 未綁類型化緩衝：全零（像素類與著色器的 texture_buffer<T> 一致） | Unbound typed buffer: all zeros (pixel class matches the shader's texture_buffer<T>)
    static __strong id<MTLTexture> cache[4];
    id<MTLTexture> t = cache[dpf & 3]; if (t) return t;
    MTLPixelFormat pf = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
    id<MTLBuffer> b = dummy_buf();
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:b.length / 16 resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
    t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:b.length];
    cache[dpf & 3] = t; g_dummy_tbuf_rid[dpf & 3] = t.gpuResourceID._impl; return t;
}
static id<MTLSamplerState> dummy_samp(void) {
    static id<MTLSamplerState> s; if (!s) { MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.supportArgumentBuffers = YES; s = [g_dev newSamplerStateWithDescriptor:d]; }
    return s;
}
static uint64_t dummy_samp_rid(void) { static uint64_t rid; if (!rid) rid = dummy_samp().gpuResourceID._impl; return rid; }
static id<MTLBuffer> dummy_buf(void) { static id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t dummy_buf_gpu(void) { static uint64_t gpu; if (!gpu) gpu = dummy_buf().gpuAddress; return gpu; }
// useResource 在同一編碼器內對同一（資源, 用法, 階段）是冪等的：記下本渲染編碼器已聲明過的，重復的跳過（計算編碼器每次派發新建，不去重）
// useResource is idempotent for the same (resource, usage, stage) within one encoder: remember what this render encoder has already declared and skip repeats (a compute encoder is created anew for each dispatch, no dedup)
static uint8_t g_args_dirty[6] = {1, 1, 1, 1, 1, 1};
static uint64_t g_args_reg_seen, g_args_dirty_seen;
static inline void args_dirty_all(void) { memset(g_args_dirty, 1, sizeof g_args_dirty); }
static inline void args_sync_dirty(void) {
    uint64_t reg = g_reg_gen, dirty = nvn_dirty_now();
    if (reg != g_args_reg_seen || dirty != g_args_dirty_seen) {
        g_args_reg_seen = reg; g_args_dirty_seen = dirty; args_dirty_all();
    }
}
static void bind_args(id<MTLCommandEncoder> en, int nvn_stage, int metal_vertex, id<MTLBuffer> ab, uint64_t gpu, size_t off, NSUInteger ai) {
    if (nvn_stage == 5) enc_c_args((id<MTLComputeCommandEncoder>)en, gpu, ai);
    else enc_r_args((id<MTLRenderCommandEncoder>)en, metal_vertex ? MTLRenderStageVertex : MTLRenderStageFragment, gpu, ai);
}
// B 路線的靜態常量緩衝：Ryujinx support_buffer（佈局見 Ryujinx.Graphics.Shader/SupportBuffer.cs：alpha_test u32、is_bgra u32[8]、
// viewport_inverse f4、viewport_size f4、frag_scale_count i32、render_scale f32[73]…，各字段按 16 字節對齊）與全 0 驅動常量段（64 KB = 著色器聲明的上限）
// Static constant buffers of route B: the Ryujinx support_buffer (layout in Ryujinx.Graphics.Shader/SupportBuffer.cs: alpha_test u32, is_bgra u32[8],
// viewport_inverse f4, viewport_size f4, frag_scale_count i32, render_scale f32[73]..., each field aligned to 16 bytes) and the all-0 driver constant segment (64 KB = the upper bound declared by shaders)
static id<MTLBuffer> support_buf(void) {
    static __strong id<MTLBuffer> b; if (b) return b;
    b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b);   // 新建即全 0 | All 0 when newly created
    const int render_scale_off = 192;   // MSL 佈局實測：alpha_test 0、is_bgra[8] 16、viewport_inverse 144、viewport_size 160、frag_scale_count 176、render_scale float4[73] 192（取 .x） | Measured MSL layout: alpha_test 0, is_bgra[8] 16, viewport_inverse 144, viewport_size 160, frag_scale_count 176, render_scale float4[73] 192 (take .x)
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)b.contents + render_scale_off + 16 * i) = 1.0f;
    return b;
}
static uint64_t support_gpu(void) { static uint64_t gpu; if (!gpu) gpu = support_buf().gpuAddress; return gpu; }
static id<MTLBuffer> zero_bank(void) { static __strong id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t zero_bank_gpu(void) { static uint64_t gpu; if (!gpu) gpu = zero_bank().gpuAddress; return gpu; }
// 界面繪製整體下移（iPad 真全屏，界面矩形不貼畫面頂邊）：給頂點階段 cb0 開頭的投影矩陣加一個裁剪空間的縱向偏移，換一份改過的拷貝綁上去。
// 不用挪視口的辦法：視口頂邊就是裁剪體的邊，宿主上提回真實畫面頂部的貼頂控件（界面坐標 y 為負）會被整塊裁掉。
// g_ui_moff 由 draw() 逐次繪製定（0 = 不改）；拷貝放在一塊環形緩衝裡，同一通道同一地址只拷一次。
// Shift all UI draws down as a whole (iPad true fullscreen, the UI rect does not touch the top edge of the picture): add a clip-space vertical offset to the projection matrix at the start of the vertex stage's cb0, and bind a modified copy instead.
// Moving the viewport is not used: the viewport's top edge is the edge of the clip volume, so top-anchored widgets that the host lifts back to the real top of the picture (UI coordinate y negative) would be clipped away entirely.
// g_ui_moff is set per draw by draw() (0 = no change); the copies live in a ring buffer, and the same address in the same pass is copied only once.
static double g_ui_moff;
static int ui_mat_ok(void) { const p_bind_buf *u = &S.ubo[0][0]; void *mb; uint64_t mo; return u->addr && u->size >= 64 && u->size <= 4096 && !pend_hits(u->addr, u->size) && mtl_addr(u->addr, u->size, &mb, &mo); }
static uint64_t ui_mat_patch(const p_bind_buf *u, void *mb, uint64_t mo) {
    enum { RING = 4 << 20 }; static __strong id<MTLBuffer> ring; static uint64_t ring_gpu, la, ls, lp, lg; static size_t at; static double lo;
    if (!ring) { ring = [g_dev newBufferWithLength:RING options:MTLResourceStorageModeShared]; res_add(ring); ring_gpu = ring.gpuAddress; }
    if (la == u->addr && ls == u->size && lp == g_pass.serial && lo == g_ui_moff) return lg;
    const uint8_t *src = [(__bridge id<MTLBuffer>)mb contents]; if (!src || u->size < 64 || u->size > 4096) return 0;
    size_t n = (u->size + 255) & ~(size_t)255; if (at + n > RING) at = 0;
    uint8_t *dst = (uint8_t *)ring.contents + at; memcpy(dst, src + mo, u->size);
    float m[16], o = (float)g_ui_moff; memcpy(m, dst, 64); m[1] += o * m[3]; m[5] += o * m[7]; m[9] += o * m[11]; m[13] += o * m[15]; memcpy(dst, m, 64);
    la = u->addr; ls = u->size; lp = g_pass.serial; lo = g_ui_moff; lg = ring_gpu + at; at += n;
    return lg;
}
// 無綁定句柄（TEXU / SAMPU）：uniform 槽 reg 字節偏移 off 處的 32 位字 = 遊戲寫進去的 NVN 句柄低 32 位（低 20 位貼圖號、高 12 位採樣器號）。
// 回放時從 CPU 側讀（與真機 GPU 執行時讀同一時刻的內存）；槽沒綁 / 越界 / 讀不到 = 0（按空槽：啞貼圖 / 啞採樣器）。
// 本提交裡排著 GPU 側寫入（pend_hits）的位置照樣讀當前值，首例報紅。
// Bindless handle (TEXU / SAMPU): the 32-bit word at byte offset off of uniform slot reg = the low 32 bits of the NVN handle the game wrote there (low 20 bits texture ID, high 12 bits sampler ID).
// Read on the CPU side at replay time (the memory at the same moment a real GPU would read it at execution); slot unbound / out of range / unreadable = 0 (empty slot: dummy texture / dummy sampler).
// A location with a GPU-side write queued in this submit (pend_hits) is still read at its current value; the first case is reported red.
static uint32_t ubo_handle(int stage, int reg, int off) {
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16) return 0;
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || (uint64_t)off + 4 > u->size) return 0;
    if (pend_hits(u->addr + off, 4)) {   // 本提交裡排著寫入：取 GPU 執行到這裡時的值（內聯更新的數據副本）；取不到才讀當前值 | writes queued in this submission: take the value the GPU will see here (the inline update's data copy); only fall back to the current value when none
        const uint8_t *v = pend_value(u->addr + off, 4); if (v) { uint32_t h; memcpy(&h, v, 4); return h; }
        RED_ONCE(47, "無綁定句柄所在的 uniform 緩衝（階段 %d 槽 %d +%#x）本提交裡有 GPU 側寫入且無數據副本：讀的是當前值", stage, reg, off);
    }
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, 4, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    uint32_t h; memcpy(&h, base + mo, 4); return h;
}
static int encode_args_(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex);
static int encode_args(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) { AP_B; int r = encode_args_(en, sh, nvn_stage, metal_vertex); AP_E(3); return r; }
static int encode_args_(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) {
    if (!sh->ae) return 1;
    uint64_t slots[NVN_REFL_MAXA] = {0};
    args_sync_dirty();
    if (!passdump() && !g_args_dirty[nvn_stage] && sh->ab_buf && sh->ab_pass == g_pass.serial && nvn_stage != 5) {
        bind_args(en, nvn_stage, metal_vertex, sh->ab_buf, sh->ab_gpu, sh->ab_off, sh->rf.abi);
        g_stat_args_reuse++;
        return 1;
    }
    #define USE(r, u) do { (void)(u); if (__builtin_expect(g_res_check_on, 0)) res_check((r), sh->key, reg); } while (0)
    // DM_NVN_PASSDUMP：每通道每階段第一次新編時記著色器鍵（K）與各綁定的實際來源（B：UBO 地址 / 長度 / 前 4 個浮點，貼圖號與序號或「啞」）
    // DM_NVN_PASSDUMP: on the first new encode per pass per stage, records the shader key (K) and the actual source of each binding (B: UBO address / length / first 4 floats, texture ID and serial, or 「啞」(dummy))
    static uint64_t bl_pass[6]; static int bl_all = -1; if (bl_all < 0) bl_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;   // 每次新編都記（否則只記每通道每階段第一次） | Record on every new encode (otherwise only the first time per pass per stage)
    int bl = passdump() && (bl_all || bl_pass[nvn_stage] != g_stat_pass);
    if (bl) { bl_pass[nvn_stage] = g_stat_pass; PD("K %llu st%d key=%s nb=%d", g_stat_pass, nvn_stage, sh->key[0] ? sh->key : sh->builtin ? sh->builtin : "?", sh->rf.nb); }
    pthread_mutex_lock(&g_rmu);   // 貼圖 / 採樣器登記表可能正被遊戲線程擴容或改寫 | The texture / sampler registration tables may be being grown or rewritten by the game thread
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        int reg = b->reg, aid = b->aid;
        if (b->kind == NVN_RB_CBUF) {
            const p_bind_buf *u = reg < 16 ? &S.ubo[nvn_stage][reg] : NULL;
            void *mb; uint64_t mo, gpu;
            if (!u || !u->addr || !mtl_gpu_addr(u->addr, 1, &mb, &mo, &gpu)) { RED_ONCE(16, "階段 %d cb%d 沒有綁定 uniform 緩衝", nvn_stage, reg); continue; }
            use_rec("buf", u->addr, u->size, 0, NULL);
            if (nvn_stage == 0 && reg == 0 && g_ui_moff != 0) { uint64_t pg = ui_mat_patch(u, mb, mo); if (pg) gpu = pg; }
            if (bl) {
                PD("B %llu st%d cb reg%d addr=%#llx size=%llu pool=%#llx+%#llx f=%g,%g,%g,%g", g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr, (unsigned long long)u->size,
                   (unsigned long long)(u->addr - mo), (unsigned long long)mo, ((float *)u->addr)[0], ((float *)u->addr)[1], ((float *)u->addr)[2], ((float *)u->addr)[3]);
                // DM_NVN_CBDUMP=目錄：同時把整塊 uniform 原樣存盤（F幀_p通道_st階段_cb槽_地址.bin），供真值側按客體地址逐字節對
                // DM_NVN_CBDUMP=directory: also saves the whole uniform block to disk as-is (F<frame>_p<pass>_st<stage>_cb<slot>_<address>.bin), for the ground-truth side to compare byte by byte by guest address
                // DM_NVN_CBDUMP_KEY=著色器鍵：只存該著色器（任一階段）所在繪製的 | DM_NVN_CBDUMP_KEY=shader key: only save for draws using that shader (any stage)
                static const char *cbd, *cbk; static int cbd_init; if (!cbd_init) { cbd = getenv("DM_NVN_CBDUMP"); cbk = getenv("DM_NVN_CBDUMP_KEY"); cbd_init = 1; }
                if (cbd && (!cbk || !strcmp(sh->key, cbk))) {
                    char p[1024]; snprintf(p, sizeof p, "%s/F%llu_p%llu_st%d_cb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr);
                    FILE *f = fopen(p, "wb"); if (f) { fwrite((const void *)u->addr, 1, u->size, f); fclose(f); } else RED_ONCE(41, "CBDUMP 寫不出 %s", p);
                }
            }
            slots[aid] = gpu;
            USE((__bridge id<MTLBuffer>)mb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SAMP || b->kind == NVN_RB_SAMPT || b->kind == NVN_RB_SAMPU) {
            // 數組（SAMPU）：第 e 個元素的句柄在 off + 8e（每元素一個 64 位 NVN 句柄，取低 32 位）| arrays (SAMPU): element e's handle is at off + 8e (one 64-bit NVN handle per element, low 32 bits used)
            for (int e = 0; e < (b->kind == NVN_RB_SAMPU ? b->cnt : 1); e++) {   // SAMPT：合併句柄（貼圖號 | 採樣器號 << 20）的採樣器部分；SAMPU：同上，句柄在 uniform 緩衝裡 | SAMPT: the sampler part of a combined handle (texture ID | sampler ID << 20); SAMPU: the same, with the handle in a uniform buffer
            int sid = b->kind == NVN_RB_SAMPU ? (int)(ubo_handle(nvn_stage, reg, b->off + 8 * e) >> 20) :
                      reg < 128 ? (int)((b->kind == NVN_RB_SAMPT ? S.tex[nvn_stage][reg] : S.samp[nvn_stage][reg]) >> 20) : 0;
            __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
            slots[aid + e] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
            USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            if (bl && sid > 0) { int ok = sid < g_nsreg && g_sreg_ok[sid]; samp_par sp = ok ? g_sreg[sid] : (samp_par){0};   // 本段已持 g_rmu | g_rmu is already held in this section
                if (ok) PD("B %llu st%d samp reg%d sid=%d mag=%d min=%d（0 最近點 / 1 雙線性；2–5 帶 mip）wrap=%d,%d,%d border=%g,%g,%g,%g", g_stat_pass, nvn_stage, reg, sid, (int)sp.magf, (int)sp.minf,
                           (int)sp.ws, (int)sp.wt, (int)sp.wr, sp.border[0], sp.border[1], sp.border[2], sp.border[3]); }
            }
        } else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TEXU) {
            // 數組（TEXU）同上；非無綁定的貼圖數組宿主還沒有，元素 1 起給啞貼圖 | arrays (TEXU) as above; the host has no non-bindless texture arrays yet, so elements from 1 on get dummy textures
            for (int e = 0; e < b->cnt; e++) {
            if (e > 0 && b->kind != NVN_RB_TEXU) { RED_ONCE(55, "貼圖數組綁定（非無綁定）未實現：元素 1 起給啞貼圖"); slots[aid + e] = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; USE(dummy_tex(b->dtt, b->dpf), MTLResourceUsageRead); continue; }
            int tid = b->kind == NVN_RB_TEXU ? (int)(ubo_handle(nvn_stage, reg, b->off + 8 * e) & 0xfffff) : reg < 128 ? (int)(S.tex[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = tex_type_fix(tid, &t, (MTLTextureType)b->dtt); if (r2) trid = r2; }
            if (t && g_fbx_n && tid < g_ntreg) { id<MTLTexture> fv = fbx_bind(tid, t); if (fv) { t = fv; trid = fv.gpuResourceID._impl; } }   // 讀自己的附件：改讀開通道前的副本 | reading one's own attachment: read the copy made before the pass
            if (t && tid < g_ntreg) use_rec("tex", (uint64_t)g_treg[tid].t.mem, g_treg[tid].t.L.size, tid, &g_treg[tid].t);
            if (t && passdump() && tid < g_ntreg) PD("R %llu st%d ser=%llu", g_stat_pass, nvn_stage, g_treg[tid].t.serial);
            if (bl) PD("B %llu st%d tex reg%d tid=%d %s ser=%llu fmt=%lu %lux%lux%lu view_lv=%lu+%lu view_sl=%lu array=%lu target=%lu", g_stat_pass, nvn_stage, reg, tid, t ? "實" : "啞", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                       (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth,
                       (unsigned long)(t.parentTexture ? t.parentRelativeLevel : 0), (unsigned long)t.mipmapLevelCount,
                       (unsigned long)(t.parentTexture ? t.parentRelativeSlice : 0), (unsigned long)t.arrayLength, (unsigned long)t.textureType);
            if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; }
            slots[aid + e] = trid;
            USE(t, MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_IMG) {   // 可寫貼圖：NVN 圖像槽 r（BindImages）↔ D3D u_r | Writable texture: NVN image slot r (BindImages) ↔ D3D u_r
            int tid = reg < 32 ? (int)(S.img[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (passdump()) PD("W %llu st%d reg%d tid=%d %s ser=%llu", g_stat_pass, nvn_stage, reg, tid, t ? "實" : "啞", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull);
            if (t && !(t.usage & MTLTextureUsageShaderWrite)) { RED_ONCE(32, "圖像槽綁的貼圖沒有可寫用途（貼圖號 %d）", tid); t = nil; }
            if (!t) { t = dummy_img(b->dtt, b->dpf); trid = g_dummy_img_rid[b->dtt & 7][b->dpf & 3]; }   // 未綁：D3D 語義寫入丟棄——寫進專用啞貼圖，不污染讀出 0 的那套 | Unbound: D3D semantics discard the write - write into a dedicated dummy texture so the set that reads 0 is not polluted
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead | MTLResourceUsageWrite);
        } else if (b->kind == NVN_RB_TBUF) {   // 類型化緩衝：貼圖槽 r 裡的緩衝貼圖（目標 BUFFER） | Typed buffer: the buffer texture (target BUFFER) in texture slot r
            int tid = reg < 128 ? (int)(S.tex[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && t.textureType != MTLTextureTypeTextureBuffer) { RED_ONCE(40, "類型化緩衝槽綁的不是緩衝貼圖（貼圖號 %d 類型 %lu）", tid, (unsigned long)t.textureType); t = nil; }
            if (bl) PD("B %llu st%d tbuf reg%d tid=%d %s ser=%llu", g_stat_pass, nvn_stage, reg, tid, t ? "實" : "啞", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull);
            if (!t) { t = dummy_tbuf(b->dpf); trid = g_dummy_tbuf_rid[b->dpf & 3]; }
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) {   // B：存儲緩衝 / 計算階段 uniform 是設備指針，按 NVN 槽取地址 | B: storage buffers / compute-stage uniforms are device pointers; the address is taken by NVN slot
            const p_bind_buf *u = reg < 16 ? (b->kind == NVN_RB_UBOP ? &S.ubo[nvn_stage][reg] : &S.ssbo[nvn_stage][reg]) : NULL;
            void *mb; uint64_t mo, gpu;
            if (u && u->addr && mtl_gpu_addr(u->addr, u->size ? u->size : 1, &mb, &mo, &gpu)) { use_rec("buf", u->addr, u->size, 0, NULL); slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); }
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); }
            if (bl) PD("B %llu st%d sbufp reg%d %s addr=%#llx size=%llu %s", g_stat_pass, nvn_stage, reg, u && u->addr ? "實" : "啞", u ? (unsigned long long)u->addr : 0ull, u ? (unsigned long long)u->size : 0ull, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_C1) {      // B：著色器自帶常數段，取著色器時建好 | B: the shader's own constant segment, built when the shader is fetched
            id<MTLBuffer> cb = sh->c1buf ? sh->c1buf : zero_bank();   // c1 長 0（全庫 600 份）而著色器仍聲明：讀 0 | c1 has length 0 (600 of them in the whole library) yet the shader still declares it: reads 0
            slots[aid] = (sh->c1buf ? sh->c1_gpu : zero_bank_gpu()) + (sh->c1buf ? sh->c1off : 0);
            USE(cb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SUPPORT) {   // support_buffer：縮放 1.0、BGRA 互換 0、視口變換未關閉（視口字段不讀） | support_buffer: scale 1.0, BGRA swap 0, viewport transform not disabled (viewport fields are not read)
            slots[aid] = support_gpu();
            USE(support_buf(), MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_DRV0 || b->kind == NVN_RB_DRV2) {
            // NVN 驅動常量段：出包時靜態統計證明不讀的供 0；讀的按當前綁定現填——c0 = 存儲緩衝表（基址 + 0x10 r：地址 64 位、大小 32 位），
            // c2 = 貼圖句柄表、圖像句柄表與分離採樣器句柄表（基址與槽數取自遊戲檔案 nvn_*；馬力歐 0x20 + 8 r、0x120 + 8 r），值即遊戲綁定時給的 NVN 句柄；讀到表外的位置出包時已登記、加載時報紅
            // NVN driver constant segments: those proven unread by static analysis at packaging time are supplied as 0; those read are filled on the spot from the current bindings - c0 = storage buffer table (base + 0x10 r: address 64 bits, size 32 bits),
            // c2 = texture handle table (0x168 + 8 r) and sampler handle table (0x568 + 8 r), the values being the NVN handles the game gave at bind time; positions read outside the tables are registered at packaging time and reported a failure at load time
            int bank = b->kind == NVN_RB_DRV2 ? 2 : 0;
            if (!sh->rf.drv_read[bank]) { slots[aid] = zero_bank_gpu(); USE(zero_bank(), MTLResourceUsageRead); }
            else {
                id<MTLBuffer> db; size_t doff = arena_alloc(0xA00, &db); uint8_t *d = (uint8_t *)db.contents + doff; memset(d, 0, 0xA00);
                if (bank == 0) {
                    int base = DM_GAME_NVN_SSBO_BASE(nvn_stage);   // 遊戲檔案 nvn_ssbo_bases（馬力歐：頂點 0x110、像素 0x510、計算 0x310） | game profile nvn_ssbo_bases (Mario: vertex 0x110, pixel 0x510, compute 0x310)
                    if (base < 0) RED_ONCE(45, "階段 %d 的著色器讀驅動段 c0，存儲緩衝表基址未實測", nvn_stage);
                    else for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[nvn_stage][r].addr; uint32_t n = (uint32_t)S.ssbo[nvn_stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
                } else {
                    for (int r = 0; r < DM_GAME_NVN_TEX_SLOTS; r++) { uint32_t th = (uint32_t)S.tex[nvn_stage][r]; memcpy(d + DM_GAME_NVN_TEX_BASE + 8 * r, &th, 4); }
                    for (int r = 0; r < DM_GAME_NVN_IMG_SLOTS; r++) { uint32_t ih = (uint32_t)S.img[nvn_stage][r]; memcpy(d + DM_GAME_NVN_IMG_BASE + 8 * r, &ih, 4); }   // 圖像句柄表 0x120 + 8 r | image handle table 0x120 + 8 r   // 本作（NVN 55）貼圖句柄表 0x20 + 8 r（dm/gfx/tools/dmsh_build.py TEX_BASE） | This game (NVN 55): texture handle table at 0x20 + 8 r (dm/gfx/tools/dmsh_build.py TEX_BASE)
                    // 分離採樣器句柄表（Engage 等舊驅動佈局：貼圖 0x168 + 8 r、採樣器 0x568 + 8 r；馬力歐不用分離採樣器，這裡全 0）
                    // separate sampler handle table (older driver layout such as Engage's: textures 0x168 + 8 r, samplers 0x568 + 8 r; Mario uses no separate samplers, so this is all zero)
                    for (int r = 0; r < 128; r++) { uint32_t sh_ = (uint32_t)S.samp[nvn_stage][r]; memcpy(d + DM_GAME_NVN_SAMP_BASE + 8 * r, &sh_, 4); }
                }
                slots[aid] = g_cur_arena->gpu + doff;
                USE(db, MTLResourceUsageRead);
            }
        } else RED_ONCE(46, "綁定種類 %d 未實現", b->kind);
    }
    #undef USE
    pthread_mutex_unlock(&g_rmu);
    NSUInteger arg_len = sh->arg_len ? sh->arg_len : sh->ae.encodedLength;
    id<MTLBuffer> ab; size_t off = arena_alloc(arg_len, &ab);
    memcpy((uint8_t *)ab.contents + off, slots, arg_len);
    bind_args(en, nvn_stage, metal_vertex, ab, g_cur_arena->gpu + off, off, sh->rf.abi);
    sh->ab_buf = ab; sh->ab_gpu = g_cur_arena->gpu + off; sh->ab_off = off; sh->ab_pass = g_pass.serial; g_args_dirty[nvn_stage] = 0;
    g_stat_args_enc++;
    return 1;
}
static id<MTLBuffer> g_quad_ib; static uint64_t g_quad_gpu;
static id<MTL4RenderCommandEncoder> rp_begin(void);
// ---------- 通道前瞻：本通道是否用到深度 / 模板 ----------
// 開通道時從當前繪製的下一條起往後掃，模擬深度模板狀態的變化，直到本通道必然結束處（重設目標、拷貝、計算派發——
// 與回放裡結束通道的操作一一對應）。期間沒有任何繪製開深度測試 / 深度寫 / 模板測試，則本通道可不掛深度附件：
// 深度內容一字節不動，TBDR 上省掉整張深度的讀入和寫出（真機實測：單繪製全屏通道佔 GPU 一半，多掛著 D32S8 讀入寫出）。
// 掃描按真實回放順序跨子列表返回、跨本提交餘下句柄；提交末尾 = 通道必然結束。通道中途清除、嵌套過深等無法判定的情況一律算「用到」。
// ---------- Pass lookahead: does this pass use depth / stencil ----------
// When opening a pass, scan forward from the op after the current draw, simulating the changes of depth-stencil state, up to where this pass must end (target reset, copy, compute dispatch -
// matching one-to-one the ops that end a pass in the replay). If no draw in between enables depth test / depth write / stencil test, this pass may leave the depth attachment unattached:
// not one byte of the depth content is touched, and on TBDR the load and store of the whole depth texture are saved (measured on a real device: a single-draw fullscreen pass takes half the GPU, with an extra D32S8 load and store attached).
// The scan follows real replay order across sub-list returns and across the remaining handles of this submission; end of submission = the pass must end. Undecidable cases such as a mid-pass clear or too-deep nesting always count as 'used'.
static const uint8_t *g_exec_next;   // 回放中當前繪製的下一條操作 | The op following the current draw in the replay
static const uint8_t *g_ret[16]; static int g_ret_n;   // 回放的調用返回棧（OP_CALL 處壓入調用方的下一條） | Call return stack of the replay (at OP_CALL the caller's next op is pushed)
static const uint64_t *g_rest; static int g_nrest;      // 本次提交裡當前句柄之後還要回放的句柄 | Handles in this submission still to be replayed after the current handle
void mtl_exec_rest(const uint64_t *h, int n) { g_rest = h; g_nrest = n; }
static uint64_t g_stat_ds_omit;      // 因前瞻證明不用而少掛深度模板的通道數（含只掛模板 / 只掛深度） | Number of passes that attach less depth-stencil because the lookahead proved it unused (including stencil-only / depth-only)
static uint64_t g_stat_s_only, g_stat_d_only;   // 其中只掛模板 / 只掛深度的通道數 | Of those, the number of passes that attach only stencil / only depth
// 純清除通道前提（hoist_try）：已提前執行、回放走到原位置時要跳過的操作（各組的 SetRenderTargets 與其中的清除）。
// 執行循環、寫回前瞻（sdx 索引）、深度模板前瞻（ds_scan）都按它跳過——否則前瞻會把原位置的清除當成「再次被清」，誤判提前清好的內容多餘
// Clear-only pass hoist (hoist_try): ops that were already executed early and must be skipped when replay reaches their original position (each group's SetRenderTargets and the clears in it).
// The exec loop, the store lookahead (sdx index) and the depth-stencil lookahead (ds_scan) all skip by it - otherwise the lookahead would take the clear at the original position as 'cleared again' and wrongly judge the early-cleared content redundant
#define HOIST_MAX 40   // 最多 4 組 ×（1 條設目標 + 8 條清除） | At most 4 groups × (1 set-target op + 8 clears)
static const uint8_t *g_hoist_skip[HOIST_MAX]; static int g_hoist_n; static uint64_t g_hoist_groups;
static int hoist_skipped(const uint8_t *p) { for (int i = 0; i < g_hoist_n; i++) if (g_hoist_skip[i] == p) return 1; return 0; }
typedef struct { const uint8_t *p, *ret[8]; int ret_n, rest_i, bad; } fz_cursor;
static int fz_next(fz_cursor *c, const uint8_t **out, op_hdr *oh, const void **oa) {
    for (int guard = 0; guard < 1000000; guard++) {
        if (!c->p) return 0;
        op_hdr h; memcpy(&h, c->p, sizeof h); const void *a = c->p + 8;
        switch (h.op) {
            case OP_END:
                if (c->ret_n) c->p = c->ret[--c->ret_n];
                else if (c->rest_i < g_nrest) c->p = (const uint8_t *)g_rest[c->rest_i++];
                else { c->p = NULL; return 0; }
                continue;
            case OP_JUMP: memcpy(&c->p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (c->ret_n >= 8) { c->bad = 1; return 0; }
                c->ret[c->ret_n++] = c->p + h.size; c->p = (const uint8_t *)t; continue; }
            default: break;
        }
        const uint8_t *p = c->p; c->p += h.size;
        if (g_hoist_n && hoist_skipped(p)) continue;
        if (out) *out = p; if (oh) *oh = h; if (oa) *oa = a; return 1;
    }
    c->bad = 1; return 0;
}
// 可讀 tile 通道合併（合併 B 第 2 步）的狀態：前一個通道（階段 1）與後一個通道（階段 2）共用一個渲染通道，附件取並集
// State of the readable-tile pass merge (merge B step 2): the earlier pass (phase 1) and the later pass (phase 2) share one render pass, and the attachments are the union
typedef struct {
    int active, phase, n1, nskip;
    int nreuse;
    const uint8_t *rt2_op, *reuse[8], *skip[12];
    p_set_rt rt1, rt2;                 // 兩個通道各自的邏輯目標（R.rt 在合併期間是並集） | Logical targets of each of the two passes (R.rt is the union during the merge)
    int8_t p2map[8];                   // 後一個通道的邏輯顏色號 → 物理附件 | Logical color number of the later pass → physical attachment
    uint8_t lmap1[8], lmap2[8];        // 物理附件 → 本階段繪製的邏輯顏色號（0xff = 不寫） | Physical attachment → logical color number of this phase's draws (0xff = not written)
    uint8_t clr2[8], cd2f, cs2f; MTLClearColor cc2[8]; double cd2; uint32_t cs2;   // 後一個通道開頭的清除（局內雙渲染重畫時照原樣用） | Clears at the start of the later pass (used as-is when the in-run dual render redraws)
    uint8_t wrote[8];                   // 後一個通道寫過的物理附件 | Physical attachments written by the later pass
    uint8_t cov2[8];                     // 後一個通道首繪覆蓋的物理附件 | Physical attachments covered by the first draw of the later pass
} fuse_t;
static fuse_t g_fz; static uint64_t g_fz_n, g_fz_draws, g_fz_fallback;
typedef struct { int idx; p_draw dw; p_draw_idx di; p_scissor sc; int has_sc; void *S_; uint8_t pm[8]; } defer_snap;   // pm：組 P 目標下標 → 本次繪製的邏輯顏色號（自身繪製為恆等；被吸收的上游組為復合映射） | pm: group P target index → logical color number of this draw (identity for its own draws; a composed mapping for absorbed upstream groups)
typedef struct {
    int active, at_q, in_p, n, reason, reason_op, logged, reset_between; uint64_t born;   // born：創建序號（回放按它排，不按槽位） | born: creation serial (replay is ordered by it, not by slot)
    p_set_rt p_rt, q_rt;
    const uint8_t *p_end_op, *q_op;
    uint8_t clear_c[8], lmap[8];
    MTLClearColor cc[8];
    uint8_t clear_d, clear_s;
    double cd; uint32_t cs;
    uint64_t p_reads[512]; int nreads, p_dw, p_st;
    defer_snap snap[64];
} defer_t;
static defer_t g_defer_slots[4];
static defer_t *g_defer_cur = &g_defer_slots[0];
#define g_defer (*g_defer_cur)
static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb);
static uint64_t g_defer_n, g_defer_draws, g_defer_fallbacks;
static const uint8_t *g_defer_pend;   // 在 P 區間那一組的 P 區間結束操作（沒有則 NULL）：執行循環每條操作只比這一個指針 | The P-range end op of the group that is in its P range (NULL if none): the exec loop compares each op against just this one pointer
static int g_defer_nact, g_defer_natq;   // 活動組數、已到 Q 等回放的組數（熱路徑只看計數，不逐槽掃） | Number of active groups, number of groups that reached Q and await replay (the hot path only looks at the counts, no per-slot scan)
static int g_defer_ab = -1, g_defer_ab_on;
static void draw(const p_draw *dw, const p_draw_idx *di);
static int defer_has_in_p(void) { return g_defer_pend != NULL; }
static int defer_has_active(void) { return g_defer_nact > 0; }
static int defer_slot_free(void) { for (int i = 0; i < 4; i++) if (!g_defer_slots[i].active) return i; return -1; }
static int defer_same_q_depth(const defer_t *a, const p_set_rt *q) {
    if (!a->q_rt.depth || !q->depth) return 1;
    return sub_eq(&a->q_rt, 8, q, 8);
}
static int defer_next(int at_q_only, uint64_t after) {   // 下一個（創建序號最小且 > after）活動組的槽號；沒有返回 −1 | Slot number of the next active group (smallest creation serial that is > after); returns −1 if none
    int best = -1; for (int i = 0; i < 4; i++) { const defer_t *d = &g_defer_slots[i]; if (!d->active || (at_q_only && !d->at_q) || d->born <= after) continue; if (best < 0 || d->born < g_defer_slots[best].born) best = i; }
    return best;
}
static void defer_release_one(defer_t *d) { if (d->active) { g_defer_nact--; if (d->at_q) g_defer_natq--; if (d->in_p) g_defer_pend = NULL; } for (int i = 0; i < d->n; i++) free(d->snap[i].S_); memset(d, 0, sizeof *d); }
static void defer_clear_all(void) { for (int i = 0; i < 4; i++) defer_release_one(&g_defer_slots[i]); g_defer_nact = g_defer_natq = 0; g_defer_pend = NULL; }
static int hoist_take(const uint8_t *p) { for (int i = 0; i < g_hoist_n; i++) if (g_hoist_skip[i] == p) { g_hoist_skip[i] = g_hoist_skip[--g_hoist_n]; return 1; } return 0; }
enum { DS_FREE = 0, DS_USED = 1, DS_RET = 2 };
static int g_ds_why; static int g_ds_scanned; static int g_ds_need;   // g_ds_need：掃描中累計的需求，1 深度 2 模板   // 診斷：判「用到」時觸發的操作碼（繪製則記 1000 + dt*100 + dw*10 + st）、掃過的操作數 | g_ds_need: need accumulated during the scan, 1 depth 2 stencil   // Diagnostics: the opcode that triggered the 'used' verdict (for a draw, records 1000 + dt*100 + dw*10 + st), and the number of ops scanned
static int ds_scan(const uint8_t *p, ds_st *ds, int depth) {
    for (int guard = 0; guard < 1000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8; g_ds_scanned++;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }   // 已提前執行的清除通道 | Clear pass already executed early
        switch (h.op) {
            case OP_END: return DS_RET;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth >= 8) return DS_USED;
                int r = ds_scan((const uint8_t *)t, ds, depth + 1); if (r != DS_RET) return r; break; }
            case OP_SET_RT: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_UPDATE_UB: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX: case OP_DISPATCH: return DS_FREE;
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: g_ds_why = h.op; return DS_USED;
            case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 5) memcpy(ds, b->bytes, sizeof *ds); break; }
            case OP_DRAW: case OP_DRAW_IDX:   // 深度測試 / 寫入 → 要深度；模板測試 → 要模板；兩項都要即可停 | Depth test / write → depth needed; stencil test → stencil needed; can stop once both are needed
                g_ds_need |= (ds->dt || ds->dw ? 1 : 0) | (ds->st ? 2 : 0);
                if (g_ds_need == 3) { g_ds_why = 1000 + ds->dt * 100 + ds->dw * 10 + ds->st; return DS_USED; } break;
            default: break;
        }
        p += h.size;
    }
    return DS_USED;
}
// 寫回前瞻的判定（逐條命令，按真實回放順序）：
//   多餘：再次被設為附件後、任何繪製之前被整塊清除（顏色：該序號四通道全寫；深度：深度清除；模板：模板 8 位全清。深度與模板分開判、分開設寫回）
//   需要：有繪製 / 派發時它綁在任一階段的貼圖或圖像槽（含掃描開始時已綁著的）；作拷貝源 / 拷貝目標 / 清貼圖對象；再次作附件後先畫後清（讀入）
// 同一貼圖的不同級 / 層分開判（降採樣鏈寫第 k 級、讀第 k−1 級）；採樣綁定按整張貼圖算讀取（保守）
// Store lookahead decision (command by command, in real replay order):
//   Redundant: after being set as an attachment again and before any draw, it is cleared in full (color: all four channels of that index written; depth: depth clear; stencil: all 8 stencil bits cleared. Depth and stencil are decided separately and their write-back is set separately)
//   Needed: at a draw / dispatch it is bound to a texture or image slot of any stage (including ones already bound when the scan starts); used as copy source / copy destination / texture-clear target; drawn before cleared after becoming an attachment again (load)
// Different levels / layers of the same texture are decided separately (a downsample chain writes level k and reads level k−1); a sampler binding counts as a read of the whole texture (conservative)
static const uint8_t *g_exec_cur;   // 回放中當前這條操作（含） | The op currently being replayed (inclusive)
static void sd_need(sd_att *t, int why) { if (!t->state) { t->state = 1; t->why = (uint8_t)why; } }   // why：1 繪製作貼圖讀 2 作附件先畫後清 3 拷貝 4 清貼圖 5 掃到末尾未定 6 無起點 / 索引不可用 | why: 1 a draw reads it as a texture 2 as an attachment, drawn before cleared 3 copy 4 texture clear 5 scanned to the end undecided 6 no start point / index unavailable
static int sdx_decide(uint8_t st[9]);   // 寫回前瞻判定（索引見 mtl_sub_index） | Store lookahead decision (for the index see mtl_sub_index)
static void sd_finish(void) {
    if (!g_nsd) return;
    if (!tbdr_on()) {   // 優化全關：一律寫回 | All optimizations off: always write back
        for (int i = 0; i < g_nsd; i++) { sd_att *t = &g_sd[i];
            if (t->depth == 1) [g_enc setDepthStoreAction:MTLStoreActionStore]; else if (t->depth == 2) [g_enc setStencilStoreAction:MTLStoreActionStore]; else [g_enc setColorStoreAction:MTLStoreActionStore atIndex:(NSUInteger)t->idx]; }
        g_nsd = 0; return; }
    uint8_t nst[9];
    if (sdx_decide(nst)) { for (int i = 0; i < g_nsd; i++) if (nst[i] == 2) g_sd[i].state = 2; else sd_need(&g_sd[i], nst[i] >> 2); }
    else for (int i = 0; i < g_nsd; i++) sd_need(&g_sd[i], 6);   // 列表外結束的通道 / 索引不可用：一律寫回 | Pass ended outside the list / index unavailable: always write back
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i]; int udc_dead = udc_on() && t->load && !t->wrote;
        // 與窗口貼圖寫在同一個通道裡的顏色附件不進賬本：它是出圖鏈上的一環，不是「每幀先重置再用」的瞬態附件。
        // 局內「放大」通道一次寫兩張——場景貼圖 + 窗口貼圖，界面合成並進了同一通道，所以局內沒人回讀場景貼圖；遊戲一停畫 3D（讀取畫面）
        // 就改由單獨的合成通道回頭採它。「要進讀取畫面」在上一幀提交之後才知道，預判攔不住：iPad 真全屏每個安裝第一次換幕必撞一次
        // （幀 N 那張 2732×2048 附件沒寫回，幀 N+1 起被連採 4 幀；鍵永久禁用）。iPhone 比例下同尺寸的鍵在試用期就見到讀者、被靜默禁用。
        // A color attachment written in the same pass as the window texture does not enter the ledger: it is a link in the present chain, not a transient attachment that is 'reset every frame before use'.
        // The in-run 'upscale' pass writes two textures at once - scene texture + window texture - and the UI composite was merged into the same pass, so in-run nobody reads back the scene texture; as soon as the game stops drawing 3D (loading screen)
        // a separate composite pass samples it afterwards instead. 'About to enter the loading screen' is only known after the previous frame was submitted, so prediction cannot stop it: on iPad true fullscreen every install hits it once on the first act change
        // (the 2732×2048 attachment of frame N was not written back and is sampled for 4 consecutive frames starting at frame N+1; the key is permanently disabled). At the iPhone aspect ratio the same-size key already sees a reader during the trial period and is silently disabled.
        int xv_win = 0; for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k] && xv_present(R.rt.color[k]->serial)) xv_win = 1;
        uint64_t xtag = 0; int xvcand = xv_on() && !xv_win && t->depth == 0 && t->state != 2 && t->why == 5 && xv_first_good(t->ser, t->lv, t->sl) && !xv_present(t->ser)
                                        && !xv_disabled(xtag = xv_tag_pass(R.rt.color[t->idx], t->lv, t->idx, &R.rt));
        int xvdead = xvcand && xv_trial_get(xtag, 0) >= XV_TRIAL;   // 過了試用期才真的不寫回 | Only after the trial period is the write-back really skipped
        if (xvcand && !xvdead) xv_set(t->ser, t->lv, t->sl, XV_PROBE, xtag);   // 試用期：照常寫回，只跟蹤下一次觸碰 | Trial period: write back as usual, only track the next touch
        int dead = t->state == 2 || udc_dead || xvdead; MTLStoreAction sa = dead ? MTLStoreActionDontCare : MTLStoreActionStore;
        if (xvdead) { xv_set(t->ser, t->lv, t->sl, XV_INVALID, xtag); g_xv_dc++; }
        if (t->depth == 1) [g_enc setDepthStoreAction:sa]; else if (t->depth == 2) [g_enc setStencilStoreAction:sa]; else [g_enc setColorStoreAction:sa atIndex:(NSUInteger)t->idx];
        g_sd_n_all++; if (dead) { g_sd_n_dead++; g_sd_dead_bytes += t->bytes; g_bw_store -= t->bytes; if (udc_dead) g_udc_saved_bytes += t->bytes; }
        if (!t->state) t->why = 5;
        PD("Z %llu %s ser=%llu lv%u sl%u %s why=%d by=%s slot=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, xvdead ? "dontcare-xv" : dead ? "dontcare" : "store", dead ? 0 : t->why, t->why == 1 && t->wkey ? t->wkey : "-", t->wslot);
        if (udc_on()) PD("U %llu %s ser=%llu lv%u sl%u runtime=%s reason=%s state=%d load=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, t->wrote ? "寫過" : "沒寫", t->wwhy == 1 ? "圖像寫" : t->wwhy == 2 ? "顏色寫掩碼" : t->wwhy == 3 ? "片元輸出" : t->wwhy == 4 ? "深度寫" : t->wwhy == 5 ? "模板寫" : "無", t->state, t->load);
    }
    g_nsd = 0;
}
// 按真實回放順序接著掃：當前列表 → 各層調用方（返回棧自頂向下）→ 本提交餘下句柄；提交結束時通道必然結束（commit 前 rp_flush）
// 覆蓋證明（全部滿足才成立；任何一項認不出按不成立）：
//   著色器：頂點位置 xyz 原樣取自一個頂點屬性、w 為 1 或同屬性 .w（dmsh_build raster_facts）；片元不丟棄、不寫 sample_mask、寫該顏色號
//   狀態：該附件不混合、寫掩碼滿、無 alpha-to-coverage、填充模式、無深度測試（或 ALWAYS）、無模板測試、單採樣、非 3D
//   頂點：非索引或 16 / 32 位索引繪製，索引與位置都從 CPU 可見的池內存讀出；剪裁 / 視口 / 幾何 / 朝向 / 深度裁剪的判定在 nvn_cover.h（純函數，有單測）
// 讀出本次繪製全部頂點的屬性 a（非索引或 16 / 32 位索引；頂點號 = first + k 或 索引值 + base_vertex），缺分量按取數規則補 0 / 1。
// 成功返回 0，失敗返回原因碼（6 頂點數 / 索引類型，7 屬性號，8 格式 / 流，9 越界，10 地址）
// Continue scanning in real replay order: current list -> callers at each level (return stack from the top down) -> remaining handles of this submit; the render pass necessarily ends when the submit ends (rp_flush before commit)
// Coverage proof (holds only if all are satisfied; anything unrecognized is treated as not holding):
//   Shader: the vertex position xyz is taken as-is from one vertex attribute, w is 1 or the same attribute's .w (dmsh_build raster_facts); the fragment does not discard, does not write sample_mask, and writes this color index
//   State: this attachment has no blending, a full write mask, no alpha-to-coverage, fill mode, no depth test (or ALWAYS), no stencil test, single sample, not 3D
//   Vertices: non-indexed or 16 / 32-bit indexed draws, with indices and positions both read from CPU-visible pool memory; the scissor / viewport / geometry / winding / depth clipping decisions are in nvn_cover.h (pure functions, with unit tests)
// Read out attribute a of all vertices of this draw (non-indexed or 16 / 32-bit indexed; vertex number = first + k or index value + base_vertex), with missing components filled with 0 / 1 per the fetch rules.
// Returns 0 on success, a reason code on failure (6 vertex count / index type, 7 attribute index, 8 format / stream, 9 out of bounds, 10 address)
static int attr_read(int a, const p_draw *dw, const p_draw_idx *di, float P[64][4]) {
    int n = dw ? dw->count : di->count;
    if (n < 1 || n > 64 || (dw && dw->first < 0) || (di && (di->itype < 1 || di->itype > 2))) return 6;
    if (a < 0 || a >= S.nattr || S.attr[a].stream >= 16) return 7;
    va_st at = S.attr[a]; vs_st sm = S.stream[at.stream]; NVNbufferRange vb = S.vbuf[at.stream];
    MTLVertexFormat vf = vfmt(at.fmt); int nc = vf == MTLVertexFormatFloat2 ? 2 : vf == MTLVertexFormatFloat3 ? 3 : vf == MTLVertexFormatFloat4 ? 4 : 0;
    if (!nc || !sm.stride || sm.divisor || !vb.address) return 8;
    const uint8_t *ib = NULL; int isz = di ? (di->itype == 1 ? 2 : 4) : 0;
    if (di) { void *m; uint64_t o; if (!mtl_addr(di->ibuf, (uint64_t)n * isz, &m, &o) || !(ib = [(__bridge id<MTLBuffer>)m contents])) return 10; ib += o; }
    for (int k = 0; k < n; k++) {
        int64_t vid = dw ? (int64_t)dw->first + k : (isz == 2 ? (int64_t)((const uint16_t *)ib)[k] : (int64_t)((const uint32_t *)ib)[k]) + di->base_vertex;
        uint64_t off = (uint64_t)vid * sm.stride + at.offset;
        if (vid < 0 || off + 4 * nc > vb.size) return 9;
        void *mb; uint64_t mo; const uint8_t *base;
        if (!mtl_addr(vb.address + off, 4 * nc, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 10;
        float v[4] = {0, 0, 0, 1}; memcpy(v, base + mo, 4 * nc); memcpy(P[k], v, sizeof v);
    }
    return 0;
}
static int first_draw_covers(int i, id<MTLTexture> t, NSUInteger level) {
    const p_draw *dw = g_fd_dw; const p_draw_idx *di = g_fd_di;
    int n = dw ? dw->count : di->count, prim = dw ? dw->prim : di->prim;
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr) return g_fd_why = 1, 0;
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs || !fs || vs->rf.pos_attr < 0 || fs->rf.may_discard || fs->rf.writes_smask || !(fs->rf.color_mask >> i & 1)) return g_fd_why = 2, 0;
    if (!t || t.sampleCount != 1 || t.textureType == MTLTextureType3D) return g_fd_why = 3, 0;
    if ((S.color.blend_en >> i & 1) || ((S.chmask.mask >> (4 * i)) & 0xf) != 0xf || S.ms.a2c || S.poly.mode != NVN_POLYGON_MODE_FILL) return g_fd_why = 4, 0;
    if (R.rt.depth && ((S.ds.dt && S.ds.df != NVN_DEPTH_FUNC_ALWAYS) || S.ds.st)) return g_fd_why = 5, 0;
    if (n < 3 || n > 64) return g_fd_why = 6, 0;
    float P[64][4]; int why = attr_read(vs->rf.pos_attr, dw, di, P);
    if (why) return g_fd_why = why, 0;
    if (vs->rf.pos_w1) for (int k = 0; k < n; k++) P[k][3] = 1.0f;   // 位置 w 為常數 1（不取屬性 .w） | Position w is the constant 1 (the attribute's .w is not taken)
    int sc[4] = {R.sc.x, R.sc.y, R.sc.w, R.sc.h}; float vp[4] = {S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3]};
    g_fd_why = 11;
    return nvn_cover((const float (*)[4])P, n, prim, (int)S.poly.cull, S.poly.front == NVN_FRONT_FACE_CCW,
                     S.has_vp, vp, R.has_sc, sc, (long)MAX(t.width >> level, 1), (long)MAX(t.height >> level, 1));
}

static int state_op(uint32_t op, const void *a) {
    switch (op) {
        case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
        case OP_BIND_PROGRAM: { const prog_t *np = ((const p_bind_program *)a)->prog; if (np != S.prog) { S.prog = np; g_pso_dirty = 1; args_dirty_all(); } break; }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; void *dst = NULL; size_t n = 0;
            blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); dst = &S.blend[bs.target]; n = sizeof bs; break;
                case 1: dst = &S.color; n = sizeof S.color; break;
                case 2: dst = &S.chmask; n = sizeof S.chmask; break;
                case 3: dst = &S.ms; n = sizeof S.ms; break;
                case 4: dst = &S.poly; n = sizeof S.poly; break;
                case 5: dst = &S.ds; n = sizeof S.ds; break;
            }
            if (dst && memcmp(dst, b->bytes, n)) { memcpy(dst, b->bytes, n); if (b->kind == 5) g_dss_dirty = 1; else g_pso_dirty = 1; }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (S.nattr != b->n || memcmp(S.attr, b->st, 4 * b->n)) { memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (S.nstream != b->n || memcmp(S.stream, b->st, 8 * b->n)) { memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16 && memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16 && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) { S.ssbo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage < 6 && b->first + b->n <= 128 && memcmp(&S.tex[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first + b->n <= 128 && memcmp(&S.samp[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) { if (memcmp(&S.img[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } } else RED_ONCE(31, "圖像槽越界 階段 %d 起 %d 數 %d", b->stage, b->first, b->n); break; }
        case OP_STENCIL: {
            const p_stencil *b = a;
            if (b->which != 2) g_dss_dirty = 1;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; }
            break;
        }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; PD("Z %llu 深度夾限 %d", g_stat_pass, S.depth_clamp); break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
        default: return 0;
    }
    return 1;
}

static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl);

// 並行預編的預測（見 pso_get 前的說明）：從當前操作之後掃到本次提交結束，只重放狀態；遇到繪製就按「那時的狀態 + 那個通道的附件格式」算管線鍵，沒建過的丟後台。
// 通道實際掛不掛深度 / 模板要到回放時才定（純深度 / 純模板 / 都不掛的省略），所以每種可能的掛法各算一條鍵——猜多了只是後台白編，不影響結果。
// Prediction for parallel precompilation (see the note before pso_get): scan from after the current operation to the end of this submit, replaying only state; at a draw, compute the pipeline key from "the state at that time + that render pass's attachment formats" and dispatch those not yet built to the background.
// Whether a render pass actually attaches depth / stencil is only decided at replay (depth-only / stencil-only / neither attached are omitted), so one key is computed for each possible attach mode -- guessing too many only means wasted background compiles and does not affect the result.
static void pso_predict_rest(void) {
    if (!g_exec_next) return;
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S);
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    uint32_t cf[8]; memcpy(cf, g_pass.cfmt, sizeof cf); uint32_t df = g_pass.dfmt, sf = g_pass.sfmt, samples = g_pass.samples, dfull = df, sfull = sf; int kicked0 = (int)g_pp_kicked, draws = 0;
    fz_cursor cur = {.p = g_exec_next};
    // 先收齊再丟：按「附件原樣掛」的鍵排在前（最可能用到），其餘掛法排在後——後台併發有限，先丟的先編完
    // Collect them all first, then dispatch: keys for "attachments attached as-is" go first (most likely to be used), the other attach modes after -- background concurrency is limited, and what is dispatched first is compiled first
    enum { QN = 160 }; static struct { pso_key k; void *vs, *fs; } qa[QN], qb[QN]; int na = 0, nb = 0;
    for (int guard = 0; guard < 200000 && na < QN; guard++) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!fz_next(&cur, &q, &hq, &aq)) break;
        if (hq.op == OP_SET_RT) {
            const p_set_rt *r = aq; memset(cf, 0, sizeof cf); dfull = sfull = 0; samples = 1;
            for (int i = 0; i < r->n && i < 8; i++) { id<MTLTexture> t = r->color[i] ? tex_mtl(r->color[i]) : nil; if (t) { cf[i] = (uint32_t)rt_view_fmt(t, &r->cview[i], r->has_cview[i]); samples = (uint32_t)t.sampleCount; } }
            id<MTLTexture> dt = r->depth ? tex_mtl(r->depth) : nil;
            if (dt) { MTLPixelFormat f = dt.pixelFormat; samples = (uint32_t)dt.sampleCount;
                dfull = f != MTLPixelFormatStencil8 ? (uint32_t)f : 0; sfull = (f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8) ? (uint32_t)f : 0; }
            df = dfull; sf = sfull; continue;
        }
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) {
            draws++;
            __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr || !pr->sh[0]) continue;
            __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
            const uint32_t dv[4] = {dfull, 0, 0, dfull}, sv[4] = {sfull, 0, sfull, 0};
            for (int v = 0; v < 4; v++) {
                if (v && dv[v] == dv[0] && sv[v] == sv[0]) continue;
                if (v == 2 && !sfull) continue; if (v == 3 && (!dfull || !sfull)) continue;
                pso_key k; pso_key_fill(&k, vs, fs, cf, dv[v], sv[v], samples, 0);
                if (hm_get_id(&g_pso, &k)) continue;
                int dup = 0; for (int i = 0; i < (v ? nb : na) && !dup; i++) dup = !memcmp(&(v ? qb : qa)[i].k, &k, sizeof k);
                if (dup) continue;
                if (!v && na < QN) { qa[na].k = k; qa[na].vs = (__bridge void *)vs; qa[na].fs = (__bridge void *)fs; na++; }
                else if (v && nb < QN) { qb[nb].k = k; qb[nb].vs = (__bridge void *)vs; qb[nb].fs = (__bridge void *)fs; nb++; }
            }
            continue;
        }
        if (hq.op == OP_CLEAR_COLOR || hq.op == OP_CLEAR_DS) continue;
        state_op(hq.op, aq);
    }
    (void)df; (void)sf;
    for (int i = 0; i < na; i++) pso_pend_kick(&qa[i].k, (__bridge NvShader *)qa[i].vs, (__bridge NvShader *)qa[i].fs);
    for (int i = 0; i < nb; i++) pso_pend_kick(&qb[i].k, (__bridge NvShader *)qb[i].vs, (__bridge NvShader *)qb[i].fs);
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty;
    static int logged; if (g_pp_kicked > (uint64_t)kicked0 && logged++ < 20) MLOG("並行預編：本次提交餘下 %d 次繪製，丟給後台 %d 條管線", draws, (int)g_pp_kicked - kicked0);
}

static void fz_probe_cov2(const p_set_rt *r2, const uint8_t *first, const int8_t *p2map, const uint8_t *clr, int n1, int n) {
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R;
    const p_draw *fdw = g_fd_dw; const p_draw_idx *fdi = g_fd_di; int fdwhy = g_fd_why;
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    op_hdr fh; memcpy(&fh, first, sizeof fh);
    g_fd_dw = fh.op == OP_DRAW ? (const p_draw *)(first + 8) : NULL;
    g_fd_di = fh.op == OP_DRAW_IDX ? (const p_draw_idx *)(first + 8) : NULL;
    R.rt = *r2; R.has_rt = 1;
    fz_cursor cur = {.p = g_exec_next};
    for (;;) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!fz_next(&cur, &q, &hq, &aq)) break;
        if (q == first) break;
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX || hq.op == OP_SET_RT || hq.op == OP_CLEAR_COLOR || hq.op == OP_CLEAR_DS) continue;
        state_op(hq.op, aq);
    }
    for (int j = n1; j < n && j < 8; j++) {
        int k = -1;
        for (int q = 0; q < 8; q++) if (p2map[q] == j) { k = q; break; }
        if (k < 0 || clr[k] || !r2->color[k]) continue;
        uint64_t ser; int level, slice;
        rt_sub(r2, k, &ser, &level, &slice);
        (void)ser; (void)slice;
        g_fz.cov2[j] = (uint8_t)first_draw_covers(k, tex_mtl(r2->color[k]), (NSUInteger)level);
    }
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave;
    g_fd_dw = fdw; g_fd_di = fdi; g_fd_why = fdwhy;
    g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty;
}

static int fz_load_covers(int i, id<MTLTexture> t, NSUInteger level) {
    if (g_fz.active && g_fz.phase == 1 && i >= g_fz.n1) return g_fz.cov2[i];
    return first_draw_covers(i, t, level);
}

// 探針收尾：每個被改成清哨兵色的附件回讀該級 / 層，與同格式 1x1 哨兵像素逐像素比，數剩餘（> 0 即覆蓋證明有誤，報紅）
// Probe wrap-up: for every attachment changed to clear to the sentinel color, read back that level / layer, compare pixel by pixel with a same-format 1x1 sentinel pixel, and count what remains (> 0 means the coverage proof is wrong, report a failure)
static void lp_finish(void) {
    id<MTLCommandBuffer> dcb = q_cb();
    NSArray *ps = g_lp; g_lp = nil;
    for (NSArray *e in ps) {
        id<MTLTexture> t = e[0]; NSUInteger lv = [e[1] unsignedIntegerValue], sl = [e[2] unsignedIntegerValue];
        NSUInteger W = MAX(t.width >> lv, 1), H = MAX(t.height >> lv, 1), bpp = (NSUInteger)(att_bytes(t, lv) / (W * H));
        MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:1 height:1 mipmapped:NO];
        rd.usage = MTLTextureUsageRenderTarget; rd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> ref = [g_dev newTextureWithDescriptor:rd];
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        pd.colorAttachments[0].texture = ref; pd.colorAttachments[0].loadAction = MTLLoadActionClear; pd.colorAttachments[0].storeAction = MTLStoreActionStore;
        pd.colorAttachments[0].clearColor = [e[6] intValue] == 3 ? MTLClearColorMake(NAN, NAN, NAN, NAN) : [e[6] intValue] == 2 ? MTLClearColorMake(0.777, 0.222, 0.555, 0.888) : MTLClearColorMake(0.123, 0.456, 0.789, 0.321);
        [[dcb renderCommandEncoderWithDescriptor:pd] endEncoding];
        id<MTLBuffer> bt = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared], br = [g_dev newBufferWithLength:bpp options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [dcb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:sl sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:bt destinationOffset:0 destinationBytesPerRow:W * bpp destinationBytesPerImage:W * H * bpp];
        [b copyFromTexture:ref sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:br destinationOffset:0 destinationBytesPerRow:bpp destinationBytesPerImage:bpp];
    enc_b_end(b);
        unsigned long long pass = [e[3] unsignedLongLongValue], ser = [e[5] unsignedLongLongValue]; int idx = [e[4] intValue];
        [dcb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
            const uint8_t *p = bt.contents, *r = br.contents; uint64_t left = 0; char at[96] = ""; size_t ao = 0;
            for (NSUInteger k = 0; k < W * H; k++) if (!memcmp(p + k * bpp, r, bpp)) { if (left++ < 4) ao += snprintf(at + ao, sizeof at - ao, " (%lu,%lu)", (unsigned long)(k % W), (unsigned long)(k / W)); }
            if (left) MLOG("【紅】讀入可省探針：通道 %llu c%d ser=%llu lv%lu sl%lu %lux%lu 剩哨兵像素 %llu，前幾個位置%s", pass, idx, ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)W, (unsigned long)H, (unsigned long long)left, at);
            else MLOG("讀入可省探針：通道 %llu c%d ser=%llu lv%lu sl%lu %lux%lu 全覆蓋（哨兵剩 0）", pass, idx, ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)W, (unsigned long)H);
        }];
    }
    [dcb commit];
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {   // 同一組目標：按貼圖序號與視圖比（不比對象指針：遊戲可能按值拷貝貼圖對象） | Same set of targets: compare by texture serial and view (not by object pointer: the game may copy texture objects by value)
    if (a->n != b->n || !a->depth != !b->depth || (a->depth && a->depth->serial != b->depth->serial) || a->has_dview != b->has_dview) return 0;
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return 0;
    for (int k = 0; k < a->n && k < 8; k++) {
        if (!a->color[k] != !b->color[k] || (a->color[k] && a->color[k]->serial != b->color[k]->serial) || a->has_cview[k] != b->has_cview[k]) return 0;
        if (a->has_cview[k] && memcmp(&a->cview[k], &b->cview[k], sizeof a->cview[k])) return 0;
    }
    return 1;
}
// 實驗RT範圍候選：只忽略實際渲染附件未使用的正nlevels差異，其它字段保持逐字相同。
// Experimental RT range candidate: only ignore positive nlevels differences not used by the actual render attachments; other fields stay word-for-word identical.
#define NVN_RT_RANGE_IMPL 1
static int rt_range_shape(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != 1 || b->n != 1 || !a->color[0] || !b->color[0] || !a->has_cview[0] || !b->has_cview[0] ||
        !a->depth != !b->depth || a->has_dview != b->has_dview ||
        (a->has_dview && memcmp(&a->dview,&b->dview,sizeof a->dview))) return 0;
    const tex_t *x=a->color[0], *y=b->color[0];
    if (!x->serial || x->serial != y->serial || x->d.target != NVN_TEXTURE_TARGET_2D || y->d.target != NVN_TEXTURE_TARGET_2D ||
        x->d.samples > 1 || y->d.samples > 1 || (x->has_layout && x->mem) || (y->has_layout && y->mem) ||
        memcmp(&x->d,&y->d,sizeof x->d) || x->mem != y->mem || x->pool != y->pool || x->off != y->off) return 0;
    const view_t *v=&a->cview[0], *w=&b->cview[0]; int levels=x->d.levels;
    if (!v->set_levels || !w->set_levels || v->base_level < 0 || v->base_level >= levels ||
        v->nlevels <= 0 || v->nlevels > levels-v->base_level || w->nlevels <= 0 || w->nlevels > levels-v->base_level ||
        (v->set_layers && (v->min_layer || v->nlayers != 1)) || (v->set_format && v->format != x->d.format) ||
        (v->set_target && v->target != NVN_TEXTURE_TARGET_2D)) return 0;
    view_t cv=*w; cv.nlevels=v->nlevels;
    if (memcmp(v,&cv,sizeof cv)) return 0;
    if (a->depth) { const tex_t *d=a->depth,*e=b->depth;
        if (!d->serial || d->serial != e->serial || d->d.target != NVN_TEXTURE_TARGET_2D || e->d.target != NVN_TEXTURE_TARGET_2D ||
            d->d.samples > 1 || e->d.samples > 1 || (d->has_layout && d->mem) || (e->has_layout && e->mem) ||
            memcmp(&d->d,&e->d,sizeof d->d) || d->mem != e->mem || d->pool != e->pool || d->off != e->off) return 0;
        int dl=a->has_dview && a->dview.set_levels ? a->dview.base_level : 0;
        if (dl < 0 || dl >= d->d.levels || mip_extent(d->d.width,dl) < mip_extent(x->d.width,v->base_level) ||
            mip_extent(d->d.height,dl) < mip_extent(x->d.height,v->base_level)) return 0; }

    return v->nlevels != w->nlevels;
}
#define NVN_RT_RANGE_DEPTH_IMPL 1
static void rt_range_note_depth(void) { if (rt_range_on() && S.ds.dw) g_pass.range_depth_nonro=1; }
static int rt_range_same(const p_set_rt *a, const p_set_rt *b) {
    if (!rt_range_on() || !g_enc || g_fz.active || g_pass.range_external || g_pass.range_depth_nonro || !S.ds.dt || S.ds.dw || S.ds.st || g_enc_mode != 4 || !g_pass.hd || g_pass.hs ||
        (g_vis_on && !g_enc_has_vis) || g_pass.reg_gen != g_reg_gen || g_pass.ser_gen != g_ser_gen || g_pass.range_dirty != nvn_dirty_now() || !rt_range_shape(a,b) || !b->depth) return 0;
    id<MTLTexture> color=tex_mtl(b->color[0]), depth=tex_mtl(b->depth);
    int lv=b->cview[0].base_level;
    return mip_extent(color.width,lv) == g_pass.tw && mip_extent(color.height,lv) == g_pass.th &&
        color == g_pass.range_color && depth == g_pass.range_depth && color.textureType == MTLTextureType2D &&
        depth.textureType == MTLTextureType2D && color.sampleCount == 1 && depth.sampleCount == 1 &&
        g_pass.reg_gen == g_reg_gen && g_pass.ser_gen == g_ser_gen && g_pass.range_dirty == nvn_dirty_now();
}
static int draw_samples_rt(NvProg *pr) {   // 採樣附件保持原ser級保守guard，不分級/層 | Sampled attachments keep the original conservative ser-level guard, not split by level/layer
    uint64_t rs[9]; int nr = 0;
    for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k]) rs[nr++] = R.rt.color[k]->serial;
    if (R.rt.depth) rs[nr++] = R.rt.depth->serial;
    if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 && sh->stage < 6 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 && sh->stage < 6 ? S.tex[sh->stage][b->reg] : 0; else continue;
            int tid = (int)(h & 0xfffff); if (!tid) continue;
            pthread_mutex_lock(&g_rmu); uint64_t ser = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t.serial : 0; pthread_mutex_unlock(&g_rmu);
            for (int j = 0; j < nr; j++) if (ser && ser == rs[j]) return 1; } }
    return 0;
}
// 整通道對深度 / 模板的需求（位：1 深度 2 模板；3 = 兩項都要或證不了）。從開通道的這次繪製起，按真實回放順序掃到通道結束：
// 掛上的清除與本次繪製的狀態先計入；之後的繪製按其深度模板狀態累計；中途清除、嵌套過深等證不了的一律按兩項都要
// The whole render pass's need for depth / stencil (bits: 1 depth 2 stencil; 3 = both needed or cannot be proven). Starting from the draw that opens the render pass, scan in real replay order to the end of the render pass:
// the attached clears and this draw's state are counted first; later draws accumulate by their depth-stencil state; anything unprovable such as a mid-pass clear or nesting too deep is treated as both needed
static int pass_ds_need(void) {
    int need = (R.clear_d || S.ds.dt || S.ds.dw ? 1 : 0) | (R.clear_s || S.ds.st ? 2 : 0);
    if (!R.rt.depth || need == 3 || !g_exec_next || g_ret_n > 16) {
        PD("S %llu 不前瞻 depth=%d clear_d=%d clear_s=%d dt=%d dw=%d st=%d next=%d ret=%d", g_stat_pass + 1, R.rt.depth != NULL, R.clear_d, R.clear_s, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.st, g_exec_next != NULL, g_ret_n);
        return 3;
    }
    ds_st ds = S.ds; g_ds_why = -1; g_ds_scanned = 0; g_ds_need = need;
    int r = ds_scan(g_exec_next, &ds, 0);
    for (int k = g_ret_n - 1; r == DS_RET && k >= 0; k--) r = ds_scan(g_ret[k], &ds, 0);
    for (int k = 0; r == DS_RET && k < g_nrest; k++) r = ds_scan((const uint8_t *)g_rest[k], &ds, 0);
    int out = r == DS_USED ? 3 : g_ds_need;
    PD("S %llu 前瞻 結果=%d 需求=%d（1 深度 2 模板）原因=%d 掃過 %d 條 返回棧 %d 餘句柄 %d", g_stat_pass + 1, r, out, g_ds_why, g_ds_scanned, g_ret_n, g_nrest);
    return out;
}
// 純清除通道前提：開通道時向後掃，形如「本通道若干繪製 → 若干組（SetRenderTargets + 只有清除）→ 回到與本通道相同的目標」，
// 就把這幾組清除提到本通道之前執行、原位置跳過，回到同一目標時由同目標續用接上——本通道的附件不再為夾在中間的清除整塊寫回再讀入
// （選角頁：grunge 與粒子都畫 28 / 27 第 0 級 + 深度，中間只清第 1、2 級）。
// 等價條件（全部滿足才前提，否則照原樣）：掃描區間只有繪製、狀態與這些清除，沒有拷貝 / 派發 / 清貼圖 / 子列表；被清的子資源（貼圖 + 級 + 層）
// 與本通道附件不相交；被清的貼圖既不在當前任一貼圖 / 圖像槽上、掃描區間裡也沒被綁上（保守：不論程序採不採樣）；清除時的剪裁覆蓋整塊（同原位置執行的要求）
// Pure-clear render pass hoisting: when a render pass is opened, scan forward, and if the shape is "several draws of this render pass -> several groups (SetRenderTargets + only clears) -> back to the same targets as this render pass",
// then hoist those groups of clears to execute before this render pass and skip them at their original positions, and on returning to the same targets the same-target continuation picks up -- this render pass's attachments are no longer stored whole and loaded again for the clears sandwiched in between
// (character select page: grunge and particles both draw 28 / 27 level 0 + depth, with only levels 1 and 2 cleared in between).
// Equivalence conditions (hoist only if all hold, otherwise leave as-is): the scan range has only draws, state and these clears, no copies / dispatches / texture clears / sub-lists; the cleared subresources (texture + level + layer)
// are disjoint from this render pass's attachments; the cleared textures are neither on any current texture / image slot nor bound within the scan range (conservative: regardless of whether the program samples them); the scissor at the time of the clear covers the whole block (same requirement as executing at the original position)
static uint64_t h_ser(uint64_t h) { int tid = (int)(h & 0xfffff); return tid ? atomic_load_explicit(&g_tser[tid], memory_order_relaxed) : 0; }
// ---------- 寫回前瞻索引：提交開始按回放順序線性掃一遍，按貼圖記事件；通道結束時每個附件二分找「之後第一個有效事件」定論 ----------
// 取代原「每個通道結束向後整段掃」（通道數 × 命令數，營地每幀 49 通道）。與原掃描逐附件對拍：畫面閘門 17 頁 + 營地局約 34 萬次附件判定（多餘約 4.6 萬）0 不一致後刪去原掃描。
// 事件（seq = 回放序號）：READ 繪製 / 派發按程序反射讀到該貼圖；ATT 繪製時它是當前附件；CLEAR 作附件被整塊清除；COPY / CLRTEX 作拷貝兩端 / 被清貼圖。
// 判定（首個有效事件定論）：READ / COPY / CLRTEX → 需要；ATT 且其 SetRenderTargets 在通道結束之後 → 需要（讀入）；CLEAR 且同上 → 多餘；無事件 → 需要。
// 任何不確定（回放序號與索引對不上、嵌套過深）索引作廢，本次提交餘下通道一律寫回（寫回總是正確的，只是少省帶寬）。
// ---------- Store lookahead index: at the start of a submit do one linear scan in replay order and record events per texture; when a render pass ends, binary-search each attachment for "the first valid event afterwards" to decide ----------
// Replaces the original "scan the whole remainder forward at every render pass end" (pass count x command count, 49 render passes per frame in camp). Cross-checked per attachment against the original scan: after 0 mismatches over the 17 pages of the screenshot gate + a camp run with about 340,000 attachment decisions (about 46,000 redundant), the original scan was removed.
// Events (seq = replay sequence number): READ a draw / dispatch reads this texture per program reflection; ATT it is a current attachment at a draw; CLEAR cleared as a whole block while an attachment; COPY / CLRTEX either end of a copy / the cleared texture.
// Decision (the first valid event decides): READ / COPY / CLRTEX -> needed; ATT whose SetRenderTargets is after the render pass end -> needed (load); CLEAR with the same -> redundant; no event -> needed.
// On any uncertainty (replay sequence number does not match the index, nesting too deep) the index is invalidated and all remaining render passes of this submit store (storing is always correct, it just saves less bandwidth).
typedef struct { uint32_t seq, rtseq; uint64_t ser; const uint8_t *op; uint16_t lv, sl; uint8_t kind, plane, anyls, why; } sdx_ev;   // plane 0 顏色 8 深度模板（ATT）1 深度 2 模板（CLEAR） | plane 0 color 8 depth-stencil (ATT) 1 depth 2 stencil (CLEAR)
// ---- 條件繪製（SetRenderEnable / SetRenderEnableConditional）| Conditional rendering ----
// 與 Maxwell 硬件同：兩種條件比較 addr 與 addr + 16 兩份計數記錄的計數值（每份 16 字節 = 計數 + 時戳），RENDER_IF_EQUAL 相等才畫、RENDER_IF_NOT_EQUAL 不等才畫；
// 關著時繪製與清除都跳過，狀態設置、計數記錄照常。時戳類記錄（TIMESTAMP / TIMESTAMP_TOP）在真機上計數為 0；宿主寫記錄時計數也填了時戳（其他遊戲可能直接讀它），
// 所以判定時按「這地址最後一次是時戳記錄」把計數當 0，內存內容不改。朋友收集畫 Mii 全身像：深度預畫後的清色、清深度包在「不等才畫」裡，
// 兩份計數都是 0 → 跳過，上色那一道才拿得到預畫的深度做「相等」測試；原先宿主恆畫，深度被清成 1，Mii 預覽整個不見。
// 判定在編碼時做（讀前面幀已落地的記錄），同一提交裡排在條件之前的記錄按類型追蹤，不必等它落地
// Same as Maxwell hardware: both conditions compare the counter values of the two counter records at addr and addr + 16 (16 bytes each = counter + timestamp); RENDER_IF_EQUAL draws if equal, RENDER_IF_NOT_EQUAL if not;
// while off, draws and clears are skipped, state setting and counter reports run as usual. Timestamp records (TIMESTAMP / TIMESTAMP_TOP) have counter 0 on hardware; the host also puts the timestamp in the counter when writing a record (other games may read it directly),
// so the evaluation treats the counter as 0 when the address was last written by a timestamp record, without changing memory. Tomodachi Life's full-body Mii picture: the color and depth clears after the depth prepass sit inside "draw if not equal";
// both counters are 0 → skipped, so the shading pass gets the prepass depth for its "equal" test; the host used to always draw, depth was cleared to 1, and the Mii preview vanished entirely.
// The evaluation happens at encode time (reading records landed by earlier frames); records earlier in the same submission are tracked by type, without waiting for them to land
enum { CR_TAB = 1024 };
static uint64_t g_cr_addr[CR_TAB]; static uint8_t g_cr_ts[CR_TAB];
static void cr_note(uint64_t addr, uint32_t type) {
    if (!addr) return;
    for (uint32_t i = (uint32_t)(addr >> 4) * 2654435761u % CR_TAB, n = 0; n < CR_TAB; i = (i + 1) % CR_TAB, n++)
        if (!g_cr_addr[i] || g_cr_addr[i] == addr) { g_cr_addr[i] = addr; g_cr_ts[i] = type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP; return; }
}
static uint64_t cr_counter(uint64_t addr) {
    if (!addr) return 0;
    for (uint32_t i = (uint32_t)(addr >> 4) * 2654435761u % CR_TAB, n = 0; n < CR_TAB && g_cr_addr[i]; i = (i + 1) % CR_TAB, n++)
        if (g_cr_addr[i] == addr) { if (g_cr_ts[i]) return 0; break; }
    uint64_t v; memcpy(&v, (const void *)addr, 8); return v;
}
static int g_render_on = 1;   // 回放時當前的繪製開關 | the current render enable during replay
static int cr_eval(const p_render_en *e) {
    if (e->kind <= 1) return e->kind;
    uint64_t a = cr_counter(e->addr), b = cr_counter(e->addr + 16);
    return e->kind == 2 ? a == b : a != b;
}
static int g_sdx_gate = 1;    // 寫回前瞻掃描時的繪製開關（同一套判定，與回放一致） | render enable during the store lookahead scan (same evaluation, consistent with replay)
enum { SDX_READ = 1, SDX_NEED, SDX_ATT, SDX_CLEAR };
#define SDX_NONE 0xffffffffu
static sdx_ev *g_sdx, *g_sdx_tmp; static uint32_t g_sdx_n, g_sdx_cap; static int g_sdx_ok;   // 索引有效 | Index valid
// 候選貼圖（當過附件的）→ 桶：開放定址；事件發生時尚未當過附件的貼圖，對此前結束的任何通道都無關，不記
// Candidate textures (those that have been attachments) -> buckets: open addressing; a texture that has not yet been an attachment when an event occurs is irrelevant to any render pass that ended before then, so it is not recorded
#define SDX_HB 2048
static uint64_t g_sdx_hk[SDX_HB]; static uint16_t g_sdx_hb[SDX_HB]; static uint32_t g_sdx_nb, g_sdx_bcnt[SDX_HB + 1], g_sdx_bst[SDX_HB + 1];
static int sdx_bucket(uint64_t ser, int add) {   // 返回桶號；不在集合（且不加）返回 −1；滿了作廢索引 | Returns the bucket number; returns -1 if not in the set (and not adding); invalidates the index when full
    if (!ser) return -1;
    for (uint32_t i = (uint32_t)(ser * 0x9E3779B97F4A7C15ull >> 53), n = 0; n < SDX_HB; i = (i + 1) & (SDX_HB - 1), n++) {
        if (g_sdx_hk[i] == ser) return g_sdx_hb[i];
        if (!g_sdx_hk[i]) { if (!add) return -1; if (g_sdx_nb >= SDX_HB / 2) { g_sdx_ok = 0; return -1; } g_sdx_hk[i] = ser; g_sdx_hb[i] = (uint16_t)g_sdx_nb; return (int)g_sdx_nb++; }
    }
    return -1;
}
static const uint8_t **g_sdx_op; static uint32_t g_sdx_nop, g_sdx_opcap;
static uint32_t g_exec_seq;   // 回放中當前操作的序號 | Sequence number of the current operation during replay
static uint64_t g_pp_slot[6][160]; static const prog_t *g_pp_prog; static const p_set_rt *g_pp_rt; static uint32_t g_pp_rtseq;
static uint32_t *g_rtx_seq, *g_drx_seq; static uint32_t g_rtx_n, g_rtx_cap, g_drx_n, g_drx_cap;   // 本次提交的 SET_RT / 繪製序號表（可讀 tile 合併的預篩用） | This submit's SET_RT / draw sequence number tables (used by the pre-screen of the readable tile merge)
static void seq_push(uint32_t **v, uint32_t *n, uint32_t *cap, uint32_t x) { if (*n == *cap) { *cap = *cap ? *cap * 2 : 1024; *v = realloc(*v, *cap * sizeof **v); } (*v)[(*n)++] = x; }
static void sdx_ev_add(sdx_ev e) { if (sdx_bucket(e.ser, 0) < 0) return; if (g_sdx_n == g_sdx_cap) { g_sdx_cap = g_sdx_cap ? g_sdx_cap * 2 : 4096; g_sdx = realloc(g_sdx, g_sdx_cap * sizeof *g_sdx); g_sdx_tmp = realloc(g_sdx_tmp, g_sdx_cap * sizeof *g_sdx); } g_sdx[g_sdx_n++] = e; }
static void sdx_lvsl(const view_t *v, int hv, uint16_t *lv, uint16_t *sl) { *lv = (uint16_t)(hv && v->set_levels ? v->base_level : 0); *sl = (uint16_t)(hv && v->set_layers ? v->min_layer : 0); }
static void sdx_walk(const uint8_t *p, int depth) {
    if (depth > 64) { g_sdx_ok = 0; return; }
    for (uint32_t guard = 0; guard < 4000000 && p && g_sdx_ok; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (g_sdx_nop == g_sdx_opcap) { g_sdx_opcap = g_sdx_opcap ? g_sdx_opcap * 2 : 16384; g_sdx_op = realloc(g_sdx_op, g_sdx_opcap * sizeof *g_sdx_op); }
        uint32_t seq = g_sdx_nop; g_sdx_op[g_sdx_nop++] = p;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); sdx_walk((const uint8_t *)t, depth + 1); break; }
            case OP_SET_RT: { const p_set_rt *r = a; g_pp_rt = r; g_pp_rtseq = seq; seq_push(&g_rtx_seq, &g_rtx_n, &g_rtx_cap, seq);
                for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) sdx_bucket(r->color[k]->serial, 1);
                if (r->depth) sdx_bucket(r->depth->serial, 1);
                break; }
            case OP_RENDER_EN: g_sdx_gate = cr_eval(a); break;
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); cr_note(ad, (uint32_t)t); break; }
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; const p_set_rt *r = g_pp_rt;
                if (!g_sdx_gate) break;   // 條件繪製關著：這次清除不會發生 | conditional rendering is off: this clear does not happen
                if (!r || (c->mask & 0xf) != 0xf || c->index < 0 || c->index >= r->n || c->index >= 8 || !r->color[c->index]) break;
                uint16_t lv, sl; sdx_lvsl(&r->cview[c->index], r->has_cview[c->index], &lv, &sl); int last = -1;   // 同一子資源掛在多個序號時按最後一個算 | When the same subresource is attached at several indices, the last one counts
                for (int k = 0; k < r->n && k < 8; k++) { uint16_t l2, s2; if (!r->color[k] || r->color[k]->serial != r->color[c->index]->serial) continue; sdx_lvsl(&r->cview[k], r->has_cview[k], &l2, &s2); if (l2 == lv && s2 == sl) last = k; }
                if (last == c->index) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[c->index]->serial, p, lv, sl, SDX_CLEAR, 0, 0, 0});
                break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; const p_set_rt *r = g_pp_rt; if (!r || !r->depth || !g_sdx_gate) break;
                uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl);
                if (c->depth_mask) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 1, !r->has_dview, 0});
                if ((c->stencil_mask & 0xff) == 0xff) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 2, !r->has_dview, 0});
                break; }
            case OP_BIND_TEX: case OP_BIND_IMG: { const p_bind_handles *b = a; int base = h.op == OP_BIND_IMG ? 128 : 0, lim = h.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) g_pp_slot[b->stage][base + b->first + k] = h_ser(b->h[k]);
                break; }
            case OP_BIND_PROGRAM: g_pp_prog = ((const p_bind_program *)a)->prog; break;
            case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: {
                if (h.op != OP_DISPATCH) seq_push(&g_drx_seq, &g_drx_n, &g_drx_cap, seq);
                __unsafe_unretained NvProg *pr = g_pp_prog ? prog_get(g_pp_prog) : nil;
                if (!pr) { for (int st = 0; st < 6; st++) for (int k = 0; k < 160; k++) if (g_pp_slot[st][k]) sdx_ev_add((sdx_ev){seq, 0, g_pp_slot[st][k], p, 0, 0, SDX_READ, 0, 0, 1}); }
                else for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || (h.op == OP_DISPATCH) != (st == 5)) continue;
                    for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; int k = b->kind == NVN_RB_IMG ? 128 + b->reg : (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) ? b->reg : -1;
                        if (k < 0 || k >= 160 || sh->stage < 0 || sh->stage >= 6) continue; uint64_t ser = g_pp_slot[sh->stage][k];
                        if (ser) sdx_ev_add((sdx_ev){seq, 0, ser, p, 0, 0, SDX_READ, 0, 0, 1}); } }
                const p_set_rt *r = g_pp_rt;
                if (h.op != OP_DISPATCH && r) {
                    for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) { uint16_t lv, sl; sdx_lvsl(&r->cview[k], r->has_cview[k], &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[k]->serial, p, lv, sl, SDX_ATT, 0, 0, 2}); }
                    if (r->depth) { uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_ATT, 8, !r->has_dview, 2}); }
                }
                break; }
            case OP_COPY_T2T: { const p_copy_t2t *c = a; if (c->src) sdx_ev_add((sdx_ev){seq, 0, c->src->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); if (c->dst) sdx_ev_add((sdx_ev){seq, 0, c->dst->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_COPY_T2B: case OP_COPY_B2T: { const p_copy_b2t *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_CLEAR_TEX: { const p_clear_tex *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 4}); break; }
            default: break;
        }
        p += h.size;
    }
}
void mtl_sub_index(const uint64_t *h, int n) {   // 提交開始（hle_nvn.c QueueSubmitCommands）：建本次提交的寫回前瞻索引 | Start of submit (hle_nvn.c QueueSubmitCommands): build this submit's store lookahead index
    g_xv_ntouch = 0; g_sdx_n = g_sdx_nop = 0; g_rtx_n = g_drx_n = 0; g_exec_seq = 0; g_sdx_ok = tbdr_on();
    if (!g_sdx_ok) { g_xv_n = 0; g_xv_block = 1; return; }   // 索引不可用：賬本清空、本次提交不新增（沒法核下一次觸碰） | Index unavailable: clear the ledger and add nothing in this submit (the next touch cannot be verified)
    g_xv_block = 0;
    memset(g_sdx_hk, 0, sizeof g_sdx_hk); g_sdx_nb = 0;
    for (int i = 0; i < g_xv_n; i++) sdx_bucket(g_xv[i].ser, 1);   // 賬本裡的貼圖進候選集：否則它在本次提交裡只被採樣、不作附件時讀事件不入索引，兜底看不見 | Textures in the ledger enter the candidate set: otherwise, when one is only sampled and not used as an attachment in this submit, its read events are not indexed and the fallback cannot see them
    if (R.has_rt) { for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k]) sdx_bucket(R.rt.color[k]->serial, 1); if (R.rt.depth) sdx_bucket(R.rt.depth->serial, 1); }   // 提交開始時的目標（首個繪製可能不經 SetRenderTargets 重開通道） | The targets at the start of the submit (the first draw may reopen the render pass without going through SetRenderTargets)
    g_pp_prog = S.prog; g_pp_rt = NULL; g_pp_rtseq = SDX_NONE;
    for (int st = 0; st < 6; st++) { for (int k = 0; k < 128; k++) g_pp_slot[st][k] = S.tex[st][k] ? h_ser(S.tex[st][k]) : 0;
                                     for (int k = 0; k < 32; k++) g_pp_slot[st][128 + k] = S.img[st][k] ? h_ser(S.img[st][k]) : 0; }
    g_sdx_gate = g_render_on;
    for (int i = 0; i < n && g_sdx_ok; i++) sdx_walk((const uint8_t *)h[i], 0);
    if (!g_sdx_ok) return;
    // 計數排序到桶：事件本按序號追加，同一序號內 READ 先於 ATT（同一繪製先判讀後判附件），穩定散列後桶內有序
    // Counting sort into buckets: events are appended in sequence-number order, and within the same sequence number READ precedes ATT (the same draw is judged for reads first, then attachments), so after a stable scatter each bucket is ordered
    memset(g_sdx_bcnt, 0, (g_sdx_nb + 1) * sizeof *g_sdx_bcnt);
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++;
    for (uint32_t b = 0, acc = 0; b <= g_sdx_nb; b++) { g_sdx_bst[b] = acc; acc += b < g_sdx_nb ? g_sdx_bcnt[b] : 0; }
    for (uint32_t b = 0; b < g_sdx_nb; b++) g_sdx_bcnt[b] = g_sdx_bst[b];
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_tmp[g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++] = g_sdx[i];
    sdx_ev *t = g_sdx; g_sdx = g_sdx_tmp; g_sdx_tmp = t;
    xv_forward();
}
static void sdx_seq(const uint8_t *p) {   // mtl_exec 每條操作：序號與索引核對（不符即作廢，餘下一律寫回） | Every operation in mtl_exec: check the sequence number against the index (on mismatch invalidate, and store everything for the rest)
    if (!g_sdx_ok) return;
    if (g_exec_seq >= g_sdx_nop || g_sdx_op[g_exec_seq] != p) { RED_ONCE(40, "寫回前瞻索引與回放順序不符（序號 %u / %u），本次提交餘下通道一律寫回", g_exec_seq, g_sdx_nop); g_sdx_ok = 0; return; }
    g_exec_seq++;
}
static int sdx_decide(uint8_t st[9]) {   // st[i]：2 多餘；否則 why << 2（需要的原因，同 sd_need）。返回 0 = 索引不可用 | st[i]: 2 redundant; otherwise why << 2 (the reason it is needed, same as sd_need). Returns 0 = index unavailable
    if (!g_sdx_ok || !g_exec_cur || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 0;
    uint32_t P = g_exec_seq - 1;
    for (int i = 0; i < g_nsd; i++) { const sd_att *t = &g_sd[i]; st[i] = 5 << 2;
        int b = sdx_bucket(t->ser, 0); if (b < 0) continue;   // 不在候選集：本提交裡沒有任何相關事件 → 掃到末尾未定，需要 | Not in the candidate set: no related event at all in this submit -> undecided after scanning to the end, needed
        uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;   // 桶內第一個 seq ≥ P | First seq >= P in the bucket
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq < P) lo = m + 1; else hi = m; }
        for (uint32_t j = lo; j < end; j++) { const sdx_ev *e = &g_sdx[j];
            if (g_hoist_n && hoist_skipped(e->op)) continue;
            if (e->kind == SDX_READ || e->kind == SDX_NEED) { st[i] = (uint8_t)(e->why << 2); break; }
            if (e->rtseq == SDX_NONE || e->rtseq < P || (g_hoist_n && hoist_skipped(g_sdx_op[e->rtseq]))) continue;   // 該目標在通道結束之前設的：通道結束之後未重設為附件，不算 | This target was set before the render pass ended: not re-set as an attachment after the render pass ended, so it does not count
            int hit = t->depth ? (e->kind == SDX_ATT ? e->plane == 8 : e->plane == t->depth) && (e->anyls || (e->lv == t->lv && e->sl == t->sl))
                               : e->plane == 0 && e->lv == t->lv && e->sl == t->sl;
            if (!hit) continue;
            st[i] = e->kind == SDX_CLEAR ? 2 : (uint8_t)(2 << 2); break; }
    }
    return 1;
}
static void xv_forward(void) {
    if (!g_xv_n) return;
    if (!g_sdx_ok) { g_xv_n = 0; g_xv_block = 1; return; }
    for (int i = 0; i < g_xv_n;) {
        xv_ent *e = &g_xv[i]; int b = sdx_bucket(e->ser, 0), action = 0;
        if (b >= 0) for (uint32_t j = g_sdx_bst[b]; j < g_sdx_bst[b + 1]; j++) { sdx_ev *v = &g_sdx[j];
            if (v->kind == SDX_READ || v->kind == SDX_NEED) { xv_ent copy = *e; xv_violation(&copy, "提交前瞻讀/需求"); action = 2; break; }
            if (v->lv != e->lv || v->sl != e->sl) continue;
            if (v->kind == SDX_CLEAR) { xv_good(e); action = 1; break; }
            if (v->kind == SDX_ATT) break;
        }
        if (!action) i++;
    }
}
static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl) {   // 附件 k（0–7 顏色，8 深度）的子資源 | Subresource of attachment k (0–7 color, 8 depth)
    const tex_t *t = k == 8 ? r->depth : r->color[k]; const view_t *v = k == 8 ? &r->dview : &r->cview[k]; int hv = k == 8 ? r->has_dview : r->has_cview[k];
    *ser = t ? t->serial : 0; *lv = hv && v->set_levels ? v->base_level : 0; *sl = hv && v->set_layers ? v->min_layer : 0;
}
static void hoist_try(void) {
    if (g_hoist_n || !g_exec_next) return;
    typedef struct { const uint8_t *rt_op; const p_set_rt *rt; uint8_t clear_c[8]; MTLClearColor cc[8]; uint8_t clear_d, clear_s; double cd; uint32_t cs;
                     p_scissor sc; int has_sc; const uint8_t *ops[8]; int nops; } grp;
    grp G[4]; int ng = 0, in_grp = 0; p_scissor sc = R.sc; int has_sc = R.has_sc;
    // 「被清的貼圖不在任一槽上」要的綁定集合：只在真找到可前提的清除組、回到本通道目標時才查表（絕大多數通道走不到那一步）。
    // 掃描區間裡綁上的句柄先記下，查表時與當前槽一並換成貼圖序號；掃描只讀命令，期間當前槽 S 不變，結果與開頭就查相同
    // The binding set needed for "the cleared texture is not on any slot": the table is looked up only when a hoistable clear group is really found and we are back to this render pass's targets (the vast majority of render passes never get that far).
    // Handles bound within the scan range are noted first and converted to texture serials together with the current slots at lookup time; the scan only reads commands and the current slots S do not change meanwhile, so the result is the same as checking at the start
    uint64_t bh[512]; int nbh = 0;
    const uint8_t *p = g_exec_next;
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        switch (h.op) {
            case OP_END: case OP_CALL: case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_RENDER_EN: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_DRAW: case OP_DRAW_IDX: if (in_grp) return; break;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_BIND_TEX: case OP_BIND_IMG: { const p_bind_handles *b = a; for (int k = 0; k < b->n && k < 32 && nbh < 512; k++) if (b->h[k]) bh[nbh++] = b->h[k]; break; }
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: {
                if (!in_grp || G[ng - 1].nops >= 8) return;
                grp *g = &G[ng - 1];
                if (h.op == OP_CLEAR_COLOR) { const p_clear_color *c = a;
                    if (c->index < 0 || c->index >= g->rt->n || !g->rt->color[c->index] || c->mask != 0xf) return;
                    const tex_t *t = g->rt->color[c->index];
                    if (has_sc && !(sc.x <= 0 && sc.y <= 0 && sc.x + sc.w >= (int)t->d.width && sc.y + sc.h >= (int)t->d.height)) return;
                    g->clear_c[c->index] = 1; g->cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]); }
                else { const p_clear_ds *c = a; if (!g->rt->depth) return;
                    if (c->depth_mask) { g->clear_d = 1; g->cd = c->depth; } if (c->stencil_mask) { g->clear_s = 1; g->cs = (uint32_t)c->stencil; } }
                g->sc = sc; g->has_sc = has_sc; g->ops[g->nops++] = p; break; }
            case OP_SET_RT: { const p_set_rt *r = a;
                if (rt_same(&R.rt, r)) {   // 回到本通道的目標：前提成立的話執行 | Back to this render pass's targets: execute if hoisting holds
                    if (!ng) return;
                    uint64_t bound[512]; int nb = 0;   // 原「開頭即查」的順序：當前各槽（階段 × 貼圖 128 + 圖像 32），再接掃描區間裡綁上的 | The order of the original "check at the start": the current slots (stage x textures 128 + images 32), followed by those bound within the scan range
                    for (int st = 0; st < 6; st++) { for (int k = 0; k < 128 && nb < 512; k++) if (S.tex[st][k]) bound[nb++] = h_ser(S.tex[st][k]);
                                                     for (int k = 0; k < 32 && nb < 512; k++) if (S.img[st][k]) bound[nb++] = h_ser(S.img[st][k]); }
                    for (int k = 0; k < nbh && nb < 512; k++) bound[nb++] = h_ser(bh[k]);
                    for (int gi = 0; gi < ng; gi++) {   // 不相交、未被綁 | Disjoint and not bound
                        grp *g = &G[gi]; if (!g->nops) return;
                        for (int k = 0; k < 9; k++) { if (k < 8 ? !g->clear_c[k] : !(g->clear_d || g->clear_s)) continue;
                            uint64_t ser; int lv, sl; rt_sub(g->rt, k, &ser, &lv, &sl); if (!ser) return;
                            for (int j = 0; j < nb; j++) if (bound[j] == ser) return;
                            for (int m = 0; m < 9; m++) { uint64_t s2; int l2, sl2; if (m < 8 && m >= R.rt.n) continue; rt_sub(&R.rt, m, &s2, &l2, &sl2);
                                if (s2 == ser && l2 == lv && sl2 == sl) return; } } }
                    // 先登記全部跳過項再執行：前提通道結束時寫回前瞻就往後掃，原位置的清除必須已在跳過集合裡
                    // Register all skip entries first, then execute: when a hoisted render pass ends the store lookahead scans forward, and the clears at their original positions must already be in the skip set
                    for (int gi = 0; gi < ng; gi++) { g_hoist_skip[g_hoist_n++] = G[gi].rt_op; for (int k = 0; k < G[gi].nops; k++) g_hoist_skip[g_hoist_n++] = G[gi].ops[k]; }
                    p_set_rt rt0 = R.rt; p_scissor sc0 = R.sc; int hs0 = R.has_sc;   // 本通道的目標、剪裁與掛起的清除：執行完原樣恢復 | This render pass's targets, scissor and pending clears: restored as they were after execution
                    uint8_t cc0f[8]; MTLClearColor cc0[8]; uint8_t cd0f = R.clear_d, cs0f = R.clear_s; double cd0 = R.cd; uint32_t cs0 = R.cs;
                    memcpy(cc0f, R.clear_c, 8); memcpy(cc0, R.cc, sizeof cc0);
                    for (int gi = 0; gi < ng; gi++) {   // 執行：臨時換上該組目標與清除，開只為清除的通道 | Execute: temporarily swap in that group's targets and clears and open a clear-only render pass
                        grp *g = &G[gi];
                        R.rt = *g->rt; memcpy(R.clear_c, g->clear_c, 8); memcpy(R.cc, g->cc, sizeof R.cc); R.clear_d = g->clear_d; R.clear_s = g->clear_s; R.cd = g->cd; R.cs = g->cs;
                        R.sc = g->sc; R.has_sc = g->has_sc;
                        PD("H %llu 前提純清除通道（原位置在本通道之後）", g_stat_pass + 1);
                        rp_begin_ex(1); rp_end();
                    }
                    R.rt = rt0; R.sc = sc0; R.has_sc = hs0; memcpy(R.clear_c, cc0f, 8); memcpy(R.cc, cc0, sizeof cc0); R.clear_d = cd0f; R.clear_s = cs0f; R.cd = cd0; R.cs = cs0;
                    if (!g_hoist_groups) MLOG("純清除通道前提：首例 %d 組（通道 %llu 前）", ng, g_stat_pass + 1);
                    g_hoist_groups += (uint64_t)ng;
                    return;
                }
                if (in_grp && !G[ng - 1].nops) return;   // 上一組沒有清除（只設目標）：不認 | The previous group has no clear (only set targets): not recognized
                if (ng >= 4) return;
                memset(&G[ng], 0, sizeof G[ng]); G[ng].rt_op = p; G[ng].rt = r; ng++; in_grp = 1; break; }
            default: break;
        }
        p += h.size;
    }
}
// ---------- 可讀 tile 通道合併（合併 B 第 2 步） ----------
// 後一個通道的繪製在本像素採樣前一個通道寫的顏色附件時，兩個通道並成一個渲染通道：前一個通道寫的內容留在 tile 裡，
// 後一個通道用可讀 tile 版本（<鍵>_bf，函數常量特化）直接讀，不為被採樣而先落主存再讀回。
// 規劃（fuse_try，前一個通道開頭）只做結構檢查；本像素尋址的閉式證明在後一個通道每次繪製時做（fz_p2_draw），證不出就結束合併通道、
// 按後一個通道的原目標重開普通通道（前一個通道的結果照常寫回，等價於不合併）。
// ---------- Readable tile render pass merge (merge B step 2) ----------
// When a draw of the later pass samples, at the same pixel, a color attachment written by the earlier pass, the two passes are merged into one render pass: what the earlier pass wrote stays in the tile,
// and the later pass reads it directly with the readable tile version (<key>_bf, function-constant specialization), instead of first landing in main memory and being read back just to be sampled.
// Planning (fuse_try, at the start of the earlier pass) does only structural checks; the closed-form proof of same-pixel addressing is done at every draw of the later pass (fz_p2_draw), and if it cannot be proven the merged render pass is ended and
// a normal render pass is reopened with the later pass's original targets (the earlier pass's result is stored as usual, equivalent to not merging).
static void fz_unskip(void) { for (int i = 0; i < g_fz.nskip; i++) for (int j = 0; j < g_hoist_n; j++) if (g_hoist_skip[j] == g_fz.skip[i]) { g_hoist_skip[j] = g_hoist_skip[--g_hoist_n]; break; } }
static int fz_reuse_op(const uint8_t *p) { for (int i = 0; i < g_fz.nreuse; i++) if (g_fz.reuse[i] == p) return 1; return 0; }
// 診斷 DM_NVN_FUSE_CHECK=1：局內雙渲染。後一個通道證得出的繪製逐次留狀態快照；合併通道結束後，把後一個通道寫過的附件拷一份（合併路徑的結果），
// 再關掉全部前瞻、按後一個通道的原目標與原清除用普通路徑逐次重畫，兩份逐字節比（同一幀同一輸入，不依賴跨局確定性）。重畫結果留在附件上（相同即無影響）
// Diagnostic DM_NVN_FUSE_CHECK=1: in-run double rendering. For the later pass's draws that can be proven, keep a state snapshot per draw; after the merged render pass ends, make a copy of the attachments the later pass wrote (the merge path's result),
// then turn off all lookahead and redraw one by one through the normal path with the later pass's original targets and original clears, and compare the two byte by byte (same frame, same input, no dependence on cross-run determinism). The redraw result is left on the attachment (no effect if identical)
static int fz_check_on(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_FUSE_CHECK") != NULL; return v; }
typedef struct { int idx; p_draw dw; p_draw_idx di; p_scissor sc; int has_sc; void *S_; } fz_snap;
static fz_snap g_fzs[8]; static int g_nfzs, g_fzs_bad, g_fz_checking; static uint64_t g_fzc_same, g_fzc_diff;
static void draw(const p_draw *dw, const p_draw_idx *di);
static void fz_snap_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_nfzs >= 8) { g_fzs_bad = 1; return; }
    fz_snap *z = &g_fzs[g_nfzs++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di; z->sc = R.sc; z->has_sc = R.has_sc;
    z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S);
}
static void fz_check_run(const fuse_t *fz) {
    if (!g_nfzs || g_fzs_bad) { if (g_fzs_bad) MLOG("【紅】局內雙渲染：後一個通道繪製超過 8 次，未比"); goto out; }
    {
    id<MTLTexture> att[8], scr[8]; uint64_t sers[8]; int na = 0;
    // fz_end 已把 R.rt 從物理並集還原為後通道邏輯目標；wrote 仍按物理槽登記，必須經 p2map 對位。
    // fz_end has already restored R.rt from the physical union to the later pass's logical targets; wrote is still registered by physical slot and must be aligned through p2map.
    for (int j = 0; j < fz->rt2.n && j < 8; j++) if (fz->p2map[j] >= 0 && fz->wrote[fz->p2map[j]] && fz->rt2.color[j]) {
        id<MTLTexture> t = tex_mtl(fz->rt2.color[j]); if (!t) continue;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d]; if (!c) continue;
        enc_b_copy_texture(blit(), t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(t.width, t.height, 1), c, 0, 0, MTLOriginMake(0, 0, 0));
        att[na] = t; scr[na] = c; sers[na] = fz->rt2.color[j]->serial; na++; }
    if (!na) { MLOG("【紅】局內雙渲染：已留 %d 次繪製快照，但沒有可比附件", g_nfzs); goto out; }
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R; const uint8_t *en0 = g_exec_next;
    g_fz_checking = 1; g_exec_next = NULL;   // 重畫時關掉全部前瞻（續用 / 前提 / 合併 / 深度模板按需）：普通路徑 | Turn off all lookahead during the redraw (continuation / hoisting / merging / depth-stencil on demand): normal path
    R.rt = fz->rt2; memcpy(R.clear_c, fz->clr2, 8); memcpy(R.cc, fz->cc2, sizeof R.cc); R.clear_d = fz->cd2f; R.clear_s = fz->cs2f; R.cd = fz->cd2; R.cs = fz->cs2;
    for (int i = 0; i < g_nfzs; i++) { fz_snap *z = &g_fzs[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1;
        draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
    rp_end();
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave; g_exec_next = en0; g_fz_checking = 0; g_pso_dirty = g_dss_dirty = 1;
    for (int a = 0; a < na; a++) {
        id<MTLTexture> t = att[a], c = scr[a]; NSUInteger W = t.width, H = t.height, bpp = (NSUInteger)(att_bytes(t, 0) / (W * H));
        id<MTLBuffer> b1 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared], b2 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared];
        id<MTL4ComputeCommandEncoder> b = blit();
        enc_b_copy_texture_buffer(b, c, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b1, 0, W * bpp, W * H * bpp);
        enc_b_copy_texture_buffer(b, t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b2, 0, W * bpp, W * H * bpp);
        unsigned long long ser = sers[a], pass = g_stat_pass, frame = atomic_load(&g_cur_frame); NSUInteger nb = W * H * bpp;
        [g_commit_blocks addObject:^{
            const uint8_t *x = b1.contents, *y = b2.contents; uint64_t dpx = 0, maxd = 0;
            for (NSUInteger k = 0; k < nb; k += bpp) if (memcmp(x + k, y + k, bpp)) { dpx++; for (NSUInteger q = 0; q < bpp; q++) { uint64_t dd = (uint64_t)abs((int)x[k + q] - (int)y[k + q]); if (dd > maxd) maxd = dd; } }
            if (dpx) { g_fzc_diff++; MLOG("【紅】局內雙渲染：幀 %llu 通道 %llu 附件 ser=%llu 合併 vs 普通 不同像素 %llu / %lu（逐字節最大差 %llu）", frame, pass, ser, (unsigned long long)dpx, (unsigned long)(nb / bpp), (unsigned long long)maxd); }
            else { g_fzc_same++; MLOG("局內雙渲染：幀 %llu 通道 %llu 附件 ser=%llu 合併 vs 普通 逐字節相同（%lu 像素）", frame, pass, ser, (unsigned long)(nb / bpp)); }
            if ((g_fzc_same + g_fzc_diff) % 300 == 0) MLOG("局內雙渲染累計：逐字節相同 %llu 次、不同 %llu 次", (unsigned long long)g_fzc_same, (unsigned long long)g_fzc_diff);
        }];
    }
    }
out:
    for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_);
    g_nfzs = 0; g_fzs_bad = 0;
}
static void fz_end(void) {   // 合併通道結束（rp_end 調）：還沒進後一個通道就被打斷 → 撤回跳過項、目標還原成前一個通道的；否則還原成後一個通道的 | End of a merged render pass (called by rp_end): interrupted before entering the later pass -> retract the skip entries and restore the targets to the earlier pass's; otherwise restore to the later pass's
    if (!g_fz.active) return;
    if (g_fz.phase == 1) { fz_unskip(); R.rt = g_fz.rt1; } else R.rt = g_fz.rt2;
    g_fz.active = 0;
    if (g_fz.phase == 2 && fz_check_on() && !g_fz_checking) { fuse_t fz = g_fz; R.rt = fz.rt2; fz_check_run(&fz); }
    else { for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_); g_nfzs = 0; g_fzs_bad = 0; }
}
static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb) { uint64_t s1, s2; int l1, l2, x1, x2; rt_sub(a, ka, &s1, &l1, &x1); rt_sub(b, kb, &s2, &l2, &x2); return s1 && s1 == s2 && l1 == l2 && x1 == x2; }
static int prog_samples_ser(const prog_t *pg, uint64_t tex[6][128], uint64_t img[6][32], uint64_t ser) {   // 程序反射聲明的貼圖 / 圖像 / 類型化緩衝槽裡有沒有綁著該貼圖 | Whether this texture is bound in any texture / image / typed buffer slot declared by the program's reflection
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? tex[sh->stage][b->reg] : 0; else continue;
            if (h && h_ser(h) == ser) return 1; } }
    return 0;
}
static int g_defer_env_ab, g_defer_env_off;   // 環境開關：每次提交開始（defer_reset）讀一次，熱路徑不調 getenv | Environment switches: read once at the start of each submit (defer_reset); the hot path does not call getenv
static void defer_env(void) { const char *x = getenv("DM_NVN_DEFER_AB"), *e = getenv("DM_NVN_DEFER"); g_defer_env_ab = x && !strcmp(x, "1"); g_defer_env_off = e && !strcmp(e, "0"); }
static int defer_on(void) {
    if (!g_defer_env_ab) return tbdr_on() && !g_defer_env_off;
    if (g_defer_ab < 0) { g_defer_ab = 0; g_defer_ab_on = 0; }
    return tbdr_on() && (g_defer_ab || g_defer_ab_on); }
static int defer_handle_sub(uint64_t h, const p_set_rt *r, int k, int cover) {
    int tid = (int)(h & 0xfffff); if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    uint64_t ser; int lv, sl; rt_sub(r, k, &ser, &lv, &sl); if (!ser || g_treg[tid].t.serial != ser) return 0;
    if (!g_treg[tid].has_v || !g_treg[tid].v.set_levels) return cover;
    view_t v = g_treg[tid].v; int n = v.nlevels > 0 ? v.nlevels : 1;
    return lv >= v.base_level && lv < v.base_level + n && (!v.set_layers || (sl >= v.min_layer && sl < v.min_layer + (v.nlayers > 0 ? v.nlayers : 1)));
}
static int defer_prog_hits(const prog_t *pg, uint64_t tex[6][128], uint64_t img[6][32], const p_set_rt *r, int color_only, int cover) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? tex[sh->stage][b->reg] : 0;
            else continue;
            if (!h) continue;
            for (int k = 0; k < (color_only ? r->n : 9); k++) if ((color_only ? r->color[k] : (k < 8 ? r->color[k] : r->depth)) && defer_handle_sub(h, r, k, cover)) return 1;
        }
    }
    return 0;
}
static void defer_collect_reads(defer_t *d, const prog_t *pg, uint64_t tex[6][128], uint64_t img[6][32]) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) { d->nreads = 513; return; }
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? tex[sh->stage][b->reg] : 0;
            else continue;
            if (!h) continue; int seen = 0; for (int j = 0; j < d->nreads; j++) seen |= d->p_reads[j] == h;
            if (!seen && d->nreads < 512) d->p_reads[d->nreads++] = h;
        }
    }
}
static int defer_rt_hits_reads(const p_set_rt *r, int idx) {
    if (idx < 0 || idx >= r->n || !r->color[idx]) return 0;
    for (int i = 0; i < g_defer.nreads; i++) if (defer_handle_sub(g_defer.p_reads[i], r, idx, 1)) return 1;
    return 0;
}
static int defer_q_superset(const p_set_rt *q) {
    memset(g_defer.lmap, 0xff, sizeof g_defer.lmap);
    for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i]) { int found = 0;
        // 下標須相同：普通片元函數的 color(n) 輸出寫到物理第 n 個附件，lmap 只改寫掩碼 / 混合、搬不動輸出（能搬的只有帶輸出映射常量的 _bf 版本）。
        // 下標不同時推遲會把 P 的輸出寫進 Q 的別的附件。
        // The indices must be the same: a normal fragment function's color(n) output is written to physical attachment n, and lmap only changes the write mask / blend and cannot move the output (only the _bf version with output mapping constants can).
        // With different indices, deferral would write P's output into a different attachment of Q.
        if (i < q->n && q->color[i] && sub_eq(&g_defer.p_rt, i, q, i)) { g_defer.lmap[i] = (uint8_t)i; found = 1; }
        if (!found) return 0;
    }
    return 1;
}
static int defer_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_defer.n >= 64) { g_defer.reason = 20; return 0; }
    defer_snap *z = &g_defer.snap[g_defer.n++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di;
    z->sc = R.sc; z->has_sc = R.has_sc; z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S); for (int k = 0; k < 8; k++) z->pm[k] = (uint8_t)k; g_defer_draws++; return 1;
}
static void defer_release(void) { defer_release_one(&g_defer); }
static void defer_env(void);
static void defer_reset(void) { defer_clear_all(); g_defer_replaying = 0; defer_env(); }
static void defer_apply_p_clear(void) {
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int j = 0; j < g_defer.q_rt.n; j++) { int i = g_defer.lmap[j]; if (i >= 0 && g_defer.clear_c[i]) { R.clear_c[j] = 1; R.cc[j] = g_defer.cc[i]; } }
    if (g_defer.clear_d) { R.clear_d = 1; R.cd = g_defer.cd; } if (g_defer.clear_s) { R.clear_s = 1; R.cs = g_defer.cs; }
}
static int defer_begin_q(const uint8_t *op) {
    int found = 0, first = -1;
    for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && !g_defer_slots[i].at_q && g_defer_slots[i].q_op == op) { found = 1; if (first < 0) first = i; }
    if (!found) return 0;
    rp_flush();
    g_defer_cur = &g_defer_slots[first]; R.rt = g_defer.q_rt; R.has_rt = 1;
    for (int i = 0; i < 4 && !R.rt.depth; i++) { const defer_t *d = &g_defer_slots[i]; if (d->active && !d->at_q && d->q_op == op && d->q_rt.depth) { R.rt.depth = d->q_rt.depth; R.rt.dview = d->q_rt.dview; R.rt.has_dview = d->q_rt.has_dview; } }   // 任一組需要的深度模板都掛上（同 Q 各組深度已核相容） | Attach the depth-stencil needed by any group (the depths of groups with the same Q have been checked compatible)
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int i = 0; i < 4; i++) { defer_t *d = &g_defer_slots[i]; if (!d->active || d->at_q || d->q_op != op) continue;
        for (int j = 0; j < d->q_rt.n; j++) { int k = d->lmap[j]; if (k >= 0 && d->clear_c[k]) { R.clear_c[j] = 1; R.cc[j] = d->cc[k]; } }
        if (d->clear_d) { R.clear_d = 1; R.cd = d->cd; } if (d->clear_s) { R.clear_s = 1; R.cs = d->cs; }
        d->at_q = 1; g_defer_natq++;
    }
    return 1;
}
static void defer_replay(void) {
    if (g_defer_replaying) return;
    int any = 0; for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && g_defer_slots[i].at_q) any = 1;
    if (!any) return;
    typeof(S) save; memcpy(&save, &S, sizeof S); p_scissor sc = R.sc; int has_sc = R.has_sc; int vis = g_vis_counting;
    const uint8_t *en0 = g_exec_next; g_defer_replaying = 1; g_exec_next = NULL;
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born;   // 按創建先後：各組 P 區間不交疊且先後排列，組序即原繪製順序 | In creation order: the groups' P ranges do not overlap and are arranged in sequence, so the group order is the original draw order
        g_defer_cur = d; if (d->reset_between) g_vis_counting = 0;
        { static int once; if (!once++) MLOG("推遲合併通道：首例 通道 %llu，組 %d 回放繪製 %d 次", g_stat_pass, gi, d->n); }   // 多組時首個回放前可能已建多組，不能按 g_defer_n == 1 判 | With multiple groups, several groups may already have been created before the first replay, so it cannot be judged by g_defer_n == 1
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; static uint8_t lm[8]; for (int j = 0; j < 8; j++) lm[j] = d->lmap[j] == 0xff || d->lmap[j] >= 8 ? 0xff : z->pm[d->lmap[j]];
            memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; g_pso_lmap = lm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        defer_release_one(d);
    }
    g_vis_counting = vis; memcpy(&S, &save, sizeof S); args_dirty_all(); R.sc = sc; R.has_sc = has_sc; g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = g_dss_dirty = 1; g_exec_next = en0; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static void defer_fallback(void) {
    if (g_defer_replaying || !defer_has_active()) return;
    g_defer_fallbacks++; MLOG("【紅】推遲合併通道：提交結束仍有未回放組，按各組 P 原目標兜底");
    if (g_enc) { enc_r_end(g_enc); g_enc = nil; }
    typeof(S) save; memcpy(&save, &S, sizeof S); p_set_rt rt = R.rt; g_defer_replaying = 1;
    for (int gi, after = 0; (gi = defer_next(0, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; g_defer_cur = d; R.rt = d->p_rt; R.has_rt = 1; memcpy(R.clear_c, d->clear_c, 8); memcpy(R.cc, d->cc, sizeof R.cc);
        R.clear_d = d->clear_d; R.clear_s = d->clear_s; R.cd = d->cd; R.cs = d->cs;
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; g_pso_lmap = z->pm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        rp_end(); defer_release_one(d);
    }
    memcpy(&S, &save, sizeof S); args_dirty_all(); R.rt = rt; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static const uint8_t *defer_cont(int *ci) {   // 列表結束後的下一段（同一次提交內）：先調用方返回棧，再本次提交餘下的句柄；沒有了返回 NULL | The next segment after a list ends (within the same submit): first the caller return stack, then the remaining handles of this submit; returns NULL when there are none left
    for (;;) { int k = (*ci)++;
        if (k < g_ret_n) { if (g_ret_n > 16) return NULL; return g_ret[g_ret_n - 1 - k]; }
        k -= g_ret_n; if (k < g_nrest) return (const uint8_t *)g_rest[k];
        return NULL; }
}
// 推遲預篩（寫回前瞻索引二分，零掃描）：P 每個顏色子資源在本次繪製之後、首個「作附件 / 被清」事件必須是「另一次 SetRenderTargets 下作附件、
// 且那次目標覆蓋 P 全部顏色」；先被清 / 本次提交再沒作附件 → 不可能推遲，直接返回。讀事件不帶級別，交給完整前瞻按級別核。
// Deferral pre-screen (binary search on the store lookahead index, zero scanning): for every color subresource of P, the first "used as attachment / cleared" event after this draw must be "used as attachment under a different SetRenderTargets,
// whose targets cover all of P's colors"; cleared first / never used as attachment again in this submit -> deferral is impossible, return directly. Read events carry no level; handed to the full lookahead to check by level.
static int defer_pre(void) {
    if (!g_sdx_ok || !g_exec_cur || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 0;
    uint32_t P = g_exec_seq - 1;
    for (int k = 0; k < R.rt.n; k++) { if (!R.rt.color[k]) continue;
        uint64_t ser; int lv, sl; rt_sub(&R.rt, k, &ser, &lv, &sl);
        int b = sdx_bucket(ser, 0); if (b < 0) return 0;
        uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq <= P) lo = m + 1; else hi = m; }
        int ok = 0, same = 0; uint32_t walk = 0, prt = lo > g_sdx_bst[b] && g_sdx[lo - 1].seq == P ? g_sdx[lo - 1].rtseq : SDX_NONE;   // P 本通道的 SetRenderTargets 序號（取本次繪製的作附件事件） | SetRenderTargets sequence number of P's own render pass (taken from this draw's used-as-attachment event)
        for (uint32_t j = lo; j < end; j++) { const sdx_ev *e = &g_sdx[j];
            if (++walk > 64) return 0;   // 事件太多（被大量採樣的貼圖）：不當候選，保守且免長走 | Too many events (a heavily sampled texture): not taken as a candidate, conservative and avoids a long walk
            if (g_hoist_n && hoist_skipped(e->op)) continue;
            if (e->kind == SDX_READ || e->kind == SDX_NEED) continue;   // 讀事件不帶級別（讀別的級不衝突），交給完整前瞻按級別核 | Read events carry no level (reading another level is not a conflict); handed to the full lookahead to check by level
            if (e->plane != 0 || e->lv != lv || e->sl != sl) continue;
            if (e->kind == SDX_CLEAR) return 0;
            if (e->rtseq == SDX_NONE || e->rtseq >= g_sdx_nop) return 0;
            const uint8_t *rop = g_sdx_op[e->rtseq]; const p_set_rt *r = (const p_set_rt *)(rop + 8);
            if (rt_same(r, &R.rt)) { if (e->rtseq != prt) same = 1; continue; }   // 同目標：緊接的是 P 續段、隔了別的通道的是 Q；預篩不分，走完仍無別的候選時交給完整前瞻 | Same target: one immediately following is a continuation segment of P, one separated by other render passes is Q; the pre-screen does not distinguish them, and if there is still no other candidate at the end it is handed to the full lookahead
            for (int i = 0; i < R.rt.n; i++) { if (!R.rt.color[i]) continue; int f = 0;
                for (int q = 0; q < r->n && !f; q++) if (r->color[q] && sub_eq(&R.rt, i, r, q)) f = 1;
                if (!f) return 0; }
            ok = 1; break; }
        if (!ok && !same) return 0; }
    return 1;
}
static void defer_try(const p_draw *dw, const p_draw_idx *di) {
    if (!defer_on() || defer_has_in_p() || g_defer_replaying || g_fz.active || g_fz_checking || !g_exec_next || !R.rt.n) return;
    int slot = defer_slot_free(); if (slot < 0) return;
    g_defer_cur = &g_defer_slots[slot];
    int ncol = 0; for (int i = 0; i < R.rt.n; i++) ncol += R.rt.color[i] != NULL; if (!ncol) return;   // 只掛深度模板的通道（陰影圖等）：Q 的定義是覆蓋 P 的顏色，無顏色的 P 不可能推遲——原先預篩對它空轉放行、完整前瞻白掃幾十次繪製 | Render passes with only depth-stencil attached (shadow maps etc.): Q is defined as covering P's colors, so a P without color can never be deferred -- previously the pre-screen let it through idly and the full lookahead scanned dozens of draws for nothing
    int has_clear = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) has_clear |= R.clear_c[i]; if (!has_clear) return;
    if (!defer_pre()) return;
    memset(&g_defer, 0, sizeof g_defer); g_defer.p_rt = R.rt; memcpy(g_defer.clear_c, R.clear_c, 8); memcpy(g_defer.cc, R.cc, sizeof R.cc);
    g_defer.clear_d = R.clear_d; g_defer.clear_s = R.clear_s; g_defer.cd = R.cd; g_defer.cs = R.cs;
    uint64_t tex[6][128], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img); const prog_t *pg = S.prog; ds_st ds = S.ds;
    const uint8_t *p = g_exec_next, *p_end = NULL, *qop = NULL; const p_set_rt *q = NULL; p_set_rt scan_rt = R.rt; int srt_hit = -1, ci = 0, draws = 1, p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img);   // 當前這次繪製就是 P 的第一次 | The current draw is P's first
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }
        switch (h.op) {
            case OP_END: { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 1; goto fail; } p = nx; continue; }
            case OP_CALL: g_defer.reason = 2; goto fail;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_BIND_PROGRAM: pg = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_TEX: case OP_BIND_IMG: { const p_bind_handles *b = a; int lim = h.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } break; }
            case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); break; }
            case OP_DRAW: case OP_DRAW_IDX:
                if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { /* 建議在這裡識別需要輸出映射但缺少 _bf 後調用 bf_want_note。 | Suggestion: here, after identifying that an output mapping is needed but _bf is missing, call bf_want_note. */ g_defer.reason = 7; goto fail; }
                if (p_end) {   // 中間區間的繪製：不得寫 P 讀的貼圖、不得寫 P 的深度模板（P 寫模板時也不得測模板） | Draws in the intermediate range: must not write textures P reads, must not write P's depth-stencil (and must not test stencil when P writes stencil)
                    if (srt_hit < 0) { srt_hit = 0; for (int k = 0; k < scan_rt.n && !srt_hit; k++) if (scan_rt.color[k] && defer_rt_hits_reads(&scan_rt, k)) srt_hit = 1; }   // 附件只在 SetRenderTargets 變：每段算一次 | Attachments change only at SetRenderTargets: computed once per segment
                    if (srt_hit) { g_defer.reason = 12; goto fail; }
                    if (scan_rt.depth && g_defer.p_rt.depth && scan_rt.depth->serial == g_defer.p_rt.depth->serial && (ds.dw || (g_defer.p_st && ds.st))) { g_defer.reason = 13; goto fail; }
                    break; }
                draws++; if (draws > 64) { g_defer.reason = 20; goto fail; } p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img); g_defer.p_dw |= ds.dw; g_defer.p_st |= ds.st && ((S.st_wmask[0] | S.st_wmask[1]) != 0) && (ds.fail_f != NVN_STENCIL_OP_KEEP || ds.zfail_f != NVN_STENCIL_OP_KEEP || ds.zpass_f != NVN_STENCIL_OP_KEEP || ds.fail_b != NVN_STENCIL_OP_KEEP || ds.zfail_b != NVN_STENCIL_OP_KEEP || ds.zpass_b != NVN_STENCIL_OP_KEEP); if (g_defer.p_dw) { g_defer.reason = 8; goto fail; } break;
            case OP_RESET_COUNTER: g_defer.reset_between = 1; break;
            case OP_COPY_T2T: { const p_copy_t2t *c = a;   // 貼圖拷貝：兩端都不碰 P 的附件、目標不是 P 採樣的貼圖才放行 | Texture copy: allowed only if neither end touches P's attachments and the destination is not a texture P samples
                uint64_t ss = c->src ? c->src->serial : 0, ds_ = c->dst ? c->dst->serial : 0; int bad = !ss || !ds_;
                for (int k = 0; k < g_defer.p_rt.n && !bad; k++) if (g_defer.p_rt.color[k] && (g_defer.p_rt.color[k]->serial == ss || g_defer.p_rt.color[k]->serial == ds_)) bad = 1;
                if (g_defer.p_rt.depth && (g_defer.p_rt.depth->serial == ss || g_defer.p_rt.depth->serial == ds_)) bad = 1;
                for (int k = 0; k < g_defer.nreads && !bad; k++) if (h_ser(g_defer.p_reads[k]) == ds_) bad = 1;
                if (g_defer.nreads > 512) bad = 1;
                if (bad) { g_defer.reason = 3; g_defer.reason_op = h.op; goto fail; } break; }
            case OP_REPORT_COUNTER: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_RENDER_EN: g_defer.reason = 3; g_defer.reason_op = h.op; goto fail;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (!p_end) { g_defer.reason = 16; goto fail; }   // P 續段裡的清除：不推遲 | A clear inside P's continuation segment: do not defer
                if (c->index >= 0 && c->index < scan_rt.n && (defer_rt_hits_reads(&scan_rt, c->index) || (scan_rt.color[c->index] && defer_rt_hits_reads(&g_defer.p_rt, c->index)))) { g_defer.reason = 5; goto fail; } break; }
            case OP_CLEAR_DS: if (!p_end) { g_defer.reason = 16; goto fail; } break;
            case OP_SET_RT: { const p_set_rt *r = a;
                if (!p_end && rt_same(r, &g_defer.p_rt)) break;   // 同目標再設一次（遊戲常連發）：仍是 P 的續段，繪製一並推遲 | The same target set once more (the game often sends it repeatedly): still a continuation segment of P, and its draws are deferred together
                if (!p_end) p_end = p; int shared = 0; for (int i = 0; i < g_defer.p_rt.n; i++) for (int j = 0; j < r->n; j++) if (g_defer.p_rt.color[i] && r->color[j] && sub_eq(&g_defer.p_rt, i, r, j)) shared = 1;
                if (shared) {   // 空通道（設完目標不畫不清就又換目標）不碰任何附件：既不當 Q 也不算衝突 | An empty render pass (targets set, then changed again without drawing or clearing) touches no attachment: it is neither taken as Q nor counted as a conflict
                    const uint8_t *e = p + h.size; int ec = ci, empty = 0;
                    for (int g2 = 0; g2 < 200000; g2++) { op_hdr he; memcpy(&he, e, sizeof he);
                        if (he.op == OP_JUMP) { memcpy(&e, e + 8, 8); continue; }
                        if (he.op == OP_END) { const uint8_t *nx = defer_cont(&ec); if (!nx) break; e = nx; continue; }
                        if (he.op == OP_SET_RT) { empty = 1; break; }
                        if (he.op == OP_DRAW || he.op == OP_DRAW_IDX || he.op == OP_CLEAR_COLOR || he.op == OP_CLEAR_DS || he.op == OP_CALL || he.op == OP_DISPATCH || he.op == OP_CLEAR_TEX || he.op == OP_COPY_T2T || he.op == OP_COPY_B2T || he.op == OP_COPY_T2B) break;
                        e += he.size; }
                    if (empty) break; }
                if (!shared) break; if (!p_has_draw || !defer_q_superset(r)) { g_defer.reason = 6; goto fail; } q = r; qop = p; goto q_found; }
            default: break;
        }
        if (h.op == OP_SET_RT) { scan_rt = *(const p_set_rt *)a; srt_hit = -1; }
        p += h.size;
    }
fail:
    if (g_defer.reason && passdump()) PD("G %llu 不成立 reason=%d op=%d", g_stat_pass + 1, g_defer.reason, g_defer.reason_op); memset(&g_defer, 0, sizeof g_defer); return;
q_found:
    p = qop; op_hdr qh; memcpy(&qh, p, sizeof qh); p += qh.size; int q_body = 0, q_clears = 0;   // 綁定跟蹤接著主掃描的狀態（不回到 P 開頭的 S） | Binding tracking continues from the main scan's state (does not go back to S at the start of P)
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8; if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        // Q 整段（到下一個 SetRenderTargets / 列表結束）：任何繪製都不得按覆蓋 P 級別的視圖採樣 P 的顏色附件——推遲後 P 的結果還在 tile 裡沒落主存，採樣會讀到舊內容
        // The whole Q segment (up to the next SetRenderTargets / end of list): no draw may sample P's color attachments through a view covering P's level -- after deferral P's result is still in the tile and has not landed in main memory, so sampling would read old content
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) { if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { g_defer.reason = 14; goto fail; } q_body = 1; p += h.size; continue; }
        if (q_body && (h.op == OP_SET_RT || h.op == OP_END)) break;
        if (!q_body && h.op == OP_END) { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 9; goto fail; } p = nx; continue; }
        if (h.op == OP_CALL) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        // Q 的 SetRenderTargets 之後、首次繪製與任何清除之前又來一次 SetRenderTargets（遊戲重復設目標，前一次是空通道）：新目標仍覆蓋 P 就以它為 Q，否則不推遲
        // After Q's SetRenderTargets and before its first draw and any clear, another SetRenderTargets arrives (the game sets targets repeatedly, the previous one being an empty render pass): if the new targets still cover P take it as Q, otherwise do not defer
        if (!q_body && h.op == OP_SET_RT && !q_clears) { const p_set_rt *r2 = a; if (!defer_q_superset(r2)) { g_defer.reason = 15; goto fail; } q = r2; qop = p; p += h.size; continue; }
        if (h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS) q_clears = 1;
        if (!q_body && (h.op == OP_END || h.op == OP_SET_RT || h.op == OP_UPDATE_UB || h.op == OP_COPY_B2B || h.op == OP_CLEAR_BUF || h.op == OP_DISPATCH)) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        if (h.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)a)->prog; else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG) { const p_bind_handles *b = a; int lim = h.op == OP_BIND_IMG ? 32 : 128; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
        else if (h.op == OP_CLEAR_COLOR) { const p_clear_color *c = a; if (c->index >= 0 && c->index < q->n && q->color[c->index]) for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i] && sub_eq(q, c->index, &g_defer.p_rt, i)) { g_defer.reason = 10; goto fail; } }
        p += h.size;
    }
    if (!q || !defer_q_superset(q) || g_defer.n >= 64) { g_defer.reason = 11; goto fail; }
    for (int i = 0; i < 4; i++) if (&g_defer_slots[i] != &g_defer && g_defer_slots[i].active && g_defer_slots[i].q_op == qop && !defer_same_q_depth(&g_defer_slots[i], q)) { g_defer.reason = 17; goto fail; }
    g_defer.q_rt = *q; if (!q->depth && g_defer.p_rt.depth) { g_defer.q_rt.depth = g_defer.p_rt.depth; g_defer.q_rt.dview = g_defer.p_rt.dview; g_defer.q_rt.has_dview = g_defer.p_rt.has_dview; } { int na = 0; for (int i = 0; i < 4; i++) { const defer_t *d = &g_defer_slots[i]; if (d != &g_defer && d->active && d->at_q) na += d->n; }
      if (na + 1 > 64) { g_defer.reason = 20; goto fail; } }
    g_defer.q_op = qop; g_defer.p_end_op = p_end; g_defer.in_p = 1; g_defer.active = 1; g_defer_nact++; g_defer_pend = p_end; g_defer_n++; g_defer.born = g_defer_n;
    // 鏈式：本通道正是上游組的 Q（它們 at_q、等本通道開通道時回放），本通道又整體推遲——上游組按創建先後並進新組：
    // 上游組的目標空間 = 本通道目標 = 新組 P 目標，快照的映射復合為 pm' = 上游 lmap ∘ pm；上游的清除已在 R 裡，下面隨新組帶走
    // Chained: this render pass is exactly the Q of upstream groups (they are at_q, waiting to be replayed when this render pass opens), and this render pass is itself deferred as a whole -- the upstream groups are merged into the new group in creation order:
    // the target space of an upstream group = this render pass's targets = the new group's P targets, and the snapshot's mapping is composed as pm' = upstream lmap o pm; the upstream clears are already in R and are carried along with the new group below
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; if (d == &g_defer) continue;
        for (int k = 0; k < 8; k++) if (d->lmap[k] != 0xff && d->lmap[k] != k) { g_defer.reason = 18; goto fail; }   // 只並恆等映射的上游組（見 defer_q_superset） | Only merge in upstream groups with an identity mapping (see defer_q_superset)
        for (int i = 0; i < d->n; i++) { defer_snap z = d->snap[i]; uint8_t pm[8]; for (int k = 0; k < 8; k++) pm[k] = d->lmap[k] == 0xff || d->lmap[k] >= 8 ? 0xff : z.pm[d->lmap[k]];
            memcpy(z.pm, pm, 8); g_defer.snap[g_defer.n++] = z; }
        g_defer.reset_between |= d->reset_between; d->n = 0; defer_release_one(d); } if (!defer_take(dw, di)) { defer_release_one(&g_defer); return; }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0; PD("G %llu 成立 組 %d P→Q draws=%d", g_stat_pass + 1, slot, draws);
}
// 合併的必要條件預篩（按提交索引二分，失敗零掃描）：合併要求「後一個通道（下一個目標不同、未被提前的 SET_RT）的首次繪製
// 採樣前一個通道的某張顏色附件」，即那次繪製的序號上有該附件的 SDX_READ 事件。不滿足就必然不合併，省掉下面逐條掃描；
// 滿足才走原規劃（決定與不預篩時完全相同）。索引不可用時照舊規劃。
// Pre-screen of the necessary condition for merging (binary search on the submit index, zero scanning on failure): merging requires that "the first draw of the later pass (the next SET_RT with a different target that has not been hoisted)
// samples some color attachment of the earlier pass", i.e. there is an SDX_READ event for that attachment at that draw's sequence number. If not satisfied there is certainly no merge, saving the per-operation scan below;
// only if satisfied does it go through the original planning (the decision is exactly the same as without the pre-screen). When the index is unavailable, plan as before.
static int fz_prefilter(void) {
    if (!g_sdx_ok || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 1;
    uint32_t P = g_exec_seq - 1, lo = 0, hi = g_rtx_n;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_rtx_seq[m] <= P) lo = m + 1; else hi = m; }
    for (; lo < g_rtx_n; lo++) { const uint8_t *op = g_sdx_op[g_rtx_seq[lo]]; if (g_hoist_n && hoist_skipped(op)) continue; if (!rt_same(&R.rt, (const p_set_rt *)(op + 8))) break; }
    if (lo >= g_rtx_n) return 0;
    uint32_t r2 = g_rtx_seq[lo], a = 0, b = g_drx_n;
    while (a < b) { uint32_t m = (a + b) / 2; if (g_drx_seq[m] <= r2) a = m + 1; else b = m; }
    if (a >= g_drx_n) return 0;
    uint32_t fd = g_drx_seq[a];
    for (uint32_t k = lo + 1; k < g_rtx_n && g_rtx_seq[k] < fd; k++) if (!(g_hoist_n && hoist_skipped(g_sdx_op[g_rtx_seq[k]]))) return 0;   // 首繪前又換目標：原規劃第二步也放棄 | The target changes again before the first draw: step two of the original planning gives up as well
    for (int j = 0; j < R.rt.n && j < 8; j++) { if (!R.rt.color[j]) continue; int bk = sdx_bucket(R.rt.color[j]->serial, 0); if (bk < 0) continue;
        uint32_t e0 = g_sdx_bst[bk], e1 = g_sdx_bst[bk + 1];
        while (e0 < e1) { uint32_t m = (e0 + e1) / 2; if (g_sdx[m].seq < fd) e0 = m + 1; else e1 = m; }
        for (; e0 < g_sdx_bst[bk + 1] && g_sdx[e0].seq == fd; e0++) if (g_sdx[e0].kind == SDX_READ) return 1; }
    return 0;
}
static void fuse_try(void) {
    if (g_fz.active || !g_exec_next || g_hoist_n + 12 > HOIST_MAX || !R.rt.n) return;
    if (!fz_prefilter()) return;
    // 一：前一個通道到下一次 SetRenderTargets，只許繪製與狀態
    // One: from the earlier pass to the next SetRenderTargets, only draws and state are allowed
    const uint8_t *p = NULL, *r2op = NULL, *reuse[8]; int nreuse = 0; const p_set_rt *r2 = NULL; p_scissor sc = R.sc; int has_sc = R.has_sc;
    fz_cursor cur = {.p = g_exec_next}; op_hdr h; const void *a;
    for (int guard = 0; guard < 200000 && !r2; guard++) {
        if (!fz_next(&cur, &p, &h, &a)) return;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH:
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_RENDER_EN: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_SET_RT: {
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (rt_same(&R.rt, a)) {
                    if (g_enc_mode == 1 || pend || !R.has_rt || !tbdr_on() || nreuse >= 8) return;
                    reuse[nreuse++] = p; continue;
                }
                r2 = a; r2op = p; continue;
            }
            default: break;
        }
    }
    if (!r2 || rt_same(&R.rt, r2)) return;
    // 二：後一個通道開頭到首次繪製，只許清除（不清共享附件）與狀態
    // Two: from the start of the later pass to its first draw, only clears (not clearing shared attachments) and state are allowed
    const uint8_t *cl[8]; int ncl = 0; uint8_t clr[8] = {0}; MTLClearColor cc[8]; int cd_f = 0, cs_f = 0; double cdv = 0; uint32_t csv = 0;
    if (!fz_next(&cur, &p, &h, &a)) return;   // 接著第一步的游標（已越過後一個通道的 SET_RT，保留跨句柄 / 返回棧位置） | Continue from the cursor of step one (already past the later pass's SET_RT, keeping the cross-handle / return stack position)
    for (int guard = 0; guard < 200000; guard++) {
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) break;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_SET_RT: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (ncl >= 8 || c->index < 0 || c->index >= r2->n || !r2->color[c->index] || c->mask != 0xf) return;
                const tex_t *t = r2->color[c->index];
                if (has_sc && !(sc.x <= 0 && sc.y <= 0 && sc.x + sc.w >= (int)t->d.width && sc.y + sc.h >= (int)t->d.height)) return;
                clr[c->index] = 1; cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]); cl[ncl++] = p; break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; if (ncl >= 8 || !r2->depth) return;
                if (c->depth_mask) { cd_f = 1; cdv = c->depth; } if (c->stencil_mask) { cs_f = 1; csv = (uint32_t)c->stencil; } cl[ncl++] = p; break; }
            default: break;
        }
        if (!fz_next(&cur, &p, &h, &a)) return;
    }
    // 三：附件並集（前一個通道的附件保持原序號，後一個通道獨有的接在後面）、尺寸一致、深度相容
    // Three: union of attachments (the earlier pass's attachments keep their original indices, those unique to the later pass are appended), consistent sizes, compatible depth
    p_set_rt U = R.rt; int n = R.rt.n; int8_t p2map[8]; memset(p2map, -1, sizeof p2map);
    for (int k = 0; k < r2->n && k < 8; k++) { if (!r2->color[k]) continue;
        int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && sub_eq(&R.rt, m, r2, k)) { j = m; break; }
        if (j < 0) { if (n >= 8) return; j = n++; U.color[j] = r2->color[k]; U.cview[j] = r2->cview[k]; U.has_cview[j] = r2->has_cview[k]; }
        else if (clr[k]) return;   // 後一個通道開頭要清前一個通道正在畫的附件：合併後無處安放這個清除 | The start of the later pass wants to clear an attachment the earlier pass is drawing to: after merging this clear has nowhere to go
        p2map[k] = (int8_t)j; }
    U.n = n;
    { uint64_t s0; int l0, x0; rt_sub(&U, 0, &s0, &l0, &x0); if (!U.color[0]) return;
      uint32_t w0 = MAX(U.color[0]->d.width >> l0, 1u), h0 = MAX(U.color[0]->d.height >> l0, 1u);
      for (int j = 1; j < n; j++) { uint64_t s_; int l_, x_; rt_sub(&U, j, &s_, &l_, &x_); if (!U.color[j] || MAX(U.color[j]->d.width >> l_, 1u) != w0 || MAX(U.color[j]->d.height >> l_, 1u) != h0) return; } }
    int depth_from_r2 = 0;
    if (R.rt.depth && r2->depth && !sub_eq(&R.rt, 8, r2, 8)) return;
    if (!R.rt.depth && r2->depth) { U.depth = r2->depth; U.dview = r2->dview; U.has_dview = r2->has_dview; depth_from_r2 = 1; }
    if ((cd_f || cs_f) && !depth_from_r2) return;   // 深度在前一個通道裡就在用：後一個通道開頭的深度清除無處安放 | The depth is already in use in the earlier pass: the depth clear at the start of the later pass has nowhere to go
    // 四：前一個通道的繪製不得採樣後一個通道帶來的附件；深度來自後一個通道時不得開深度模板測試
    // Four: draws of the earlier pass must not sample attachments brought in by the later pass; when the depth comes from the later pass, depth-stencil testing must not be enabled
    static uint64_t tex[6][128], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img);
    const prog_t *pg = S.prog; ds_st ds = S.ds;
    cur = (fz_cursor){.p = g_exec_next};
    for (;;) {   // 第一次循環核開通道的這次繪製（當前狀態），之後按區間裡的繪製逐個核 | The first loop iteration checks the draw that opens the render pass (current state), then checks the draws in the range one by one
        if (depth_from_r2 && (ds.dt || ds.dw || ds.st)) return;
        for (int j = R.rt.n; j < n; j++) if (prog_samples_ser(pg, tex, img, U.color[j]->serial)) return;
        if (depth_from_r2 && prog_samples_ser(pg, tex, img, U.depth->serial)) return;
        for (;;) {
            const uint8_t *q; op_hdr hq; const void *aq;
            if (!fz_next(&cur, &q, &hq, &aq)) return;
            if (q == r2op) goto planned;
            if (hq.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_STATE) { const p_bind_state *b = aq; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); }
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_IMG) { const p_bind_handles *b = aq; int lim = hq.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
            else if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) break;
        }
    }
planned:
    {   // 五：預篩後一個通道的第一次繪製（靜態條件），免得白掛並集附件：跟蹤到那次繪製時的程序與貼圖綁定 | Five: pre-screen the first draw of the later pass (static conditions), to avoid attaching the union of attachments for nothing: track the program and texture bindings up to that draw
        const prog_t *pg2 = S.prog; static uint64_t t2[6][128], i2[6][32]; memcpy(t2, S.tex, sizeof t2); memcpy(i2, S.img, sizeof i2);
        cur = (fz_cursor){.p = g_exec_next};
        for (;;) { const uint8_t *q; op_hdr hq; const void *aq; if (!fz_next(&cur, &q, &hq, &aq)) return; if (q == p) break;
            if (hq.op == OP_BIND_PROGRAM) pg2 = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_IMG) { const p_bind_handles *b = aq; int lim = hq.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) i2[b->stage][b->first + k] = b->h[k]; else t2[b->stage][b->first + k] = b->h[k]; } }
        }
        NvProg *pr2 = pg2 ? prog_get(pg2) : nil; NvShader *vs2 = pr2 ? pr2->sh[0] : nil, *fs2 = pr2 ? pr2->sh[1] : nil;
        if (!vs2 || !fs2) return;
        int touches = 0;
        for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr2->sh[st]; if (!sh || sh->stage >= 6) continue;
            for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *bb = &sh->rf.b[i]; uint64_t h;
                if (bb->kind == NVN_RB_IMG) h = bb->reg < 32 ? i2[sh->stage][bb->reg] : 0; else if (bb->kind == NVN_RB_TEX || bb->kind == NVN_RB_TBUF) h = bb->reg < 128 ? t2[sh->stage][bb->reg] : 0; else continue;
                uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
                int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
                if (j < 0) continue;
                if (sh != fs2 || bb->kind != NVN_RB_TEX || (fs2->rf.fc_mixed[bb->reg >> 6] >> (bb->reg & 63) & 1)) return;
                int any = 0;
                for (int c = 0; c < fs2->rf.nfc; c++) if (fs2->rf.fc_reg[c] == bb->reg) { int l = fs2->rf.fc_locn[c];
                    if (l >= 32 || (vs2->rf.uv_attr[l] < 0 && vs2->rf.uv_aff_attr[l] < 0)) return; any = 1; }
                if (!any) return;
                touches = 1; } }
        if (!touches) return;   // 後一個通道不讀前一個通道的附件：合併沒有收益 | The later pass does not read the earlier pass's attachments: merging has no benefit
        int ident = 1; for (int c = 0; c < 8; c++) if (fs2->rf.color_mask >> c & 1) { if (p2map[c] < 0) return; if (p2map[c] != c) ident = 0; }
        if (!ident && !fs2->rf.nfo) return;
    }
    memset(&g_fz, 0, sizeof g_fz);
    g_fz.active = 1; g_fz.phase = 1; g_fz.n1 = R.rt.n; g_fz.nreuse = nreuse; g_fz.rt1 = R.rt; g_fz.rt2 = *r2; g_fz.rt2_op = r2op; memcpy(g_fz.reuse, reuse, sizeof reuse); memcpy(g_fz.p2map, p2map, 8);
    for (int j = 0; j < 8; j++) g_fz.lmap1[j] = j < R.rt.n ? (uint8_t)j : 0xff;
    fz_probe_cov2(r2, p, p2map, clr, g_fz.n1, n);
    g_fz.skip[g_fz.nskip++] = r2op; for (int i = 0; i < ncl; i++) g_fz.skip[g_fz.nskip++] = cl[i];
    for (int i = 0; i < g_fz.nskip; i++) g_hoist_skip[g_hoist_n++] = g_fz.skip[i];
    R.rt = U;
    memcpy(g_fz.clr2, clr, 8); memcpy(g_fz.cc2, cc, sizeof cc); g_fz.cd2f = (uint8_t)cd_f; g_fz.cs2f = (uint8_t)cs_f; g_fz.cd2 = cdv; g_fz.cs2 = csv;
    for (int k = 0; k < 8; k++) if (clr[k]) { R.clear_c[p2map[k]] = 1; R.cc[p2map[k]] = cc[k]; }
    if (cd_f) { R.clear_d = 1; R.cd = cdv; } if (cs_f) { R.clear_s = 1; R.cs = csv; }
    if (!g_fz_n++) MLOG("可讀 tile 通道合併：首例 通道 %llu（物理附件 %d 個）", g_stat_pass + 1, n);
    PD("F %llu 合併下一個通道（可讀 tile）：物理附件 %d 個，前一個通道 %d 個", g_stat_pass + 1, n, R.rt.n == n ? g_fz.n1 : g_fz.n1);
}
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap) {   // 可讀 tile 版本按常量特化（緩存） | The readable tile version is specialized by constants (cached)
    os_unfair_lock_lock(&g_fz_fn_lock);
    if (!g_bf_fn_cache) { g_bf_fn_cache = [NSMutableDictionary dictionary]; g_bf_lib_cache = [NSMutableDictionary dictionary]; }
    size_t kl = strlen(fs->key); NSString *base = [NSString stringWithFormat:@"%.*s", (int)(kl > 2 ? kl - 2 : kl), fs->key];
    id<MTLLibrary> lib = g_bf_lib_cache[base];
    if (!lib) {
        NSError *e = nil; NSString *path = [NSString stringWithFormat:@"%@/%@_bf.metallib", g_msl_dir, base];
        lib = lib_from_file(path, &e);
        if (!lib) MLOG("【紅】可讀 tile 版本載入失敗 %s：%s", path.UTF8String, e.localizedDescription.UTF8String);
        else g_bf_lib_cache[base] = lib;
    }
    if (!lib) { os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    NSMutableString *k = [NSMutableString stringWithFormat:@"%02x", fbmask];
    for (int i = 0; i < fs->rf.nfc; i++) [k appendFormat:@",%d", fx[i]];
    for (int j = 0; j < 8; j++) [k appendFormat:@"/%02x", lmap[j]];
    NSString *cacheKey = [NSString stringWithFormat:@"%@|%@", base, k];
    id<MTLFunction> f = g_bf_fn_cache[cacheKey];
    if (f) { os_unfair_lock_unlock(&g_fz_fn_lock); return f; }
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    for (int i = 0; i < fs->rf.nfc; i++) { int v = fx[i]; [cv setConstantValue:&v type:MTLDataTypeInt atIndex:(NSUInteger)i]; }
    for (int q = 0; q < 8; q++) { bool b = fbmask >> q & 1; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(32 + q)]; }
    for (int j = 0; j < fs->rf.nfo; j++) for (int q = 0; q < 8; q++) { bool b = lmap[q] == fs->rf.fo_color[j]; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(64 + 8 * j + q)]; }
    NSError *e = nil; f = [lib newFunctionWithName:@"main0" constantValues:cv error:&e];
    if (!f) { MLOG("【紅】可讀 tile 版本特化失敗 %s（%s）：%s", fs->key, k.UTF8String, e.localizedDescription.UTF8String); os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    g_bf_fn_cache[cacheKey] = f; os_unfair_lock_unlock(&g_fz_fn_lock); return f;
}
static int ubo_float(int slot, uint16_t ref, float *out) {   // uniform 槽的第 vec 個向量第 cmp 分量（CPU 側讀；落在本提交待執行的緩衝寫裡就不讀） | Component cmp of vector vec of the uniform slot (read on the CPU side; not read if it falls within a buffer write pending execution in this submit)
    int sl = ref >> 8, vec = ref >> 2 & 63, cmp = ref & 3;
    if (slot < 0 || slot >= 6 || sl < 0 || sl >= 16) return 0;
    const p_bind_buf *u = &S.ubo[slot][sl]; uint64_t off = (uint64_t)vec * 16 + (uint64_t)cmp * 4;
    if (!u->addr || off + 4 > u->size || pend_hits(u->addr + off, 4)) return 0;
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, 4, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    memcpy(out, base + mo, 4); return 1;
}
static int zskip_fact_for(const char *key, zskip_fact *out) {
    if (!key) return 0;
    for (int i = 0; i < g_zskip_nf; i++) if (!strncmp(key, g_zskip_facts[i].key, strlen(g_zskip_facts[i].key))) { *out = g_zskip_facts[i]; return 1; }   // 運行時鍵帶 _b 後綴 | Runtime keys carry the _b suffix
    return 0;
}
static int zskip_view_zero(int tid) {
    if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    treg_t *r = &g_treg[tid]; const tex_t *t = &r->t; int lv0 = r->has_v && r->v.set_levels ? r->v.base_level : 0;
    int nl = r->has_v && r->v.set_levels && r->v.nlevels > 0 ? r->v.nlevels : 1;
    int sl0 = r->has_v && r->v.set_layers ? r->v.min_layer : 0;
    int ns = r->has_v && r->v.set_layers && r->v.nlayers > 0 ? r->v.nlayers : 1;
    for (int l = 0; l < nl; l++) for (int s = 0; s < ns; s++) if (!zskip_has(t->serial, lv0 + l, sl0 + s)) return 0;
    return 1;
}
// 遮擋查詢在計數時，被跳過的繪製本該貢獻通過採樣數：只有「下一次清零之前沒有報告」才無關緊要。
// 從本次繪製之後順著回放順序看：先遇到採樣數清零 → 無關（返回 1）；先遇到採樣數報告、或看不到頭（提交結束）→ 有關（返回 0）
// While an occlusion query is counting, a skipped draw should have contributed passed samples: it only does not matter if "there is no report before the next reset".
// Looking forward in replay order from after this draw: sample count reset encountered first -> does not matter (return 1); sample count report encountered first, or the end cannot be seen (submit ends) -> matters (return 0)
static int zskip_vis_dead(void) {
    if (!g_vis_counting) return 1;
    const uint8_t *p = g_exec_next; int ci = 0; if (!p) return 0;
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_END) { p = defer_cont(&ci); if (!p) return 0; continue; }
        if (h.op == OP_CALL) return 0;
        if (h.op == OP_RESET_COUNTER || h.op == OP_REPORT_COUNTER) { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) return h.op == OP_RESET_COUNTER; }
        p += h.size; }
    return 0;
}
static int zskip_can_draw(NvProg *pr) {
    // 跳過條件（全部滿足）：片元鍵在事實表、它反射聲明的每個貼圖槽綁的視圖只覆蓋已知全零的子資源、系數有限、
    // 不寫深度不做模板、沒在數遮擋查詢、要寫的每個顏色附件本來就是 0（本通道整張清 0 的候選，或已知全零）——寫 0 蓋 0。
    // 不成立原因（PASSDUMP 的 N 行）：1 深度 / 模板 2 遮擋查詢計數中且清零前有報告 3 系數讀不到或非有限 4 圖像槽 5 被採樣視圖不是已知全零 6 無目標 7 目標不是 0
    // Skip conditions (all must hold): the fragment key is in the fact table, the view bound to every texture slot declared by its reflection covers only subresources known to be all zero, the coefficient is finite,
    // no depth write and no stencil, not counting an occlusion query, and every color attachment to be written is already 0 (a candidate cleared entirely to 0 in this render pass, or known all zero) -- writing 0 over 0.
    // Reasons for not holding (the N line of PASSDUMP): 1 depth / stencil 2 occlusion query counting with a report before the reset 3 coefficient unreadable or non-finite 4 image slot 5 sampled view not known all zero 6 no target 7 target is not 0
    if (!zskip_on() || !pr) return 0;
    NvShader *fs = nil; for (int st = 0; st < 5; st++) if (pr->sh[st] && pr->sh[st]->stage == 1) fs = pr->sh[st];
    zskip_fact f; float coeff; int why = 0;
    if (!fs || !zskip_fact_for(fs->key, &f)) return 0;
    if (S.ds.dw || S.ds.st) why = 1;
    else if (!zskip_vis_dead()) why = 2;
    else if (f.ubo_slot >= 0 && (!ubo_float(1, (uint16_t)((f.ubo_slot << 8) | (f.vec << 2) | f.comp), &coeff) || !isfinite(coeff))) why = 3;   // ubo_slot < 0：該著色器沒有系數 | ubo_slot < 0: this shader has no coefficient
    for (int i = 0; !why && i < fs->rf.nb; i++) { nvn_rbind *b = &fs->rf.b[i];
        if (b->kind == NVN_RB_IMG) { why = 4; break; }
        if (b->kind != NVN_RB_TEX && b->kind != NVN_RB_TBUF) continue;
        int tid = b->reg < 128 ? (int)(S.tex[1][b->reg] & 0xfffff) : 0; if (!zskip_view_zero(tid)) why = 5; }
    if (!why && (!R.has_rt || !R.rt.n)) why = 6;
    for (int i = 0; !why && i < R.rt.n && i < 8; i++) { if (!R.rt.color[i]) continue;
        int pending = g_enc ? g_zskip_cand_ok[i] : zskip_zero_clear(i);   // 通道已開：看開通道時記下的候選（還沒被畫過）；未開：看掛起的清除 | Render pass already open: look at the candidates noted when the render pass was opened (not yet drawn to); not open: look at the pending clears
        if (!pending && (g_enc || R.clear_c[i] || !zskip_target_zero(R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]))) why = 7; }
    if (why) PD("N %llu 零源不跳過 reason=%d 集合 %d", g_stat_pass + 1, why, g_zskip_n);
    return !why;
}
static int uv_proof(NvShader *vs, int locn, const p_draw *dw, const p_draw_idx *di) {   // 插值輸出 locn 在每個像素中心恰為本像素的紋素中心：u = (x + 1) / 2、v = (1 − y) / 2，w = 1，視口與剪裁覆蓋整塊 | The interpolated output locn at every pixel center is exactly this pixel's texel center: u = (x + 1) / 2, v = (1 - y) / 2, w = 1, viewport and scissor cover the whole block
    if (locn < 0 || locn >= 32 || vs->rf.pos_attr < 0) return 0;
    int direct = vs->rf.uv_attr[locn] >= 0, aff = vs->rf.uv_aff_attr[locn] >= 0;
    if (!direct && !aff) return 0;
    float sc[4] = {1, 0, 1, 0};   // 原樣直傳 = 縮放 1、偏移 0 | Pass-through as-is = scale 1, offset 0
    if (!direct) for (int q = 0; q < 4; q++) if (!ubo_float(0, vs->rf.uv_aff[locn][q], &sc[q])) return 0;
    NSUInteger W = g_pass.tw, H = g_pass.th;
    if (S.has_vp && !(S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)W && S.vp[0][3] == (float)H)) return 0;
    if (R.has_sc && !(R.sc.x <= 0 && R.sc.y <= 0 && R.sc.x + R.sc.w >= (int)W && R.sc.y + R.sc.h >= (int)H)) return 0;
    float P[64][4], Q[64][4]; int n = dw ? dw->count : di->count;
    if (attr_read(vs->rf.pos_attr, dw, di, P) || attr_read(direct ? vs->rf.uv_attr[locn] : vs->rf.uv_aff_attr[locn], dw, di, Q)) return 0;
    for (int k = 0; k < n; k++) { float w = vs->rf.pos_w1 ? 1.0f : P[k][3];
        float u = direct ? Q[k][0] : fmaf(Q[k][0], sc[0], sc[1]), v = direct ? Q[k][1] : fmaf(Q[k][1], sc[2], sc[3]);   // 與著色器同一個 fma | The same fma as the shader
        if (w != 1.0f || u != (P[k][0] + 1.0f) * 0.5f || v != (1.0f - P[k][1]) * 0.5f) { PD("F %llu uv 證明：頂點 %d 位置 %g,%g uv %g,%g（縮放 / 偏移 %g,%g,%g,%g）", g_stat_pass, k, P[k][0], P[k][1], u, v, sc[0], sc[1], sc[2], sc[3]); return 0; } }
    return 1;
}
// 後一個通道的一次繪製：核對並給出特化。返回 0 = 證不出（調用方結束合併通道、按原目標重開）
// One draw of the later pass: verify and produce the specialization. Returns 0 = cannot be proven (the caller ends the merged render pass and reopens with the original targets)
static int fz_p2_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di, uint8_t lmap[8], __strong id<MTLFunction> *fn) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1]; *fn = nil;
    if (!vs || !fs) { PD("F %llu 證不出：無頂點 / 片元著色器", g_stat_pass); return 0; }
    if (!g_fz.rt2.depth && R.rt.depth && (S.ds.dt || S.ds.dw || S.ds.st)) { PD("F %llu 證不出：深度來自前一個通道而本次繪製開了深度模板測試", g_stat_pass); return 0; }
    int fx[32]; for (int i = 0; i < 32; i++) fx[i] = -1; uint8_t fbmask = 0;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? S.tex[sh->stage][b->reg] : 0; else continue;
            uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
            int j = -1; for (int m = 0; m < g_fz.n1; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
            if (j < 0) continue;   // 採樣的不是前一個通道的附件 | What is sampled is not an attachment of the earlier pass
            if (sh != fs || b->kind != NVN_RB_TEX) { PD("F %llu 證不出：非片元階段或非貼圖槽讀前一個通道的附件", g_stat_pass); return 0; }
            if (fs->rf.fc_mixed[b->reg >> 6] >> (b->reg & 63) & 1) { PD("F %llu 證不出：槽 %d 混用", g_stat_pass, b->reg); return 0; }
            uint64_t s_; int lv, sl; rt_sub(&R.rt, j, &s_, &lv, &sl);
            if (lv || sl || MAX(R.rt.color[j]->d.width, 1u) != g_pass.tw || MAX(R.rt.color[j]->d.height, 1u) != g_pass.th) { PD("F %llu 證不出：附件不是第 0 級第 0 層或尺寸不符", g_stat_pass); return 0; }
            {   // 採樣的那個視圖也得正好是附件那一張（原先只核附件是第 0 級第 0 層，沒核著色器綁的視圖——同一張貼圖按第 k 級 / 第 k 層 / 換格式的視圖採樣時， | The sampled view must also be exactly the attachment itself (previously only the attachment being level 0 layer 0 was checked, not the view bound by the shader -- when the same texture is sampled through a view of level k / layer k / a changed format,
                // 證明照樣成立，採樣會被改成讀片上第 0 級 / 原格式，畫面就錯了）。視圖起始級 / 層須為 0、格式須與附件實際格式相同，否則不合併
                // the proof would still hold, the sampling would be changed to read on-tile level 0 / the original format, and the picture would be wrong). The view's starting level / layer must be 0 and its format must equal the attachment's actual format, otherwise do not merge
                int tid_ = (int)(h & 0xfffff); void *tv_ = NULL; if (tid_) tex_fast_rid(tid_, &tv_); __unsafe_unretained id<MTLTexture> tv = (__bridge id<MTLTexture>)tv_;
                uint32_t apf_ = tv.parentTexture ? (uint32_t)tv.parentTexture.pixelFormat : (uint32_t)tv.pixelFormat; uint8_t swz_[4];
                if (R.rt.has_cview[j] && R.rt.cview[j].set_format && !(nvn_fmt_to_mtl(R.rt.cview[j].format, &apf_, swz_) && apf_)) apf_ = 0;   // 附件按換格式視圖掛：取那個格式；認不出就不合 | The attachment is attached through a format-changing view: take that format; if unrecognized, do not merge
                MTLPixelFormat apf = (MTLPixelFormat)apf_;
                if (!tv || (tv.parentTexture && (tv.parentRelativeLevel || tv.parentRelativeSlice)) || tv.pixelFormat != apf) {
                    static int lg; if (lg++ < 8) LOG("[nvn-mtl] 片上讀合併：槽 %d 採樣視圖（起始級 %lu 層 %lu 格式 %lu）與附件（第 0 級第 0 層 格式 %lu）不符，不合併（幀 %llu 通道 %llu）", b->reg,
                        tv && tv.parentTexture ? (unsigned long)tv.parentRelativeLevel : 0ul, tv && tv.parentTexture ? (unsigned long)tv.parentRelativeSlice : 0ul, (unsigned long)tv.pixelFormat, (unsigned long)apf,
                        (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
                    PD("F %llu 證不出：槽 %d 採樣視圖不是附件那一張（級 / 層 / 格式不符）", g_stat_pass, b->reg); return 0; } }
            int any = 0;
            for (int c = 0; c < fs->rf.nfc; c++) if (fs->rf.fc_reg[c] == b->reg) {
                if (!uv_proof(vs, fs->rf.fc_locn[c], dw, di)) { PD("F %llu 證不出：槽 %d 的採樣坐標不是本像素（插值輸出 %d）", g_stat_pass, b->reg, fs->rf.fc_locn[c]); return 0; }
                fx[c] = j; fbmask |= (uint8_t)(1u << j); any = 1; }
            if (!any) { PD("F %llu 證不出：槽 %d 的採樣不在可改表裡", g_stat_pass, b->reg); return 0; } } }
    memset(lmap, 0xff, 8);
    for (int c = 0; c < 8; c++) if (fs->rf.color_mask >> c & 1) { if (g_fz.p2map[c] < 0) { PD("F %llu 證不出：輸出顏色 %d 無對應附件", g_stat_pass, c); return 0; } lmap[g_fz.p2map[c]] = (uint8_t)c; }
    int ident = !fbmask; for (int c = 0; c < 8; c++) if ((fs->rf.color_mask >> c & 1) && g_fz.p2map[c] != c) ident = 0;
    if (ident) { for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1; if (fz_check_on()) fz_snap_take(dw, di); return 1; }   // 不讀 tile、輸出不移位：用原版本（寫掩碼仍按映射） | Does not read the tile and the output is not shifted: use the original version (the write mask still follows the mapping)
    if (!fs->rf.nfo) { bf_want_note(fs->key); PD("F %llu 證不出：沒有可讀 tile 版本（輸出須移位）", g_stat_pass); return 0; }
    *fn = bf_fn(fs, fx, fbmask, lmap);
    if (!*fn) return 0;
    memset(&g_fz_desc, 0, sizeof g_fz_desc); g_fz_desc.kind = 1; g_fz_desc.fbmask = fbmask; for (int i = 0; i < 32; i++) g_fz_desc.fx[i] = (int8_t)fx[i];
    for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1;
    if (fz_check_on()) fz_snap_take(dw, di);
    g_fz_draws++; return 1;
}
static void bloomq_reset(void) { memset(&g_bloomq, 0, sizeof g_bloomq); }
static int bloomq_enabled(void) {
    if (g_bloom_q < 0) { const char *e = getenv("DM_NVN_BLOOM_Q"); g_bloom_q = !e || strcmp(e, "0"); }
    return g_bloom_q;
}
static int bloomq_target(uint64_t *ser, int *lv, int *sl) {
    if (!R.has_rt || R.rt.n != 1 || !R.rt.color[0]) return 0;
    rt_sub(&R.rt, 0, ser, lv, sl); return *ser != 0;
}
static int bloomq_make_pipeline(MTLPixelFormat fmt) {
    if (g_bloomq_pso && g_bloomq_fmt == fmt) return 1;
    static const char *src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 uv=float2((id<<1)&2,id&2); V o; o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1); o.uv=uv; return o; }\n"
        "struct P { float threshold; float coeff_g; float coeff_b; float dm; float sh; };\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant P& p [[buffer(0)]]) { float2 d=float2(1.0/p.dm,1.0/p.sh); float3 c=0; for (int y=-1;y<=1;y+=2) for (int x=-1;x<=1;x+=2) { float3 v=t.sample(s,in.uv+float2(x,y)*d).rgb; float l=v.r*0.2126+v.g*p.coeff_g+v.b*p.coeff_b; if(l>=p.threshold) c+=v/(1.0+l); } return float4(c*0.25,1); }\n";
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!lib) { MLOG("【紅】泛光 1/4 著色器編譯失敗：%s", e.localizedDescription.UTF8String); return 0; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = fmt;
    g_bloomq_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!g_bloomq_pso) { MLOG("【紅】泛光 1/4 管線失敗：%s", e.localizedDescription.UTF8String); return 0; }
    g_bloomq_fmt = fmt;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES;
    g_bloomq_samp = [g_dev newSamplerStateWithDescriptor:sd]; return g_bloomq_samp != nil;
}
static int bloomq_capture(void) {
    uint64_t ser; int lv, sl; if (!bloomq_target(&ser, &lv, &sl) || !g_bloomq.s_tex) return 0;
    id<MTLTexture> base = tex_mtl(R.rt.color[0]), src = tex_mtl(g_bloomq.s_tex); if (!base || !src) return 0;
    id<MTLTexture> dst = base; if (lv || sl) { static id<MTLTexture> vb; static int vl, vs; if (vb != base || vl != lv || vs != sl) { g_bloomq_view = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:base.textureType levels:NSMakeRange(lv, 1) slices:NSMakeRange(sl, 1)]; if (g_bloomq_view) res_add(g_bloomq_view); vb = base; vl = lv; vs = sl; } dst = g_bloomq_view; }
    if (!dst || !bloomq_make_pipeline(dst.pixelFormat)) return 0;
    rp_end();
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new]; rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> en = [g_cb renderCommandEncoderWithDescriptor:rp]; if (!en) return 0;
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [en setArgumentTable:g_bloomq_args atStages:MTLRenderStageFragment];
    [g_bloomq_args setTexture:src.gpuResourceID atIndex:0]; [g_bloomq_args setSamplerState:g_bloomq_samp.gpuResourceID atIndex:0];
    float *p = g_bloomq_const[g_bloomq_i].contents; p[0] = g_bloomq.threshold; p[1] = g_bloomq.coeff_g; p[2] = g_bloomq.coeff_b; p[3] = src.width; p[4] = src.height; [g_bloomq_args setAddress:g_bloomq_const[g_bloomq_i].gpuAddress atIndex:0];
    [en setViewport:(MTLViewport){0,0,dst.width,dst.height,0,1}]; [en setRenderPipelineState:g_bloomq_pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; m4_end(en);
    g_bloomq_i = (g_bloomq_i + 1) % 3; args_dirty_all(); g_pso_dirty = g_dss_dirty = 1; return 1;
}
// ---------- 拖貨跟手圖：商人貨架上的貨沒有「拿起」態，拖動時由回放層把「畫這件貨圖標的那次繪製」在同一趟末尾平移到手指處再畫一遍 ----------
// 界面那一趟每個矩形是一次 4 頂點繪製（頂點 20 字節：位置 3 浮點 + 貼圖坐標 2 浮點，位置經頂點常量 0 號的矩陣到裁剪坐標）。
// 前端（front_ios.m）給出手指按下的那一點、貨架格網與跟手圖中心（歸一化，左上原點）；這裡在界面趟裡找「蓋住這一點、四邊都壓在格線上、面積最小」的矩形 = 那件貨
// （只按點認會認錯：按下約 0.1 秒後遊戲在按下點畫一個以它為中心、一格大小的矩形，不壓格線——真機上兩次把它當成了貨）
// （不依賴遊戲的選中態：選中要等手柄光標落到貨上約 0.1 秒，按下就拖的快拖等不到）。同一矩形上有格底和圖標兩次繪製：
// 格底用公共貼圖（本趟很多矩形都用它），圖標每件一張——取「本趟用到次數最少」的那張貼圖的那次。認到的矩形回報給前端（貨架原位蓋暗色用）。
// 界面矩形在真實畫面裡的縱向位置與高度（都按佔畫面高的比例：y = 0 貼頂，h = 1 即整個畫面）；前端登記，每次繪製現讀（切主機 / 掌機檔會變）。
// 遊戲把界面畫在界面貼圖的上部 h 那一截（視口是整張貼圖，投影矩陣裡帶著縮放）。
// ---------- Drag-follow image for shop items: items on a vendor's shelf have no "picked up" state, so during a drag the replay layer takes "the draw that draws this item's icon", translates it to the finger at the end of the same pass and draws it once more ----------
// In the UI pass each rectangle is one 4-vertex draw (20-byte vertices: position 3 floats + texture coordinates 2 floats; the position goes to clip coordinates through the matrix in vertex constant 0).
// The front end (front_ios.m) gives the point where the finger pressed, the shelf grid and the center of the drag-follow image (normalized, top-left origin); here, in the UI pass, find the rectangle that "covers this point, has all four edges on grid lines, and has the smallest area" = that item
// (identifying by the point alone gets it wrong: about 0.1 seconds after the press the game draws a one-cell-sized rectangle centered on the press point, not on grid lines -- on a real device it was twice taken for the item)
// (does not depend on the game's selected state: selection has to wait about 0.1 seconds for the controller cursor to land on the item, and a quick drag that starts on press cannot wait for that). The same rectangle has two draws, cell background and icon:
// the cell background uses a common texture (many rectangles in this pass use it), while each item has its own icon texture -- take the draw with the texture "used the fewest times in this pass". The recognized rectangle is reported back to the front end (used to cover the original shelf position with a dark color).
// Vertical position and height of the UI rectangle in the real picture (both as a fraction of the picture height: y = 0 touches the top, h = 1 is the whole picture); registered by the front end and read fresh on every draw (changes when switching between docked / handheld mode).
// The game draws the UI into the upper h portion of the UI texture (the viewport is the whole texture, and the projection matrix carries the scaling).
static void (*g_ui_rect_fn)(double *y, double *h);
void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h)) { g_ui_rect_fn = fn; }
static double ui_hfrac(void) { double y = 0, h = 1; if (g_ui_rect_fn) g_ui_rect_fn(&y, &h); return h > 0 && h < 1 ? h : 1; }
static _Atomic int g_ghost_on; static _Atomic uint64_t g_ghost_pt, g_ghost_at, g_ghost_found, g_ghost_grid;
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) {
    if (on) { uint64_t gv = 0; for (int i = 0; i < 4; i++) gv |= (uint64_t)(uint16_t)(fminf(fmaxf(grid[i], 0), 1) * 65535 + .5f) << (16 * i); atomic_store(&g_ghost_grid, gv);
              atomic_store(&g_ghost_pt, (uint64_t)(uint32_t)lroundf(fminf(fmaxf(px, 0), 1) * 65535) | (uint64_t)(uint32_t)lroundf(fminf(fmaxf(py, 0), 1) * 65535) << 32);
              atomic_store(&g_ghost_at, (uint64_t)(uint32_t)(int32_t)lroundf(cx * 65535) | (uint64_t)(uint32_t)(int32_t)lroundf(cy * 65535) << 32); }
    else atomic_store(&g_ghost_found, 0);
    atomic_store(&g_ghost_on, on);
}
int mtl_ui_ghost_rect(float r[4]) { uint64_t v = atomic_load(&g_ghost_found); if (!v) return 0; for (int i = 0; i < 4; i++) r[i] = (float)((v >> (16 * i)) & 0xffff) / 65535.f; return 1; }
static struct { uint64_t pass; int n, nt, busy; float area, r[4]; struct { void *S_; p_draw dw; uint64_t tex; } c[4]; uint64_t tid[128]; uint16_t tn[128]; } g_gh;
static uint64_t ghost_tex(void) {   // 這次繪製的片元著色器實際採樣的那張貼圖（按著色器反射取槽位；S.tex 裡別的槽是之前繪製留下的） | The texture actually sampled by this draw's fragment shader (slot taken from shader reflection; the other slots in S.tex are left over from earlier draws)
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; NvShader *f = pr ? pr->sh[1] : nil; if (!f) return 0;
    for (int i = 0; i < f->rf.nb; i++) if ((f->rf.b[i].kind == NVN_RB_TEX || f->rf.b[i].kind == NVN_RB_SAMP) && f->rf.b[i].reg < 128 && S.tex[1][f->rf.b[i].reg]) return S.tex[1][f->rf.b[i].reg];
    return 0; }
// 當前這次界面繪製是不是「不帶貼圖、正好蓋滿整個界面矩形」（界面矩形佔貼圖上部 hf）
// Whether the current UI draw is "untextured and exactly covering the whole UI rectangle" (the UI rectangle occupies the upper hf of the texture)
static int ui_cover_all(double hf) {
    if (S.vbuf[0].size != 80 || ghost_tex()) return 0;
    const p_bind_buf *u = &S.ubo[0][0]; void *vb, *mb; uint64_t vo, mo; const uint8_t *vp_, *mp_;
    if (!u->addr || u->size < 64 || pend_hits(u->addr, 64) || pend_hits(S.vbuf[0].address, 80)) return 0;
    if (!mtl_addr(S.vbuf[0].address, 80, &vb, &vo) || !(vp_ = [(__bridge id<MTLBuffer>)vb contents]) || !mtl_addr(u->addr, 64, &mb, &mo) || !(mp_ = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    float m[16], x0 = 9, y0 = 9, x1 = -9, y1 = -9; memcpy(m, mp_ + mo, 64);
    for (int i = 0; i < 4; i++) { float f[2]; memcpy(f, vp_ + vo + 20 * i, 8); float nx = (m[0] * f[0] + m[4] * f[1] + m[12] + 1) / 2, ny = (m[1] * f[0] + m[5] * f[1] + m[13] + 1) / 2;
        x0 = fminf(x0, nx); x1 = fmaxf(x1, nx); y0 = fminf(y0, ny); y1 = fmaxf(y1, ny); }
    return x0 < 0.002f && x1 > 0.998f && y0 < 0.002f && fabsf(y1 - (float)hf) < 0.002f;
}
static void ghost_note(const p_draw *dw) {
    if (g_gh.busy || dw->count != 4 || S.nattr != 2 || S.stream[0].stride != 20 || S.vbuf[0].size != 80) return;
    if (g_gh.pass != g_stat_pass) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = g_gh.nt = 0; g_gh.pass = g_stat_pass; }
    uint64_t t = ghost_tex(); int k = 0; for (; k < g_gh.nt; k++) if (g_gh.tid[k] == t) break;
    if (k == g_gh.nt && g_gh.nt < 128) { g_gh.tid[k] = t; g_gh.tn[k] = 0; g_gh.nt++; } if (k < g_gh.nt) g_gh.tn[k]++;
    const p_bind_buf *u = &S.ubo[0][0]; void *vb, *mb; uint64_t vo, mo; const uint8_t *vp_, *mp_;
    if (!u->addr || u->size < 64 || pend_hits(u->addr, 64) || pend_hits(S.vbuf[0].address, 80)) return;
    if (!mtl_addr(S.vbuf[0].address, 80, &vb, &vo) || !(vp_ = [(__bridge id<MTLBuffer>)vb contents]) || !mtl_addr(u->addr, 64, &mb, &mo) || !(mp_ = [(__bridge id<MTLBuffer>)mb contents])) return;
    float m[16], x0 = 9, y0 = 9, x1 = -9, y1 = -9; memcpy(m, mp_ + mo, 64);
    for (int i = 0; i < 4; i++) { float f[2]; memcpy(f, vp_ + vo + 20 * i, 8); float nx = (m[0] * f[0] + m[4] * f[1] + m[12] + 1) / 2, ny = (m[1] * f[0] + m[5] * f[1] + m[13] + 1) / 2;
        x0 = fminf(x0, nx); x1 = fmaxf(x1, nx); y0 = fminf(y0, ny); y1 = fmaxf(y1, ny); }
    { float hf = (float)ui_hfrac(); y0 /= hf; y1 /= hf; }   // 前端給的點是界面矩形裡的歸一化坐標；界面只佔貼圖上部 hf 那一截時換到同一口徑（hf = 1 時不變） | The points given by the front end are normalized coordinates within the UI rectangle; when the UI occupies only the upper hf portion of the texture, convert to the same convention (unchanged when hf = 1)
    uint64_t pv = atomic_load(&g_ghost_pt); float qx = (float)(uint32_t)pv / 65535.f, qy = (float)(pv >> 32) / 65535.f;
    if (qx < x0 || qx > x1 || qy < y0 || qy > y1) return;
    uint64_t gv = atomic_load(&g_ghost_grid); float gx = (float)(gv & 0xffff) / 65535.f, gy = (float)((gv >> 16) & 0xffff) / 65535.f, gw = (float)((gv >> 32) & 0xffff) / 65535.f, gh = (float)(gv >> 48) / 65535.f;
    if (gw <= 0 || gh <= 0) return;
    float w = x1 - x0, h = y1 - y0, area = w * h, c0 = (x0 - gx) / gw, r0 = (y0 - gy) / gh, cn = w / gw, rn = h / gh;   // 左上角在第幾格、佔幾格 | Which cell the top-left corner is in, and how many cells it occupies
    if (fabsf(c0 - roundf(c0)) > 0.15f || fabsf(r0 - roundf(r0)) > 0.15f || fabsf(cn - roundf(cn)) > 0.15f || fabsf(rn - roundf(rn)) > 0.15f || cn < 0.85f || rn < 0.85f || cn > 2.15f || rn > 4.15f) return;   // 一件貨最大 2×4 格 | One item is at most 2x4 cells
    if (g_gh.n && area > g_gh.area * 1.03f) return;                       // 已有更小的 | A smaller one already exists
    if (g_gh.n && area < g_gh.area * 0.97f) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0; }   // 找到更小的：前面的作廢 | Found a smaller one: the earlier ones are invalidated
    if (g_gh.n == 4) { free(g_gh.c[0].S_); memmove(&g_gh.c[0], &g_gh.c[1], 3 * sizeof g_gh.c[0]); g_gh.n = 3; }
    g_gh.area = area; g_gh.r[0] = x0; g_gh.r[1] = y0; g_gh.r[2] = x1; g_gh.r[3] = y1;
    typeof(g_gh.c[0]) *c = &g_gh.c[g_gh.n++]; c->S_ = malloc(sizeof S); memcpy(c->S_, &S, sizeof S); c->dw = *dw; c->tex = t;
}
static void ghost_flush(void) {   // 渲染通道結束前：把認到的圖標繪製平移到手指處再畫一遍（挪視口，不改頂點；剪裁放開） | Before the render pass ends: translate the recognized icon draw to the finger and draw it once more (moves the viewport, does not change vertices; the scissor is opened up)
    if (g_gh.busy || !g_gh.n) return;
    if (g_gh.pass != g_stat_pass || !atomic_load(&g_ghost_on)) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0; return; }
    int best = -1, bn = 1 << 30; for (int i = 0; i < g_gh.n; i++) { int cnt = 0; for (int k = 0; k < g_gh.nt; k++) if (g_gh.tid[k] == g_gh.c[i].tex) cnt = g_gh.tn[k]; if (cnt < bn) { bn = cnt; best = i; } }
    typeof(g_gh.c[0]) *c = &g_gh.c[best]; uint64_t at = atomic_load(&g_ghost_at); float ax = (float)(int32_t)(uint32_t)at / 65535.f, ay = (float)(int32_t)(uint32_t)(at >> 32) / 65535.f;
    float W = (float)g_pass.tw, H = (float)g_pass.th, ccx = (g_gh.r[0] + g_gh.r[2]) / 2, ccy = (g_gh.r[1] + g_gh.r[3]) / 2;
    { uint64_t v = 0; for (int i = 0; i < 4; i++) v |= (uint64_t)(uint16_t)(fminf(fmaxf(g_gh.r[i], 0), 1) * 65535 + .5f) << (16 * i); atomic_store(&g_ghost_found, v ? v : 1); }
    g_gh.busy = 1; typeof(S) save; memcpy(&save, &S, sizeof S); p_scissor sc = R.sc; int has_sc = R.has_sc;
    memcpy(&S, c->S_, sizeof S); if (!S.has_vp) { S.vp[0][0] = S.vp[0][1] = 0; S.vp[0][2] = W; S.vp[0][3] = H; S.has_vp = 1; }
    S.vp[0][0] += (ax - ccx) * W; S.vp[0][1] += (ay - ccy) * H * (float)ui_hfrac(); R.has_sc = 0;
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1; draw(&c->dw, NULL);
    { static int once; if (!once++) MLOG("拖貨跟手圖：首例 通道 %llu，候選 %d 個，取貼圖本趟用到 %d 次的那次", g_stat_pass, g_gh.n, bn); }
    memcpy(&S, &save, sizeof S); args_dirty_all(); R.sc = sc; R.has_sc = has_sc; g_pso_dirty = g_dss_dirty = 1; g_gh.busy = 0;
    for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0;
}
// 這次繪製取樣了哪些顏色附件（位 k = 附件 k）：比貼圖序號，且取樣的級範圍要含附件那一級（寫第 k 級、讀第 k−1 級的降採樣鏈不算）
// Which colour attachments this draw samples (bit k = attachment k): compare texture serials, and the sampled level range must include the attachment's level (downsample chains writing level k and reading level k-1 do not count)
static unsigned fbx_hazard(NvProg *pr) {
    unsigned mask = 0; if (!pr) return 0;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i];
            if (b->kind != NVN_RB_TEX || b->reg >= 128 || sh->stage >= 6) continue;
            int tid = (int)(S.tex[sh->stage][b->reg] & 0xfffff); if (!tid) continue;
            pthread_mutex_lock(&g_rmu);
            uint64_t ser = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t.serial : 0; view_t v = tid < g_ntreg && g_treg[tid].has_v ? g_treg[tid].v : (view_t){0};
            pthread_mutex_unlock(&g_rmu);
            for (int k = 0; ser && k < R.rt.n && k < 8; k++) {
                if (!R.rt.color[k] || R.rt.color[k]->serial != ser) continue;
                unsigned alv = R.rt.has_cview[k] && R.rt.cview[k].set_levels ? R.rt.cview[k].base_level : 0;
                if (v.set_levels && (alv < v.base_level || alv >= (unsigned)v.base_level + (v.nlevels ? v.nlevels : 1))) continue;
                mask |= 1u << k;
            } } }
    return mask;
}
static int fbx_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_FBX"); on = !(e && !strcmp(e, "0")); if (!on) MLOG("DM_NVN_FBX=0：附件回授不拷副本（對照用）"); } return on; }   // DM_NVN_FBX=0 關（A/B 對照） | DM_NVN_FBX=0 turns it off (A/B comparison)
static void fbx_prepare(NvProg *pr) {
    if (!fbx_on()) return;
    unsigned mask = fbx_hazard(pr);
    for (int k = 0; mask && k < R.rt.n && k < 8 && g_fbx_n < FBX_MAX; k++) {
        if (!(mask >> k & 1)) continue;
        id<MTLTexture> a = tex_mtl(R.rt.color[k]);
        if (!a || a.textureType != MTLTextureType2D || a.sampleCount != 1 || bc_block_bytes(a.pixelFormat)) continue;
        int hit = -1, lru = 0;
        for (int i = 0; i < FBX_MAX; i++) {
            int busy = 0; for (int j = 0; j < g_fbx_n; j++) busy |= g_fbx_copy[j] == g_fbx_pool[i];
            id<MTLTexture> c = g_fbx_pool[i];
            if (c && !busy && c.pixelFormat == a.pixelFormat && c.width == a.width && c.height == a.height && c.mipmapLevelCount == a.mipmapLevelCount) { hit = i; break; }
            if (!busy && g_fbx_use[i] < g_fbx_use[lru]) lru = i;
        }
        if (hit < 0) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:a.pixelFormat width:a.width height:a.height mipmapped:a.mipmapLevelCount > 1];
            d.mipmapLevelCount = a.mipmapLevelCount; d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
            id<MTLTexture> c = [g_dev newTextureWithDescriptor:d]; if (!c) continue;
            if (g_fbx_pool[lru]) defer_obj_release(g_fbx_pool[lru]);
            res_add(c); g_fbx_pool[lru] = c; hit = lru;
            static int logged; if (logged++ < 4) MLOG("附件回授：%lux%lu 格式 %lu 的附件在同一通道被取樣，開通道前拷一份給取樣用", (unsigned long)a.width, (unsigned long)a.height, (unsigned long)a.pixelFormat);
        }
        g_fbx_use[hit] = ++g_fbx_tick;
        id e = blit();
        for (NSUInteger lv = 0; lv < a.mipmapLevelCount; lv++)
            enc_b_copy_texture(e, a, 0, lv, MTLOriginMake(0, 0, 0), MTLSizeMake(MAX(a.width >> lv, 1u), MAX(a.height >> lv, 1u), 1), g_fbx_pool[hit], 0, lv, MTLOriginMake(0, 0, 0));
        g_fbx_copy[g_fbx_n] = g_fbx_pool[hit]; g_fbx_ser[g_fbx_n] = R.rt.color[k]->serial; g_fbx_n++;
        g_fbx_count++;
    }
}
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (!S.prog) { RED_ONCE(18, "繪製前沒有綁定程序"); return; }
    __unsafe_unretained NvProg *pr = prog_get(S.prog);
    if (g_ruler_truth == 0) {   // 尺子負對照（DM_NVN_RULER_NEG=<片元鍵>）：被測模式下丟掉該著色器的繪製，尺子必須報不等價 | Ruler negative control (DM_NVN_RULER_NEG=<fragment key>): in the mode under test drop this shader's draws, and the ruler must report non-equivalence
        static const char *neg; static int ng = -1; if (ng < 0) { neg = getenv("DM_NVN_RULER_NEG"); ng = neg && *neg; }
        if (ng && pr) for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (sh && sh->stage == 1 && !strncmp(sh->key, neg, strlen(neg))) { static int once; if (!once++) MLOG("DM_NVN_RULER_NEG：被測模式丟棄著色器 %s 的繪製", sh->key); return; } }
    }
    if (zskip_can_draw(pr)) {   // 零源跳過：輸出恆為 0、寫 0 蓋 0，這次繪製不改變任何結果（見 zskip_can_draw） | Zero-source skip: the output is always 0, writing 0 over 0, so this draw changes no result (see zskip_can_draw)
        g_zskip_count++; { static int once; if (!once++) MLOG("零源跳過：首例 通道 %llu", g_stat_pass + 1); }
        PD("N %llu 零源跳過", g_stat_pass + 1); return; }
    if (!g_defer_replaying) {
        if (g_defer.active && g_defer.in_p && !g_defer.at_q) { if (defer_take(dw, di)) return; }
    }
    // 什麼都畫不出的繪製（兩面全剔、剪裁為空）在開通道之前就返回：否則通道已開，TBDR 上整張附件白讀白寫
    // （Mac 選角頁：3 個通道只因此類繪製而開，全部讀入、零繪製）
    // 量上界用的診斷開關 DM_NVN_SKIP_FS=<片元鍵>[,<片元鍵>…]（缺省無）：丟掉這些片元著色器的全部繪製（畫面會錯，只用來在真機 ABA 裡量「這類繪製總共值多少 GPU 時間」）
    // Draws that cannot draw anything (both faces culled, empty scissor) return before the render pass is opened: otherwise the render pass is already open and on TBDR the whole attachment is loaded and stored for nothing
    // (Mac character select page: 3 render passes are opened only because of such draws, all loaded, zero draws)
    // Diagnostic switch for measuring an upper bound DM_NVN_SKIP_FS=<fragment key>[,<fragment key>...] (none by default): drops all draws of these fragment shaders (the picture will be wrong; only used in real-device ABA to measure "how much GPU time this class of draws is worth in total")
    { static char sk[16][20]; static int nsk = -1;
      if (nsk < 0) { nsk = 0; for (const char *q = getenv("DM_NVN_SKIP_FS"); q && *q && nsk < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 16) n = 16; memcpy(sk[nsk], q, n); sk[nsk++][n] = 0; }
                     if (nsk) MLOG("DM_NVN_SKIP_FS：丟棄 %d 個片元著色器的繪製（診斷，畫面不對）", nsk); }
      if (g_ub_n > 0 && pr->sh[1] && !g_defer_replaying && ub_skip(pr->sh[1]->key)) return;   // 同進程交替測量：本相丟這一類 | In-process alternating measurement: this phase drops this class
      if (nsk && pr->sh[1] && !g_defer_replaying) for (int i = 0; i < nsk; i++) if (!strncmp(pr->sh[1]->key, sk[i], strlen(sk[i]))) return; }
    // 同類診斷 DM_NVN_SKIP_SMALL=<寬>：丟掉目標寬度（按所掛的級）≤ 該值的全部繪製——量「極小通道整批丟掉」在並行流水下值多少（每通道固定開銷）
    // Similar diagnostic DM_NVN_SKIP_SMALL=<width>: drop all draws whose target width (by the attached level) is <= this value -- measures how much "dropping tiny render passes in bulk" is worth under the parallel pipeline (fixed per-pass overhead)
    { static int small = -1; if (small < 0) { const char *e = getenv("DM_NVN_SKIP_SMALL"); small = e ? atoi(e) : 0; if (small) MLOG("DM_NVN_SKIP_SMALL：丟棄目標寬度 ≤ %d 的繪製（診斷，畫面不對）", small); }
      if (small > 0 && R.has_rt && !g_defer_replaying) { int k = R.rt.n && R.rt.color[0] ? 0 : R.rt.depth ? 8 : -1;
        if (k >= 0) { uint64_t s_; int lv, sl; rt_sub(&R.rt, k, &s_, &lv, &sl); const tex_t *t = k == 8 ? R.rt.depth : R.rt.color[0]; if ((int)MAX(t->d.width >> lv, 1u) <= small) return; } } }
    // 試驗開關 DM_NVN_NOFXAA=1（缺省關，去留按視覺尺子 + 實機看圖定）：跳過 FXAA 那次全屏繪製，後面的銳化 / 放大改讀 FXAA 的輸入（色調映射結果）
    // Experimental switch DM_NVN_NOFXAA=1 (off by default; keep or drop decided by the visual ruler + looking at the picture on a real machine): skips the FXAA full-screen draw, and the later sharpening / upscale reads FXAA's input instead (the tone mapping result)
    { static int nofxaa = -1; static uint64_t fx_from, fx_to; if (nofxaa < 0) { const char *e = getenv("DM_NVN_NOFXAA"); nofxaa = e && !strcmp(e, "1"); if (nofxaa) MLOG("DM_NVN_NOFXAA=1：跳過 FXAA"); }
      if (nofxaa && tbdr_on() && !g_defer_replaying && pr->sh[1]) { NvShader *f = pr->sh[1];
        if (!strncmp(f->key, "ebcfcd1fa0a77dbc", 16) && !g_enc && R.rt.n == 1 && R.rt.color[0] && !R.clear_c[0]) {
            uint64_t h = 0; for (int i = 0; i < f->rf.nb && !h; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128) h = S.tex[1][f->rf.b[i].reg];
            if (h) { fx_from = R.rt.color[0]->serial; fx_to = h; PD("N %llu 跳過 FXAA：ser=%llu 的讀者改讀 ser=%llu", g_stat_pass, (unsigned long long)fx_from, (unsigned long long)h_ser(h)); return; } }
        else if (fx_from && !strncmp(f->key, "52aac1f2306b8fc5", 16)) { for (int r = 0; r < 128; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == fx_from) S.tex[1][r] = fx_to; fx_from = 0; } } }
    { static uint64_t bloomq_frame; if (bloomq_frame != atomic_load(&g_cur_frame)) { bloomq_frame = atomic_load(&g_cur_frame); bloomq_reset(); }
      if (bloomq_enabled() && tbdr_on() && !g_defer_replaying && pr->sh[1]) {
        NvShader *f = pr->sh[1]; uint64_t tgt = 0; int lv, sl;
        if (!strncmp(f->key, "2938a5b71b8e21b2", 16)) {
            if (g_bloomq.state) bloomq_reset();
            if (bloomq_target(&tgt, &lv, &sl) && R.rt.color[0]) {
                uint64_t h = 0; const tex_t *st = NULL; int cbuf = 0;
                for (int i = 0; i < f->rf.nb; i++) { nvn_rbind *b = &f->rf.b[i];
                    if (b->kind == NVN_RB_TEX && b->reg < 128 && !h) { h = S.tex[1][b->reg]; int tid = (int)(h & 0xfffff); if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) st = &g_treg[tid].t; }
                    if (b->kind == NVN_RB_SAMPT && b->reg < 128) g_bloomq.samp_h = S.tex[1][b->reg] >> 20;
                    if (b->kind == NVN_RB_SAMP && b->reg < 128) g_bloomq.samp_h = S.samp[1][b->reg] >> 20;
                    if (b->kind == NVN_RB_CBUF && !cbuf) cbuf = ubo_float(1, (uint16_t)(b->reg << 8), &g_bloomq.threshold);   // c7[0].x
                }
                if (h && st && cbuf && f->rf.c1_len >= sizeof(float) * 2) {
                    float c1[2]; memcpy(c1, f->rf.c1, sizeof c1); g_bloomq.s_tex = st; g_bloomq.s_h = h; g_bloomq.s_ser = st->serial; g_bloomq.b0_ser = tgt; g_bloomq.coeff_g = c1[0]; g_bloomq.coeff_b = c1[1]; g_bloomq.state = 1;
                    { static int once; if (!once++) MLOG("泛光 1/4 起點：生效（S ser=%llu B0 ser=%llu）", (unsigned long long)g_bloomq.s_ser, (unsigned long long)g_bloomq.b0_ser); } PD("A %llu 泛光 1/4 起點 S=%llu B0=%llu", g_stat_pass, (unsigned long long)g_bloomq.s_ser, (unsigned long long)g_bloomq.b0_ser); R.clear_c[0] = 0;   // B0 整級不要了：掛起的清除也丟掉，否則會為它單開一個只清除的通道 | The whole B0 level is no longer needed: drop the pending clear too, otherwise a clear-only render pass would be opened just for it
                    return;
                }
            }
        } else if (g_bloomq.state == 1 && !strncmp(f->key, "019ce2f82edbda84", 16)) {
            uint64_t src = 0; for (int i = 0; i < f->rf.nb; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128) { src = h_ser(S.tex[1][f->rf.b[i].reg]); break; }
            if (src == g_bloomq.b0_ser && bloomq_target(&tgt, &lv, &sl) && bloomq_capture()) { R.clear_c[0] = 0; g_bloomq.b1_ser = tgt; g_bloomq.state = 2; PD("B %llu 泛光 1/4 原生截取 B0=%llu B1=%llu", g_stat_pass, (unsigned long long)g_bloomq.b0_ser, (unsigned long long)g_bloomq.b1_ser); return; }
            RED_ONCE(62, "泛光 1/4 起點：截取已跳過但原生截取沒畫成，本幀缺泛光"); bloomq_reset();
        } else if (g_bloomq.state == 2 && !strncmp(f->key, "019ce2f82edbda84", 16)) {
            for (int i = 0; i < f->rf.nb; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128 && h_ser(S.tex[1][f->rf.b[i].reg]) == g_bloomq.b1_ser) { g_bloomq.b1_h = S.tex[1][f->rf.b[i].reg]; break; }
        } else if (g_bloomq.state == 2 && !strncmp(f->key, "cc264583f2000c81", 16)) {
            if (bloomq_target(&tgt, &lv, &sl) && tgt == g_bloomq.b0_ser) { g_bloomq.state = 3; PD("C %llu 泛光 1/4 跳過 B0 上採樣", g_stat_pass); return; }
        } else if (g_bloomq.state == 3 && !strncmp(f->key, "2e2d6700a2569795", 16)) {
            if (g_bloomq.b1_h) { for (int r = 0; r < 128; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == g_bloomq.b0_ser) S.tex[1][r] = g_bloomq.b1_h; PD("D %llu 泛光 1/4 疊加改讀 B1", g_stat_pass); g_bloomq_last_b0 = g_bloomq.b0_ser; g_bloomq_last_b1h = g_bloomq.b1_h; g_bloomq_last_b1s = g_bloomq.b1_ser; bloomq_reset(); }
            else { RED_ONCE(63, "泛光 1/4 狀態缺少 B1 句柄"); bloomq_reset(); }
        } else if (!g_bloomq.state && g_bloomq_last_b1h && h_ser(g_bloomq_last_b1h) == g_bloomq_last_b1s && !strncmp(f->key, "2e2d6700a2569795", 16)) {   // B1 句柄仍指同一張貼圖（換分辨率重建後不認） | The B1 handle still points to the same texture (not recognized after a rebuild on resolution change)
            // 本幀泛光鏈沒跑（運行時外推的外推幀跳過了它：輸入 469 區域外是上一幀 LDR，不能跑）就到了泛光疊加：照最近一次改讀 B1，疊上一幀的泛光。
            // 不改讀就讀 B0——開了泛光 1/4 的話 B0 的上採樣一直被跳過、從沒寫過，讀到的是未初始化顯存（M4 Pro 上區域裡的單位整個發綠，別處多半是 0、缺泛光）
            // The bloom overlay is reached without the bloom chain having run this frame (the extrapolated frame of runtime extrapolation skipped it: outside the region the input 469 is the previous frame's LDR, so it cannot run): redirect the read to B1 as last time, overlaying the previous frame's bloom.
            // Without the redirect it would read B0 -- with bloom 1/4 on, B0's upsample is always skipped and never written, so what is read is uninitialized video memory (on M4 Pro the units in the region turn entirely green; elsewhere it is mostly 0, i.e. missing bloom)
            int hit = 0; for (int r = 0; r < 128; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == g_bloomq_last_b0) { S.tex[1][r] = g_bloomq_last_b1h; hit = 1; }
            if (hit) { PD("D %llu 泛光鏈沒跑：疊加沿用上一幀 B1", g_stat_pass); static uint64_t n; if (n++ % 600 == 0) MLOG("泛光 1/4：泛光鏈沒跑的幀疊加沿用上一幀 B1（第 %llu 次）", (unsigned long long)n); }
        } else if (g_bloomq.state) { RED_ONCE(61, "泛光 1/4 起點：鏈中途出現別的繪製，本幀泛光不完整"); bloomq_reset(); }
      }
    }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) return;
    // 續用的通道可能是少掛深度 / 模板開的（開法 2 / 3 / 4）：這次繪製要的平面沒掛，就在它之前切開（重開時按新前瞻掛）
    // The continued render pass may have been opened with fewer depth / stencil attachments (open modes 2 / 3 / 4): if the plane this draw needs is not attached, split before it (on reopen, attach per the new lookahead)
    if (g_rt_merged && g_enc_mode != 0 && (((S.ds.dt || S.ds.dw) && !g_pass.hd) || (S.ds.st && !g_pass.hs))) {
        static int once2; if (!once2++) MLOG("同目標續用：續用通道內繪製要的深度 / 模板平面沒掛，在此切開（首例 通道 %llu）", g_stat_pass);
        PD("M %llu 續用通道內繪製要的深度 / 模板平面沒掛，在此切開", g_stat_pass); rp_end(); }
    if (g_rt_range_active && (!rt_range_pr_ro(pr) || !S.ds.dt || S.ds.dw || S.ds.st)) rp_end();   // 原encoder邊界的外部寫可見性不刪 | The external write visibility at the original encoder boundary is not removed
    if (g_rt_merged && draw_samples_rt(pr)) { static int once; if (!once++) MLOG("同目標續用：續用通道內繪製要採樣附件，在此切開（首例 通道 %llu）", g_stat_pass);
        PD("M %llu 續用通道內繪製要採樣附件，在此切開", g_stat_pass); rp_end(); }
    static id<MTLFunction> fz_fn;   // 合併通道後一個通道本次繪製的特化片元函數（nil = 用原版本） | Specialized fragment function for this draw of the later pass in a merged render pass (nil = use the original version)
    if (g_fz.active && g_enc && g_fz.phase == 2 && !fz_p2_draw(pr, dw, di, g_fz.lmap2, &fz_fn)) {   // 證不出：結束合併通道（前一個通道的結果照常寫回），按後一個通道的原目標重開 | Cannot be proven: end the merged render pass (the earlier pass's result is stored as usual) and reopen with the later pass's original targets
        static int once3; if (!once3++) MLOG("可讀 tile 通道合併：後一個通道的繪製證不出，退回分開（首例 通道 %llu）", g_stat_pass);
        g_fz_fallback++; rp_end(); }
    if (R.has_sc && !g_enc) {
        NSUInteger tw0 = 0, th0 = 0; rt_dims(&tw0, &th0);
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw0) x1 = tw0; if (y1 > (long)th0) y1 = th0;
        if (x1 <= x0 || y1 <= y0) return;
    }
#ifdef NVN_RT_RANGE_TEST
    if (!g_enc) rr_begin();
#endif
    int defer_q_draw = !g_enc && !g_defer_replaying && g_defer_natq > 0;   // Q 開通道的這次繪製：通道照常經 hoist_try / fuse_try 開好後，先回放 P 再畫本次 | The draw that opens Q's render pass: after the render pass is opened as usual through hoist_try / fuse_try, replay P first and then draw this one
    if (g_frz_state) goto frz_plain;   // 世界凍結的完整幀 / 凍結幀：不前提、不推遲、不合併（跳通道的狀態機要看到每一條設目標） | Full frame / frozen frame of world freeze: no hoisting, no deferral, no merging (the pass-skipping state machine must see every set-target)
    if (!g_enc && tbdr_on()) hoist_try();   // 先把夾在本通道與其續用之間的純清除通道提前做掉（rp_begin_ex 之前，不佔本通道） | First hoist and execute the pure-clear render passes sandwiched between this render pass and its continuation (before rp_begin_ex, not occupying this render pass)
    // 推遲判定在 hoist_try 之後：被提前的純清除通道在前瞻裡跳過，P 的同目標續段才認得出來
    // The deferral decision comes after hoist_try: hoisted pure-clear render passes are skipped in the lookahead, and only then can P's same-target continuation segment be recognized
    if (!g_enc && tbdr_on() && !defer_has_in_p() && !g_defer_replaying && !g_fz.active && !g_fz_checking && g_exec_next) { defer_try(dw, di); if (g_defer_cur->active && g_defer_cur->in_p) return; }
    if (!g_enc && tbdr_on() && !g_fz_checking) fuse_try();   // 可讀 tile 通道合併缺省開；結構 / 本像素證明不成立仍退回普通通道，CHECK 重畫時不重入 | Readable tile render pass merge is on by default; when the structure / same-pixel proof does not hold it still falls back to a normal render pass, and it is not re-entered during the CHECK redraw
frz_plain:;
    int need = g_enc || !tbdr_on() ? 3 : pass_ds_need(), mode = defer_q_draw ? 0 : need == 0 ? 2 : need == 2 ? 3 : need == 1 ? 4 : 0;   // 2 都不掛 3 只掛模板 4 只掛深度 | 2 attach neither, 3 attach stencil only, 4 attach depth only
    int skip_d = mode != 0;
    if (g_enc && !g_fbx_n && fbx_on() && fbx_hazard(pr)) rp_end();   // 通道中途要讀自己的附件：先結束通道，下面重開時拷副本 | a draw mid-pass reads its own attachment: end the pass first, the copy is made when it reopens below
    if (!g_enc) { g_fd_dw = dw; g_fd_di = di; fbx_prepare(pr); }
    id<MTLRenderCommandEncoder> en = rp_begin_ex(mode);
    g_fd_dw = NULL; g_fd_di = NULL;
    if (!en) return;
    if (defer_q_draw) defer_replay();
    if (skip_d) { g_stat_ds_omit++; if (mode == 3) g_stat_s_only++; else if (mode == 4) g_stat_d_only++; }
    // 管線只依賴（程序, 通道目標, 混合 / 顏色 / 寫掩碼 / 多重採樣狀態, 頂點佈局）：這些沒變就沿用上次的
    // The pipeline depends only on (program, render pass targets, blend / color / write mask / multisample state, vertex layout): if these did not change, reuse the previous one
    static id<MTLRenderPipelineState> cur_ps; static NvProg *cur_pr; static uint64_t cur_ps_pass;
    if (g_fz.active && !g_defer_replaying) { g_pso_lmap = g_fz.phase == 1 ? g_fz.lmap1 : g_fz.lmap2; g_pso_fsfn = g_fz.phase == 1 ? nil : fz_fn; g_pso_fz = g_fz.phase == 1 ? (pso_fz_desc){0} : g_fz_desc; g_pso_dirty = 1; }   // 可讀 tile 合併：按物理 → 邏輯映射與特化取管線 | Readable tile merge: fetch the pipeline by the physical -> logical mapping and the specialization
    if (g_pso_dirty || pr != cur_pr || cur_ps_pass != g_pass.serial) { cur_ps = pso_get(pr, nil); cur_pr = pr; cur_ps_pass = g_pass.serial; g_pso_dirty = !cur_ps; }
    if (g_pso_lmap) { g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = 1; }   // 下一次繪製重新按自身情況取 | The next draw fetches again according to its own situation
    __unsafe_unretained id<MTLRenderPipelineState> ps = cur_ps;
    if (!ps) return;
    if (ps != g_enc_pso) { enc_r_pso(en, ps); g_enc_pso = ps; }
    static id<MTLDepthStencilState> cur_ds; static uint64_t cur_ds_pass;
    if (g_dss_dirty || cur_ds_pass != g_pass.serial) { cur_ds = dss_get(g_pass.hd, g_pass.hs); cur_ds_pass = g_pass.serial; g_dss_dirty = 0; }
    if (cur_ds != g_enc_dss) { enc_r_dss(en, cur_ds); g_enc_dss = cur_ds; }
    if (g_rt_ovr && g_pass.hs) enc_r_stencil_ref(en, g_rt_ovr == 3 ? 0x40 : 0, 0); else enc_r_stencil_ref(en, S.st_ref[0], S.st_ref[1]);
    // 視口 / 剪裁（夾到目標尺寸內）
    // Viewport / scissor (clamped within the target size)
    NSUInteger tw = g_pass.tw, th = g_pass.th;
    // 界面矩形不貼畫面頂邊時（iPad 真全屏：界面按 16:9 擺在畫面底部，見 dm/host/dm_uiscreen.c），界面繪製整體下移：投影矩陣加縱向偏移（ui_mat_patch），剪裁跟著下移。
    // 界面繪製 = 4 頂點、2 個屬性、頂點 20 字節，獨佔一趟、幀末合成（量過：啓動讀取、選角、建角、進局讀取、局內、開面板、全屏地圖都不混別的繪製）。
    // 矩陣這次讀不到（本提交裡還有沒落地的寫）時退回挪視口：位置一樣，只是貼頂上提的控件這一次會被視口頂邊裁掉。
    // 從頂邊起的剪裁（y ≤ 0）不下移頂邊：宿主把貼頂的控件（小地圖、地名等）上提回真實畫面頂部時，它們畫在界面矩形上方，不能被裁掉。
    // 例外：不帶貼圖、正好蓋滿整個界面矩形的那一塊（開面板時的壓暗、轉場的黑幕）改成蓋滿整個畫面——視口縱向放大 1/h、不下移，否則界面矩形外的 3D 留一條明暗分界。
    // 沒人登記入口或界面矩形就是整個畫面（iPhone、Mac 缺省）時 vo = so = 0、vs = 1、不改矩陣，下面與原來逐字相同。
    // When the UI rectangle does not touch the top edge of the picture (iPad true full screen: the UI is laid out at 16:9 at the bottom of the picture, see dm/host/dm_uiscreen.c), UI draws are shifted down as a whole: a vertical offset is added to the projection matrix (ui_mat_patch), and the scissor moves down with it.
    // A UI draw = 4 vertices, 2 attributes, 20-byte vertices, in a pass of its own and composited at end of frame (measured: startup loading, character select, character creation, entering-game loading, in-game, open panel, and full-screen map never mix in other draws).
    // When the matrix cannot be read this time (there are still unlanded writes in this submit), fall back to moving the viewport: same position, except that widgets lifted to the top get clipped by the viewport's top edge this one time.
    // A scissor starting from the top edge (y <= 0) does not have its top edge moved down: when the host lifts top-anchored widgets (minimap, area name etc.) back to the real top of the picture, they are drawn above the UI rectangle and must not be clipped.
    // Exception: the untextured block that exactly covers the whole UI rectangle (the dimming when a panel opens, the black curtain of a transition) is changed to cover the whole picture -- the viewport is scaled vertically by 1/h and not moved down, otherwise the 3D outside the UI rectangle is left with a light/dark dividing line.
    // When nobody registered the entry or the UI rectangle is the whole picture (iPhone, Mac default), vo = so = 0, vs = 1, the matrix is not changed, and what follows is word-for-word the same as before.
    long vo = 0, so = 0; double vs = 1, moff = 0;
    if (g_ui_rect_fn && dw && dw->count == 4 && S.nattr == 2 && S.stream[0].stride == 20) { double uy = 0, uh = 1; g_ui_rect_fn(&uy, &uh);
        if (uh > 0 && uh < 1) { if (ui_cover_all(uh)) vs = 1 / uh; else if (uy > 0) { so = lround(uy * th); if (ui_mat_ok()) moff = 2 * uy; else { vo = so; static int n; if (n++ < 4) MLOG("界面下移：投影矩陣這次讀不到，退回挪視口（通道 %llu）", g_stat_pass); } } } }
    if (moff != g_ui_moff) { g_ui_moff = moff; g_args_dirty[0] = 1; }
    if (S.has_vp) enc_r_viewport(en, (MTLViewport){S.vp[0][0], S.vp[0][1] + vo, S.vp[0][2], S.vp[0][3] * vs, S.dr[0], S.dr[1]});
    else enc_r_viewport(en, (MTLViewport){0, vo, tw, th * vs, 0, 1});
    if (S.depth_clamp != g_enc_clamp) { enc_r_depth_clip(en, !S.depth_clamp); g_enc_clamp = S.depth_clamp; }
    if (passdump()) PD("w %llu 視口 %g,%g,%g,%g 深度 %g–%g 夾限 %d 深度測試 %d/%d 寫 %d", g_stat_pass, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0], S.dr[1], S.depth_clamp, S.ds.dt, S.ds.df, S.ds.dw);
    MTLScissorRect sc = {0, 0, tw, th};
    if (R.has_sc) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y + so, x1 = (long)R.sc.x + R.sc.w, y1 = vs != 1 ? (long)th : (long)R.sc.y + R.sc.h + so;
        if (x1 > (long)tw) x1 = tw; if (y1 > (long)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) return;
        sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    enc_r_scissor(en, sc);
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) return;
    enc_r_cull(en, S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone);
    enc_r_winding(en, S.poly.front == NVN_FRONT_FACE_CCW ? MTLWindingCounterClockwise : MTLWindingClockwise);
    enc_r_fill(en, S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill);
    if (S.poly.offset_en & 4) enc_r_depth_bias(en, S.po.units, S.po.factor, S.po.clamp); else enc_r_depth_bias(en, 0, 0, 0);
    enc_r_blend_color(en, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]);
    for (int s_ = 0; s_ < 16; s_++) {
        if (!S.vbuf[s_].address) continue;
        void *mb; uint64_t mo;
        uint64_t gpu;
        if (mtl_gpu_addr(S.vbuf[s_].address, 1, &mb, &mo, &gpu)) { enc_r_vbuf(en, 16 + s_, gpu); use_rec("buf", S.vbuf[s_].address, S.vbuf[s_].size, 0, NULL); }
    }
    static id<MTLBuffer> zero; if (!zero) { zero = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(zero); }
    static uint64_t zero_gpu; if (!zero_gpu) zero_gpu = zero.gpuAddress;
    enc_r_vbuf(en, 30, zero_gpu);
    if (pr->sh[0]) encode_args(en, pr->sh[0], 0, 1);
    if (pr->sh[1]) encode_args(en, pr->sh[1], 1, 0);
    // 參數綁定在登記表鎖內捕獲實際資源；資格判斷到綁定期間有版本變化，則在任何GPU draw前切開重試。
    // Parameter binding captures the actual resources under the registry lock; if the version changed between the eligibility check and the binding, split and retry before any GPU draw.
    if (g_rt_range_active && (g_pass.reg_gen != g_reg_gen || g_pass.ser_gen != g_ser_gen || g_pass.range_dirty != nvn_dirty_now())) { rp_end(); draw(dw, di); return; }
    int prim = dw ? dw->prim : di->prim;
    MTLPrimitiveType mp;
    switch (prim) {
        case 0: mp = MTLPrimitiveTypePoint; break; case 1: mp = MTLPrimitiveTypeLine; break; case 3: mp = MTLPrimitiveTypeLineStrip; break;
        case 4: case 7: mp = MTLPrimitiveTypeTriangle; break; case 5: mp = MTLPrimitiveTypeTriangleStrip; break;
        default: RED_ONCE(19, "圖元類型 %d 未實現", prim); return;
    }
    if (g_vis_counting) {   // 遮擋查詢計數：本通道新槽（報告 / 清零後也換新槽） | Occlusion query counting: a new slot for this render pass (also switch to a new slot after a report / reset)
        if (!g_enc_has_vis) RED_ONCE(34, "遮擋查詢計數開始時所在通道沒掛可見性緩衝（首次清零發生在通道中途），該通道的採樣不計");
        else {
            if (g_vis_cur < 0 || g_vis_cur_pass != g_pass.serial) {
                if (g_vis_n >= VIS_SLOTS) RED_ONCE(35, "可見性槽用盡");
                else { g_vis_cur = (int)g_vis_n++; g_vis_cur_pass = g_pass.serial;
                    if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; g_vs = realloc(g_vs, g_vscap * sizeof *g_vs); }
                    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), (uint32_t)g_vis_cur}; }
            }
            if (g_vis_cur >= 0 && g_enc_vis != g_vis_cur + 1) { enc_r_visibility(en, MTLVisibilityResultModeCounting, (NSUInteger)g_vis_cur * 8); g_enc_vis = g_vis_cur + 1; }
        }
    }
    g_stat_draw++;
    { static int tg; if (!tg) { tg = 1; const char *e = getenv("DM_TEST_GHOST"); float v[8];   // 測試開關 DM_TEST_GHOST=px,py,gx,gy,gw,gh,cx,cy（歸一化；按下點、格網、跟手圖中心）：不經前端直接開跟手圖（Mac 宿主截圖驗證用） | Test switch DM_TEST_GHOST=px,py,gx,gy,gw,gh,cx,cy (normalized; press point, grid, center of the drag-follow image): turns on the drag-follow image directly without going through the front end (for Mac host screenshot verification)
        if (e && sscanf(e, "%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) mtl_ui_ghost(1, v[0], v[1], &v[2], v[6], v[7]); } }
    if (dw && atomic_load(&g_ghost_on)) ghost_note(dw);
    if (enc_label_on() && !g_el_draws++ && pr->sh[1]) { memcpy(g_el_key, pr->sh[1]->key, 8); g_el_key[8] = 0; }
    if (passdump()) {   // 通道首次繪製的特徵（判讀入是否可省：全屏、無混合、寫滿通道、不依賴深度）；DM_NVN_PASSDUMP_ALL 時每次繪製都記 | Characteristics of a render pass's first draw (to judge whether the load can be omitted: full screen, no blending, writes all channels, does not depend on depth); with DM_NVN_PASSDUMP_ALL every draw is recorded
        static uint64_t last_pass = ~0ull; static int all = -1; if (all < 0) all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
        if (all) PD("d %llu bv=%d bi=%d first=%d vs=%s fs=%s vc=%d inst=%d blend=%#x b0=%#llx dt=%d dw=%d df=%d cull=%d st=%d sf=%d,%d sop=%d,%d,%d/%d,%d,%d smask=%#x,%#x/%#x,%#x sref=%#x,%#x ib=%#llx it=%d cmask=%#x", g_stat_pass, di ? di->base_vertex : 0, di ? di->base_instance : 0, dw ? dw->first : 0, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
                    dw ? dw->count : di->count, dw ? dw->instances : di->instances, S.color.blend_en, (unsigned long long)*(uint64_t *)&S.blend[0], (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.poly.cull,
                    (int)S.ds.st, (int)S.ds.sf_f, (int)S.ds.sf_b, (int)S.ds.fail_f, (int)S.ds.zfail_f, (int)S.ds.zpass_f,
                    (int)S.ds.fail_b, (int)S.ds.zfail_b, (int)S.ds.zpass_b, S.st_vmask[0], S.st_vmask[1], S.st_wmask[0], S.st_wmask[1], S.st_ref[0], S.st_ref[1],
                    di ? (unsigned long long)di->ibuf : 0ull, di ? di->itype : 0, S.chmask.mask);
        if (all && pr->sh[0]) {   // 頂點階段各常量緩衝的內容指紋（判某次繪製跨幀是否完全相同：陰影緩存等用）；讀不到（本提交待執行的寫）記 P | Content fingerprint of each constant buffer of the vertex stage (to judge whether a draw is completely identical across frames: used by the shadow cache etc.); when unreadable (a write pending execution in this submit) record P
            char ub[400]; size_t o = 0; NvShader *v = pr->sh[0];
            for (int i = 0; i < v->rf.nb && o < sizeof ub - 40; i++) if (v->rf.b[i].kind == NVN_RB_CBUF && v->rf.b[i].reg < 16) {
                const p_bind_buf *u = &S.ubo[0][v->rf.b[i].reg]; void *mb; uint64_t mo; const uint8_t *base; uint64_t h = 1469598103934665603ull;
                if (!u->addr || !u->size) { o += snprintf(ub + o, sizeof ub - o, " r%d=-", v->rf.b[i].reg); continue; }
                if (pend_hits(u->addr, u->size) || !mtl_addr(u->addr, u->size, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { o += snprintf(ub + o, sizeof ub - o, " r%d=P/%llu", v->rf.b[i].reg, (unsigned long long)u->size); continue; }
                uint64_t h64 = 0; for (uint64_t q = 0; q < u->size; q++) { h = (h ^ base[mo + q]) * 1099511628211ull; if (q == 63) h64 = h; }   // h64：前 64 字節（物體緩衝的世界矩陣 vec0–3） | h64: the first 64 bytes (world matrix vec0–3 of the object buffer)
                { static uint64_t hp = ~0ull; static int hn; if (hp != g_stat_pass) { hp = g_stat_pass; hn = 0; }   // 每通道前 6 次繪製另把 ≤ 608 字節的緩衝按十六進制打出來（逐字節比跨幀變了哪幾個字段） | For the first 6 draws of each render pass, additionally print buffers of <= 608 bytes in hex (to compare byte by byte which fields changed across frames)
                  if (hn < 12 && u->size <= 608 && v->rf.b[i].reg == 3) { hn++; char hx[1300]; for (uint64_t q = 0; q < u->size; q++) snprintf(hx + 2 * q, 3, "%02x", base[mo + q]); PD("x %llu r%d %s", g_stat_pass, v->rf.b[i].reg, hx); } }
                o += snprintf(ub + o, sizeof ub - o, " r%d=%016llx/%llu/%016llx", v->rf.b[i].reg, (unsigned long long)h, (unsigned long long)u->size, (unsigned long long)h64); }
            PD("u %llu%s", g_stat_pass, ub); }
        if (all) { char vb[1600]; size_t o = 0; o += snprintf(vb + o, sizeof vb - o, "nattr=%d", S.nattr);
            for (int i = 0; i < S.nattr && i < 16 && o < sizeof vb - 100; i++) { int st = S.attr[i].stream; if (!S.attr[i].fmt) continue;   // 只打有格式的屬性 | only attributes with a format
                o += snprintf(vb + o, sizeof vb - o, " a%d{fmt=%#x off=%d s=%d stride=%u div=%u vb=%#llx/%llu}", i, S.attr[i].fmt, S.attr[i].offset, st,
                              st < 16 ? S.stream[st].stride : 0, st < 16 ? S.stream[st].divisor : 0, st < 16 ? (unsigned long long)S.vbuf[st].address : 0ull, st < 16 ? (unsigned long long)S.vbuf[st].size : 0ull); }
            PD("v %llu %s", g_stat_pass, vb);
            // DM_NVN_CBDUMP + DM_NVN_CBDUMP_KEY=頂點著色器鍵：另存該繪製的各頂點流與索引（F幀_p通道_vb流號_地址.bin / _ib_地址.bin）
            // DM_NVN_CBDUMP + DM_NVN_CBDUMP_KEY=vertex shader key: also save that draw's vertex streams and indices (F<frame>_p<pass>_vb<stream>_<address>.bin / _ib_<address>.bin)
            const char *cbd = getenv("DM_NVN_CBDUMP"), *cbk = getenv("DM_NVN_CBDUMP_KEY");
            if (cbd && cbk && pr->sh[0] && !strcmp(pr->sh[0]->key, cbk)) {
                char fp[1024];
                for (int st = 0; st < 16; st++) if (S.vbuf[st].address && S.vbuf[st].size) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_vb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, st, (unsigned long long)S.vbuf[st].address);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)S.vbuf[st].address, 1, S.vbuf[st].size, f); fclose(f); } }
                if (di && di->ibuf) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_ib%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, di->itype, (unsigned long long)di->ibuf);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)di->ibuf, 1, (size_t)di->count << (di->itype == 2 ? 2 : di->itype), f); fclose(f); } }
            } }
        if (last_pass != g_stat_pass) {
            last_pass = g_stat_pass;
            PD("D %llu prim=%d vc=%d inst=%d blend=%#x wm=%#x dt=%d dw=%d df=%d st=%d sc=%lu,%lu,%lu,%lu tgt=%lux%lu vp=%.0f,%.0f,%.0f,%.0f", g_stat_pass, prim,
               dw ? dw->count : di->count, dw ? dw->instances : di->instances, S.color.blend_en, S.chmask.mask, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.ds.st,
               (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height, (unsigned long)tw, (unsigned long)th,
               S.has_vp ? S.vp[0][0] : 0, S.has_vp ? S.vp[0][1] : 0, S.has_vp ? S.vp[0][2] : (float)tw, S.has_vp ? S.vp[0][3] : (float)th);
        }
    }
    if (rt_range_on() && !rt_range_pr_ro(pr)) g_pass.range_external=1;
    rt_range_note_depth();
#ifdef NVN_RT_RANGE_TEST
    rr_snap(dw,di);
#endif
    udc_note_draw(pr);
    // 0 個圖元的繪製（遊戲會發 count=0）不畫任何像素，跳過編碼；Metal 規定計數必須非零，校驗層否則當場中止（營地 / 野外 / 戰鬥頁實測）
    // A draw with 0 primitives (the game does send count=0) draws no pixels, so skip encoding; Metal requires the count to be non-zero, otherwise the validation layer aborts on the spot (measured on the camp / wilderness / combat pages)
    if (dw && prim == 7) {   // 四邊形：共享索引 0,1,2,0,2,3 | Quads: shared indices 0,1,2,0,2,3
        if (!g_quad_ib) {
            uint32_t n = 65536; uint32_t *ix = malloc(n * 6 * 4);
            for (uint32_t q = 0; q < n; q++) { uint32_t b = q * 4; uint32_t v[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; memcpy(ix + q * 6, v, 24); }
            g_quad_ib = [g_dev newBufferWithBytes:ix length:n * 24 options:MTLResourceStorageModeShared]; g_quad_gpu = g_quad_ib.gpuAddress; res_add(g_quad_ib); free(ix);
        }
        if (dw->count >= 4) enc_r_draw_indexed(en, mp, (dw->count / 4) * 6, MTLIndexTypeUInt32, g_quad_gpu, g_quad_ib.length, dw->instances > 0 ? dw->instances : 1, dw->first, dw->base_instance);
    } else if (dw) {
        if (dw->count && (dw->instances > 1 || dw->base_instance)) enc_r_draw_instanced(en, mp, dw->first, dw->count, dw->instances > 0 ? dw->instances : 1, dw->base_instance);
        else if (dw->count) enc_r_draw(en, mp, dw->first, dw->count);
    } else {
        if (prim == 7) { RED_ONCE(20, "帶索引的四邊形未實現"); return; }
        if (di->itype == 0) {   // 8 位索引：Metal 只有 16 / 32 位。回放時從客體內存讀出、展開成 16 位放進本幀競技場（SMBW 的全屏通道與最後拷進窗口那一下都用 8 位索引，不展開就整屏黑）
                                // 8-bit indices: Metal only has 16 / 32-bit. At replay they are read from guest memory and widened to 16-bit in this frame's arena (SMBW's full-screen passes and the final copy into the window all use 8-bit indices; without this the whole screen is black)
            if (!di->count) return;
            void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, di->count, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "索引緩衝不在內存池裡"); return; }
            if (pend_hits(di->ibuf, di->count)) RED_ONCE(48, "8 位索引緩衝本提交裡有 GPU 側寫入：按當前內容展開");
            id<MTLBuffer> ab; size_t off = arena_alloc(((size_t)di->count * 2 + 15) & ~(size_t)15, &ab);
            uint16_t *d = (uint16_t *)((uint8_t *)ab.contents + off); src += mo;
            for (int32_t i = 0; i < di->count; i++) d[i] = src[i];
            use_rec("buf", di->ibuf, (uint64_t)di->count, 0, NULL);
            enc_r_draw_indexed(en, mp, di->count, MTLIndexTypeUInt16, g_cur_arena->gpu + off, (NSUInteger)di->count * 2, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
            if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)di->count); g_rt_range_pending = 0; }
            return;
        }
        void *mb; uint64_t mo;
        uint64_t gpu;
        if (!mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &gpu)) { RED_ONCE(22, "索引緩衝不在內存池裡"); return; }
        use_rec("buf", di->ibuf, (uint64_t)di->count * (di->itype == 1 ? 2 : 4), 0, NULL);
        if (di->count) enc_r_draw_indexed(en, mp, di->count, di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, gpu, ((__bridge id<MTLBuffer>)mb).length - mo,
                di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
    }
    if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)(dw ? dw->count : di->count)); g_rt_range_pending=0; }
}
static nvn_hmap g_cps = {.klen = sizeof(void *)};
static void warm_claim(void) {
    if (!atomic_exchange_explicit(&g_warm_ready, 0, memory_order_acq_rel)) return;
    os_unfair_lock_lock(&g_warm_lock);
    NSArray<WarmItem *> *items = [g_warm_items copy];
    [g_warm_items removeAllObjects];
    os_unfair_lock_unlock(&g_warm_lock);
    for (WarmItem *item in items) {
        if (item->kind == 0) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            if (!g_shcache[item->key]) { g_shcache[item->key] = item->shader; g_warm_claim_shaders++; }
        } else if (item->kind == 1) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            NvShader *vs = g_shcache[item->vs_key], *fs = item->fs_key.length ? g_shcache[item->fs_key] : nil;
            if (!vs) { vs = item->vs; g_shcache[item->vs_key] = vs; }
            if (item->fs_key.length && !fs) { fs = item->fs; g_shcache[item->fs_key] = fs; }
            item->pso.vs = (__bridge void *)vs; item->pso.fs = (__bridge void *)fs;
            if (!hm_get_id(&g_pso, &item->pso)) { hm_put_id(&g_pso, &item->pso, item->object); g_warm_claim_rps++; }
        } else {
            NvShader *cs = g_shcache[item->key];
            if (!cs) { cs = item->shader; if (!g_shcache) g_shcache = [NSMutableDictionary dictionary]; g_shcache[item->key] = cs; }
            const void *key = (__bridge const void *)cs;
            if (!hm_get_id(&g_cps, &key)) { hm_put_id(&g_cps, &key, item->object); g_warm_claim_cps++; }
        }
    }
}
// ---------- 3D 畫面加亮 ----------
// 色調映射（片元 a1b1aab66933fa8f）= 把合成後的 HDR ×0.008 做 PQ 編碼、再查 32³ 調色表（每幀由計算著色器 552f45c348a61b86 按區域重算，只被色調映射讀）。
// HDR 先乘曝光 k 等價於把表換成 L'(u) = L(PQ(k · PQ⁻¹(u)))：遊戲算完表的那次派發之後，拷一份再按此原地重寫（32³ 一次小派發）；表每幀重算，不會越乘越亮。
// 只動 3D 世界：界面、文字、血球在色調映射之後才合；亮部經 PQ 壓縮，不會像最後整屏乘系數那樣糊成一片白。普通 / 可讀 tile 合併 / 凍結各條路徑都只是讀這張表，一處生效。
// k 每幀由宿主 dm_bright_k() 給（dm/host/dm_bright.c：局外 1、迷宮類 4、原版畫面不隨晝夜 4、其餘按原版環境亮度推，平滑過渡）；k ≈ 1 時不重寫。
// ---------- 3D picture brightening ----------
// Tone mapping (fragment a1b1aab66933fa8f) = PQ-encode the composited HDR x0.008, then look up a 32^3 color lookup table (recomputed per region every frame by compute shader 552f45c348a61b86, read only by tone mapping).
// Multiplying HDR by exposure k first is equivalent to replacing the table with L'(u) = L(PQ(k * PQ^-1(u))): after the dispatch in which the game finishes computing the table, make a copy and rewrite it in place accordingly (one small 32^3 dispatch); the table is recomputed every frame, so it does not get brighter with each multiplication.
// Only the 3D world is touched: UI, text and the health orbs are composited after tone mapping; highlights are compressed by PQ and do not blur into a sheet of white as a final whole-screen multiply would. The normal / readable tile merge / freeze paths all just read this table, so it takes effect in one place.
// k is given every frame by the host's dm_bright_k() (dm/host/dm_bright.c: 1 out of game, 4 for maze-type areas, 4 where the original game's picture does not follow day and night, the rest derived from the original game's ambient brightness, with smooth transitions); no rewrite when k ~= 1.
__attribute__((weak)) float dm_bright_k(void) { return 1.0f; }   // 宿主 dm_bright.c 提供強定義；圖形單測不鏈宿主時不加亮 | The host's dm_bright.c provides the strong definition; no brightening when graphics unit tests do not link the host
static const char k_lut_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"constant float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"   // 與色調映射著色器裡的 PQ 常數逐位相同 | Bit-identical to the PQ constants in the tone mapping shader
"static float3 pq(float3 v) { float3 p = pow(abs(v), m1); return pow((c1 + c2 * p) / (1.0 + c3 * p), m2); }\n"
"static float3 pq_inv(float3 u) { float3 q = pow(u, 1.0 / m2); return pow(max(q - c1, 0.0) / max(c2 - c3 * q, 1e-6), 1.0 / m1); }\n"
"kernel void lutk(texture3d<float> src [[texture(0)]], texture3d<float, access::write> dst [[texture(1)]], constant float &k [[buffer(0)]], uint3 g [[thread_position_in_grid]]) {\n"
"  uint3 n = uint3(dst.get_width(), dst.get_height(), dst.get_depth()); if (any(g >= n)) return;\n"
"  constexpr sampler ls(filter::linear, address::clamp_to_edge, coord::normalized);\n"
"  float3 u = (float3(g) + 0.5) / float3(n); float3 u2 = clamp(pq(k * pq_inv(u)), 0.0, 1.0);\n"
"  float4 b = src.sample(ls, u2), o = src.read(g);\n"   // b：按曝光 k 提亮後的輸出；o：原版這一格 | b: output after brightening by exposure k; o: this cell in the original game
"  float s = smoothstep(0.65, 1.0, dot(b.rgb, float3(0.2126, 0.7152, 0.0722)));\n"   // 高光保護：提亮後越接近白，越收回原值（雪、天光不糊成一片白）；暗處 s = 0 照常提亮 | Highlight protection: after brightening, the closer to white, the more it is pulled back to the original value (snow and skylight do not blur into a sheet of white); in dark areas s = 0 and brightening proceeds as usual
"  dst.write(float4(max(mix(b.rgb, o.rgb, s), o.rgb), b.a), g); }\n";
static void bright_lut(NvShader *cs) {
    static int failed; static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at; static id<MTLTexture> tmp; static id<MTLBuffer> kb[3]; static int ki;
    if (failed || strncmp(cs->key, "552f45c348a61b86", 16)) return;
    float k = dm_bright_k(); if (!(k > 1.005f)) return;   // k ≈ 1（局外 / 正午）：表保持原版 | k ~= 1 (out of game / noon): the table stays as in the original game
    if (k > 4.0f) k = 4.0f;
    int tid = 0; for (int i = 0; i < cs->rf.nb && !tid; i++) if (cs->rf.b[i].kind == NVN_RB_IMG && cs->rf.b[i].reg < 32) tid = (int)(S.img[5][cs->rf.b[i].reg] & 0xfffff);
    pthread_mutex_lock(&g_rmu); id<MTLTexture> lut = tid ? tex_by_id(tid) : nil; pthread_mutex_unlock(&g_rmu);
    if (!lut || lut.textureType != MTLTextureType3D || !(lut.usage & MTLTextureUsageShaderWrite)) { static int n; if (!n++) MLOG("【紅】3D 畫面加亮：調色表認不出（貼圖號 %d），不加亮", tid); return; }
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_lut_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"lutk"] : nil;
        ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2; ad.maxBufferBindCount = 1; at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        for (int i = 0; i < 3; i++) { kb[i] = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; if (kb[i]) res_add(kb[i]); }
        if (!ps || !at || !kb[0] || !kb[1] || !kb[2]) { failed = 1; MLOG("【紅】3D 畫面加亮：計算管線建不出 %s（退回原版亮度）", e.localizedDescription.UTF8String); return; }
        MLOG("3D 畫面加亮：建好（調色表 %lux%lux%lu，曝光每幀由 dm_bright_k 給）", (unsigned long)lut.width, (unsigned long)lut.height, (unsigned long)lut.depth); }
    if (!tmp || tmp.width != lut.width || tmp.height != lut.height || tmp.depth != lut.depth || tmp.pixelFormat != lut.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.textureType = MTLTextureType3D; d.pixelFormat = lut.pixelFormat; d.width = lut.width; d.height = lut.height; d.depth = lut.depth;
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; if (tmp) defer_obj_release(tmp); tmp = [g_dev newTextureWithDescriptor:d]; if (!tmp) { failed = 1; return; } res_add(tmp); }
    id<MTLBuffer> b0 = kb[ki]; ki = (ki + 1) % 3; *(float *)b0.contents = k;   // 三份輪換：在途的幀還在讀上一份 | Three copies in rotation: in-flight frames are still reading the previous copy
    id<MTL4ComputeCommandEncoder> b = blit(); enc_b_copy_texture(b, lut, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(lut.width, lut.height, lut.depth), tmp, 0, 0, MTLOriginMake(0, 0, 0)); enc_b_end(g_blit); g_blit = nil;
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL); [ce setArgumentTable:at]; [at setTexture:tmp.gpuResourceID atIndex:0]; [at setTexture:lut.gpuResourceID atIndex:1]; [at setAddress:b0.gpuAddress atIndex:0];
    enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake((lut.width + 3) / 4, (lut.height + 3) / 4, (lut.depth + 3) / 4), MTLSizeMake(4, 4, 4)); enc_c_end(ce);
    args_dirty_all();
}
static void dispatch(const p_dispatch *dp) {
    zskip_clear();
    if (!S.prog) { RED_ONCE(27, "派發前沒有綁定程序"); return; }
    NvShader *cs = prog_get(S.prog)->sh[5];
    if (!cs) { RED_ONCE(28, "派發的程序沒有計算著色器（或未映射）"); return; }
    const void *k = (__bridge const void *)cs;
    id<MTLComputePipelineState> ps = hm_get_id(&g_cps, &k);
    if (!ps) {
        NSError *e = nil; uint64_t bt = bp_now(); ps = [g_dev newComputePipelineStateWithFunction:cs->fn error:&e];
        g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
        if (!ps) { MLOG("【紅】建計算管線失敗：%s", e.localizedDescription.UTF8String); return; }
        pso_record("C\t%s\n", cs->key);
        if (cs->key[0]) pso_user_record("C\t%s\n", cs->key);
        hm_put_id(&g_cps, &k, ps);
    }
    if (cs->builtin) {   // 內嵌計算核：n = uniform 緩衝 0，src = 存儲緩衝 0，dst = 存儲緩衝 1 | Built-in compute kernel: n = uniform buffer 0, src = storage buffer 0, dst = storage buffer 1
        const p_bind_buf *u = &S.ubo[5][0], *a0 = &S.ssbo[5][0], *a1 = &S.ssbo[5][1];
        void *ub, *sb, *db; uint64_t uo, so, dof;
        if (!mtl_addr(u->addr, 4, &ub, &uo) || !mtl_addr(a0->addr, 8, &sb, &so) || !mtl_addr(a1->addr, 8, &db, &dof)) { RED_ONCE(30, "內嵌計算核 %s 的緩衝沒綁全", cs->builtin); return; }
        if (!cs->logged++) {
            module_t *m = module_of((void *)dp->caller);
            MLOG("內嵌計算核 %s 首次派發：調用點 %s+%#llx，線程組 %dx%dx%d，元素數（CPU 側當前值）%u", cs->builtin, m ? m->name : "?",
                 m ? (unsigned long long)(dp->caller - (uint64_t)m->base) : (unsigned long long)dp->caller, dp->x, dp->y, dp->z, *(uint32_t *)u->addr);
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLComputeCommandEncoder> ce = pg_compute();
        enc_c_pso(ce, ps);
        enc_c_args(ce, ((__bridge id<MTLBuffer>)ub).gpuAddress + uo, 0); enc_c_args(ce, ((__bridge id<MTLBuffer>)sb).gpuAddress + so, 1); enc_c_args(ce, ((__bridge id<MTLBuffer>)db).gpuAddress + dof, 2);   // 內嵌計算核：冷路徑，直接取地址 | Built-in compute kernel: cold path, take the addresses directly
        enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(32, 1, 1));
        enc_c_end(ce);
        return;
    }
    const uint16_t *tg = cs->rf.tg;
    if (!tg[0]) { RED_ONCE(29, "計算著色器缺線程組大小反射"); return; }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> ce = pg_compute();
    enc_c_pso(ce, ps);
    encode_args(ce, cs, 5, 0);
    enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(tg[0], tg[1], tg[2]));
    enc_c_end(ce);
    bright_lut(cs);   // 3D 畫面加亮：遊戲算完調色表之後重寫 | 3D picture brightening: rewrite after the game finishes computing the color lookup table
}
// GPU：貼圖 → 緩衝（行距取 SetCopyRowStride，0 = 緊排）
// GPU: texture -> buffer (row stride taken from SetCopyRowStride, 0 = tightly packed)
static void copy_t2b(const p_copy_b2t *c) {
    if (c->tex) xv_check(c->tex->serial, c->has_view && c->view.set_levels ? c->view.base_level : 0, c->has_view && c->view.set_layers ? c->view.min_layer : 0, "貼圖讀緩衝");
    id<MTLTexture> t = tex_mtl(c->tex);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(31, "貼圖→緩衝拷貝的目標不在內存池裡"); return; }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_texture_buffer(b, t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset), MTLSizeMake(c->r.width, c->r.height, nz), (__bridge id<MTLBuffer>)mb, mo, row, img);
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture_buffer(b, t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0), MTLSizeMake(c->r.width, c->r.height, 1), (__bridge id<MTLBuffer>)mb, mo + z * img, row, img);
}
// CPU 讀貼圖（ReadTexels）：預排布貼圖以池內存為真值，解排布直讀；其餘從 MTLTexture blit 讀回（調用方已按 NVN 規矩等過 GPU）
// CPU texture read (ReadTexels): for packaged textures the pool memory is the ground truth, de-layout and read directly; the rest are read back from the MTLTexture by blit (the caller has already waited for the GPU per NVN rules)
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    xv_check(t->serial, level, layer0, "CPU讀回");
    PD("U cpu_read ser=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d dst=%p row=%lu img=%lu", t->serial, level, layer0, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth, dst, (unsigned long)row, (unsigned long)img);
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    if (t->has_layout && t->mem) {
        uint32_t lw = MAX(t->d.width >> level, 1u), lh = MAX(t->d.height >> level, 1u), lz = is3d ? MAX(t->d.depth >> level, 1u) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { RED_ONCE(32, "ReadTexels 解排布失敗"); free(lin); return; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin);
        return;
    }
    id<MTLTexture> mt = tex_mtl(t);
    if (!mt) { memset(dst, 0, img * nz); return; }
    mtl_finish_gpu();
    id<MTLBuffer> buf = [g_dev newBufferWithLength:wb * hb * nz options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = enc_b_begin(cb);
    for (int z = 0; z < nz; z++)
        [b copyFromTexture:mt sourceSlice:is3d ? 0 : layer0 + r->zoffset + z sourceLevel:level sourceOrigin:MTLOriginMake(r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0)
                sourceSize:MTLSizeMake(r->width, r->height, 1) toBuffer:buf destinationOffset:z * wb * hb destinationBytesPerRow:wb destinationBytesPerImage:wb * hb];
    enc_b_end(b); [cb commit]; [cb waitUntilCompleted];
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, (uint8_t *)buf.contents + z * wb * hb + y * wb, wb);
}
static void copy_b2t(const p_copy_b2t *c) {
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    PD("C ser=%llu", c->tex->serial);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(23, "緩衝→貼圖拷貝的源不在內存池裡"); return; }
    uint32_t upf; int ubpp;
    if (bc_unpack(c->tex->d.format, &upf, &ubpp)) {   // 無 BC：源裡的壓縮塊在 CPU 上解壓進臨時緩衝，再按像素拷進解壓格式的貼圖 | No BC: the compressed blocks in the source are decompressed on the CPU into a temporary buffer, then copied pixel-wise into the decompressed-format texture
        const uint8_t *src = [(__bridge id<MTLBuffer>)mb contents];
        if (!src) { RED_ONCE(46, "無 BC 解壓：緩衝→貼圖拷貝的源緩衝 CPU 讀不到"); return; }
        size_t up = (size_t)c->r.width * ubpp, uimg = up * c->r.height;
        id<MTLBuffer> tb = [g_dev newBufferWithLength:uimg * nz ?: 16 options:MTLResourceStorageModeShared];
        for (int z = 0; z < nz; z++) nvn_bc_decode(c->tex->d.format, src + mo + z * img, row, c->r.width, c->r.height, (uint8_t *)tb.contents + z * uimg, up);
        res_add(tb);
        id<MTL4ComputeCommandEncoder> b = blit();
        if (is3d) enc_b_copy_buffer_texture(b, tb, 0, up, uimg, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
        else for (int z = 0; z < nz; z++) enc_b_copy_buffer_texture(b, tb, z * uimg, up, uimg, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
        defer_obj_release(tb);   // 本次提交跑完再放（並撤駐留） | Release after this submit has finished running (and remove from residency)
        return;
    }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo, row, img, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo + z * img, row, img, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
}

// 無 BC + 老機型補充包：遊戲把文件來的貼圖（已是 ASTC 4x4）整塊拷進自己建的 BC3 紋理數組（無 BC 時建成 RGBA8）。Switch 上兩種格式塊大小相同、
// 拷貝是按原始塊搬；這裡改成計算核逐像素解出來寫進去，sRGB 源先讀出線性值再編碼回 sRGB 值，結果 = 原樣搬塊後按目標格式讀到的值
// No BC + Legacy Device Pack: the game copies textures from files (already ASTC 4x4) whole into a BC3 texture array it creates itself (created as RGBA8 when there is no BC). On Switch the two formats have the same block size and
// the copy moves raw blocks; here it is changed to a compute kernel that decodes pixel by pixel and writes them in, with sRGB sources first read out as linear values and then encoded back to sRGB values, so the result = the values read in the destination format after moving the blocks as-is
static const char k_a2u_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"static float3 enc(float3 c) { return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }\n"
"kernel void a2u(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], constant uint4 *p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= p[1].x || g.y >= p[1].y) return; float4 v = src.read(g + p[0].xy); if (p[1].z) v.rgb = enc(v.rgb); dst.write(v, g + p[0].zw); }\n";
static int copy_t2t_astc(id<MTLTexture> s, id<MTLTexture> d, unsigned sl, unsigned ss, unsigned dl, unsigned ds, int sx, int sy, int dx, int dy, int w, int h) {
    static int failed; static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at;
    if (failed) return 0;
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_a2u_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"a2u"] : nil;
        ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2; ad.maxBufferBindCount = 1; at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        if (!ps || !at) { failed = 1; MLOG("【紅】無 BC：ASTC → 解壓格式拷貝的計算管線建不出 %s", e.localizedDescription.UTF8String); return 0; } }
    MTLTextureType tt = MTLTextureType2D;
    id<MTLTexture> sv = [s newTextureViewWithPixelFormat:s.pixelFormat textureType:tt levels:NSMakeRange(sl, 1) slices:NSMakeRange(ss, 1)];
    id<MTLTexture> dv = [d newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:tt levels:NSMakeRange(dl, 1) slices:NSMakeRange(ds, 1)];
    id<MTLBuffer> pb = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    if (!sv || !dv || !pb) { RED_ONCE(47, "無 BC：ASTC → 解壓格式拷貝建不出視圖 / 參數塊"); return 0; }
    uint32_t *p = pb.contents; p[0] = sx; p[1] = sy; p[2] = dx; p[3] = dy; p[4] = w; p[5] = h; p[6] = s.pixelFormat == MTLPixelFormatASTC_4x4_sRGB; p[7] = 0;
    res_add(pb);
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL); [ce setArgumentTable:at];
    [at setTexture:sv.gpuResourceID atIndex:0]; [at setTexture:dv.gpuResourceID atIndex:1]; [at setAddress:pb.gpuAddress atIndex:0];
    enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1), MTLSizeMake(8, 8, 1)); enc_c_end(ce);
    args_dirty_all();
    defer_obj_unref(sv); defer_obj_unref(dv); defer_obj_release(pb);   // 本次提交跑完再放（視圖不單獨駐留，參數塊撤駐留） | Release after this submit has finished running (views are not made resident separately; the parameter block is removed from residency)
    return 1;
}
// GPU：貼圖 → 貼圖（原先誤列為空操作，選角頁每幀 3 次）。等尺寸、不鏡像、同格式 → 逐層 blit；其餘語義（縮放帶過濾、鏡像、換格式）blit 表達不了，報紅待實現
// GPU: texture -> texture (previously mislisted as a no-op, 3 times per frame on the character select page). Same size, not mirrored, same format -> blit layer by layer; the other semantics (scaling with filtering, mirroring, format change) cannot be expressed by blit: report a failure, to be implemented
static void copy_t2t(const p_copy_t2t *c) {
    if (c->src) xv_check(c->src->serial, c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, "貼圖拷貝源");
    rp_flush();   // 掛起的純清除通道先落地（零源候選在通道結束時才進集合）；下面 blit() 本來也會結束通道 | Land the pending pure-clear render pass first (zero-source candidates only enter the set when the render pass ends); the blit() below would end the render pass anyway
    if (c->dst) zskip_del(c->dst->serial);
    if (c->src && c->dst && c->sr.xoffset == 0 && c->sr.yoffset == 0 && c->dr.xoffset == 0 && c->dr.yoffset == 0
        && c->sr.width == (int)c->src->d.width && c->sr.height == (int)c->src->d.height && c->dr.width == (int)c->dst->d.width && c->dr.height == (int)c->dst->d.height) {
        int sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
        int ss = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, ds = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
        if (zskip_has(c->src->serial, sl, ss)) zskip_add(c->dst->serial, dl, ds);
    }
    id<MTLTexture> s = tex_mtl(c->src), d = tex_mtl(c->dst);
    PD("T src=%llu dst=%llu %dx%dx%d→%dx%dx%d flags=%#x", c->src->serial, c->dst->serial, c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, c->flags);
    if (!s || !d) return;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (c->flags & DM_COPY_RESOLVE) {   // Downsample：多重採樣解析進單採樣目標（深度取 0 號樣本）；兩邊採樣數相同時就是普通整張拷貝 | Downsample: multisample resolve into the single-sample destination (depth takes sample 0); with equal sample counts it is a plain whole copy
        if (s.sampleCount == d.sampleCount && s.pixelFormat == d.pixelFormat && s.width == d.width && s.height == d.height) {
            enc_b_copy_texture(blit(), s, sz0, sl, MTLOriginMake(0, 0, 0), MTLSizeMake(s.width, s.height, 1), d, dz0, dl, MTLOriginMake(0, 0, 0));
            return;
        }
        if (s.sampleCount <= 1 || d.sampleCount != 1 || s.pixelFormat != d.pixelFormat || s.width != d.width || s.height != d.height) {
            RED_ONCE(72, "Downsample 未實現的形態：%lux%lu %lu 樣本 格式 %lu → %lux%lu %lu 樣本 格式 %lu", (unsigned long)s.width, (unsigned long)s.height, (unsigned long)s.sampleCount,
                     (unsigned long)s.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.sampleCount, (unsigned long)d.pixelFormat);
            return;
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLPixelFormat pf = s.pixelFormat;
        int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8;
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : rp.colorAttachments[0];
        a.texture = s; a.level = sl; a.slice = sz0; a.resolveTexture = d; a.resolveLevel = dl; a.resolveSlice = dz0;
        a.loadAction = MTLLoadActionLoad; a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        if (depth) rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        ser_gate(); enc_r_end(enc_r_begin(rp));
        return;
    }
    PD("t src=%llu lv%u sl%u xyz=%d,%d,%d dst=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d", c->src->serial, sl, sz0, c->sr.xoffset, c->sr.yoffset, c->sr.zoffset,
       c->dst->serial, dl, dz0, c->dr.xoffset, c->dr.yoffset, c->dr.zoffset, c->sr.width, c->sr.height, c->sr.depth);
    int s3 = c->src->d.target == 2, d3 = c->dst->d.target == 2, nz = c->sr.depth ? c->sr.depth : 1;
    if (no_bc() && (s.pixelFormat == MTLPixelFormatASTC_4x4_LDR || s.pixelFormat == MTLPixelFormatASTC_4x4_sRGB) && (d.pixelFormat == MTLPixelFormatRGBA8Unorm || d.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB)
        && !s3 && !d3 && c->sr.width == c->dr.width && c->sr.height == c->dr.height && (d.usage & MTLTextureUsageShaderWrite)) {   // 無 BC：ASTC 源 → 解壓格式目標（見 copy_t2t_astc） | No BC: ASTC source -> decompressed-format destination (see copy_t2t_astc)
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) ok = copy_t2t_astc(s, d, sl, sz0 + c->sr.zoffset + z, dl, dz0 + c->dr.zoffset + z, c->sr.xoffset, c->sr.yoffset, c->dr.xoffset, c->dr.yoffset, c->sr.width, c->sr.height);
        if (ok) return;
    }
    if (c->sr.width != c->dr.width || c->sr.height != c->dr.height || (c->sr.depth ? c->sr.depth : 1) != (c->dr.depth ? c->dr.depth : 1) || (c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT & ~NVN_COPY_FLAGS_LINEAR_FILTER_BIT)
        || s.pixelFormat != d.pixelFormat || s3 != d3) {
        RED_ONCE(33, "貼圖→貼圖拷貝未實現的形態：%dx%dx%d→%dx%dx%d 標誌 %#x 格式 %lu→%lu 3D %d→%d", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth,
                 c->flags, (unsigned long)s.pixelFormat, (unsigned long)d.pixelFormat, s3, d3);
        return;
    }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (s3)
        enc_b_copy_texture(b, s, 0, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, c->sr.zoffset), MTLSizeMake(c->sr.width, c->sr.height, nz), d, 0, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, c->dr.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture(b, s, sz0 + c->sr.zoffset + z, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, 0), MTLSizeMake(c->sr.width, c->sr.height, 1), d, dz0 + c->dr.zoffset + z, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, 0));
    // 診斷 DM_NVN_T2T_TRACE=寬x高:文件：拷進該尺寸 RGBA16F 貼圖的結果，在 GPU 時間線上逐次另拷一份進追蹤緩衝（幀號記在 CPU 側），滿 240 次寫文件
    // Diagnostic DM_NVN_T2T_TRACE=WxH:file: after each copy into an RGBA16F texture of that size, copy the result once more into a trace buffer on the GPU timeline (frame numbers kept on the CPU side); after 240 copies write the file
    { static int on = -1; static uint32_t tw, th; static const char *tf; static id<MTLBuffer> tb; static int tn; static uint64_t tfr[240], from;
      if (on < 0) { const char *e = getenv("DM_NVN_T2T_TRACE"), *fe = getenv("DM_NVN_T2T_FROM"); on = e && sscanf(e, "%ux%u", &tw, &th) == 2 && (tf = strchr(e, ':')) && *++tf; from = fe ? strtoull(fe, NULL, 10) : 0; }
      if (on > 0 && tn < 240 && atomic_load(&g_cur_frame) >= from && d.width == tw && d.height == th && d.pixelFormat == MTLPixelFormatRGBA16Float && !s3) {
          if (!tb) { tb = [g_dev newBufferWithLength:(size_t)240 * tw * th * 8 options:MTLResourceStorageModeShared]; res_add(tb); }
          enc_b_copy_texture_buffer(blit(), d, dz0, dl, MTLOriginMake(0, 0, 0), MTLSizeMake(tw, th, 1), tb, (size_t)tn * tw * th * 8, (size_t)tw * 8, (size_t)tw * th * 8);
          tfr[tn++] = atomic_load(&g_cur_frame);
          if (tn == 240) {
              rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
              mtl_finish_gpu();
              FILE *f = fopen(tf, "wb");
              if (f) { fprintf(f, "%u %u %d\n", tw, th, tn); fwrite(tfr, 8, 240, f); fwrite(tb.contents, 1, (size_t)240 * tw * th * 8, f); fclose(f); }
              MLOG("[t2t-trace] %d 次 → %s（幀 %llu–%llu）", tn, tf, (unsigned long long)tfr[0], (unsigned long long)tfr[239]);
          }
      }
    }
}

// GPU：用給定值清貼圖區域（原先誤列為空操作，初始化調 5 次；Metal 私有貼圖初值未定義，遊戲依賴清出的初值）。
// 整級整面、四通道全寫 → 每個層 / 深度切片開一個只清不畫的通道（硬件按貼圖格式換算清除值，含整數、深度）；
// 視圖換格式：在該格式的視圖上清；部分區域、部分通道、壓縮 / 預排布貼圖（無渲染目標用途）：報紅待實現
// GPU: clear a texture region with the given value (previously mislisted as a no-op, called 5 times at initialization; the initial value of a Metal private texture is undefined, and the game relies on the cleared initial value).
// Whole level and whole face, all four channels written -> open one clear-only render pass per layer / depth slice (the hardware converts the clear value by the texture format, including integer and depth);
// view with changed format: clear on a view of that format; partial region, partial channels, compressed / packaged textures (no render target usage): report a failure, to be implemented
static void clear_tex(const p_clear_tex *c) {
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    PD("X ser=%llu 視圖格式 %#x 區域 %d,%d,%d %dx%dx%d 掩碼 %#x %s %g,%g,%g,%g", c->tex->serial, c->has_view && c->view.set_format ? c->view.format : -1, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, c->mask,
       c->is_uint ? "ui" : "f", c->is_uint ? (double)c->c.u[0] : c->c.f[0], c->is_uint ? (double)c->c.u[1] : c->c.f[1], c->is_uint ? (double)c->c.u[2] : c->c.f[2], c->is_uint ? (double)c->c.u[3] : c->c.f[3]);
    if (!t) return;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
    int is3d = t.textureType == MTLTextureType3D, nz = c->r.depth ? c->r.depth : 1;
    if (c->has_view && c->view.set_format) {   // 視圖換格式：清除值按視圖格式解釋（遊戲 5 次全是 ui 版配整數視圖），在同格式視圖上清 | View with changed format: the clear value is interpreted in the view format (all 5 of the game's calls are the ui version with an integer view), and the clear is done on a view of the same format
        uint32_t vpf = 0; uint8_t swz[4];
        if (!nvn_fmt_to_mtl(c->view.format, &vpf, swz) || !vpf) { RED_ONCE(42, "清貼圖視圖格式 %#x 無對應", c->view.format); return; }
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf) && !(t.usage & MTLTextureUsagePixelFormatView)) t = tex_upgrade_pfv(c->tex, t, (MTLPixelFormat)vpf);
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf)) {
            id<MTLTexture> v = [t newTextureViewWithPixelFormat:(MTLPixelFormat)vpf];
            if (!v) { RED_ONCE(43, "清貼圖建不出視圖：格式 %lu → %u", (unsigned long)t.pixelFormat, vpf); return; }
            t = v;
        }
    }
    MTLPixelFormat pf = t.pixelFormat;
    int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8, stencil = pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth32Float_Stencil8;
    if (c->r.xoffset || c->r.yoffset || (NSUInteger)c->r.width != w || (NSUInteger)c->r.height != h || (c->mask & 0xf) != 0xf
        || !(t.usage & MTLTextureUsageRenderTarget)) {
        RED_ONCE(44, "清貼圖未實現的形態：區域 %d,%d %dx%d（本級 %lux%lu）掩碼 %#x 可作渲染目標 %d", c->r.xoffset, c->r.yoffset, c->r.width, c->r.height,
                 (unsigned long)w, (unsigned long)h, c->mask, !!(t.usage & MTLTextureUsageRenderTarget));
        return;
    }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : stencil ? rp.stencilAttachment : rp.colorAttachments[0];
        a.texture = t; a.level = lv; a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore;
        if (is3d) a.depthPlane = c->r.zoffset + z; else a.slice = l0 + c->r.zoffset + z;
        if (depth) { rp.depthAttachment.clearDepth = c->c.f[0];
            if (stencil) { rp.stencilAttachment.texture = t; rp.stencilAttachment.level = lv; rp.stencilAttachment.slice = a.slice; rp.stencilAttachment.loadAction = MTLLoadActionClear;
                           rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = c->c.u[1]; } }
        else if (stencil) rp.stencilAttachment.clearStencil = c->c.u[0];
        else rp.colorAttachments[0].clearColor = c->is_uint ? MTLClearColorMake(c->c.u[0], c->c.u[1], c->c.u[2], c->c.u[3]) : MTLClearColorMake(c->c.f[0], c->c.f[1], c->c.f[2], c->c.f[3]);
        ser_gate(); enc_r_end(enc_r_begin(rp));
    }
}

// 一次提交的現場描述（mtl_sub_commit 每次提交取一份，命令緩衝出錯時打進紅行）：進崔凡克首幀的 GPU 內存故障，紅行裡只有序號與錯誤碼，看不出那次提交裡有什麼
// Context description of one submit (mtl_sub_commit takes one per submit and prints it into the failure line when the command buffer errors): for the GPU memory fault on the first frame entering Travincal, the failure line had only the sequence number and error code, and it was impossible to tell what was in that submit
static const char *cctx_snap(void) {
    static char b[320]; static uint64_t p_pass, p_up, p_reup, p_ser; uint64_t up = 0, reup = 0; mtl_pkg_stats(&up, &reup);
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    snprintf(b, sizeof b, "幀 %llu；自上次提交：通道 +%llu、貼圖首傳 +%llu、重傳 +%llu、序號表變動 +%llu；延遲釋放隊列 %lu 個；世界凍結狀態 %d",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)(g_stat_pass - p_pass), (unsigned long long)(up - p_up), (unsigned long long)(reup - p_reup),
             (unsigned long long)(g_ser_gen - p_ser), nd, g_frz_state);
    p_pass = g_stat_pass; p_up = up; p_reup = reup; p_ser = g_ser_gen; return b;
}
// ---------- 世界凍結時不畫世界（面板開著時整個遊戲可以凍結） ----------
// 「世界已凍結」狀態位 g_frz_on 由 mtl_world_frozen() 置（宿主的凍結握手調它；測試時可用 DM_NVN_TEST_FRZ=1 頂替，也可經 DM_NVN_UB 的 @set:g_frz_on 同進程切）。
// 這裡不自己判面板開沒開。一幀的結構（3.3.94012 局內，逐通道轉儲）：界面趟 →［界面模板趟］→ 陰影 / 深度 / 光照 / 透明 / 後處理約 40 趟 → 末尾合成趟（寫窗口貼圖）。
// 置位後的第一幀 = 完整幀：照常全畫，只丟界面模板趟的繪製（模板全 0 = 沒有被面板擋住的像素）——後處理平時不寫被面板擋住的像素，面板縮小會露出舊內容；
// 第二幀起 = 凍結幀：從第一個不帶顏色附件的通道起、到寫窗口貼圖的通道之前，設目標 / 清除 / 繪製 / 貼圖間拷貝 / 計算全不執行，場景結果留著完整幀那一份，界面趟與末尾合成趟照畫。
// 幀的界線按通道認：寫過窗口貼圖之後第一條不帶窗口貼圖的設目標 = 新一幀開始，狀態位在這裡鎖存。認不出結構（一幀裡沒有不帶顏色的通道）= 什麼都不跳。
// 狀態常量 / 緩衝更新 / 事件 / 計數器 / 貼圖上傳照常執行（界面趟要用，遊戲在等）。
// ---------- Do not draw the world while the world is frozen (the whole game can be frozen while a panel is open) ----------
// The "world frozen" status bit g_frz_on is set by mtl_world_frozen() (the host's freeze handshake calls it; in tests DM_NVN_TEST_FRZ=1 can stand in, or it can be switched in-process via @set:g_frz_on of DM_NVN_UB).
// This code does not judge by itself whether a panel is open. Structure of a frame (3.3.94012 in-game, per-pass dump): UI pass -> [UI stencil pass] -> about 40 passes of shadows / depth / lighting / transparency / post-processing -> final compositing pass (writes the window texture).
// The first frame after the bit is set = full frame: draw everything as usual, dropping only the draws of the UI stencil pass (stencil all 0 = no pixels blocked by the panel) -- post-processing normally does not write pixels blocked by the panel, so a shrinking panel would expose old content;
// from the second frame on = frozen frame: from the first render pass without a color attachment up to before the render pass that writes the window texture, set-target / clear / draw / texture-to-texture copy / compute are all not executed, the scene result stays as the full frame's copy, and the UI pass and the final compositing pass are drawn as usual.
// Frame boundaries are identified by render pass: the first set-target without the window texture after the window texture has been written = start of a new frame, and the status bit is latched here. Unrecognized structure (no render pass without color in a frame) = skip nothing.
// State constants / buffer updates / events / counters / texture uploads execute as usual (the UI pass needs them, the game is waiting).
int g_frz_on = -1;
static int g_frz_phase = 2, g_frz_drop, g_frz_seen_ds; static uint64_t g_frz_full_n, g_frz_skip_n;
void mtl_world_frozen(int on) { g_frz_on = on != 0; }
static int frz_wanted(void) { if (g_frz_on < 0) { const char *e = getenv("DM_NVN_TEST_FRZ"); g_frz_on = e && atoi(e); if (g_frz_on) MLOG("DM_NVN_TEST_FRZ：世界凍結狀態位由測試開關頂替（常開）"); } tbdr_on(); return g_frz_on > 0 && tbdr_on() && g_ruler_truth <= 0; }
// ---------- 運行時外推（省電畫法；不給玩家開關；線上出過卡頓缺陷，現缺省關，A17 上有不卡的證據再開）----------
// 3D 隔幀：全畫幀照常畫，並在附件被挪作他用之前拷下不透明 HDR（468）、主深度模板、色調映射後的 468、FXAA 後的 469；
// 外推幀：界面模板趟照跑；陰影圖 / 深度預渲染 / 主通道只留會動單位（蒙皮）的繪製；深度預渲染第一個蒙皮繪製前跑一趟外推（nvn_rtex.inc：按 cb3 +464 → +336
// 把上一幀 HDR 與深度搬到本幀，舊單位位置 0x10 / 界面挖空 0x2 當洞補，洞與屏邊新露出的像素導出模板 0x80）；半透明照畫並寫特效位 0x40；
// 合成 / 加泛光 / 加霧 / 色調映射 / FXAA 只在 0xD0 區域裡真跑，區域外是上一幀 LDR 結果按整幅平移（正交相機，取整）拷進來；泛光鏈等全屏項跳過；銳化放大與合界面照跑。
// 退回原畫法：版本文件 render_safe = true（dm_update.m）、外推管線建不出（整局）、測試 DM_RT_AB=0。面板開著世界凍結時讓給凍結路徑。
// 鏡頭跳變（朝向變或平移超 64 像素）與換 3D 渲染比例前後 4 幀整幀全畫。地面上的單位影子沿用上一全畫幀。
// ---------- Runtime extrapolation (power-saving rendering method; no switch for the player; it caused a stutter defect in released builds, now off by default, to be re-enabled once there is evidence of no stutter on A17) ----------
// 3D every other frame: a fully drawn frame draws as usual and, before the attachments are repurposed, copies the opaque HDR (468), the main depth-stencil, the post-tone-mapping 468 and the post-FXAA 469;
// extrapolated frame: the UI stencil pass runs as usual; shadow map / depth prepass / main pass keep only the draws of moving units (skinned); before the first skinned draw of the depth prepass run one extrapolation pass (nvn_rtex.inc: by cb3 +464 -> +336
// move the previous frame's HDR and depth to this frame, treat old unit positions 0x10 / UI cutout 0x2 as holes to fill, and export stencil 0x80 for holes and pixels newly exposed at the screen edge); translucents are drawn as usual and write effect bit 0x40;
// compositing / add bloom / add fog / tone mapping / FXAA really run only in the 0xD0 region, and outside the region the previous frame's LDR result is copied in with a whole-image shift (orthographic camera, rounded); full-screen items such as the bloom chain are skipped; sharpening upscale and UI compositing run as usual.
// Fallback to the original rendering method: version file render_safe = true (dm_update.m), the extrapolation pipeline cannot be built (whole run), test DM_RT_AB=0. When a panel is open and the world is frozen, yield to the freeze path.
// On a camera jump (orientation change or translation over 64 pixels) and for 4 frames before and after switching the 3D render scale, whole frames are fully drawn. Unit shadows on the ground keep using the previous fully drawn frame.
static int rt_is_skin(void) {   // 會動單位 = 頂點著色器按骨骼下標讀常量的那 10 個（鍵表取自 Mac 著色器緩存 b-424a072084e6，按源碼裡 vp_c5 動態下標認） | Moving units = the 10 whose vertex shader reads constants by bone index (the key table is taken from the Mac shader cache b-424a072084e6, identified by the dynamic vp_c5 index in the source)
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr || !pr->sh[0]) return 0;
    static const char *k[] = { "0fa4eb7559f1637a", "1d97d33ab66abcba", "3deaf68fd65b5298", "4dffa355f2fbcdaf", "54c93fa22abff9ee",
                               "64dd7d022f3fae39", "67e2ebc28c830f44", "add86ee8e20f89b9", "b3ae9be5cc670f35", "fbf6dc1873a449e9" };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strncmp(pr->sh[0]->key, k[i], 16)) return 1;
    return 0;
}
int g_dm_rt_ab = 0;   // 缺省原畫法（新畫法在 A17 iPhone 15 Pro 上局內卡、選角頁人物重影，先關）；DM_RT_AB=1 測試時開新畫法。版本文件 render_safe 只會置 0，不影響 | Default is the original rendering method (the new method stutters in-game on A17 iPhone 15 Pro and ghosts characters on the character select page, so off for now); DM_RT_AB=1 turns on the new method for testing. The version file render_safe can only set it to 0, no effect
static struct { int env, frame, filt, first, hdrp, mainp_seen, skipall, oit_seen, tm_seen, fx_state, comp_seen; uint64_t ldr_ser; float vpc[16], vpp[16]; id<MTLTexture> ldr, pl468, pl469; int ui_seen, main_seen, copied, ex_done, shadow, have_prev, have_ldr, sdx, sdy, cam_checked, force_full, disabled, was_on; uint64_t n_jump; uint64_t ds_ser, hdr_ser, n_ex, n_full, n_noex;
    id<MTLTexture> ds, hdr, pds, phdr; struct { uint64_t key; int skin, other; } sh[16]; int nsh, cur_sh; } g_rt = { .env = -1 };
static int rt_want(void);
static void rt_toggle_poll(void) { static int tg = -1; if (tg < 0) { const char *e = getenv("DM_RT_AB_TOGGLE"); tg = e ? atoi(e) : 0; (void)rt_want(); }   // 首次順帶讀 DM_RT_AB（真機上世界凍結位早早置 0，frz_op 不一定被調到）   // 測試：每 tg 秒按牆鐘在 A / B 之間來回切 | On first call also read DM_RT_AB (on a real device the world-freeze bit is set to 0 very early, so frz_op is not necessarily called)   // test: switch back and forth between A / B every tg seconds by wall clock
    if (tg > 0) { int v = (int)((time(NULL) / tg) & 1); if (v != g_dm_rt_ab) { g_dm_rt_ab = v; MLOG("DM_RT_AB_TOGGLE：畫法切到 %s", v ? "B" : "A"); } } }
void mtl_rt_force_full(int n) { g_rt.force_full = n; }   // 宿主調：要換 3D 渲染比例等畫面整體變化前，之後 n 幀一律全畫 | Called by the host: before an overall picture change such as switching the 3D render scale, the following n frames are all fully drawn
// ---------- 省電模式（30 幀）----------
// 開：顯示節拍對象改 30 Hz（幀率範圍 30–30，ProMotion 屏跟著降刷新率；front_ios.m 的幀率提示一起降），遊戲每拍出一幀；運行時外推關（每幀真畫）。
// 遊戲邏輯本來就按 25 拍 / 秒走、單位位置按時間插值，30 幀下沒有重復幀（真機實測：每幀鏡頭位移的變異系數 30 幀 0.292、60 幀 0.296）。
// 開關在「自定義」頁（dm_custom.c eco_fps，缺省關，應用即生效）；測試開關 DM_FPS=30/60 釘住。
// ---------- Power-saving mode (30 frames) ----------
// On: the display link object changes to 30 Hz (frame rate range 30–30, a ProMotion screen lowers its refresh rate accordingly; the frame rate hint in front_ios.m is lowered together), the game presents one frame per display-link tick; runtime extrapolation off (really draws every frame).
// Game logic already runs at 25 ticks / second and unit positions are interpolated by time, so there are no repeated frames at 30 frames (measured on a real device: coefficient of variation of per-frame camera displacement 0.292 at 30 frames, 0.296 at 60 frames).
// The switch is on the 「自定義」(Custom) page (dm_custom.c eco_fps, off by default, takes effect on apply); the test switch DM_FPS=30/60 pins it.
__attribute__((weak)) int dm_fps_setting(void) { return 60; }   // 宿主 dm_custom.c 提供強定義；圖形單測不鏈宿主時恆 60 | The host's dm_custom.c provides the strong definition; always 60 when graphics unit tests do not link the host
// 遊戲自己要 30 幀（按遊戲出圖間隔、間隔 2，hle_nvn.c）時同樣走 30 Hz 顯示節拍：Engage 這類邏輯按幀計數的遊戲在 iOS 上不被顯示節拍帶到 60 幀
// When the game itself asks for 30 fps (pacing by the game's present interval, interval 2, hle_nvn.c) the display link runs at 30 Hz too: games whose logic counts frames, such as Engage, are not pulled up to 60 fps by the display link on iOS
__attribute__((weak)) int nvn_game_fps(void) { return 60; }   // hle_nvn.c 提供強定義 | hle_nvn.c provides the strong definition
int mtl_fps_target(void) { static int env = -2; if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; else MLOG("DM_FPS=%d：幀數由測試開關釘住", env); }
    return env > 0 ? env : dm_fps_setting() == 30 || nvn_game_fps() == 30 ? 30 : 60; }
// 外推啓用狀態一變（省電模式切換、世界凍結接手 / 交回）：先讓「跨幀不寫回」賬本重學（試用期裡照常寫回），外推晚一幀再真正切換——
// 保證切換前最後一次提交是全量寫回的。不然 30 幀時每幀先清空的附件被學成不寫回，切回 60 幀的第一個外推幀讀到舊內容、閃一幀（測試版實測，xv 自檢報紅後才撤銷）
// Whenever the extrapolation enabled state changes (power-saving mode toggled, world freeze takes over / hands back): first let the "no store across frames" ledger relearn (during the trial period it stores as usual), and the extrapolation really switches one frame later --
// guaranteeing the last submit before the switch is a full store. Otherwise attachments cleared first every frame at 30 frames are learned as no-store, and the first extrapolated frame after switching back to 60 frames reads old content and flashes for one frame (measured on a test build; only reverted after the xv self-check reported a failure)
static void xv_relearn(const char *why) { g_xv_n = 0; g_xv_ntrial = 0; g_xv_ntouch = 0; MLOG("跨幀不寫回：%s，賬本清空重學（試用期內照常寫回）", why); }
static int rt_want_raw(void);
static int rt_want(void) {
    static int eff = -1; static uint64_t pend; int want = rt_want_raw(); uint64_t f = atomic_load(&g_cur_frame);
    if (eff < 0) eff = want;
    if (want == eff) { pend = 0; return eff; }
    if (!pend) { pend = f + 1; xv_relearn(want ? "外推將啓用" : "外推將停用"); return eff; }
    if (f > pend) { eff = want; pend = 0; }
    return eff;
}
static int rt_want_raw(void) { static int forced; if (g_rt.env < 0) { const char *e = getenv("DM_RT_AB"); g_rt.env = e ? atoi(e) : 0; forced = e != NULL; if (e) MLOG("DM_RT_AB=%d：畫法由環境變量釘住（測試用，自定義頁的開關不再起作用）", g_rt.env); }
    if (forced) g_dm_rt_ab = g_rt.env;
    return g_dm_rt_ab == 1 && !g_rt.disabled && g_frz_on <= 0 && tbdr_on() && g_ruler_truth <= 0 && mtl_fps_target() == 60; }   // 面板開著世界凍結時讓給凍結那條（更省）；省電模式（30 幀）每幀真畫 | When a panel is open and the world is frozen, yield to the freeze path (saves more); power-saving mode (30 frames) really draws every frame
#include "nvn_rtex.inc"
static uint64_t rt_key(const p_set_rt *r) { uint64_t s_; int lv, sl; rt_sub(r, 8, &s_, &lv, &sl); return s_ * 64 + (uint64_t)lv * 8 + (uint64_t)sl; }
static int rt_is_shadow(const p_set_rt *r) { return !r->n && r->depth && r->depth->d.width == r->depth->d.height; }
static id<MTLTexture> rt_cp(id<MTLTexture> src, id<MTLTexture> dst0) {
    id<MTLTexture> dst = dst0;
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width height:src.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
        if (dst0) defer_obj_release(dst0);   // 換尺寸：舊副本可能還被在途的命令緩衝讀著（MTL4 命令緩衝不保留資源），延遲放 | Size change: the old copy may still be read by an in-flight command buffer (MTL4 command buffers do not retain resources), so release it deferred
        dst = [g_dev newTextureWithDescriptor:d]; if (!dst) return nil; res_add(dst); }
    id<MTL4ComputeCommandEncoder> b = blit(); enc_b_copy_texture(b, src, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(src.width, src.height, 1), dst, 0, 0, MTLOriginMake(0, 0, 0)); return dst; }
static void rt_blit_shift(id<MTLTexture> src, id<MTLTexture> dst, int dx, int dy) {   // 整數偏移拷貝：dst(x + dx, y + dy) = src(x, y)；搬不到的屏邊一條留給區域重算（外推趟已標 0x80） | Integer-offset copy: dst(x + dx, y + dy) = src(x, y); the strip at the screen edge that cannot be moved is left for the region recompute (the extrapolation pass has already marked it 0x80)
    if (!src || !dst || src.width != dst.width || src.height != dst.height) return;
    int W = (int)src.width, H = (int)src.height, w = W - abs(dx), h = H - abs(dy); if (w <= 0 || h <= 0) return;
    id<MTL4ComputeCommandEncoder> b = blit(); enc_b_copy_texture(b, src, 0, 0, MTLOriginMake(MAX(0, -dx), MAX(0, -dy), 0), MTLSizeMake(w, h, 1), dst, 0, 0, MTLOriginMake(MAX(0, dx), MAX(0, dy), 0)); }
static void rt_copy_prev(void) {   // 全畫幀：不透明 HDR 與主深度模板各拷一份（GPU 內按序，在本命令緩衝裡） | Fully drawn frame: copy the opaque HDR and the main depth-stencil once each (in order within the GPU, in this command buffer)
    if (g_rt.copied || !g_rt.main_seen || !g_rt.hdr || !g_rt.ds) return; g_rt.copied = 1;   // 本幀沒認出主通道（前台 / 讀取畫面）不拷：上次認的貼圖可能已被遊戲放掉、不在駐留集裡 | Do not copy when the main pass was not recognized this frame (foreground / loading screen): the textures recognized last time may already have been released by the game and not be in the residency set
    if (!rx_setup(g_rt.hdr.pixelFormat, g_rt.ds.pixelFormat)) { if (!g_rt.disabled) { g_rt.disabled = 1; MLOG("【紅】運行時外推：外推管線在這台設備上建不出，整局退回原畫法"); } return; }   // 建不出（老 GPU 不支持模板導出等）：整局退回 | Cannot be built (old GPU does not support stencil export etc.): fall back for the whole run
    g_rt.phdr = rt_cp(g_rt.hdr, g_rt.phdr); g_rt.pds = rt_cp(g_rt.ds, g_rt.pds); g_rt.have_prev = g_rt.phdr && g_rt.pds; }
static void rt_frame_begin(void) { g_rt.filt = g_rt.ui_seen = g_rt.main_seen = g_rt.copied = g_rt.ex_done = g_rt.mainp_seen = 0; g_rt.cur_sh = -1;
    g_rt.oit_seen = g_rt.tm_seen = g_rt.fx_state = g_rt.comp_seen = g_rt.skipall = g_rt.cam_checked = 0; g_rt_ovr = 0; g_dss_dirty = 1;
    if (g_frz_state == 0) { g_rt.have_prev = g_rt.have_ldr = 0; for (int i = 0; i < g_rt.nsh; i++) g_rt.sh[i].skin = g_rt.sh[i].other = 0; } }   // 全畫幀：本幀拷成了才許下一幀外推 | Fully drawn frame: the next frame may extrapolate only if this frame's copies succeeded
static void rt_set_rt(const p_set_rt *r, int win) {   // 每條設目標（全畫幀與外推幀都走）：認角色、全畫幀找拷貝點、外推幀定本通道是否只留蒙皮 | Every set-target (both fully drawn and extrapolated frames go through here): identify roles, find the copy points in fully drawn frames, and in extrapolated frames decide whether this render pass keeps only skinned draws
    g_rt.cur_sh = -1; g_rt.shadow = 0; g_rt.first = 1; g_rt.hdrp = 0;
    if (win) { if (g_frz_state == 0) { rt_copy_prev(); if (g_rt.fx_state == 2 && g_rt.ldr) { g_rt.pl469 = rt_cp(g_rt.ldr, g_rt.pl469); g_rt.have_ldr = g_rt.pl469 != nil; g_rt.fx_state = 3; } }
        g_rt.filt = 0; g_rt.skipall = 0; if (g_rt_ovr) { g_rt_ovr = 0; g_dss_dirty = 1; } return; }
    id<MTLTexture> c0 = r->n > 0 && r->color[0] ? tex_mtl(r->color[0]) : nil, dt = r->depth ? tex_mtl(r->depth) : nil;
    int hdr_here = 0; for (int i = 0; i < r->n && i < 8; i++) if (r->color[i] && g_rt.hdr_ser && r->color[i]->serial == g_rt.hdr_ser) hdr_here = 1;
    if (g_frz_state == 0) {
        if (!g_rt.main_seen && r->n == 1 && c0 && c0.pixelFormat == MTLPixelFormatRG11B10Float && dt && dt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && dt.width >= 500 && dt.width != dt.height) {
            { static uint64_t lh, ld; if (lh != r->color[0]->serial || ld != r->depth->serial) { lh = r->color[0]->serial; ld = r->depth->serial; MLOG("運行時外推：認出主通道 不透明 HDR 序號 %llu %lux%lu、主深度模板序號 %llu %lux%lu（通道 %llu）", (unsigned long long)lh, (unsigned long)c0.width, (unsigned long)c0.height, (unsigned long long)ld, (unsigned long)dt.width, (unsigned long)dt.height, g_stat_pass + 1); } }
            g_rt.main_seen = 1; g_rt.hdr_ser = r->color[0]->serial; g_rt.ds_ser = r->depth->serial; g_rt.hdr = c0; g_rt.ds = dt; }
        else if (g_rt.main_seen && hdr_here && !g_rt.copied) rt_copy_prev();   // 主通道之後再掛不透明 HDR 的第一個設目標（半透明那趟，468 佔 0 號槽但不寫）：之前拷 HDR 與主深度模板 | The first set-target after the main pass that attaches the opaque HDR again (the translucent pass; 468 occupies slot 0 but is not written): before it, copy the HDR and the main depth-stencil
        else if (g_rt.main_seen && hdr_here && r->n == 1 && r->depth && r->depth->serial == g_rt.ds_ser) g_rt.fx_state = 1;   // 只掛 468 + 主深度 = 色調映射（把 468 挪作 LDR 輸出） | Only 468 + main depth attached = tone mapping (repurposes 468 as the LDR output)
        else if (g_rt.fx_state == 1) {   // 色調映射之後第一個通道 = FXAA（讀 468、寫 469）：之前拷色調映射後的 468（LDR），記下 469 | The first render pass after tone mapping = FXAA (reads 468, writes 469): before it, copy the post-tone-mapping 468 (LDR) and note down 469
            if (r->n >= 1 && r->color[0] && c0) { g_rt.ldr_ser = r->color[0]->serial; g_rt.ldr = c0; g_rt.pl468 = rt_cp(g_rt.hdr, g_rt.pl468); g_rt.fx_state = 2; } else g_rt.fx_state = 0; }
        else if (g_rt.fx_state == 2 && g_rt.ldr) { g_rt.pl469 = rt_cp(g_rt.ldr, g_rt.pl469); g_rt.have_ldr = g_rt.pl468 && g_rt.pl469; g_rt.fx_state = 3; }   // FXAA 之後第一個通道之前：拷 FXAA 後的 469 | Before the first render pass after FXAA: copy the post-FXAA 469
    }
    if (rt_is_shadow(r)) { uint64_t k = rt_key(r); int i; for (i = 0; i < g_rt.nsh && g_rt.sh[i].key != k; i++) {}
        if (i == g_rt.nsh && i < 16) { g_rt.sh[i].key = k; g_rt.sh[i].skin = g_rt.sh[i].other = 0; g_rt.nsh++; } g_rt.cur_sh = i < 16 ? i : -1; g_rt.shadow = 1; }
    int ds_here = r->depth && g_rt.ds_ser && r->depth->serial == g_rt.ds_ser;
    if (ds_here && !r->n && !g_rt.ui_seen) { g_rt.ui_seen = 1; g_rt.filt = 0; return; }   // 本幀第一個帶主深度、無顏色的通道 = 界面模板趟：照跑 | The first render pass in this frame with the main depth and no color = the UI stencil pass: run as usual
    g_rt.hdrp = hdr_here && ds_here && r->n == 1 && !g_rt.mainp_seen; if (g_rt.hdrp) g_rt.mainp_seen = 1;   // 本幀第一個「只掛不透明 HDR + 主深度」的通道 = 主通道 | The first render pass in this frame with "only opaque HDR + main depth attached" = the main pass
    g_rt.filt = g_rt.shadow || (ds_here && !r->n) || g_rt.hdrp;
    if (g_frz_state == 2 && g_rt.mainp_seen && !g_rt.hdrp) {   // 外推幀、主通道之後：後處理只在單位 / 特效區域跑（乙方案） | Extrapolated frame, after the main pass: post-processing runs only in the unit / effect region (plan B)
        int c0ldr = r->n >= 1 && r->color[0] && g_rt.ldr_ser && r->color[0]->serial == g_rt.ldr_ser, ovr = 0; g_rt.skipall = 0;
        if (g_rt.fx_state == 2) {}   // FXAA 之後（銳化放大 + 暗角、合界面）：全跑 | After FXAA (sharpening upscale + vignette, UI compositing): run everything
        else if (g_rt.fx_state == 1) g_rt.fx_state = 2;
        else if (!g_rt.oit_seen && ds_here && r->n >= 2) { g_rt.oit_seen = 1; ovr = 3; }   // 半透明（加權混合雙目標）：照畫，通過處寫特效位 0x40 | Translucent (weighted blended, two targets): draw as usual, writing effect bit 0x40 where it passes
        else if (!g_rt.comp_seen) { if (ds_here && c0ldr) { g_rt.comp_seen = 1; ovr = 1; rt_blit_shift(g_rt.pl469, g_rt.ldr, g_rt.sdx, g_rt.sdy); } else g_rt.skipall = 1; }   // 合成之前：上一幀 FXAA 後的 469 整數平移拷進來（區域裡隨後重算）   // 合成之前的其餘各趟（mip 鏈、511 折射層等）跳過 | Before compositing: the previous frame's post-FXAA 469 is copied in with an integer shift (then recomputed in the region)   // the other passes before compositing (mip chain, 511 refraction layer etc.) are skipped
        else if (ds_here && c0ldr && g_rt.tm_seen) {   // FXAA 本趟（在區域裡重算；區域外的 469 合成前已平移拷好） | The FXAA pass itself (recomputed in the region; the 469 outside the region was already shift-copied before compositing)
            g_rt.fx_state = 1; ovr = 1; }
        else if (ds_here) { if (hdr_here && r->n == 1) { g_rt.tm_seen = 1; rt_blit_shift(g_rt.pl468, g_rt.hdr, g_rt.sdx, g_rt.sdy); } ovr = 1; }   // 色調映射之前：上一幀色調映射後的 468（LDR）整數平移拷進來   // 加泛光、加霧、色調映射：只在區域裡跑 | Before tone mapping: the previous frame's post-tone-mapping 468 (LDR) is copied in with an integer shift   // add bloom, add fog, tone mapping: run only in the region
        else g_rt.skipall = 1;   // 泛光鏈等不掛主深度的：跳過（區域裡沿用上一幀的泛光） | Those without the main depth attached, such as the bloom chain: skip (the region keeps using the previous frame's bloom)
        if (ovr != g_rt_ovr) { g_rt_ovr = ovr; g_dss_dirty = 1; }
    }
}
static int rt_op(unsigned op) {   // 外推幀裡非設目標的操作：返回 1 = 不執行 | Non-set-target operations in an extrapolated frame: returns 1 = not executed
    if (g_frz_state == 2 && g_rt_ovr == 1 && op == OP_CLEAR_COLOR) return 1;   // 區域後處理各趟不清：區域外是平移拷進來的上一幀結果 | The region post-processing passes do not clear: outside the region is the previous frame's result copied in with a shift
    if (g_rt.skipall && (op == OP_DRAW || op == OP_DRAW_IDX || op == OP_CLEAR_COLOR || op == OP_CLEAR_DS || op == OP_COPY_T2T || op == OP_CLEAR_TEX || op == OP_DISPATCH)) return 1;
    if (op == OP_DRAW || op == OP_DRAW_IDX) {
        if (g_frz_state == 2 && !g_rt.cam_checked) {   // 外推幀的第一個繪製（界面模板趟，陰影 / 預渲染之前）：核本幀相機，跳變就整幀改全畫 | First draw of an extrapolated frame (UI stencil pass, before shadows / prepass): check this frame's camera, and on a jump switch the whole frame to fully drawn
            g_rt.cam_checked = 1; const p_bind_buf *u = &S.ubo[0][3]; void *mb; uint64_t mo; const uint8_t *base; int ok = 0;
            if (u->addr && u->size >= 528 && mtl_addr(u->addr, u->size, &mb, &mo) && (base = [(__bridge id<MTLBuffer>)mb contents])) {
                const float *cur = (const float *)(base + mo + 336), *prev = (const float *)(base + mo + 464); int dx, dy; float md = 0;
                for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) md = MAX(md, fabsf(cur[r * 4 + c] - prev[r * 4 + c]));   // 前三行（朝向 / 縮放）應不變：正交相機只平移 | The first three rows (orientation / scale) should be unchanged: an orthographic camera only translates
                rt_shift_px(prev, cur, g_rt.hdr.width, g_rt.hdr.height, &dx, &dy);
                ok = md < 1e-4f && abs(dx) <= 64 && abs(dy) <= 64; if (!ok) { static int n; if (n++ < 20) MLOG("運行時外推：鏡頭跳變（平移 %d,%d 像素、朝向差 %g），本幀改全畫", dx, dy, md); } }
            if (!ok) { g_frz_state = 0; g_rt.n_jump++; rt_frame_begin(); g_rt.cam_checked = 1; return 0; }
        }
        if (g_frz_state == 0) { if (g_rt.cur_sh >= 0) { if (rt_is_skin()) g_rt.sh[g_rt.cur_sh].skin++; else g_rt.sh[g_rt.cur_sh].other++; } return 0; }
        if (!g_rt.filt) return 0;
        if (!rt_is_skin()) return 1;
        if (!g_rt.ex_done && !g_rt.shadow) {   // 深度預渲染第一個蒙皮繪製之前：外推趟（讀本繪製綁定的 cb3：+336 本幀、+464 上一幀視圖投影） | Before the first skinned draw of the depth prepass: the extrapolation pass (reads the cb3 bound by this draw: +336 this frame's, +464 the previous frame's view-projection)
            g_rt.ex_done = 1; const p_bind_buf *u = &S.ubo[0][3]; void *mb; uint64_t mo; const uint8_t *base;
            if (u->addr && u->size >= 528 && mtl_addr(u->addr, u->size, &mb, &mo) && (base = [(__bridge id<MTLBuffer>)mb contents])) {
                float cur[16], prev[16]; memcpy(cur, base + mo + 336, 64); memcpy(prev, base + mo + 464, 64); memcpy(g_rt.vpc, cur, 64); memcpy(g_rt.vpp, prev, 64); rt_shift_px(prev, cur, g_rt.hdr.width, g_rt.hdr.height, &g_rt.sdx, &g_rt.sdy);
                rp_end(); if (rt_ex_encode(g_rt.hdr, g_rt.ds, g_rt.phdr, g_rt.pds, prev, cur, 0x10, 0x2)) g_rt.n_ex++; else g_rt.n_noex++;
            } else g_rt.n_noex++;
        }
        return 0;
    }
    if (g_frz_state == 2 && g_rt.filt) {
        if (op == OP_CLEAR_COLOR || op == OP_CLEAR_DS) {
            if (g_rt.shadow && g_rt.cur_sh >= 0 && !g_rt.sh[g_rt.cur_sh].other) return 0;   // 上一全畫幀只有蒙皮的陰影級（人物專用級）：照清照畫 | A shadow level that had only skinned draws in the previous fully drawn frame (character-only level): clear and draw as usual
            return 1; }
    }
    return 0;
}
static int frz_op(unsigned op, const void *a) {   // 返回 1 = 這條操作不執行 | Returns 1 = this operation is not executed
    if (op == OP_SET_RT) {
        const p_set_rt *r = a; int win = 0;
        for (int i = 0; i < r->n; i++) { uint64_t s_; int lv, sl; rt_sub(r, i, &s_, &lv, &sl); if (s_ && xv_present(s_)) win = 1; }
        if (g_frz_phase == 2 && !win && rt_want()) {   // 運行時外推：新一幀，全畫幀與外推幀交替 | Runtime extrapolation: new frame, fully drawn frames and extrapolated frames alternate
            int was = g_frz_state; g_frz_phase = 0; g_rt.was_on = 1;
            { static int dbg = -1, n; if (dbg < 0) dbg = getenv("DM_RT_DBG") != NULL; if (dbg && n < 4000 && g_rt.ds_ser) { n++; MLOG("[rtdbg] 幀界 通道 %llu 上一段狀態 %d 認出主通道 %d 拷了 %d 外推 %d fx %d ldr %d hoist %d fz %d defer %d/%d", g_stat_pass, was, g_rt.main_seen, g_rt.copied, g_rt.ex_done, g_rt.fx_state, g_rt.have_ldr, g_hoist_n, g_fz.active, defer_has_active(), g_defer_replaying); } }
            if (g_rt.force_full > 0) g_rt.force_full--;
            g_frz_state = !was && g_rt.have_prev && g_rt.have_ldr && !g_rt.force_full && !g_hoist_n && !g_fz.active && !defer_has_active() && !g_defer_replaying ? 2 : 0;
            if (g_frz_state == 2) g_rt.frame++; else g_rt.n_full++;
            if (g_frz_state != was && (g_rt.frame + g_rt.n_full) % 600 < 2) MLOG("運行時外推：全畫幀 %llu、外推幀 %d（外推趟編了 %llu 次、沒編 %llu 次；鏡頭跳變改全畫 %llu 次）", g_rt.n_full, g_rt.frame, g_rt.n_ex, g_rt.n_noex, g_rt.n_jump);
            rt_frame_begin();
        }
        if (g_frz_phase != 2 && rt_want()) { if (win) g_frz_phase = 2; rt_set_rt(r, win); return g_rt.skipall; }
        if (g_frz_phase == 2 && win && rt_want()) { rt_set_rt(r, win); return 0; }
        { int on = rt_want(); if (g_rt.was_on && !on) {   // 新畫法剛關（版本文件退回 / 面板凍結接手 / 整局退回）：狀態歸零，交回原路徑 | The new rendering method was just turned off (version file fallback / panel freeze takes over / fallback for the whole run): reset state to zero and hand back to the original path
              g_frz_state = 0; g_frz_phase = 2; g_rt.have_prev = g_rt.have_ldr = 0; if (g_rt_ovr) { g_rt_ovr = 0; g_dss_dirty = 1; } g_rt.skipall = 0;
              MLOG("運行時外推：新畫法停用（畫法 %d、世界凍結 %d、整局退回 %d）", g_dm_rt_ab, g_frz_on, g_rt.disabled); }
          g_rt.was_on = on; }
        if (g_frz_phase == 2 && !win) {   // 新一幀 | New frame
            int on = frz_wanted(), was = g_frz_state; g_frz_phase = 0; g_frz_seen_ds = 0;
            { static uint64_t fr, p0, k0; if (was == 2 && (++fr == 4 || fr % 600 == 0)) MLOG("世界凍結：第 %llu 個凍結幀開了 %llu 個通道、跳過 %llu 條設目標", fr, g_stat_pass - p0, g_frz_skip_n - k0); if (was != 2) fr = 0; p0 = g_stat_pass; k0 = g_frz_skip_n; }
            if (!on) g_frz_state = 0;
            else if (was) g_frz_state = 2;
            else if (!g_hoist_n && !g_fz.active && !defer_has_active() && !g_defer_replaying) g_frz_state = 1;   // 上一幀留下的前提 / 合併 / 推遲還沒走完：下一幀再進 | The premises / merges / deferrals left by the previous frame have not finished yet: enter next frame
            if (g_frz_state != was) MLOG("世界凍結：通道 %llu 起 %s（完整幀累計 %llu，已跳過通道 %llu）", g_stat_pass + 1, g_frz_state == 1 ? "完整幀（不帶界面模板）" : g_frz_state == 2 ? "凍結幀（只畫界面與末尾合成）" : "恢復正常", g_frz_full_n += g_frz_state == 1, g_frz_skip_n);
        }
        g_frz_drop = 0;
        if (win) { g_frz_phase = 2; return 0; }
        if (!g_frz_state || g_frz_phase == 2) return 0;
        if (g_frz_phase == 0 && !r->n && r->depth && !g_frz_seen_ds) {   // 本幀第一個不帶顏色的通道 | The first render pass without color in this frame
            g_frz_seen_ds = 1;
            if (g_frz_state == 2) g_frz_phase = 1;
            else { uint64_t s_; int lv, sl; rt_sub(r, 8, &s_, &lv, &sl); static int neg = -1; if (neg < 0) neg = getenv("DM_NVN_TEST_FRZ_NEG") != NULL;   // 負對照：完整幀照帶界面模板畫，面板縮小時必須露出舊像素 | Negative control: the full frame is drawn with the UI stencil as usual, and old pixels must be exposed when the panel shrinks
                   g_frz_drop = !neg && MAX(r->depth->d.width >> lv, 1u) != MAX(r->depth->d.height >> lv, 1u); }   // 完整幀：不是正方形（陰影圖）的那個就是界面模板趟 | Full frame: the one that is not square (shadow map) is the UI stencil pass
        }
        if (g_frz_phase == 1) { g_frz_skip_n++; return 1; }
        return 0;
    }
    if (rt_want() && g_frz_phase != 2) return rt_op(op);
    if (g_frz_drop) return op == OP_DRAW || op == OP_DRAW_IDX;
    if (g_frz_state == 2 && g_frz_phase == 1) return op == OP_CLEAR_COLOR || op == OP_CLEAR_DS || op == OP_DRAW || op == OP_DRAW_IDX || op == OP_COPY_T2T || op == OP_CLEAR_TEX || op == OP_DISPATCH;
    return 0;
}

void mtl_exec(const uint8_t *p) {
    static int inited; if (!inited) { inited = 1; state_reset(); }
    static int dump_all = -1; if (dump_all < 0) dump_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
    int trace = dump_all && passdump();   // 普通模式不逐操作查幀範圍，完整命令位置僅用於指定診斷幀 | Normal mode does not check the frame range per operation; the full command position is only used for the designated diagnostic frames
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        g_exec_cur = p;   // 寫回前瞻的掃描起點：結束通道的那條操作（含） | Scan start point of the store lookahead: the operation that ends the render pass (inclusive)
        if (p == g_defer_pend) { for (int gi = 0; gi < 4; gi++) if (g_defer_slots[gi].active && g_defer_slots[gi].p_end_op == p) g_defer_slots[gi].in_p = 0; g_defer_pend = NULL; }
        sdx_seq(p);
        if (h.op == OP_SET_RT) rt_toggle_poll();
        if ((g_frz_on || g_frz_state || g_dm_rt_ab == 1) && frz_op(h.op, p + 8)) { p += h.size; continue; }
        if (trace) PD("o cmd=%p pass=%llu op=%u size=%u", p, g_stat_pass, (unsigned)h.op, (unsigned)h.size);
        // 診斷 DM_NVN_ALLOCPROF=1：顯卡已分配量的增長記到上一條操作的種類上（粗略：其他線程的分配也會算進來），內存讀數行附帶前 4 名
        // Diagnostic DM_NVN_ALLOCPROF=1: growth of the GPU allocated size is charged to the previous operation's type (rough: allocations on other threads are counted too); the memory report line appends the top 4
        if (g_allocprof < 0) g_allocprof = getenv("DM_NVN_ALLOCPROF") != NULL;
        if (g_allocprof) { static uint64_t last; static int prev = -1; uint64_t now = g_dev.currentAllocatedSize; if (prev >= 0 && prev < 64 && now > last) g_ap_bytes[prev] += now - last; last = now; prev = h.op; }
        if (g_hoist_n && hoist_take(p)) { if (g_fz.active && p == g_fz.rt2_op) { g_fz.phase = 2; g_rt_merged = 0; PD("F %llu 合併通道進入後一個通道的繪製", g_stat_pass); } p += h.size; continue; }   // 已提前執行的純清除通道 / 已併入的後一個通道開頭   // 進入後一個通道：前一個通道的同目標續用到此為止，後一個通道讀前一個通道附件由 fz_p2_draw 證本像素，不再按續用規則切開 | A pure-clear render pass already executed early / the start of a later pass already merged in   // Entering the later pass: the same-target continuation of the earlier pass ends here; the later pass reading the earlier pass's attachment is proven to be same-pixel by fz_p2_draw, and is no longer split by the continuation rule
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: g_exec_cur = NULL;
                if (g_hoist_n && !g_ret_n && !g_nrest) {
                    if (g_fz.active) fz_unskip();
                    if (g_hoist_n) { MLOG("【紅】純清除通道前提：提交結束時還有 %d 條已前提的操作沒走到，清空（不應發生）", g_hoist_n); g_hoist_n = 0; }
                }
                return;   // 列表外（出圖 / CPU 讀回等）觸發的結束通道不得掃已過期的命令內存：起點為空即全部寫回 | An end-of-pass triggered outside a list (present / CPU readback etc.) must not scan expired command memory: a null start point means store everything
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (g_ret_n < 16) g_ret[g_ret_n] = p + h.size; g_ret_n++;   // 前瞻用的返回棧：子列表結束後接著看調用方 | Return stack for lookahead: after a sub-list ends, continue looking at the caller
                mtl_exec((const uint8_t *)t); g_ret_n--; break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                PD("Y signal_event cmd=%p dst=%p mode=%u value=%u", p, val, m, v); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); g_cnt_report[t & 31]++; cr_note(ad, (uint32_t)t);
                PD("Q report type=%llu cmd=%p dst=%#llx", (unsigned long long)t, p, (unsigned long long)ad);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { post((post_op){3, NULL, 0, 0, ad, (uint32_t)t, vis_snapshot()}); g_vis_cur = -1; }
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); g_cnt_reset[t & 31]++; PD("Q reset type=%llu cmd=%p", (unsigned long long)t, p);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
#ifdef NVN_RT_RANGE_TEST
                    rr_reset();
#endif
                    g_vis_on = 1; g_vis_counting = 1; vis_clear(); }
                break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
                PD("Y copy_b2b cmd=%p src=%#llx dst=%#llx bytes=%llu", p, (unsigned long long)s_, (unsigned long long)d, (unsigned long long)n); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                PD("Y update_ub cmd=%p src=%#llx dst=%#llx bytes=%llu f=%g,%g,%g,%g", p, (unsigned long long)((const uint8_t *)a + 16), (unsigned long long)d, (unsigned long long)n,
                   n >= 4 ? ((const float *)((const uint8_t *)a + 16))[0] : 0.f, n >= 8 ? ((const float *)((const uint8_t *)a + 16))[1] : 0.f, n >= 12 ? ((const float *)((const uint8_t *)a + 16))[2] : 0.f, n >= 16 ? ((const float *)((const uint8_t *)a + 16))[3] : 0.f); mtl_sub_update((const uint8_t *)a + 16, d, n); break; }
            case OP_SET_RT: {
                const p_set_rt *r = a;
                if (defer_begin_q(p)) break;
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (g_enc && !pend && R.has_rt && tbdr_on() && rt_range_same(&R.rt,r)) {
                    g_rt_merged = g_rt_range_active = g_rt_range_pending = 1; R.rt = *r; break; }
                if (g_enc && g_fz.active && g_fz.phase == 1 && fz_reuse_op(p) && !pend && g_enc_mode != 1 && R.has_rt && tbdr_on() && rt_same(&g_fz.rt1, r)) {
                    g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("同目標續用：首例 通道 %llu", g_stat_pass); PD("M %llu 同目標續用通道", g_stat_pass); break; }
                // 不續用只為清除開的通道；少掛深度 / 模板的可續用（繪製前核平面）
                // Do not continue a render pass opened only for a clear; one with fewer depth / stencil attachments can be continued (check the planes before drawing)
                if (g_enc && g_enc_mode != 1 && !pend && R.has_rt && tbdr_on() && rt_same(&R.rt, r)) { g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("同目標續用：首例 通道 %llu", g_stat_pass); PD("M %llu 同目標續用通道", g_stat_pass); R.rt = *r; break; }
                rp_flush(); R.rt = *r; R.has_rt = 1; break;
            }
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = a;
                if (!g_render_on) { PD("X %llu 清色 %d：條件繪製關著，跳過", g_stat_pass, c->index); break; }
                // 通道外、全通道、清成 0 的剪裁清除（SMBW 在 480×272 等目標上按子矩形清金字塔各級）：在回放流裡從全零緩衝拷進剪裁矩形
                // A scissored clear to 0 outside a pass with all channels (SMBW clears pyramid levels as sub-rectangles of 480×272 and other targets): copy from the zero buffer into the scissor rectangle in the replay stream
                if (!g_enc && c->mask == 0xf && !c->rgba[0] && !c->rgba[1] && !c->rgba[2] && !c->rgba[3] && R.has_sc && c->index >= 0 && c->index < R.rt.n && R.rt.color[c->index]
                    && !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index]) && clear_rect_zero(c->index)) break;
                if (g_rt_merged) rp_end();   // 續用的通道裡來了清除：在此結束，清除照舊掛到下一個通道開頭 | A clear arrived in a continued render pass: end it here, and the clear is attached to the start of the next render pass as usual
                if (c->index < 0 || c->index >= R.rt.n || !R.rt.color[c->index]) { RED_ONCE(5, "清色目標 %d 無效", c->index); break; }
                if (g_enc || c->mask != 0xf || !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index])) {
                    RED_ONCE(6, "通道中途 / 部分掩碼 / 部分剪裁的清色未實現（首例 掩碼 %#x）", c->mask);
                    static uint64_t n; if (getenv("DM_NVN_CLEARLOG") && n++ < 40) MLOG("清色跳過 幀%llu 通道%llu 目標%d ser=%llu %ux%u 中途=%d 掩碼=%#x 剪裁=%d,%d,%d,%d 色=%g,%g,%g,%g", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, c->index,
                        (unsigned long long)R.rt.color[c->index]->serial, R.rt.color[c->index]->d.width, R.rt.color[c->index]->d.height, g_enc != nil, c->mask, R.sc.x, R.sc.y, R.sc.w, R.sc.h, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                    break; }
                R.clear_c[c->index] = 1; R.cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                break;
            }
            case OP_CLEAR_DS: {
                const p_clear_ds *c = a;
                if (!g_render_on) { PD("C %llu 深度模板清除：條件繪製關著，跳過", g_stat_pass); break; }
                if (g_rt_merged) rp_end();
                // 通道中途的深度模板清除：結束當前通道，清除掛到下一個通道開頭（顏色附件照舊載入，同上面續用通道遇到清除的做法）。原先直接跳過，之後的繪製拿舊深度做測試。
                // 剪裁沒蓋滿深度目標時，Metal 的載入清除表達不了局部清除，仍跳過
                // Depth / stencil clear in the middle of a pass: end the current pass and attach the clear to the start of the next one (color attachments load as usual, as a continued pass does above when a clear arrives). It used to be skipped, and later draws depth-tested against stale depth.
                // When the scissor does not cover the depth target, Metal's load-action clear cannot express a partial clear; still skipped
                if (g_enc) {
                    if (R.rt.depth && !scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview)) { RED_ONCE(7, "通道中途的剪裁深度模板清除未實現"); break; }
                    { static int once; if (!once++) MLOG("通道中途的深度模板清除：結束本通道，清除掛到下一個通道開頭（首例 通道 %llu）", g_stat_pass); }
                    rp_end();
                }
                PD("C %llu 深度模板清除 depth=%g dmask=%#x stencil=%d smask=%#x 當前寫深度 %d 深度測試 %d", g_stat_pass, c->depth, (unsigned)c->depth_mask, c->stencil, (unsigned)c->stencil_mask, S.ds.dw, S.ds.dt);
                if (c->depth_mask) { R.clear_d = 1; R.cd = c->depth; }
                if (c->stencil_mask) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
                break;
            }
            case OP_DRAW: if (!g_render_on) break; g_exec_next = p + h.size; draw(a, NULL); g_exec_next = NULL; break;   // 前瞻起點；條件繪製關著不畫 | Lookahead start point; skipped while conditional rendering is off
            case OP_DRAW_IDX: if (!g_render_on) break; g_exec_next = p + h.size; draw(NULL, a); g_exec_next = NULL; break;
            // 狀態沒變就不置髒（遊戲每次繪製前常重發同值的程序 / 狀態塊 / 頂點格式；髒標誌只決定是否重取管線 / 深度模板對象，值相同則對象相同）
            // Do not mark dirty if the state did not change (the game often re-sends the same program / state block / vertex format before each draw; the dirty flag only decides whether to re-fetch the pipeline / depth-stencil object, and equal values mean the same object)
            case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
            case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BLEND_COLOR: case OP_COPY_STRIDE: case OP_DEPTH_CLAMP:
                state_op(h.op, a); break;
            case OP_RENDER_EN: { const p_render_en *e = a; g_render_on = cr_eval(e);
                PD("E %llu 條件繪製 kind=%d addr=%#llx 計數 %#llx / %#llx → %s", g_stat_pass, e->kind, (unsigned long long)e->addr,
                   (unsigned long long)(e->addr ? cr_counter(e->addr) : 0), (unsigned long long)(e->addr ? cr_counter(e->addr + 16) : 0), g_render_on ? "畫" : "不畫");
                static int once; if (!once++ && e->kind >= 2) MLOG("條件繪製：首例 模式 %s → %s", e->kind == 2 ? "相等才畫" : "不等才畫", g_render_on ? "畫" : "不畫");
                break; }
            case OP_COPY_B2T: copy_b2t(a); break;
            case OP_COPY_T2B: copy_t2b(a); break;
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DISPATCH: dispatch(a); break;
            case OP_BARRIER: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            default: break;
        }
        p += h.size;
    }
}

// ---------- 出圖：交換鏈貼圖畫到 CAMetalLayer；可選截圖 ----------
// ---------- Present: draw the swapchain texture to the CAMetalLayer; optional screenshot ----------
static id<MTLRenderPipelineState> g_blit_pso; static MTLPixelFormat g_blit_fmt;
static id<MTLSamplerState> g_blit_samp;
static uint64_t g_present_n;
uint64_t mtl_present_count(void) { return g_present_n; }   // 實際出圖次數（發熱歸因 DM_HEAT 算出圖幀率） | Actual present count (heat attribution DM_HEAT computes the present frame rate)
void mtl_res_snapshot(mtl_res_count *out) {
    memset(out, 0, sizeof *out);
    out->pso = g_pso.n;
    out->cps = g_cps.n;
    out->dss = g_dss.n;
    out->shaders = (uint32_t)(g_shcache ? g_shcache.count : 0);
    os_unfair_lock_lock(&g_c1_lock);
    out->c1_bufs = (uint32_t)(g_c1_banks.count + g_c1_exclusive); out->c1_alloc = g_c1_alloc_bytes; out->c1_data = g_c1_data_bytes;
    os_unfair_lock_unlock(&g_c1_lock);
    pthread_mutex_lock(&g_smu);
    out->live_tex = g_by_serial.n;
    pthread_mutex_unlock(&g_smu);
    pthread_mutex_lock(&g_mu);
    out->pools = (uint32_t)g_npool;
    out->pool_adds = g_pool_adds;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_rmu);
    out->view_slots = (uint32_t)g_ntreg;
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].view) out->view_nonnull++;
    for (int i = 0; i < g_nsreg; i++) if (g_sview[i]) out->sview_nonnull++;
    pthread_mutex_unlock(&g_rmu);
}
// 資源計數（長時間穩定性 L3 讀）：宿主按遊戲拍 / 事件調用（每 250 拍、讀取畫面已撤、退出），不按牆鐘 / 出圖數取樣。
// 行格式：[nvn] 資源計數 拍 <拍號> <觸發>：<名> <數值> …（名不含空格，單空格分隔）；加字段就往這一行後面追加
// Resource counts (read by long-run stability L3): the host calls it per game tick / event (every 250 ticks, loading screen dismissed, exit), not sampled by wall clock / present count.
// Line format: [nvn] 資源計數 拍 <拍號> <觸發>：<名> <數值> … ([nvn] resource counts tick <tick number> <trigger>: <name> <value> ...) (names contain no spaces, single-space separated); new fields are appended to the end of this line
void mtl_res_counts(uint32_t tick, const char *why) {
    mtl_res_count c;
    mtl_res_snapshot(&c);
    int dl, du; uint64_t da; nvn_dirty_slot_stats(&dl, &du, &da);
    LOG("[nvn] 資源計數 拍 %u %s：渲染管線 %u 計算管線 %u 著色器 %u 深度模板 %u view非空 %u view槽 %u 存活貼圖 %u 內存池存活 %u 內存池累計 %llu 採樣器view非空 %u 髒頁區間存活 %d 髒頁區間累計 %llu c1緩衝 %u c1字節 %llu c1實際 %llu",
        tick, why, c.pso, c.cps, c.shaders, c.dss, c.view_nonnull, c.view_slots, c.live_tex,
        c.pools, (unsigned long long)c.pool_adds, c.sview_nonnull, dl, (unsigned long long)da, c.c1_bufs, (unsigned long long)c.c1_alloc, (unsigned long long)c.c1_data);
}
// 按請求截圖（DM_NVN_SHOT_UI 用）：dm_gfx_shot_at(k, 路徑) = 在當前出圖幀之後第 k 幀截圖；每幀出圖時檢查到期請求
// Screenshot on request (used by DM_NVN_SHOT_UI): dm_gfx_shot_at(k, path) = take a screenshot at the kth frame after the current present frame; due requests are checked at every frame's present
#define SHOTQ_MAX 8
static struct { uint64_t frame; char path[1024]; } g_shotq[SHOTQ_MAX]; static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]);
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], int uv_i);
// 整窗截圖要的遊戲畫面（前端 window_shot，不再對掛著顯示節拍的 Metal 圖層做 drawViewHierarchy）：下一次上屏時，出圖線程把同一幀
// 按上屏同一套編碼（present_encode）畫進一張與交換鏈同尺寸的離屏貼圖，GPU 做完後在完成回調裡交出 CGImage（回調不阻塞上屏）。g_shotq_mu 護
// Game picture needed by the whole-window screenshot (front end window_shot; no longer does drawViewHierarchy on a Metal layer with the display link attached): at the next present, the present thread draws the same frame
// with the same encoding as the present (present_encode) into an offscreen texture of the same size as the swapchain, and after the GPU finishes hands out a CGImage in the completion callback (the callback does not block the present). Guarded by g_shotq_mu
static void (^g_grab_cb)(CGImageRef img);
void mtl_grab_next_present(void (^cb)(CGImageRef img)) { pthread_mutex_lock(&g_shotq_mu); g_grab_cb = [cb copy]; pthread_mutex_unlock(&g_shotq_mu); }
static void grab_present(id<MTLTexture> src, const int crop[4], CGSize sz) {
    pthread_mutex_lock(&g_shotq_mu); void (^cb)(CGImageRef) = g_grab_cb; g_grab_cb = nil; pthread_mutex_unlock(&g_shotq_mu);
    if (!cb) return;
    size_t w = (size_t)sz.width, h = (size_t)sz.height, row = w * 4;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = w && h ? [g_dev newTextureWithDescriptor:d] : nil; id<MTLBuffer> buf = out ? [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared] : nil;
    id<MTLCommandBuffer> cb2 = buf ? q_cb() : nil;
    if (!cb2 || !present_encode(cb2, src, out, crop)) { MLOG("【紅】整窗截圖：遊戲畫面離屏編碼失敗（%zux%zu）", w, h); cb(NULL); return; }
    id<MTLBlitCommandEncoder> b = [cb2 blitCommandEncoder];
    [b copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding];
    [cb2 addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL; cb(img);
        CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs); }];
    [cb2 commit];
}
void dm_gfx_shot_at(long k, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < SHOTQ_MAX) { g_shotq[g_nshotq].frame = g_present_n + (uint64_t)(k > 0 ? k : 0); strlcpy(g_shotq[g_nshotq].path, path, sizeof g_shotq[0].path); g_nshotq++; }
    else MLOG("【紅】截圖請求超過 %d 個，丟棄 %s", SHOTQ_MAX, path);
    pthread_mutex_unlock(&g_shotq_mu);
}
// DM_NVN_SHOT_OUT=寬x高（畫質度量用）：出圖幀截圖改截「經 present_encode 放大到該分辨率」的結果，即玩家在屏幕上看到的畫面；screen=0（貼圖轉儲）不受影響
// DM_NVN_SHOT_OUT=WIDTHxHEIGHT (for picture quality metrics): the present-frame screenshot instead captures the result "upscaled to this resolution by present_encode", i.e. the picture the player sees on screen; screen=0 (texture dump) is unaffected
static void shot(id<MTLTexture> src, const char *path, int screen, const int crop[4]) {
    static int out_w = -1, out_h;
    if (out_w < 0) {
        out_w = 0;
        const char *e = getenv("DM_NVN_SHOT_OUT"); int w = 0, h = 0; char sep = 0, extra = 0;
        if (e && sscanf(e, "%d%c%d%c", &w, &sep, &h, &extra) == 3 && sep == 'x' && w > 0 && h > 0) { out_w = w; out_h = h; }
    }
    id<MTLTexture> read = src;
    if (out_w && screen) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:out_w height:out_h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
        if (!out) { MLOG("【紅】截圖輸出貼圖建不出 %dx%d", out_w, out_h); return; }
        id<MTLCommandBuffer> cb = q_cb();
        if (!present_encode(cb, src, out, crop)) return;
        [cb commit]; [cb waitUntilCompleted];
        read = out;
    }
    if (read.pixelFormat != MTLPixelFormatRGBA8Unorm && read.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB &&
        read.pixelFormat != MTLPixelFormatBGRA8Unorm && read.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) { MLOG("【紅】截圖不支持格式 %lu", (unsigned long)read.pixelFormat); return; }
    size_t w = read.width, h = read.height, row = w * 4;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:read sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int bgra = read.pixelFormat == MTLPixelFormatBGRA8Unorm || read.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGBitmapInfo bi = bgra ? (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little) : (CGBitmapInfo)kCGImageAlphaNoneSkipLast;
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, bi);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    // 任一步為空（目錄不存在 / 不可寫、位圖上下文建不出）報紅返回；原先直接 CFRelease(NULL) 崩在 CFRelease.cold（1952 寬 TEXDUMP 時撞到）
    // If any step yields null (directory does not exist / not writable, bitmap context cannot be created), report a failure and return; previously it went straight to CFRelease(NULL) and crashed in CFRelease.cold (hit during a 1952-wide TEXDUMP)
    int ok = 0;
    if (!ctx || !img || !dst) MLOG("【紅】截圖 %s 失敗：%s", path, !ctx ? "位圖上下文建不出" : !img ? "取圖失敗" : "寫不出（目錄不存在或不可寫）");
    else { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); }
    if (dst) CFRelease(dst); if (url) CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    dm_shot_written(path, ok);   // 先讓宿主寫「凍結拍」行，再寫「成功」行：截圖腳本見到「成功」就停宿主，順序反了滿載時日誌會缺「凍結拍」行（閘門 require_log 誤紅） | Let the host write the 「凍結拍」(frozen tick) line first, then write the 「成功」(success) line: the screenshot script stops the host as soon as it sees 「成功」(success), and with the order reversed the log would lack the 「凍結拍」(frozen tick) line under full load (false failure in the gate check's require_log)
    MLOG("截圖 %zux%zu 格式 %lu → %s %s", w, h, (unsigned long)read.pixelFormat, path, ok ? "成功" : "失敗");
    if (g_ruler_truth && getenv("DM_NVN_RULER") && !strcmp(getenv("DM_NVN_RULER"), "1") && ok) { static int shots; if (++shots == 2) g_ruler_pending = 1; }
    if (g_defer_ab >= 0 && getenv("DM_NVN_DEFER_AB") && !strcmp(getenv("DM_NVN_DEFER_AB"), "1") && ok) { static int shots; if (++shots == 2) { g_defer_ab_on = 1; MLOG("DM_NVN_DEFER_AB：第二張截圖寫盤完成，打開推遲合併通道"); } }
    if (g_udc_ab >= 0 && getenv("DM_NVN_UDC_AB") && !strcmp(getenv("DM_NVN_UDC_AB"), "1") && ok) {
        static int shots;
        if (++shots == 2) { g_udc_ab_on = 1; MLOG("DM_NVN_UDC_AB：第二張截圖寫盤完成，打開 DM_NVN_UNWRITTEN_DC"); }
    }
}
// ---------- DM_NVN_TEXDUMP=幀號:路徑（路徑裡 %d 換成「序號*100+mip 級」）：把該幀用過的顏色渲染目標逐張存圖 ----------
// 診斷用：在下一幀第一次開通道前等 GPU 做完，再把每張（取該幀用到的 mip 級）全屏採樣畫進 RGBA8 存 PNG（浮點格式按 [0,1] 截斷）
// ---------- DM_NVN_TEXDUMP=frame number:path (%d in the path is replaced with "index*100+mip level"): save each color render target used by that frame as an image ----------
// Diagnostic: before the first render pass of the next frame is opened, wait for the GPU to finish, then draw each one (the mip level used by that frame) by full-screen sampling into RGBA8 and save as PNG (float formats are clamped to [0,1])
static long g_td_frame = -2; static const char *g_td_path; static NSMutableArray *g_td_list; static int g_td_done;
static void td_dump(void);
void td_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    if (g_td_frame == -2) { const char *e = getenv("DM_NVN_TEXDUMP"), *c = e ? strchr(e, ':') : NULL; g_td_frame = c ? atol(e) : -1; g_td_path = c ? c + 1 : NULL; g_td_list = [NSMutableArray array]; }
    if (g_td_frame < 0 || g_td_done) return;
    long f = (long)atomic_load(&g_cur_frame);
    if (f > g_td_frame) { td_dump(); return; }
    if (f != g_td_frame) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) {
        id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        uint64_t ser = i < (NSUInteger)R.rt.n && R.rt.color[i] ? R.rt.color[i]->serial : 0;
        NSUInteger lv = pd.colorAttachments[i].level;
        BOOL seen = NO; for (NSArray *e in g_td_list) if ([e[1] unsignedLongLongValue] == ser && [e[2] unsignedIntegerValue] == lv) seen = YES;
        if (!seen) [g_td_list addObject:@[t, @(ser), @(lv)]];
    }
}
static void td_dump(void) {
    g_td_done = 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    mtl_finish_gpu();
    static id<MTLRenderPipelineState> ps; static id<MTLSamplerState> sp;
    if (!ps) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { float4 c = t.sample(s, in.uv); return float4(c.rgb, 1); }\n";
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sp = [g_dev newSamplerStateWithDescriptor:sd];
        if (!ps) { MLOG("【紅】TEXDUMP 管線失敗 %s", e.localizedDescription.UTF8String); return; }
    }
    MLOG("[texdump] 幀 %ld 共 %lu 張顏色目標", g_td_frame, (unsigned long)g_td_list.count);
    // DM_NVN_TEXRAW=序號,…：按貼圖序號存原始字節（含 3D；文件 路徑%d 換成序號、後綴 .raw，首行文本頭：寬 高 深 格式 每行字節）
    // DM_NVN_TEXRAW=index,...: save raw bytes by texture index (including 3D; file: %d in the path is replaced with the index, suffix .raw, first-line text header: width height depth format bytes-per-row)
    for (const char *q = getenv("DM_NVN_TEXRAW"); q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
        if (!strncmp(q, "info", 4)) {   // DM_NVN_TEXRAW=info：DM_NVN_TEXINFO 記下的那幾張 | DM_NVN_TEXRAW=info: the textures remembered by DM_NVN_TEXINFO
            static char buf[256]; size_t o = 0; for (int i = 0; i < g_texinfo_n; i++) o += snprintf(buf + o, sizeof buf - o, "%s%llu", i ? "," : "", (unsigned long long)g_texinfo_ser[i]);
            if (o) { static char env[300]; snprintf(env, sizeof env, "%s", buf); setenv("DM_NVN_TEXRAW", env, 1); q = getenv("DM_NVN_TEXRAW"); MLOG("[texraw] info → %s", q); } else continue;
        }
        uint64_t ser = (uint64_t)atoll(q); id o = serial_get(ser);
        id<MTLTexture> t = [o isKindOfClass:[NvPkg class]] ? ((NvPkg *)o)->tex : o;
        if (!t) { MLOG("[texraw] ser=%llu 不在序號表", ser); continue; }
        int bw, bh; NSUInteger w = t.width, h = t.height, dz = t.depth;
        size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : 4; (void)bw; (void)bh;
        size_t row = w * bpp, img = row * h;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:img * dz options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, dz) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:img];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)ser); strlcat(path, ".raw", sizeof path);
        FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %lu %zu\n", (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, row); fwrite(buf.contents, 1, img * dz, f); fclose(f); }
        MLOG("[texraw] ser=%llu %lux%lux%lu 格式 %lu → %s", ser, (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, path);
    }
    for (NSArray *e in g_td_list) {
        id<MTLTexture> t = e[0]; uint64_t ser = [e[1] unsignedLongLongValue]; NSUInteger lv = [e[2] unsignedIntegerValue];
        if (t.textureType != MTLTextureType2D) { MLOG("[texdump] ser=%llu 類型 %lu 跳過", ser, (unsigned long)t.textureType); continue; }
        if (getenv("DM_NVN_SNAPRAW")) {   // 同時存原始字節（HDR 目標轉 8 位圖會截斷；首行文本頭：寬 高 格式 每行字節） | Also save the raw bytes (converting an HDR target to an 8-bit image truncates; first-line text header: width height format bytes-per-row)
            NSUInteger rw = MAX(t.width >> lv, 1u), rh = MAX(t.height >> lv, 1u);
            size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR16Float || t.pixelFormat == MTLPixelFormatR16Unorm ? 2 : 4;
            id<MTLBuffer> rb = [g_dev newBufferWithLength:rw * rh * bpp options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> rc = q_cb(); id<MTLBlitCommandEncoder> bb = [rc blitCommandEncoder];
            [bb copyFromTexture:t sourceSlice:0 sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rw, rh, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:rw * bpp destinationBytesPerImage:rw * rh * bpp];
            [bb endEncoding]; [rc commit]; [rc waitUntilCompleted];
            char rp_[1024]; snprintf(rp_, sizeof rp_, g_td_path, (int)(ser * 100 + lv)); strlcat(rp_, ".raw", sizeof rp_);
            FILE *f = fopen(rp_, "wb"); if (f) { fprintf(f, "%lu %lu %lu %zu\n", (unsigned long)rw, (unsigned long)rh, (unsigned long)t.pixelFormat, rw * bpp); fwrite(rb.contents, 1, rw * rh * bpp, f); fclose(f); }
        }
        id<MTLTexture> v = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(lv, 1) slices:NSMakeRange(0, 1)];
        NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModePrivate;
        id<MTLTexture> o = [g_dev newTextureWithDescriptor:od];
        id<MTLCommandBuffer> cb = q_cb();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en setFragmentTexture:v atIndex:0]; [en setFragmentSamplerState:sp atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)(ser * 100 + lv));
        MLOG("[texdump] ser=%llu lv=%lu 格式 %lu %lux%lu", ser, (unsigned long)lv, (unsigned long)t.pixelFormat, (unsigned long)w, (unsigned long)h);
        shot(o, path, 0, NULL);
    }
    [g_td_list removeAllObjects];
}
// DM_NVN_TEXSNAP=幀內序號,…（配合 DM_NVN_TEXDUMP 的幀號與路徑）：這些通道一結束就把其顏色附件拷一份，與幀末存圖一起存
// （同一張目標常在幀內被清掉復用，幀末狀態反映不了中間通道的輸出）；文件名序號 = 幀內序號*100000 + 貼圖序號*100 + mip 級
// DM_NVN_TEXSNAP=in-frame index,... (together with the frame number and path of DM_NVN_TEXDUMP): as soon as these render passes end, copy their color attachments and save them together with the end-of-frame dump
// (the same target is often cleared and reused within a frame, so the end-of-frame state cannot reflect the output of intermediate passes); file name index = in-frame index*100000 + texture index*100 + mip level
static int g_ts_idx[64], g_ts_n = -1, g_ts_on; static uint64_t g_ts_first, g_ts_curf = ~0ull;
static __strong id<MTLTexture> g_ts_att[8]; static uint64_t g_ts_ser[8]; static NSUInteger g_ts_lv[8]; static int g_ts_na;
void ts_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    if (g_ts_n < 0) { g_ts_n = 0; const char *e = getenv("DM_NVN_TEXSNAP");
        for (const char *q = e; q && *q && g_ts_n < 64; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) g_ts_idx[g_ts_n++] = atoi(q); }
    g_ts_on = 0; if (!g_ts_n || g_td_frame < 0 || g_td_done) return;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_ts_curf) { g_ts_curf = f; g_ts_first = g_stat_pass; }
    if ((long)f != g_td_frame) return;
    int idx = (int)(g_stat_pass - g_ts_first); for (int i = 0; i < g_ts_n; i++) if (g_ts_idx[i] == idx) g_ts_on = idx + 1;
    g_ts_na = 0; if (!g_ts_on) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ts_att[g_ts_na] = t; g_ts_ser[g_ts_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ts_lv[g_ts_na] = pd.colorAttachments[i].level; g_ts_na++; }
}
void ts_after(void) {
    if (!g_ts_on) return;
    id<MTLCommandBuffer> dcb = q_cb();
    for (int i = 0; i < g_ts_na; i++) {
        id<MTLTexture> t = g_ts_att[i]; if (t.textureType != MTLTextureType2D) continue;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:t.mipmapLevelCount > 1];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; d.mipmapLevelCount = t.mipmapLevelCount;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d];
        id<MTLBlitCommandEncoder> b = [dcb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:g_ts_lv[i] toTexture:c destinationSlice:0 destinationLevel:g_ts_lv[i] sliceCount:1 levelCount:1]; [b endEncoding];
        [g_td_list addObject:@[c, @((uint64_t)(g_ts_on - 1) * 1000 + g_ts_ser[i]), @(g_ts_lv[i])]];
    }
    g_ts_on = 0;
    [dcb commit];
}
// 真實呈現時刻環：出圖序號 → presentedTime（秒；−1 = 被系統丟棄未呈現）
// Ring of actual presentation times: present sequence number -> presentedTime (seconds; -1 = dropped by the system, not presented)
#define PRING 1024
typedef struct { uint64_t n; double t; } pres_ent;
static pres_ent g_pres[PRING];
// 取第 n 次出圖的實際呈現時刻（未就緒返回 0，被丟棄返回 −1）
// Get the actual presentation time of the nth present (returns 0 if not ready, -1 if dropped)
double mtl_presented_time(uint64_t n) { pres_ent e = g_pres[n % PRING]; return e.n == n ? e.t : 0; }
static _Atomic uint64_t g_drawable_ns;   // 出圖裡等 nextDrawable 的累計牆鐘（計時埋點） | Accumulated wall clock waiting for nextDrawable in present (timing instrumentation)
uint64_t mtl_drawable_ns_take(void) { return atomic_exchange(&g_drawable_ns, 0); }
// 有窗口時屏幕的最高刷新率（Hz）；無窗口返回 0。Mac 用 CoreGraphics 顯示模式（線程安全的 C 接口，出圖配置在遊戲線程上讀）
// Maximum refresh rate (Hz) of the screen when there is a window; returns 0 without a window. Mac uses CoreGraphics display modes (thread-safe C interface; the present configuration is read on the game thread)
int mtl_display_hz(void) {
    if (!front_metal_layer()) return 0;
#if TARGET_OS_OSX
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
    return (int)(hz + 0.5);
#else
    Class c = NSClassFromString(@"UIScreen"); id s = c ? [c valueForKey:@"mainScreen"] : nil;
    return s ? [[s valueForKey:@"maximumFramesPerSecond"] intValue] : 0;
#endif
}
static _Atomic uint64_t g_present_stall_ns;   // 單測用：出圖線程每次出圖前額外停頓（模擬 drawable 遲遲不可用） | For unit tests: an extra pause before each present on the present thread (simulating a drawable that stays unavailable)
static CAMetalLayer *g_present_layer_resident;
void mtl_present_test_stall(uint64_t ns) { g_present_stall_ns = ns; }
static void present_pipeline(MTLPixelFormat format) {
    if (!g_blit_pso || g_blit_fmt != format) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "struct Crop { float4 rect; float4 edge; };\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        if (!lib) MDIE("出圖著色器編譯失敗 %s", e.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = format;
        g_blit_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!g_blit_pso) MDIE("出圖管線失敗 %s", e.localizedDescription.UTF8String);
        g_blit_fmt = format;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.supportArgumentBuffers = YES;
        g_blit_samp = [g_dev newSamplerStateWithDescriptor:sd];
    }
}
// 出圖僅採NVN聲明的有效源矩形；與原始交換鏈讀回截圖分開。邊緣鉗到有效區紋素中心，雙線性不讀外側黑紋素。
// Present samples only the valid source rectangle declared by NVN; kept separate from the raw swapchain readback screenshot. Edges are clamped to the texel centers of the valid area, so bilinear does not read the black texels outside.
// 出圖採樣交換鏈貼圖存的原始字節（Switch 掃描輸出就是原樣送存的字節）：sRGB 格式的源換成同存儲的非 sRGB 視圖，
// 否則採樣先解成線性、BGRA8Unorm 的層不再編碼，整幅偏暗。別的格式原樣返回（馬力歐的交換鏈是 RGBA8，不走這裡）。
// 視圖緩存 4 格：交換鏈貼圖壽命長、張數少；視圖持有父貼圖，父指針相等即同一對象。換下的視圖等用過它的提交跑完再放（Metal 4 不持有資源）
// Present samples the raw bytes stored in the swapchain texture (the Switch's scan-out sends the stored bytes as they are): an sRGB source is replaced by a non-sRGB view of the same storage,
// otherwise sampling decodes to linear first, the BGRA8Unorm layer does not encode again, and the whole picture comes out dark. Other formats are returned as they are (Mario's swapchain is RGBA8 and does not come here).
// View cache of 4 entries: swapchain textures live long and are few; a view holds its parent, so an equal parent pointer means the same object. A replaced view is released after the submissions that used it complete (Metal 4 does not retain resources)
static id<MTLTexture> present_raw_src(id<MTLTexture> src) {
    MTLPixelFormat pf = src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
    if (pf == MTLPixelFormatInvalid) return src;
    static __strong id<MTLTexture> views[4]; static int next; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    id<MTLTexture> v = nil;
    for (int i = 0; i < 4 && !v; i++) if (views[i] && views[i].parentTexture == src) v = views[i];
    if (!v && (v = [src newTextureViewWithPixelFormat:pf])) { if (views[next]) defer_obj_unref(views[next]); views[next] = v; next = (next + 1) & 3; }
    pthread_mutex_unlock(&mu);
    if (!v) { RED_ONCE(69, "出圖：sRGB 交換鏈貼圖的非 sRGB 視圖建不出（格式 %lu），按解碼後的值出圖", (unsigned long)src.pixelFormat); return src; }
    return v;
}
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]) {
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("【紅】出圖裁切越界：%d,%d %dx%d / 原紋理 %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    static uint64_t last_crop[8]; uint64_t meta[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(meta, last_crop, sizeof meta)) { memcpy(last_crop, meta, sizeof meta);
        MLOG("出圖裁切：原紋理 %lux%lu，有效 %lu,%lu %lux%lu → 目標 %lux%lu", (unsigned long)src.width, (unsigned long)src.height,
             (unsigned long)cx, (unsigned long)cy, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height); }
    present_pipeline(dst.pixelFormat);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    // 保持遊戲畫面寬高比居中，餘下留黑邊（屏幕比例與遊戲不同時不拉伸，如 iPhone 約 19.5:9 對遊戲 16:9）；
    // 比例差 < 1%（全屏口徑下遊戲寬度取 8 的倍數帶來的零頭，如 1952x900 對 2868x1320 差 0.17%）直接鋪滿，不留幾像素的邊
    // Keep the game picture's aspect ratio and center it, leaving black bars in the rest (no stretching when the screen ratio differs from the game's, e.g. iPhone about 19.5:9 versus the game's 16:9);
    // when the ratio difference is < 1% (the remainder from the game width being a multiple of 8 in the full-screen setup, e.g. 1952x900 versus 2868x1320 differs by 0.17%) fill directly, leaving no few-pixel border
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    static double last_vp[4];   // 視口變化時記一行（真機屏幕級驗收 screen_check.py 核寬高比用） | Log one line when the viewport changes (used by the real-device screen-level acceptance screen_check.py to check the aspect ratio)
    if (memcmp(last_vp, &vp, sizeof last_vp)) { memcpy(last_vp, &vp, sizeof last_vp);
        MLOG("出圖視口 %.0f,%.0f %.0fx%.0f / 畫布 %.0fx%.0f（遊戲畫面 %lux%lu）", vp.originX, vp.originY, vp.width, vp.height, dw, dh, (unsigned long)src.width, (unsigned long)src.height); }
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setViewport:vp];
    [en setRenderPipelineState:g_blit_pso]; [en setFragmentTexture:present_raw_src(src) atIndex:0]; [en setFragmentSamplerState:g_blit_samp atIndex:0];
    float uv[8] = {(float)cx / src.width, (float)cy / src.height, (float)cw / src.width, (float)ch / src.height,
                   (cx + 0.5f) / src.width, (cy + 0.5f) / src.height, (cx + cw - 0.5f) / src.width, (cy + ch - 0.5f) / src.height};
    [en setFragmentBytes:uv length:sizeof uv atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    return 1;
}
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], int uv_i) {
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("【紅】出圖裁切越界：%d,%d %dx%d / 原紋理 %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    present_pipeline(dst.pixelFormat);
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { MLOG("【紅】M4 出圖渲染編碼器創建失敗"); return 0; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en setArgumentTable:g_present_args atStages:MTLRenderStageFragment];
    [g_present_args setTexture:present_raw_src(src).gpuResourceID atIndex:0];
    [g_present_args setSamplerState:g_blit_samp.gpuResourceID atIndex:0];
    float *uv = g_present_uv[uv_i].contents;
    uv[0] = (float)cx / src.width; uv[1] = (float)cy / src.height; uv[2] = (float)cw / src.width; uv[3] = (float)ch / src.height;
    uv[4] = (cx + 0.5f) / src.width; uv[5] = (cy + 0.5f) / src.height; uv[6] = (cx + cw - 0.5f) / src.width; uv[7] = (cy + ch - 0.5f) / src.height;
    [g_present_args setAddress:g_present_uv[uv_i].gpuAddress atIndex:0];
    [en setViewport:vp]; [en setRenderPipelineState:g_blit_pso];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    m4_end(en);
    return 1;
}
static double g_link_it_tin;   // 節拍回調正在上屏的那一幀的入隊時刻（只在節拍線程讀寫） | Enqueue time of the frame the display-link callback is presenting (read and written only on the display-link thread)
static int present_now(const tex_t *t, double period, uint64_t frame, uint64_t ticket, const int crop[4], id<CAMetalDrawable> supplied, double target_ts) {
    if (g_present_stall_ns) { struct timespec ts = {0, (long)g_present_stall_ns}; nanosleep(&ts, NULL); }
    g_present_n++;
    if (g_use != USE_OFF && g_use != USE_DONE) use_tick();
    if (bootprof() && (g_present_n == 1 || g_present_n % 120 == 0)) {
        if (g_present_n == 1) g_bp_first_present = bp_now() - g_bp_t0;
        MLOG("[bootprof] t=%.3f s 出圖第 %llu 次：metallib 載入 %llu 個 / %.1f ms；管線創建 %llu 個 / %.1f ms（均在提交線程同步）；預熱領用 著色器 %llu / 渲染管線 %llu / 計算管線 %llu；"
             "貼圖上傳 %.1f MB / %llu 張首傳 + %llu 次重傳 / %.1f ms；首幀出圖 t=%.3f s", (bp_now() - g_bp_t0) / 1e9, g_present_n,
             g_bp_lib_n, g_bp_lib_ns / 1e6, g_bp_pso_n, g_bp_pso_ns / 1e6, g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps,
             g_bp_up_bytes / 1048576.0, g_pkg_uploads, g_pkg_reuploads,
             g_up_total_ns / 1e6, g_bp_first_present / 1e9);
    }
    PD("X ser=%llu", t->serial);
    id<MTLTexture> src = tex_mtl(t);
    if (!src) { RED_ONCE(8, "出圖貼圖建不出 MTLTexture"); return 0; }
    // DM_NVN_REGLUM=x0,y0,x1,y1:文件（歸一化坐標）：每次出圖回讀該區域，記「出圖序號、出圖時刻 ns、區域平均亮度、隔點灰度小圖（十六進制）」（診斷：核動畫速度與幀率無關）。
    // 單獨的命令緩衝排在本幀渲染之後（同一隊列按提交序執行），不等 GPU、不擾動幀節奏
    // DM_NVN_REGLUM=x0,y0,x1,y1:file (normalized coordinates): read back this region on every present and record "present sequence number, present time ns, region mean luminance, subsampled grayscale thumbnail (hex)" (diagnostic: verify that animation speed is independent of frame rate).
    // A separate command buffer is queued after this frame's rendering (the same queue executes in submit order); does not wait for the GPU and does not disturb frame pacing
    static int rl = -1; static float rr[4]; static FILE *rlf; static pthread_mutex_t rlmu = PTHREAD_MUTEX_INITIALIZER;
    if (rl < 0) { const char *e = getenv("DM_NVN_REGLUM"), *c = e ? strchr(e, ':') : NULL;
        rl = c && sscanf(e, "%f,%f,%f,%f", &rr[0], &rr[1], &rr[2], &rr[3]) == 4 && (rlf = fopen(c + 1, "w")) != NULL;
        if (e && !rl) MLOG("【紅】DM_NVN_REGLUM 參數或文件無效：%s", e); }
    if (rl && (src.pixelFormat == MTLPixelFormatRGBA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB)) {
        NSUInteger x0 = (NSUInteger)(rr[0] * src.width), y0 = (NSUInteger)(rr[1] * src.height), w = (NSUInteger)(rr[2] * src.width) - x0, h = (NSUInteger)(rr[3] * src.height) - y0;
        id<MTLBuffer> rb = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        [b endEncoding];
        unsigned long long n = g_present_n, tns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; const uint8_t *q = rb.contents; uint64_t sum = 0;
            uint8_t g[4096]; NSUInteger ng = 0;   // 隔點取樣的灰度小圖（每行 / 每列取 1/4，最多 4096 點），隨均值一起記下，供逐幀圖像相關分析 | Subsampled grayscale thumbnail (every 4th row / column, at most 4096 points), recorded along with the mean, for per-frame image correlation analysis
            for (NSUInteger y = 0; y < h; y += 4) for (NSUInteger x = 0; x < w && ng < sizeof g; x += 4) { const uint8_t *px = q + 4 * (y * w + x); g[ng++] = (uint8_t)((px[0] + px[1] + px[2]) / 3); }
            for (NSUInteger k = 0; k < w * h; k++) sum += q[4 * k] + q[4 * k + 1] + q[4 * k + 2];
            pthread_mutex_lock(&rlmu); fprintf(rlf, "%llu\t%llu\t%.4f\t", n, tns, sum / (3.0 * w * h));
            for (NSUInteger k = 0; k < ng; k++) fprintf(rlf, "%02x", g[k]);
            fputc('\n', rlf); fflush(rlf); pthread_mutex_unlock(&rlmu); }];
        [cb commit];
    }
    // DM_NVN_SHOT=幀號[,幀號…]:路徑（路徑裡的 %d 換成幀號）
    // DM_NVN_SHOT=frame number[,frame number...]:path (%d in the path is replaced with the frame number)
    static long shots[4096]; static int nshot = -1; static const char *shot_path;   // 最多 4096 幀（連拍一段幀給逐幀打分用） | up to 4096 frames (a burst over a frame range, for scoring frame by frame)
    if (nshot < 0) {
        nshot = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { shot_path = c + 1; for (const char *q = e; q < c && nshot < 4096; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) shots[nshot++] = atol(q); }
    }
    for (int i = 0; i < nshot; i++) if ((long)g_present_n == shots[i]) { char path[1024]; snprintf(path, sizeof path, shot_path, (int)g_present_n); mtl_finish_gpu(); shot(src, path, 1, crop); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (g_present_n >= g_shotq[i].frame) { mtl_finish_gpu(); shot(src, g_shotq[i].path, 1, crop); g_shotq[i] = g_shotq[--g_nshotq]; } else i++;
    pthread_mutex_unlock(&g_shotq_mu);

    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer) return 0;
    if (g_present_layer_resident != layer) { [g_q4 addResidencySet:layer.residencySet]; g_present_layer_resident = layer; }
#if TARGET_OS_OSX
    if (!layer.displaySyncEnabled) layer.displaySyncEnabled = YES;   // 窗口模式按顯示器垂直同步，防撕裂 | Windowed mode follows the display's vsync to prevent tearing
#endif
    present_pipeline(layer.pixelFormat);   // 和原路徑一樣，首幀編管線在取drawable之前 | Same as the original path: the first frame compiles the pipeline before taking the drawable
    if (g_grab_cb) grab_present(src, crop, layer.drawableSize);   // 整窗截圖要的遊戲畫面（不加鎖先看一眼，真取在函數裡加鎖） | Game picture needed by the whole-window screenshot (peek without the lock first; the real take locks inside the function)
    uint64_t dt0 = now_ns();
    id<CAMetalDrawable> dr = supplied ? supplied : [layer nextDrawable];
    if (!supplied) g_drawable_ns += now_ns() - dt0;
    if (!dr) return 0;
    int alloc_i = (int)(ticket % 3);
    if (g_present_alloc_fence[alloc_i]) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_done < g_present_alloc_fence[alloc_i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu);
    }
    [g_present_alloc4[alloc_i] reset];
    [g_q4 waitForDrawable:dr];
    id<MTL4CommandBuffer> cb = g_present_cb[alloc_i];
    [cb beginCommandBufferWithAllocator:g_present_alloc4[alloc_i]];
    if (!present_encode_m4(cb, src, dr.texture, crop, alloc_i)) { [cb endCommandBuffer]; [g_q4 signalDrawable:dr]; return 0; }
    [cb endCommandBuffer];
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) MLOG("【紅】上屏命令緩衝錯誤：%s", f.error.description.UTF8String);
        pthread_mutex_lock(&g_pq_mu); if (ticket > g_pq_done) g_pq_done = ticket; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }];
    id<MTL4CommandBuffer> buffers[] = {cb};
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
    [g_q4 commit:buffers count:1 options:o];
    if (target_ts > 0) {
        static double leads[300], e2e[300];   // e2e：遊戲入隊 → 系統預定上屏（含在隊列裡等節拍回調的時間），輸入延遲裡我們這一側能讀到的全程 | e2e: game enqueue -> system-scheduled present (including the time waiting in the queue for the display-link callback), the full span of input latency that our side can read
        static uint64_t lead_n;
        leads[lead_n % 300] = (target_ts - CACurrentMediaTime()) * 1000.0; e2e[lead_n % 300] = g_link_it_tin > 0 ? (target_ts - g_link_it_tin) * 1000.0 : 0;
        lead_n++;
        if (lead_n % 300 == 0) {
            double a[300]; memcpy(a, leads, sizeof a);
            for (int i = 1; i < 300; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
            double b[300]; memcpy(b, e2e, sizeof b);
            for (int i = 1; i < 300; i++) { double v = b[i]; int j = i - 1; while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; } b[j + 1] = v; }
            MLOG("顯示節拍 至 %llu：節拍 300、出圖 %llu、空節拍 %llu、合併丟棄舊幀 %llu；提交領先目標上屏 p50 %.3f ms / 最小 %.3f ms；遊戲入隊到目標上屏 p50 %.3f ms / 最小 %.3f / 最大 %.3f ms",
                 (unsigned long long)g_link_tick_n, (unsigned long long)g_present_n, (unsigned long long)(g_link_tick_n - g_present_n),
                 (unsigned long long)g_link_dropped, a[149], a[0], b[149], b[0], b[299]);
        }
    }
    [g_q4 signalDrawable:dr];
    g_present_alloc_fence[alloc_i] = ticket;
#if TARGET_OS_SIMULATOR   // iOS 模擬器 SDK 沒有呈現時刻回調與最短顯示時長（只作畫面驗收用，不計節拍） | The iOS simulator SDK has no presentation-time callback and no minimum display duration (used only for picture acceptance, display-link ticks are not counted)
    (void)period; [dr present];
#else
    uint64_t pn = ticket;   // 按遊戲出圖序號（票號）記上屏時刻：hle 的上屏統計按「遊戲第 n 次出圖」取；顯示節拍路徑裡實際出圖次數 ≠ 遊戲出圖次數（舊幀會被合併丟棄） | Record the present time by the game's present sequence number (ticket number): hle's present statistics are taken by "the game's nth present"; in the display link path the actual present count != the game's present count (old frames get merged and dropped)
    [dr addPresentedHandler:^(id<MTLDrawable> d) {   // 真實呈現時刻（屏幕上真正換上這一幀的時間，CACurrentMediaTime 基準） | Actual presentation time (the time this frame is really put on screen, CACurrentMediaTime base)
        CFTimeInterval tt = d.presentedTime;
        g_pres[pn % PRING] = (pres_ent){pn, tt > 0 ? tt : -1};
    }];
    // 每張至少顯示一個出圖週期：GPU 完成時刻在 120 Hz 刷新邊界附近抖動時，不再出現「25 ms + 8.33 ms」成對的頓挫
    // Each frame is displayed for at least one present period: when the GPU completion time jitters around a 120 Hz refresh boundary, the paired "25 ms + 8.33 ms" stutter no longer appears
    if (target_ts > 0) [dr present];
    else if (period > 0) [dr presentAfterMinimumDuration:period]; else [dr present];
#endif
    (void)frame;
    return 1;
}
// ---------- 出圖線程 ----------
// NVN 語義：QueuePresentTexture 只把出圖排進隊列、立即返回（遊戲常持鎖調用它；iOS 真機：在裡面同步等 nextDrawable
// 約 30 ms，鎖被佔約 90%，裝載線程餓死）。取 drawable、blit、present 都在這條線程上按入隊順序做。
// 票號 = 第幾次入隊（從 1 起）；g_pq_done = 已處理完（blit 已提交到 GPU 隊列）的票號。
// 交換鏈貼圖歸還：AcquireTexture 拿到某張前等它上一次出圖的票號處理完（mtl_present_wait），之後遊戲再往裡畫的命令
// 在 GPU 隊列上必排在那次 blit 之後。
// ---------- Present thread ----------
// NVN semantics: QueuePresentTexture only queues the present and returns immediately (the game often calls it while holding a lock; iOS real device: synchronously waiting for nextDrawable inside it
// takes about 30 ms, the lock is held about 90% of the time, and the loading thread starves). Taking the drawable, blit and present are all done on this thread in enqueue order.
// Ticket number = which enqueue it is (starting from 1); g_pq_done = the ticket number that has been processed (blit committed to the GPU queue).
// Swapchain texture return: before AcquireTexture gets a texture it waits for the ticket of that texture's previous present to be processed (mtl_present_wait), after which the commands the game draws into it
// are necessarily ordered after that blit on the GPU queue.
#define PQ 8
typedef struct { tex_t t; double period; uint64_t frame; int crop[4]; double t_in; } pq_item;   // t_in：遊戲把這一幀交進出圖隊列的時刻（CACurrentMediaTime） | t_in: the time the game handed this frame into the present queue (CACurrentMediaTime)
static pq_item g_pq[PQ];
static uint64_t g_pq_in;
static int present_now(const tex_t *, double, uint64_t, uint64_t, const int[4], id<CAMetalDrawable>, double);

@interface NvMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end

// 前台節拍停了的兜底（兩次合入冒煙卡死：前台時節拍不再回調，排隊幀沒人取，遊戲在「取下一張交換鏈貼圖」前等上一次出圖處理完，永遠等下去）：
// mtl_present_wait 等超過 STALL_NS 且節拍這麼久都沒回調，就照進後台那樣把等著的這張及更早的排隊幀丟掉（按掉幀記賬），放遊戲繼續跑；節拍回來自動照常。
// 一段停頓記一行線索、恢復時記一行（都不是紅）；同一段停頓連著超過 STALL_RED_NS 才紅。全在 g_pq_mu 下
// Fallback for the display link going silent while in the foreground (two smoke-test hangs before merge: the display link stopped calling back while the app was in the foreground, nobody takes the queued frames, and the game waits forever for its previous present to be processed before "acquiring the next swapchain texture"):
// when mtl_present_wait has waited longer than STALL_NS and the display link has not called back for that long, drop the waiting frame and earlier queued frames as when entering the background (accounted as dropped frames) and let the game keep running; when the display link comes back things return to normal automatically.
// One stall logs one hint line, and one line on recovery (neither is a failure); only when the same stall lasts longer than STALL_RED_NS is it a failure. All under g_pq_mu
#define STALL_NS 100000000ull
#define STALL_RED_NS 2000000000ull
static uint64_t g_stall_t0, g_stall_drop, g_stall_n; static int g_stall_red, g_stall_logged;
// 顯示節拍看門狗只認「激活」狀態：App 失去活躍（下拉控制中心 / 通知、來電、系統彈窗）和後台裡系統本來就不回調，畫面也被蓋住——丟幀兜底照舊，
// 但不算卡住、不報紅；停頓從「最近一次回調 / 最近一次激活」較晚的那個算起（A16 日誌：失活 8–17 秒、後台 17 秒都被報成「前台卡住」）
// The display link watchdog only recognizes the "active" state: when the App loses active status (pulling down Control Center / notifications, incoming call, system popup) and in the background the system does not call back anyway, and the picture is covered too -- the frame-drop fallback stays as before,
// but it is not counted as stuck and no failure is reported; the stall is measured from the later of "the most recent callback / the most recent activation" (A16 logs: 8–17 seconds inactive and 17 seconds in the background were both reported as 「前台卡住」(stuck in foreground))
__attribute__((weak)) int dm_focus_now(void) { return 1; }   // 宿主 hle_oe.c 提供強定義；圖形單測不鏈宿主時視為一直激活 | The host's hle_oe.c provides the strong definition; when graphics unit tests do not link the host it is treated as always active
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
@implementation NvMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    pthread_mutex_lock(&g_pq_mu);
    if (g_stall_t0) { uint64_t since = dm_focus_in_since(), t = now_ns();
        MLOG("顯示節拍恢復：這段停了 %.0f ms（其中激活中 %.0f ms），兜底丟了 %llu 幀（第 %llu 段）", (t - g_stall_t0) / 1e6, (t - (g_stall_t0 > since ? g_stall_t0 : since)) / 1e6, (unsigned long long)g_stall_drop, (unsigned long long)g_stall_n); g_stall_t0 = 0; }
    pthread_mutex_unlock(&g_pq_mu);
    pthread_mutex_lock(&g_link_mu);
    g_link_tick_ns = now_ns();
    g_link_tick_n++;
    pthread_cond_broadcast(&g_link_cv);
    pthread_mutex_unlock(&g_link_mu);
    pthread_mutex_lock(&g_pq_mu);
    if (!g_dm_present_link || g_pq_read == g_pq_in) { pthread_mutex_unlock(&g_pq_mu); return; }
    uint64_t first = g_pq_read + 1, ticket = g_pq_in;
    pq_item it = g_pq[(g_pq_in - 1) % PQ]; g_link_it_tin = it.t_in;
    g_pq_read = g_pq_in;
    if (ticket > first) { g_link_dropped += ticket - first; for (uint64_t k = first; k < ticket; k++) g_pres[k % PRING] = (pres_ent){k, -1}; }   // 被合併丟棄的舊幀記「未上屏」，上屏統計如實算掉幀 | Old frames merged and dropped are recorded as "not presented", so the present statistics honestly count them as dropped frames
    if (ticket > 1 && g_pq_done < ticket - 1) g_pq_done = ticket - 1;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    int submitted;
    @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, update.drawable, update.targetPresentationTimestamp); }
    if (!submitted) {
        pthread_mutex_lock(&g_pq_mu);
        if (ticket > g_pq_done) g_pq_done = ticket;
        pthread_cond_broadcast(&g_pq_cv);
        pthread_mutex_unlock(&g_pq_mu);
    }
}
@end

static CAMetalDisplayLink *g_link; static CFRunLoopRef g_link_rl; static int g_link_started;
static int g_link_owns_layer, g_classic_busy;   // 持 g_pq_mu：圖層掛著節拍對象期間 nextDrawable 會拋異常，自取 drawable 的出圖線程與節拍對象互斥 | Under g_pq_mu: nextDrawable throws while the layer has the display link object attached, so the present thread that takes its own drawable and the display link object are mutually exclusive
static int link_on(void) { return g_dm_present_link && g_link != nil; }
static CAFrameRateRange link_range(int mode) { return mode == 3 ? CAFrameRateRangeMake(30, 30, 30) : mode == 2 ? CAFrameRateRangeMake(60, 120, 120) : CAFrameRateRangeMake(60, 60, 60); }
static int g_link_mode = 1;   // 節拍對象當前的模式：1 / 2 同 g_dm_present_link，3 = 省電模式 30 Hz | Current mode of the display link object: 1 / 2 same as g_dm_present_link, 3 = power-saving mode 30 Hz
static void *link_main(void *arg) {   // 顯示節拍線程：切到節拍模式時起，切回 0 時退出 | Display link thread: started when switching to display-link mode, exits when switching back to 0
    (void)arg;
    pthread_setname_np("nvn-present-link");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; while (g_classic_busy) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu);   // 等出圖線程手上那一幀交完 | Wait for the present thread to finish handing over the frame it holds
    NvMetalDisplayLinkDelegate *delegate = [NvMetalDisplayLinkDelegate new];
    CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    link.preferredFrameRateRange = link_range(g_link_mode);
    link.preferredFrameLatency = g_present_latency;
    link.delegate = delegate;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    g_link_rl = CFRunLoopGetCurrent(); g_link = link;
    CFRunLoopRun();   // 切回模式 0 時節拍對象作廢、本線程退出（圖層還掛著節拍對象時 nextDrawable 取不到圖，不能只暫停） | When switching back to mode 0 the display link object is invalidated and this thread exits (nextDrawable cannot get a drawable while the layer still has the display link object attached, so pausing alone is not enough)
    g_link_started = 0;
    return NULL;
}
static void link_sync(void) {   // 遊戲出圖線程每幀調：把節拍對象的狀態對齊到 g_dm_present_link 與省電模式（運行中可改） | Called every frame by the game's present thread: aligns the display link object's state to g_dm_present_link and the power-saving mode (can change at run time)
    static int applied; int want = g_dm_present_link;
    if (want < 0 || want > 2) { g_dm_present_link = applied == 3 ? 1 : applied; return; }
    if (want && mtl_fps_target() == 30) want = 3;   // 省電模式：顯示節拍 30 Hz | Power-saving mode: display link at 30 Hz
    if (want == applied) return;
    g_link_mode = want;
    if (!g_link_started) {
        if (!want) { applied = 0; return; }
        if (!front_metal_layer()) return;
        g_link_started = 1; pthread_t th; if (pthread_create(&th, NULL, link_main, NULL)) MDIE("顯示節拍線程起不來"); pthread_detach(th);
        return;   // 節拍對象在新線程裡建，下一幀再對齊 | The display link object is created on a new thread; align again next frame
    }
    CAMetalDisplayLink *link = g_link; if (!link) return;
    if (want) link.preferredFrameRateRange = link_range(want);
    else { CFRunLoopRef rl = g_link_rl; g_link = nil; CFRunLoopPerformBlock(rl, kCFRunLoopDefaultMode, ^{ [link invalidate]; pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); CFRunLoopStop(rl); }); CFRunLoopWakeUp(rl); }
    MLOG("出圖節拍模式 %d → %d（幀 %llu）", applied, want, (unsigned long long)atomic_load(&g_cur_frame));
    applied = want;
    pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
// 進後台前（mtl_drain）：顯示節拍驅動出圖時，排著隊還沒上屏的幀只能由節拍回調取走，而後台裡系統不再回調——
// 原樣等「出圖隊列清空」會永遠等下去，進後台的回調不返回，系統把進程殺掉（切到別的 App 後 22 秒內進程消失）。
// 這些幀反正不會被看到：直接丟棄（按掉幀記賬），只等已經提交給 GPU 的那一次上屏做完。
// Before going to the background (mtl_drain): when the display link drives presents, frames queued but not yet presented can only be taken by the display-link callback, and in the background the system no longer calls back --
// waiting as-is for "the present queue to empty" would wait forever, the enter-background callback would not return, and the system would kill the process (the process disappears within 22 seconds after switching to another App).
// These frames will not be seen anyway: discard them directly (accounted as dropped frames) and only wait for the one present already submitted to the GPU to finish.
static void present_drop_for_suspend(void) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_link_owns_layer) {
        uint64_t n = g_pq_in - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= g_pq_in; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = g_pq_in;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < g_pq_in) g_pq_done = g_pq_in;
        pthread_cond_broadcast(&g_pq_cv);
        if (n) MLOG("進後台：丟棄排隊中未上屏的 %llu 幀（顯示節拍在後台不回調）", (unsigned long long)n);
    }
    pthread_mutex_unlock(&g_pq_mu);
}
static void *present_main(void *arg) {
    (void)arg;
    pthread_setname_np("nvn-present");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp); atomic_store(&g_present_qos, (uint32_t)qc);
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_read == g_pq_in || g_link_owns_layer) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // 節拍模式下由節拍回調取；空隊列看「已取出」；「已完成」要等上屏提交的 feedback，落後於取出 | In display-link mode the display-link callback takes them; an empty queue is judged by "taken"; "done" has to wait for the feedback of the present submit and lags behind taken
        pq_item it = g_pq[g_pq_read % PQ];
        uint64_t ticket = ++g_pq_read; g_classic_busy = 1;
        pthread_mutex_unlock(&g_pq_mu);
        int submitted;
        @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, nil, 0); }
        pthread_mutex_lock(&g_pq_mu); g_classic_busy = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
        if (!submitted) {
            pthread_mutex_lock(&g_pq_mu);
            if (ticket > g_pq_done) g_pq_done = ticket;
            pthread_cond_broadcast(&g_pq_cv);
            pthread_mutex_unlock(&g_pq_mu);
        }
    }
    return NULL;
}
uint32_t mtl_present_qos(void) { return atomic_load(&g_present_qos); }
static void present_start(void) { pthread_t th; if (pthread_create(&th, NULL, present_main, NULL)) MDIE("出圖線程起不來"); pthread_detach(th); }
static _Atomic int g_present_discard;
void mtl_link_pause(int on);
// 前端進後台、等遊戲存盤那一小段：出圖直接丟（顯示節拍後台不回調，入隊會把遊戲線程堵住；畫面也沒人看）。
// 打開時把已經排著隊的幀一並清掉：系統是先停節拍回調、之後才通知進後台，這段空檔裡遊戲已把交換鏈畫滿，正卡在「取下一張貼圖前等它上一次出圖處理完」
// （hle_nvn.c nvnWindowAcquireTexture → mtl_present_wait）。不清的話等存盤那 2.5 秒遊戲一拍都走不了，盤沒存成（玩家日誌裡三次同形：排隊 3 幀、提交 0 次）
// The short period when the front end goes to the background and waits for the game to save: presents are dropped directly (the display link does not call back in the background, enqueueing would block the game thread; nobody sees the picture anyway).
// When turned on, also clear the frames already queued: the system first stops the display-link callback and only afterwards notifies about going to the background, and in that gap the game has already filled the swapchain and is stuck at "before acquiring the next texture, wait for its previous present to be processed"
// (hle_nvn.c nvnWindowAcquireTexture -> mtl_present_wait). Without clearing, the game cannot advance a single tick during those 2.5 seconds of waiting for the save, and the save is not written (the same pattern three times in player logs: 3 frames queued, 0 submits)
void mtl_present_discard(int on) { atomic_store(&g_present_discard, on != 0); if (on) present_drop_for_suspend(); }
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    if (atomic_load(&g_present_discard)) return 0;   // 票號 0 = 沒有待完成的上屏，取交換鏈貼圖處不等 | Ticket number 0 = no pending present; the swapchain texture acquire does not wait
    if (t) { xv_check(t->serial, 0, 0, "出圖"); if (g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = t->serial; }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, present_start);
    link_sync();
    {   // 測試開關 DM_TEST_LINK_STALL=起始秒,時長秒：從第一次出圖起算到點暫停顯示節拍（模仿前台時節拍不回調），時長到了放開。驗兜底用，產品不設 | Test switch DM_TEST_LINK_STALL=start seconds,duration seconds: counting from the first present, pause the display link at that point (imitating the display link not calling back while in the foreground) and release it when the duration is up. For verifying the fallback; not set in the product
        static double t0, at = -1, dur; static int st; double now = CACurrentMediaTime();
        if (at < 0) { at = 0; const char *e = getenv("DM_TEST_LINK_STALL"); if (e && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0) { t0 = now; st = 1; } else at = 0; }
        if (st == 1 && link_on() && now - t0 >= at) { st = 2; mtl_link_pause(1); MLOG("DM_TEST_LINK_STALL：暫停顯示節拍 %.1f 秒（幀 %llu）", dur, (unsigned long long)atomic_load(&g_cur_frame)); }
        else if (st == 2 && now - t0 >= at + dur) { st = 3; mtl_link_pause(0); MLOG("DM_TEST_LINK_STALL：放開顯示節拍（幀 %llu）", (unsigned long long)atomic_load(&g_cur_frame)); }
    }
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_in - g_pq_done >= PQ) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // 隊滿才等；正常由 AcquireTexture 先擋住 | Wait only when the queue is full; normally AcquireTexture blocks first
    pq_item *it = &g_pq[g_pq_in % PQ]; *it = (pq_item){*t, period, atomic_load(&g_cur_frame), {0}, CACurrentMediaTime()};
    if (crop) memcpy(it->crop, crop, sizeof it->crop);   // 當幀狀態隨票號入隊，不在出圖線程晚讀可變window | The current frame's state is enqueued with the ticket number, rather than reading the mutable window late on the present thread
    uint64_t tk = ++g_pq_in;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return tk;
}
// 丟棄期間不等：標誌置上之後才入隊的那一幀（出圖線程正好走過了標誌檢查）沒人取，等它就又卡住；它留給隨後的 mtl_drain 清
// 節拍模式下不無限等：見 STALL_NS 處的說明。只丟「還沒被節拍取走」的幀，已在上屏途中的那張等它交給 GPU（同進後台的丟法）
// Do not wait during discard: the frame enqueued only after the flag was set (the present thread had just passed the flag check) has nobody to take it, and waiting for it would stall again; it is left for the following mtl_drain to clear
// Do not wait indefinitely in display-link mode: see the note at STALL_NS. Only frames "not yet taken by the display link" are dropped; the one already on its way to the screen is waited for until handed to the GPU (same dropping as when entering the background)
void mtl_present_wait(uint64_t tk) {
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_done < tk && !atomic_load(&g_present_discard)) {
        if (!g_link_owns_layer) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }
        if (g_pq_read >= tk) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }   // 這張已被節拍取走、正在上屏：等它交給 GPU（GPU 完成回調會叫醒） | This one has already been taken by the display link and is being presented: wait for it to be handed to the GPU (the GPU completion callback will wake us)
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)STALL_NS; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        // 已認定在停頓裡（節拍回來時回調先清 g_stall_t0、再記節拍時刻）：不再每幀白等 100 ms，直接丟，遊戲按自己的節拍等待超時（約兩拍）走
        // Already determined to be in a stall (when the display link comes back the callback first clears g_stall_t0, then records the display-link tick time): no longer wait 100 ms in vain every frame, drop directly, and the game proceeds by its own display-link wait timeout (about two ticks)
        if (!g_stall_t0 && (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT || g_pq_done >= tk)) continue;
        uint64_t now = now_ns(), last = __atomic_load_n(&g_link_tick_ns, __ATOMIC_RELAXED);
        if (g_pq_read >= tk) continue;
        if (now - last < STALL_NS) { pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts); continue; }   // 節拍還在走，只是這張還沒輪到：接著等（有上限，不空轉） | The display link is still running, it is just not this one's turn yet: keep waiting (bounded, no busy spinning)
        uint64_t n = tk - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= tk; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = tk;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < tk) g_pq_done = tk;
        pthread_cond_broadcast(&g_pq_cv);
        if (!g_stall_t0) { g_stall_t0 = last ? last : now; g_stall_drop = 0; g_stall_red = 0; g_stall_logged = 0; g_stall_n++; }
        g_stall_drop += n;
        int active = dm_focus_now() == 1; uint64_t since = dm_focus_in_since(), from = g_stall_t0 > since ? g_stall_t0 : since;
        if (active && !g_stall_logged && now - from >= STALL_NS) { g_stall_logged = 1;
            MLOG("顯示節拍 %.0f ms 沒回調（激活中）：丟掉排隊未上屏的 %llu 幀，遊戲繼續（第 %llu 段；節拍回來自動照常）", (now - from) / 1e6, (unsigned long long)n, (unsigned long long)g_stall_n); }
        if (active && !g_stall_red && now - from >= STALL_RED_NS) { g_stall_red = 1; MLOG("【紅】顯示節拍激活中連續 %.1f 秒沒回調，出圖全靠兜底丟幀，畫面不動", (now - from) / 1e9); }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
// 等已入隊的出圖全部處理完（mtl_finish = nvnQueueFinish 等用）：走 mtl_present_wait，節拍停了一樣有兜底（真機：停節拍期間遊戲調了一次 QueueFinish，
// 原先這裡不帶超時地等，排隊的 3 幀沒人取，遊戲停住；Mac 上那兩局沒撞到這個調用）
// Wait until all queued presents have been processed (used by mtl_finish = nvnQueueFinish etc.): goes through mtl_present_wait, so there is still a fallback when the display link stops (real device: the game called QueueFinish once while the display link was stopped,
// and this used to wait here without a timeout; nobody took the 3 queued frames and the game stalled; those two runs on Mac did not hit this call)
void mtl_present_flush(void) { pthread_mutex_lock(&g_pq_mu); uint64_t h = g_pq_in; pthread_mutex_unlock(&g_pq_mu); mtl_present_wait(h); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_pass; *draws = g_stat_draw; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = g_stat_args_enc; *reuse = g_stat_args_reuse; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = 0; *skip = 0; }
uint64_t mtl_ds_omit(void) { return g_stat_ds_omit; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = g_bw_load; *store = g_bw_store; *full = g_pass_full_rw; }
uint64_t mtl_udc_saved_bytes(void) { return g_udc_saved_bytes; }
int mtl_has_window(void) { return front_metal_layer() != NULL; }
int mtl_link_active(void) { return link_on(); }
void mtl_link_pause(int on) { CAMetalDisplayLink *l = g_link; if (l) l.paused = on != 0; }   // 測試用（DM_TEST_BG）：模仿系統在後台停掉節拍回調 | For testing (DM_TEST_BG): imitates the system stopping the display-link callback in the background
int mtl_link_wait_tick(uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    // 只在跑得比顯示快時才等節拍：上一幀開工之後若已經過了節拍（這一幀已落後），立刻開工，不再白等到下一個節拍——
    // 否則幀時間略大於一個節拍時會「錯過節拍 → 再等一整拍」，幀率直接鎖到一半（Mac 有窗口負載下實測成串掉幀）
    // Wait for the display link only when running faster than the display: if a display-link tick has already passed since the previous frame started work (this frame is already behind), start immediately instead of waiting in vain for the next display-link tick --
    // otherwise when the frame time is slightly more than one display-link period it would "miss the tick -> wait another whole tick", locking the frame rate straight to half (measured on Mac under windowed load: runs of dropped frames)
    static uint64_t started_tick;   // 上一幀開工時的節拍號（只有遊戲出圖線程調本函數） | Display-link tick number when the previous frame started work (only the game's present thread calls this function)
    const uint64_t step = g_dm_present_link == 2 ? 2 : 1;   // 一幀佔幾拍：60 Hz 節拍 1 拍；120 Hz 節拍 2 拍（遊戲仍按 60 幀出） | How many display-link ticks one frame takes: 1 with a 60 Hz display link; 2 with a 120 Hz display link (the game still outputs 60 frames)
    pthread_mutex_lock(&g_link_mu);
    if (g_link_tick_n >= started_tick + step) { started_tick = g_link_tick_n; pthread_mutex_unlock(&g_link_mu); return 1; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000ull; ts.tv_nsec += timeout_ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_link_tick_n < started_tick + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    started_tick = g_link_tick_n;
    pthread_mutex_unlock(&g_link_mu); return 1;
}

static id<CAMetalDrawable> g_prefetched_drawable; // 合成直畫 drawable 用 | Used for compositing straight onto the drawable
id<CAMetalDrawable> mtl_present_take_prefetched(void) { return g_prefetched_drawable; }

#ifdef NVN_RT_RANGE_TEST
#include "rt_range_check.inc"
#endif

// 看門狗（無進展）時調用：把圖形側所有阻塞等待點的狀態寫進日誌——誰在等誰一眼可見（換 Metal 4 提交模型後出現過一次「跑到 900 幀後停住、無紅行」）
// Called by the watchdog (no progress): writes the state of all blocking wait points on the graphics side to the log -- who is waiting for whom is visible at a glance (after switching to the Metal 4 submit model there was once a case of "ran to 900 frames then stopped, no failure line")
void mtl_wait_state_dump(void) {
    MLOG("等待點狀態：主柵欄 已提交 %llu / 已完成 %llu；主分配器柵欄 [%llu %llu %llu]；上傳 未完成 %d、分配器佔用 [%d %d %d]、序號 %u、有未衝刷批次 %d；出圖隊列 入 %llu / 完成 %llu；主命令緩衝在編 %d、提交線程已設 %d",
         (unsigned long long)g_fence, (unsigned long long)g_done, (unsigned long long)g_alloc_fence[0], (unsigned long long)g_alloc_fence[1], (unsigned long long)g_alloc_fence[2],
         atomic_load(&g_up_pending), atomic_load(&g_up_alloc_busy[0]), atomic_load(&g_up_alloc_busy[1]), atomic_load(&g_up_alloc_busy[2]), g_up_seq, g_up_cb != nil,
         (unsigned long long)g_pq_in, (unsigned long long)g_pq_done, g_cb != nil, atomic_load(&g_sub_thread_set));
}
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = g_alloc_waits; *grown = g_alloc_grown; *n = g_alloc_n; }   // 主提交分配器池：等過幾次、擴過幾次、現有幾個 | Main submit allocator pool: how many times it waited, how many times it grew, how many exist now
#pragma clang attribute pop
