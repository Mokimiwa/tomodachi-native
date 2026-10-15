// 宿主生命週期：優雅退出（SIGTERM；iOS 由 App 終止回調調 dm_host_shutdown）。
// 停後台線程（啓動塊預取）、暫停音頻、圖形掛起（等在途 NVN 調用結束並清空 GPU 工作），再 _exit(0)；1.5 秒兜底直接 _exit(0)。
// 缺省動作同樣約 0.08 秒退出（退出碼 143）；本函數的價值是有序停後台線程、退出碼 0、給 iOS 生命週期回調一個入口。
// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Stop background threads (boot block prefetch), pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void tact_prefetch_stop(void);
void granny_prof_dump(void);
void hlecount_flush(void);
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("收到 %s：優雅退出", why);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("【紅】優雅退出 1.5 秒未完成，直接退出"); _exit(0); });
    tact_prefetch_stop();
    granny_prof_dump();
    hlecount_flush();                               // DM_HLECOUNT：_exit 不走 atexit，這裡寫一份 | DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("優雅退出完成（%.3f 秒）", (now_ns() - t0) / 1e9);
    _exit(0);
}
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);                       // 不走缺省動作：交給 GCD 信號源在普通線程上收尾 | do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
}
