// 設定面板：按 HOME 打開，遊戲暫停；整個畫面變暗，正中一張黃卡片，鍵是遊戲那種白鍵（選中的是確認橘）。介面在 duo_shell.h。四頁：
//   語言：遊戲語言（跟隨這台裝置，或遊戲的 13 種之一），下次開啟遊戲時生效。
//   畫面：畫面形狀（填滿螢幕 / 16:9，下次開啟）、MetalFX 放大（即時）、幀率（30 / 60：這個遊戲有 fps60 剪口才出現，下次開啟）。
//   按鍵：螢幕上的按鍵顯示 / 隱藏（HOME 和截圖鍵一律留著）、疊在畫面上時的不透明度，即時。
//   存檔：立即備份、匯出到「檔案」、備份列表（還原）、從頭開始。還原和從頭開始要再按一次確認，下次開啟遊戲時才做，目前的存檔先整個搬進備份（DuoSave.m）。
//   開機才讀的設定改過、還沒套用時，每頁底部有「關閉遊戲」，也要再按一次確認，並說明上次存檔之後的進度會不見。只有淡入淡出，沒有呼吸 / 脈動。
// The settings panel: opened by HOME with the game paused; the whole screen dims and a yellow card sits in the middle, with keys in the game's white style (the selected one in the confirm orange). Interface in duo_shell.h. Four pages:
//   Language: the game language (same as this device, or one of the game's 13), effective the next time the game opens.
//   Display: the picture shape (fill the screen / 16:9, next opening), MetalFX upscaling (immediate), frame rate (30 / 60: shown only when this game has the fps60 cut, next opening).
//   Controls: on-screen buttons shown / hidden (HOME and capture always stay) and their opacity over the picture, immediate.
//   Save data: back up now, export to Files, the backup list (restore), start over. Restore and start over need a second press and run at the next opening, after the whole current save is moved into a backup (DuoSave.m).
//   While a boot-time setting has changed and not yet applied, every page ends with Close Game, which also needs a second press and says that progress since the last save will be lost. Fades only: no breathing or pulsing.
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <errno.h>
#include "duo_shell.h"

__attribute__((weak)) int duo_game_option_known(const char *name) { (void)name; return 0; }
__attribute__((weak)) void duo_game_set_upscale(int on) { (void)on; }
__attribute__((weak)) void duo_game_quit(void) { exit(0); }

