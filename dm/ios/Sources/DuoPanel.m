// 設定面板：按 HOME 打開，遊戲暫停；整個畫面換成遊戲那種黃色的頁，樣子取自遊戲自己的選單（居住地區、島名頁、日期確認）。介面在 duo_shell.h。四頁：
//   語言：遊戲語言（跟隨這台裝置，或遊戲的 13 種之一），下次開啟遊戲時生效。
//   畫面：畫面比例（填滿螢幕 / 16:9，下次開啟）、MetalFX 放大（即時）、幀率（30 / 60：這個遊戲有 fps60 剪口才出現，下次開啟）。
//   按鍵：螢幕上的按鍵顯示 / 隱藏（HOME 和截圖鍵一律留著）、疊在畫面上時的不透明度，即時。
//   存檔：立即備份、匯出到「檔案」、點一份備份還原、從頭開始。還原和從頭開始要在確認頁再按一次，下次開啟遊戲時才做，目前的存檔先整個搬進備份（DuoSave.m）。
//   開機才讀的設定改過、還沒套用時，每頁底部有「關閉遊戲」，也要在確認頁再按一次，並說明上次存檔之後的進度會不見。只有淡入淡出，沒有呼吸 / 脈動。
// The settings panel: opened by HOME with the game paused; the whole screen becomes one of the game's yellow pages, its look taken from the game's own menus (region of residence, island name, date check). Interface in duo_shell.h. Four pages:
//   Language: the game language (same as this device, or one of the game's 13), effective the next time the game opens.
//   Display: the aspect ratio (fill the screen / 16:9, next opening), MetalFX upscaling (immediate), frame rate (30 / 60: shown only when this game has the fps60 cut, next opening).
//   Controls: on-screen buttons shown / hidden (HOME and capture always stay) and their opacity over the picture, immediate.
//   Save data: back up now, export to Files, tap a backup to restore it, start over. Restore and start over need a second press on a confirmation page and run at the next opening, after the whole current save is moved into a backup (DuoSave.m).
//   While a boot-time setting has changed and not yet applied, every page ends with Close Game, which also needs a second press on a confirmation page saying that progress since the last save will be lost. Fades only: no breathing or pulsing.
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

// ---- 遊戲樣式的選項鍵 | Option keys in the game's style ----
// 照遊戲自己的選單（居住地區、島名頁）取樣：白色、輪廓略帶手繪起伏的膠囊，下緣一道灰（#E5E5E5），落在黃底上一道深黃影（#E7AE00）；
// 選中的換成黃色（#FFE353）加淺色波浪紋（#FFF0A8）；確認、會動到存檔的那一步用確認橘。按下時鍵面往下沉、蓋住下緣，同外殼的鍵
// Sampled from the game's own menus (region of residence, island name): a white capsule whose outline wobbles slightly as if hand-drawn, a grey lower edge (#E5E5E5) and a darker-yellow drop on the yellow (#E7AE00);
// the picked one turns yellow (#FFE353) with pale wavy stripes (#FFF0A8); the confirm step, and any step that touches the save, uses the confirm orange. Pressing sinks the face over the edge, as the shell's keys do
enum { PILL_PLAIN, PILL_PICKED, PILL_CONFIRM };
static const uint32_t k_pick = 0xFFE353, k_pick_stripe = 0xFFF0A8, k_pick_lip = 0xECC23A, k_note = 0xF2660D, k_rule = 0xFFA100;

static UIBezierPath *wobble(CGRect r) {   // 手繪感的膠囊：上下兩邊各有一點起伏 | a hand-drawn-looking capsule: the top and bottom edges rise and dip a little
    CGFloat w = r.size.width, h = r.size.height, rr = h / 2, x = r.origin.x, y = r.origin.y;
    UIBezierPath *p = [UIBezierPath bezierPath];
    [p moveToPoint:CGPointMake(x + rr, y)];
    [p addCurveToPoint:CGPointMake(x + w - rr, y) controlPoint1:CGPointMake(x + w * 0.36, y - h * 0.05) controlPoint2:CGPointMake(x + w * 0.64, y + h * 0.04)];
    [p addArcWithCenter:CGPointMake(x + w - rr, y + rr) radius:rr startAngle:-M_PI_2 endAngle:M_PI_2 clockwise:YES];
    [p addCurveToPoint:CGPointMake(x + rr, y + h) controlPoint1:CGPointMake(x + w * 0.62, y + h * 1.04) controlPoint2:CGPointMake(x + w * 0.38, y + h * 0.97)];
    [p addArcWithCenter:CGPointMake(x + rr, y + rr) radius:rr startAngle:M_PI_2 endAngle:M_PI_2 * 3 clockwise:YES];
    [p closePath];
    return p;
}

