// 我們自己加的界面文字取串（英文版：語言跟原版設置走，簡中以外一律英文；物品注釋 / 舊譯名英文下不顯示）。
// 唯一來源 dm/i18n/ui_text.tsv（鍵 / 中文 / English / 用在哪），dm/ios/tools/dm_i18n.py 用它做兩件事：
//   · 追加進原版字符串表 ui.json（RomFS 覆蓋層）：zhCN 列 = 中文、其餘 12 列 = 英文。佈局寫「@鍵」、宿主 dm_str 走原版取串 main+0x7f5630，
//     切語言時原版自己清空 + 重裝字符串表（SetLanguage 0x317298，r34 起不剪），不用我們跟。
//   · 生成 dm_i18n_tab.h（本文件編進去）：遊戲畫不到的地方（UIKit 彈框、資源下載頁、分享文件名、底欄字形）按 dm_lang_zh() 取。
// 語言：遊戲設過（SetLanguage 觀測門）就以它為準；之前（資源下載頁、進前端前）讀存檔 Settings/Settings.json 的 "Language"；
//   沒有設置文件 = 首次安裝，按 iOS 系統語言（dm_sys_lang）。我們的字：簡中、繁體用中文，其餘英文。
// String lookup for the UI text we added ourselves (English version: the language follows the original game's setting, and everything other than
// Simplified Chinese is English; item notes / legacy translated names are not shown in English).
// Single source: dm/i18n/ui_text.tsv (key / Chinese / English / where used); dm/ios/tools/dm_i18n.py does two things with it:
//   - Appends it to the original string table ui.json (RomFS overlay): zhCN column = Chinese, the other 12 columns = English. Layouts write "@key", and
//   the host's dm_str goes through the original string lookup at main+0x7f5630;
//     on a language switch the original game clears and reloads the string tables itself (SetLanguage 0x317298, no longer patched out as of r34), so we
//     do not have to follow it.
//   - Generates dm_i18n_tab.h (compiled into this file): places the game cannot draw (UIKit alerts, the asset download page, shared file names,
//   bottom-bar glyphs) pick by dm_lang_zh().
// Language: once the game has set it (SetLanguage observation gate), that value wins; before that (asset download page, before entering the front end)
// read "Language" from Settings/Settings.json in the save data;
//   no settings file = first install, use the iOS system language (dm_sys_lang). Our text: Chinese for Simplified and Traditional Chinese, English for
//   everything else.
#include "dm_i18n.h"
#include "dm_i18n_tab.h"
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Atomic int g_dm_lang = -1;

