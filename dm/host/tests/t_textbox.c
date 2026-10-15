// dm_textbox_core 單測：期望全部手寫。U+E07E = EE 81 BE。
// 判據：表裡的注釋版整串 / 子串換成文字框版；表外的串（含原版自帶的帶碼串、不帶碼的串）逐字節不動；沒有表時一律不動。
// dm_textbox_core unit test: all expectations are hand-written. U+E07E = EE 81 BE.
// Criterion: whole strings / substrings of the annotated version found in the table are replaced by the text box version; strings outside the table (including the original game's own coded strings and strings without codes) are untouched byte for byte; with no table nothing is touched.
#include "../dm_textbox_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C "\xee\x81\xbe"
static int fails, checks;
static void expect(const char *what, const char *in, int want_k, const char *want) {
    char *o; size_t on; int k = dm_textbox_rewrite(in, strlen(in), &o, &on); checks++;
    int ok = k == want_k && (want ? (o && on == strlen(want) && !memcmp(o, want, on)) : !o);
    if (!ok) { fails++; printf("✗ %s：得到 k=%d「%.*s」\n", what, k, o ? (int)on : 0, o ? o : ""); } else printf("✓ %s\n", what);
    free(o);
}
int main(void) {
    char path[] = "/tmp/t_textbox_XXXXXX"; int fd = mkstemp(path); FILE *f = fdopen(fd, "w");
    fprintf(f, "# 注釋版\t文字框版\n");
    fprintf(f, "%s\t%s\n", C "5[強傷60-70%]\\n" C "4死亡之鏟", "死亡之鏟");   // 範圍行 + 名字 | range line + name
    fprintf(f, "%s\t%s\n", C "1懷特之腳", "懷特之腳");                         // 紅葉名字色 | Hongye mod name color
    fclose(f);
    expect("沒有表：帶碼串不動", C "1懷特之腳", 0, NULL);
    if (dm_textbox_load(path) != 2) { printf("✗ 讀表條數不是 2\n"); return 1; }
    expect("表裡的注釋版整串 → 文字框版", C "5[強傷60-70%]\n" C "4死亡之鏟", 1, "死亡之鏟");
    expect("紅葉名字色整串 → 原名", C "1懷特之腳", 1, "懷特之腳");
    expect("被拼進長句的注釋版只換那一段", "發現編年史物品：" C "1懷特之腳" "！", 2, "發現編年史物品：懷特之腳！");
    expect("同一句裡出現兩次都換", C "1懷特之腳" "和" C "1懷特之腳", 2, "懷特之腳和懷特之腳");
    expect("原版自帶的帶碼串（悄悄話）不在表裡：不動", C "0你悄悄地對" C "1某人" C "0說: 你好", 0, NULL);
    expect("不帶碼的串：不動", "死亡之鏟", 0, NULL);
    expect("多行原版文字（不帶碼）：不動", "第一行\n第二行", 0, NULL);
    expect("像注釋但表裡沒有的：不動", C "5[強傷1-2%]\n" C "4死亡之鏟", 0, NULL);
    unlink(path); dm_textbox_reset();
    expect("清表後：不動", C "1懷特之腳", 0, NULL);
    printf("TEXTBOX %s checks=%d failures=%d\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