@interface DuoPill : UIControl
@property (nonatomic) int style;
@property (nonatomic) CGFloat depth;
@property (nonatomic, readonly) UILabel *title, *sub;
@end
@implementation DuoPill {
    CAShapeLayer *_drop, *_lip, *_face, *_stripes, *_stripeMask;
}
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        _drop = [CAShapeLayer layer]; _lip = [CAShapeLayer layer]; _face = [CAShapeLayer layer]; _stripes = [CAShapeLayer layer]; _stripeMask = [CAShapeLayer layer];
        _stripes.fillColor = nil; _stripes.lineCap = kCALineCapRound; _stripes.mask = _stripeMask;
        for (CALayer *l in @[_drop, _lip, _face, _stripes]) [self.layer addSublayer:l];
        _title = [UILabel new]; _title.textAlignment = NSTextAlignmentCenter; _title.adjustsFontSizeToFitWidth = YES; _title.minimumScaleFactor = 0.7; [self addSubview:_title];
        _sub = [UILabel new]; _sub.textAlignment = NSTextAlignmentCenter; _sub.hidden = YES; [self addSubview:_sub];
        _title.userInteractionEnabled = _sub.userInteractionEnabled = NO;
        _depth = 4; self.isAccessibilityElement = YES;
    }
    return self;
}
- (void)setStyle:(int)s { _style = s; [self setNeedsLayout]; }
- (void)layoutSubviews {
    [super layoutSubviews];
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    CGFloat d = _depth, w = self.bounds.size.width, h = self.bounds.size.height - 2 * d, sink = self.highlighted ? d : 0;
    CGRect face = CGRectMake(0, 0, w, h);
    _drop.path = [wobble(CGRectOffset(face, 0, 2 * d)) CGPath]; _lip.path = [wobble(CGRectOffset(face, 0, d)) CGPath];
    _face.path = _stripeMask.path = [wobble(CGRectOffset(face, 0, sink)) CGPath];
    UIColor *faceC = _style == PILL_PICKED ? duo_rgb(k_pick) : _style == PILL_CONFIRM ? duo_rgb(DUO_ORANGE) : UIColor.whiteColor;
    UIColor *lipC = _style == PILL_PICKED ? duo_rgb(k_pick_lip) : _style == PILL_CONFIRM ? duo_rgb(DUO_ORANGE_LIP) : duo_rgb(DUO_LIP);
    _face.fillColor = faceC.CGColor; _lip.fillColor = lipC.CGColor; _drop.fillColor = duo_rgb(DUO_DROP).CGColor;
    _stripes.hidden = _style != PILL_PICKED;
    if (!_stripes.hidden) {   // 淺色波浪紋：幾道橫向的正弦線，同遊戲選中的鍵 | the pale wavy stripes: a few horizontal sine lines, as on the game's picked key
        UIBezierPath *sp = [UIBezierPath bezierPath]; CGFloat lam = h * 0.46, amp = h * 0.075;
        for (CGFloat yy = h * 0.2; yy < h + amp; yy += h * 0.4) {
            [sp moveToPoint:CGPointMake(-lam, yy + sink)];
            for (CGFloat x = -lam; x <= w + lam; x += lam / 8) [sp addLineToPoint:CGPointMake(x, yy + sink + amp * sin(x / lam * 2 * M_PI))];
        }
        _stripes.path = sp.CGPath; _stripes.strokeColor = duo_rgb(k_pick_stripe).CGColor; _stripes.lineWidth = h * 0.11; _stripes.frame = self.bounds; _stripeMask.frame = self.bounds;
    }
    [CATransaction commit];
    UIColor *textC = _style == PILL_CONFIRM ? UIColor.whiteColor : duo_rgb(DUO_NAVY);
    _title.textColor = textC; _sub.textColor = [textC colorWithAlphaComponent:0.62];
    CGFloat pad = h * 0.45, th = _sub.hidden ? h : h * 0.56;
    _title.frame = CGRectMake(pad, sink + (_sub.hidden ? 0 : h * 0.12), w - 2 * pad, _sub.hidden ? h : th - h * 0.06);
    _sub.frame = CGRectMake(pad, sink + h * 0.52, w - 2 * pad, h * 0.34);
}
- (void)setHighlighted:(BOOL)hl { BOOL was = self.highlighted; [super setHighlighted:hl]; if (was != hl) [self setNeedsLayout]; }
@end

