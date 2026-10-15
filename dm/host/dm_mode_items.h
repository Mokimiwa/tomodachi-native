// 觸屏專屬 / 手柄專屬行為的登記表：模式切換事務（dm_padmode.c）在改佈局之前調舊一側各項的「出」，重建完成之後調新一側各項的「進」。
// 出 = 這一項自己收手：作廢在途的替按鍵 / 替發消息請求、把挪過的原版控件寫回、清掉緩存的控件地址（重建後舊地址全是懸空的）。
// 側：0 = 只在觸屏模式做事，1 = 只在手柄模式做事，2 = 兩邊都在、但握著會隨重建失效的東西（每次切換都出一次、進一次）。
// 新加按模式分的行為在這裡登記一行，不要在別處各自判模式、也不要讓切換代碼點名調某個模塊。
//   X(名字, 側, 進, 出)   進 / 出：void (*)(void)，沒有寫 dm_mode_none（空函數）
// Registry of touch-only / controller-only behaviors: the mode switch transaction (dm_padmode.c) calls the "leave" of each item on the old side before changing the layout, and the "enter" of each item on the new side after the rebuild completes.
// Leave = the item stands down by itself: void in-flight key-press / message requests made on the player's behalf, write moved original widgets back, clear cached widget addresses (after a rebuild the old addresses are all dangling).
// Side: 0 = acts only in touch mode, 1 = acts only in controller mode, 2 = present on both sides but holding things that become invalid with a rebuild (leaves once and enters once on every switch).
// Register a new mode-specific behavior with one row here; do not check the mode separately elsewhere, and do not have the switch code call some module by name.
//   X(name, side, enter, leave)   enter / leave: void (*)(void); write dm_mode_none (an empty function) when there is none
#define DM_MODE_ITEMS(X) \
    X("符文之語清單", 2, dm_mode_none, dm_runeword_ui_rebuild) \
    X("前端路徑理順與替按鍵", 0, dm_mode_none, dm_ui_auto_mode_leave) \
    X("界面觸摸緩存與挪位表", 2, dm_mode_none, dm_ui_cache_mode_leave) \
    X("主菜單構建號與新版本提示", 2, dm_update_mode_enter, dm_mode_none) \
    X("死亡彈窗按當前模式重開", 2, dm_dead_modal_mode_enter, dm_mode_none) \
    X("魔盒並排", 0, dm_mode_none, dm_cube_side_mode_leave)
