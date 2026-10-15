// 較新 SDK（16.x）遊戲用到、d2r-mobile 那一作沒用到的系統服務接口：oe / time / pl（共享字體）/ vi 垂直同步 / diag。
// 真機上都經 Horizon 系統服務（IPC）；宿主直接給值或做成「不可用」。
// System service interfaces used by games on newer SDKs (16.x) that the d2r-mobile title did not use: oe / time / pl (shared font) / vi vsync / diag.
// On the real device all of these go through Horizon system services (IPC); the host gives values directly or makes them "unavailable".
#include "hle.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------- oe ----------
HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "CPU 加速檔；宿主不調頻", void, h_oe_CpuBoost, int m) { (void)m; }
// DisplayVersion = char[16]（應用的顯示版本號）：按 main 的構建號取該版控制數據（NACP）裡的值（遊戲檔案的 build 行），認不出的構建給 1.0.0
// DisplayVersion = char[16] (the application's display version): the value from that version's control data (NACP), chosen by main's build ID (the game profile's build rows); unknown builds get 1.0.0
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", "顯示版本號（按 main 構建號）", void, h_oe_DispVer, char *v) {
    static const struct { const char *build, *ver; } k[] = {DM_GAME_DISPLAY_VERSIONS};
    const char *ver = "1.0.0";
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (unsigned j = 0; j < sizeof k / sizeof *k; j++) if (!strcmp(g_mod[i].build_id, k[j].build)) ver = k[j].ver;
    memset(v, 0, 16); strlcpy(v, ver, 16);
}
HLE_FORCE("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", "系統 / 應用音量平衡；宿主恆 1:1", void, h_oe_GetVolBal, float *sys, float *app) { *sys = 1.0f; *app = 1.0f; }
HLE_FORCE("_ZN2nn2oe24SetExpectedVolumeBalanceEff", "音量平衡；宿主忽略", void, h_oe_SetVolBal, float sys, float app) { (void)sys; (void)app; }
// 程序累計運行時長（nn::TimeSpan，納秒）：取宿主啓動以來的單調時間
// Program total active time (nn::TimeSpan, nanoseconds): the host's monotonic time since startup
HLE_FORCE("_ZN2nn2oe25GetProgramTotalActiveTimeEv", "累計運行時長 → 宿主啓動以來", int64_t, h_oe_ActiveTime, void) { return (int64_t)(now_ns() - g_t0_ns); }
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "退出請求臨界區；宿主的退出走 SIGTERM / 生命週期", void, h_oe_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "退出請求臨界區；宿主忽略", void, h_oe_LeaveExit, void) {}
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "自動休眠延長；宿主忽略", void, h_oe_Inactivity, unsigned on) { (void)on; }

