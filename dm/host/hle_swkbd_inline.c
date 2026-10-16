// swkbd 內嵌鍵盤（InlineKeyboard，朋友收集給 Mii 起名、寫暱稱用）→ 宿主原生輸入框。原版在遊戲畫面裡嵌一個鍵盤小程序：
// 遊戲每幀 Calc() 收小程序的回復（經 applet 存儲），GetImage() 取鍵盤畫面自己貼上去；宿主沒有小程序進程，整個接口按名字接管：
//   · Initialize 報成功，下一次 Calc 回調「初始化完成」；畫面一律不給（GetImage 返回 false），遊戲不畫鍵盤；
//   · Appear（遊戲要輸入）後的下一次 Calc：彈宿主輸入框（Mac 窗口 / iOS 系統鍵盤，front_text_input，同 hle_swkbd.c；DM_KBD="文本" 直給），
//     確定 → 先按原版順序回調「文本已變」（整串），再回調「確定」；取消 → 回調「取消」。
//   · 遊戲收到確定 / 取消後每幀調 Disappear，等鍵盤收完：鍵盤彈著時的 Disappear → 下一次 Calc 回調一次「鍵盤已結束」（FinishedKeyboard）。
//     這個回調只能在 Disappear 之後發：鍵盤還彈著就收到它，遊戲當鍵盤掉了，重建再彈（實測朋友收集島名頁因此每幀重彈）。
// 回調參數的佈局取自原版回復的存儲格式（sdk 20.5.3：CalcDecidedEnter_ 0x43df54 讀 0x3f0 字節原樣交給回調；遊戲的「文本已變」回調 main 0x209dd18 讀
// +0 UTF-16 串、+0x3f0 / +0x3f4 兩個光標位置）：ChangedStringArg = { u16 串[0x1f4]; u32 長度 @0x3e8; s32 詞典起 @0x3ec; s32 詞典止 @0x3f0; s32 光標 @0x3f4 }，
// DecidedEnterArg = { u16 串[0x1f4]; u32 長度 @0x3e8 }。InputText（遊戲用 InputText::Set 填，SDK 原碼）= { u16 串[0x1f5] @0; … @0x3ec }。
// swkbd inline keyboard (InlineKeyboard, used by Tomodachi Life to name Miis and write nicknames) → the host's native input box. On hardware a keyboard applet is embedded in the game picture:
// every frame the game's Calc() collects the applet's replies (through applet storage) and GetImage() fetches the keyboard picture, which the game draws itself; the host has no applet process, so the whole interface is taken over by name:
//   · Initialize reports success and the next Calc calls back "initialization finished"; no picture is ever given (GetImage returns false), so the game draws no keyboard;
//   · on the next Calc after Appear (the game wants input): the host input box comes up (a Mac window / the iOS system keyboard, front_text_input, as in hle_swkbd.c; DM_KBD="text" answers directly),
//     OK → first the "string changed" callback (whole string) in the original order, then the "decided enter" callback; cancel → the "decided cancel" callback.
//   · after OK / cancel the game calls Disappear every frame until the keyboard has gone: a Disappear while the keyboard is up → "keyboard finished" (FinishedKeyboard) once on the next Calc.
//     That callback may only follow a Disappear: received while the keyboard is still up, the game treats the keyboard as lost and rebuilds and reopens it (measured: Tomodachi Life's island name page reopened it every frame).
// The callback argument layouts follow the original reply storage format (sdk 20.5.3: CalcDecidedEnter_ 0x43df54 reads 0x3f0 bytes and passes them to the callback as is; the game's "string changed" callback main 0x209dd18 reads
// a UTF-16 string at +0 and two cursor positions at +0x3f0 / +0x3f4): ChangedStringArg = { u16 str[0x1f4]; u32 length @0x3e8; s32 dic start @0x3ec; s32 dic end @0x3f0; s32 cursor @0x3f4 },
// DecidedEnterArg = { u16 str[0x1f4]; u32 length @0x3e8 }. InputText (filled by the game through InputText::Set, SDK code) = { u16 str[0x1f5] @0; … @0x3ec }.
#include "hle.h"
#include "front.h"

size_t swkbd_u16_to_u8(const uint16_t *s, size_t n, char *out, size_t cap);
size_t swkbd_u8_to_u16(const char *s, size_t max_units, uint16_t *out, size_t cap);

enum { STR_N = 0x1f4 };
typedef struct { uint16_t str[STR_N]; uint32_t len; int32_t dic_start, dic_end, cursor; } changed_arg;
typedef struct { uint16_t str[STR_N]; uint32_t len; uint32_t pad; } decided_arg;
_Static_assert(sizeof(changed_arg) == 0x3f8 && sizeof(decided_arg) == 0x3f0, "回調參數佈局 | callback argument layout");

