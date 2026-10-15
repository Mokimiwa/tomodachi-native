// SDK 20 一代（朋友收集 夢想生活，SDK 20.5.3）新導入的服務接口。原版都經 sm 連繫統服務；宿主沒有服務進程，按名字接管。
// 來源：朋友收集 main 的未定義動態符號裡、宿主還沒接的那些（survey 的「導入與宿主鈎子」，再按 sm 連服務的篩出）。返回約定同 hle_offline.c：
// 「已保存 / 已提交」類回 RES_OK 免得遊戲重試；查詢類給 Switch 上合理的值；事件給已綁定、永不觸發的 SystemEvent。
// Service interfaces newly imported by the SDK 20 generation (Tomodachi Life: Living the Dream, SDK 20.5.3). On hardware they all connect to system services through sm; the host has no service processes and takes them over by name.
// Source: the undefined dynamic symbols of Tomodachi Life's main that the host did not hook yet (the survey's "imports vs host hooks", filtered to those that connect to a service through sm). Return conventions as in hle_offline.c:
// "saved / submitted" kinds return RES_OK so the game does not retry; queries return values reasonable on a Switch; events get a bound SystemEvent that never fires.
#include "hle.h"

// ---- oe ----
// 截圖通知開關：原版取 am 的 SelfController（oe::Initialize 宿主是空操作，取回空指針）再調它；宿主沒有系統截圖
// Screenshot notification switch: on hardware it fetches am's SelfController (oe::Initialize is a no-op in the host, so that returns null) and calls it; the host has no system screenshots
HLE_FORCE("_ZN2nn2oe33EnableAlbumImageTakenNotificationEv", "截圖通知開關；宿主無系統截圖（原版取 am SelfController 為空即崩）", void, h_oe_EnableAlbumNtf, void) {}

// 地區碼（nn::oe::RegionCode：0 日本、1 美、2 歐、3 澳、4 中國、5 韓國、6 台灣）。朋友收集按 地區 + 語言 選資源（RomFS System/RegionLangMask.txt：JPja USen … KRko CNzh TWzh）。
// DM_REGION=數字 優先；否則按 DM_LANG（與 oe::GetDesiredLanguage 同一個開關，缺省 zh-Hans）推：ja→0、ko→5、zh-Hans→4、zh-Hant→6、英法西德意荷（非美式）→2，其餘→1
// Region code (nn::oe::RegionCode: 0 Japan, 1 USA, 2 Europe, 3 Australia, 4 China, 5 Korea, 6 Taiwan). Tomodachi Life picks resources by region + language (RomFS System/RegionLangMask.txt: JPja USen … KRko CNzh TWzh).
// DM_REGION=number wins; otherwise it follows DM_LANG (the same switch as oe::GetDesiredLanguage, default zh-Hans): ja→0, ko→5, zh-Hans→4, zh-Hant→6, European en/fr/es/de/it/nl→2, everything else→1
int dm_region_code(void) {
    const char *r = getenv("DM_REGION");
    if (r) return atoi(r);
    const char *l = getenv("DM_LANG"); if (!l) l = "zh-Hans";
    if (!strncmp(l, "ja", 2)) return 0;
    if (!strncmp(l, "ko", 2)) return 5;
    if (!strcmp(l, "zh-Hant") || !strcmp(l, "zh-TW")) return 6;
    if (!strncmp(l, "zh", 2)) return 4;
    if (!strcmp(l, "en-GB") || !strcmp(l, "fr") || !strcmp(l, "es") || !strcmp(l, "de") || !strcmp(l, "it") || !strcmp(l, "nl")) return 2;
    return 1;
}
HLE_FORCE("_ZN2nn2oe13GetRegionCodeEv", "地區碼；按 DM_REGION / DM_LANG 推", int, h_oe_GetRegion, void) {
    static int once; int v = dm_region_code(); if (!once++) LOG("oe::GetRegionCode → %d", v);
    return v;
}

// ---- 本地無線：聯網已剪（SSL 的帶參 Initialize 在 hle_net.c，與其餘 SSL 接口一處） | Local wireless: networking is cut (SSL's Initialize with an argument is in hle_net.c, together with the other SSL interfaces) ----
HLE_FORCE("_ZN2nn3ldn11SetProtocolENS0_8ProtocolE", "本地無線協議選擇；聯網已剪，忽略", void, h_ldn_SetProto, int p) { (void)p; }

