// nn::audio AudioOut：遊戲（FMOD nx 輸出層）按塊追加 PCM 緩衝，播完由系統放回「已釋放」隊列並觸發事件。
// 宿主：真出聲 = CoreAudio AudioUnit（Mac DefaultOutput / iOS RemoteIO）；靜音接收端 = 按真實時間消耗緩衝（不出聲，節奏與真機一致）。
//       iOS 缺省真出聲（產品必須有聲音），測試要靜音顯式 DM_AUDIO=0；Mac 缺省靜音接收端（回放 / 閘門不出聲），DM_AUDIO=1 真出聲。
// AudioOut / AudioOutBuffer / AudioOutParameter 的內容只經下列接口讀寫，宿主自定內部佈局，只寫結構體前部。
// nn::audio AudioOut: the game (FMOD nx output layer) appends PCM buffers block by block; once played the system puts them back on the "released" queue and signals the event.
// Host: real audio output = CoreAudio AudioUnit (Mac DefaultOutput / iOS RemoteIO); silent sink = consumes buffers in real time (no sound, pacing matches the real device).
//       iOS defaults to real audio output (the product must have sound), tests that want silence set DM_AUDIO=0 explicitly; Mac defaults to the silent sink (replay / gate checks are silent), DM_AUDIO=1 for real audio output.
// The contents of AudioOut / AudioOutBuffer / AudioOutParameter are read and written only through the interfaces below; the host defines its own internal layout and only writes the front part of the structs.
#include "hle.h"
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // 寫進客體 AudioOutBuffer 的前 24 字節 | The first 24 bytes written into the guest AudioOutBuffer
#define QMAX 64
#define RELMAX QMAX                    // 已釋放最多 = 在途緩衝數（遊戲收回緩衝才能再追加），與待播同上限 | Released max = number of in-flight buffers (the game must take a buffer back before it can append again), same limit as pending
typedef struct {
    pthread_mutex_t m;
    void *event;                       // 客體 SystemEvent：有緩衝被釋放就觸發 | Guest SystemEvent: signaled whenever a buffer is released
    int ch;
    _Atomic int state;
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // 待播：遊戲側寫，出聲端讀 | Pending: written by the game side, read by the output side
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // 已釋放：出聲端寫，遊戲側讀 | Released: written by the output side, read by the game side
    uint64_t pos;                      // 當前塊已播字節 | Bytes already played of the current block
    AudioUnit au;
    pthread_t null_th; _Atomic int null_run;
    int null_active;                   // 靜音接收端正在消耗這個輸出（改動與讀取都持 g_aos_lock） | the silent sink is currently consuming this output (changed and read under g_aos_lock)
    dispatch_semaphore_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
    int id;                            // 打開序號（日誌裡的 #N；同時開著幾個輸出時分得清，例如 Engage 的 Wwise 與 Unity 自帶的 FMOD） | open sequence number (#N in the log; tells simultaneous outputs apart, e.g. Engage's Wwise and Unity's built-in FMOD)
    int nlog;                          // Start / Stop / Close 已記的行數（每個輸出最多記 32 行） | Start / Stop / Close lines logged so far (at most 32 per output)
    _Atomic uint64_t under_n, under_b; // 欠載：回調要數據時待播隊列已空的次數 / 補零字節 | underruns: times the pending queue was empty when the callback needed data / zero-filled bytes
    uint64_t st_calls, st_qsum, st_u0, st_ub0; uint32_t st_fmin, st_fmax;   // 聲卡回調統計（只在回調線程裡讀寫） | sound card callback statistics (read and written only on the callback thread)
} aout;

