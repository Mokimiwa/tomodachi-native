// 我們自己加的界面文字取串（英文版；表 = dm/i18n/ui_text.tsv，生成器 dm/ios/tools/dm_i18n.py）
// String lookup for the UI text we added ourselves (English version; table = dm/i18n/ui_text.tsv, generator dm/ios/tools/dm_i18n.py)
#pragma once
#include <stdatomic.h>
#include <stdint.h>
extern _Atomic int g_dm_lang;                         // 原版當前語言號（0 enUS … 8 zhCN），SetLanguage 觀測門寫（diag.c）；-1 = 遊戲還沒設過 | the original game's current language index (0 enUS ... 8 zhCN), written by the SetLanguage observation gate (diag.c); -1 = the game has not set it yet
int dm_lang_index(void);                              // 當前原版語言號（遊戲設過 > 設置文件 > 首次安裝按系統語言） | current language index of the original game (set by the game > settings file > system language on first install)
int dm_lang_zh(void);                                 // 1 = 我們的字用中文（zhCN、zhTW），其餘語言用英文 | 1 = our text uses Chinese (zhCN, zhTW); all other languages use English
int dm_lang_from_tag(const char *tag);                // 語言標記（en-US / zh-Hant-TW / es-419 …）→ 原版語言號：中文 / 漢語方言 = 8 zhCN，其餘 = 0 enUS | language tag (en-US / zh-Hant-TW / es-419 ...) → the original game's language index: Chinese / Chinese varieties = 8 zhCN, everything else = 0 enUS
int dm_sys_lang(void);                                // 系統語言 → 原版語言號（DM_LANG > iOS 首選語言 > zh-Hans） | system language → the original game's language index (DM_LANG > iOS preferred language > zh-Hans)
const char *dm_str(uint8_t *main_base, const char *key);   // 遊戲裡畫的字：原版取串（表已追加進原版 ui.json，跟原版一起換語言） | text drawn inside the game: the original string lookup (the table is appended to the original ui.json and switches language together with the original game)
const char *dm_ui(const char *key);                   // 遊戲外畫的字（UIKit / 資源下載頁 / 底欄字形）：編譯期表，按 dm_lang_zh() 取 | text drawn outside the game (UIKit / asset download page / bottom-bar glyphs): compile-time table, picked by dm_lang_zh()
