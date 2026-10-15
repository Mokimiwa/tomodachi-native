// nn::audio AudioRenderer（audren）——進程內渲染：用遊戲 SDK 自帶的整套渲染器，不重寫混音。
// 較新的 SDK 把渲染器服務端（nn::audio::server：InfoUpdater / CommandGenerator …）和 DSP 命令處理的 CPU 版（nn::audio::dsp::CommandListProcessor：
// ADPCM / PCM 解碼、重採樣、混音、混響 / 延遲 / 雙二階 / 壓限 …）都鏈進了應用，供「CPU 渲染 + 手動執行」模式用（AudioRendererParameter
// +0x2c renderingDevice = 1 CPU，+0x30 executionMode = 1 手動）。這個模式下 OpenAudioRenderer 走進程內的 DFC 管理器（CreateAudioRendererManagerByDfc），
// RequestUpdate 直接在進程內解析，渲染由調用 nn::audio::ExecuteAudioRendererRendering 的線程在 CPU 上做，做完觸發遊戲的渲染器事件。
// 宿主做三件事：
//   1. 接管 GetAudioRendererWorkBufferSize / OpenAudioRenderer：參數改成 CPU + 手動後調 SDK 原函數（工作區大小按改後的參數算）；
//   2. 渲染線程（宿主起的客體線程）：輸出端每放回一塊 5 ms 緩衝就渲一幀，節奏跟著聲卡時鐘走；
//   3. 接管 AudioRendererImpl::ExecuteAudioRendererRendering（經 PLT 調到）：照原函數生成命令、交 CommandListProcessor 處理，
//      再從命令表裡找 DeviceSink 命令（CPU 處理器對它是空操作），把它的輸入混音緩衝（int32，16 位範圍）夾成 int16 立體聲交給 AudioOut 輸出（hle_audio.c）。
// 命令表格式（SDK 16）：頭 +0x04 命令數、+0x08 混音緩衝地址、+0x10 緩衝數、+0x14 每幀採樣數、+0x18 採樣率，命令從 +0x20 起；
//   每條命令 +0x00 魔數 0xcafebabe、+0x04 啓用、+0x05 命令號（DeviceSink = 0x13）、+0x06 大小；DeviceSink +0x128 上採樣緩衝、+0x130 輸入數、+0x134 輸入下標（s16）。
// nn::audio AudioRenderer (audren) -- in-process rendering: use the complete renderer the game's SDK ships with, no mixer of our own.
// Newer SDKs link the renderer server side (nn::audio::server: InfoUpdater / CommandGenerator ...) and the CPU version of the DSP command processing (nn::audio::dsp::CommandListProcessor:
// ADPCM / PCM decoding, resampling, mixing, reverb / delay / biquad / limiter / compressor ...) into the application, for the "CPU rendering + manual execution" mode (AudioRendererParameter
// +0x2c renderingDevice = 1 CPU, +0x30 executionMode = 1 manual). In this mode OpenAudioRenderer goes through the in-process DFC manager (CreateAudioRendererManagerByDfc),
// RequestUpdate is parsed in-process, and rendering happens on the CPU of whichever thread calls nn::audio::ExecuteAudioRendererRendering, which then signals the game's renderer event.
// The host does three things:
//   1. Takes over GetAudioRendererWorkBufferSize / OpenAudioRenderer: switches the parameter to CPU + manual and calls the SDK original (the work buffer size follows the modified parameter);
//   2. A render thread (a guest thread started by the host): renders one frame whenever the output hands back a 5 ms buffer, so pacing follows the sound card clock;
//   3. Takes over AudioRendererImpl::ExecuteAudioRendererRendering (reached through the PLT): generates commands and runs CommandListProcessor like the original,
//      then finds the DeviceSink command in the command list (a no-op for the CPU processor) and clamps its input mix buffers (int32 holding 16-bit range) to int16 stereo for AudioOut (hle_audio.c).
// Command list format (SDK 16): header +0x04 command count, +0x08 mix buffer address, +0x10 buffer count, +0x14 samples per frame, +0x18 sample rate, commands from +0x20;
//   each command +0x00 magic 0xcafebabe, +0x04 enabled, +0x05 command ID (DeviceSink = 0x13), +0x06 size; DeviceSink +0x128 upsampler buffer, +0x130 input count, +0x134 input indices (s16).
#include "hle.h"
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
void *dm_pcmout_open(int ch);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);