// ---------- time ----------
// CalendarTime = { int16 年; int8 月, 日, 時, 分, 秒 }，按值返回（8 字節內，走 x0）
// CalendarTime = { int16 year; int8 month, day, hour, minute, second }, returned by value (within 8 bytes, in x0)
HLE_FORCE("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", "絕對秒 → UTC 日曆（宿主 gmtime_r）", uint64_t, h_time_ToCalUtc, const int64_t *t) {
    time_t v = (time_t)*t; struct tm tm; gmtime_r(&v, &tm);
    return (uint64_t)(uint16_t)(tm.tm_year + 1900) | (uint64_t)(uint8_t)(tm.tm_mon + 1) << 16 | (uint64_t)(uint8_t)tm.tm_mday << 24 |
           (uint64_t)(uint8_t)tm.tm_hour << 32 | (uint64_t)(uint8_t)tm.tm_min << 40 | (uint64_t)(uint8_t)tm.tm_sec << 48;
}
// LocationName = char[0x24]（時區名）：跟宿主系統時區
// LocationName = char[0x24] (time zone name): follows the host's system time zone
HLE_FORCE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", "設備時區名 → 宿主時區", Result, h_time_LocName, char *out) {
    memset(out, 0, 0x24); tzset(); strlcpy(out, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24); return RES_OK;
}
// 網絡時鐘：缺省「離線、不可用」。遊戲檔案 network_clock = synced（或 DM_NET_CLOCK=synced）時按宿主時間答、可用——真機只要聯網校過一次時，離線也有網絡時鐘。
// 朋友收集靠它判斷主機時間可不可信：不可用就先警告「遊戲內時間可能改變」，再要求到 HOME 設定裡調時間，卡在那裡
// Network clock: "offline, unavailable" by default. With the profile's network_clock = synced (or DM_NET_CLOCK=synced) it answers the host time and is available -- a real console that synced once has a network clock even offline.
// Tomodachi Life uses it to judge whether the console time can be trusted: when unavailable it first warns that "the in-game time may change", then asks to fix the time in the HOME settings and stays there
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "網絡時鐘：缺省離線不可用；檔案 network_clock=synced 時按宿主時間", Result, h_time_NetNow, int64_t *out) {
    static int synced = -1; if (synced < 0) { const char *e = getenv("DM_NET_CLOCK"); synced = e ? !strcmp(e, "synced") : DM_GAME_NET_CLOCK_SYNCED; }
    if (!synced) return RES_FAIL;
    const char *ft = getenv("DM_FIXED_TIME"); *out = ft ? strtoll(ft, 0, 0) : (int64_t)time(NULL); return RES_OK;
}
HLE_FORCE("_ZN2nn4time8FinalizeEv", "與 Initialize 配對；宿主無會話", Result, h_time_Finalize, void) { return RES_OK; }
// 「確保網絡時鐘可用」異步任務：不發請求，完成事件立即觸發、結果失敗。對象本身不動（構造 / 析構空操作），事件放宿主靜態區
// "Ensure network clock availability" async task: no request is sent; the finish event fires immediately and the result is failure. The object itself is untouched (constructor / destructor no-ops), the event lives in host static storage
static _Alignas(16) uint8_t g_ev_netclock[0x100];
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", "網絡時鐘任務構造；宿主不建會話", void, h_time_NcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", "網絡時鐘任務析構；無會話可關", void, h_time_NcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", "網絡時鐘任務初始化（離線）", Result, h_time_NcInit, void *self, int mode) { (void)self; (void)mode; return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", "網絡時鐘任務開跑：立即以失敗結束", Result, h_time_NcStart, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", "網絡時鐘任務完成事件：已觸發", void *, h_time_NcEvent, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return g_ev_netclock; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", "網絡時鐘任務結果：離線，失敗", Result, h_time_NcResult, void *self) { (void)self; return RES_FAIL; }