static BOOL zh(void) { return [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"]; }
static NSString *L(NSString *z, NSString *e) { return zh() ? z : e; }
static NSUserDefaults *ud(void) { return NSUserDefaults.standardUserDefaults; }

// ---- 遊戲語言 | Game language ----
// 遊戲的 13 種（同 Mac 的 k_langs，名字用各自的語言寫） | the game's 13 (as the Mac's k_langs, each named in its own language)
static const struct { const char *code, *name; } k_langs[] = {
    {"en-US", "English (US)"}, {"en-GB", "English (UK)"}, {"fr", "Français"}, {"fr-CA", "Français (Canada)"}, {"es", "Español (España)"},
    {"es-419", "Español (Latinoamérica)"}, {"de", "Deutsch"}, {"it", "Italiano"}, {"nl", "Nederlands"}, {"ja", "日本語"}, {"ko", "한국어"},
    {"zh-Hans", "簡體中文"}, {"zh-Hant", "繁體中文"},
};
enum { N_LANGS = sizeof k_langs / sizeof k_langs[0] };
NSString *duo_lang_name(NSString *code) { for (int i = 0; i < N_LANGS; i++) if ([code isEqualToString:@(k_langs[i].code)]) return @(k_langs[i].name); return code; }
// 系統首選語言對到遊戲的語言，對法同 Mac 啓動器（scripts/build_mac_app.py）；iPadOS 的「App 語言」改的也是這個首選語言
// The system's preferred language mapped to a game language, as the Mac launcher does (scripts/build_mac_app.py); iPadOS's per-app language changes this same preference
static NSString *system_lang(void) {
    NSString *p = NSLocale.preferredLanguages.firstObject ?: @"en";
    for (NSString *h in @[@"zh-Hant", @"zh-TW", @"zh-HK", @"zh-MO", @"yue"]) if ([p hasPrefix:h]) return @"zh-Hant";
    if ([p hasPrefix:@"zh"]) return @"zh-Hans";
    if ([p hasPrefix:@"ja"]) return @"ja";
    if ([p hasPrefix:@"ko"]) return @"ko";
    for (NSString *h in @[@"en-GB", @"en-AU", @"en-NZ", @"en-IE", @"en-IN", @"en-ZA"]) if ([p hasPrefix:h]) return @"en-GB";
    if ([p hasPrefix:@"fr-CA"]) return @"fr-CA";
    if ([p hasPrefix:@"fr"]) return @"fr";
    if ([p isEqualToString:@"es"] || [p hasPrefix:@"es-ES"]) return @"es";
    if ([p hasPrefix:@"es"]) return @"es-419";
    for (NSString *h in @[@"de", @"it", @"nl"]) if ([p hasPrefix:h]) return h;
    return @"en-US";
}
NSString *duo_lang_code(void) { NSString *p = [ud() stringForKey:@"GameLanguage"]; return p.length ? p : system_lang(); }

// 這一局開機時的值：與目前的設定不同 = 下次開啟才生效（面板第一次建時記，那時還沒人改過） | this run's values at boot: differing from the current setting = takes effect next time (noted when the panel is first built, before anything was changed)
static NSDictionary *g_boot;
static NSString *aspect_pref(void) { NSString *a = [ud() stringForKey:@"GameAspect"]; return [a isEqualToString:@"screen"] ? @"" : a ?: @""; }
static int fps_pref(void) { return [ud() integerForKey:@"FrameRate"] == 60 ? 60 : 30; }
static double pad_opacity(void) { return [ud() objectForKey:@"PadOpacity"] ? [ud() doubleForKey:@"PadOpacity"] : 0.6; }
static void boot_note(void) {
    if (g_boot) return;
    const char *l = getenv("DM_LANG"), *o = getenv("DM_OPTIONS");
    g_boot = @{@"lang": l ? @(l) : duo_lang_code(), @"aspect": aspect_pref(), @"fps": @(o ? (strstr(o, "fps60") ? 60 : 30) : fps_pref())};
}
static BOOL boot_changed(void) {
    boot_note();
    return ![duo_lang_code() isEqualToString:g_boot[@"lang"]] || ![aspect_pref() isEqualToString:g_boot[@"aspect"]]
        || (duo_game_option_known("fps60") && fps_pref() != [g_boot[@"fps"] intValue]) || duo_save_pending();
}

// ---- 開機保護 | Safe boot ----
// 開機才讀的設定（語言、畫面比例、幀率）改壞了會讓遊戲一開就崩，而設定面板要遊戲開起來才進得去。所以：開機時記下「開機中」和這次用的設定；
// 遊戲畫出畫面（front_ios.m，開場畫面收起時）才算這組設定能開，記成「上次能開的」。下次開機若「開機中」還在、設定又和上次能開的不同，就改回去，
// 開場後打開設定面板說明改了哪些。設定沒變的崩潰不動設定（那不是設定的錯）
// Boot-time settings (language, aspect ratio, frame rate) can make the game crash at launch, and the settings panel is only reachable once the game is up. So: at boot, note "booting" and the settings this boot uses;
// only once the game draws (front_ios.m, when the opening screen clears) does this set count as working, saved as "last good". If the next boot finds "booting" still set and the settings differ from the last good ones, they go back,
// and after the opening the settings panel opens saying which changed. A crash with unchanged settings leaves them alone (the settings are not the cause)
static NSArray *guard_keys(void) { return @[@"GameLanguage", @"GameAspect", @"FrameRate"]; }
static NSDictionary *guard_now(void) { NSMutableDictionary *d = [NSMutableDictionary dictionary]; for (NSString *k in guard_keys()) { id v = [ud() objectForKey:k]; if (v) d[k] = v; } return d; }
static NSDictionary *g_guard_boot;   // 這次開機用的設定 | the settings this boot uses
void duo_boot_guard_begin(void) {
    NSDictionary *now = guard_now();
    if ([ud() boolForKey:@"BootStarted"]) {
        NSDictionary *good = [ud() dictionaryForKey:@"LastGoodSettings"] ?: @{};
        NSMutableArray *back = [NSMutableArray array];
        for (NSString *k in guard_keys()) {
            if (now[k] == good[k] || [now[k] isEqual:good[k]]) continue;
            [back addObject:k];
            if (good[k]) [ud() setObject:good[k] forKey:k]; else [ud() removeObjectForKey:k];
        }
        if (back.count) { [ud() setObject:back forKey:@"SafeBootReverted"]; now = guard_now(); fprintf(stderr, "【紅】boot: 上次開機沒畫出畫面，%s 改回上次能開的設定\n", [back componentsJoinedByString:@" "].UTF8String); }
        else fprintf(stderr, "【紅】boot: 上次開機沒畫出畫面（設定和上次能開的一樣，不改）\n");
    }
    g_guard_boot = now;
    [ud() setBool:YES forKey:@"BootStarted"]; [ud() synchronize];
}
void duo_boot_guard_ok(void) {
    if (!g_guard_boot || ![ud() boolForKey:@"BootStarted"]) return;
    [ud() setObject:g_guard_boot forKey:@"LastGoodSettings"]; [ud() removeObjectForKey:@"BootStarted"];
    fprintf(stderr, "boot: 遊戲畫出畫面，這組開機設定記為能開\n");
}
int duo_boot_guard_page(void) {   // 改回過設定時開場後要打開的頁；沒有 = -1 | the page to open after the opening when settings were put back; none = -1
    NSArray *r = [ud() arrayForKey:@"SafeBootReverted"];
    return !r.count ? -1 : [r containsObject:@"GameLanguage"] && r.count == 1 ? DUO_PAGE_LANGUAGE : DUO_PAGE_DISPLAY;
}
static NSString *guard_name(NSString *k) {
    return [k isEqualToString:@"GameLanguage"] ? L(@"遊戲語言", @"Game Language") : [k isEqualToString:@"GameAspect"] ? L(@"畫面比例", @"Aspect Ratio") : L(@"幀率", @"Frame Rate");
}

// ---- 面板 | The panel ----
@interface DuoPanel () <UIDocumentPickerDelegate>
@end
@implementation DuoPanel {
    UIView *_card, *_clip, *_body; CAShapeLayer *_shadow; UIScrollView *_scroll; UILabel *_title; DuoButton *_resumeKey; NSArray<DuoButton *> *_tabs;
    CGFloat _u, _y, _bodyW, _laidW;
    NSString *_confirm;     // 開著的確認：@"fresh"、@"quit"、@"restore:<名>"，沒有 = nil | the open confirmation: @"fresh", @"quit", @"restore:<name>", none = nil
    NSString *_flash;       // 存檔頁上一個動作的結果 | the result of the last action on the save page
    NSString *_failed;      // 上次開機的還原 / 從頭開始沒做成（duo_save_take_failure） | the last boot's restore / start over failed (duo_save_take_failure)
    NSArray *_reverted;     // 開機保護改回的設定（SafeBootReverted） | settings put back by the safe-boot guard (SafeBootReverted)
    NSURL *_export;
}
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        boot_note(); _failed = duo_save_take_failure();
        _reverted = [ud() arrayForKey:@"SafeBootReverted"]; [ud() removeObjectForKey:@"SafeBootReverted"];
        self.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
        _card = [UIView new]; [self addSubview:_card];
        _shadow = [CAShapeLayer layer]; _shadow.fillColor = duo_rgb(DUO_YELLOW).CGColor;
        _shadow.shadowColor = UIColor.blackColor.CGColor; _shadow.shadowOpacity = 0.22; _shadow.shadowRadius = 18; _shadow.shadowOffset = CGSizeMake(0, 6);
        [_card.layer addSublayer:_shadow];
        _clip = [UIView new]; _clip.backgroundColor = duo_rgb(DUO_YELLOW); _clip.clipsToBounds = YES; [_card addSubview:_clip];
        _title = [UILabel new]; _title.textColor = duo_rgb(DUO_NAVY); _title.text = L(@"設定", @"Settings"); _title.accessibilityTraits = UIAccessibilityTraitHeader; [_clip addSubview:_title];
        _resumeKey = [DuoButton new]; _resumeKey.key.faceColor = duo_rgb(DUO_ORANGE); _resumeKey.key.lipColor = duo_rgb(DUO_ORANGE_LIP);
        _resumeKey.isAccessibilityElement = YES; _resumeKey.accessibilityLabel = L(@"繼續", @"Resume"); _resumeKey.accessibilityTraits = UIAccessibilityTraitButton;
        [_resumeKey addTarget:self action:@selector(resumeTapped) forControlEvents:UIControlEventTouchUpInside]; [_clip addSubview:_resumeKey];
        NSMutableArray *tabs = [NSMutableArray array];
        for (int i = 0; i < DUO_PAGE_N; i++) {
            DuoButton *t = [DuoButton new]; t.tag = i; t.isAccessibilityElement = YES;
            [t addTarget:self action:@selector(tabTapped:) forControlEvents:UIControlEventTouchUpInside]; [_clip addSubview:t]; [tabs addObject:t];
        }
        _tabs = tabs;
        _scroll = [UIScrollView new]; _scroll.alwaysBounceVertical = NO; [_clip addSubview:_scroll];
        _body = [UIView new]; [_scroll addSubview:_body];
    }
    return self;
}
- (void)resumeTapped { _confirm = nil; _reverted = nil; if (self.resume) self.resume(); }
- (void)tabTapped:(DuoButton *)t { if (t.tag == _page) return; [[UISelectionFeedbackGenerator new] selectionChanged]; self.page = t.tag; }
- (void)setPage:(NSInteger)p { _page = MAX(0, MIN(p, DUO_PAGE_N - 1)); _confirm = nil; _flash = nil; _scroll.contentOffset = CGPointZero; [self relayout]; }
- (void)refresh { _confirm = nil; _flash = nil; NSString *f = duo_save_take_failure(); if (f) { _failed = f; _page = DUO_PAGE_SAVE; } [self relayout]; }
- (void)relayout { _laidW = 0; [self setNeedsLayout]; [self layoutIfNeeded]; UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nil); }

