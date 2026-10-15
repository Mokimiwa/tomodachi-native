// 按偏移讀寫原版界面控件的「類專有字段」之前，先核它是不是預期的那個類（虛表）。
// 來由：按名字找到的控件不保證是當初量偏移時的那個類（佈局改了、同名控件換了類、原版把槽位換成別的控件），按偏移寫下去就是往別的對象裡亂寫。
// 所有控件共有的基類字段（名字 +8、可見 +0x50 / +0x51、子控件 +0x58 / +0x60、矩形 +0x70）不用核。
// 虛表值是 main 模塊內偏移（3.3.94012），宿主實讀：
// Before reading or writing "class-specific fields" of a native UI widget by offset, check that it is the expected class (vtable).
// Why: a widget found by name is not guaranteed to be the class the offsets were measured on (layout changed, a same-named widget changed class, the game swapped the slot for another widget), and writing by offset then scribbles into some other object.
// Base-class fields shared by all widgets (name +8, visible +0x50 / +0x51, children +0x58 / +0x60, rect +0x70) need no check.
// Vtable values are offsets within the main module (3.3.94012), read on the host:
#pragma once
#include <stdint.h>

enum {
    DM_VT_TEXT = 0x20ba4c8,      // 文字框：串 +0x88 / 長 +0x90，字色 +0xbc | text box: string +0x88 / length +0x90, text color +0xbc
    DM_VT_INPUT = 0x20b8718,     // 輸入框：正式文字串 +0x558 | input box: committed text string +0x558
    DM_VT_NUMINPUT = 0x20b91c0,  // 數字輸入框 | numeric input box
    DM_VT_TOGGLE = 0x20bc110,    // 勾選框 / 子頁簽（ToggleButtonWidget）：勾上 +0xa88，四個幀號 +0x6e8.. ，此刻的幀 +0x610 | checkbox / sub-tab (ToggleButtonWidget): checked +0xa88, four frame ids at +0x6e8.., current frame +0x610
    DM_VT_TABBAR = 0x20ba3a8,    // 頁籤條（TabBarWidget） | tab bar (TabBarWidget)
    DM_VT_SLOT = 0x2089fa0,      // 物品格：部位號 +0x5d8、屬主 +0x5c8 | item slot: body location +0x5d8, owner +0x5c8
    DM_VT_SLOT_B = 0x2089ab8,    // 物品格（另一種，dm_actions.c / dm_runeword.c 認的） | item slot (another kind, recognized by dm_actions.c / dm_runeword.c)
    DM_VT_SLOT_C = 0x2088fc8,    // 物品格（dm_cursor_item.c 認的第三種） | item slot (the third kind, recognized by dm_cursor_item.c)
    DM_VT_IMAGE = 0x20b8650,     // 圖片控件：此刻的幀 +0x90（宿主實讀：編年史頁的 Img） | image widget: current frame +0x90 (read on the host: Img on the Chronicle page)
    DM_VT_BUTTON = 0x20b7b00,    // 按鈕：四個幀號 +0x6e8..，此刻的幀 +0x610（宿主實讀：秘境選層頁的 TouchRF_M1） | button: four frame ids at +0x6e8.., current frame +0x610 (read on the host: TouchRF_M1 on the Greater Rift level select page)
};

static inline uint64_t dm_widget_vt(const uint8_t *base, uint64_t w) { return w && base ? *(const uint64_t *)(uintptr_t)w - (uint64_t)(uintptr_t)base : 0; }
static inline int dm_widget_is(const uint8_t *base, uint64_t w, uint64_t vt) { return w && base && dm_widget_vt(base, w) == vt; }
static inline int dm_widget_is_slot(const uint8_t *base, uint64_t w) { uint64_t v = dm_widget_vt(base, w); return v == DM_VT_SLOT || v == DM_VT_SLOT_B || v == DM_VT_SLOT_C; }

// 調用點用：是預期的類 → 1；不是 → 0，並記一次紅（每個文件最多 5 次，不刷屏）。用它的文件要有 LOG（hle.h），並在文件裡放一次 DM_WIDGET_RED_DEF
// For call sites: expected class → 1; otherwise → 0 and log one failure (at most 5 per file, no log flooding). A file using it needs LOG (hle.h) and one DM_WIDGET_RED_DEF in the file
#define DM_WIDGET_OK(base, w, vt, who) (dm_widget_is((base), (w), (vt)) ? 1 : dm_widget_red_((base), (w), (vt), (who)))
#define DM_WIDGET_RED_DEF \
    static int dm_widget_red_(const uint8_t *base, uint64_t w, uint64_t vt, const char *who) { static int n; \
        if (w && base && n++ < 5) { const char *nm = *(const char **)(uintptr_t)(w + 8); \
            LOG("【紅】%s：控件「%s」不是預期的類（虛表 main+%#llx，應為 %#llx），不按偏移讀寫", who, nm ? nm : "?", (unsigned long long)dm_widget_vt(base, w), (unsigned long long)vt); } \
        return 0; }
