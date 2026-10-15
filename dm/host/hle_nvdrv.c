// nvdrv（NVIDIA 驅動服務）：宿主把 NVN 整個接管了，用不到它；只有 SDK 影片庫的硬件解碼器（subsdk0 的 NVOMX → NvRmChannelOpen）直接開 /dev/nvhost-*。
// 原版 NvOsDrvOpen 先連 nvdrv 服務，宿主沒有服務進程，SDK 當場中止（Engage 起機時 Unity 建解碼器即中止）。
// 接管：開設備一律失敗（fd = −1），ioctl / 映射 / 查事件一律回錯，解碼器走它自己的失敗路徑
// nvdrv (the NVIDIA driver service): the host takes over NVN entirely and has no use for it; only the SDK movie library's hardware decoder (subsdk0's NVOMX → NvRmChannelOpen) opens /dev/nvhost-* directly.
// The original NvOsDrvOpen first connects to the nvdrv service; the host has no service process and the SDK aborts on the spot (Engage aborted when Unity created a decoder at startup).
// Takeover: opening a device always fails (fd = -1), and ioctls / mappings / event queries all return an error, so the decoder takes its own failure path
#include "hle.h"

#define NV_ERR 3   // NvError：非零即失敗（3 = 未初始化） | NvError: non-zero means failure (3 = not initialized)

HLE_FORCE("NvOsDrvOpen", "nvdrv 開設備：宿主沒有 nvdrv，回 −1", int, h_nvos_Open, const char *path) {
    static int red; if (!red++) LOG("【紅】nvdrv：%s 開不了（宿主沒有 nvdrv；只有影片硬件解碼用它）", path ? path : "(null)");
    return -1;
}
HLE_FORCE("NvOsDrvClose", "nvdrv 關設備；無事可做", void, h_nvos_Close, int fd) { (void)fd; }
HLE_FORCE("NvOsDrvIoctl", "nvdrv ioctl：回錯", int, h_nvos_Ioctl, int fd, uint32_t req, void *arg, size_t n) { (void)fd; (void)req; (void)arg; (void)n; return NV_ERR; }
HLE_FORCE("NvOsDrvIoctl2", "nvdrv ioctl2：回錯", int, h_nvos_Ioctl2, int fd, uint32_t req, void *arg, size_t n) { (void)fd; (void)req; (void)arg; (void)n; return NV_ERR; }
HLE_FORCE("NvOsDrvIoctl3", "nvdrv ioctl3：回錯", int, h_nvos_Ioctl3, int fd, uint32_t req, void *arg, size_t n) { (void)fd; (void)req; (void)arg; (void)n; return NV_ERR; }
HLE_FORCE("NvOsDrvMapSharedMem", "nvdrv 共享內存映射：回錯", int, h_nvos_MapShared, int fd) { (void)fd; return NV_ERR; }
HLE_FORCE("_Z17NvOsDrvQueryEventijPN2nn2os15SystemEventTypeE", "nvdrv 查事件：回錯", int, h_nvos_QueryEvent, int fd, uint32_t id, void *ev) { (void)fd; (void)id; (void)ev; return NV_ERR; }