// 打開中的輸出登記表：前端生命週期（切後台）一次性暫停 / 恢復
// Registry of open outputs: suspended / resumed all at once by the front-end lifecycle (going to background)
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p 不是宿主建的", g); return a; }
// 調用點寫成「模塊+偏移」；宿主自己調（進程內音頻渲染器）時寫「宿主」 | the call site as "module+offset"; "宿主" (host) when the host itself calls (in-process audio renderer)
static const char *site(void *lr, char *buf, size_t n) {
    module_t *m = lr ? module_of(lr) : NULL;
    if (m) snprintf(buf, n, "%s+%#llx", m->name, (unsigned long long)((uint8_t *)lr - m->base)); else snprintf(buf, n, "宿主");
    return buf;
}
// Start / Stop / Close 每個輸出最多記 32 行：中間件只在初始化、掛起、恢復時調，多了說明在反復開關 | Start / Stop / Close: at most 32 lines per output; middleware calls them only at init, suspend and resume, more means it keeps toggling
static void au_note(aout *a, const char *what, void *lr) {
    char b[64];
    if (a->nlog < 32) { a->nlog++; LOG("audio #%d %s（調用點 %s）%s", a->id, what, site(lr, b, sizeof b), a->nlog == 32 ? "；此後不再記" : ""); }
}

static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        dispatch_semaphore_signal(a->notify_sem);
}

static void *event_notify(void *p) {
    aout *a = p;
    for (;;) {
        dispatch_semaphore_wait(a->notify_sem, DISPATCH_TIME_FOREVER);
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            dispatch_semaphore_signal(a->notify_sem);
    }
    return NULL;
}

// 從待播隊列取 n 字節交給出聲端；整塊播完就挪到已釋放並通知普通線程。
// Take n bytes from the pending queue and hand them to the output side; once a whole block has played, move it to released and notify the ordinary thread.
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) {
            if (qh == qt && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) { atomic_fetch_add(&a->under_n, 1); atomic_fetch_add(&a->under_b, n); }
            memset(dst, 0, n); break; }
        gbuf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut 已釋放隊列溢出");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
        }
    }
    if (released) notify_release(a);
}

static _Atomic uint64_t g_render_calls;   // 聲卡回調累計（生命週期驗證：恢復後須繼續增長） | Cumulative sound card callbacks (lifecycle verification: must keep growing after resume)
uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
static OSStatus au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)f; (void)ts; (void)bus; (void)frames;
    aout *a = ctx;
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed);
    {   // 每個輸出每 2000 次回調記一行：每次要多少幀、回調開始時待播多少塊、欠載次數（換新設備時看這行；docs/agents/03-bringup.md「iOS 上爆音」）。
        // 按輸出分開記：同時開著兩個輸出時合在一起的數會誤導（一個餵滿、一個沒人餵，合起來像「每隔一次欠載」）
        // Every 2000 callbacks of each output, log one line: frames requested per call, pending blocks at callback start, underruns (check this line on a new device; docs/agents/03-bringup.md "Crackling on iOS only").
        // Kept per output: with two outputs open, combined numbers mislead (one fully fed and one fed by nobody look like "every other callback underruns")
        uint32_t qd = atomic_load(&a->q_tail) - atomic_load(&a->q_head);
        if (!a->st_calls || frames < a->st_fmin) a->st_fmin = frames; if (frames > a->st_fmax) a->st_fmax = frames; a->st_qsum += qd;
        if (++a->st_calls % 2000 == 0) { uint64_t un = atomic_load(&a->under_n), ubb = atomic_load(&a->under_b);
            LOG("audio #%d 聲卡回調 %llu 次：每次 %u–%u 幀，回調時待播均 %.1f 塊；這 2000 次裡欠載 %llu 次、補零 %.1f ms", a->id, (unsigned long long)a->st_calls, a->st_fmin, a->st_fmax, a->st_qsum / 2000.0,
                (unsigned long long)(un - a->st_u0), (ubb - a->st_ub0) / (2.0 * a->ch) / (RATE / 1000.0)); a->st_fmin = ~0u; a->st_fmax = 0; a->st_qsum = 0; a->st_u0 = un; a->st_ub0 = ubb; } }
    pull(a, bl->mBuffers[0].mData, bl->mBuffers[0].mDataByteSize);
    { static int mute = -1; if (mute < 0) { const char *e = getenv("DM_AUDIO_MUTE"); mute = e && atoi(e); }   // 診斷：走真出聲的整條路（AudioUnit 回調節奏、切後台接手），但輸出靜音（Mac 上測生命週期不吵人） | diagnostic: the whole real-output path (AudioUnit callback pacing, background take-over) but with the output silenced (lifecycle tests on the Mac without noise)
      if (mute) memset(bl->mBuffers[0].mData, 0, bl->mBuffers[0].mDataByteSize); }
    return noErr;
}
static AudioUnit au_create(aout *a) {
    AudioUnit au = NULL;
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
                                   .componentSubType = kAudioUnitSubType_RemoteIO,
#else
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &au)) { LOG("【紅】AudioUnit 建不出來：繼續由靜音接收端消耗（沒有聲音）"); return NULL; }
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 2u * a->ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * a->ch,
                                     .mChannelsPerFrame = (UInt32)a->ch, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {au_render, a};
