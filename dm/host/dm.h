// Switch 原生宿主公共頭（多模塊）。
// Common header of the native Switch host (multi-module).
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // 當前遊戲檔案的常數（dm/games/<名>/game.tsv 生成，Makefile 的 GAME 選） | constants of the current game profile (generated from dm/games/<id>/game.tsv, chosen by the Makefile's GAME)

#define MAX_MOD 8
#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint8_t *d; size_t n; } dm_blob;   // 整份讀進內存的一個原版存檔文件（dm_pages.c dm_save_blobs） | One save file of the original game read whole into memory (dm_pages.c dm_save_blobs)
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
enum { DT_INIT = 12, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };
#define ELF64_ST_BIND(i) ((i) >> 4)            // 符號綁定：0 = STB_LOCAL | symbol binding: 0 = STB_LOCAL
#define ELF64_ST_VISIBILITY(o) ((o) & 3)       // 可見性：0 = STV_DEFAULT | visibility: 0 = STV_DEFAULT
enum { STB_LOCAL = 0, STV_DEFAULT = 0 };

// 一個 NSO 模塊的運行時狀態
// Run-time state of one NSO module
typedef struct {
    char name[16];
    uint8_t *base;             // 映像（含補丁區）在宿主地址空間的首址 | Start address of the image (including the patch area) in the host address space
    size_t size;
    uint64_t text_end, ro_off, data_off, key_page_off, key_page_size;   // 來自改寫器 fixups 頭 | From the rewriter fixups header
    int64_t patch_off;         // 補丁區相對首址；為負 = 補丁區在首址前（映像太大，見 patch_nso.py FAR），這時 size 不含它 | patch area relative to the base; negative = before the base (image too large, see patch_nso.py FAR), and size does not include it
    uint64_t image_size, file_image_len, patch_len;   // 僅補丁區在首址前時：整個映像（含 bss 與鍵頁）、文件裡映像部分的長度、補丁區長 | only with the patch area before the base: whole image (with bss and key page), length of the image part of the file, patch area length
    char build_id[33];         // NSO 構建號前 16 字節（十六進制）；舊產物沒有則為空 | First 16 bytes of the NSO build ID (hex); empty for older build outputs
    unsigned key_idx[8];
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh; unsigned symh_mask;   // 已定義符號的名字哈希表（開放尋址，存符號下標；同名只留下標最小者 = 線性掃描的首個命中） | Name hash table of defined symbols (open addressing, stores symbol indices; for a duplicated name only the smallest index is kept = the first hit of a linear scan)
    uint64_t init, init_array, init_arraysz;   // init = DT_INIT（模塊 _init：登記 DSO 並跑本模塊 init_array） | init = DT_INIT (the module's _init: registers the DSO and runs this module's init_array)
} module_t;

extern module_t g_mod[MAX_MOD];
extern int g_nmod;
// main 模塊的構建號（沒有則空串）。DM_D2R_BUILD = d2r-mobile 帶來的 D2R 構建：寫死 D2R 地址或改 D2R 存檔的代碼只在它上面開
// main's build ID (empty if none). DM_D2R_BUILD = the D2R build that the code inherited from d2r-mobile belongs to: code with fixed D2R addresses or that edits D2R saves runs only on it
#define DM_D2R_BUILD "db0507f36b84200ac05f75d76ed5026f"
static inline const char *dm_main_build(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return g_mod[i].build_id; return ""; }
// 四個線程本地槽：K18 = Switch 的 x18、KTLS = tpidr_el0、KTLR = tpidrro_el0（ThreadLocalRegion）、KGATE = 宿主門入口；實際鍵號由裝載器寫入鍵頁
// Four thread-local slots: K18 = the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (ThreadLocalRegion), KGATE = host gate entry; the actual key numbers are written into the key page by the loader
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
extern pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;