#if TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#endif
// 語言標記（BCP 47，如 en-US、zh-Hans-CN、zh-Hant-TW、yue-Hant-HK）→ 原版語言號。只落兩種：語言碼是中文（zh）或漢語方言
// （yue 粵、wuu 吳、hak 客、nan 閩南、cmn 官話、gan 贛、hsn 湘、lzh 文言）→ 8 簡中，不看字形和地區——我們沒有繁體的自有文字，
// 原版繁中 + 我們的簡體字一屏兩種字；其餘一律 0 英文——我們自有的字只有中英兩套，德 / 日等原版語言配英文自有字沒人驗過。空標記 = 簡中
// Language tag (BCP 47, e.g. en-US, zh-Hans-CN, zh-Hant-TW, yue-Hant-HK) → the original game's language index. Only two outcomes: a language code that
// is Chinese (zh) or a Chinese variety
// (yue Cantonese, wuu Wu, hak Hakka, nan Min Nan, cmn Mandarin, gan Gan, hsn Xiang, lzh Literary Chinese) → 8 Simplified Chinese, regardless of script
// and region, because we have no Traditional Chinese text of our own
// and the original game's Traditional Chinese plus our Simplified text would put two scripts on one screen; everything else → 0 English, because our own
// text exists only in Chinese and English, and nobody has verified the original game's other languages such as German / Japanese paired with our English
// text. Empty tag = Simplified Chinese
int dm_lang_from_tag(const char *t) {
    if (!t || !*t) return 8;
    char l[4] = { 0 }; int i = 0; while (i < 3 && t[i] && t[i] != '-' && t[i] != '_') { l[i] = (char)(t[i] | 0x20); i++; }
    static const char *const zh[] = { "zh", "yue", "wuu", "hak", "nan", "cmn", "gan", "hsn", "lzh" };
    for (unsigned k = 0; k < sizeof zh / sizeof *zh; k++) if (!strcmp(l, zh[k])) return 8;
    return 0;
}
// 首次安裝（還沒有設置文件）時的語言：按 iOS 首選語言第一項（CFLocaleCopyPreferredLanguages，過 dm_lang_from_tag：中文及漢語方言 → 簡中，其餘 → 英文）。
// 曾一律簡中（那時觸屏版沒有能改語言的入口，系統是英文的中文玩家新裝進了英文版改不回來）；自定義頁標題行有語言開關（dm_custom.c lang_en），放開。
// DM_LANG（宿主 / 測試模擬，同 hle_oe.c）優先；非 iOS 的宿主沒有系統語言可問，照舊簡中
// Language on first install (no settings file yet): by the first entry of the iOS preferred languages (CFLocaleCopyPreferredLanguages, through dm_lang_from_tag: Chinese and Sinitic varieties → Simplified Chinese, everything else → English).
// It used to be always Simplified Chinese (back then the touch version had no way to change the language, and a Chinese player whose system was in English got the English version on a fresh install and could not change it back); the Custom page title row now has a language switch (dm_custom.c lang_en), so this is opened up.
// DM_LANG (host / test simulation, same as hle_oe.c) takes priority; a non-iOS host has no system language to ask and stays Simplified Chinese as before
int dm_sys_lang(void) {
    const char *e = getenv("DM_LANG"); if (e && *e) return dm_lang_from_tag(e);
#if TARGET_OS_IPHONE
    int r = 8; CFArrayRef a = CFLocaleCopyPreferredLanguages();
    if (a) { if (CFArrayGetCount(a) > 0) { char t[64]; if (CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(a, 0), t, sizeof t, kCFStringEncodingUTF8)) r = dm_lang_from_tag(t); } CFRelease(a); }
    return r;
#else
    return 8;
#endif
}

static int settings_lang(void) {   // 設置文件裡的 "Language"；文件不在 = 首次安裝，按系統語言（hle_fs.c 首次建設置文件時也寫這個值）；文件在但沒有這個鍵 = -1 | "Language" in the settings file; file missing = first install, use the system language (hle_fs.c writes the same value when it first creates the settings file); file present but key missing = -1
    const char *sv = getenv("DM_SAVE"); if (!sv) return dm_sys_lang();
    char path[1200]; snprintf(path, sizeof path, "%s/Settings/Settings.json", sv);
    FILE *f = fopen(path, "rb"); if (!f) return dm_sys_lang();
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    const char *k = strstr(buf, "\"Language\""); if (!k) return -1;
    k = strchr(k + 10, ':'); return k ? atoi(k + 1) : -1;
}

int dm_lang_index(void) {
    int l = atomic_load(&g_dm_lang); if (l >= 0) return l;
    static _Atomic int file = -2; int f = atomic_load(&file); if (f == -2) { f = settings_lang(); atomic_store(&file, f); }
    return f < 0 ? 8 : f;   // 有設置文件但沒寫語言 = 遊戲缺省（宿主報 zh-Hans） | settings file exists but has no language entry = the game default (the host reports zh-Hans)
}
int dm_lang_zh(void) { int l = dm_lang_index(); return l == 8 || l == 9; }   // 繁體（zhTW）我們的字也用簡中（繁體玩家讀簡體比讀英文順） | for Traditional Chinese (zhTW) our text also uses Simplified Chinese (Traditional Chinese players read Simplified more easily than English)

const char *dm_ui(const char *key) {
    for (unsigned i = 0; i < sizeof k_dm_i18n / sizeof *k_dm_i18n; i++) if (!strcmp(k_dm_i18n[i].k, key)) return dm_lang_zh() ? k_dm_i18n[i].zh : k_dm_i18n[i].en;
    return key;
}

const char *dm_str(uint8_t *b, const char *key) {
    if (!b) return dm_ui(key);
    struct { const char *k; uint64_t n; } s = { key, strlen(key) };
    const char *r = ((const char *(*)(void *))(b + 0x7f5630))(&s);
    return r && *r && strcmp(r, key) ? r : dm_ui(key);   // 覆蓋層裡沒有這個鍵（不該發生）就退回編譯期表 | if the overlay lacks this key (should not happen), fall back to the compile-time table
}