// ---------- pl：共享字體 ----------
// 系統共享字體（0 標準 / 1 簡中 / 2 簡中擴展 / 3 繁中 / 4 韓文 / 5 任天堂擴展）宿主沒有原版，用替身：
//   任天堂擴展 = DM_SHARED_FONT_EXT 或 $DM_FONT_DIR/nintendo_ext.ttf（dm/tools/make_ext_font.py 生成：按鍵圖標在原版私用區碼位，如 U+E0E0 = A 鍵）；
//   其餘類型共用一份 = DM_SHARED_FONT 或 $DM_FONT_DIR/standard.ttf，再缺省 macOS 的 Arial Unicode（iOS 沒有這個路徑，App 裡自帶 dm/font/）。
//   擴展字體打不開時退回標準字體（按鍵圖標畫成「?」，其餘照常）。
// 遊戲自己的文字用 RomFS 裡的字體，共享字體只在系統名字、按鍵圖標等少數地方用。
// 格式：原版共享內存裡是 BFTTF——8 字節頭 + 按字異或混淆的 TTF；遊戲拿到地址後先調 nn::fontll::ScalableFontEngineHelper::Decode(地址, 大小)
//   （SMBW sdk +0x3093c0）就地解混淆並返回地址 + 8。Decode 先看頭一個字：已是「解過」標記（小端 0x18029a7f）就直接返回地址 + 8、不動數據。
//   所以宿主給「解過」標記 + 原樣 TTF；第二個字照原版寫混淆後的長度（鍵 0x49621806：存的是 bswap(長度 ^ 鍵)）。給裸 TTF 會被當密文異或成亂碼，
//   字體引擎隨後用野指針崩（SMBW main +0x107dbc0）。
// The host has none of the original system shared fonts (0 standard / 1 Simplified Chinese / 2 Simplified Chinese extension / 3 Traditional Chinese / 4 Korean / 5 Nintendo extension) and uses stand-ins:
//   Nintendo extension = DM_SHARED_FONT_EXT or $DM_FONT_DIR/nintendo_ext.ttf (generated by dm/tools/make_ext_font.py: button icons at the original private-use code points, e.g. U+E0E0 = A button);
//   all other types share one = DM_SHARED_FONT or $DM_FONT_DIR/standard.ttf, then macOS's Arial Unicode (that path does not exist on iOS; the App ships dm/font/).
//   If the extension font cannot be opened it falls back to the standard font (button icons draw as "?", everything else works).
// The game's own text uses the fonts in its RomFS; shared fonts are only used in a few places such as system names and button icons.
// Format: the original shared memory holds BFTTF - an 8-byte header + a TTF obfuscated by per-word XOR; after getting the address the game calls nn::fontll::ScalableFontEngineHelper::Decode(address, size)
//   (SMBW sdk +0x3093c0), which deobfuscates in place and returns address + 8. Decode first checks the first word: if it is already the "decoded" marker (little-endian 0x18029a7f) it returns address + 8 without touching the data.
//   So the host provides the "decoded" marker + the TTF as is; the second word holds the obfuscated length as in the original (key 0x49621806: stored as bswap(length ^ key)). A raw TTF would be XORed into garbage as if it were ciphertext,
//   and the font engine then crashes on a wild pointer (SMBW main +0x107dbc0).
typedef struct { uint8_t *p; size_t n; } font_t;
static font_t font_map(const char *p) {
    font_t f = {0}; int fd = open(p, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); return f; }
    size_t n = (size_t)st.st_size;
    uint8_t *m = mmap(NULL, n + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { LOG("【紅】共享字體內存分配失敗（%zu 字節）", n + 8); close(fd); return f; }
    size_t got = 0; while (got < n) { ssize_t r = pread(fd, m + 8 + got, n - got, (off_t)got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (got != n) { LOG("【紅】共享字體讀不全：%s（%zu / %zu 字節）", p, got, n); munmap(m, n + 8); return f; }
    uint32_t done = 0x18029a7fu, len = __builtin_bswap32((uint32_t)n ^ 0x49621806u);
    memcpy(m, &done, 4); memcpy(m + 4, &len, 4);
    f.p = m; f.n = n + 8; return f;
}
static font_t font_first(const char *env, const char *file, const char *fallback) {
    char buf[1024]; const char *dir = getenv("DM_FONT_DIR"), *c[3] = {getenv(env), NULL, fallback};
    if (dir && *dir) { snprintf(buf, sizeof buf, "%s/%s", dir, file); c[1] = buf; }
    for (int i = 0; i < 3; i++) {
        if (!c[i] || !*c[i]) continue;
        font_t f = font_map(c[i]); if (f.p) { LOG("共享字體（%s）：%s（%zu 字節，BFTTF「已解」頭 + 原樣 TTF）", file, c[i], f.n - 8); return f; }
    }
    return (font_t){0};
}
static font_t font_get(int type) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static font_t std_f, ext_f; static int std_done, ext_done;
    pthread_mutex_lock(&mu);
    if (!std_done) { std_done = 1; std_f = font_first("DM_SHARED_FONT", "standard.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
                     if (!std_f.p) LOG("【紅】標準共享字體打不開（DM_SHARED_FONT 或 DM_FONT_DIR/standard.ttf 指一個 TTF）"); }
    if (type == 5 && !ext_done) { ext_done = 1; ext_f = font_first("DM_SHARED_FONT_EXT", "nintendo_ext.ttf", NULL);
                                  if (!ext_f.p) LOG("【黃】任天堂擴展共享字體缺（dm/tools/make_ext_font.py 生成 nintendo_ext.ttf）：按鍵圖標會畫成「?」"); }
    font_t f = type == 5 && ext_f.p ? ext_f : std_f;
    pthread_mutex_unlock(&mu);
    return f;
}
HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", "共享字體裝載狀態：已裝（1）", int, h_pl_LoadState, int type) { return font_get(type).p ? 1 : 0; }
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", "共享字體地址 → BFTTF 頭（宿主替身字體）", const void *, h_pl_Addr, int type) { return font_get(type).p; }
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", "共享字體大小 → 頭 8 字節 + 替身字體文件大小", size_t, h_pl_Size, int type) { return font_get(type).n; }
// 是否跑在 Switch 2（代號 Ounce）上（返回 bool）：宿主按 Switch 1 報假。馬力歐 1.2.1 據此決定要不要數追加內容（升級包）、提示升級到 Switch 2 版
// Whether running on a Switch 2 (codename Ounce) (returns bool): the host reports false, as a Switch 1. Mario 1.2.1 uses it to decide whether to count add-on content (the upgrade pack) and suggest the Switch 2 Edition
HLE_FORCE("_ZN2nn2pl16IsRunningOnOunceEv", "是否 Switch 2：否", unsigned, h_pl_IsOunce, void) { return 0; }

// ---------- ec：商店小程序（升級到 Switch 2 版的頁面）----------
// 商店屬排除範圍：空操作。遊戲不看返回值（兩個重載都按無返回值處理）
// ---------- ec: shop applet (the upgrade-to-Switch-2-Edition page) ----------
// The shop is out of scope: no-op. The game does not look at the return value (both overloads are handled as returning nothing)
HLE_FORCE("_ZN2nn2ec40ShowShopApplicationInformationForUpgradeENS0_14UpgradeEditionE", "商店升級頁，排除範圍；空操作", void, h_ec_ShowUpg, int ed) { LOG("ec::ShowShopApplicationInformationForUpgrade(%d)（剪）", ed); }
HLE_FORCE("_ZN2nn2ec40ShowShopApplicationInformationForUpgradeENS0_14UpgradeEditionERKNS_7account10UserHandleE", "商店升級頁（指定用戶），排除範圍；空操作", void, h_ec_ShowUpgU, int ed, const void *user) { (void)user; LOG("ec::ShowShopApplicationInformationForUpgrade(%d, 用戶)（剪）", ed); }

// ---------- nfp：amiibo ----------
// 馬力歐 1.2.1 起啓動即初始化 nfp（v0 不用）。真 SDK 要連 sm: 服務，宿主沒有 → 中止，所以這一族全部接管。
// 宿主沒有 NFC 讀卡器：報一台 NFC 設備，掛在當前接著的手柄上（同 hle_hid.c），但找標籤永遠找不到——與真機手邊沒有 amiibo 時一樣：
// 遊戲的「請碰 amiibo」界面照常出現，取消即退出。要標籤才能做的操作（掛載、讀寫應用區、amiibo 設置小程序）一律回「沒找到標籤」。
// 遊戲不掛激活 / 失活事件，靠輪詢 GetDeviceState 看有沒有標籤。
// ---------- nfp: amiibo ----------
// From Mario 1.2.1 the game initializes nfp at boot (v0 did not use it). The real SDK would connect to the sm: service, which the host lacks → abort, so the whole family is taken over.
// The host has no NFC reader: it reports one NFC device on the currently connected controller (same as hle_hid.c) whose detection never finds a tag -- the same as real hardware with no amiibo at hand:
// the game's "touch an amiibo" screen appears as usual and cancelling leaves it. Operations that need a tag (mount, application area read / write, amiibo settings applets) all return "tag not found".
// The game attaches no activate / deactivate events; it polls GetDeviceState to see whether a tag is there.
#define NFP_RES_TAG_NOT_FOUND (115u | 97u << 9)   // 模塊 115（nfp）描述 97 | module 115 (nfp), description 97
enum { NFP_STATE_INIT = 0, NFP_STATE_SEARCH = 1 };   // nn::nfp::DeviceState：0 初始、1 找標籤中（2 有標籤，宿主不會出現） | 0 initialized, 1 searching for a tag (2 = tag found, never on the host)
typedef struct { uint32_t id; uint8_t reserved[4]; } NfpDeviceHandle;
#define NFP_DEVICE_ID 0x4e465030u   // 'NFP0'
static _Atomic int g_nfp_state;
uint32_t hle_hid_connected_npad(void);
static Result nfp_no_tag(const char *what) {
    static _Atomic int logged; if (!atomic_exchange(&logged, 1)) LOG("nfp：%s 等要 amiibo 標籤的操作 → 沒找到標籤（宿主無 NFC）", what);
    return NFP_RES_TAG_NOT_FOUND;
}
HLE_FORCE("_ZN2nn3nfp10InitializeEv", "amiibo 庫初始化；宿主一台 NFC 設備、永遠找不到標籤", void, h_nfp_Init, void) { LOG("nfp::Initialize：宿主一台 NFC 設備，找不到 amiibo 標籤"); }
HLE_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "NFC 可用性變化事件；綁內核事件、恆不觸發", void, h_nfp_AttachAvail, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "NFC 設備列表：一台", Result, h_nfp_List, NfpDeviceHandle *out, int *n, int max) {
    *n = 0; if (max >= 1) { memset(&out[0], 0, sizeof out[0]); out[0].id = NFP_DEVICE_ID; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", "NFC 設備所在手柄：當前接著的那個", Result, h_nfp_NpadId, uint32_t *out, const NfpDeviceHandle *h) { (void)h; *out = hle_hid_connected_npad(); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", "開始找標籤（永遠找不到）", Result, h_nfp_Start, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_SEARCH); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", "停止找標籤", Result, h_nfp_Stop, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_INIT); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", "設備狀態：初始 / 找標籤中", int, h_nfp_State, const NfpDeviceHandle *h) { (void)h; return atomic_load(&g_nfp_state); }
HLE_FORCE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", "標籤信息：沒有標籤", Result, h_nfp_TagInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetTagInfo"); }
HLE_FORCE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", "型號信息：沒有標籤", Result, h_nfp_ModelInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetModelInfo"); }
HLE_FORCE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", "掛載標籤：沒有標籤", Result, h_nfp_Mount, const NfpDeviceHandle *h, int model, int target) { (void)h; (void)model; (void)target; return nfp_no_tag("Mount"); }
HLE_FORCE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", "寫回標籤：沒有標籤", Result, h_nfp_Flush, const NfpDeviceHandle *h) { (void)h; return nfp_no_tag("Flush"); }
HLE_FORCE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", "打開應用區：沒有標籤", Result, h_nfp_OpenArea, const NfpDeviceHandle *h, uint32_t id) { (void)h; (void)id; return nfp_no_tag("OpenApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", "讀應用區：沒有標籤", Result, h_nfp_GetArea, void *buf, uint64_t *n, const NfpDeviceHandle *h, uint64_t sz) { (void)buf; (void)h; (void)sz; if (n) *n = 0; return nfp_no_tag("GetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", "寫應用區：沒有標籤", Result, h_nfp_SetArea, const NfpDeviceHandle *h, const void *buf, uint64_t sz) { (void)h; (void)buf; (void)sz; return nfp_no_tag("SetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "建應用區：沒有標籤", Result, h_nfp_CreateArea, const NfpDeviceHandle *h, const void *info) { (void)h; (void)info; return nfp_no_tag("CreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "重建應用區：沒有標籤", Result, h_nfp_RecreateArea, const NfpDeviceHandle *h, const void *info) { (void)h; (void)info; return nfp_no_tag("RecreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo 修復小程序：沒有標籤", Result, h_nfp_Restorer, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartRestorer"); }
HLE_FORCE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo 遊戲數據刪除小程序：沒有標籤", Result, h_nfp_Eraser, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartGameDataEraser"); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo 暱稱與主人設置小程序：沒有標籤", Result, h_nfp_Nick, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag) { (void)h; (void)reg; (void)p; (void)tag; if (changed) *changed = 0; return nfp_no_tag("StartNicknameAndOwnerSettings"); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_", "amiibo 暱稱與主人設置小程序（帶原登記信息）：沒有標籤", Result, h_nfp_Nick2, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag, const void *old) { (void)h; (void)reg; (void)p; (void)tag; (void)old; if (changed) *changed = 0; return nfp_no_tag("StartNicknameAndOwnerSettings"); }

