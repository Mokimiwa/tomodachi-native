// HLE 公共：按 Switch 改編名登記宿主實現。
// 注意蘋果 AAPCS64 與 Switch 的差異：蘋果要求調用方把 <32 位參數（bool/char/short）擴展好，Switch 代碼不擴展 ⇒
// 這類參數一律聲明成 int/unsigned 再自己截斷。可變參數蘋果走棧、Switch 走寄存器 ⇒ 可變參數函數不能直接用 C 的 "..."。
// HLE common: register host implementations by Switch mangled name.
// Note the differences between Apple AAPCS64 and Switch: Apple requires the caller to extend <32-bit arguments (bool/char/short), Switch code does not extend ⇒
// such parameters are always declared as int/unsigned and truncated by ourselves. Variadic arguments go on the stack on Apple and in registers on Switch ⇒ variadic functions cannot use C's "..." directly.
#pragma once
#include "dm.h"
typedef struct { const char *name; void *fn; } hle_ent;
#define HLE(mangled, ret, cname, ...)                                                             \
    static ret cname(__VA_ARGS__);                                                                \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent hle_ent_##cname = {mangled, (void *)cname}; \
    static ret cname(__VA_ARGS__)

typedef uint32_t Result;   // nn::Result：w0，0 = 成功 | nn::Result: w0, 0 = success
#define RES_OK 0u
#define RES_FAIL 0x2A2u      // 通用失敗（聯網等被剪功能統一回這個） | Generic failure (cut features such as networking uniformly return this)

// 客體地址 → 宿主對象 旁表
// Guest address → host object side table
void *objmap_get(const void *guest);
void objmap_put(const void *guest, void *host);
void *objmap_del(const void *guest);
uint64_t now_ns(void);
// 本線程正在做宿主自己的重活（NVN 提交 / 出圖：編 Metal 命令；真機上這是 GPU 的事，不佔客體核）。同核搶佔的近似（hle_thread.c）不把這段當「在跑」
// This thread is doing the host's own heavy work (NVN submit / present: encoding Metal commands; on hardware that is the GPU's business and does not occupy a guest core). The same-core preemption approximation (hle_thread.c) does not count it as "running"
extern __thread _Atomic int dm_host_work;
uint64_t guest_ns(void);              // 客體時鐘（hle_os.c；DM_TICK_HOLD 凍結時不走） | Guest clock (hle_os.c; does not advance while DM_TICK_HOLD is frozen)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
uint64_t dm_guest_frame(void);        // 宿主每次出圖遞增；不受機器負載影響 | Incremented each time the host presents a frame; unaffected by machine load
uint64_t dm_server_tick(void);        // 局內服務端拍；不在局內或 DM_TICK_HOLD 凍結中返回 UINT64_MAX | In-run server tick; returns UINT64_MAX when not in a run or while DM_TICK_HOLD is frozen
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // 按原因凍結（1 後台，2 系統鍵盤）；見 hle_os.c | Freeze by reason (1 background, 2 system keyboard); see hle_os.c

// 強制接管：即使 SDK 模塊裡有定義，裝載時所有對該符號的引用（JUMP_SLOT/GLOB_DAT/ABS64）一律解析到宿主實現。
// 只用於服務層 nn::（fs/oe/aoc/pctl… 的服務客戶端）；SDK 的 libc++、nn::os 內部不進這張表。
// 每條必須寫接管理由；表裡有、但沒有任何模塊引用的條目，裝載時報紅中止（清單寫錯要響亮）。
// Forced takeover: even if the SDK module has a definition, all references to that symbol at load time (JUMP_SLOT/GLOB_DAT/ABS64) are resolved to the host implementation.
// Used only for the service-layer nn:: (service clients of fs/oe/aoc/pctl...); the SDK's libc++ and nn::os internals do not go into this table.
// Every entry must state the reason for the takeover; an entry in the table that no module references makes loading report a failure and abort (a wrong list must fail loudly).
typedef struct { const char *name; void *fn; const char *why; } hle_force_ent;
#define HLE_FORCE(mangled, why, ret, cname, ...)                                                   \
    static ret cname(__VA_ARGS__);                                                                \
    __attribute__((used, section("__DATA,swforce"))) static const hle_force_ent hle_force_##cname = {mangled, (void *)cname, why}; \
    static ret cname(__VA_ARGS__)

