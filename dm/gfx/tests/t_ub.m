#include "../nvn/nvn_mtl.m"
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) { (void)p; (void)stage; memset(key, 0, 8); return 0; }   // hle_nvn.c 的著色器鍵：本測不鏈它，給零鍵（原先缺這個樁，鏈接失敗） | shader keys from hle_nvn.c: this test does not link it, so a zero key (the missing stub broke the link)
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { long long w=(want), g=(got); count++; printf("%s 期望 %lld → 真結果 %lld %s\n", name, w, g, w==g?"OK":"FAIL"); if(w!=g) fail++; } while(0)
int g_t_ub_var = 7, g_t_ub_var2 = -3;   // 被 @set 按符號名找到的全局變量 | global variables found by @set via symbol name
// 同進程交替測量（DM_NVN_UB）的相位邏輯單測：不起渲染，只按幀號推進。
// 配置：兩類，每相 60 幀。相序 = 基準, a, 基準, b, 基準, a, …；a = 設 g_t_ub_var=5 且丟鍵 abcd…；b = 設 g_t_ub_var2=9 且丟全部（*）。
// Unit tests for the phase logic of same-process alternating measurement (DM_NVN_UB): no rendering is started, it only advances by frame number.
// Configuration: two classes, 60 frames per phase. Phase order = baseline, a, baseline, b, baseline, a, ...; a = set g_t_ub_var=5 and drop keys abcd...; b = set g_t_ub_var2=9 and drop everything (*).
int main(void) {
    setenv("DM_NVN_UB", "a=@set:g_t_ub_var=5,abcd;b=@set:g_t_ub_var2=9,*;c=@set:g_no_such_var=1", 1); setenv("DM_NVN_UB_N", "60", 1);
    struct { int f, v1, v2, skip_abcd, skip_other; const char *what; } T[] = {
        {0, 7, -3, 0, 0, "相 0 基準"}, {59, 7, -3, 0, 0, "相 0 末幀"},
        {60, 5, -3, 1, 0, "相 1 = a：變量 1 被設、丟 abcd"}, {119, 5, -3, 1, 0, "相 1 末幀"},
        {120, 7, -3, 0, 0, "相 2 基準：變量 1 還原"},
        {180, 7, 9, 1, 1, "相 3 = b：變量 2 被設、全丟"},
        {240, 7, -3, 0, 0, "相 4 基準：變量 2 還原"},
        {300, 7, -3, 0, 0, "相 5 = c：變量不存在，什麼都不改"},
        {360, 7, -3, 0, 0, "相 6 基準"},
        {420, 5, -3, 1, 0, "相 7 = a（第二圈）"},
    };
    int ti = 0, n = (int)(sizeof T / sizeof T[0]);
    for (int f = 0; f <= 420 && ti < n; f++) { mtl_set_frame((uint64_t)f);
        if (f != T[ti].f) continue;
        char nm[96]; snprintf(nm, sizeof nm, "%s 變量1", T[ti].what); EXPECT(nm, T[ti].v1, g_t_ub_var);
        snprintf(nm, sizeof nm, "%s 變量2", T[ti].what); EXPECT(nm, T[ti].v2, g_t_ub_var2);
        snprintf(nm, sizeof nm, "%s 丟 abcd0123", T[ti].what); EXPECT(nm, T[ti].skip_abcd, ub_skip("abcd0123ffff0000"));
        snprintf(nm, sizeof nm, "%s 丟別的鍵", T[ti].what); EXPECT(nm, T[ti].skip_other, ub_skip("9999888877776666"));
        ti++; }
    EXPECT("走完的檢查點數", n, ti);
    // 上屏間隔按相統計：相內第 45 幀起才計；三檔界線 20.8 / 29.2 ms；−1 記丟棄
    // On-screen present intervals are tallied per phase: counted only from frame 45 of the phase on; boundaries of the three bands are 20.8 / 29.2 ms; −1 is recorded as discarded
    for (int f = 480; f < 480 + 45; f++) { mtl_set_frame((uint64_t)f); mtl_ub_present(16.7); }   // 過渡段：不計 | transition segment: not counted
    EXPECT("過渡段不計", 0, atomic_load(&g_ubp_c[0]));
    mtl_set_frame(480 + 45); mtl_ub_present(16.7); mtl_ub_present(25.0); mtl_ub_present(25.0); mtl_ub_present(33.3); mtl_ub_present(-1); mtl_ub_present(0);
    EXPECT("16.7 ms 檔", 1, atomic_load(&g_ubp_c[0])); EXPECT("25 ms 檔", 2, atomic_load(&g_ubp_c[1])); EXPECT("≥33 ms 檔", 1, atomic_load(&g_ubp_c[2])); EXPECT("丟棄", 1, atomic_load(&g_ubp_c[3]));
    EXPECT("間隔和（微秒）", 16700 + 25000 + 25000 + 33300, (long long)atomic_load(&g_ubp_sum_us));
    printf("t_ub：%d 項，失敗 %d\n", count, fail); return fail != 0;
}
