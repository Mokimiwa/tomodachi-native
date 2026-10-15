// 「自定義」設置：我們自己加的功能的啓停開關，統一在這裡讀寫。各模塊只調 dm_custom_get，不各自讀文件 / 環境變量當設置。
// 「自定義」(Custom) settings: on/off switches for the features we added ourselves, all read and written here. Each module only calls dm_custom_get and does not read files / environment variables as settings on its own.
#pragma once

// 已生效的值 0 / 1：只在玩家點選項頁「應用」後變，「放棄」不變，「默認」= 各鍵缺省（省電模式關，其餘開）。鍵不認識返回 1。
// 任意線程可調：第一次調用讀一次文件，之後只是原子量讀 + 幾項字符串比較，無鎖。iOS 上啓動期（存檔掛上之前）就能讀。
// 鍵：auto_mana 自動喝藥水（紅藍兩種，鍵名沿用）/ drop_light 高價值掉落光柱 / map_mark 小地圖標注出入口、小站與任務終點 / item_notes 物品注釋與舊譯名 / icloud iCloud 同步存檔（cloud_ios.m，啓動時讀）/ pot_pick 藥水自動拾取（dm_potpick.c）/ drop_haptic 高價值掉落震動（dm_drophaptic.c） / gem_pick 符文寶石自動拾取進高級倉庫（百寶囊）/ store_btn 背包「存放」按鈕（dm_pouch.c）/ eco_fps 省電模式 30 幀（nvn_mtl.m，缺省關）
// Effective value 0 / 1: changes only after the player taps 「應用」(Apply) on the options page, unchanged by 「放棄」(Discard), 「默認」(Default) = each key's default (power-saving mode off, the rest on). Returns 1 for an unrecognized key.
// Callable from any thread: the first call reads the file once, and after that it is just an atomic read + a few string comparisons, lock-free. On iOS it can be read already during startup (before the save is mounted).
// Keys: auto_mana auto-drink potions (both red and blue; the key name is kept) / drop_light light beam on high-value drops / map_mark minimap markers for entrances and exits, waypoints and quest goals / item_notes item notes and old translated names / icloud iCloud save sync (cloud_ios.m, read at startup) / pot_pick potion auto-pickup (dm_potpick.c) / drop_haptic haptics on high-value drops (dm_drophaptic.c) / gem_pick auto-pickup of runes and gems into the advanced stash (pouch) / store_btn the inventory 「存放」(Deposit) button (dm_pouch.c) / eco_fps power-saving mode 30 fps (nvn_mtl.m, off by default)
int dm_custom_get(const char *key);
int dm_custom_lang_explicit(void);   // 語言開關（鍵 lang_en）在設置文件裡寫著的值：-1 沒寫過（不動原版設置）/ 0 簡中 / 1 英文；hle_fs.c 掛存檔時讀 | Value of the language switch (key lang_en) as written in the settings file: -1 never written (original settings left alone) / 0 Simplified Chinese / 1 English; read by hle_fs.c at save mount
int dm_fps_setting(void);   // 省電模式（鍵 eco_fps，缺省關）：開 = 30 幀、關 = 60；nvn_mtl.m mtl_fps_target 讀 | Power-saving mode (key eco_fps, off by default): on = 30 fps, off = 60; read by mtl_fps_target in nvn_mtl.m

void dm_custom_init(const char *dir);   // 設置文件所在目錄（iOS 外殼啓動時給 Documents）；不調則用存檔提交目錄的上一級（Mac 宿主） | Directory holding the settings file (the iOS shell passes Documents at startup); if not called, the parent of the save commit directory is used (Mac host)

// 以下只給自定義頁的界面接線用
// The following are only for the UI wiring of the custom page
void dm_custom_on_uimsg(const char *cat, const char *act, const char *arg);   // 選項頁應用 / 默認 / 放棄（dm_cursor.c 的界面消息回調轉來） | Options page apply / default / discard (forwarded from the UI message callback in dm_cursor.c)
void dm_custom_ui_pump(unsigned long long root);                               // 遊戲主線程每幀：選項頁開著時讀各行勾選框（控件名 TouchCustom_<鍵>） | Game main thread, every frame: while the options page is open, read the checkbox of each row (widget name TouchCustom_<key>)
