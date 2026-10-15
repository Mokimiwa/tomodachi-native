// 控件類型核對（dm_widget.h）的單測，秒級。假「主模塊」是一塊內存，控件第一個字 = 基址 + 虛表偏移。
// Unit test for the widget type check (dm_widget.h), runs in seconds. The fake "main module" is a block of memory; a widget's first word = base + vtable offset.
#include "dm_widget.h"
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static uint8_t base[16]; static uint64_t w_toggle[4], w_text[4], w_slot[4], w_slotb[4], w_slotc[4], w_other[4];
    w_toggle[0] = (uint64_t)(uintptr_t)base + DM_VT_TOGGLE; w_text[0] = (uint64_t)(uintptr_t)base + DM_VT_TEXT; w_other[0] = (uint64_t)(uintptr_t)base + 0x1234;
    w_slot[0] = (uint64_t)(uintptr_t)base + DM_VT_SLOT; w_slotb[0] = (uint64_t)(uintptr_t)base + DM_VT_SLOT_B; w_slotc[0] = (uint64_t)(uintptr_t)base + DM_VT_SLOT_C;
    uint64_t tg = (uint64_t)(uintptr_t)w_toggle, tx = (uint64_t)(uintptr_t)w_text, ot = (uint64_t)(uintptr_t)w_other;

    CHECK(dm_widget_vt(base, tg) == DM_VT_TOGGLE, "勾選框的虛表偏移應讀到 %#x，讀到 %#llx", DM_VT_TOGGLE, (unsigned long long)dm_widget_vt(base, tg));
    CHECK(dm_widget_is(base, tg, DM_VT_TOGGLE), "勾選框應認作勾選框");
    CHECK(!dm_widget_is(base, tx, DM_VT_TOGGLE), "文字框不該認作勾選框（同名控件換了類時就是這種情況）");
    CHECK(dm_widget_is(base, tx, DM_VT_TEXT), "文字框應認作文字框");
    CHECK(!dm_widget_is(base, ot, DM_VT_TOGGLE) && !dm_widget_is(base, ot, DM_VT_TEXT), "不認識的類一律不認");
    CHECK(!dm_widget_is(base, 0, DM_VT_TOGGLE) && dm_widget_vt(base, 0) == 0, "空控件：不認、虛表讀 0");
    CHECK(!dm_widget_is(NULL, tg, DM_VT_TOGGLE) && dm_widget_vt(NULL, tg) == 0, "沒有基址：不認、虛表讀 0");
    CHECK(dm_widget_is_slot(base, (uint64_t)(uintptr_t)w_slot) && dm_widget_is_slot(base, (uint64_t)(uintptr_t)w_slotb) && dm_widget_is_slot(base, (uint64_t)(uintptr_t)w_slotc), "三種物品格都認作物品格");
    CHECK(!dm_widget_is_slot(base, tg) && !dm_widget_is_slot(base, 0), "勾選框 / 空控件不是物品格");
    // 各類的虛表值兩兩不同（抄錯一個數會讓兩類互認）
    // Vtable values of the classes are pairwise distinct (one mistyped number would make two classes match each other)
    { const uint64_t v[] = { DM_VT_TEXT, DM_VT_INPUT, DM_VT_NUMINPUT, DM_VT_TOGGLE, DM_VT_TABBAR, DM_VT_SLOT, DM_VT_SLOT_B, DM_VT_SLOT_C, DM_VT_IMAGE, DM_VT_BUTTON }; int dup = 0;
      for (unsigned i = 0; i < sizeof v / sizeof *v; i++) for (unsigned j = i + 1; j < sizeof v / sizeof *v; j++) dup += v[i] == v[j];
      CHECK(dup == 0, "虛表常量有 %d 對重復", dup); }

    printf("t_widget：%d 項，%d 項不通\n", checks, fails); return fails != 0;
}