// ---- 面板 | The panel ----
// 整個畫面變成遊戲那種黃色的頁（遊戲暫停中，畫面本來就停著）：上面置中的標題、右上確認橘的「繼續」、一排分頁鍵；底下是遊戲的波浪帶。
// 每項設定是一排大選項鍵（兩到四欄，選中的是黃色波浪紋）；開機才生效、或會動到存檔的提醒用遊戲的警示橘字。從頭開始、還原、關閉遊戲的第二步照遊戲的確認頁：
// 置中的問句、一條淺色帶寫要做的事、橘字說明，下面左「取消」右確認橘
// The whole screen becomes one of the game's yellow pages (the game is paused, so its picture is still anyway): a centred title, Resume in confirm orange at the top right, a row of page keys, and the game's wave band along the bottom.
// Each setting is a row of big option keys (two to four columns, the picked one with the yellow wavy pattern); notes about the next launch or about the save are in the game's warning orange. The second step of Start Over, Restore and Close Game follows the game's own confirmation page:
// a centred question, a pale band with what will happen, an orange note, then Cancel on the left and the confirm-orange key on the right
@interface DuoPanel () <UIDocumentPickerDelegate>
@end
@implementation DuoPanel {
    UIImageView *_band; UIView *_head, *_body; UIScrollView *_scroll; UILabel *_title; DuoPill *_resumeKey; NSArray<DuoPill *> *_tabs; CAShapeLayer *_rule;
    CGFloat _u, _y, _bodyW, _laidW, _bodyH; CGSize _bandSize;
    CGFloat _foldA, _foldB, _laidFold;   // 直的摺痕（內容坐標的左右邊，沒有 = 0, 0）：偶數欄分在兩邊 | a vertical fold (left and right edges in content coordinates; none = 0, 0): even column counts split around it
    NSString *_confirm;     // 開著的確認頁：@"fresh"、@"quit"、@"restore:<名>"，沒有 = nil | the open confirmation page: @"fresh", @"quit", @"restore:<name>", none = nil
    NSString *_flash;       // 存檔頁上一個動作的結果 | the result of the last action on the save page
    NSString *_failed;      // 上次開機的還原 / 從頭開始沒做成（duo_save_take_failure） | the last boot's restore / start over failed (duo_save_take_failure)
    NSArray *_reverted;     // 開機保護改回的設定（SafeBootReverted） | settings put back by the safe-boot guard (SafeBootReverted)
    NSURL *_export;
}
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        boot_note(); _failed = duo_save_take_failure();
        _reverted = [ud() arrayForKey:@"SafeBootReverted"]; [ud() removeObjectForKey:@"SafeBootReverted"];
        self.backgroundColor = duo_rgb(DUO_YELLOW);
        _band = [UIImageView new]; [self addSubview:_band];
        _scroll = [UIScrollView new]; _scroll.alwaysBounceVertical = NO; _scroll.showsHorizontalScrollIndicator = NO; [self addSubview:_scroll];
        _body = [UIView new]; [_scroll addSubview:_body];
        _head = [UIView new]; _head.backgroundColor = duo_rgb(DUO_YELLOW); [self addSubview:_head];
        _rule = [CAShapeLayer layer]; _rule.strokeColor = duo_rgb(k_rule).CGColor; _rule.lineCap = kCALineCapRound; [_head.layer addSublayer:_rule];
        _title = [UILabel new]; _title.textColor = duo_rgb(DUO_NAVY); _title.text = L(@"設定", @"Settings"); _title.textAlignment = NSTextAlignmentCenter; _title.accessibilityTraits = UIAccessibilityTraitHeader; [_head addSubview:_title];
        _resumeKey = [DuoPill new]; _resumeKey.style = PILL_CONFIRM; _resumeKey.title.text = L(@"繼續", @"Resume"); _resumeKey.accessibilityLabel = _resumeKey.title.text; _resumeKey.accessibilityTraits = UIAccessibilityTraitButton;
        [_resumeKey addTarget:self action:@selector(resumeTapped) forControlEvents:UIControlEventTouchUpInside]; [_head addSubview:_resumeKey];
        NSMutableArray *tabs = [NSMutableArray array];
        for (int i = 0; i < DUO_PAGE_N; i++) { DuoPill *t = [DuoPill new]; t.tag = i; [t addTarget:self action:@selector(tabTapped:) forControlEvents:UIControlEventTouchUpInside]; [_head addSubview:t]; [tabs addObject:t]; }
        _tabs = tabs;
    }
    return self;
}
- (void)resumeTapped { _confirm = nil; _reverted = nil; if (self.resume) self.resume(); }
- (void)tabTapped:(DuoPill *)t { if (t.tag == _page && !_confirm) return; [[UISelectionFeedbackGenerator new] selectionChanged]; self.page = t.tag; }
- (void)setPage:(NSInteger)p { _page = MAX(0, MIN(p, DUO_PAGE_N - 1)); _confirm = nil; _flash = nil; _scroll.contentOffset = CGPointZero; [self relayout]; }
- (void)refresh { _confirm = nil; _flash = nil; NSString *f = duo_save_take_failure(); if (f) { _failed = f; _page = DUO_PAGE_SAVE; } [self relayout]; }
- (void)relayout { _laidW = 0; [self setNeedsLayout]; [self layoutIfNeeded]; UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nil); }
- (void)changed { [[UISelectionFeedbackGenerator new] selectionChanged]; [self relayout]; }
- (void)ask:(NSString *)what { _confirm = what; _scroll.contentOffset = CGPointZero; [self changed]; }

