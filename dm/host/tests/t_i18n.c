// dm_i18n.c 的語言來源（UIKit 文字用）：遊戲設過語言（SetLanguage 觀測門寫 g_dm_lang）以它為準；沒設過讀存檔 Settings/Settings.json 的 "Language"；
// 沒有設置文件 = 首次安裝，按系統語言（測試用 DM_LANG 模擬）。我們的字：zhCN、zhTW 用中文，其餘英文。
// 期望值來自規則本身（8 zhCN / 9 zhTW 中文，其餘英文；語言號表見 diag.c 名表 main+0x20ae478），不跑遊戲。每個情形單獨起進程（文件讀一次就緩存），所以本程序按 argv 跑一個情形，Makefile 逐個調。
// Language source of dm_i18n.c (for UIKit text): once the game has set the language (the SetLanguage observation gate writes g_dm_lang), that value
// wins; if not, read "Language" from Settings/Settings.json in the save data;
// no settings file = first install, use the system language (simulated in tests with DM_LANG). Our text: Chinese for zhCN and zhTW, English for
// everything else.
// Expected values come from the rule itself (8 zhCN / 9 zhTW are Chinese, everything else is English; for the language index table see the name table at
// main+0x20ae478 in diag.c), without running the game. Each case needs its own process (the file is read once and then cached), so this program runs one
// case per argv and the Makefile invokes them one by one.
#include "../dm_i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static int fails;
static void expect(const char *what, const char *got, const char *want) { int ok = !strcmp(got, want); if (!ok) fails++; printf("%s %s：期望「%s」→ 真結果「%s」\n", ok ? "✓" : "✗", what, want, got); }
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    char dir[512]; snprintf(dir, sizeof dir, "%s/i18n_%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", mode);
    char sd[600], f[700]; snprintf(sd, sizeof sd, "%s/Settings", dir); snprintf(f, sizeof f, "%s/Settings.json", sd);
    mkdir(dir, 0755); mkdir(sd, 0755); remove(f); setenv("DM_SAVE", dir, 1);
    if (!strcmp(mode, "en") || !strcmp(mode, "game")) { FILE *o = fopen(f, "w"); fputs("{\n    \"Item Name Display\": 1,\n    \"Language\": 0,\n    \"AutoMapMode\": 2\n}\n", o); fclose(o); }
    if (!strcmp(mode, "zh")) { FILE *o = fopen(f, "w"); fputs("{ \"Language\": 8 }", o); fclose(o); }
    if (!strcmp(mode, "en")) expect("設置文件 Language 0、遊戲還沒設", dm_ui("dmConfirm"), "OK");
    else if (!strcmp(mode, "zh")) expect("設置文件 Language 8", dm_ui("dmConfirm"), "確定");
    else if (!strcmp(mode, "none")) { unsetenv("DM_LANG"); expect("沒有設置文件、沒給系統語言（Mac 缺省 zh-Hans）", dm_ui("dmConfirm"), "確定"); }
    else if (!strcmp(mode, "sysen")) { setenv("DM_LANG", "en-US", 1); expect("沒有設置文件（首次安裝）、系統語言 en-US", dm_ui("dmConfirm"), "OK"); }
    else if (!strcmp(mode, "systw")) { setenv("DM_LANG", "zh-Hant-TW", 1); expect("沒有設置文件、系統語言 zh-Hant-TW（繁體用中文）", dm_ui("dmConfirm"), "確定"); }
    else if (!strcmp(mode, "tags")) {   // 語言標記 → 原版語言號 | language tag → the original game's language index
        static const struct { const char *t; int want; } c[] = { { "en-US", 0 }, { "en-GB", 0 }, { "zh-Hans-CN", 8 }, { "zh-Hans", 8 }, { "zh", 8 }, { "zh-Hans-HK", 8 }, { "zh-Hans-TW", 8 }, { "zh-Hans-MO", 8 },
            { "zh-Hant-TW", 8 }, { "zh-Hant-HK", 8 }, { "zh-Hant", 8 }, { "zh-HK", 8 }, { "zh_TW", 8 }, { "yue-Hant-HK", 8 }, { "yue-Hans-CN", 8 }, { "yue", 8 }, { "wuu-Hans-CN", 8 },
            { "de-DE", 0 }, { "es-ES", 0 }, { "es-419", 0 }, { "fr-CA", 0 }, { "it-IT", 0 }, { "ko-KR", 0 }, { "pl-PL", 0 }, { "ru-RU", 0 }, { "ja-JP", 0 }, { "pt-BR", 0 }, { "nl-NL", 0 }, { "ar", 0 }, { "zu-ZA", 0 } };
        for (unsigned i = 0; i < sizeof c / sizeof *c; i++) { char w[8], g[8]; snprintf(w, sizeof w, "%d", c[i].want); snprintf(g, sizeof g, "%d", dm_lang_from_tag(c[i].t)); expect(c[i].t, g, w); } }
    else if (!strcmp(mode, "game")) {   // 設置文件寫的英文，遊戲隨後切回簡中：以遊戲為準；再切到 zhTW（9）：簡中以外一律英文 | the settings file says English, then the game switches back to Simplified Chinese: the game wins; then switch to zhTW (9): everything other than Simplified Chinese is English
        expect("遊戲沒設前按設置文件", dm_ui("dmConfirm"), "OK");
        atomic_store(&g_dm_lang, 8); expect("遊戲切到 zhCN", dm_ui("dmConfirm"), "確定");
        atomic_store(&g_dm_lang, 9); expect("遊戲切到 zhTW（繁體用中文）", dm_ui("dmConfirm"), "確定");
        atomic_store(&g_dm_lang, 1); expect("遊戲切到 deDE（按英文）", dm_ui("dmConfirm"), "OK");
        expect("表裡沒有的鍵原樣返回", dm_ui("dmNoSuchKey"), "dmNoSuchKey"); }
    else { fprintf(stderr, "用法：t_i18n en|zh|none|sysen|systw|tags|game\n"); return 2; }
    return fails ? 1 : 0;
}
