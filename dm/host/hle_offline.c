// 聯網與系統周邊服務（排除範圍：聯機、本地無線、好友、遙測、SSL）在較新 SDK 上的接口：一律做成「不可用」，遊戲走離線分支。
// 只接會走服務的入口；只讀寫遊戲自己緩衝的庫函數（prepo 的 Add / Array / Struct、err 的 ErrorResultVariant、friends 的取值函數、socket 的字節序轉換）留在 SDK 原樣跑。
// 返回約定同 hle_friends.c：Result 回 RES_FAIL（「已保存 / 已提交」類回 RES_OK 免得遊戲重試）；bool 回假；事件給已綁定、永不觸發的 SystemEvent。
// Networking and peripheral system services (out of scope: online play, local wireless, friends, telemetry, SSL) as seen through newer SDKs: all made "unavailable", the game takes its offline branch.
// Only entry points that go to a service are hooked; library functions that only read and write the game's own buffers (prepo Add / Array / Struct, err ErrorResultVariant, friends getters, socket byte-order conversion) keep running in the SDK.
// Return convention as in hle_friends.c: Result returns RES_FAIL ("saved / submitted" kinds return RES_OK so the game does not retry); bool returns false; events get a bound SystemEvent that never fires.
#include "hle.h"

static _Alignas(16) uint8_t g_ev_ldn[0x100];

// ---------- prepo：遙測報告。存 / 提交都報成功，什麼也不發 ----------
// ---------- prepo: telemetry reports. Saving / submitting all report success; nothing is sent ----------
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "遙測報告保存（用戶）；不發送，報成功", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "遙測報告保存；不發送，報成功", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10InitializeEv", "遙測初始化：原版連 prepo 服務（經 sm），宿主沒有服務進程；空操作（Engage 顯式調用）", Result, h_prepo_Init, void) { return RES_OK; }
// 往報告裡加會話號：真機要先連 prepo 服務取會話（超級馬力歐 3D 世界 + 狂怒世界進存檔選擇時調）；宿主不加，報成功。聯網狀態那個見 hle_sdk20.c
// Add the session ID to a report: on hardware it first connects to the prepo service for the session (Super Mario 3D World + Bowser's Fury calls it entering the save select); the host adds nothing and reports success. The connection-status one is in hle_sdk20.c
HLE_FORCE("_ZN2nn5prepo12AddSessionIdEPNS0_10PlayReportEPKc", "遙測會話號：不連服務，不加，報成功", Result, h_prepo_AddSess, void *rep, const char *key) { (void)rep; (void)key; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "遙測立即上傳；不發送，報成功", Result, h_prepo_Tx, void) { return RES_OK; }

