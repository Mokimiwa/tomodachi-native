// swkbd 軟鍵盤（系統小程序）→ 宿主原生輸入框。MakePreset / SetInitialTextUtf8 / GetRequired*Size 留在 SDK 原樣跑，
// 這裡只讀它們寫出的 ShowKeyboardArg，並按真 swkbd 小程序的輸出約定回寫 String。
// 佈局出處（sdk 3.3.94012 原映像偏移）：
//   ShowKeyboard 0x3a6770 逐字段搬 KeyboardConfig 進小程序參數：+0x24 標題 u16[65]、+0xa6 副標題 u16[129]、+0x1a8 提示 u16[257]、
//     +0x3ac 最大長度、+0x3b0 最小長度、+0x3bd isUseUtf8、+0x3c0 初始文本在工作緩衝內的偏移、+0x3c4 初始文本 UTF-16 長度；
//     工作緩衝指針 / 大小在 +0x4d0 / +0x4d8（隨 CreateLargeStorage 一起交給小程序）
//   SetInitialTextUtf8 0x3a74c0：UTF-8 → UTF-16 寫進 工作緩衝+偏移（偏移 0x14，至多 500 字 + 結尾 0）
//   ReadCloseResultAndString 0x3a63f0：輸出存儲 = u32 CloseResult + 文本區；文本區原樣拷進 String{+0 指針, +8 字節數}
//   ShowKeyboard 收尾 0x3a6acc：CloseResult == 0（確定）→ Result 0，否則（取消）→ 0x29F；GetRequiredStringBufferSize = 0x7D4
// 遊戲側（main 0xc6deac）MakePreset 後置 isUseUtf8 = 1，讀回按 C 串（0x1b66640 strlen）；UTF-16 輸出按原約定一並支持。
// 無窗口且未設 DM_KBD：按取消返回（不彈窗、不卡住）。DM_KBD="文本"：不彈窗，直接按確定返回該文本（無窗口測試 / 對拍）。
// swkbd software keyboard (system applet) → host native input box. MakePreset / SetInitialTextUtf8 / GetRequired*Size are left to run as is in the SDK,
// here we only read the ShowKeyboardArg they write and write back String per the output convention of the real swkbd applet.
// Layout sources (offsets in the original sdk 3.3.94012 image):
//   ShowKeyboard 0x3a6770 moves KeyboardConfig field by field into the applet arguments: +0x24 title u16[65], +0xa6 subtitle u16[129], +0x1a8 hint u16[257],
//     +0x3ac maximum length, +0x3b0 minimum length, +0x3bd isUseUtf8, +0x3c0 offset of the initial text within the work buffer, +0x3c4 UTF-16 length of the initial text;
//     work buffer pointer / size at +0x4d0 / +0x4d8 (handed to the applet together with CreateLargeStorage)
//   SetInitialTextUtf8 0x3a74c0: UTF-8 → UTF-16 written to work buffer+offset (offset 0x14, at most 500 characters + terminating 0)
//   ReadCloseResultAndString 0x3a63f0: output storage = u32 CloseResult + text area; the text area is copied as is into String{+0 pointer, +8 byte count}
//   ShowKeyboard wrap-up 0x3a6acc: CloseResult == 0 (OK) → Result 0, otherwise (cancel) → 0x29F; GetRequiredStringBufferSize = 0x7D4
// Game side (main 0xc6deac) sets isUseUtf8 = 1 after MakePreset and reads back as a C string (0x1b66640 strlen); UTF-16 output is supported as well per the original convention.
// No window and DM_KBD unset: return as cancel (no popup, no hang). DM_KBD="text": no popup, return that text directly as OK (windowless testing / lockstep comparison).
#include "hle.h"
#include <unistd.h>
#include "front.h"

enum { ARG_HEADER = 0x24, ARG_SUB = 0xa6, ARG_GUIDE = 0x1a8, ARG_MAXLEN = 0x3ac, ARG_MINLEN = 0x3b0, ARG_UTF8 = 0x3bd,
       ARG_INIT_OFF = 0x3c0, ARG_INIT_LEN = 0x3c4, ARG_WORKBUF = 0x4d0,
       HEADER_N = 65, SUB_N = 129, GUIDE_N = 257 };
