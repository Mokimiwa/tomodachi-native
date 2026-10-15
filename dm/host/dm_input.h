// 前端（Mac 鍵盤 / iOS 觸屏）→ nn::hid 的輸入狀態。按鈕位與 nn::hid::NpadButton 一致：
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15；搖桿 ±32767。
// Front end (Mac keyboard / iOS touchscreen) -> nn::hid input state. Button bits match nn::hid::NpadButton:
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15; sticks ±32767.
#pragma once
#include <stdint.h>
// 按鈕位（= nn::hid::NpadButton 位序；hle_hid.c 的 DM_KEYS 鍵名表由這裡生成）。前端一律用這些名字組 buttons。
// Button bits (= nn::hid::NpadButton bit order; the DM_KEYS key-name table in hle_hid.c is generated from here). Frontends always compose buttons from these names.
enum {
    DM_BTN_A = 1u << 0, DM_BTN_B = 1u << 1, DM_BTN_X = 1u << 2, DM_BTN_Y = 1u << 3,
    DM_BTN_LS = 1u << 4, DM_BTN_RS = 1u << 5, DM_BTN_L = 1u << 6, DM_BTN_R = 1u << 7,
    DM_BTN_ZL = 1u << 8, DM_BTN_ZR = 1u << 9, DM_BTN_PLUS = 1u << 10, DM_BTN_MINUS = 1u << 11,
    DM_BTN_LEFT = 1u << 12, DM_BTN_UP = 1u << 13, DM_BTN_RIGHT = 1u << 14, DM_BTN_DOWN = 1u << 15,
};
#define DM_STICK_MAX 32767
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
uint64_t dm_input_samples(void);   // 遊戲已取手柄的累計次數 | Cumulative number of times the game has sampled the controller
// 實體 / 虛擬手柄（iOS GameController，dm_gamepad.m）：與觸屏輸入分開存，遊戲取手柄時按鍵按位或、搖桿手柄非零就用手柄的。沒連手柄時全 0，取值與原來逐位相同
// Physical / virtual controller (iOS GameController, dm_gamepad.m): stored separately from touch input; when the game reads the controller, buttons are
// ORed bitwise and a stick uses the controller's value if it is non-zero. All 0 when no controller is connected, so values are bit-for-bit the same as before
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

// 可選診斷DM_HID_RECEIPT_LOG=<可寫JSONL路徑>：真實GetNpadState返回收樣。
// 字段read_start_ns/time_ns同宿主now_ns()；前端publish窗口必須同helper，讀取區間跨發佈不得強配。
// 狀態變化+最多每秒一次活動見證，1024條後limit_reached；未設環境變量不寫文件。
// 實際返回含id/style/sampling/buttons/四軸/attributes/script_mask，不把前端輸出當收到。
// 無新行不證明空白觸點讀到，須有效連接/採樣活動/原UI與命令包證據；日誌有IO開銷，不作計時。
// Optional diagnostic DM_HID_RECEIPT_LOG=<writable JSONL path>: received samples of real GetNpadState returns.
// Fields read_start_ns/time_ns use the same host now_ns(); the front-end publish window must use the same helper, and a read interval that spans a publish must not be force-matched.
// State changes + at most one activity witness per second, limit_reached after 1024 entries; no file is written when the environment variable is not set.
// The actual return includes id/style/sampling/buttons/four axes/attributes/script_mask; front-end output is not treated as received.
// No new line does not prove that a blank touch point was read; a valid connection / sampling activity / evidence from the original UI and command packets is required; the log has IO overhead and is not used for timing.

// 私有skill-aim-v1：只取已實際GetNpad返回的右軸成對收樣，非前端尚未消費值。
// Private skill-aim-v1: takes only paired received samples of the right axis that GetNpad has actually returned, not values the front end has not yet had consumed.
void dm_input_sampled_right(int32_t *rx,int32_t *ry);

// 手柄狀態注入點：GetNpadState 填完原狀態後、偽鍵覆蓋前調用。Mac 宿主觸點腳本（touch_mac.c，Mac 專用、按 _mac 約定不進 iOS 包）啓動時註冊；iOS 不帶該文件，保持空
// Controller state injection point: called after GetNpadState has filled in the original state and before the pseudo-key override. The Mac host touch point script (touch_mac.c, Mac only, kept out of the iOS app by the _mac convention) registers at startup; iOS does not carry that file and it stays empty
extern void (*dm_pad_hook)(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
