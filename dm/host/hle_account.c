// nn::account：真機經 acc:u0 服務管理用戶；宿主固定一個本地用戶。
// 聯網賬號（網絡服務賬號 / 任天堂賬號授權 / 令牌）屬排除範圍，統一回 RES_FAIL，異步上下文的完成事件立即觸發。
// 環境開關：DM_NICK=暱稱（默認 Player）。
// nn::account: the real device manages users via the acc:u0 service; the host has one fixed local user.
// Online accounts (network service account / Nintendo Account authorization / tokens) are out of scope and uniformly return RES_FAIL; the async context's completion event fires immediately.
// Environment switch: DM_NICK=nickname (default Player).
#include "hle.h"

typedef struct { uint64_t v[2]; } Uid;                 // nn::account::Uid，16 字節 | nn::account::Uid, 16 bytes
typedef struct { uint64_t v[3]; } UserHandle;          // nn::account::UserHandle，24 字節 | nn::account::UserHandle, 24 bytes
_Static_assert(sizeof(Uid) == 16, "Uid");
static const Uid k_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // 非零即有效 | Non-zero means valid
#define HANDLE_MAGIC 0x55534552484e444cull                                    // "USERHNDL"

HLE_FORCE("_ZN2nn7account10InitializeEv", "連 acc:u0 賬號服務；宿主固定本地用戶", void, h_acc_Init, void) {}

HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "已打開用戶列表；恆為本地用戶 1 個", Result, h_acc_ListOpen, int *n, Uid *out, int max) {
    *n = 0;
    if (max > 0) { out[0] = k_uid; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "用戶選擇小程序；宿主直接選本地用戶", Result, h_acc_ShowSel, Uid *out, const void *settings) {
    (void)settings; *out = k_uid; return RES_OK;
}
// 取走系統代應用打開的用戶（馬力歐 1.2.1 起先問它，0 個再走 TryOpenPreselectedUser；遊戲只看個數、不看返回值）：
// 宿主沒有代開的用戶，給 0 個，遊戲照 v0 的路自己打開預選用戶
// Take the users the system opened on the application's behalf (Mario 1.2.1 asks this first and falls back to TryOpenPreselectedUser on 0; the game only reads the count, not the return value):
// the host has no such users and returns 0, so the game opens the preselected user itself, as in v0
HLE_FORCE("_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", "系統代開的用戶：無（遊戲改走預選用戶）", void, h_acc_PopOpen, int *n, UserHandle *out, int max) { (void)out; (void)max; *n = 0; }
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "啓動時預選用戶；恆為本地用戶（返回 bool）", unsigned, h_acc_TryOpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return 1;
}
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "打開用戶；句柄內直接存 Uid", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) {
    h->v[0] = uid->v[0]; h->v[1] = uid->v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "從句柄取 Uid", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    if (h->v[2] != HANDLE_MAGIC) die("account::GetUserId 句柄不是宿主發的（%#llx）", h->v[2]);
    out->v[0] = h->v[0]; out->v[1] = h->v[1]; return RES_OK;
}
// nn::account::Nickname = struct { char name[33]; }，UTF-8
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "用戶暱稱；按 DM_NICK 給值", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    (void)uid;
    const char *e = getenv("DM_NICK");
    memset(nick, 0, 33); strncpy(nick, e ? e : "Player", 32);
    return RES_OK;
}

// ---------- 聯網賬號：剪 ----------
// ---------- Online accounts: cut ----------
HLE_FORCE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", "網絡服務賬號，排除範圍；剪口：恆不可用", Result, h_acc_EnsureNsa, const UserHandle *h) { (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", "網絡服務賬號，排除範圍；剪口", Result, h_acc_GetNsaId, void *id, const UserHandle *h) { (void)id; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", "網絡服務賬號令牌，排除範圍；剪口", Result, h_acc_LoadTok, uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    (void)buf; (void)sz; (void)h; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", "聯網會員提示小程序，排除範圍；剪口", Result, h_acc_ShowLic, const UserHandle *h) { (void)h; return RES_FAIL; }