// 鍵：選中 = 確認橘配白字，其他 = 白鍵配深藍字；寬度跟字走 | a key: selected = confirm orange with white text, otherwise white with navy text; width follows the text
- (DuoButton *)key:(NSString *)t on:(BOOL)on tap:(void (^)(void))tap {
    DuoButton *b = [DuoButton new]; CGFloat d = 3 * _u; UIFont *f = duo_font(17 * _u, UIFontWeightBold);
    b.key.depth = d;
    if (on) { b.key.faceColor = duo_rgb(DUO_ORANGE); b.key.lipColor = duo_rgb(DUO_ORANGE_LIP); }
    [b.key setText:t font:f color:on ? UIColor.whiteColor : duo_rgb(DUO_NAVY)];
    CGFloat w = MAX(ceil([t sizeWithAttributes:@{NSFontAttributeName: f}].width) + 36 * _u, 68 * _u);
    b.frame = CGRectMake(0, 0, MIN(w, _bodyW), 44 * _u + 2 * d);
    b.isAccessibilityElement = YES; b.accessibilityLabel = t; b.accessibilityTraits = UIAccessibilityTraitButton | (on ? UIAccessibilityTraitSelected : 0);
    if (tap) [b addAction:[UIAction actionWithHandler:^(UIAction *a) { (void)a; tap(); }] forControlEvents:UIControlEventTouchUpInside];
    return b;
}
- (void)flow:(NSArray<UIView *> *)vs {   // 由左往右排，放不下就換行 | left to right, wrapping when a row is full
    CGFloat x = 0, rowH = 0, gap = 10 * _u;
    for (UIView *v in vs) {
        CGSize s = v.frame.size;
        if (x > 0 && x + s.width > _bodyW) { x = 0; _y += rowH + gap; rowH = 0; }
        v.frame = CGRectMake(x, _y, s.width, s.height); [_body addSubview:v];
        x += s.width + gap; rowH = MAX(rowH, s.height);
    }
    _y += rowH + 12 * _u;
}
- (UILabel *)label:(NSString *)t font:(UIFont *)f alpha:(CGFloat)a {
    UILabel *l = [UILabel new]; l.text = t; l.font = f; l.numberOfLines = 0; l.textColor = [duo_rgb(DUO_NAVY) colorWithAlphaComponent:a];
    CGFloat h = ceil([l sizeThatFits:CGSizeMake(_bodyW, CGFLOAT_MAX)].height);
    l.frame = CGRectMake(0, _y, _bodyW, h); [_body addSubview:l];
    return l;
}
- (void)heading:(NSString *)t { if (_y > 0) _y += 10 * _u; UILabel *l = [self label:t font:duo_font(19 * _u, UIFontWeightHeavy) alpha:1]; l.accessibilityTraits = UIAccessibilityTraitHeader; _y += l.frame.size.height + 8 * _u; }
- (void)note:(NSString *)t { UILabel *l = [self label:t font:duo_font(15 * _u, UIFontWeightSemibold) alpha:0.68]; _y += l.frame.size.height + 10 * _u; }
- (void)changed { [[UISelectionFeedbackGenerator new] selectionChanged]; [self relayout]; }