// ---------- ldn：本地無線。初始化即失敗，狀態恆為「無」 ----------
// ---------- ldn: local wireless. Initialization fails, state is always "none" ----------
HLE_FORCE("_ZN2nn3ldn10InitializeEv", "本地無線（剪）：初始化失敗", Result, h_ldn_Init, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn8FinalizeEv", "本地無線（剪）", void, h_ldn_Fini, void) {}
HLE_FORCE("_ZN2nn3ldn8GetStateEv", "本地無線（剪）：狀態 0（無）", int, h_ldn_State, void) { return 0; }
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "本地無線狀態事件：綁定、永不觸發", void, h_ldn_Attach, void *ev) { hle_bind_system_event(ev, 0); (void)g_ev_ldn; }
HLE_FORCE("_ZN2nn3ldn11OpenStationEv", "本地無線（剪）", Result, h_ldn_OpenSta, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn12CloseStationEv", "本地無線（剪）", Result, h_ldn_CloseSta, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", "本地無線（剪）", Result, h_ldn_OpenAp, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", "本地無線（剪）", Result, h_ldn_CloseAp, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn10DisconnectEv", "本地無線（剪）", Result, h_ldn_Disc, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", "本地無線（剪）", Result, h_ldn_Destroy, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "本地無線掃描（剪）：0 個", Result, h_ldn_Scan, void *out, int *n, int max, const void *f, int ch) { (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "本地無線掃描（剪）：0 個", Result, h_ldn_ScanP, void *out, int *n, int max, const void *f, int ch) { (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "本地無線（剪）", Result, h_ldn_Create,
          const void *a, const void *b, const void *c) { (void)a; (void)b; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "本地無線（剪）",
          Result, h_ldn_CreateP, const void *a, const void *b, const void *c, const void *d, int n, const void *e) { (void)a; (void)b; (void)c; (void)d; (void)n; (void)e; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "本地無線（剪）", Result, h_ldn_Connect,
          const void *a, const void *b, const void *c, int v, int o) { (void)a; (void)b; (void)c; (void)v; (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "本地無線（剪）",
          Result, h_ldn_ConnectP, const void *a, const void *b, const void *c, const void *d, int v, int o) { (void)a; (void)b; (void)c; (void)d; (void)v; (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "本地無線（剪）", Result, h_ldn_NetInfo, void *o) { (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "本地無線（剪）", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) { if (a) *a = 0; if (m) *m = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "本地無線（剪）", Result, h_ldn_SecPrm, void *o) { (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "本地無線（剪）", Result, h_ldn_Adv, const void *d, uint64_t n) { (void)d; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "本地無線（剪）：0", int, h_ldn_DiscReason, void) { return 0; }
HLE_FORCE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", "本地無線（剪）", Result, h_ldn_Filter, uint64_t mac) { (void)mac; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "本地無線（剪）", Result, h_ldn_Policy, int p) { (void)p; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "本地無線（剪）", Result, h_ldn_Reject, uint32_t a) { (void)a; return RES_FAIL; }

// ---------- socket：離線。初始化成功（網絡庫全局初始化依賴它），之後的操作一律失敗、errno = ENETDOWN ----------
// ---------- socket: offline. Initialization succeeds (the network library's global init depends on it), every later operation fails with errno = ENETDOWN ----------
enum { SK_ENETDOWN = 50, SK_EAI_FAIL = 4 };
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "連 bsd 套接字服務；離線，不建會話（剪）", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) { (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK; }
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "與 Initialize 配對（剪）", Result, h_sk_Fini, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "離線：ENETDOWN", int, h_sk_Errno, void) { return SK_ENETDOWN; }
HLE_FORCE("_ZN2nn6socket6SocketEiii", "離線（剪）：-1", int, h_sk_Socket3i, int a, int b, int c) { (void)a; (void)b; (void)c; return -1; }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", "離線（剪）", int, h_sk_Shutdown, int s, int h) { (void)s; (void)h; return -1; }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", "離線（剪）", int, h_sk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket4PollEP6pollfdmi", "離線（剪）", int, h_sk_Poll, void *p, uint64_t n, int t) { (void)p; (void)n; (void)t; return -1; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "離線（剪）", long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "離線（剪）", long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "離線（剪）", long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "域名解析（剪）：EAI_FAIL", int, h_sk_Gai2, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "與 GetAddrInfo 配對（剪）", void, h_sk_FreeAi2, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "解析錯誤文字", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }

// ---------- ssl：離線，建上下文即失敗 ----------
// ---------- ssl: offline, creating a context fails ----------
HLE_FORCE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "SSL（剪）", Result, h_ssl_ImpCli, void *self, uint64_t *id, const char *a, const char *b, unsigned x, unsigned y) { (void)self; (void)a; (void)b; (void)x; (void)y; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "SSL（剪）", Result, h_ssl_ImpSrv, void *self, uint64_t *id, const char *a, unsigned n, int f) { (void)self; (void)a; (void)n; (void)f; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", "SSL（剪）", Result, h_ssl_ImpCrl, void *self, uint64_t *id, const char *a, unsigned n) { (void)self; (void)a; (void)n; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", "SSL（剪）", Result, h_ssl_Certs,
          void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }

// ---------- nifm / nsd：離線 ----------
// ---------- nifm / nsd: offline ----------
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "離線：本地網絡模式開關忽略", void, h_nifm_LocalMode, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "離線：網絡請求不提交（立即返回，IsNetworkAvailable 恆假）", void, h_nifm_SubmitWait, void) {}
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "離線：無 IP 配置", Result, h_nifm_IpCfg, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
// SDK 10 的同名函數參數是 socket::InAddr / in_addr，與上面和 hle_net.c 裡的另一代剛好反過來（超級馬力歐 3D 世界 + 狂怒世界）
// SDK 10's functions of the same name take socket::InAddr / in_addr, exactly the other way round from the generation above and in hle_net.c (Super Mario 3D World + Bowser's Fury)
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", "離線：無 IP 配置（SDK 10 重載）", Result, h_nifm_IpCfg10, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", "離線：無 IP（SDK 10 重載）", Result, h_nifm_GetIp10, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
// EnvironmentIdentifier = char[8]：零售環境 "lp1"
// EnvironmentIdentifier = char[8]: retail environment "lp1"
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "服務器環境名：零售 lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }

// ---------- friends：較新 SDK 的重載 ----------
// ---------- friends: newer SDK overloads ----------
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "好友庫分配器；不建會話，忽略", void, h_fr_SetAlloc, void *a, void *f) { (void)a; (void)f; }
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "好友列表為空（剪）", Result, h_fr_List2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "好友列表不可用（剪）", Result, h_fr_ListAvail, uint8_t *out, const void *uid) { (void)uid; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "好友列表（剪）：請求即失敗", Result, h_fr_ListEnsure, void *ctx, const void *uid) { (void)ctx; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12AsyncContext7HasDoneEPb", "異步上下文：恆已完成", Result, h_fr_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "遊玩記錄不登記（剪）",
          Result, h_fr_PlayHist2, const void *uid, const void *key, const void *a, const void *b) { (void)uid; (void)key; (void)a; (void)b; return RES_FAIL; }