#define SWKBD_RESULT_CANCELED 0x29Fu   // swkbd 模塊 159 / 描述 1：SDK 在 CloseResult != 0 時返回的常數 | swkbd module 159 / description 1: the constant the SDK returns when CloseResult != 0

// UTF-16（至多 n 個單元，遇 0 止）→ UTF-8，寫 out（cap 含結尾 0），返回寫入字節數
// UTF-16 (at most n units, stops at 0) → UTF-8, written to out (cap includes the terminating 0), returns the number of bytes written
size_t swkbd_u16_to_u8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        char b[4]; int k = c < 0x80 ? (b[0] = (char)c, 1) : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), 2)
                 : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), 3)
                 : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 63)), b[2] = (char)(0x80 | (c >> 6 & 63)), b[3] = (char)(0x80 | (c & 63)), 4);
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k); o += k;
    }
    if (cap) out[o] = 0;
    return o;
}
// UTF-8 → UTF-16，至多 max_units 個單元（不拆代理對），寫 out（cap 單元含結尾 0），返回單元數
// UTF-8 → UTF-16, at most max_units units (surrogate pairs are not split), written to out (cap units including the terminating 0), returns the unit count
size_t swkbd_u8_to_u16(const char *s, size_t max_units, uint16_t *out, size_t cap) {
    size_t o = 0; const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t c; int k = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
        c = k == 1 ? *p : k == 2 ? *p & 31 : k == 3 ? *p & 15 : *p & 7;
        for (int j = 1; j < k; j++) { if ((p[j] & 0xC0) != 0x80) { k = j; break; } c = c << 6 | (p[j] & 63); }
        size_t need = c >= 0x10000 ? 2 : 1;
        if (o + need > max_units || o + need + 1 > cap) break;
        if (need == 2) { c -= 0x10000; out[o++] = (uint16_t)(0xD800 | c >> 10); out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF)); }
        else out[o++] = (uint16_t)c;
        p += k;
    }
    if (cap) out[o] = 0;
    return o;
}