#if !TARGET_OS_IPHONE
    { const char *e = getenv("DM_DBG_AU_FRAMES"); if (e) {   // 診斷：Mac 上把聲卡回調改成每次 N 幀（1024 = iOS 缺省，復現爆音） | diagnostic: on the Mac, make the sound card callback ask N frames each time (1024 = the iOS default, reproduces the crackling)
        UInt32 fr = (UInt32)atoi(e); OSStatus st = AudioUnitSetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &fr, sizeof fr);
        LOG("audio 聲卡緩衝幀數設為 %u：狀態 %d", fr, (int)st); } }
#endif
    if (AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(au)) { LOG("【紅】AudioUnit 設置失敗：繼續由靜音接收端消耗（沒有聲音）"); AudioComponentInstanceDispose(au); return NULL; }
    return au;
}
static void null_start(aout *a);
// 真出聲的 AudioUnit 在後台建（CoreAudio 偶爾要很久：一次實測開遊戲後 40 秒才建好，渲染線程一直卡在這裡、整段沒聲）。
// 建好之前由靜音接收端照實時消耗，遊戲的音頻節奏不停；建好後在 g_aos_lock 下切過去（同 dm_audio_suspend 的恢復）。輸出已關（不在登記表裡）就直接丟掉
// The real-output AudioUnit is created in the background (CoreAudio sometimes takes very long: one measured launch took 40 seconds before it was ready, the render thread stayed stuck here and the whole stretch was silent).
// Until it is ready the silent sink consumes in real time, so the game's audio pacing never stops; once ready it is switched in under g_aos_lock (like the resume in dm_audio_suspend). If the output was already closed (no longer registered) it is discarded
static void au_open(aout *a) {
    int id = a->id; uint64_t t0 = now_ns();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        AudioUnit au = au_create(a);
        uint64_t ms = (now_ns() - t0) / 1000000;
        pthread_mutex_lock(&g_aos_lock);
        int live = 0; for (int i = 0; i < AOMAX; i++) live |= g_aos[i] == a && g_aos[i]->id == id;
        if (!live) { pthread_mutex_unlock(&g_aos_lock); if (au) { AudioUnitUninitialize(au); AudioComponentInstanceDispose(au); } return; }
        if (au) {
            a->au = au;
            if (atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED && !g_suspended && a->null_active) {
                atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); a->null_active = 0;
                if (AudioOutputUnitStart(au)) { LOG("【紅】audio #%d AudioUnit 啓動失敗：保持靜音接收端", id); null_start(a); }
            }
        }
        pthread_mutex_unlock(&g_aos_lock);
        if (au) LOG("audio #%d CoreAudio 輸出建好，用時 %llu ms%s", id, (unsigned long long)ms, ms > 1000 ? "（期間由靜音接收端頂著，這段沒聲）" : "");
    });
}
// 靜音接收端：每 5 ms 按 48 kHz 實時消耗。落後超過 100 ms（iOS 在後台把整個進程掛起過、Mac 上 SIGSTOP）就從現在重新起算，不補播漏掉的時間——
// 補的話會一口氣連取幾百次（幾秒的量），把遊戲的緩衝全釋放、再記幾百次欠載；真機掛起時也不播這段
// Silent sink: consumes in real time at 48 kHz every 5 ms. When more than 100 ms behind (iOS suspended the whole process in the background, SIGSTOP on the Mac), restart the count from now and do not make up the missed time --
// making it up would pull hundreds of times in one go (seconds' worth), releasing all of the game's buffers and logging hundreds of underruns; a suspended console does not play that stretch either
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8];
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns();
        if (t > now) usleep((useconds_t)((t - now) / 1000)); else if (now - t > 100000000) t = now;
        pull(a, scratch, bytes);
    }
    return NULL;
}
static void null_start(aout *a) { atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); a->null_active = 1; }   // 調用方持 g_aos_lock | caller holds g_aos_lock
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : TARGET_OS_IPHONE; }

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }，0 = 系統默認（48 kHz / 2 聲道）
// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }, 0 = system default (48 kHz / 2 channels)
HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "音頻輸出參數默認值", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
// AudioOutInfo = { char name[256] }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "音頻輸出設備列表；宿主 1 個", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
static Result au_open_at(void *g, void *ev, const char *name, const uint8_t *param, void *lr) {
    static _Atomic int nopen;
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) die("AudioOut 採樣率 %d 不支持（只做 48 kHz）", rate);
    aout *a = calloc(1, sizeof *a);
    pthread_mutex_init(&a->m, NULL); a->event = ev; a->ch = ch;
    atomic_init(&a->state, STATE_STOPPED);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    a->notify_sem = dispatch_semaphore_create(0); a->id = atomic_fetch_add(&nopen, 1);
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);   // 緩衝釋放事件：客體線程上綁內核事件（自動清除），之後由音頻線程觸發 | Buffer release event: bind a kernel event on the guest thread (auto-clear), afterwards signaled by the audio thread
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("AudioOut 同時打開超過 %d 個", AOMAX);
    g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    char sb[64];
    LOG("audio #%d OpenAudioOut %s 聲道 %d %s%s（調用點 %s）", a->id, name ? name : "(默認)", ch, real_audio() ? "真出聲" : "靜音接收端", ev ? "" : "，無釋放事件（遊戲輪詢）", site(lr, sb, sizeof sb));
    if (real_audio()) au_open(a);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "連 audout 服務；宿主 AudioUnit / 靜音接收端",
          Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) { return au_open_at(g, ev, name, param, __builtin_return_address(0)); }