// ---- 相簿：遊戲內拍照存進系統相簿 | Album: in-game photos saved into the system album ----
HLE_FORCE("_ZN2nn5album10InitializeEv", "相簿服務；宿主沒有，空操作", void, h_album_Init, void) {}
HLE_FORCE("_ZN2nn5album8FinalizeEv", "相簿服務收尾", void, h_album_Fin, void) {}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", "存截圖進相簿；宿主不存，報已保存（免得遊戲重試）", Result, h_album_Save,
          const void *img, size_t sz, int isz, int opt) {
    static int n; if (n++ < 4) LOG("album::SaveScreenshot %zu 字節（圖像尺寸 %d，選項 %d）：宿主不存", sz, isz, opt);
    (void)img; return RES_OK;
}

// ---- 遙測（prepo） | Telemetry (prepo) ----
HLE_FORCE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", "遙測裡加聯網狀態：原版查 nifm；宿主不加，報成功", Result, h_prepo_AddNet,
          void *rep, const char *a, const char *b, const char *c) { (void)rep; (void)a; (void)b; (void)c; return RES_OK; }
// 返回 SystemEvent*：保存頻率超限通知。宿主給一個已綁定、永不觸發的事件（大小留足 0x100）
// Returns a SystemEvent*: the save frequency over-limit notification. The host gives a bound event that never fires (0x100 bytes leaves room)
HLE_FORCE("_ZN2nn5prepo30GetOverSaveFrequencyLimitEventEv", "遙測保存頻率超限事件：綁定、永不觸發", void *, h_prepo_OverEv, void) {
    static _Alignas(16) uint8_t ev[0x100]; static int bound;
    if (!bound) { hle_bind_system_event(ev, 0); bound = 1; }
    return ev;
}

// ---- 不良用語過濾 | Profanity filter ----
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", "不良用語過濾收尾；Initialize 已由宿主接管（hle_misc.c）", Result, h_ngc_Fin, void *self) { (void)self; return RES_OK; }