static struct {
    void (*finished_init)(void), (*finished_kbd)(void), (*cancel)(void);
    void (*changed)(const changed_arg *), (*moved)(const void *), (*enter)(const decided_arg *);
    int inited, init_reported, appear_pending, shown, hide_pending, max_len;
    uint16_t text[STR_N + 1];
} g_ik;
static changed_arg g_changed; static decided_arg g_decided;   // 回調參數放宿主靜態區（客體與宿主同一地址空間） | callback arguments live in host static storage (guest and host share one address space)

HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard10InitializeEPv", "內嵌鍵盤：原版啓動小程序；宿主報成功，下一次 Calc 回調初始化完成", unsigned, h_ik_Init, void *self, void *work) {
    (void)self; (void)work; g_ik.inited = 1; g_ik.init_reported = 0; g_ik.shown = 0; g_ik.hide_pending = 0; LOG("swkbd 內嵌鍵盤：初始化（宿主接管，鍵盤畫面不給）"); return 1;
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard8FinalizeEv", "內嵌鍵盤收尾", void, h_ik_Fin, void *self) { (void)self; g_ik.inited = 0; LOG("swkbd 內嵌鍵盤：Finalize"); }
// 析構（D1）：原版會收掉小程序，宿主沒起小程序，只清宿主狀態（超級馬力歐 3D 世界 + 狂怒世界直接析構，上游 42a50ef）
// Destructor (D1): the original closes the applet; the host never started one and only clears its own state (Super Mario 3D World + Bowser's Fury destroys it directly, upstream 42a50ef)
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboardD1Ev", "內嵌鍵盤析構：沒起小程序，只清宿主狀態", void, h_ik_Dtor, void *self) { h_ik_Fin(self); }
// 畫面內存：原版按 1280x720 RGBA8 的間接圖層問 vi（服務不在就崩）；宿主給同樣大小，畫面不填
// Picture memory: on hardware vi is asked for a 1280x720 RGBA8 indirect layer (crashes without the service); the host gives the same size and never fills it
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard25GetImageMemoryRequirementEPmS2_", "鍵盤畫面內存需求：1280x720 RGBA8，0x1000 對齊", void, h_ik_ImgReq, void *self, uint64_t *size, uint64_t *align) {
    (void)self; if (size) *size = 0x3a0000; if (align) *align = 0x1000;
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard8GetImageEPvm", "鍵盤畫面：不給（返回 false），遊戲不畫鍵盤", unsigned, h_ik_GetImage, void *self, void *buf, uint64_t n) { (void)self; (void)buf; (void)n; return 0; }
HLE_FORCE("_ZNK2nn5swkbd14InlineKeyboard13GetWindowSizeEPiS2_", "鍵盤窗口大小：1280x720", void, h_ik_WinSize, const void *self, int32_t *w, int32_t *h) { (void)self; if (w) *w = 1280; if (h) *h = 720; }