// ---- 頁 | Pages ----
- (void)pageLanguage {
    __weak DuoPanel *ws = self;
    [self heading:L(@"遊戲語言", @"Game Language")];
    NSString *pref = [ud() stringForKey:@"GameLanguage"] ?: @"", *dev = UIDevice.currentDevice.model;
    NSMutableArray *ks = [NSMutableArray arrayWithObject:[self key:[NSString stringWithFormat:L(@"跟隨 %@", @"Same as %@"), dev] on:!pref.length tap:^{
        [ud() removeObjectForKey:@"GameLanguage"]; duo_shell_install_shortcuts(); [ws changed]; }]];
    for (int i = 0; i < N_LANGS; i++) {
        NSString *code = @(k_langs[i].code);
        [ks addObject:[self key:@(k_langs[i].name) on:[pref isEqualToString:code] tap:^{ [ud() setObject:code forKey:@"GameLanguage"]; duo_shell_install_shortcuts(); [ws changed]; }]];
    }
    [self flow:ks];
    if (!pref.length) [self note:[NSString stringWithFormat:L(@"這台 %@ 的語言對應到 %@。", @"On this %@, that's %@."), dev, duo_lang_name(system_lang())]];
    NSString *next = duo_lang_code();
    if (![next isEqualToString:g_boot[@"lang"]]) [self note:[NSString stringWithFormat:L(@"下次開啟遊戲時會換成%@。", @"The game switches to %@ the next time you open it."), duo_lang_name(next)]];
}
- (void)pageDisplay {
    __weak DuoPanel *ws = self;
    [self heading:L(@"畫面比例", @"Aspect Ratio")];
    NSString *a = aspect_pref();
    [self flow:@[[self key:L(@"填滿螢幕", @"Fill the Screen") on:!a.length tap:^{ [ud() removeObjectForKey:@"GameAspect"]; [ws changed]; }],
                 [self key:@"16:9" on:[a isEqualToString:@"16:9"] tap:^{ [ud() setObject:@"16:9" forKey:@"GameAspect"]; [ws changed]; }]]];
    if (![a isEqualToString:g_boot[@"aspect"]]) [self note:L(@"下次開啟遊戲時套用。", @"Takes effect the next time you open the game.")];
    [self heading:L(@"MetalFX 放大", @"MetalFX Upscaling")];
    BOOL up = [ud() objectForKey:@"Upscale"] ? [ud() boolForKey:@"Upscale"] : YES;
    [self flow:@[[self key:L(@"開", @"On") on:up tap:^{ [ud() setBool:YES forKey:@"Upscale"]; duo_game_set_upscale(1); [ws changed]; }],
                 [self key:L(@"關", @"Off") on:!up tap:^{ [ud() setBool:NO forKey:@"Upscale"]; duo_game_set_upscale(0); [ws changed]; }]]];
    [self note:L(@"畫面放大到螢幕大小時，邊緣和文字比較清楚。", @"Keeps edges and text sharp when the picture is enlarged to fit the screen.")];
    if (!duo_game_option_known("fps60")) return;
    [self heading:L(@"幀率", @"Frame Rate")];
    int fps = fps_pref();
    [self flow:@[[self key:@"30 FPS" on:fps == 30 tap:^{ [ud() removeObjectForKey:@"FrameRate"]; [ws changed]; }],
                 [self key:@"60 FPS" on:fps == 60 tap:^{ [ud() setInteger:60 forKey:@"FrameRate"]; [ws changed]; }]]];
    if (fps == 60) [self note:L(@"比較耗電，複雜的 3D 場景也可能跑不到 60 FPS。", @"Uses more battery. Busy 3D scenes may run below 60 FPS.")];
    if (fps != [g_boot[@"fps"] intValue]) [self note:[NSString stringWithFormat:L(@"下次開啟遊戲時會改成 %d FPS。", @"The game switches to %d FPS the next time you open it."), fps]];
}
- (void)pageControls {
    __weak DuoPanel *ws = self;
    [self heading:L(@"螢幕上的按鍵", @"On-Screen Buttons")];
    BOOL hidden = [ud() boolForKey:@"PadHidden"];
    void (^pad)(void) = ^{ DuoPanel *s = ws; if (s.padChanged) s.padChanged(); [s changed]; };
    [self flow:@[[self key:L(@"顯示", @"Show") on:!hidden tap:^{ [ud() removeObjectForKey:@"PadHidden"]; pad(); }],
                 [self key:L(@"隱藏", @"Hide") on:hidden tap:^{ [ud() setBool:YES forKey:@"PadHidden"]; pad(); }]]];
    [self note:hidden ? L(@"HOME 和截圖鍵會留著。", @"HOME and Capture stay on screen.")
                      : L(@"接上手把時會自動隱藏，HOME 和截圖鍵會留著。", @"The buttons hide while a controller is connected. HOME and Capture always stay.")];
    if (hidden) return;
    [self heading:L(@"按鍵不透明度", @"Button Opacity")];
    double op = pad_opacity(); NSMutableArray *ks = [NSMutableArray array];
    for (int p = 40; p <= 100; p += 20) [ks addObject:[self key:[NSString stringWithFormat:@"%d%%", p] on:fabs(op * 100 - p) < 10 tap:^{ [ud() setDouble:p / 100.0 forKey:@"PadOpacity"]; pad(); }]];
    [self flow:ks];
    [self note:L(@"橫向拿著時，按鍵會疊在遊戲畫面上。", @"When you hold it sideways, the buttons sit over the game.")];
}
- (NSString *)when:(NSDate *)d {
    static NSDateFormatter *f; static dispatch_once_t once;
    dispatch_once(&once, ^{ f = [NSDateFormatter new]; f.locale = [NSLocale localeWithLocaleIdentifier:NSLocale.preferredLanguages.firstObject ?: @"en"]; f.dateStyle = NSDateFormatterMediumStyle; f.timeStyle = NSDateFormatterShortStyle; });
    return [f stringFromDate:d];
}
- (NSString *)backupTitle:(NSString *)name { for (NSDictionary *b in duo_backups()) if ([b[@"name"] isEqualToString:name]) return [self when:b[@"date"]]; return name; }
- (void)confirmRow:(NSString *)text yes:(NSString *)yes action:(void (^)(void))action {   // 第二步確認：說明 + 確認橘的鍵 + 取消 | the second step: what happens + a confirm-orange key + Cancel
    __weak DuoPanel *ws = self;
    [self note:text];
    [self flow:@[[self key:yes on:YES tap:action], [self key:L(@"取消", @"Cancel") on:NO tap:^{ DuoPanel *s = ws; s->_confirm = nil; [s relayout]; }]]];
}
- (void)pageSave {
    __weak DuoPanel *ws = self;
    if (_failed) [self note:[_failed isEqualToString:@"restore"] ? L(@"上次的還原沒有完成，存檔沒有變。", @"The last restore didn't finish, so your save didn't change.")
                                                                  : L(@"上次的從頭開始沒有完成，存檔沒有變。", @"Starting over didn't finish last time, so your save didn't change.")];
    NSString *pend = duo_save_pending();
    if (pend) {
        NSString *what = [pend hasPrefix:@"restore:"] ? [NSString stringWithFormat:L(@"還原 %@ 的備份", @"restore the backup from %@"), [self backupTitle:[pend substringFromIndex:8]]] : L(@"從頭開始", @"start over");
        [self heading:L(@"下次開啟遊戲時", @"Next Time You Open the Game")];
        [self note:[NSString stringWithFormat:L(@"遊戲會%@，目前的存檔會先移到備份。", @"The game will %@. Your current save moves to Backups first."), what]];
        [self flow:@[[self key:L(@"取消", @"Cancel") on:NO tap:^{ duo_save_set_pending(nil); [ws changed]; }]]];
    }
    [self heading:L(@"備份", @"Backups")];
    [self flow:@[[self key:L(@"立即備份", @"Back Up Now") on:NO tap:^{ [ws backupNow]; }], [self key:L(@"匯出到「檔案」", @"Export to Files") on:NO tap:^{ [ws exportSave]; }]]];
    if (_flash) [self note:_flash];
    NSArray *bs = duo_backups();
    if (!bs.count) [self note:L(@"還沒有備份。", @"No backups yet.")];
    for (NSUInteger i = 0; i < MIN(bs.count, (NSUInteger)20); i++) [self backupRow:bs[i]];
    if (bs.count) [self note:L(@"備份也可以在「檔案」App 裡找到。", @"You can also find backups in the Files app.")];
    [self heading:L(@"從頭開始", @"Start Over")];
    if ([_confirm isEqualToString:@"fresh"])
        [self confirmRow:L(@"目前的存檔會移到備份，不會刪除。下次開啟遊戲時從頭開始。", @"Your current save moves to Backups and isn't deleted. The game starts from the beginning the next time you open it.")
                     yes:L(@"確定從頭開始", @"Start Over") action:^{ DuoPanel *s = ws; s->_confirm = nil; duo_save_set_pending(@"fresh"); [s changed]; }];
    else [self flow:@[[self key:L(@"從頭開始…", @"Start Over…") on:NO tap:^{ DuoPanel *s = ws; s->_confirm = @"fresh"; [s changed]; }]]];
}
- (void)backupRow:(NSDictionary *)b {   // 一列：白底圓角，時間、種類，右邊「還原」 | one row: white rounded, the time and kind, Restore on the right
    __weak DuoPanel *ws = self;
    NSString *name = b[@"name"], *kind = b[@"kind"];
    NSString *k = [kind isEqualToString:@"restore"] ? L(@"還原前留下的", @"Kept before a restore") : [kind isEqualToString:@"fresh"] ? L(@"從頭開始前留下的", @"Kept before starting over") : nil;
    CGFloat h = (k ? 62 : 52) * _u, r = 16 * _u;
    UIView *row = [[UIView alloc] initWithFrame:CGRectMake(0, _y, _bodyW, h)];
    row.backgroundColor = UIColor.whiteColor; row.layer.cornerRadius = r; [_body addSubview:row];
    DuoButton *re = [self key:L(@"還原", @"Restore") on:NO tap:^{ DuoPanel *s = ws; s->_confirm = [@"restore:" stringByAppendingString:name]; [s changed]; }];
    re.key.faceColor = duo_rgb(DUO_FIELD); re.key.lipColor = duo_rgb(DUO_WAVE); re.key.dropColor = duo_rgb(DUO_LIP);   // 白底上用淺黃鍵 | a pale yellow key on the white row
    CGFloat rw = re.frame.size.width, rh = re.frame.size.height; re.frame = CGRectMake(_bodyW - rw - 8 * _u, (h - rh) / 2 + 1.5 * _u, rw, rh); [row addSubview:re];
    UILabel *t = [UILabel new]; t.text = [self when:b[@"date"]]; t.font = duo_font(17 * _u, UIFontWeightBold); t.textColor = duo_rgb(DUO_NAVY);
    t.frame = CGRectMake(16 * _u, k ? 9 * _u : 0, _bodyW - rw - 32 * _u, k ? 24 * _u : h); [row addSubview:t];
    if (k) { UILabel *s = [UILabel new]; s.text = k; s.font = duo_font(14 * _u, UIFontWeightSemibold); s.textColor = [duo_rgb(DUO_NAVY) colorWithAlphaComponent:0.6]; s.frame = CGRectMake(16 * _u, 33 * _u, t.frame.size.width, 20 * _u); [row addSubview:s]; }
    re.accessibilityLabel = [NSString stringWithFormat:L(@"還原 %@ 的備份", @"Restore the backup from %@"), t.text];
    if (k) { UILabel *sub = row.subviews.lastObject; row.accessibilityElements = @[t, sub, re]; } else row.accessibilityElements = @[t, re];
    _y += h + 8 * _u;
    if ([_confirm isEqualToString:[@"restore:" stringByAppendingString:name]])
        [self confirmRow:L(@"目前的存檔會先移到備份，下次開啟遊戲時換成這份。", @"Your current save moves to Backups first, and this backup loads the next time you open the game.")
                     yes:L(@"確定還原", @"Restore") action:^{ DuoPanel *s = ws; s->_confirm = nil; duo_save_set_pending([@"restore:" stringByAppendingString:name]); [s changed]; }];
}
- (void)quitSection {
    __weak DuoPanel *ws = self;
    [self heading:L(@"關閉遊戲", @"Close Game")];
    if ([_confirm isEqualToString:@"quit"])
        [self confirmRow:L(@"遊戲上次存檔之後的進度會不見。", @"Progress since the game last saved will be lost.") yes:L(@"關閉遊戲", @"Close Game") action:^{
            [ud() synchronize]; fprintf(stderr, "duo: 從設定關閉遊戲\n"); duo_game_quit(); }];
    else {
        [self note:L(@"想現在套用，就關閉遊戲再開一次。", @"To apply these changes now, close the game and open it again.")];
        [self flow:@[[self key:L(@"關閉遊戲…", @"Close Game…") on:NO tap:^{ DuoPanel *s = ws; s->_confirm = @"quit"; [s changed]; }]]];
    }
}