// 核心（單測直接調）：從 arg 解出顯示參數，經 input 取得用戶文本（返回 1 確定 / 0 取消 / -1 無窗口），按 arg 約定回寫 out
// 遊戲的離線角色名規則（原映像，逐字節判）：2–15 字節、首尾必須「字母」、'-' '_' 合計最多一個，其餘必須是字母。原版只認 ASCII 字母；
// hle_charname.c 把 ≥0x80 的字節也放行（中文名），所以規則變成：英文字母或漢字，UTF-8 合計不超過 15 字節（漢字每個 3 字節，最多 5 個）。
// 起名面板開著時讓前端按這條規則當場攔，並把規則寫在對話框裡。diag.c 按界面消息維護這個標誌。
// Core (called directly by unit tests): decode the display parameters from arg, get the user text through input (returns 1 OK / 0 cancel / -1 no window), write back out per the arg convention
// The game's offline character name rule (original image, judged byte by byte): 2–15 bytes, first and last must be a "letter", '-' and '_' at most one in total, the rest must be letters. The original game only accepts ASCII letters;
// hle_charname.c also lets bytes ≥0x80 through (Chinese names), so the rule becomes: English letters or Chinese characters, at most 15 UTF-8 bytes in total (3 bytes per Chinese character, at most 5).
// While the naming panel is open the front end blocks on the spot by this rule and writes the rule in the dialog. diag.c maintains this flag from UI messages.
int g_swkbd_name_panel;
int g_swkbd_numeric_max = -1;   // ≥0 = 這一次輸入是數量框、值 = 上限（dm_textinput.c 置，前端輸入框讀，用完置回 -1） | ≥0 = this input is a quantity box, value = upper limit (set by dm_textinput.c, read by the front end input box, set back to -1 after use)
int (*g_swkbd_unfilter)(void);   // 宿主登記（hle_charname.c 的 dm_charname_unfilter；單測裡為空）：起名面板的鍵盤請求不過遊戲的 ASCII 鍵入過濾 | registered by the host (dm_charname_unfilter in hle_charname.c; NULL in unit tests): keyboard requests from the naming panel bypass the game's ASCII typing filter
typedef int (*swkbd_input_fn)(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);
Result swkbd_show(void *out, const void *arg_, swkbd_input_fn input) {
    const uint8_t *arg = arg_;
    char title[HEADER_N * 3 + 1], sub[SUB_N * 3 + 1], guide[GUIDE_N * 3 + 1], init[501 * 3 + 1] = "", text[1002 * 3 + 1];
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_HEADER), HEADER_N, title, sizeof title);
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_SUB), SUB_N, sub, sizeof sub);
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_GUIDE), GUIDE_N, guide, sizeof guide);
    int32_t maxlen, ilen, ioff; memcpy(&maxlen, arg + ARG_MAXLEN, 4); memcpy(&ilen, arg + ARG_INIT_LEN, 4); memcpy(&ioff, arg + ARG_INIT_OFF, 4);
    const uint8_t *work; memcpy(&work, arg + ARG_WORKBUF, 8);
    if (ilen > 0 && work) swkbd_u16_to_u8((const uint16_t *)(work + ioff), (size_t)ilen, init, sizeof init);

    const char *script = getenv("DM_KBD");
    // 鍵盤彈著期間凍結客體時鐘（離線單機裡玩家打字時怪不該照打、回來也不該補拍；這裡按 iOS 原生體驗，不守 Switch「小程序佔前台時鐘照走」的原版語義）
    // Freeze the guest clock while the keyboard is up (in offline single-player, monsters should not keep attacking while the player types, nor should ticks be caught up on return; this follows the native iOS experience, not the original game's Switch semantics of "the clock keeps running while an applet holds the foreground")
    if (!script) guest_clock_freeze_for(2, 1);
    int r = script ? (snprintf(text, sizeof text, "%s", script), 1) : input(title, sub, guide, init, maxlen > 0 ? maxlen : 0, text, sizeof text);
    if (!script) guest_clock_freeze_for(2, 0);
    g_swkbd_numeric_max = -1;   // 數字框標記只管這一次（dm_textinput.c 在彈鍵盤前置） | the numeric box mark applies to this one time only (set by dm_textinput.c before bringing up the keyboard)
    LOG("swkbd：標題「%s」初始「%s」上限 %d → %s%s%s", title, init, maxlen,
        r > 0 ? "確定「" : r == 0 ? "取消" : "無窗口按取消", r > 0 ? text : "", r > 0 ? "」" : "");
    if (r <= 0) return SWKBD_RESULT_CANCELED;
    if (g_swkbd_name_panel && g_swkbd_unfilter && g_swkbd_unfilter()) LOG("swkbd：起名面板——已關掉遊戲對返回串的 ASCII 鍵入過濾（中文名放行，見 hle_charname.c）");

    uint8_t *buf; uint64_t cap; memcpy(&buf, (uint8_t *)out, 8); memcpy(&cap, (uint8_t *)out + 8, 8);
    if (!buf || !cap) return RES_OK;                         // 與 SDK 同：String 未給緩衝則不讀文本 | same as the SDK: if String has no buffer, the text is not read
    memset(buf, 0, cap);
    uint16_t u16[1003]; size_t n = swkbd_u8_to_u16(text, maxlen > 0 ? (size_t)maxlen : 1002, u16, 1003);   // 按字數上限截斷（UTF-16 單元計） | truncate to the character limit (counted in UTF-16 units)
    if (arg[ARG_UTF8]) swkbd_u16_to_u8(u16, n, (char *)buf, cap);
    else memcpy(buf, u16, (n + 1) * 2 <= cap ? n * 2 : (cap / 2 - 1) * 2);
    { char fin[64]; swkbd_u16_to_u8(u16, n, fin, sizeof fin); void dm_ui_text_entered(const char *text); dm_ui_text_entered(n > 0 ? fin : NULL); }
    { const char *lag = getenv("DM_TEST_SWKBD_LAG_MS"); if (lag) usleep((useconds_t)atoi(lag) * 1000); }   // 測試：把「鍵盤已交回 → 遊戲那條線程拿到結果」拖慢，驗自動接受不搶跑   // 前端路徑理順（dm_cursor.c）：起名頁輸完名字自動接受 | test: slow down "keyboard handed back → the game's thread gets the result" to verify that auto-accept does not jump the gun   // front end path cleanup (dm_cursor.c): auto-accept after the name is typed on the naming page
    return RES_OK;
}

HLE_FORCE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", "軟鍵盤小程序 → 宿主原生輸入框（DM_KBD 直給文本）",
          Result, h_swkbd_Show, void *out, const void *arg) {
    return swkbd_show(out, arg, front_text_input);
}