// AsyncContext / NintendoAccountAuthorizationRequestContext：對象內容只由下列接口讀寫，構造不寫內存，完成事件立即觸發、結果恆失敗
// AsyncContext / NintendoAccountAuthorizationRequestContext: object contents are read and written only by the interfaces below; construction writes no memory, the completion event fires immediately, and the result is always failure
HLE_FORCE("_ZN2nn7account12AsyncContextC1Ev", "異步上下文構造；宿主不建 IPC 對象", void, h_acc_AcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", "網絡服務賬號令牌，排除範圍；剪口", Result, h_acc_EnsureTokAsync, void *ctx, const UserHandle *h) { (void)ctx; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "異步完成事件；立即觸發", Result, h_acc_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext9GetResultEv", "異步結果；聯網已剪，恆失敗", Result, h_acc_AcGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev", "任天堂賬號授權，排除範圍；構造不寫內存", void, h_acc_NaCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev", "任天堂賬號授權，排除範圍", void, h_acc_NaDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm",
          "任天堂賬號授權，排除範圍；剪口", Result, h_acc_NaCreate, void *ctx, const UserHandle *h, const void *prm, void *buf, uint64_t sz) {
    (void)ctx; (void)h; (void)prm; (void)buf; (void)sz; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE", "授權完成事件；立即觸發", Result, h_acc_NaGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv", "授權結果；恆失敗", Result, h_acc_NaGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm", "授權令牌；恆失敗", Result, h_acc_NaGetTok, void *self, uint64_t *n, char *buf, uint64_t sz) {
    (void)self; (void)buf; (void)sz; if (n) *n = 0; return RES_FAIL;
}

// ---------- 較新 SDK 用到的賬號接口：用戶枚舉 / 存在性 / 頭像，異步上下文的其餘成員 ----------
// ---------- Account interfaces used by newer SDKs: user enumeration / existence / profile image, the remaining AsyncContext members ----------
HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "用戶數；恆為本地用戶 1 個", Result, h_acc_UserCount, int *n) { *n = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "可用用戶列表；恆為本地用戶 1 個", Result, h_acc_ListQual, int *n, Uid *out, int max) {
    int k = 0; if (max > 0 && out) { out[0] = k_uid; k = 1; } *n = k; return RES_OK;
}
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "用戶是否存在；本地用戶為真", Result, h_acc_Exists, uint8_t *out, const Uid *uid) {
    *out = uid->v[0] == k_uid.v[0] && uid->v[1] == k_uid.v[1]; return RES_OK;
}
HLE_FORCE("_ZN2nn7account15ShowUserCreatorEv", "新建用戶小程序；宿主已有本地用戶", Result, h_acc_ShowCreator, void) { return RES_OK; }
HLE_FORCE("_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", "用戶頭像；宿主沒有（失敗）", Result, h_acc_LoadImage, uint64_t *n, void *buf, uint64_t sz, const Uid *uid) {
    (void)buf; (void)sz; (void)uid; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "網絡服務賬號，排除範圍；恆不可用", Result, h_acc_NsaAvail, uint8_t *out, const UserHandle *h) { (void)h; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "關閉用戶；句柄裡沒有會話", void, h_acc_CloseUser, const UserHandle *h) { (void)h; }
HLE_FORCE("_ZN2nn7account12AsyncContext6CancelEv", "異步上下文取消；無請求", Result, h_acc_AcCancel, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext7HasDoneEPb", "異步上下文：恆已完成", Result, h_acc_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContextD1Ev", "異步上下文析構；無會話可關", void, h_acc_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "異步上下文移動賦值；對象裡沒有宿主狀態", void *, h_acc_AcMove, void *self, void *other) { (void)other; return self; }
