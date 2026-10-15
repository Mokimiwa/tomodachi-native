// 輸入映射層（dm_bind.h）：控制項串的解析、映射表的讀入與覆蓋、按下來源的合成。與平台無關，Mac 與 iOS 共用
// Input mapping layer (dm_bind.h): parsing control strings, loading and overriding the table, combining the held sources. Platform independent, shared by Mac and iOS
#include "dm.h"
#include "dm_bind.h"
#include "dm_input.h"
#include "front.h"
#include <ctype.h>
#include <pthread.h>
#include <strings.h>

static const char *const k_btn[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
// 搖桿方向按 NpadButton 16–23 的次序：左、上、右、下 | Stick directions in the order of NpadButton 16-23: left, up, right, down
static const char *const k_dir[4] = {"Left", "Up", "Right", "Down"};
static const float k_dx[4] = {-1, 0, 1, 0}, k_dy[4] = {0, 1, 0, -1};
const char dm_ctl_names[] = "A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down、StickL<Left|Up|Right|Down>[=0–1]、StickR<…>[=0–1]、StickL、StickR、Touch=x/y（1280x720）、Pointer";

static int parse_one(const char *w, dm_ctl *c) {
    char name[32]; const char *eq = strchr(w, '='); size_t n = eq ? (size_t)(eq - w) : strlen(w);
    if (!n || n >= sizeof name) return -1;
    memcpy(name, w, n); name[n] = 0;
    for (int b = 0; b < 16; b++) if (!strcasecmp(name, k_btn[b])) { if (eq) return -1; c->btn |= 1ull << b; return 0; }
    if (!strcasecmp(name, "Pointer")) { if (eq) return -1; c->touch = 2; return 0; }
    if (!strcasecmp(name, "Touch")) {
        float x, y; char tail;
        if (!eq || sscanf(eq + 1, "%f/%f%c", &x, &y, &tail) != 2 || x < 0 || y < 0 || x >= 1280 || y >= 720) return -1;
        c->touch = 1; c->tx = x / 1280.f; c->ty = y / 720.f; return 0;
    }
    if (strncasecmp(name, "Stick", 5) || (toupper((unsigned char)name[5]) != 'L' && toupper((unsigned char)name[5]) != 'R')) return -1;
    int s = toupper((unsigned char)name[5]) == 'R';
    if (!name[6]) { if (eq) return -1; c->analog |= 1u << s; return 0; }
    for (int d = 0; d < 4; d++) if (!strcasecmp(name + 6, k_dir[d])) {
        float a = 1; if (eq) { char *end; a = strtof(eq + 1, &end); if (end == eq + 1 || *end || a < 0 || a > 1) return -1; }
        c->ax[2 * s] += k_dx[d] * a; c->ax[2 * s + 1] += k_dy[d] * a; return 0;
    }
    return -1;
}
int dm_ctl_parse(const char *s, dm_ctl *c) {
    memset(c, 0, sizeof *c);
    char buf[256]; snprintf(buf, sizeof buf, "%s", s); int bad = 0;
    for (char *save, *w = strtok_r(buf, "+", &save); w; w = strtok_r(NULL, "+", &save))
        if (parse_one(w, c)) { LOG("【紅】不認識的手柄控制項「%s」（可用：%s）", w, dm_ctl_names); bad++; }
    return bad;
}

static float clamp1(float v) { return v < -1 ? -1 : v > 1 ? 1 : v; }
void dm_ctl_combine(const dm_ctl *const *c, const float (*analog)[2], int n, uint64_t *btn, int32_t ax[4]) {
    float a[4] = {0}; uint64_t b = 0;
    for (int i = 0; i < n; i++) {
        b |= c[i]->btn;
        for (int k = 0; k < 4; k++) a[k] += c[i]->ax[k];
        for (int s = 0; s < 2; s++) if (analog && (c[i]->analog >> s & 1)) { a[2 * s] += analog[i][0]; a[2 * s + 1] += analog[i][1]; }
    }
    *btn = b;
    for (int k = 0; k < 4; k++) ax[k] = (int32_t)(clamp1(a[k]) * DM_STICK_MAX);
}

// ---- 映射表 | The table ----
enum { BIND_MAX = 160, SRC_LEN = 40 };
typedef struct { char src[SRC_LEN]; dm_ctl ctl; } bind_row;
static bind_row g_row[BIND_MAX];
static uint8_t g_down[BIND_MAX];
static float g_val[BIND_MAX][2];
static int g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int find_locked(const char *src) { for (int i = 0; i < g_n; i++) if (!strcasecmp(g_row[i].src, src)) return i; return -1; }
static void drop_locked(int i) {
    g_n--; memmove(&g_row[i], &g_row[i + 1], (size_t)(g_n - i) * sizeof *g_row);
    memmove(&g_down[i], &g_down[i + 1], (size_t)(g_n - i)); memmove(&g_val[i], &g_val[i + 1], (size_t)(g_n - i) * sizeof *g_val);
}
// 讀一份表文本；同來源後來的覆蓋先來的，「-」刪掉。返回改動的條數 | Read one table text; a later line for the same source overrides an earlier one, "-" deletes it. Returns the number of entries changed
static int load_text(const char *text, const char *what) {
    int changed = 0, line = 0;
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p); line++;
        char buf[256]; if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, p, n); buf[n] = 0; p = e ? e + 1 : NULL;
        char *hash = strchr(buf, '#'); if (hash) *hash = 0;
        char *save, *src = strtok_r(buf, " \t\r", &save), *dst = src ? strtok_r(NULL, " \t\r", &save) : NULL;
        if (!src) continue;
        if (!dst || strtok_r(NULL, " \t\r", &save)) { LOG("【紅】鍵位表 %s 第 %d 行要「來源 目標」兩欄：%s", what, line, src); continue; }
        if (strlen(src) >= SRC_LEN) { LOG("【紅】鍵位表 %s 第 %d 行來源名太長：%s", what, line, src); continue; }
        int i = find_locked(src);
        if (!strcmp(dst, "-")) { if (i >= 0) { drop_locked(i); changed++; } continue; }
        dm_ctl c; if (dm_ctl_parse(dst, &c)) { LOG("【紅】鍵位表 %s 第 %d 行（%s）跳過", what, line, src); continue; }
        if (i < 0) { if (g_n == BIND_MAX) { LOG("【紅】鍵位表滿 %d 條，%s 第 %d 行起不收", BIND_MAX, what, line); break; } i = g_n++; snprintf(g_row[i].src, SRC_LEN, "%s", src); }
        g_row[i].ctl = c; g_down[i] = 0; g_val[i][0] = g_val[i][1] = 0; changed++;
    }
    return changed;
}
int dm_bind_load(const char *defaults, const char *user_path) {
    pthread_mutex_lock(&g_mu);
    g_n = 0; load_text(defaults, "默認");
    int user = -1;
    FILE *f = user_path ? fopen(user_path, "r") : NULL;
    if (f) {
        static char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); text[n] = 0; fclose(f);
        if (n == sizeof text - 1) LOG("【紅】鍵位表 %s 超過 %zu 字節，後面的不讀", user_path, sizeof text - 1);
        user = load_text(text, user_path);
    }
    int n = g_n;
    pthread_mutex_unlock(&g_mu);
    if (user >= 0) LOG("鍵位：%d 條（玩家的表 %s 改了 %d 條）", n, user_path, user);
    else LOG("鍵位：%d 條（默認；玩家的表可放 %s）", n, user_path ? user_path : "DM_KEYMAP");
    return n;
}
int dm_bind_find(const char *source) { pthread_mutex_lock(&g_mu); int i = find_locked(source); pthread_mutex_unlock(&g_mu); return i; }
void dm_bind_press(int i, int down) { if (i < 0) return; pthread_mutex_lock(&g_mu); if (i < g_n) g_down[i] = down != 0; pthread_mutex_unlock(&g_mu); }
void dm_bind_analog(int i, float x, float y) {
    if (i < 0) return;
    pthread_mutex_lock(&g_mu); if (i < g_n) { g_val[i][0] = clamp1(x); g_val[i][1] = clamp1(y); } pthread_mutex_unlock(&g_mu);
}
void dm_bind_reset(void) { pthread_mutex_lock(&g_mu); memset(g_down, 0, sizeof g_down); memset(g_val, 0, sizeof g_val); pthread_mutex_unlock(&g_mu); }
int dm_bind_is_pointer(int i) { pthread_mutex_lock(&g_mu); int r = i >= 0 && i < g_n && g_row[i].ctl.touch == 2; pthread_mutex_unlock(&g_mu); return r; }
void dm_bind_publish(void (*set)(uint64_t, int32_t, int32_t, int32_t, int32_t)) {
    static dm_ctl part[BIND_MAX]; const dm_ctl *held[BIND_MAX]; float val[BIND_MAX][2]; dm_touch t[DM_TOUCH_BIND_N]; int n = 0, nt = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; i++) {
        const dm_ctl *c = &g_row[i].ctl;
        if (g_down[i]) {
            held[n] = c;
            if (c->touch == 1 && nt < DM_TOUCH_BIND_N) { t[nt] = (dm_touch){DM_TOUCH_BIND + (uint32_t)nt, c->tx, c->ty}; nt++; }
        } else if (c->analog && (g_val[i][0] || g_val[i][1])) {   // 模擬量來源不必「按下」：只取它的搖桿部分 | an analog source need not be "down": only its stick part is taken
            part[n] = (dm_ctl){.analog = c->analog}; held[n] = &part[n];
        } else continue;
        val[n][0] = g_val[i][0]; val[n][1] = g_val[i][1]; n++;
    }
    uint64_t b; int32_t ax[4]; dm_ctl_combine(held, (const float (*)[2])val, n, &b, ax);
    pthread_mutex_unlock(&g_mu);
    set(b, ax[0], ax[1], ax[2], ax[3]);
    dm_touch_set_range(t, nt, DM_TOUCH_BIND, DM_TOUCH_BIND + DM_TOUCH_BIND_N);
}
