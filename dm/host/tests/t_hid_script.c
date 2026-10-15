// HID腳本單測：假時鐘逐幀推進，真實解析DM_KEYS、真實GetNpad路徑；不睡眠、不按時長跳步驟。
// Begin是動作開始/重復觸發，End標記動作結束：兩個Begin只需同一動作的一次End。
// HID script unit test: a fake clock advances frame by frame, with real DM_KEYS parsing and the real GetNpad path; no sleeping, no skipping steps by duration.
// Begin is action start/repeat trigger and End marks action end: two Begins need only one End of the same action.
#ifndef HLE_HID_SOURCE
#define HLE_HID_SOURCE "../hle_hid.c"
#endif
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include HLE_HID_SOURCE
module_t g_mod[MAX_MOD]; int g_nmod;
static uint64_t clock_ns = 1000000000ull;
static uint64_t guest_frame;
static uint64_t server_tick;
static int server_available;
uint64_t now_ns(void) { return clock_ns; }
uint64_t guest_ns(void) { return clock_ns; }
uint64_t dm_guest_frame(void) { return guest_frame; }
uint64_t dm_server_tick(void) { return server_available ? server_tick : UINT64_MAX; }
int touch_fill(void *out, uint64_t max) { (void)out; (void)max; return 0; }
static int test_loadscreen_up, hold_requests;
int dm_loadscreen_up(void) { return test_loadscreen_up; }                 // 真判定在 diag.c；單測手動置位 | the real check is in diag.c; the unit test sets it by hand
void dm_hold_for_loadscreen(void) { hold_requests++; }                     // 真實現凍結客體時鐘；單測只數請求次數 | the real implementation freezes the guest clock; the unit test only counts requests
int dm_opmode(void) { return 1; }
void dm_touch_set(const dm_touch *t, int n) { (void)t; (void)n; }
static void (*ui_cb)(const char *, const char *, const char *);
void uimsg_subscribe(void (*f)(const char *, const char *, const char *)) { ui_cb = f; }
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", s); } while (0)
static void setup(const char *script) {
    memset(g_steps, 0, sizeof g_steps); memset(g_keys, 0, sizeof g_keys);
    g_nsteps = g_nkeys = g_kcur = g_ktries = g_kbeg = g_kend = 0; g_kstate = KS_PRE; g_krel = 1;
    clock_ns = 1000000000ull; guest_frame = 0; server_tick = 0; server_available = 1; setenv("DM_KEYS", script, 1); keys_parse();
}
static void setup_pregame(const char *script) {
    memset(g_steps, 0, sizeof g_steps); memset(g_keys, 0, sizeof g_keys);
    g_nsteps = g_nkeys = g_kcur = g_ktries = g_kbeg = g_kend = 0; g_kstate = KS_PRE; g_krel = 1;
    clock_ns = 1000000000ull; guest_frame = 0; server_tick = 0; server_available = 0; setenv("DM_KEYS", script, 1); keys_parse();
}
static void ui(const char *act) { ui_cb("InputMessage", act, ""); }
static NpadState read_state(void) { NpadState s; fill(&s, 0, STYLE_FULLKEY); return s; }
static int neutral(NpadState s) { return !s.buttons && !s.lx && !s.ly; }
static void release(void) { guest_frame += ACTION_HOLD_TICKS; server_tick += ACTION_HOLD_TICKS; read_state(); }
static void frames(uint64_t n) { guest_frame += n; server_tick += n; read_state(); }
int main(int argc, char **argv) {
    unsetenv("DM_KEYS"); hid_keys_init();
    setup("LSL@+0.05,LSU@+0.05");
    NpadState s = read_state(); CHECK(s.lx == -32767 && s.ly == 0 && s.buttons == 0, "首步LSL送入真手柄佈局，不進按鈕位");
    ui("ControllerActionBegin"); ui("ControllerActionBegin"); release();
    CHECK(neutral(read_state()), "兩個Begin尚未End時，下一步仍不放行");
    ui("ControllerActionEnd"); s = read_state();
    CHECK(s.ly == 32767 && s.lx == 0, "兩個重復Begin/一次End後，下一步LSU放行");

    setup("A@+0.05,B@+0.05"); CHECK(read_state().buttons == DM_BTN_A, "普通A按下");
    ui("ControllerActionBegin"); release();
    CHECK(neutral(read_state()), "單次Begin未End不放下一鍵");
    ui("ControllerActionEnd"); CHECK(read_state().buttons == DM_BTN_B, "End後才發B，不吞鍵");

    setup("A@+0.6"); CHECK(read_state().buttons == DM_BTN_A, "按住時長測試先按下A");
    clock_ns += 60000000000ull; guest_frame += 15 - 1; server_tick += 15 - 1;   // +0.6 秒 = 0.6×25 = 15 拍 | +0.6 seconds = 0.6*25 = 15 ticks
    CHECK(read_state().buttons == DM_BTN_A, "牆鐘跳躍但客體幀未到點時A仍按住");
    guest_frame++; server_tick++; CHECK(neutral(read_state()), "服務端拍到點後A才鬆開");

    setup("LSL@+0.05,LSU@+0.05"); read_state(); release();
    CHECK(read_state().ly == 32767, "局內未出現Begin：鬆開即空閒，不等不存在的End");

    setup("A@+0.05,B@+0.05"); read_state(); ui("ControllerActionBegin"); ui("ControllerActionEnd"); ui("ControllerActionBegin"); release();
    CHECK(neutral(read_state()), "End後又有新Begin：重新忙，舊End不替新動作放行");
    ui("ControllerActionEnd"); CHECK(read_state().buttons == DM_BTN_B, "新動作自己的End後放行");

    setup("A@+0.05,B@+0.05"); read_state(); ui("ControllerActionEnd"); ui("ControllerActionEnd"); ui("ControllerActionBegin"); release();
    CHECK(neutral(read_state()), "先到的多餘End不能抵消後來Begin");
    ui("ControllerActionEnd"); CHECK(read_state().buttons == DM_BTN_B, "後來Begin結束後可繼續");

    setup("A@>OpenPanel:Done,B@+0.05"); read_state(); ui("ControllerActionBegin"); ui("ControllerActionBegin");
    ui_cb("PanelManager", "OpenPanel", "Done");
    CHECK(neutral(read_state()), "按住期間確認會鬆開，但動作未End仍等空閒");
    ui("ControllerActionEnd"); CHECK(read_state().buttons == DM_BTN_B, "確認後的重復Begin只需一End，再放B");

    setup("A@+0.05,B@%0.2+0.05"); read_state(); ui("ControllerActionBegin"); ui("ControllerActionBegin"); release(); ui("ControllerActionEnd");
    CHECK(neutral(read_state()), "End不跳過腳本原定延時");
    frames(4); CHECK(neutral(read_state()), "服務端延時 4 拍未到 5 拍時不放下一鍵");
    frames(1); CHECK(read_state().buttons == DM_BTN_B, "服務端延時到 5 拍且動作結束才放下一鍵");
    release(); CHECK(neutral(read_state()), "末步鬆開後無額外按鍵");

    setup("A@>Never"); read_state(); release();
    frames(OK_WAIT_TICKS - ACTION_HOLD_TICKS - 1); CHECK(g_ktries == 1 && neutral(read_state()), "確認等待 150 服務端拍前不重按");
    frames(1); CHECK(g_ktries == 2 && read_state().buttons == DM_BTN_A, "確認等待滿 150 服務端拍才重按");

    setup_pregame("A@%0.2"); read_state(); frames(11); CHECK(neutral(read_state()), "主菜單出圖幀延時 11/12 幀不提前按鍵");
    frames(1); CHECK(read_state().buttons == DM_BTN_A, "主菜單出圖幀延時滿 12 幀才按鍵");

    pid_t short_hold = fork();
    if (!short_hold) { setup("A@+0.01"); _exit(read_state().buttons == 0 ? 0 : 99); }
    int short_status; waitpid(short_hold, &short_status, 0);
    CHECK(WIFEXITED(short_status) && WEXITSTATUS(short_status) == 0, "反例：按住不足 1 服務端拍，真實讀數越線為未採到");

    pid_t child = fork();
    if (!child) { setup("A@>Never"); read_state(); release(); clock_ns += 60000000000ull; read_state(); _exit(99); }
    int status; waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "客體幀不動但牆鐘 60 秒只走兜底報紅");

    setup("A@Start>OpenPanel:Done,B@>OpenPanel:Next");
    ui_cb("PanelManager", "OpenPanel", "Done"); ui_cb("PanelManager", "OpenPanel", "Start");
    CHECK(read_state().buttons == DM_BTN_A && g_kcur == 0, "按下前的舊確認不完成當前步");
    ui("ControllerActionBegin"); ui("ControllerActionBegin"); ui_cb("PanelManager", "OpenPanel", "Done");
    ui_cb("PanelManager", "OpenPanel", "Next");
    CHECK(neutral(read_state()), "下一步尚未按下時的舊確認不能跳步，仍等前一動作End");
    ui("ControllerActionEnd");
    CHECK(read_state().buttons == DM_BTN_B && g_kcur == 1, "前一End後下一步真按B，舊Next確認不算完成");
    ui_cb("PanelManager", "OpenPanel", "Next");
    CHECK(neutral(read_state()) && g_kcur == 2, "下一步按下後的新鮮確認才完成並鬆手");
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r"); if (!f) { perror("trace"); return 2; }
        setup("LSL@+8,LSU@+8"); read_state(); char line[2048]; int entered = 0, nb = 0, ne = 0;
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "hid 腳本步 3 按下")) entered = 1;
            if (!entered) continue;
            double t;
            if (sscanf(line, "[dm] UIMSG [%lfs]", &t) != 1) continue;
            int beg = strstr(line, "InputMessage ControllerActionBegin") != NULL;
            int end = strstr(line, "InputMessage ControllerActionEnd") != NULL;
            if (!beg && !end) continue;
            clock_ns = 1000000000ull + (uint64_t)((t - 16.13) * 1e9); read_state();
            ui(beg ? "ControllerActionBegin" : "ControllerActionEnd"); nb += beg; ne += end;
            if (end) break;
        }
        fclose(f); s = read_state();
        CHECK(nb == 73 && ne == 1 && s.ly == 32767, "iOS原始步3軌跡73 Begin/1 End回放後進入LSU（假時鐘按消息時間推進）");
    }
    // 凍結拍（DM_TICK_HOLD）期間拍號不前進：前置在局內拍時鐘下見到，隨後宿主報「無拍」，%1 須改按出圖幀 60 幀走完（host_savecmp 死等 451 s）
    // During a tick freeze (DM_TICK_HOLD) the tick number does not advance: the precondition was seen under the in-game tick clock, then the host reports "no tick", and %1 must be finished by presented frames instead, 60 frames (host_savecmp waited 451 s dead)
    setup("A@OpenPanel:PauseLayout%1>PausePanelMessage:ExitGame"); server_tick = 401; guest_frame = 1000;
    ui_cb("PanelManager", "OpenPanel", "PauseLayout"); CHECK(neutral(read_state()), "凍結前見到前置，延時未到不按");
    server_available = 0; read_state();
    guest_frame += 59; CHECK(neutral(read_state()), "凍結後按幀計：59 幀未到 1 秒不按");
    guest_frame += 1; CHECK(read_state().buttons == DM_BTN_A, "凍結後按幀計：滿 60 幀（1 秒）按下A，不因拍號停住死等");
    // 讀取畫面還開著時到點的局內按鍵：不按、請求凍結拍；撤下後才按（畫面閘門 NPC 頁：拍 84 的 A 被讀取畫面吃掉）
    // An in-game key press that comes due while the loading screen is still up: do not press, request a tick freeze; press only after it is down (screenshot gate NPC page: the A at tick 84 was swallowed by the loading screen)
    setup("A@DM:Tick:84#>OpenPanel:NpcDialogPanel"); server_tick = 84; test_loadscreen_up = 1; hold_requests = 0;
    ui_cb("DM", "Tick", "84#"); CHECK(neutral(read_state()) && hold_requests == 1, "讀取畫面開著：前置到了也不按 A，並請求凍結拍");
    frames(30); CHECK(neutral(read_state()) && hold_requests >= 2, "讀取畫面仍開著：繼續不按");
    test_loadscreen_up = 0; hold_requests = 0; CHECK(read_state().buttons == DM_BTN_A && hold_requests == 0, "讀取畫面撤下：按下 A，不再請求凍結");
    setup("A@DM:Tick:84#>OpenPanel:NpcDialogPanel"); server_tick = 84; test_loadscreen_up = 0; hold_requests = 0;
    ui_cb("DM", "Tick", "84#"); CHECK(read_state().buttons == DM_BTN_A && hold_requests == 0, "讀取畫面本來就撤了：到點即按，不請求凍結（時序不變）");
    printf("== HID腳本 %d 項，失敗 %d ==\n", checks, fails); return fails != 0;
}
