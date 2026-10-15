// 輸入映射層：前端的來源（鍵盤鍵、實體手柄鍵、虛擬鍵……）→ Switch 手柄的控制項（按鍵、搖桿方向、整支搖桿、觸屏點）。
// 三層：來源（各前端）→ 映射表（這裡）→ 虛擬 Switch 手柄狀態（dm_input.h / front.h 的 dm_touch）→ nn::hid（hle_hid.c / hle_touch.c）。
// 映射表是文本，一行一條「來源 目標[+目標…]」，空白分隔，# 起是注釋。來源名由前端定（Mac 鍵盤是 key:<鍵名>，front_mac.m）。
// 目標用 Switch 的名字（大小寫不論）：
//   按鍵 A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down；
//   搖桿方向 StickLLeft / StickLUp / StickLRight / StickLDown、StickRLeft / …（= NpadButton 16–23 的名字），可帶 =幅度（0–1，缺省推滿）；
//   整支搖桿 StickL / StickR：收前端報的模擬量（實體手柄的搖桿）；
//   觸屏點 Touch=x/y：按著時在遊戲面板坐標（1280x720）的 (x, y) 有一根手指；Pointer：在前端指針（鼠標）所在處觸屏，拖動時跟著走；
//   「-」= 這個來源不映射（玩家表用來解除默認鍵）。
// 前端給默認表，玩家的表按來源逐條覆蓋；前端只報來源的按下 / 抬起 / 模擬量，再 dm_bind_publish 把合成的手柄狀態交給宿主。
// DM_KEYS 腳本直接說 Switch 控制項（dm_ctl_parse），不經映射表：測試不隨玩家改鍵而變。
// Input mapping layer: front-end sources (keyboard keys, physical controller buttons, virtual keys...) → Switch controller controls (buttons, stick directions, whole sticks, touch points).
// Three layers: sources (each front end) → mapping table (here) → virtual Switch controller state (dm_input.h / dm_touch in front.h) → nn::hid (hle_hid.c / hle_touch.c).
// The table is text, one entry per line "source target[+target...]", whitespace separated, # starts a comment. Source names are defined by the front end (the Mac keyboard uses key:<key name>, front_mac.m).
// Targets use Switch names (case-insensitive):
//   buttons A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down;
//   stick directions StickLLeft / StickLUp / StickLRight / StickLDown, StickRLeft / ... (= the names of NpadButton 16-23), optionally with =amount (0-1, full tilt by default);
//   whole sticks StickL / StickR: take the analog values the front end reports (a physical controller's stick);
//   touch point Touch=x/y: while held, a finger at (x, y) in the game's panel coordinates (1280x720); Pointer: touch where the front end's pointer (mouse) is, following it while dragging;
//   "-" = this source is not mapped (the player's table uses it to unbind a default).
// The front end supplies a default table, and the player's table overrides it source by source; the front end only reports source down / up / analog values, then dm_bind_publish hands the combined controller state to the host.
// The DM_KEYS script speaks Switch controls directly (dm_ctl_parse), bypassing the table, so tests do not change when a player remaps keys.
#pragma once
#include <stdint.h>

// 一組 Switch 控制項（一個來源映射到的東西，或腳本一步要按的東西）
// A set of Switch controls (what one source maps to, or what one script step presses)
typedef struct {
    uint64_t btn;      // 按鍵位（dm_input.h 的 DM_BTN_*） | button bits (DM_BTN_* in dm_input.h)
    float ax[4];       // 搖桿方向的貢獻 lx ly rx ry，±1 = 推滿，y 向上為正 | stick direction contributions lx ly rx ry, ±1 = full tilt, y up is positive
    uint8_t analog;    // 收模擬量的搖桿：bit0 左、bit1 右 | sticks that take analog values: bit0 left, bit1 right
    uint8_t touch;     // 1 = 按著時有一根手指在 (tx, ty)；2 = Pointer：手指跟著前端的指針（鼠標）走，由前端推 | 1 = a finger at (tx, ty) while held; 2 = Pointer: the finger follows the front end's pointer (mouse), pushed by the front end
    float tx, ty;      // 觸屏點，歸一化 [0,1)（文本裡寫面板像素） | touch point, normalized [0,1) (written as panel pixels in the text)
} dm_ctl;

// 解析「A+StickLUp=0.5+Touch=640/360」這樣的控制項串。返回不認識的名字個數（0 = 全認得）；認識的部分照樣填進 c
// Parse a control string such as "A+StickLUp=0.5+Touch=640/360". Returns the number of unrecognized names (0 = all recognized); the recognized part is still filled into c
int dm_ctl_parse(const char *s, dm_ctl *c);
// 控制項串的說明（給紅字和文檔用） | Description of the control-string vocabulary (for red lines and docs)
extern const char dm_ctl_names[];

// 讀映射表：先 defaults（前端的默認文本），再 user_path（玩家的表，可為 NULL / 不存在）。返回生效的條數；壞行記紅字並跳過
// Load the table: defaults (the front end's default text) first, then user_path (the player's table, may be NULL / missing). Returns the number of active entries; bad lines are logged red and skipped
int dm_bind_load(const char *defaults, const char *user_path);
// 來源名 → 條目號（前端可緩存）；沒映射 = -1 | Source name → entry index (front ends may cache it); not mapped = -1
int dm_bind_find(const char *source);
// 來源按下 / 抬起；模擬量來源報 [-1,1]（y 向上為正）。i < 0 忽略 | Source down / up; analog sources report [-1,1] (y up is positive). i < 0 is ignored
void dm_bind_press(int i, int down);
void dm_bind_analog(int i, float x, float y);
// 這個來源映射到 Pointer（前端自己按指針位置推觸點）| whether this source maps to Pointer (the front end pushes the touch at the pointer itself)
int dm_bind_is_pointer(int i);
// 全部來源放開、模擬量歸零（窗口失焦時用，免得卡鍵） | release every source and zero analog values (used when the window loses focus, so no key sticks)
void dm_bind_reset(void);
// 合成當前狀態：按鍵與搖桿交給 set（前端選 dm_input_set 或 dm_pad_set），Touch 目標的手指推到映射層專用的觸屏槽
// Combine the current state: buttons and sticks go to set (the front end picks dm_input_set or dm_pad_set), fingers of Touch targets go to the touch slots reserved for the mapping layer
void dm_bind_publish(void (*set)(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry));
// 合成算法本身（純函數，dm_bind_publish 與 DM_KEYS 共用）：幾組控制項按位或、搖桿逐軸相加夾到 ±1 再乘 DM_STICK_MAX
// The combining itself (a pure function shared by dm_bind_publish and DM_KEYS): sets of controls are ORed, sticks are summed per axis, clamped to ±1, then scaled by DM_STICK_MAX
void dm_ctl_combine(const dm_ctl *const *c, const float (*analog)[2], int n, uint64_t *btn, int32_t ax[4]);
