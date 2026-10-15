// 手柄 / 觸屏切換事務單測（秒級）：dm_modetx.h 的狀態機與輸入閘，不碰遊戲。期望值都從規則手算：
// 重建完成 = 提示面板重開次數動過之後連續 SETTLE 幀不動；TIMEOUT 幀沒等到按超時往下走；凍結期間按著過的鍵放開後要鬆開過一次才交。
// Unit test of the controller / touch switch transaction (takes seconds): the state machine and input gate of dm_modetx.h, without touching the game. Expected values are all computed by hand from the rules:
// rebuild complete = SETTLE consecutive frames without movement after the hint panel reopen count has moved; if that has not happened within TIMEOUT frames, proceed as a timeout; keys that were held during the freeze are passed to the game only after they have been released once following the unfreeze.
#include <stdio.h>
#include "../dm_modetx.h"
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(void) {
    { dm_modetx t = {0}; uint64_t f = 100;   // 一次正常切換：觸屏(0) → 手柄(1) | One normal switch: touch (0) → controller (1)
      CHECK(dm_modetx_step(&t, f, -1, 0, 7) == DM_TXA_NONE, "沒請求不該動");
      CHECK(dm_modetx_step(&t, f, 0, 0, 7) == DM_TXA_NONE, "想要的就是當前模式不該動");
      CHECK(dm_modetx_step(&t, f, 1, 0, 7) == DM_TXA_BEGIN && t.st == DM_TX_REBUILD1, "請求另一模式應開始");
      for (int i = 1; i <= 10; i++) CHECK(dm_modetx_step(&t, f + i, 1, 0, 7) == DM_TXA_NONE, "提示面板沒重開前不該往下走（第 %d 幀）", i);
      CHECK(dm_modetx_step(&t, f + 11, 1, 0, 8) == DM_TXA_NONE, "剛重開那一幀不算完成");
      CHECK(dm_modetx_step(&t, f + 13, 1, 0, 9) == DM_TXA_NONE, "又重開一次：重新計穩定");
      CHECK(dm_modetx_step(&t, f + 16, 1, 0, 9) == DM_TXA_NONE, "穩定 3 幀不夠");
      CHECK(dm_modetx_step(&t, f + 17, 1, 0, 9) == DM_TXA_REPORT_BACK && t.st == DM_TX_REBUILD2, "穩定 4 幀 → 報回原運行模式");
      CHECK(dm_modetx_step(&t, f + 30, 0, 1, 9) == DM_TXA_NONE, "事務中來的反向請求不打斷");
      CHECK(dm_modetx_step(&t, f + 31, 0, 1, 10) == DM_TXA_NONE, "第二次重建剛開始");
      CHECK(dm_modetx_step(&t, f + 35, 0, 1, 10) == DM_TXA_DONE && t.st == DM_TX_IDLE && !t.timeouts, "第二次重建穩定 → 完成、無超時");
      CHECK(dm_modetx_step(&t, f + 36, 0, 1, 10) == DM_TXA_BEGIN, "完成後下一幀處理排著的反向請求"); }
    { dm_modetx t = {0};   // 超時：提示面板一直沒重開 | Timeout: the hint panel never reopens
      CHECK(dm_modetx_step(&t, 0, 1, 0, 0) == DM_TXA_BEGIN, "開始");
      CHECK(dm_modetx_step(&t, DM_MODETX_TIMEOUT - 1, 1, 0, 0) == DM_TXA_NONE, "差 1 幀不超時");
      CHECK(dm_modetx_step(&t, DM_MODETX_TIMEOUT, 1, 0, 0) == DM_TXA_REPORT_BACK && t.timeouts == 1, "到點按超時往下走並記數");
      CHECK(dm_modetx_step(&t, 2 * DM_MODETX_TIMEOUT - 1, 1, 0, 0) == DM_TXA_NONE, "第二段重新計時");
      CHECK(dm_modetx_step(&t, 2 * DM_MODETX_TIMEOUT, 1, 0, 0) == DM_TXA_DONE && t.timeouts == 2 && t.st == DM_TX_IDLE, "第二段也超時 → 完成、記 2 次"); }
    { dm_modegate g = {0}; uint64_t b;   // 輸入閘：A=0x1 觸發切換並一直按著，途中又按了 B=0x2，鬆開 B 後再按 | Input gate: A=0x1 triggers the switch and stays held, B=0x2 is also pressed along the way, and after releasing B it is pressed again
      b = 0x1; CHECK(dm_modegate_apply(&g, 1, &b) == 1 && b == 0, "凍結：鍵清零、搖桿也清");
      b = 0x3; CHECK(dm_modegate_apply(&g, 1, &b) == 1 && b == 0, "凍結中新按的鍵也不交");
      b = 0x3; CHECK(dm_modegate_apply(&g, 0, &b) == 0 && b == 0, "放開後還按著的 A、B 不交");
      b = 0x5; CHECK(dm_modegate_apply(&g, 0, &b) == 0 && b == 0x4, "B 鬆了、A 還按著：新按的 X=0x4 照常交，A 仍不交");
      b = 0x0; CHECK(dm_modegate_apply(&g, 0, &b) == 0 && b == 0, "全鬆開");
      b = 0x3; CHECK(dm_modegate_apply(&g, 0, &b) == 0 && b == 0x3, "鬆開過之後再按照常交"); }
    printf("t_modetx: %d 項，%d 失敗\n", checks, fails); return fails != 0;
}
