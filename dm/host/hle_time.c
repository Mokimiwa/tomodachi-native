// nn::time：真機經 time 服務取網絡校準的用戶時鐘；宿主直接取系統時鐘（單機、離線）。
// 佈局按公開 Horizon ABI：PosixTime = int64 秒；CalendarTime = {s16 年, s8 月/日/時/分/秒}；
// CalendarAdditionalInfo = {u32 星期, u32 年內日, char 時區名[8], u32 夏令時, s32 UTC 偏移秒}。
// nn::time: a real device gets the network-calibrated user clock through the time service; the host reads the system clock directly (single machine, offline).
// Layout follows the public Horizon ABI: PosixTime = int64 seconds; CalendarTime = {s16 year, s8 month/day/hour/minute/second};
// CalendarAdditionalInfo = {u32 day of week, u32 day of year, char time zone name[8], u32 DST, s32 UTC offset in seconds}.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;

// DM_FIXED_TIME=秒（確定性對拍用；真值側對應 dm/verify/ryu/drive.py --fix-time）：
// ① 用戶時鐘（存檔 d2s +0x20 最後遊玩時間、.meta last_modified_time 的來源）恆返回該值；
// ② 建局隨機數種子那一處 time()（main 0x3509a0 bl time，返回點 0x3509a4，與 drive.py 同點）返回該值。
// 本 SDK 的 libc time() 是開機時長（clock_gettime(0) → __nnmusl_ClockGetTime → GetSystemTick），不是牆鐘；
// main 另有 17 處 time() 調用不動，原樣轉給 SDK 自己的 time()。
// DM_FIXED_TIME=seconds (for deterministic comparison; the ground-truth side counterpart is dm/verify/ryu/drive.py --fix-time):
// ① the user clock (source of the save's d2s +0x20 last-played time and .meta last_modified_time) always returns this value;
// ② the one time() at the game-creation random seed (main 0x3509a0 bl time, return point 0x3509a4, same point as drive.py) returns this value.
// This SDK's libc time() is uptime (clock_gettime(0) → __nnmusl_ClockGetTime → GetSystemTick), not wall clock;
// the other 17 time() call sites in main are left alone and forwarded unchanged to the SDK's own time().
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) { const char *e = getenv("DM_FIXED_TIME"); v = e ? strtoll(e, 0, 0) : -1;
                   if (v >= 0) LOG("DM_FIXED_TIME=%lld：用戶時鐘與建局 time()（main 0x3509a4）返回定值", (long long)v); }
    return v;
}
HLE_FORCE("time", "按返回點認出建局種子那一處（DM_FIXED_TIME）；其餘原樣轉調 SDK 的 time()", int64_t, h_time_time, int64_t *t) {
    uint64_t lr = (uint64_t)__builtin_return_address(0);
    static int64_t (*sdk_time)(int64_t *); static uint64_t seed_ret;
    if (!sdk_time) {
        module_t *ms = NULL, *mm = NULL;
        for (int i = 0; i < g_nmod; i++) { if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i]; if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i]; }
        void *f = ms ? module_sym(ms, "time") : NULL; if (!f) die("SDK 裡找不到 time");
        uint32_t w = mm && !strcmp(mm->build_id, DM_D2R_BUILD) ? *(uint32_t *)(mm->base + 0x3509a0) : 0;   // 只在 D2R 構建上讀（別的遊戲這裡是不相干的代碼，main 小於 3.3 MB 還會越界） | read only on the D2R build (elsewhere this is unrelated code, and a main smaller than 3.3 MB would be read past its end)
        if ((w & 0xfc000000) == 0x94000000) seed_ret = (uint64_t)mm->base + 0x3509a4;               // bl time（構建 db0507f3…；改寫器會改跳轉目標，只認 bl 本身） | bl time (build db0507f3…; the rewriter changes the branch target, so only the bl itself is matched)
        else if (fixed_time() >= 0) LOG("【紅】DM_FIXED_TIME：main 0x3509a0 不是預期的 bl time（%#x），建局 time() 未固定", w);
        sdk_time = (int64_t (*)(int64_t *))f;
    }
    int64_t ft = fixed_time();
    if (ft >= 0 && lr == seed_ret) {
        static int once; if (!once++) LOG("DM_FIXED_TIME：建局 time() 返回 %lld", (long long)ft);
        if (t) *t = ft; return ft;
    }
    return sdk_time(t);
}