// 缺省設備的兩個重載（Engage，SDK 12）：同 OpenAudioOut，設備名取缺省；不帶事件的重載不綁釋放事件（遊戲輪詢 GetReleasedAudioOutBuffer）
// The two default-device overloads (Engage, SDK 12): as OpenAudioOut with the default device name; the overload without an event binds no release event (the game polls GetReleasedAudioOutBuffer)
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", "連 audout 服務（缺省設備）；同 OpenAudioOut",
          Result, h_au_OpenDef, void *g, void *ev, const uint8_t *param) { return au_open_at(g, ev, NULL, param, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", "連 audout 服務（缺省設備、無事件）；同 OpenAudioOut",
          Result, h_au_OpenDef0, void *g, const uint8_t *param) { return au_open_at(g, NULL, NULL, param, __builtin_return_address(0)); }
static Result au_start_at(void *g, void *lr) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STARTED, memory_order_release); pthread_mutex_unlock(&a->m);
    au_note(a, was == STATE_STARTED ? "Start（已在播）" : g_suspended ? "Start（App 在後台：先由靜音接收端接手）" : "Start", lr);
    if (was == STATE_STARTED) return RES_OK;
    pthread_mutex_lock(&g_aos_lock);   // 與後台建好 AudioUnit 的切換互斥 | mutually exclusive with the switch when the background AudioUnit becomes ready
    if (a->au && !g_suspended) { if (AudioOutputUnitStart(a->au)) { LOG("【紅】audio #%d AudioUnit 啓動失敗：改由靜音接收端消耗", a->id); null_start(a); } }
    else null_start(a);   // 暫停中，或 AudioUnit 還在後台建 | suspended, or the AudioUnit is still being created in the background
    pthread_mutex_unlock(&g_aos_lock);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "開始播放", Result, h_au_Start, void *g) { return au_start_at(g, __builtin_return_address(0)); }