enum { P_DEVICE = 0x2c, P_MODE = 0x30, P_SIZE = 0x38, FRAME_MAX = 1024, NBUF = 4 };
static void *gsym(const char *n) { void *p = guest_sym(n); if (!p) die("audren：缺 SDK 符號 %s", n); return p; }
static void cpu_manual(uint8_t *dst, const uint8_t *src) { memcpy(dst, src, P_SIZE); *(int32_t *)(dst + P_DEVICE) = 1; *(int32_t *)(dst + P_MODE) = 1; }

HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "渲染器工作區大小：按 CPU 渲染 + 手動執行的參數算",
          size_t, h_ar_WorkSize, const uint8_t *param) {
    static size_t (*orig)(const uint8_t *); if (!orig) orig = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
    uint8_t p[P_SIZE]; cpu_manual(p, param); return orig(p);
}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm", "開渲染器：改成 CPU 渲染 + 手動執行後走 SDK 進程內渲染器",
          Result, h_ar_Open0, void **handle, const uint8_t *param, void *work, size_t sz) {
    static Result (*orig)(void **, const uint8_t *, void *, size_t);
    if (!orig) orig = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm");
    uint8_t p[P_SIZE]; cpu_manual(p, param); return orig(handle, p, work, sz);
}

// ---------- 渲染與輸出 | rendering and output ----------
static void *g_handle[2];                    // AudioRendererHandle（渲染器對象、客戶端狀態）| AudioRendererHandle (renderer object, client state)
static _Atomic int g_open;
static int16_t g_frame[FRAME_MAX * 2]; static int g_frame_n;   // 最近一幀（立體聲 int16）| the latest frame (stereo int16)
static _Atomic uint64_t g_frames, g_nonsilent;
enum { CQ = 4 }; static int16_t g_cq[CQ][FRAME_MAX * 2]; static int g_cq_n[CQ]; static _Atomic uint32_t g_cq_head, g_cq_tail;   // 追幀的輸出隊列 | output queue of catch-up frames

typedef struct { void *ptr; uint64_t cap, size; } gbuf;       // 與 hle_audio.c 的 AudioOutBuffer 前部一致 | same as the front of hle_audio.c's AudioOutBuffer
static void render_main(void *arg) {
    (void)arg;
    void (*exec)(void *, void *) = gsym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE");
    void *out = dm_pcmout_open(2);
    static int16_t data[NBUF][FRAME_MAX * 2]; static gbuf bufs[NBUF]; gbuf *free_list[NBUF]; int nfree = NBUF;
    for (int i = 0; i < NBUF; i++) { bufs[i] = (gbuf){data[i], sizeof data[i], 0}; free_list[i] = &bufs[i]; }
    dm_pcmout_start(out);
    LOG("audren：渲染線程開跑（%d 塊緩衝在途，節奏跟輸出端）", NBUF);
    while (atomic_load(&g_open)) {
        while (!nfree) {   // 等輸出端放回一塊（靜音接收端 / 聲卡每 5 ms 一塊）| wait for the output to hand a block back (silent sink / sound card, one every 5 ms)
            gbuf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            usleep(1000);
            if (!atomic_load(&g_open)) return;
        }
        const int16_t *src; int got;
        uint32_t h = atomic_load(&g_cq_head);
        if (h != atomic_load(&g_cq_tail)) { src = g_cq[h % CQ]; got = g_cq_n[h % CQ]; atomic_store(&g_cq_head, h + 1); }   // 先放追幀的輸出 | play queued catch-up output first
        else { g_frame_n = 0; exec(g_handle[0], g_handle[1]); src = g_frame; got = g_frame_n; }   // SDK：生成命令 → CPU 處理（宿主接管處取出輸出）→ 觸發遊戲的渲染器事件 | SDK: generate commands → CPU processing (output taken in the host takeover) → signal the game's renderer event
        int n = got ? got : 240;
        gbuf *b = free_list[--nfree];
        if (got) memcpy(b->ptr, src, (size_t)n * 4); else memset(b->ptr, 0, (size_t)n * 4);
        b->size = (uint64_t)n * 4;
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
    }
}

HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "開渲染器（帶事件）：CPU 渲染 + 手動執行，宿主起渲染線程",
          Result, h_ar_Open, void **handle, void *ev, const uint8_t *param, void *work, size_t sz) {
    static Result (*orig)(void **, void *, const uint8_t *, void *, size_t);
    if (!orig) orig = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    uint8_t p[P_SIZE]; cpu_manual(p, param);
    LOG("audren：OpenAudioRenderer 採樣率 %d 每幀 %d 混音緩衝 %d 子混音 %d 聲部 %d 輸出 %d 效果 %d 分路 %d（原設備 %d 模式 %d → CPU / 手動）",
        *(int32_t *)param, *(int32_t *)(param + 4), *(int32_t *)(param + 8), *(int32_t *)(param + 0xc), *(int32_t *)(param + 0x10), *(int32_t *)(param + 0x14),
        *(int32_t *)(param + 0x18), *(int32_t *)(param + 0x24), *(int32_t *)(param + P_DEVICE), *(int32_t *)(param + P_MODE));
    Result r = orig(handle, ev, p, work, sz);
    if (r) { LOG("【紅】audren：SDK OpenAudioRenderer 失敗 %#x", r); return r; }
    if (atomic_load(&g_open)) { LOG("【紅】audren：第二個渲染器（宿主只驅動一個）"); return r; }
    g_handle[0] = handle[0]; g_handle[1] = handle[1]; atomic_store(&g_open, 1);
    dm_guest_thread_spawn(render_main, NULL, 9, 0x10000);
    return r;
}
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "關渲染器：先停宿主渲染線程再交 SDK", void, h_ar_Close, void *obj, void *state) {
    static void (*orig)(void *, void *); if (!orig) orig = gsym("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE");
    if (obj == g_handle[0]) { atomic_store(&g_open, 0); usleep(20000); }   // 渲染線程每輪查一次，最多等一幀多 | the render thread checks every round; wait a little over one frame
    orig(obj, state);
}