// ---- 存檔動作 | Save actions ----
- (NSString *)saveError:(int)e { return e == ENOENT ? L(@"還沒有存檔可以備份。", @"There's no save to back up yet.") : L(@"存檔沒能複製，可能是儲存空間不夠。", @"Couldn't copy the save. The device may be low on storage."); }
- (void)backupNow {
    int e = 0; NSString *n = duo_backup_now(&e);
    _flash = n ? L(@"已備份。", @"Backed up.") : [self saveError:e];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, _flash);
    [[UINotificationFeedbackGenerator new] notificationOccurred:n ? UINotificationFeedbackTypeSuccess : UINotificationFeedbackTypeError];
    [self relayout];
}
- (void)exportSave {
    int e = 0; NSURL *u = duo_save_export_copy(&e);
    if (!u) { _flash = [self saveError:e]; [self relayout]; return; }
    UIViewController *vc = self.window.rootViewController; while (vc.presentedViewController) vc = vc.presentedViewController;
    if (!vc) { [NSFileManager.defaultManager removeItemAtURL:u.URLByDeletingLastPathComponent error:nil]; return; }   // 沒有能出示選擇器的畫面：拷貝不留 | nowhere to present the picker: drop the copy
    _export = u;
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[u] asCopy:YES];
    p.delegate = self; [vc presentViewController:p animated:YES completion:nil];
}
- (void)exportDone:(BOOL)ok {
    if (_export) [NSFileManager.defaultManager removeItemAtURL:_export.URLByDeletingLastPathComponent error:nil];   // 這份暫存拷貝 | that temporary copy
    _export = nil;
    if (ok) { _flash = L(@"已匯出。", @"Exported."); [self relayout]; }
}
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls { (void)c; (void)urls; [self exportDone:YES]; }
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c { (void)c; [self exportDone:NO]; }