static void au_stop_at(void *g, void *lr) {   // lr 為 NULL：Close 裡順帶停，不另記一行 | lr NULL: the stop inside Close, not logged separately
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STOPPED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (lr) au_note(a, was == STATE_STARTED ? "Stop" : "Stop（本來就停著）", lr);
    if (was != STATE_STARTED) return;
    pthread_mutex_lock(&g_aos_lock);
    if (a->null_active) { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); a->null_active = 0; }   // 靜音接收端（暫停期間、AudioUnit 未建好時接手的） | Silent sink (taking over during suspension or before the AudioUnit is ready)
    else if (a->au) AudioOutputUnitStop(a->au);
    pthread_mutex_unlock(&g_aos_lock);
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "停止播放", void, h_au_Stop, void *g) { au_stop_at(g, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "關閉音頻輸出", void, h_au_Close, void *g) {
    au_note(ao(g), "Close", __builtin_return_address(0));
    au_stop_at(g, NULL);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    dispatch_semaphore_signal(a->notify_sem);
    pthread_join(a->notify_th, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
    if (a->au) { AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au); }
    dispatch_release(a->notify_sem);
    pthread_mutex_destroy(&a->m); free(a);
}
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "播放狀態", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "採樣率", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "聲道數", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio15GetAudioOutNameEPKNS0_8AudioOutE", "設備名；宿主只有缺省設備", const char *, h_au_Name, const void *g) { (void)g; return "DeviceOut"; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "採樣格式；只做 PCM16", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }

HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "緩衝描述；寫進客體結構體前 24 字節", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p; b->cap = cap; b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "緩衝數據指針", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "緩衝數據大小", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "追加待播緩衝", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);
    // 診斷開關 DM_AUDIO_DUMP=<文件>：遊戲追加的每塊 PCM 原樣續寫（16 位、48 kHz、聲道數見 OpenAudioOut 日誌，常見 6）。判「某個音播沒播」用：
    // 拿原版音頻對它做互相關（一檔掉落音就這麼驗的）。只在設了變量時打開文件，沒設時這裡只多一次已緩存的判斷
    // Diagnostic switch DM_AUDIO_DUMP=<file>: every PCM block the game appends is written out as is (16-bit, 48 kHz, channel count per the OpenAudioOut log, usually 6). Used to judge "did a given sound play or not":
    // cross-correlate the original game's audio against it (this is how the tier-one drop sound was verified). The file is opened only when the variable is set; when unset this costs only one extra cached check
    // 另寫 <文件>.time.tsv：每 50 塊一行「已寫字節 <TAB> CLOCK_UPTIME_RAW 納秒」，與畫面計時（DM_NVN_REGLUM）同一時鐘，聲音與畫面可對齊（量語音延遲用）
    // Also writes <file>.time.tsv: one line every 50 blocks of "bytes written <TAB> CLOCK_UPTIME_RAW nanoseconds", the same clock as the picture timing (DM_NVN_REGLUM), so sound and picture can be aligned (for measuring voice latency)
    { static FILE *df, *tf; static int on = -1; static uint64_t nb, nblk;
      if (on < 0) { const char *e = getenv("DM_AUDIO_DUMP"); on = e != NULL;
          if (on && !(df = fopen(e, "wb"))) LOG("【紅】DM_AUDIO_DUMP 打不開 %s", e);
          if (df) { char tp[1100]; snprintf(tp, sizeof tp, "%s.time.tsv", e); tf = fopen(tp, "w"); } }
      if (df && b && b->ptr && b->size) { if (tf && nblk++ % 50 == 0) { fprintf(tf, "%llu\t%llu\n", (unsigned long long)nb, (unsigned long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); fflush(tf); }
          fwrite(b->ptr, 1, (size_t)b->size, df); nb += b->size; } }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool：隊列滿時 false（真機同樣有上限） | bool: false when the queue is full (the real device has a limit too)
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "取已播完的緩衝；沒有則 NULL", gbuf *, h_au_Released, void *g) {
    aout *a = ao(g); gbuf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// 宿主內部的 PCM 輸出（進程內音頻渲染器用，見 hle_audren.c）：同一套 AudioOut（真出聲 / 靜音接收端 / 切後台接手），鍵是宿主分配的啞對象
// Host-internal PCM output (used by the in-process audio renderer, see hle_audren.c): the same AudioOut machinery (real output / silent sink / background take-over), keyed by a host-allocated dummy object
void *dm_pcmout_open(int ch) {
    void *key = calloc(1, 64); uint8_t param[8] = {0}; *(uint16_t *)(param + 4) = (uint16_t)ch;
    if (au_open_at(key, NULL, "AudioRenderer", param, NULL)) die("宿主 PCM 輸出打不開");
    return key;
}
void dm_pcmout_start(void *k) { au_start_at(k, NULL); }
int dm_pcmout_append(void *k, void *b) { return (int)h_au_Append(k, b); }
void *dm_pcmout_released(void *k) { return h_au_Released(k); }

// 前端生命週期 / 音頻會話打斷：暫停真出聲，期間由靜音接收端接手按實時消耗（節奏不斷）；恢復時切回 AudioUnit。
// 前端在 切後台 / 打斷開始 時調 1，回前台 / 打斷結束（shouldResume）時調 0。
// Front-end lifecycle / audio session interruption: suspend real audio output, with the silent sink taking over real-time consumption meanwhile (pacing unbroken); switch back to the AudioUnit on resume.
// The front end calls with 1 on going to background / interruption began, and with 0 on returning to foreground / interruption ended (shouldResume).
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on; int failed = 0;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a || !a->au || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) continue;
            // 暫停期間改由靜音接收端按實時消耗：否則緩衝不再釋放，FMOD 輸出線程永久等待、遊戲音頻節奏卡死（來電打斷同理）
            // While suspended the silent sink consumes in real time instead: otherwise buffers are no longer released, the FMOD output thread waits forever and the game's audio pacing stalls (same for an incoming call interruption)
            if (on) { if (!a->null_active) { AudioOutputUnitStop(a->au); null_start(a); } }
            else { if (!a->null_active) continue;
                   atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); a->null_active = 0;
                   OSStatus st = AudioOutputUnitStart(a->au);
                   if (st) {   // 會話還被別人佔著（通話沒掛就切回遊戲、打斷結束時人還在別的 App）：不致命，繼續由靜音接收端頂著，等下一次恢復再試 | The session is still held by someone else (switched back to the game with a call not yet hung up, or the user is still in another app when the interruption ends): not fatal, the silent sink keeps covering, and we retry at the next resume
                       LOG("【紅】AudioUnit 恢復失敗（狀態 %d）：保持靜音接收端，等下次恢復重試", (int)st);
                       null_start(a); failed = 1; } }
        }
        if (failed) g_suspended = 1;   // 沒恢復成：狀態留在「暫停」，下次調 0 會再試 | Resume did not succeed: the state stays "suspended", and the next call with 0 will retry
        LOG("audio %s", on ? "暫停（切後台 / 被打斷）" : failed ? "恢復未成" : "恢復（回前台 / 打斷結束）");
    }
    pthread_mutex_unlock(&g_aos_lock);
}