// 運行模式（0 掌機 / 1 主機）：hle_oe.c 持有，hid 的手柄樣式跟它走；前端可在運行中切換
// Operation mode (0 handheld / 1 docked): owned by hle_oe.c, and the hid controller style follows it; the front end can switch it at runtime
int dm_opmode(void);
void dm_3d_scale_hot(int hot);      // 過熱時把 3D 渲染比例降到 0.70（界面不動），0 = 恢復 | When overheating, lower the 3D render scale to 0.70 (UI untouched); 0 = restore
ssize_t dm_data_pread(int fd, void *buf, size_t n, off_t off);   // 資源檔案的讀：有「.holes」區段表的剔零檔案按表換算（hle_fs.c） | Resource archive reads: zero-stripped archives with a ".holes" extent table are translated through the table (hle_fs.c)
void dm_data_note_fd(int fd, const char *hostpath);
int dm_overlay_file(const char *rel, char *out, size_t n);   // 覆蓋層裡的文件：DM_ROMFS_OVERLAY 可以有多層（冒號分隔，前面的優先），逐層找 <層>/<rel>，找到寫進 out 返回 1（hle_fs.c） | File in the overlay: DM_ROMFS_OVERLAY may have several layers (colon-separated, earlier ones take priority); look for <layer>/<rel> layer by layer, and on a hit write it to out and return 1 (hle_fs.c)
void dm_set_opmode(int m);          // 變化時按遊戲打開的開關推送通知 30 / 31 | On change, push notification 30 / 31 according to the switches the game has turned on
// 焦點（1 前台 / 2 失焦 / 3 後台）：前端在生命週期變化時調，變化即推送通知 15
// Focus (1 foreground / 2 focus lost / 3 background): called by the front end on lifecycle changes; a change pushes notification 15
void dm_focus_set(int st);
// 音頻隨生命週期暫停 / 恢復（前端在切後台 / 回前台時調）
// Audio pause / resume following the lifecycle (called by the front end on going to background / returning to foreground)
void dm_audio_suspend(int on);
uint64_t dm_audio_render_calls(void);   // 聲卡回調累計次數（靜音接收端不計） | Cumulative sound card callback count (the silent sink is not counted)