void load_modules(const char *dir, const char **names, int n);
int dm_option_known(const char *name);   // 這個遊戲的 options.tsv 裡有沒有這個可選剪口（只在 Mac 裝載路徑上記；iOS 恆 0） | whether this game's options.tsv lists the optional cut (recorded only on the Mac load path; always 0 on iOS)
void run_module_inits(void);
void *module_sym(module_t *m, const char *name);
void *guest_sym(const char *name);
module_t *module_of(const void *p);
// 門分派：改寫後的 svc / 計時器讀經公共入口進來，ret_addr = 該門塊返回點，regs 指向保存的 x0..x30
// Gate dispatch: rewritten svc / timer reads come in through the common entry, ret_addr = the return point of that gate block, regs points to the saved x0..x30
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
// 觀測門（改寫器 OBS 表；編號與 patch_nso.py 一致）：只讀回調，掛上即在門命中時調用
// Observation gates (rewriter OBS table; numbering matches patch_nso.py): read-only callbacks, called on a gate hit once attached
enum { OBS_UIMSG = 1, OBS_TICK = 2, OBS_C2S = 3, OBS_C2S_SYS = 4, OBS_SETLANG = 5, OBS_TARGET_RANK = 7, OBS_UI_PROJECT = 8, OBS_FREEZE_WORLD = 10, OBS_FREEZE_CLIENT = 11, OBS_FREEZE_ANIM = 12, OBS_FREEZE_RENDER = 13, OBS_TEXTBOX = 14, OBS_EVENT_MSG = 15, OBS_LEVEL_WARP = 16, OBS_QUEST_KILL = 17, OBS_NOTICE_INPUT = 18, OBS_MAX = 19 };   // 6 = iOS 綁定讀取（dm_touch_game_bridge.c） | 6 = iOS binding read (dm_touch_game_bridge.c)
void gate_obs_set(int id, void (*fn)(const uint64_t *regs));
int gate_obs_present(int id);
void dm_quiet_difficulty_msg(int on);   // 秘境局裡攔原版「遊戲難度設為 N」提示（觀測門 15，diag.c） | In a rift run, block the original game's 「遊戲難度設為 N」(Game difficulty set to N) message (observation gate 15, diag.c)
int gate_svc_present(int num);   // 產物裡有沒有這個 svc 號的門（剪口引入的宿主調用號用）   // 產物裡有沒有這個觀測門（gates.tsv 登記過） | Whether the build output has a gate for this svc number (for host call numbers introduced by cuts)   // whether the build output has this observation gate (registered in gates.tsv)
uint64_t now_ns(void);
uint64_t guest_ns(void);              // 客體時鐘（hle_os.c；DM_TICK_HOLD 凍結時不走） | Guest clock (hle_os.c; does not advance while DM_TICK_HOLD is frozen)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // 按原因凍結（1 後台，2 系統鍵盤）；見 hle_os.c | Freeze by reason (1 background, 2 system keyboard); see hle_os.c
void dm_guest_frame_tick(void);        // 每次出圖調（hle_nvn.c）：DM_DETCLOCK 開時把客體時鐘推進一個固定步長 | Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
int dm_nvn_ruler_truth(void);         // 視覺尺子真值階段（DM_NVN_RULER=1 的前兩張截圖）為 1 | 1 during the ground-truth phase of the visual ruler (the first two screenshots with DM_NVN_RULER=1)
uint64_t dm_nvn_frame_count(void);
uint64_t dm_nvn_sub_cpu_ns(void);     // 提交（不含出圖）累計 CPU 納秒 | Cumulative CPU nanoseconds of submit (excluding present)
int dm_ingame_now(void);              // 是否在局內（diag.c） | Whether we are in a run (diag.c)
void dm_3d_scale_feed(double iv_ms);   // 出圖線程每幀報實際上屏間隔（−1 丟棄 / 0 無讀數），驅動動態 3D 比例 | The present thread reports the actual present interval every frame (−1 discard / 0 no reading), driving the dynamic 3D scale
float dm_3d_scale_cur(void);          // 當前生效的 3D 渲染比例（hle_oe.c scale3d_tick）     // 當前累計出圖幀數（DM_TICKS 對齊觀測） | The 3D render scale currently in effect (hle_oe.c scale3d_tick)     // current cumulative count of presented frames (for DM_TICKS alignment observation)
void dm_backtrace(uint64_t fp);
void crash_watch_progress(uint32_t tick, const char *stage);
void crash_watch_frame(void);
void crash_watch_pregame_done(void);
uint64_t crash_watch_progress_count(void);
void crash_dump_now(const char *why);   // 按需導出現場（線程 + 圖形等待點），不退出 | Dump state on demand (threads + graphics wait points), without exiting
// 診斷開關：main 啓動時從環境變量讀一次（getenv 要拿 libc 全局環境鎖，進熱路徑會讓全部客體線程排隊）
// Diagnostic switches: read once from environment variables when main starts (getenv takes the libc global environment lock, and on a hot path it would make all guest threads queue up)
extern uint64_t g_t0_ns;   // 宿主啓動時刻（now_ns），日誌相對時間用 | Host start time (now_ns), used for relative times in the log
extern struct dm_flags { int thr, svc, fs, movie, dbg, imports, bootprof; } g_flag;
// 啓動時間線（DM_BOOTPROF=1，diag.c）：分段打點 + 計數器（納秒 / 字節，全線程累加）；可用 PROF_GFX_* 埋點
// Boot timeline (DM_BOOTPROF=1, diag.c): segment marks + counters (nanoseconds / bytes, accumulated over all threads); PROF_GFX_* can be used for instrumentation
enum { PROF_FS_READ, PROF_WAIT_LOCK, PROF_WAIT_CV, PROF_WAIT_ADDR, PROF_WAIT_SYNC, PROF_SLEEP, PROF_MAIN_WAIT,
       PROF_GFX_PRESENT, PROF_GFX_SHADER, PROF_TACT, PROF_N };   // PROF_TACT：TACT 塊 zlib 解碼（svc #0x80，宿主 libdeflate）耗時與輸出字節 | PROF_TACT: time and output bytes of TACT block zlib decode (svc #0x80, host libdeflate)