// ---------- vi：垂直同步事件 ----------
// 遊戲給一個 SystemEventType，宿主綁定後由一條宿主線程按 60 Hz 觸發（DM_VSYNC_HZ 可改）
// ---------- vi: vsync event ----------
// The game passes a SystemEventType; after binding, a host thread fires it at 60 Hz (DM_VSYNC_HZ can change it)
static void *vsync_thread(void *ev) {
    const char *e = getenv("DM_VSYNC_HZ"); double hz = e ? atof(e) : 60.0; if (hz <= 0) hz = 60.0;
    uint64_t period = (uint64_t)(1e9 / hz), next = now_ns() + period;
    for (;;) {
        uint64_t t = now_ns(); if (next > t) { struct timespec ts = {(time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull)}; nanosleep(&ts, NULL); }
        hle_signal_system_event(ev); next += period; if (now_ns() > next + 4 * period) next = now_ns() + period;   // 落後太多不追幀 | do not try to catch up when far behind
    }
    return NULL;
}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "垂直同步事件：宿主線程按 60 Hz 觸發", Result, h_vi_VsyncEvent, void *ev, void *disp) {
    (void)disp; hle_bind_system_event(ev, 1);
    pthread_t th; if (pthread_create(&th, NULL, vsync_thread, ev)) die("垂直同步線程起不來"); pthread_detach(th);
    LOG("vi::GetDisplayVsyncEvent：%p 由宿主 60 Hz 觸發", ev); return RES_OK;
}