// ---- 內容積木 | Content pieces ----
- (DuoPill *)pill:(NSString *)t sub:(NSString *)sub style:(int)style tap:(void (^)(void))tap {
    DuoPill *b = [DuoPill new]; b.style = style; b.depth = 4 * _u;
    b.title.text = t; b.title.font = duo_font(19 * _u, UIFontWeightBold);
    if (sub.length) { b.sub.hidden = NO; b.sub.text = sub; b.sub.font = duo_font(14 * _u, UIFontWeightSemibold); }
    b.accessibilityLabel = sub.length ? [NSString stringWithFormat:@"%@，%@", t, sub] : t;
    b.accessibilityTraits = UIAccessibilityTraitButton | (style == PILL_PICKED ? UIAccessibilityTraitSelected : 0);
    if (tap) [b addAction:[UIAction actionWithHandler:^(UIAction *a) { (void)a; tap(); }] forControlEvents:UIControlEventTouchUpInside];
    return b;
}
// 第 i 欄的位置和寬：有直的摺痕、欄數是偶數時，一半在摺痕左邊、一半在右邊 | column i's position and width: with a vertical fold and an even column count, half sit left of the fold and half right of it
- (CGRect)col:(int)i of:(int)cols gap:(CGFloat)gap width:(CGFloat)W {
    if (_foldB > _foldA && cols % 2 == 0 && _foldA > W * 0.3 && _foldB < W * 0.7) {
        int half = cols / 2; BOOL right = i >= half; CGFloat a = right ? _foldB + gap : 0, b = right ? W : _foldA - gap;
        CGFloat w = floor((b - a - gap * (half - 1)) / half); return CGRectMake(a + (i % half) * (w + gap), 0, w, 0);
    }
    CGFloat w = floor((W - gap * (cols - 1)) / cols); return CGRectMake(i * (w + gap), 0, w, 0);
}
// 一排選項鍵：等寬分欄（每欄至少約 150 點寬、放得下最長的字，最多 cols 欄），同遊戲的居住地區頁 | a set of option keys in equal columns (each at least about 150 points wide and wide enough for the longest label, at most cols columns), as on the game's region page
- (void)grid:(NSArray<DuoPill *> *)ps cols:(int)maxCols tall:(CGFloat)tall {
    CGFloat gap = 14 * _u, h = tall * _u + 8 * _u, minW = 150 * _u;
    for (DuoPill *p in ps) minW = MAX(minW, ceil([p.title.text sizeWithAttributes:@{NSFontAttributeName: p.title.font}].width * 0.8) + h * 0.9);   // 最長的字（可縮到八成）放得下才多分欄 | more columns only while the longest label (which may shrink to 80%) still fits
    int cols = (int)MAX(1, MIN(MIN((NSInteger)maxCols, (NSInteger)ps.count), floor((_bodyW + gap) / (minW + gap))));
    if (_foldB > _foldA && cols >= 3 && cols % 2) cols--;   // 直的摺痕：欄數取偶數，不讓中間那欄壓在摺痕上 | a vertical fold: an even column count, so no middle column sits on the fold
    for (NSUInteger i = 0; i < ps.count; i++) {
        CGRect c = [self col:(int)(i % cols) of:cols gap:gap width:_bodyW];
        ps[i].frame = CGRectMake(c.origin.x, _y + (i / cols) * (h + gap * 0.7), c.size.width, h); [_body addSubview:ps[i]];
    }
    _y += ((ps.count + cols - 1) / cols) * (h + gap * 0.7) + 6 * _u;
}
- (UILabel *)label:(NSString *)t font:(UIFont *)f color:(UIColor *)c align:(NSTextAlignment)a {
    UILabel *l = [UILabel new]; l.text = t; l.font = f; l.numberOfLines = 0; l.textColor = c; l.textAlignment = a;
    CGFloat h = ceil([l sizeThatFits:CGSizeMake(_bodyW, CGFLOAT_MAX)].height);
    l.frame = CGRectMake(0, _y, _bodyW, h); [_body addSubview:l];
    return l;
}
- (void)heading:(NSString *)t {
    if (_y > 0) _y += 18 * _u;
    UILabel *l = [self label:t font:duo_font(21 * _u, UIFontWeightHeavy) color:duo_rgb(DUO_NAVY) align:NSTextAlignmentLeft];
    l.accessibilityTraits = UIAccessibilityTraitHeader; _y += l.frame.size.height + 12 * _u;
}
- (void)note:(NSString *)t { UILabel *l = [self label:t font:duo_font(16 * _u, UIFontWeightSemibold) color:[duo_rgb(DUO_NAVY) colorWithAlphaComponent:0.72] align:NSTextAlignmentLeft]; _y += l.frame.size.height + 10 * _u; }
- (void)warn:(NSString *)t { UILabel *l = [self label:t font:duo_font(16 * _u, UIFontWeightBold) color:duo_rgb(k_note) align:NSTextAlignmentLeft]; _y += l.frame.size.height + 10 * _u; }   // 遊戲的警示橘 | the game's warning orange