// 取 DeviceSink 的輸入混音緩衝 → int16 立體聲 | DeviceSink input mix buffers → int16 stereo
// sink = DeviceSink 的命令號：SDK 16 是 0x13；SDK 20 在它前面多了一種命令，往後挪成 0x14（朋友收集首幀 9 條：清混音 25、去爆音 15、音量 7×6、DeviceSink 20）
// sink = the DeviceSink command ID: 0x13 on SDK 16; SDK 20 has one more command type before it, moving it to 0x14 (Tomodachi Life's first frame, 9 commands: clear mix 25, depop 15, volume 7×6, DeviceSink 20)
static int capture(const uint8_t *cl, int16_t *dst, int sink) {
    uint32_t ncmd = *(const uint32_t *)(cl + 4), samples = *(const uint32_t *)(cl + 0x14), rate = *(const uint32_t *)(cl + 0x18);
    const int32_t *mix = *(const int32_t *const *)(cl + 8);
    static int logged, hist_logged;
    if (!hist_logged && ncmd > 4) {   // 第一幀完整命令表：命令號分布 | first full command list: command ID histogram
        hist_logged = 1;
        int hist[64] = {0}; const uint8_t *q = cl + 0x20; char line[512]; size_t o = 0;
        for (uint32_t i = 0; i < ncmd && *(const uint32_t *)q == 0xcafebabeu && *(const int16_t *)(q + 6) > 0; i++) { hist[q[5] & 63]++; q += *(const int16_t *)(q + 6); }
        for (int k = 0; k < 64; k++) if (hist[k]) o += snprintf(line + o, sizeof line - o, " %d×%d", k, hist[k]);
        LOG("audren：首幀命令 %u 條，按命令號：%s", ncmd, line);
    }
    const uint8_t *c = cl + 0x20;
    for (uint32_t i = 0; i < ncmd; i++) {
        uint32_t magic = *(const uint32_t *)c; int16_t size = *(const int16_t *)(c + 6);
        if (magic != 0xcafebabeu || size <= 0) { if (!logged++) LOG("【紅】audren：命令表第 %u 條格式不對（魔數 %#x 大小 %d）", i, magic, size); return 0; }
        if (c[5] == sink && c[4]) {
            static int dumped; if (!dumped++) {   // 頭一回：記下這條命令的原始字節，核對字段位置 | first time: log this command's raw bytes to check field offsets
                char hx[3 * 0x40 + 1]; for (int o = 0x100; o < 0x140 && o < size; o += 0x40) { size_t m = 0; for (int k = 0; k < 0x40 && o + k < size; k++) m += (size_t)snprintf(hx + m, sizeof hx - m, "%02x", c[o + k]); LOG("audren：DeviceSink 命令 大小 %d 字節 +%#x：%s", size, o, hx); } }
            uint32_t nin = *(const uint32_t *)(c + 0x130); const int16_t *in = (const int16_t *)(c + 0x134);
            const int32_t *up = *(const int32_t *const *)(c + 0x128);
            if (!logged++) LOG("audren：DeviceSink 輸入 %u 個（%d %d …）採樣率 %u 每幀 %u 混音緩衝 %p 上採樣緩衝 %p", nin, nin ? in[0] : -1, nin > 1 ? in[1] : -1, rate, samples, (void *)mix, (void *)up);
            if (!nin || samples > FRAME_MAX) return 0;
            // 輸入緩衝：48 kHz 讀混音緩衝；32 kHz 渲染時 DeviceSink 讀上採樣後的緩衝（按輸入順序連排）
            // Input buffers: at 48 kHz the mix buffers; with 32 kHz rendering DeviceSink reads the upsampled buffer (inputs laid out in order)
            const int32_t *ch[6]; uint32_t k = nin > 6 ? 6 : nin;
            for (uint32_t j = 0; j < k; j++) ch[j] = up && rate != 48000 ? up + (size_t)j * samples : mix + (size_t)in[j] * samples;
            // 聲道順序 前左 前右 中 低音 後左 後右；6 → 2 按系統缺省下混系數（前 1.0、中 0.707、低音 0.251、後 0.707，Q15）
            // Channel order front-left front-right center LFE back-left back-right; 6 → 2 with the system default downmix coefficients (front 1.0, center 0.707, LFE 0.251, back 0.707, Q15)
            enum { CF = 32768, CC = 23167, CL = 8224, CB = 23167 };
            uint64_t loud = 0;
            for (uint32_t s = 0; s < samples; s++) {
                int32_t l, r;
                if (k >= 6) { int64_t c = (int64_t)ch[2][s] * CC + (int64_t)ch[3][s] * CL;
                              l = (int32_t)(((int64_t)ch[0][s] * CF + c + (int64_t)ch[4][s] * CB) >> 15); r = (int32_t)(((int64_t)ch[1][s] * CF + c + (int64_t)ch[5][s] * CB) >> 15); }
                else { l = ch[0][s]; r = k > 1 ? ch[1][s] : l; }
                l = l > 32767 ? 32767 : l < -32768 ? -32768 : l; r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
                dst[2 * s] = (int16_t)l; dst[2 * s + 1] = (int16_t)r; loud |= (uint64_t)(l | r);
            }
            atomic_fetch_add(&g_frames, 1); if (loud) atomic_fetch_add(&g_nonsilent, 1);
            uint64_t f = atomic_load(&g_frames); if (f == 200 || f % 12000 == 0) LOG("audren：已渲 %llu 幀，其中有聲 %llu 幀", (unsigned long long)f, (unsigned long long)atomic_load(&g_nonsilent));
            return (int)samples;
        }
        c += size;
    }
    return 0;
}
// 渲染一幀（生成命令 + CPU 處理）；dst = 輸出（立體聲 int16，*nout 採樣數）。兩處調用（渲染線程、追幀）由宿主鎖串行：共用同一塊命令緩衝
// Render one frame (generate commands + CPU processing); dst = output (stereo int16, *nout samples). The two callers (render thread, catch-up) are serialized by a host lock: they share one command buffer
static pthread_mutex_t g_render_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_updates, g_renders, g_catchups;
static int sdk20_mode(void) {
    static int v = -1;
    if (v < 0) { v = !guest_sym("_ZNK2nn5audio6server17AudioRenderSystem16GetExecutionModeEv"); if (v) LOG("audren：SDK 20 渲染器，由 SDK 自己渲，宿主從命令表取 DeviceSink 輸出"); }
    return v;
}
// SDK 20 的 AudioRenderSystem::ExecuteAudioRendererRendering（sdk 20.5.3 0x229bdc）：+0x48 / +0x4c 查執行模式與渲染設備，命令緩衝工作區取自
// +0xf8（地址）/ +0x100（大小），GenerateCommand 把命令表寫在工作區開頭，再交 CommandListProcessor 處理。渲完從同一處讀命令表，按 SDK 16 的格式取 DeviceSink 輸出；
// 格式對不上時 capture 記紅字並給 0 個採樣，這裡退回靜音
// SDK 20's AudioRenderSystem::ExecuteAudioRendererRendering (sdk 20.5.3 0x229bdc): +0x48 / +0x4c check the execution mode and rendering device, the command buffer work area comes from
// +0xf8 (address) / +0x100 (size), GenerateCommand writes the command list at the start of the work area, and CommandListProcessor processes it. After rendering, read the command list from the same place and take the DeviceSink output in the SDK 16 format;
// if the format does not match, capture logs a red line and yields 0 samples, and this falls back to silence
enum { SYS20_CMDBUF = 0xf8 };
static Result sdk20_exec(uint8_t *sys, int16_t *dst, int *nout) {
    static Result (*orig)(void *); if (!orig) orig = gsym("_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv");
    pthread_mutex_lock(&g_render_mu);
    Result r = orig(sys); atomic_fetch_add(&g_renders, 1);
    int n = 0; const uint8_t *cl = *(const uint8_t *const *)(sys + SYS20_CMDBUF);
    if (dst && !r && cl) n = capture(cl, dst, 0x14);
    pthread_mutex_unlock(&g_render_mu);
    if (dst) { if (!n) { memset(dst, 0, 240 * 2 * sizeof *dst); n = 240; } *nout = n; }
    return r;
}
static Result render_once(uint8_t *sys, int16_t *dst, int *nout) {
    static int (*mode)(void *), (*device)(void *); static uint8_t *(*cbuf)(void *); static size_t (*cbsz)(void *), (*gen)(void *, void *, size_t);
    static void (*tmax)(void *, uint32_t), (*setup)(void *, const void *, size_t), (*process)(void *), (*lock)(void *), (*unlock)(void *);
    // SDK 20（朋友收集）：AudioRenderSystem 自己的 ExecuteAudioRendererRendering 在 CPU 模式下就把「生成命令 + CPU 處理」整套在進程裡做完（sdk 20.5.3 0x229bdc），
    // 而上面用的幾個取值函數已被內聯掉、GenerateCommand 改收 Span。所以 SDK 20 上直接調它渲一幀（追幀也這樣，保證效果初始化：缺了這一幀
    // 燈光限幅器的狀態沒建，ApplyLightLimiterEffect 讀空指針，同 docs/agents/03）。輸出從它留下的命令表取（sdk20_exec）
    // SDK 20 (Tomodachi Life): AudioRenderSystem's own ExecuteAudioRendererRendering does the whole "generate commands + CPU processing" in-process in CPU mode (sdk 20.5.3 0x229bdc),
    // while the getters used above were inlined and GenerateCommand now takes a Span. So on SDK 20 it is called directly to render a frame (catch-up frames too, so effects get initialized: without that frame
    // the light limiter's state is never built and ApplyLightLimiterEffect reads a null pointer, as in docs/agents/03). The output is taken from the command list it leaves behind (sdk20_exec)
    if (sdk20_mode()) return sdk20_exec(sys, dst, nout);
    if (!process) {
        lock = gsym("_ZN2nn2os12SdkMutexType4LockEv"); unlock = gsym("_ZN2nn2os12SdkMutexType6UnlockEv");
        mode = gsym("_ZNK2nn5audio6server17AudioRenderSystem16GetExecutionModeEv"); device = gsym("_ZNK2nn5audio6server17AudioRenderSystem18GetRenderingDeviceEv");
        cbuf = gsym("_ZNK2nn5audio6server17AudioRenderSystem26GetCommandBufferWorkBufferEv"); cbsz = gsym("_ZNK2nn5audio6server17AudioRenderSystem30GetCommandBufferWorkBufferSizeEv");
        gen = gsym("_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandEPvm");
        tmax = gsym("_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj"); setup = gsym("_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm");
        process = gsym("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    }
    if (mode(sys) != 1 || device(sys) != 1) return 0x40299;   // 同原函數：非 CPU 手動模式不能手動渲染 | same as the original: manual rendering needs CPU + manual mode
    pthread_mutex_lock(&g_render_mu);
    uint64_t t0 = now_ns();
    // 生成命令要與遊戲線程的 RequestUpdate（AudioRenderSystem::Update，持 +0x1f0 的互斥鎖）互斥：原版的系統線程（SendCommandToDsp）也是持同一把鎖生成。處理（CPU 版 DSP）照原版在鎖外。
    // Command generation must exclude the game thread's RequestUpdate (AudioRenderSystem::Update, which holds the mutex at +0x1f0): the original system thread (SendCommandToDsp) generates under the same lock. Processing (the CPU DSP) stays outside the lock, as in the original.
    uint8_t *cl = cbuf(sys);
    lock(sys + 0x1f0); size_t n = gen(sys, cl, cbsz(sys)); unlock(sys + 0x1f0);
    atomic_fetch_add(&g_renders, 1);
    uint64_t proc[16] __attribute__((aligned(16))) = {0};   // CommandListProcessor（0x58 字節）| CommandListProcessor (0x58 bytes)
    tmax(proc, 0xffffffffu); setup(proc, cl, n); process(proc);
    if (dst) *nout = capture(cl, dst, 0x13);
    // 每幀渲染耗時（5 ms 一幀的預算）：每 2000 幀報均值 / 最大 | render time per frame (budget: 5 ms per frame): mean / max every 2000 frames
    static uint64_t sum, mx, cnt; uint64_t d = now_ns() - t0; sum += d; if (d > mx) mx = d;
    if (++cnt == 2000) { LOG("audren：渲染耗時 均 %.2f ms / 最大 %.2f ms（每幀 5 ms 預算，最近 2000 幀）；追幀累計 %llu 次", sum / 2000 / 1e6, mx / 1e6, (unsigned long long)atomic_load(&g_catchups)); sum = mx = cnt = 0; }
    pthread_mutex_unlock(&g_render_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio6server24AudioRendererManagerImpl17AudioRendererImpl29ExecuteAudioRendererRenderingEv", "手動渲染一幀：SDK 生成命令 + CPU 處理，宿主取 DeviceSink 輸出",
          Result, h_ar_ImplExec, uint8_t *impl) { return render_once(impl + 0x60, g_frame, &g_frame_n); }   // AudioRendererImpl 裡的 AudioRenderSystem 在 +0x60 | the AudioRenderSystem sits at +0x60 inside AudioRendererImpl

// 兩次 RequestUpdate 之間至少渲一幀。效果的「待初始化」只在遊戲標「新加」的那次更新裡置上（如 LightLimiterInfo::Update：isNew → 參數狀態 0），
// 下一次更新照遊戲的值覆蓋；中間沒生成過命令，DSP 就永遠收不到初始化，之後 ApplyLightLimiterEffect 讀空指針。真機渲染器每 5 ms 自己出一幀、
// 遊戲又等渲染器事件，正常是一更新一幀；但開局遊戲連著更新兩次（相隔 0.04 ms），真出聲時聲卡打開慢、第一幀來得晚，兩次之間沒幀。
// 這裡遇到「上次更新後沒渲過」就先在當前線程追渲一幀；輸出進隊列，渲染線程下一輪放它、不再另渲。
// At least one rendered frame between two RequestUpdates. An effect's "needs initialization" is set only by the update in which the game flags it as new (e.g. LightLimiterInfo::Update: isNew → parameter status 0);
// the next update overwrites it with the game's value; if no commands were generated in between, the DSP never receives the initialization and ApplyLightLimiterEffect later reads a null pointer. The real renderer produces a frame every 5 ms on its own
// and the game waits on the renderer event, so normally it is one update per frame; but at start-up the game updates twice in a row (0.04 ms apart), and with real audio output the sound card opens slowly and the first frame comes late, leaving no frame in between.
// Here, when nothing has been rendered since the last update, one catch-up frame is rendered on the current thread first; its output is queued and played by the render thread in place of its next render.
// SDK 20：渲染線程經 nn::audio::ExecuteAudioRendererRendering 直達這裡（沒有 AudioRendererImpl 那一層）；計幀、與追幀互斥，從命令表取 DeviceSink 輸出
// SDK 20: the render thread reaches here straight from nn::audio::ExecuteAudioRendererRendering (there is no AudioRendererImpl layer); count the frame, exclude catch-ups, take the DeviceSink output from the command list
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv", "SDK 20 渲染一幀：SDK 自己生成 + CPU 處理，宿主從命令表取 DeviceSink 輸出",
          Result, h_ars_Exec, uint8_t *sys) {
    return sdk20_exec(sys, g_frame, &g_frame_n);
}
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", "渲染器更新：上次更新後沒渲過就先追渲一幀（保證效果初始化），再交 SDK",
          Result, h_ars_Update, uint8_t *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    static Result (*orig)(void *, void *, size_t, void *, size_t, const void *, size_t); if (!orig) orig = gsym("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    static uint64_t last_r = ~0ull;
    uint64_t u = atomic_fetch_add(&g_updates, 1) + 1, r = atomic_load(&g_renders);
    if (last_r != ~0ull && r == last_r) {
        uint64_t c = atomic_fetch_add(&g_catchups, 1) + 1;
        if (c <= 3) LOG("audren：第 %llu 次更新前沒渲過幀，先追渲一幀（累計 %llu 次）", (unsigned long long)u, (unsigned long long)c);
        uint32_t t = atomic_load(&g_cq_tail);
        if (t - atomic_load(&g_cq_head) < CQ) { render_once(sys, g_cq[t % CQ], &g_cq_n[t % CQ]); atomic_store(&g_cq_tail, t + 1); }
        else render_once(sys, NULL, NULL);   // 隊列滿（不該發生）：照渲，輸出丟棄 | queue full (should not happen): render anyway, output discarded
    }
    static int tr = -1; if (tr < 0) tr = getenv("DM_AUDREN_TRACE") != NULL;
    if (tr && u < 200) LOG("audren 更新 #%llu t=%.3f ms 距上次更新渲了 %llu 幀", (unsigned long long)u, now_ns() / 1e6, (unsigned long long)(atomic_load(&g_renders) - (last_r == ~0ull ? 0 : last_r)));
    Result res = orig(sys, o, on, p, pn, in, inn);
    last_r = atomic_load(&g_renders);
    return res;
}

// 輸出設備：一個內置揚聲器，立體聲（音頻設備服務仍走 IPC，宿主直接答）
// Output device: one built-in speaker, stereo (the audio device service still goes through IPC; the host answers directly)
enum { DEVNAME = 0x100 };
static void out_devname(char *out) { memset(out, 0, DEVNAME); strlcpy(out, "AudioBuiltInSpeakerOutput", DEVNAME); }
HLE_FORCE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", "輸出設備列表：內置揚聲器 1 個", int, h_ad_List, char *names, int n) { if (n <= 0 || !names) return 0; out_devname(names); return 1; }
HLE_FORCE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", "當前輸出設備：內置揚聲器", void, h_ad_Active, char *name) { out_devname(name); }
// SDK 12 的舊名（Engage）：同一服務、同樣回答 | The SDK 12 names (Engage): the same service, the same answers
HLE_FORCE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "輸出設備列表（SDK 12 名）：內置揚聲器 1 個", int, h_ad_List12, char *names, int n) { return h_ad_List(names, n); }
HLE_FORCE("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", "當前輸出設備（SDK 12 名）：內置揚聲器", void, h_ad_Active12, char *name) { out_devname(name); }
HLE_FORCE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", "輸出聲道數：2", int, h_ad_Channels, void) { return 2; }
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "設備音量；宿主忽略", void, h_ad_SetVol, const char *name, float v) { (void)name; (void)v; }
HLE_FORCE("_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", "設備音量查詢（朋友收集）：滿音量 1.0", float, h_ad_GetVol, const char *name) { (void)name; return 1.0f; }
HLE_FORCE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", "輸出設備變化通知：綁定、永不觸發", void, h_ad_Notify, void *ev) { hle_bind_system_event(ev, 0); }