// ---------- diag ----------
// LogMetaData 之後的文本（NN_LOG）直接進宿主日誌；中止打出調用點後報紅退出
// The text after LogMetaData (NN_LOG) goes straight into the host log; an abort prints the call site and exits with a failure
HLE_FORCE("_ZN2nn4diag6detail7PutImplERKNS0_11LogMetaDataEPKcm", "遊戲日誌 → 宿主日誌", void, h_diag_Put, const void *meta, const char *s, size_t n) {
    (void)meta; while (n && (s[n - 1] == '\n' || s[n - 1] == '\0')) n--; if (n) LOG("[game] %.*s", (int)n, s);
}
// 中止：報出調用點（模塊 + 偏移）與客體回溯再退出
// Abort: report the call site (module + offset) and the guest backtrace, then exit
static void diag_abort(uint32_t res, void *lr) {
    module_t *m = module_of(lr);
    LOG("遊戲調用 nn::diag 中止：Result %#x，調用點 %s+%#llx", res, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)lr - m->base) : (unsigned long long)(uintptr_t)lr);
    dm_backtrace((uint64_t)__builtin_frame_address(1));
    die("遊戲中止");
}
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEv", "遊戲中止", void, h_diag_Abort0, void) { diag_abort(0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE", "遊戲中止（帶 Result）", void, h_diag_AbortR, const uint32_t *r) { diag_abort(r ? *r : 0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag12GetBacktraceEPmi", "回溯；宿主不提供（0 幀）", int, h_diag_Bt, uint64_t *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn4diag13GetSymbolNameEPcmm", "符號名；宿主不提供（空串）", uint64_t, h_diag_SymName, char *buf, size_t n, uint64_t addr) { (void)addr; if (buf && n) buf[0] = 0; return 0; }

// ---------- settings：語言枚舉 ↔ 語言代碼 ----------
// 真機 MakeLanguageCode 問 settings 服務；表是公開固定的（nn::settings::Language 順序）。LanguageCode = char[8]
// ---------- settings: language enum <-> language code ----------
// On the real device MakeLanguageCode asks the settings service; the table is public and fixed (nn::settings::Language order). LanguageCode = char[8]
static const char *const k_lang_code[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
HLE_FORCE("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", "語言枚舉與語言代碼比較（返回 bool）", unsigned, h_set_LangEq, const int32_t *lang, const char *code) {
    if ((unsigned)*lang >= sizeof k_lang_code / sizeof *k_lang_code) return 0;
    return strncmp(k_lang_code[*lang], code, 8) == 0;
}