typedef void (*cb_void)(void); typedef void (*cb_changed)(const changed_arg *); typedef void (*cb_moved)(const void *); typedef void (*cb_enter)(const decided_arg *);
#define SETCB(mangled, field, type) \
    HLE_FORCE(mangled, "內嵌鍵盤回調登記：宿主記下，在 Calc 裡按原版順序調", void, h_ik_##field, void *self, type fn) { (void)self; g_ik.field = fn; }
SETCB("_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE", finished_init, cb_void)
SETCB("_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE", finished_kbd, cb_void)
SETCB("_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE", cancel, cb_void)
SETCB("_ZN2nn5swkbd14InlineKeyboard24SetChangedStringCallbackEPFvPKNS0_16ChangedStringArgEE", changed, cb_changed)
SETCB("_ZN2nn5swkbd14InlineKeyboard22SetMovedCursorCallbackEPFvPKNS0_14MovedCursorArgEE", moved, cb_moved)
SETCB("_ZN2nn5swkbd14InlineKeyboard23SetDecidedEnterCallbackEPFvPKNS0_15DecidedEnterArgEE", enter, cb_enter)

HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard12SetInputTextERKNS0_9InputTextE", "鍵盤初始文本：宿主記下，彈輸入框時作初值", void, h_ik_SetText, void *self, const uint16_t *it) {
    (void)self; memset(g_ik.text, 0, sizeof g_ik.text);
    for (int i = 0; i < STR_N && it[i]; i++) g_ik.text[i] = it[i];
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard12SetCursorPosEi", "鍵盤光標位置：宿主輸入框自管，忽略", void, h_ik_SetCursor, void *self, int pos) { (void)self; (void)pos; }
// AppearArg（sdk 20）：字數上限的位置還沒實測，先記下整塊的頭 0x40 字節供核對，上限按 STR_N
// AppearArg (sdk 20): the position of the length limit is not measured yet, so the first 0x40 bytes are logged for checking and the limit is STR_N
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE", "鍵盤彈出：下一次 Calc 彈宿主輸入框", void, h_ik_Appear, void *self, const uint8_t *arg) {
    (void)self; g_ik.appear_pending = 1; g_ik.shown = 1; g_ik.hide_pending = 0; g_ik.max_len = STR_N;
    char hx[0x40 * 2 + 1]; for (int i = 0; i < 0x40; i++) snprintf(hx + 2 * i, 3, "%02x", arg[i]);
    LOG("swkbd 內嵌鍵盤：Appear（參數頭 %s）", hx);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard9DisappearEv", "鍵盤收起：撤掉待彈的輸入框；彈著的話下一次 Calc 報收完", void, h_ik_Disappear, void *self) {
    (void)self; g_ik.appear_pending = 0;
    if (g_ik.shown) { g_ik.shown = 0; g_ik.hide_pending = 1; LOG("swkbd 內嵌鍵盤：Disappear（下一次 Calc 報鍵盤已結束）"); }
}
// 回調在哪個模塊的哪裡（診斷：對著反匯編找遊戲收到回調後做什麼） | Which module and offset a callback lives at (diagnostics: to find in the disassembly what the game does on a callback)
static void cb_where(const char *what, void *fn) {
    module_t *m = fn ? module_of(fn) : NULL;
    if (m) LOG("swkbd 內嵌鍵盤：回調 %s = %s+%#llx", what, m->name, (unsigned long long)((uint8_t *)fn - m->base)); else LOG("swkbd 內嵌鍵盤：回調 %s = %p", what, fn);
}

// 每幀：按原版順序發回調。初始化完成只發一次；彈出請求在這裡同步彈宿主輸入框（鍵盤彈著期間凍結客體時鐘，同 hle_swkbd.c）
// Every frame: send callbacks in the original order. "Initialization finished" is sent once; an appear request brings up the host input box synchronously here (the guest clock is frozen while it is up, as in hle_swkbd.c)
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard4CalcEv", "內嵌鍵盤每幀：按原版順序發回調，彈宿主輸入框", void, h_ik_Calc, void *self) {
    (void)self;
    if (!g_ik.inited) return;
    if (!g_ik.init_reported) {
        g_ik.init_reported = 1;
        static int once; if (!once++) { cb_where("初始化完成", (void *)g_ik.finished_init); cb_where("鍵盤已結束", (void *)g_ik.finished_kbd); cb_where("取消", (void *)g_ik.cancel);
                                         cb_where("文本已變", (void *)g_ik.changed); cb_where("光標已移", (void *)g_ik.moved); cb_where("確定", (void *)g_ik.enter); }
        if (g_ik.finished_init) g_ik.finished_init();
    }
    if (g_ik.hide_pending) { g_ik.hide_pending = 0; if (g_ik.finished_kbd) g_ik.finished_kbd(); }
    if (!g_ik.appear_pending) return;
    g_ik.appear_pending = 0;
    char init[STR_N * 3 + 1], text[STR_N * 3 + 1];
    swkbd_u16_to_u8(g_ik.text, STR_N, init, sizeof init);
    const char *script = getenv("DM_KBD");
    if (!script) guest_clock_freeze_for(2, 1);
    int r = script ? (snprintf(text, sizeof text, "%s", script), 1) : front_text_input("", "", "", init, g_ik.max_len, text, sizeof text);
    if (!script) guest_clock_freeze_for(2, 0);
    LOG("swkbd 內嵌鍵盤：初始「%s」→ %s%s%s", init, r > 0 ? "確定「" : r == 0 ? "取消" : "無窗口按取消", r > 0 ? text : "", r > 0 ? "」" : "");
    if (r > 0) {
        memset(&g_changed, 0, sizeof g_changed); memset(&g_decided, 0, sizeof g_decided);
        size_t n = swkbd_u8_to_u16(text, (size_t)g_ik.max_len, g_changed.str, STR_N);
        g_changed.len = (uint32_t)n; g_changed.dic_start = g_changed.dic_end = g_changed.cursor = (int32_t)n;
        memcpy(g_decided.str, g_changed.str, sizeof g_decided.str); g_decided.len = (uint32_t)n;
        memcpy(g_ik.text, g_changed.str, sizeof g_changed.str);
        if (g_ik.changed) g_ik.changed(&g_changed);
        if (g_ik.enter) g_ik.enter(&g_decided);
    } else if (g_ik.cancel) g_ik.cancel();
}