// ---------- ngc：較新 SDK 的重載（帶 bool）。恆判無敏感詞 ----------
// ---------- ngc: newer SDK overloads (with bool). Always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", "敏感詞過濾初始化；宿主無詞庫", Result, h_ngc_Init3, void *self, void *work, uint64_t sz, unsigned check) { (void)self; (void)work; (void)sz; (void)check; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", "敏感詞檢查：全部無敏感詞", Result, h_ngc_Check16, void *self, uint32_t *out, unsigned mask, const void **words, uint64_t n) {
    (void)self; (void)mask; (void)words; for (uint64_t i = 0; out && i < n; i++) out[i] = 0; return RES_OK;
}

// ---------- err：錯誤顯示小程序。只記日誌，不彈界面 ----------
// ---------- err: error display applet. Only logged, no UI is shown ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "錯誤顯示小程序；宿主只記日誌", void, h_err_ShowCode, uint64_t code) { LOG("遊戲請求顯示錯誤：%04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "錯誤顯示小程序；宿主只記日誌", void, h_err_ShowVar, const void *v) { (void)v; LOG("遊戲請求顯示錯誤（ErrorResultVariant）"); }
HLE_FORCE("_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", "追加內容版本不符錯誤小程序；宿主只記日誌", void, h_err_AocVer, void) { LOG("遊戲請求顯示錯誤：追加內容版本不符"); }
HLE_FORCE("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv", "遊戲版本不符錯誤小程序；宿主只記日誌", void, h_err_AppVer, void) { LOG("遊戲請求顯示錯誤：遊戲版本不符"); }

// ---------- web：網頁小程序（聯機大廳）。不可用 ----------
// ---------- web: web page applet (online lobby). Unavailable ----------
HLE_FORCE("_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", "聯機大廳網頁小程序（剪）", Result, h_web_Lobby, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowLobbyPage（剪）"); return RES_FAIL; }

// ---------- C 接口（nnsocket* / nnssl*）：同樣離線。參數都在寄存器裡，宿主不看 ----------
// ---------- C interface (nnsocket* / nnssl*): offline as well. The arguments are all in registers and the host does not look at them ----------
HLE_FORCE("nnsocketAccept", "離線（剪）", int, h_c_sk_Accept, void) { return -1; }
HLE_FORCE("nnsocketListen", "離線（剪）", int, h_c_sk_Listen, void) { return -1; }
HLE_FORCE("nnsocketRecvMsg", "離線（剪）", int, h_c_sk_RecvMsg, void) { return -1; }
HLE_FORCE("nnsocketSendMsg", "離線（剪）", int, h_c_sk_SendMsg, void) { return -1; }
HLE_FORCE("nnsocketShutdown", "離線（剪）", int, h_c_sk_Shutdown, void) { return -1; }
HLE_FORCE("nnsocketGetAddrInfo", "域名解析（剪）：EAI_FAIL", int, h_c_sk_Gai, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketFreeAddrInfo", "與 GetAddrInfo 配對（剪）", void, h_c_sk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("nnsocketGAIStrError", "解析錯誤文字", const char *, h_c_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
