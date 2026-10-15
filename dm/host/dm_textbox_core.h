#pragma once
// 文字框顏色碼對照表（實現與說明見 dm_textbox_core.c）
// Text box color code mapping table (implementation and notes in dm_textbox_core.c)
#include <stddef.h>
int dm_textbox_load(const char *path);    // 讀表，返回條數；打不開返回 -1（表清空） | Read the table, return the entry count; return -1 if it cannot be opened (table cleared)
void dm_textbox_reset(void);
int dm_textbox_count(void);
// 按表改寫：0 = 不動（*out 為空）；1 = 整串命中；2 = 換了其中的注釋版子串。*out 由調用者 free
// Rewrite by the table: 0 = unchanged (*out is NULL); 1 = whole-string hit; 2 = annotated substrings inside were replaced. *out is freed by the caller
int dm_textbox_rewrite(const char *s, size_t n, char **out, size_t *on);
// 讀對照表（只讀一次，不掛門；實現在 dm_textbox.c）：返回 1 = 有表，0 = 沒有（物品注釋關著 / 沒有覆蓋層）
// Read the mapping table (read only once, does not hook the gate; implemented in dm_textbox.c): returns 1 = table present, 0 = none (item annotations off / no overlay)
int dm_textbox_ensure_loaded(void);