// ---- 版面 | Layout ----
- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect b = self.bounds; UIEdgeInsets sa = self.safeAreaInsets;
    // 直拿時不算左右安全區：Duo 外螢幕直拿時右邊整條都算安全區，照算卡片會窄得排不下分頁鍵（相機另外用遮擋區讓開，見下） | upright, the side safe areas are ignored: on the Duo's outer display held upright the whole right strip counts as safe area, which would make the card too narrow for the page keys (the camera is avoided through the occlusion region, below)
    CGFloat side = b.size.width > b.size.height ? sa.left + sa.right : 0;
    CGFloat m = 16, W = MIN(b.size.width - side - 2 * m, 620), maxH = b.size.height - sa.top - sa.bottom - 2 * m;
    if (W <= 0 || maxH <= 0) return;
    _u = MIN(MAX(W / 580, 0.8), 1.15);
    CGFloat u = _u, pad = 22 * u, d = 3 * u;
    // 標題列：左邊「設定」，右邊確認橘的「繼續」 | the title row: Settings on the left, Resume in confirm orange on the right
    UIFont *rf = duo_font(18 * u, UIFontWeightHeavy); NSString *rt = L(@"繼續", @"Resume");
    CGFloat rw = ceil([rt sizeWithAttributes:@{NSFontAttributeName: rf}].width) + 44 * u, rh = 44 * u + 2 * d;
    _resumeKey.key.depth = d; [_resumeKey.key setText:rt font:rf color:UIColor.whiteColor];
    _resumeKey.frame = CGRectMake(W - pad - rw, pad - 4 * u, rw, rh);
    _title.font = duo_font(26 * u, UIFontWeightHeavy); _title.frame = CGRectMake(pad, pad - 4 * u, W - 3 * pad - rw, 44 * u);
    // 分頁鍵 | the page keys
    NSString *names[DUO_PAGE_N] = {L(@"語言", @"Language"), L(@"畫面", @"Display"), L(@"按鍵", @"Controls"), L(@"存檔", @"Save Data")};
    _bodyW = W - 2 * pad; _y = 0;
    CGFloat x = pad, y = pad + 56 * u, gap = 10 * u; UIFont *tf = duo_font(17 * u, UIFontWeightBold);
    for (DuoButton *t in _tabs) {
        BOOL on = t.tag == _page; NSString *n = names[t.tag];
        t.key.depth = d; t.key.faceColor = on ? duo_rgb(DUO_ORANGE) : UIColor.whiteColor; t.key.lipColor = on ? duo_rgb(DUO_ORANGE_LIP) : duo_rgb(DUO_LIP);
        [t.key setText:n font:tf color:on ? UIColor.whiteColor : duo_rgb(DUO_NAVY)];
        t.accessibilityLabel = n; t.accessibilityTraits = UIAccessibilityTraitButton | (on ? UIAccessibilityTraitSelected : 0);
        CGFloat w = MAX(ceil([n sizeWithAttributes:@{NSFontAttributeName: tf}].width) + 28 * u, 64 * u);   // 窄一點，外螢幕上四頁排得進一行 | a little narrower, so the four pages fit one row on the outer display
        if (x > pad && x + w > W - pad) { x = pad; y += rh + gap; }
        t.frame = CGRectMake(x, y, w, rh); x += w + gap;
    }
    CGFloat top = y + rh + 16 * u;
    if (_laidW != W) {   // 內容按寬度重建（換頁、改設定時 _laidW 歸零） | rebuild the contents for this width (_laidW is reset on page and setting changes)
        _laidW = W;
        for (UIView *v in _body.subviews) [v removeFromSuperview];
        if (_reverted.count) {   // 開機保護改回了設定：每頁最上面說一次 | the safe-boot guard put settings back: say so at the top of the page
            NSMutableArray *names = [NSMutableArray array]; for (NSString *k in _reverted) [names addObject:guard_name(k)];
            [self heading:L(@"設定已改回", @"Settings Put Back")];
            [self note:[NSString stringWithFormat:L(@"上次沒能開啟遊戲，這些設定已改回之前能開的：%@。", @"The game didn't open last time, so these went back to settings that worked: %@."), [names componentsJoinedByString:L(@"、", @", ")]]];
        }
        switch (_page) {
            case DUO_PAGE_LANGUAGE: [self pageLanguage]; break;
            case DUO_PAGE_DISPLAY: [self pageDisplay]; break;
            case DUO_PAGE_CONTROLS: [self pageControls]; break;
            default: [self pageSave]; break;
        }
        if (boot_changed()) [self quitSection];
        _body.frame = CGRectMake(pad, 0, _bodyW, _y);
    }
    // 卡片頂邊固定（按一張約 700 點高的卡片置中），換頁時標題和分頁鍵不跳；內容短就往下收，長就捲 | the card's top edge stays put (centred as for a card about 700 points tall), so the title and page keys do not jump between pages; short contents end higher, long ones scroll
    CGFloat y0 = round(sa.top + m + MAX(0, (maxH - MIN(maxH, 700 * u)) / 2));
    if (@available(iOS 27.1, *)) {   // 卡片頂上那一段（標題、繼續鍵）碰到相機（iOS 27.1 的遮擋保留區）就往下移 | move down when the card's top part (title, Resume) meets a camera (the iOS 27.1 occlusion reserved region)
        CGRect head = CGRectMake(round((b.size.width - W) / 2), y0, W, top);
        for (UIViewReservedRegion *r in [self reservedRegionsOfKind:UIViewReservedRegionKind.occlusionRegionKind])
            if (CGRectIntersectsRect(r.frame, head)) y0 = MAX(y0, ceil(CGRectGetMaxY(r.frame) + 8));
    }
    CGFloat H = MIN(top + _body.frame.size.height + pad, b.size.height - sa.bottom - m - y0);
    _card.frame = CGRectMake(round((b.size.width - W) / 2), y0, W, H);
    _clip.frame = _card.bounds; _clip.layer.cornerRadius = 28 * u;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    _shadow.frame = _card.bounds; _shadow.path = [UIBezierPath bezierPathWithRoundedRect:_card.bounds cornerRadius:28 * u].CGPath; _shadow.shadowPath = _shadow.path;
    [CATransaction commit];
    _scroll.frame = CGRectMake(0, top, W, H - top);
    _scroll.contentSize = CGSizeMake(W, _body.frame.size.height + pad);
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (BOOL)accessibilityViewIsModal { return YES; }
@end