// ---- Mii 數據庫：主機裡 Mii 工作室的 Mii | Mii database: the console's Mii Maker Miis ----
// 朋友收集用 nn::mii 只做兩件事：讀主機 Mii 工作室裡的 Mii（導入用），和取顏色表（純計算，留在 SDK）。臉由遊戲自己的素材畫，不需要系統的 Mii 模型數據。
// 宿主報「主機上沒有 Mii」：遊戲改用自己的 Mii 編輯器
// Tomodachi Life uses nn::mii for two things only: reading the console's Mii Maker Miis (for importing) and the colour tables (pure computation, left in the SDK). Faces are drawn from the game's own assets, so no system Mii model data is needed.
// The host reports "no Miis on this console": the game uses its own Mii editor instead
HLE_FORCE("_ZN2nn3mii8Database10InitializeEv", "Mii 數據庫：原版連 mii 服務；宿主沒有，報成功", Result, h_mii_DbInit, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn3mii8Database8FinalizeEv", "Mii 數據庫收尾", void, h_mii_DbFin, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3mii8Database3GetEPiPNS0_8CharInfoEii", "讀主機裡的 Mii：一個都沒有", Result, h_mii_DbGet, void *self, int *out_n, void *infos, int cap, int source) {
    (void)self; (void)infos; (void)cap; (void)source;
    if (out_n) *out_n = 0;
    return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database9IsUpdatedEi", "主機 Mii 有沒有變：沒有（返回 bool）", unsigned, h_mii_DbUpdated, void *self, int source) { (void)self; (void)source; return 0; }
// 預設 Mii（遊戲在 main+0x617c6c 要第 2 個）：原版經服務取；SDK 裡自帶同一張預設表與純計算的填法（detail::GetDefaultMii / CharInfoRaw::SetDefaultMii，遊戲自己也導入它），
// 宿主直接調 SDK 原碼填：SetDefaultMii(this, 序號, CreateId 低 / 高 8 字節, 喜好色, Nickname*)（sdk+0x3d65f0：x2/x3 拷進 +0x00，w4 寫 +0x27 蓋掉表裡的值，x5 拷 0x16 字節到 +0x10）。
// CreateId 給隨機 UUIDv4；喜好色取預設表本來的值（表 +0xb4）；暱稱同 Ryujinx 的預設「no name」
// Default Miis (the game asks for number 2 at main+0x617c6c): on hardware they come from the service; the SDK carries the same default table and a pure fill routine (detail::GetDefaultMii / CharInfoRaw::SetDefaultMii, which the game itself imports too),
// so the host calls the SDK's own code: SetDefaultMii(this, index, CreateId low / high 8 bytes, favourite colour, Nickname*) (sdk+0x3d65f0: x2/x3 copied to +0x00, w4 written to +0x27 over the table's value, x5's 0x16 bytes copied to +0x10).
// CreateId is a random UUIDv4; the favourite colour is the default table's own value (table +0xb4); the nickname is "no name", as in Ryujinx's defaults
HLE_FORCE("_ZNK2nn3mii8Database12BuildDefaultEPNS0_8CharInfoEi", "預設 Mii：調 SDK 自帶的預設表填（原版經 mii 服務）", Result, h_mii_DbBuildDefault, const void *self, void *out, int index) {
    (void)self;
    static const uint8_t *(*get_default)(int); static void (*set_default)(void *, int, uint64_t, uint64_t, int, const void *);
    if (!get_default) {
        get_default = (const uint8_t *(*)(int))guest_sym("_ZN2nn3mii6detail13GetDefaultMiiEi");
        set_default = (void (*)(void *, int, uint64_t, uint64_t, int, const void *))guest_sym("_ZN2nn3mii6detail11CharInfoRaw13SetDefaultMiiEiNS0_8CreateIdENS0_13FavoriteColorENS0_8NicknameE");
        if (!get_default || !set_default) die("mii：SDK 裡找不到 GetDefaultMii / SetDefaultMii");
    }
    uint8_t id[16]; arc4random_buf(id, sizeof id); id[6] = (uint8_t)((id[6] & 0x0f) | 0x40); id[8] = (uint8_t)((id[8] & 0x3f) | 0x80);
    uint64_t lo, hi; memcpy(&lo, id, 8); memcpy(&hi, id + 8, 8);
    uint16_t nick[11] = {'n', 'o', ' ', 'n', 'a', 'm', 'e', 0, 0, 0, 0};
    int fav = (int)*(const uint32_t *)(get_default(index) + 0xb4);
    set_default(out, index, lo, hi, fav, nick);
    static int once; if (!once++) LOG("mii::Database::BuildDefault %d：用 SDK 預設表", index);
    return RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database11IsAvailableEv", "Mii 數據庫可用：是（宿主接管的空庫，返回 bool）", unsigned, h_mii_DbAvail, const void *self) { (void)self; return 1; }
HLE_FORCE("_ZNK2nn3mii8Database8GetCountEi", "主機裡的 Mii 數：0", int, h_mii_DbCount, const void *self, int source) { (void)self; (void)source; return 0; }
// 在主機 Mii 裡找這個 Mii：沒有（mii ResultNotFound：模塊 126、描述 1）
// Find this Mii among the console's Miis: not there (mii ResultNotFound: module 126, description 1)
HLE_FORCE("_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", "在主機 Mii 裡找：沒有", Result, h_mii_DbIndex, const void *self, int *out, const void *info) {
    (void)self; (void)info; if (out) *out = -1; return 126u | (1u << 9);
}

// ---- 系統設置：地區 | System settings: region ----
// settings 的地區碼與 oe 的編號不同（0 日本、1 美洲、2 歐洲、3 澳新、4 港台韓、5 中國）；nn::mii::detail::GetMergedRegionCode 經它定 Mii 字體地區
// The settings region code is numbered differently from oe's (0 Japan, 1 Americas, 2 Europe, 3 Australia / NZ, 4 Hong Kong / Taiwan / Korea, 5 China); nn::mii::detail::GetMergedRegionCode uses it to pick the Mii font region
HLE_FORCE("_ZN2nn8settings6system13GetRegionCodeEPNS1_10RegionCodeE", "系統地區碼：由 oe 地區碼換算", void, h_set_Region, int32_t *out) {
    static const int32_t map[7] = {0, 1, 2, 3, 5, 4, 4};   // oe：日 美 歐 澳 中 韓 台 | oe: JP US EU AU CN KR TW
    int r = dm_region_code(); *out = r >= 0 && r < 7 ? map[r] : 1;
}

// ---- applet ----
// 小程序資源用戶號（ARUID）：真機恆非 0（am 建應用時分配）；宿主跳過了 oe / applet 初始化，SDK 讀到的是沒填過的 0。
// SDK 20 的 OpenAudioRenderer（CPU 模式那條 Dfc 路徑也一樣，sdk+0x220b14）要它非 0，否則 AbortImpl()
// Applet resource user ID (ARUID): always non-zero on hardware (assigned by am when the application is created); the host skipped oe / applet initialization, so the SDK reads an unfilled 0.
// SDK 20's OpenAudioRenderer (the Dfc path for CPU mode too, sdk+0x220b14) requires it to be non-zero, otherwise AbortImpl()
HLE_FORCE("_ZN2nn6applet23GetAppletResourceUserIdEv", "小程序資源用戶號：給固定非 0 值（真機由 am 分配，恆非 0）", uint64_t, h_applet_Aruid, void) { return 0x51; }