HLE_FORCE("_ZN2nn4time10InitializeEv", "連 time 服務；時鐘改取宿主系統時間", Result, h_time_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "time 服務已被接管，恆為已初始化", int, h_time_IsInitialized, void) { return 1; }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "用戶時鐘 → 宿主 time()", Result, h_time_GetCurrentTime, int64_t *out) {
    int64_t ft = fixed_time(); *out = ft >= 0 ? ft : time(NULL); return RES_OK;
}
// 首領連戰第 6–10 關的強制恐怖地帶（dm_bossrush.c 設）：g_dm_tz_time 非 0 且未過期時，只有「恐怖地帶」那幾處取時鐘拿到它，存檔時間等其餘調用照舊。
// 哪幾處：返回點落在地帶函數 0x58f1e0 起的那一簇（0x58f1e0–0x591440），或返回後 8 條指令內調地帶日程 0x58f310（服務端 0x636b38 套地帶、0x694b48 等 9 處）。
// 過期：dm_bossrush 每拍續 1 秒，本局停拍（退出 / 崩潰）後不再生效，下一局建局時取到的是真實時鐘。
// Forced Terror Zone for stages 6–10 of the boss rush (set by dm_bossrush.c): when g_dm_tz_time is non-zero and not expired, only the clock reads at the "Terror Zone" sites get it; save timestamps and all other calls are unchanged.
// Which sites: the return point falls in the cluster starting at the zone function 0x58f1e0 (0x58f1e0–0x591440), or the zone schedule 0x58f310 is called within 8 instructions after the return (server side 0x636b38 applying the zone, 0x694b48 and so on, 9 sites).
// Expiry: dm_bossrush renews it by 1 second every tick; once this run stops ticking (exit / crash) it no longer applies, and the next run gets the real clock when the game is created.
_Atomic int64_t g_dm_tz_time;
_Atomic uint64_t g_dm_tz_live_ns;
static int tz_site(uint64_t lr) {   // 只在強制期間才判，每次讀返回點後 8 條指令，不緩存（多線程都會取時鐘） | Checked only while forcing is active, reading the 8 instructions after the return point each time, no caching (several threads read the clock)
    static _Atomic uint64_t base; uint64_t b = atomic_load(&base);
    if (!b) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; atomic_store(&base, b); }
    if (!b || lr < b) return 0;
    uint64_t off = lr - b; if (off > 0x58f1e0 && off < 0x591440) return 1;
    for (int k = 0; k < 8; k++) { uint32_t w = *(const uint32_t *)(uintptr_t)(lr + 4 * k);
        if ((w & 0xfc000000) == 0x94000000) { int64_t imm = w & 0x3ffffff; if (imm & 0x2000000) imm -= 1 << 26; if (off + 4 * k + imm * 4 == 0x58f310) return 1; } }
    return 0;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv", "用戶時鐘 → 宿主 time()；首領連戰強制地帶時恐怖地帶那幾處另返回", int64_t, h_time_now, void) {
    int64_t tz = atomic_load(&g_dm_tz_time);
    if (tz && (uint64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC) < atomic_load(&g_dm_tz_live_ns) && tz_site((uint64_t)__builtin_return_address(0))) return tz;
    int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
          "SDK 實現與 now() 配套，一並接管", int64_t, h_time_to_time_t, const int64_t *tp) { return *tp; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "連 time 服務的時區規則；改用宿主 localtime_r", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
    time_t t = *pt; struct tm tm; localtime_r(&t, &tm);
    static int once; if (!once++) LOG("ToCalendarTime：首次換算 %lld → %04d-%02d-%02d %02d:%02d（時區 %s，偏移 %ld 秒）", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
// 本地時刻換算強制接宿主 libc：SDK 自帶的 localtime / localtime_r / mktime 沒有時區數據、恆按 UTC（宿主實測：編年史「首次發現」由
// main+0xb04d34 調 localtime 再 strftime 出，差 8 小時）。真機 Switch 的 SDK 按系統時區換算，這裡跟宿主系統時區走才一致；
// 宿主 TZ 平時 = 系統時區，只在 DM_FIXED_TIME 確定性對拍時釘 UTC（main.c）。struct tm 佈局兩邊相同（9 個 int、long 偏移、時區名指針）。
// gmtime / gmtime_r / timegm 本來就是 UTC，不接。hle_table.c 的同名直通項只在符號沒有定義時才生效（SDK 有定義），所以要 HLE_FORCE
// Local time conversion is forced onto the host libc: the SDK's own localtime / localtime_r / mktime have no time zone data and always use UTC (measured on the host: the chronicle's 「首次發現」(First Discovered) is produced by
// main+0xb04d34 calling localtime and then strftime, and was off by 8 hours). The SDK on a real Switch converts by the system time zone, so only following the host system time zone here is consistent;
// the host TZ normally = the system time zone, and is pinned to UTC only for deterministic comparison with DM_FIXED_TIME (main.c). The struct tm layout is the same on both sides (9 ints, long offset, time zone name pointer).
// gmtime / gmtime_r / timegm are UTC anyway and are not taken over. The same-named pass-through entries in hle_table.c only take effect when the symbol has no definition (the SDK defines them), hence HLE_FORCE
HLE_FORCE("localtime", "本地時刻 → 宿主 localtime（跟系統時區）", struct tm *, h_localtime, const int64_t *t) { time_t v = (time_t)*t; return localtime(&v); }
HLE_FORCE("localtime_r", "本地時刻 → 宿主 localtime_r（跟系統時區）", struct tm *, h_localtime_r, const int64_t *t, struct tm *out) { time_t v = (time_t)*t; return localtime_r(&v, out); }
HLE_FORCE("mktime", "本地日曆 → 絕對秒，宿主 mktime（跟系統時區）", int64_t, h_mktime, struct tm *tm) { return (int64_t)mktime(tm); }

// ---- SDK 20 一代（朋友收集）新導入的時鐘接口 | Clock interfaces newly imported by the SDK 20 generation (Tomodachi Life) ----
// 原版都連 time 服務。宿主一律按宿主系統時間（DM_FIXED_TIME 時為定值）與系統時區答；用戶時鐘與網絡時鐘同一個值，用戶沒改過時鐘。
// 時區規則（TimeZoneRule，0x4000 字節）只當不透明句柄：換算一律走宿主 localtime / mktime，規則內容不讀
// On hardware these all connect to the time service. The host answers everything from the host system time (a constant under DM_FIXED_TIME) and system time zone; the user and network clocks give the same value, and the user never changed the clock.
// The time zone rule (TimeZoneRule, 0x4000 bytes) is only an opaque handle: every conversion goes through the host localtime / mktime, and the rule's contents are never read
static int64_t now_posix(void) { int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
static void cal_of(int64_t t0, CalendarTime *ct, CalendarAdditionalInfo *ai) {
    time_t t = t0; struct tm tm; localtime_r(&t, &tm);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
}
// 宿主的時區名（/etc/localtime 指向 …/zoneinfo/<名字>），寫進 LocationName（0x24 字節）
// The host's time zone name (/etc/localtime points to …/zoneinfo/<name>), written into a LocationName (0x24 bytes)
static void location_name(char out[0x24]) {
    char buf[512]; ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1); const char *nm = "UTC";
    if (n > 0) { buf[n] = 0; const char *z = strstr(buf, "zoneinfo/"); if (z) nm = z + 9; }
    if (getenv("DM_FIXED_TIME")) nm = "UTC";
    memset(out, 0, 0x24); strlcpy(out, nm, 0x24);
}
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE",
          "按給定時區規則換算；規則只當句柄，改用宿主 localtime_r", Result, h_time_ToCalRule, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt, const void *rule) {
    (void)rule; cal_of(*pt, ct, ai); return RES_OK;
}
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE",
          "日曆 → 絕對秒（按時區規則）；改用宿主 mktime，只給一個候選", Result, h_time_ToPosixRule, int32_t *count, int64_t *out, int cap, const CalendarTime *ct, const void *rule) {
    (void)rule;
    struct tm tm = {0}; tm.tm_year = ct->year - 1900; tm.tm_mon = ct->month - 1; tm.tm_mday = ct->day; tm.tm_hour = ct->hour; tm.tm_min = ct->minute; tm.tm_sec = ct->second; tm.tm_isdst = -1;
    if (cap > 0 && out) out[0] = (int64_t)mktime(&tm);
    if (count) *count = cap > 0 ? 1 : 0;
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", "載入時區規則：規則只當句柄，不填內容", Result, h_time_LoadRule, void *rule, const char *name) {
    static int once; if (!once++) LOG("time::LoadTimeZoneRule %.36s：換算改走宿主時區", name); (void)rule; return RES_OK;
}
HLE_FORCE("_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "用戶時鐘（可校準）→ 宿主時間", Result, h_time_AdjUserNow, int64_t *out) { *out = now_posix(); return RES_OK; }
HLE_FORCE("_ZN2nn4time25AdjustableUserSystemClock6AdjustEv", "校準用戶時鐘；宿主時鐘不用校", Result, h_time_AdjUser, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time28AdjustableNetworkSystemClock6AdjustEv", "校準網絡時鐘；宿主時鐘不用校", Result, h_time_AdjNet, void) { return RES_OK; }

// ClockSnapshot（0xD0 字節，佈局同 yuzu 的 Service::Time::Clock::ClockSnapshot）：用戶 / 網絡時鐘上下文、兩組絕對秒與日曆、穩定時鐘時間點、時區名、自動校準、類型。
// 穩定時鐘 = 宿主單調時鐘（秒），時鐘源號固定；上下文偏移 = 絕對秒 − 穩定時鐘秒，所以用戶時間 = 時間點 + 偏移
// ClockSnapshot (0xD0 bytes, same layout as yuzu's Service::Time::Clock::ClockSnapshot): user / network clock contexts, two sets of absolute seconds and calendars, the steady clock time point, the time zone name, auto correction, type.
// Steady clock = the host monotonic clock (seconds) with a fixed clock source id; the context offset = absolute seconds − steady clock seconds, so user time = time point + offset
typedef struct { int64_t tp; uint8_t src[16]; } SteadyTP;
typedef struct { int64_t offset; SteadyTP tp; } ClockCtx;
typedef struct {
    ClockCtx user, net;                          // 0x00 / 0x20
    int64_t user_time, net_time;                 // 0x40 / 0x48
    CalendarTime user_ct, net_ct;                // 0x50 / 0x58
    CalendarAdditionalInfo user_ai, net_ai;      // 0x60 / 0x78
    SteadyTP steady;                             // 0x90
    char location[0x24];                         // 0xA8
    uint8_t auto_correction, type; uint16_t pad; // 0xCC
} ClockSnapshot;
_Static_assert(sizeof(CalendarAdditionalInfo) == 0x18 && sizeof(ClockSnapshot) == 0xD0, "ClockSnapshot 佈局 | layout");
static const uint8_t k_steady_src[16] = {'d', 'm', 'h', 'o', 's', 't', '-', 's', 't', 'e', 'a', 'd', 'y', 0, 0, 1};
static void snapshot_fill(ClockSnapshot *s) {
    memset(s, 0, sizeof *s);
    int64_t steady = (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000000ull), now = now_posix();
    SteadyTP tp = { steady, {0} }; memcpy(tp.src, k_steady_src, 16);
    s->user = s->net = (ClockCtx){ now - steady, tp };
    s->user_time = s->net_time = now;
    cal_of(now, &s->user_ct, &s->user_ai); s->net_ct = s->user_ct; s->net_ai = s->user_ai;
    s->steady = tp; location_name(s->location); s->auto_correction = 1;
}
HLE_FORCE("_ZN2nn4time13ClockSnapshot31CreateWithAdjustableSystemClockEPS1_", "時鐘快照：宿主時間 + 單調時鐘", Result, h_time_SnapAdj, ClockSnapshot *s) { snapshot_fill(s); return RES_OK; }
// 兩個快照的間隔：時鐘源相同按穩定時鐘時間點相減（秒 → 納秒），否則按網絡時間相減
// Span between two snapshots: with the same clock source, subtract the steady clock time points (seconds → nanoseconds); otherwise subtract the network times
// 返回 bool（sdk 20.5.3 0x43fd10：服務成功 → cset w0, eq），不是 Result：之前回 0 等於「算不出」，朋友收集就當主機時間不可信，
// 先警告「遊戲內時間可能改變」，再要求到 HOME 設定裡調時間，卡在那裡。算不出的情形同真機（yuzu 同）：時鐘源不同、且有一邊沒有網絡時間（首次開機存檔裡的快照全零）
// Returns bool (sdk 20.5.3 0x43fd10: service success → cset w0, eq), not a Result: returning 0 used to mean "cannot compute", so Tomodachi Life treated the console time as untrustworthy,
// warned that "the in-game time may change", then asked to fix the time in the HOME settings and stayed there. The not-computable case is as on hardware (and in yuzu): different clock sources and one side without network time (the all-zero snapshot in a first-boot save)
HLE_FORCE("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", "兩快照間隔：按穩定時鐘（返回 bool）", unsigned, h_time_Span, int64_t *out, const ClockSnapshot *a, const ClockSnapshot *b) {
    int64_t s;
    if (!memcmp(a->steady.src, b->steady.src, 16)) s = b->steady.tp - a->steady.tp;
    else if (a->net_time && b->net_time) s = b->net_time - a->net_time;
    else return 0;
    *out = s * 1000000000ll; return 1;
}
// 用戶手動改時鐘造成的差：宿主時鐘沒人改過，恆 0（TimeSpan 按值在 x0）
// The difference caused by the user changing the clock by hand: nobody changes the host clock, so always 0 (TimeSpan by value in x0)
HLE_FORCE("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", "用戶改時鐘造成的差：恆 0", int64_t, h_time_UserDiff, const ClockSnapshot *a, const ClockSnapshot *b) {
    (void)a; (void)b; return 0;
}