// 遊戲的確認頁：置中的問句、淺色帶寫要做的事、橘字說明、下面左「取消」右確認橘 | the game's confirmation page: a centred question, a pale band with what will happen, an orange note, then Cancel and the confirm-orange key
- (void)dialog:(NSString *)question band:(NSString *)band note:(NSString *)note yes:(NSString *)yes action:(void (^)(void))action {
    __weak DuoPanel *ws = self;
    CGFloat top = _y;
    UILabel *q = [self label:question font:duo_font(23 * _u, UIFontWeightHeavy) color:duo_rgb(DUO_NAVY) align:NSTextAlignmentCenter]; _y += q.frame.size.height + 18 * _u;
    UIFont *bf = duo_font(24 * _u, UIFontWeightHeavy);
    CGFloat bh = ceil([band boundingRectWithSize:CGSizeMake(_bodyW - 40 * _u, CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin attributes:@{NSFontAttributeName: bf} context:nil].size.height) + 34 * _u;
    CAGradientLayer *g = [CAGradientLayer layer]; g.frame = CGRectMake(-_bodyW * 0.1, _y, _bodyW * 1.2, bh); g.startPoint = CGPointMake(0, 0.5); g.endPoint = CGPointMake(1, 0.5);   // 兩端淡出的淺色帶，同遊戲日期確認頁 | a pale band fading out at both ends, as on the game's date check page
    UIColor *pale = duo_rgb(DUO_WAVE); g.colors = @[(id)[pale colorWithAlphaComponent:0].CGColor, (id)pale.CGColor, (id)pale.CGColor, (id)[pale colorWithAlphaComponent:0].CGColor]; g.locations = @[@0, @0.18, @0.82, @1];
    [_body.layer addSublayer:g];
    UILabel *b = [self label:band font:bf color:duo_rgb(DUO_NAVY) align:NSTextAlignmentCenter]; b.frame = CGRectMake(20 * _u, _y + 17 * _u, _bodyW - 40 * _u, bh - 34 * _u); _y += bh + 16 * _u;
    if (note.length) { UILabel *n = [self label:note font:duo_font(17 * _u, UIFontWeightBold) color:duo_rgb(k_note) align:NSTextAlignmentCenter]; _y += n.frame.size.height + 22 * _u; }
    CGFloat saveW = _bodyW; _bodyW = MIN(_bodyW, 560 * _u); CGFloat x0 = (saveW - _bodyW) / 2, y0 = _y;
    [self grid:@[[self pill:L(@"取消", @"Cancel") sub:nil style:PILL_PLAIN tap:^{ DuoPanel *s = ws; s->_confirm = nil; [s changed]; }],
                 [self pill:yes sub:nil style:PILL_CONFIRM tap:action]] cols:2 tall:60];
    for (UIView *v in _body.subviews) if (v.frame.origin.y >= y0 - 1) v.frame = CGRectOffset(v.frame, x0, 0);
    _bodyW = saveW;
    CGFloat slack = _bodyH - (_y - top);   // 頁裡只有這個確認：上下置中 | the confirmation is all the page holds: centre it vertically
    if (slack > 0) { for (UIView *v in _body.subviews) v.frame = CGRectOffset(v.frame, 0, slack * 0.4); g.frame = CGRectOffset(g.frame, 0, slack * 0.4); _y += slack * 0.4; }
}

// ---- 頁 | Pages ----
- (void)pageLanguage {
    __weak DuoPanel *ws = self;
    [self heading:L(@"遊戲語言", @"Game Language")];
    NSString *pref = [ud() stringForKey:@"GameLanguage"] ?: @"", *dev = UIDevice.currentDevice.model;
    NSMutableArray *ps = [NSMutableArray arrayWithObject:[self pill:[NSString stringWithFormat:L(@"跟隨 %@", @"Same as %@"), dev] sub:nil style:pref.length ? PILL_PLAIN : PILL_PICKED tap:^{
        [ud() removeObjectForKey:@"GameLanguage"]; duo_shell_install_shortcuts(); [ws changed]; }]];
    for (int i = 0; i < N_LANGS; i++) {
        NSString *code = @(k_langs[i].code);
        [ps addObject:[self pill:@(k_langs[i].name) sub:nil style:[pref isEqualToString:code] ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() setObject:code forKey:@"GameLanguage"]; duo_shell_install_shortcuts(); [ws changed]; }]];
    }
    [self grid:ps cols:4 tall:56];
    if (!pref.length) [self note:[NSString stringWithFormat:L(@"這台 %@ 的語言對應到 %@。", @"On this %@, that's %@."), dev, duo_lang_name(system_lang())]];
    NSString *next = duo_lang_code();
    if (![next isEqualToString:g_boot[@"lang"]]) [self warn:[NSString stringWithFormat:L(@"下次開啟遊戲時會換成%@。", @"The game switches to %@ the next time you open it."), duo_lang_name(next)]];
}
- (void)pageDisplay {
    __weak DuoPanel *ws = self;
    [self heading:L(@"畫面比例", @"Aspect Ratio")];
    NSString *a = aspect_pref();
    [self grid:@[[self pill:L(@"填滿螢幕", @"Fill the Screen") sub:nil style:!a.length ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() removeObjectForKey:@"GameAspect"]; [ws changed]; }],
                 [self pill:@"16:9" sub:nil style:[a isEqualToString:@"16:9"] ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() setObject:@"16:9" forKey:@"GameAspect"]; [ws changed]; }]] cols:2 tall:56];
    if (![a isEqualToString:g_boot[@"aspect"]]) [self warn:L(@"下次開啟遊戲時套用。", @"Takes effect the next time you open the game.")];
    [self heading:L(@"MetalFX 放大", @"MetalFX Upscaling")];
    BOOL up = [ud() objectForKey:@"Upscale"] ? [ud() boolForKey:@"Upscale"] : YES;
    [self grid:@[[self pill:L(@"開", @"On") sub:nil style:up ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() setBool:YES forKey:@"Upscale"]; duo_game_set_upscale(1); [ws changed]; }],
                 [self pill:L(@"關", @"Off") sub:nil style:up ? PILL_PLAIN : PILL_PICKED tap:^{ [ud() setBool:NO forKey:@"Upscale"]; duo_game_set_upscale(0); [ws changed]; }]] cols:2 tall:56];
    [self note:L(@"畫面放大到螢幕大小時，邊緣和文字比較清楚。", @"Keeps edges and text sharp when the picture is enlarged to fit the screen.")];
    if (!duo_game_option_known("fps60")) return;
    [self heading:L(@"幀率", @"Frame Rate")];
    int fps = fps_pref();
    [self grid:@[[self pill:@"30 FPS" sub:nil style:fps == 30 ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() removeObjectForKey:@"FrameRate"]; [ws changed]; }],
                 [self pill:@"60 FPS" sub:nil style:fps == 60 ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() setInteger:60 forKey:@"FrameRate"]; [ws changed]; }]] cols:2 tall:56];
    if (fps == 60) [self note:L(@"比較耗電，複雜的 3D 場景也可能跑不到 60 FPS。", @"Uses more battery. Busy 3D scenes may run below 60 FPS.")];
    if (fps != [g_boot[@"fps"] intValue]) [self warn:[NSString stringWithFormat:L(@"下次開啟遊戲時會改成 %d FPS。", @"The game switches to %d FPS the next time you open it."), fps]];
}
- (void)pageControls {
    __weak DuoPanel *ws = self;
    [self heading:L(@"螢幕上的按鍵", @"On-Screen Buttons")];
    BOOL hidden = [ud() boolForKey:@"PadHidden"];
    void (^pad)(void) = ^{ DuoPanel *s = ws; if (s.padChanged) s.padChanged(); [s changed]; };
    [self grid:@[[self pill:L(@"顯示", @"Show") sub:nil style:hidden ? PILL_PLAIN : PILL_PICKED tap:^{ [ud() removeObjectForKey:@"PadHidden"]; pad(); }],
                 [self pill:L(@"隱藏", @"Hide") sub:nil style:hidden ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() setBool:YES forKey:@"PadHidden"]; pad(); }]] cols:2 tall:56];
    [self note:hidden ? L(@"HOME 和截圖鍵會留著。", @"HOME and Capture stay on screen.")
                      : L(@"接上手把時會自動隱藏，HOME 和截圖鍵會留著。", @"The buttons hide while a controller is connected. HOME and Capture always stay.")];
    if (hidden) return;
    [self heading:L(@"按鍵不透明度", @"Button Opacity")];
    double op = pad_opacity(); NSMutableArray *ps = [NSMutableArray array];
    for (int p = 40; p <= 100; p += 20) [ps addObject:[self pill:[NSString stringWithFormat:@"%d%%", p] sub:nil style:fabs(op * 100 - p) < 10 ? PILL_PICKED : PILL_PLAIN tap:^{ [ud() setDouble:p / 100.0 forKey:@"PadOpacity"]; pad(); }]];
    [self grid:ps cols:4 tall:56];
    [self note:L(@"橫向拿著時，按鍵會疊在遊戲畫面上。", @"When you hold it sideways, the buttons sit over the game.")];
}
- (NSString *)when:(NSDate *)d {
    static NSDateFormatter *f; static dispatch_once_t once;
    dispatch_once(&once, ^{ f = [NSDateFormatter new]; f.locale = [NSLocale localeWithLocaleIdentifier:NSLocale.preferredLanguages.firstObject ?: @"en"]; f.dateStyle = NSDateFormatterMediumStyle; f.timeStyle = NSDateFormatterShortStyle; });
    return [f stringFromDate:d];
}
- (NSString *)backupTitle:(NSString *)name { for (NSDictionary *b in duo_backups()) if ([b[@"name"] isEqualToString:name]) return [self when:b[@"date"]]; return name; }
- (void)pageSave {
    __weak DuoPanel *ws = self;
    if ([_confirm isEqualToString:@"fresh"]) {
        [self dialog:L(@"要從頭開始嗎？", @"Start over?") band:L(@"目前的存檔會移到備份", @"Your current save moves to Backups")
                note:L(@"不會刪除。下次開啟遊戲時從頭開始。", @"Nothing is deleted. The game starts from the beginning the next time you open it.")
                 yes:L(@"從頭開始", @"Start Over") action:^{ DuoPanel *s = ws; s->_confirm = nil; duo_save_set_pending(@"fresh"); [s changed]; }];
        return;
    }
    if ([_confirm hasPrefix:@"restore:"]) {
        NSString *name = [_confirm substringFromIndex:8];
        [self dialog:L(@"要還原這份備份嗎？", @"Restore this backup?") band:[self backupTitle:name]
                note:L(@"目前的存檔會先移到備份，下次開啟遊戲時換成這份。", @"Your current save moves to Backups first, and this backup loads the next time you open the game.")
                 yes:L(@"還原", @"Restore") action:^{ DuoPanel *s = ws; s->_confirm = nil; duo_save_set_pending([@"restore:" stringByAppendingString:name]); [s changed]; }];
        return;
    }
    if (_failed) [self warn:[_failed isEqualToString:@"restore"] ? L(@"上次的還原沒有完成，存檔沒有變。", @"The last restore didn't finish, so your save didn't change.")
                                                                  : L(@"上次的從頭開始沒有完成，存檔沒有變。", @"Starting over didn't finish last time, so your save didn't change.")];
    NSString *pend = duo_save_pending();
    if (pend) {
        NSString *what = [pend hasPrefix:@"restore:"] ? [NSString stringWithFormat:L(@"還原 %@ 的備份", @"restore the backup from %@"), [self backupTitle:[pend substringFromIndex:8]]] : L(@"從頭開始", @"start over");
        [self heading:L(@"下次開啟遊戲時", @"Next Time You Open the Game")];
        [self warn:[NSString stringWithFormat:L(@"遊戲會%@，目前的存檔會先移到備份。", @"The game will %@. Your current save moves to Backups first."), what]];
        [self grid:@[[self pill:L(@"取消", @"Cancel") sub:nil style:PILL_PLAIN tap:^{ duo_save_set_pending(nil); [ws changed]; }]] cols:2 tall:56];
    }
    [self heading:L(@"備份", @"Backups")];
    [self grid:@[[self pill:L(@"立即備份", @"Back Up Now") sub:nil style:PILL_PLAIN tap:^{ [ws backupNow]; }], [self pill:L(@"匯出到「檔案」", @"Export to Files") sub:nil style:PILL_PLAIN tap:^{ [ws exportSave]; }]] cols:2 tall:56];
    if (_flash) [self note:_flash];
    NSArray *bs = duo_backups();
    [self heading:L(@"還原備份", @"Restore a Backup")];
    if (!bs.count) [self note:L(@"還沒有備份。", @"No backups yet.")];
    NSMutableArray *ps = [NSMutableArray array];
    for (NSUInteger i = 0; i < MIN(bs.count, (NSUInteger)20); i++) {
        NSDictionary *b = bs[i]; NSString *name = b[@"name"], *kind = b[@"kind"];
        NSString *k = [kind isEqualToString:@"restore"] ? L(@"還原前留下的", @"Kept before a restore") : [kind isEqualToString:@"fresh"] ? L(@"從頭開始前留下的", @"Kept before starting over") : L(@"自己備份的", @"Your backup");
        DuoPill *p = [self pill:[self when:b[@"date"]] sub:k style:PILL_PLAIN tap:^{ [ws ask:[@"restore:" stringByAppendingString:name]]; }];
        p.accessibilityHint = L(@"還原這份備份。", @"Restores this backup."); [ps addObject:p];
    }
    if (ps.count) { [self grid:ps cols:2 tall:68]; [self note:L(@"點一份就能還原。備份也可以在「檔案」App 裡找到。", @"Tap one to restore it. You can also find backups in the Files app.")]; }
    [self heading:L(@"從頭開始", @"Start Over")];
    [self grid:@[[self pill:L(@"從頭開始…", @"Start Over…") sub:nil style:PILL_PLAIN tap:^{ [ws ask:@"fresh"]; }]] cols:2 tall:56];
}
- (void)quitSection {
    __weak DuoPanel *ws = self;
    [self heading:L(@"關閉遊戲", @"Close Game")];
    [self warn:L(@"想現在套用，就關閉遊戲再開一次。", @"To apply these changes now, close the game and open it again.")];
    [self grid:@[[self pill:L(@"關閉遊戲…", @"Close Game…") sub:nil style:PILL_PLAIN tap:^{ [ws ask:@"quit"]; }]] cols:2 tall:56];
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
    if (b.size.width <= 0 || b.size.height <= 0) return;
    // 直拿時不算左右安全區（Duo 外螢幕直拿時右邊整條都算安全區）；內容最寬約 980 點，置中 | upright, the side safe areas are ignored (on the Duo's outer display held upright the whole right strip counts as safe area); contents at most about 980 points wide, centred
    CGFloat side = b.size.width > b.size.height ? MAX(sa.left, sa.right) : 0, m = MAX(20, side + 16);
    CGFloat W = MIN(b.size.width - 2 * m, 980), x0 = round((b.size.width - W) / 2);
    _u = MIN(MAX(MIN(W / 600, b.size.height / 700), 0.8), 1.2);
    _foldA = _foldB = 0;
    if (@available(iOS 27.1, *)) {   // 直的摺痕（iOS 27.1 的分隔保留區，含未啓用的） | a vertical fold (the iOS 27.1 division reserved region, inactive ones included)
        for (UIViewReservedRegion *r in [self reservedRegionsOfKind:UIViewReservedRegionKind.divisionRegionKind options:UIViewReservedRegionQueryOptionsIncludeInactive])
            if (r.frame.size.height >= b.size.height * 0.9 && r.frame.size.width < b.size.width * 0.2) { _foldA = MAX(r.frame.origin.x, CGRectGetMidX(r.frame) - 20) - x0; _foldB = MIN(CGRectGetMaxX(r.frame), CGRectGetMidX(r.frame) + 20) - x0; }
    }
    if (_foldB - _foldA != _laidFold) { _laidFold = _foldB - _foldA; _laidW = 0; }
    CGFloat u = _u, top = MAX(sa.top, 12) + 10 * u;
    // 底部波浪帶 | the bottom wave band
    CGSize band = CGSizeMake(b.size.width, round(MIN(b.size.height * 0.14, 130 * u)));
    if (!CGSizeEqualToSize(band, _bandSize)) { _bandSize = band; _band.image = duo_band_image(band, self.traitCollection.displayScale); }
    _band.frame = CGRectMake(0, b.size.height - band.height, band.width, band.height);
    // 標題列：置中的「設定」、右邊確認橘的「繼續」 | the title row: Settings centred, Resume in confirm orange on the right
    CGFloat d = 4 * u, rh = 50 * u + 2 * d;
    UIFont *rf = duo_font(20 * u, UIFontWeightHeavy); _resumeKey.title.font = rf; _resumeKey.depth = d;
    CGFloat rw = ceil([_resumeKey.title.text sizeWithAttributes:@{NSFontAttributeName: rf}].width) + 64 * u, rx = x0 + W - rw;
    if (@available(iOS 27.1, *)) {   // 相機（遮擋保留區）擋到右上就把「繼續」放左上；兩邊都擋才整列往下 | when a camera (occlusion reserved region) covers the top right, Resume goes top left; only when both sides are covered does the row move down
        NSArray<UIViewReservedRegion *> *occ = [self reservedRegionsOfKind:UIViewReservedRegionKind.occlusionRegionKind];
        BOOL (^hit)(CGRect) = ^BOOL(CGRect r) { for (UIViewReservedRegion *o in occ) if (CGRectIntersectsRect(o.frame, r)) return YES; return NO; };
        if (hit(CGRectMake(rx, top, rw, rh))) {
            if (!hit(CGRectMake(x0, top, rw, rh))) rx = x0;
            else for (UIViewReservedRegion *o in occ) top = MAX(top, ceil(CGRectGetMaxY(o.frame) + 6));
        }
    }
    _resumeKey.frame = CGRectMake(rx, top, rw, rh);
    _title.font = duo_font(28 * u, UIFontWeightHeavy); _title.frame = CGRectMake(x0 + rw, top, W - 2 * rw, 50 * u);
    // 分頁鍵：等寬一排（放不下就兩排） | the page keys: one row of equal widths (two rows when they do not fit)
    NSString *names[DUO_PAGE_N] = {L(@"語言", @"Language"), L(@"畫面", @"Display"), L(@"按鍵", @"Controls"), L(@"存檔", @"Save Data")};
    // 字先縮到 15 點左右也要排成一排；還放不下才兩排 | shrink the text to about 15 points to keep one row; two rows only when it still does not fit
    UIFont *tf = nil; CGFloat gap = 12 * u; int cols = 2;
    for (CGFloat fs = 19; fs >= 15 && cols == 2; fs -= 1) {
        tf = duo_font(fs * u, UIFontWeightBold); CGFloat need = 0;
        for (int i = 0; i < DUO_PAGE_N; i++) need = MAX(need, ceil([names[i] sizeWithAttributes:@{NSFontAttributeName: tf}].width) + 0.9 * 52 * u + 4);
        if (need * DUO_PAGE_N + gap * (DUO_PAGE_N - 1) <= W) cols = DUO_PAGE_N;
    }
    if (cols == 2) tf = duo_font(19 * u, UIFontWeightBold);
    CGFloat ty = top + rh + 12 * u, th = 52 * u + 2 * d;
    for (DuoPill *t in _tabs) {
        BOOL on = t.tag == _page && !_confirm; NSInteger i = t.tag;
        t.style = on ? PILL_PICKED : PILL_PLAIN; t.depth = d; t.title.text = names[i]; t.title.font = tf; t.title.adjustsFontSizeToFitWidth = NO;   // 四個分頁同一個字級 | the four page keys share one text size
        t.accessibilityLabel = names[i]; t.accessibilityTraits = UIAccessibilityTraitButton | (on ? UIAccessibilityTraitSelected : 0);
        CGRect c = [self col:(int)(i % cols) of:cols gap:gap width:W];
        t.frame = CGRectMake(x0 + c.origin.x, ty + (i / cols) * (th + gap * 0.7), c.size.width, th);
    }
    CGFloat headH = ty + ((DUO_PAGE_N + cols - 1) / cols) * (th + gap * 0.7) + 10 * u;
    _head.frame = CGRectMake(0, 0, b.size.width, headH);
    // 分頁鍵下一道橘色虛線，同遊戲島名頁的「或者」 | an orange dashed rule under the page keys, like the "or" line on the game's island-name page
    UIBezierPath *rp = [UIBezierPath bezierPath]; [rp moveToPoint:CGPointMake(x0, headH - 2 * u)]; [rp addLineToPoint:CGPointMake(x0 + W, headH - 2 * u)];
    _rule.path = rp.CGPath; _rule.lineWidth = 2.5 * u; _rule.lineDashPattern = @[@(7 * u), @(7 * u)];
    _scroll.frame = CGRectMake(0, headH, b.size.width, b.size.height - headH);
    _bodyH = b.size.height - headH - band.height * 0.5 - 24 * u;
    if (_laidW != W) {   // 內容按寬度重建（換頁、改設定時 _laidW 歸零） | rebuild the contents for this width (_laidW is reset on page and setting changes)
        _laidW = W; _bodyW = W; _y = 16 * u;
        for (UIView *v in _body.subviews) [v removeFromSuperview];
        for (CALayer *l in [_body.layer.sublayers copy]) if ([l isKindOfClass:CAGradientLayer.class]) [l removeFromSuperlayer];
        BOOL dialog = _confirm != nil;
        if (_reverted.count && !dialog) {   // 開機保護改回了設定：最上面說一次 | the safe-boot guard put settings back: say so at the top
            NSMutableArray *names2 = [NSMutableArray array]; for (NSString *k in _reverted) [names2 addObject:guard_name(k)];
            [self heading:L(@"設定已改回", @"Settings Put Back")];
            [self warn:[NSString stringWithFormat:L(@"上次沒能開啟遊戲，這些設定已改回之前能開的：%@。", @"The game didn't open last time, so these went back to settings that worked: %@."), [names2 componentsJoinedByString:L(@"、", @", ")]]];
        }
        __weak DuoPanel *ws = self;
        if ([_confirm isEqualToString:@"quit"])
            [self dialog:L(@"要關閉遊戲嗎？", @"Close the game?") band:L(@"改過的設定下次開啟時套用", @"Your changes apply when it opens again")
                    note:L(@"遊戲上次存檔之後的進度會不見。", @"Progress since the game last saved will be lost.")
                     yes:L(@"關閉遊戲", @"Close Game") action:^{ [ud() synchronize]; fprintf(stderr, "duo: 從設定關閉遊戲\n"); duo_game_quit(); (void)ws; }];
        else {
            switch (_page) {
                case DUO_PAGE_LANGUAGE: [self pageLanguage]; break;
                case DUO_PAGE_DISPLAY: [self pageDisplay]; break;
                case DUO_PAGE_CONTROLS: [self pageControls]; break;
                default: [self pageSave]; break;
            }
            if (!_confirm && boot_changed()) [self quitSection];
        }
        _body.frame = CGRectMake(x0, 0, W, _y + 16 * u);
    }
    _scroll.contentSize = CGSizeMake(b.size.width, _body.frame.size.height + band.height * 0.5 + 20 * u);
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (BOOL)accessibilityViewIsModal { return YES; }
@end