// ---------- 宿主調用號登記表（剪口把遊戲內部函數入口換成 `svc #號; ret`，改寫器把它變成宿主門）----------
// Horizon 的 SVC 號只用到 0x7F，0x80 起留作宿主調用。新增必須在這裡登記，號碼不得重復；分派在 hle_svc.c → svc_host_call。
//   號     用途                                         所在文件        剪口（main 偏移）
//   0x80   TACT BLTE 塊 zlib 解碼 → libdeflate           hle_tact.c      0x1b0ee60（DecodeZlib 入口）
//   0x81   客戶端命令包投遞（收包泵 → 服務端收包）         diag.c          0x5d3a88（bl 0x632510 調用點；缺省原樣轉調，DM_C2S_REPLAY 時丟棄）
//   0x82   系統類客戶端消息投遞（收包泵 → 0x6310b0）       diag.c          0x5d39c8（bl 0x6310b0 調用點；缺省原樣轉調，DM_C2S_REPLAY 時文件首拍起丟棄）
//   0x83   Granny 段逐塊解碼（段讀取 0x11f0110）           hle_granny.c    0x11f02dc（bl 0x11f4af0 調用點；缺省按預解碼候選替換，核不上即原樣轉調）
//   0x84   Granny 修正表逐塊解碼（段讀取 0x11f07d0）       hle_granny.c    0x11f0914（bl 0x11f4af0 調用點；同上）
//   0x85   私有技能 HUD 自繪查詢（只讀，寫 x8）          hle_tact.c      0x3e443c（分支門：x8≠0 跳 0x3e4460，否則執行原指令回 0x3e4440；僅觸屏私有門）
//   0x86   私有技能地面方向（轉調原 0x2b4ef8 再按右搖桿改向） hle_private_aim.c 0x2faf04（bl 0x2b4ef8 調用點；僅觸屏私有門）
//   0x87   地上物品外觀路徑（轉調原 0xa14dc4 再按規則改指帶光變體） dm_dropfilter.c 0x8fa590（bl 0xa14dc4 調用點；只改模式 3 / 5 的物品，輸出在 x8 字符串）
//   0x88   物品落地掉落音（轉調原 0x470c20 再把一檔物品換成提示音） dm_dropfilter.c 0x468f2c（bl 0x470c20 調用點；寫 x0 = 聲音號）
//   0x89   秘境出口改道（秘境局裡把目的地換成下一張圖，寫 x2 / x3 / x8） dm_rift.c      0x7cc1e0（分支門：x8≠0 跳 0x634000，否則執行原指令回 0x7cc1e4）
//   0x8a   進局給玩家取出生點（轉調原 0x5596c0；進局在城鎮時換成小站旁）  dm_townwp.c     0x5d8040（bl 0x5596c0 調用點；玩家進局函數 0x5d7f70 裡）
//   0x8b   進局給屍體取落點（同上，與玩家取同一個點）                      dm_townwp.c     0x5d8400（bl 0x5596c0 調用點；同一函數裡）
//   0x8c   秘境裡存檔的當前幕（人在秘境圖裡把 w9 換成 4 = 第五幕，寫 x9；x8 恆 0） dm_rift.c      0x6fa9d0（分支門：x8≠0 跳 0x6fa9d4，否則執行原指令回 0x6fa9d4）
//   0x8d   傭兵加裝備位（傭兵 + 秘境 ≥10 層 + 腰帶 / 鞋 / 手套 / 戒指 / 項鍊時把 w0 換成 1，寫 x0；x8 恆 0） dm_rift.c      0x64a34c（分支門：x8≠0 跳 0x64a350，否則執行原指令回 0x64a350）
//   0x8e   傭兵加裝備位的客戶端校驗（秘境 ≥10 層 + 物品是腰帶 / 鞋 / 手套 / 戒指 / 項鍊時把 w1 換成物品自己的類型號，寫 x1；x8 恆 0） dm_rift.c      0x3beb58（分支門：x8≠0 跳 0x3beb5c，否則執行原指令回 0x3beb5c）
// ---------- Host call number registry (a cut replaces a game-internal function entry with `svc #number; ret`, and the rewriter turns it into a host gate) ----------
// Horizon's SVC numbers only go up to 0x7F; 0x80 onward is reserved for host calls. New ones must be registered here and numbers must not repeat; dispatch is in hle_svc.c → svc_host_call.
//   No.    Purpose                                      File            Cut (main offset)
//   0x80   TACT BLTE block zlib decode → libdeflate           hle_tact.c      0x1b0ee60 (DecodeZlib entry)
//   0x81   Client command packet delivery (receive pump → server receive)         diag.c          0x5d3a88 (bl 0x632510 call site; forwarded as is by default, dropped under DM_C2S_REPLAY)
//   0x82   System-class client message delivery (receive pump → 0x6310b0)       diag.c          0x5d39c8 (bl 0x6310b0 call site; forwarded as is by default, dropped from the file's first tick on under DM_C2S_REPLAY)
//   0x83   Granny section per-block decode (section read 0x11f0110)           hle_granny.c    0x11f02dc (bl 0x11f4af0 call site; by default replaced with the pre-decoded candidate, forwarded as is if it does not check out)
//   0x84   Granny fixup table per-block decode (section read 0x11f07d0)       hle_granny.c    0x11f0914 (bl 0x11f4af0 call site; same as above)
//   0x85   Private skill HUD self-draw query (read-only, writes x8)          hle_tact.c      0x3e443c (branch gate: x8≠0 jumps to 0x3e4460, otherwise executes the original instruction and returns to 0x3e4440; touch-only private gate)
//   0x86   Private skill ground direction (forwards to the original 0x2b4ef8, then redirects by the right stick) hle_private_aim.c 0x2faf04 (bl 0x2b4ef8 call site; touch-only private gate)
//   0x87   Ground item appearance path (forwards to the original 0xa14dc4, then repoints by rule to the lit variant) dm_dropfilter.c 0x8fa590 (bl 0xa14dc4 call site; only items in mode 3 / 5 are changed, output in the x8 string)
//   0x88   Item landing drop sound (forwards to the original 0x470c20, then swaps tier-one items to a notification sound) dm_dropfilter.c 0x468f2c (bl 0x470c20 call site; writes x0 = sound id)
//   0x89   Rift exit rerouting (in a rift run the destination is replaced with the next map, writes x2 / x3 / x8) dm_rift.c      0x7cc1e0 (branch gate: x8≠0 jumps to 0x634000, otherwise executes the original instruction and returns to 0x7cc1e4)
//   0x8a   Spawn point for the player on run entry (forwards to the original 0x5596c0; replaced with a point next to the waypoint when entering in town)  dm_townwp.c     0x5d8040 (bl 0x5596c0 call site; in the player join function 0x5d7f70)
//   0x8b   Drop point for the corpse on run entry (same as above, the same point as the player gets)                      dm_townwp.c     0x5d8400 (bl 0x5596c0 call site; in the same function)
//   0x8c   Current act saved while in a rift (when the character is in a rift map w9 is replaced with 4 = Act V, writes x9; x8 always 0) dm_rift.c      0x6fa9d0 (branch gate: x8≠0 jumps to 0x6fa9d4, otherwise executes the original instruction and returns to 0x6fa9d4)
//   0x8d   Extra mercenary equipment slots (mercenary + rift level ≥10 + belt / boots / gloves / ring / amulet: w0 is replaced with 1, writes x0; x8 always 0) dm_rift.c      0x64a34c (branch gate: x8≠0 jumps to 0x64a350, otherwise executes the original instruction and returns to 0x64a350)
//   0x8e   Client-side check for the extra mercenary equipment slots (rift level ≥10 + the item is a belt / boots / gloves / ring / amulet: w1 is replaced with the item's own type number, writes x1; x8 always 0) dm_rift.c      0x3beb58 (branch gate: x8≠0 jumps to 0x3beb5c, otherwise executes the original instruction and returns to 0x3beb5c)
enum { SVC_HOST_TACT_ZLIB = 0x80, SVC_HOST_C2S_DELIVER = 0x81, SVC_HOST_C2S_SYS = 0x82, SVC_HOST_GRANNY_A = 0x83, SVC_HOST_GRANNY_B = 0x84, SVC_HOST_PRIVATE_SKILL_RENDER = 0x85, SVC_HOST_PRIVATE_SKILL_AIM = 0x86, SVC_HOST_GROUND_ASSET = 0x87, SVC_HOST_DROP_SOUND = 0x88, SVC_HOST_RIFT_EXIT = 0x89, SVC_HOST_JOIN_SPAWN = 0x8a, SVC_HOST_JOIN_CORPSE = 0x8b, SVC_HOST_SAVE_ACT = 0x8c, SVC_HOST_MERC_SLOT = 0x8d, SVC_HOST_MERC_SLOT_CL = 0x8e };   // 0x87：地上物品取模型路徑（dm_dropfilter.c 掉落光）   // 0x83/0x84：Granny 逐塊解碼調用點（hle_granny.c） | 0x87: fetch the model path for ground items (dm_dropfilter.c drop light)   // 0x83/0x84: Granny per-block decode call sites (hle_granny.c)