void dm_prof_add(int id, uint64_t ns, uint64_t bytes);
void dm_prof_mark(const char *seg);
uint64_t dm_prof_ribbon_ns(void);   // 第一次出現 OpenPanel MainMenuButtonRibbon 的 now_ns 時刻（0 = 尚未；需 DM_BOOTPROF=1） | The now_ns time at which OpenPanel MainMenuButtonRibbon first appeared (0 = not yet; needs DM_BOOTPROF=1)
void *objmap_get(const void *guest); void objmap_put(const void *guest, void *host); void *objmap_del(const void *guest);
void hle_signal_system_event(void *g);   // hle_thread.c：觸發服務層 SystemEvent（任何線程） | hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_bind_system_event(void *g, int autoclear);   // 客體線程上把客體 SystemEvent 綁到內核事件 | On a guest thread, bind a guest SystemEvent to a kernel event
// 缺頁鈎子 / 宿主寫客體內存前鈎子（crash.c；寫保護髒頁跟蹤用）
// Page fault hook / hook before the host writes guest memory (crash.c; used for write-protect dirty page tracking)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why));   // 資源計數回調（圖形層註冊）：每 250 拍、讀取畫面已撤、記滿退出時調 | Resource count callback (registered by the graphics layer): called every 250 ticks, when the loading screen has been dismissed, and on exit when the record is full
void (*dm_res_report_cb(void))(uint32_t tick, const char *why);   // 取已註冊的資源計數回調（未註冊為空） | Get the registered resource count callback (null if not registered)
void dm_guest_write_prepare(void *addr, size_t len);
// 圖形生命週期（hle_nvn.c）：on=1 掛起（之後提交 / 出圖阻塞，返回前 GPU 在途工作清空），on=0 恢復；iOS 前端在進出後台時調用
// Graphics lifecycle (hle_nvn.c): on=1 suspends (afterwards submit / present block, and in-flight GPU work is drained before returning), on=0 resumes; called by the iOS front end when entering and leaving the background
void dm_gfx_suspend(int on);
void dm_gfx_shot_at(long k, const char *path);
void dm_shot_written(const char *path, int ok);   // 圖形側截圖寫盤完成（diag.c：DM_TICK_HOLD=E:SHOT 據此解凍） | The graphics side finished writing a screenshot to disk (diag.c: DM_TICK_HOLD=E:SHOT unfreezes on this)
void dm_host_shutdown(const char *why);   // 優雅退出（crash.c）：停後台線程、音頻、圖形清空後 _exit(0)；SIGTERM 與 iOS App 終止回調用   // 圖形側：當前出圖幀之後第 k 幀截圖（nvn_mtl.m） | Graceful exit (crash.c): stop background threads and audio, drain graphics, then _exit(0); used by SIGTERM and the iOS App termination callback   // graphics side: screenshot of the k-th frame after the current presented frame (nvn_mtl.m)
