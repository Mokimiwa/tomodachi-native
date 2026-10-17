// Duo 外殼的根控制器：擺上半畫面、摺痕、下半手把，管開場畫面、倒影、打字卡、闔上的喀聲。介面見 duo_shell.h。
//   摺痕：iOS 27.1 的保留區（UIView reservedRegionsOfKind: division，含未啓用的）。攤平時系統報的摺痕可能是零高，所以記住見過的最大高度，攤平和半折用同一套版面。
//     沒有摺痕的螢幕（外螢幕、其他 iPhone）上半按 Duo 形狀（1920×1350）取高，同樣版面等比縮小。橫拿時畫面滿版、按鍵浮在上面（DuoPad.m 的 overlay 排法）。
//   下半疊放順序：黃底 → 底部塗鴉波浪 → 倒影 → 按鍵。倒影是上半畫面底邊的上下翻轉、模糊，起始約 30% 不透明，在下半高度的三分之一內淡出。
//   開場：系統啓動畫面就是黃底加 Logo（UILaunchScreen 的 LaunchLogo），這裡用同一張、同一個位置接手（系統語言有自己的 Logo 時交叉淡入），遊戲第一幀出來後整層淡出。全程只有淡入淡出，不做任何呼吸 / 脈動效果。
// The Duo shell's root view controller: places the top picture, the fold and the controller below, and runs the opening screen, the reflection, the typing card and the click on closing. Interface in duo_shell.h.
//   The fold: the iOS 27.1 reserved region (UIView reservedRegionsOfKind: division, inactive ones included). When flat the system may report the fold with zero height, so the tallest height seen is kept and flat and half-folded share one layout.
//     On screens without a fold (the outer display, other iPhones) the top takes the Duo shape's height (1920×1350) and the same layout scales down. Sideways, the picture fills the screen and the keys float over it (DuoPad.m's overlay arrangement).
//   Lower-half stacking order: yellow → doodle wave at the bottom → reflection → keys. The reflection is the top picture's bottom edge flipped and blurred, starting at about 30% opacity and fading out within a third of the lower half's height.
//   Opening: the system launch screen already shows yellow with the logo (LaunchLogo in UILaunchScreen); this layer takes over with the same picture in the same place (cross-fading to the system language's own logo when there is one), and fades out once the game's first frame is up. Fades only: no breathing or pulsing effect anywhere.
#import <UIKit/UIKit.h>
#import <CoreImage/CoreImage.h>
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include "front.h"
#include "duo_shell.h"

static const CGFloat k_duo_shape = 1920.0 / 1350.0;   // 沒有摺痕時上半的形狀 | the top's shape when there is no fold
static const CGFloat k_refl_alpha = 0.30, k_refl_depth = 1.0 / 3, k_refl_blur = 3.5;   // 倒影：起始不透明度、淡出深度（下半高度的比例）、模糊半徑（點） | reflection: starting opacity, fade depth (fraction of the lower half), blur radius (points)
static const CGFloat k_refl_alpha_live = 0.22;        // 遊戲畫面（不模糊）的倒影起始不透明度 | starting opacity of the game picture's (unblurred) reflection
static const CGFloat k_band_frac = 0.25;              // 底部波浪佔下半高度 | the bottom wave's share of the lower half

// 預覽 App 用：HOME 只開關設定面板（遊戲 App 另給強實現，同時暫停遊戲） | for the preview app: HOME only opens and closes the settings panel (the game app provides a strong version that also pauses the game)
__attribute__((weak)) void duo_home_pressed(void) { static BOOL p; p = !p; fprintf(stderr, "duo: HOME\n"); duo_shell_set_paused(p); }
__attribute__((weak)) void duo_capture_pressed(void) { fprintf(stderr, "duo: 截圖鍵\n"); }

// ---- 點畫面 = 觸屏 | Tapping the picture = the touchscreen ----
// 觸點換成畫面上的歸一化坐標交給宿主（dm_touch_set）；手指槽取最小的空槽，抬起前不變
// Touches become normalised picture coordinates for the host (dm_touch_set); each finger takes the lowest free slot and keeps it until lifted
@interface DuoPictureTouch : UIView
@end
@implementation DuoPictureTouch {
    UITouch *_slot[DM_TOUCH_FRONT_N];
}
- (instancetype)initWithFrame:(CGRect)f { if ((self = [super initWithFrame:f])) self.multipleTouchEnabled = YES; return self; }
- (void)push {
    dm_touch t[DM_TOUCH_FRONT_N]; int n = 0; CGSize s = self.bounds.size;
    if (s.width <= 0 || s.height <= 0) return;
    for (uint32_t i = 0; i < DM_TOUCH_FRONT_N; i++) if (_slot[i]) {
        CGPoint p = [_slot[i] locationInView:self];
        t[n++] = (dm_touch){i, (float)fmin(fmax(p.x / s.width, 0), 0.9999), (float)fmin(fmax(p.y / s.height, 0), 0.9999)};
    }
    dm_touch_set(t, n);
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) for (int i = 0; i < DM_TOUCH_FRONT_N; i++) if (!_slot[i]) { _slot[i] = t; break; }
    [self push];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self push]; }
- (void)lift:(NSSet<UITouch *> *)ts { for (UITouch *t in ts) for (int i = 0; i < DM_TOUCH_FRONT_N; i++) if (_slot[i] == t) _slot[i] = nil; [self push]; }
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self lift:ts]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self lift:ts]; }
@end

// 只轉交給子視圖的容器：自己不收觸控（橫拿時下半層蓋滿全螢幕，按鍵以外的點要穿到畫面上）
// A container that only passes touches to its subviews and never takes them itself (sideways the lower-half layer covers the whole screen, and points outside the keys must reach the picture)
@interface DuoPassView : UIView
@end
@implementation DuoPassView
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e { UIView *v = [super hitTest:p withEvent:e]; return v == self ? nil : v; }
@end

// 畫面容器：圖層是 CAReplicatorLayer。遊戲畫面（Metal）沒法像靜態圖那樣先算好倒影，直拿時讓合成器多畫一份上下翻轉、半透明的副本到摺痕下方，每幀自動跟著遊戲畫面；
// 公開 API 沒有對活圖層的模糊，所以遊戲的倒影是清楚的鏡像（靜態圖仍走 updateReflection 的模糊版）
// The picture container: its layer is a CAReplicatorLayer. The game picture (Metal) cannot have its reflection precomputed like a still, so when upright the compositor draws one more flipped, translucent copy below the fold, following the game picture every frame;
// public API has no blur for a live layer, so the game's reflection is a sharp mirror (stills still take the blurred route in updateReflection)
@interface DuoPictureHost : UIView
@end
@implementation DuoPictureHost
+ (Class)layerClass { return CAReplicatorLayer.class; }
@end

// ---- 根控制器 | The root view controller ----
@interface DuoShellVC : UIViewController
- (instancetype)initWithPicture:(UIView *)picture;
- (void)setStill:(UIImage *)image;
- (void)splashDone;
- (void)typeWithTitle:(NSString *)title sub:(NSString *)sub guide:(NSString *)guide initial:(NSString *)initial max:(int)max done:(void (^)(BOOL, NSString *))done;
@property (nonatomic, readonly) double pictureAspect;
@end
static __weak DuoShellVC *g_shell;
static double g_picture_aspect = 1920.0 / 1350.0;   // 主線程寫、各線程讀 | written on the main thread, read from any

@implementation DuoShellVC {
    UIView *_picture, *_pictureHost, *_panel, *_pad, *_splash; DuoPictureTouch *_touch;
    UIImageView *_band, *_splashBand, *_reflection, *_logo; CAGradientLayer *_reflMask;
    UIImage *_still; CGSize _bandSize, _reflSize, _launchSize; CGRect _top, _fold;   // _launchSize：啓動畫面 Logo 的點大小（沒有 = 0） | the launch screen logo's size in points (none = 0)
    CGFloat _foldH, _foldW; NSString *_foldLog;          // 見過的最大摺痕高 / 寬、上次記日誌的摺痕 | tallest / widest fold seen, the fold last logged
    BOOL _overlay, _foldActive;                          // 橫拿：按鍵浮在畫面上；摺痕啓用中（半折） | sideways: keys float over the picture; the fold is active (half-folded)
    DuoTypeCard *_card; CGFloat _kbTop;                  // 打字卡、鍵盤頂（視圖坐標，沒有鍵盤 = 視圖高） | typing card, keyboard top (view coordinates; no keyboard = view height)
    NSInteger _hinge; SystemSoundID _click;              // 上次的鉸鏈狀態、闔上的聲音（沒有 = 0） | last hinge status, the closing sound (0 = none)
    CIContext *_ci;
    double _contentAspect;                               // 非靜態圖畫面的內容寬高比（遊戲出圖形狀），按它等比置中；0 = 鋪滿 | content aspect of a non-still picture (the game's output shape), centred at it; 0 = fill
    DuoPanel *_settings; BOOL _paused, _readyCalled;      // 設定面板（暫停時開著） | the settings panel (open while paused)
    NSInteger _wantPage;                                 // 等著打開的設定頁（快速動作、-DuoPanel；開場畫面收起後打開），沒有 = -1 | a settings page waiting to open (quick action, -DuoPanel; opened once the opening screen clears), none = -1
    BOOL _ctrl;                                          // 接著實體手把 | a physical controller is connected
}
- (instancetype)initWithPicture:(UIView *)picture { if ((self = [super initWithNibName:nil bundle:nil])) { _picture = picture; _wantPage = -1; } return self; }

- (void)viewDidLoad {
    [super viewDidLoad];
    UIColor *yellow = duo_rgb(DUO_YELLOW);
    self.view.backgroundColor = yellow;
    _pictureHost = [DuoPictureHost new]; _pictureHost.backgroundColor = UIColor.blackColor;
    _pictureHost.clipsToBounds = [_picture isKindOfClass:UIImageView.class];   // 靜態圖按填滿縮放要裁；遊戲畫面等比放得下，不裁，倒影副本才畫得到框外 | a still is scaled to fill and needs clipping; the game picture fits at its own shape, so no clipping, letting the reflection copy draw outside the frame
    [self.view addSubview:_pictureHost];
    if (_picture) [_pictureHost addSubview:_picture];
    _touch = [DuoPictureTouch new]; [_pictureHost addSubview:_touch];
    _panel = [DuoPassView new]; _panel.clipsToBounds = YES; [self.view addSubview:_panel];
    _band = [UIImageView new]; [_panel addSubview:_band];
    _reflection = [UIImageView new]; _reflMask = [CAGradientLayer layer]; _reflection.layer.mask = _reflMask; [_panel addSubview:_reflection];
    _pad = duo_pad_new(); [_panel addSubview:_pad];
    _splash = [UIView new]; _splash.backgroundColor = yellow; [self.view addSubview:_splash];
    _splashBand = [UIImageView new]; [_splash addSubview:_splashBand];
    // 系統啓動畫面已經是黃底加 LaunchLogo（WW 版，Info.plist UILaunchScreen）：開場畫面先用同一張、同一個大小位置接手，看不出換畫面；沒有這張時照舊淡入
    // The system launch screen already shows yellow with LaunchLogo (the WW logo, UILaunchScreen in Info.plist): the opening screen takes over with the same picture at the same size and place, so the handover is invisible; without it, the logo fades in as before
    UIImage *launch = [UIImage imageNamed:@"LaunchLogo"]; _launchSize = launch ? launch.size : CGSizeZero;
    _logo = [[UIImageView alloc] initWithImage:launch ?: [self logoImage]]; _logo.contentMode = UIViewContentModeScaleAspectFit; _logo.alpha = launch ? 1 : 0; [_splash addSubview:_logo];
    _ci = [CIContext contextWithOptions:nil]; _kbTop = CGFLOAT_MAX;
    if ([_picture isKindOfClass:UIImageView.class]) [self setStill:((UIImageView *)_picture).image];
    [self loadClick];
    _wantPage = duo_shell_page_named([NSUserDefaults.standardUserDefaults stringForKey:@"DuoPanel"] ?: @"");   // 啓動參數 -DuoPanel save 等：開場後直接打開那頁 | the launch argument -DuoPanel save etc.: open that page after the opening
    [self applyPadPrefs];
    if (@available(iOS 27.1, *)) {
        __weak DuoShellVC *ws = self;
        [self.view addInteraction:[[UIHingeInteraction alloc] initWithUpdateHandler:^(UIHingeInteraction *i, UIHingeInteractionUpdate *u) { (void)i; [ws hingeChanged:u.hinge]; }]];
    }
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:self selector:@selector(keyboard:) name:UIKeyboardWillChangeFrameNotification object:nil];
    [nc addObserver:self selector:@selector(keyboard:) name:UIKeyboardWillHideNotification object:nil];
}
- (void)viewDidAppear:(BOOL)a {
    [super viewDidAppear:a];
    if (_launchSize.width <= 0) [UIView animateWithDuration:0.35 delay:0.05 options:UIViewAnimationOptionCurveEaseOut animations:^{ self->_logo.alpha = 1; } completion:nil];
    else if (![self.logoName isEqualToString:@"WW"]) {   // 系統語言有自己的 Logo：從啓動畫面那張（WW）交叉淡入 | the system language has its own logo: cross-fade from the launch screen's (WW)
        UIImage *mine = [self logoImage]; UIImageView *lv = _logo;
        [UIView transitionWithView:lv duration:0.3 options:UIViewAnimationOptionTransitionCrossDissolve animations:^{ lv.image = mine; } completion:nil];
    }
    if (!_readyCalled) { _readyCalled = YES; duo_shell_ready(); }   // 第一次版面排好、畫面區形狀已知：遊戲 App 在這裡定出圖形狀並起宿主 | the first layout is done and the picture area's shape is known: the game app settles the output shape and starts the host here
}
- (void)viewWillAppear:(BOOL)a { [super viewWillAppear:a]; self.view.window.backgroundColor = duo_rgb(DUO_YELLOW); }   // 換螢幕時露出的空檔是黃的，不是黑的 | any gap while switching displays shows yellow, not black
// 換螢幕（Duo 闔上 / 打開）或轉向時不播系統的旋轉動畫：闔到外螢幕橫拿時，系統會先按上一塊螢幕的方向畫、再轉 180° 到位，看起來是倒的。直接換成新方向的版面
// No system rotation animation when the display changes (the Duo closing / opening) or the device turns: closing to the outer display held sideways, the system first draws in the previous display's orientation and then turns 180° into place, which looks upside down. The new layout appears directly instead
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)c {
    [super viewWillTransitionToSize:size withTransitionCoordinator:c];
    UIWindowScene *ws = self.view.window.windowScene;
    fprintf(stderr, "duo: 版面換成 %.0f×%.0f（介面方向 %ld）\n", size.width, size.height, ws ? (long)ws.effectiveGeometry.interfaceOrientation : -1L);
    [UIView setAnimationsEnabled:NO];
    [c animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> x) { (void)x; [UIView setAnimationsEnabled:YES]; }];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }   // 邊緣手勢要滑兩次，免得誤觸退出 | edge gestures need two swipes, to avoid accidental exits
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAllButUpsideDown; }   // 外螢幕直拿、橫拿都行（橫拿同內螢幕：畫面滿版、按鍵浮在上面）；內螢幕本來就不理會這個 | the outer display works upright and sideways (sideways as on the inner display: full-screen picture, floating keys); the inner display ignores this anyway

// Logo：按系統語言挑遊戲裡對應的那張，沒有就用 logo.png（遊戲圖標） | Logo: the game's version for the system language, else logo.png (the game icon)
- (NSString *)logoName {
    NSString *l = NSLocale.preferredLanguages.firstObject ?: @"en", *pick = @"WW";
    if ([l hasPrefix:@"zh"]) pick = ([l containsString:@"Hant"] || [l hasSuffix:@"TW"] || [l hasSuffix:@"HK"] || [l hasSuffix:@"MO"]) ? @"TWzh" : @"CHzh";
    else if ([l hasPrefix:@"ja"]) pick = @"JPja";
    else if ([l hasPrefix:@"ko"]) pick = @"KRko";
    else for (NSString *eu in @[@"de", @"es", @"fr", @"it", @"nl"]) if ([l hasPrefix:eu]) pick = [@"EU" stringByAppendingString:eu];
    NSString *forced = [NSUserDefaults.standardUserDefaults stringForKey:@"DuoLogo"];   // 啓動參數 -DuoLogo TWzh 可指定 | the launch argument -DuoLogo TWzh picks one
    return forced ?: pick;
}
- (UIImage *)logoImage { return duo_asset([@"logo_" stringByAppendingString:self.logoName]) ?: duo_asset(@"logo_WW") ?: duo_asset(@"logo"); }

// ---- 版面 | Layout ----
// 直拿（高 > 寬）：上半畫面、摺痕、下半手把。橫拿：畫面滿版，按鍵浮在上面（同一般 iPhone 橫拿），中間的鍵避開直的摺痕
// Upright (taller than wide): picture on top, the fold, the controller below. Sideways: the picture fills the screen with the keys floating over it (as on any iPhone held sideways), the middle keys kept off the vertical fold
- (CGRect)foldIn:(CGRect)b {   // 橫跨整寬或整高的摺痕；沒有 = CGRectNull | a fold spanning the full width or height; none = CGRectNull
    CGRect fold = CGRectNull; NSString *log = @"沒有摺痕"; _foldActive = NO;
    if (@available(iOS 27.1, *)) {
        NSArray<UIViewReservedRegion *> *rs = [self.view reservedRegionsOfKind:UIViewReservedRegionKind.divisionRegionKind options:UIViewReservedRegionQueryOptionsIncludeInactive];
        NSMutableString *m = [NSMutableString string];
        for (UIViewReservedRegion *r in rs) {
            [m appendFormat:@"%@%@ 邊距 %@；", NSStringFromCGRect(r.frame), r.active ? @" 啓用" : @"", NSStringFromUIEdgeInsets(r.margins)];
            if ((r.frame.size.width >= b.size.width * 0.9 || r.frame.size.height >= b.size.height * 0.9) && CGRectContainsRect(CGRectInset(b, -1, -1), r.frame)) { fold = r.frame; _foldActive = r.active; }   // 只認貫穿整個畫面、整條在畫面裡的（剛換到外螢幕的那一次版面，系統還報內螢幕的摺痕，位置在畫面外） | only one that crosses the whole screen and lies inside it (on the first layout after moving to the outer display the system still reports the inner display's fold, partly off screen)
        }
        if (rs.count) log = m;
        for (UIViewReservedRegion *r in [self.view reservedRegionsOfKind:UIViewReservedRegionKind.occlusionRegionKind])   // 相機等遮擋區，只記日誌 | occlusions such as cameras, logged only
            log = [log stringByAppendingFormat:@" 遮擋 %@", NSStringFromCGRect(r.frame)];
    }
    if (![log isEqualToString:_foldLog]) { _foldLog = log; fprintf(stderr, "duo: 畫面 %.0f×%.0f，摺痕 %s\n", b.size.width, b.size.height, log.UTF8String); }
    if (CGRectIsNull(fold)) return fold;
    // 攤平時系統可能報零寬 / 零高：沿用半折時見過的大小，兩種姿勢一套版面 | flat, the system may report zero width / height: keep the size seen half-folded, one layout for both
    if (fold.size.width >= fold.size.height) { _foldH = fmax(_foldH, fold.size.height); return CGRectMake(CGRectGetMinX(fold), CGRectGetMidY(fold) - _foldH / 2, fold.size.width, _foldH); }
    _foldW = fmax(_foldW, fold.size.width); return CGRectMake(CGRectGetMidX(fold) - _foldW / 2, CGRectGetMinY(fold), _foldW, fold.size.height);
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect b = self.view.bounds; if (b.size.width <= 0 || b.size.height <= 0) return;
    CGRect fold = [self foldIn:b];
    BOOL across = !CGRectIsNull(fold) && fold.size.width >= fold.size.height;   // 橫的摺痕 | a horizontal fold
    _overlay = b.size.width > b.size.height;
    CGFloat topH, panelY;
    if (_overlay) { topH = panelY = b.size.height; }
    else if (across) { topH = CGRectGetMinY(fold); panelY = CGRectGetMaxY(fold); }
    else { topH = round(fmin(b.size.width / k_duo_shape, b.size.height * 0.55)); panelY = topH; }
    _top = CGRectMake(0, 0, b.size.width, topH); _fold = fold;
    g_picture_aspect = topH > 0 ? b.size.width / topH : k_duo_shape;
    _pictureHost.frame = _top; _picture.frame = [self pictureFrameIn:_pictureHost.bounds]; _touch.frame = _picture.frame;
    _settings.frame = b;
    _panel.frame = _overlay ? b : CGRectMake(0, panelY, b.size.width, b.size.height - panelY);
    _pad.frame = _panel.bounds;
    duo_pad_arrange(_pad, _overlay, _overlay && !across && !CGRectIsNull(fold) ? [_pad convertRect:fold fromView:self.view] : CGRectNull);
    _band.hidden = _reflection.hidden = _overlay;
    CGFloat bh = round(_overlay ? b.size.height * 0.125 : _panel.bounds.size.height * k_band_frac);   // 橫拿時波浪只在開場畫面 | sideways the wave appears only on the opening screen
    CGSize band = CGSizeMake(b.size.width, bh);
    if (!CGSizeEqualToSize(band, _bandSize)) { _bandSize = band; _band.image = _splashBand.image = [self bandImage:band]; }
    _band.frame = CGRectMake(0, _panel.bounds.size.height - band.height, band.width, band.height);
    _splash.frame = b;
    _splashBand.frame = CGRectMake(0, b.size.height - band.height, band.width, band.height);
    // Logo 在螢幕正中；半折時（摺痕啓用）改在上半正中，不讓它摺在摺痕上 | the logo sits in the middle of the screen; half-folded (fold active) it moves to the middle of the top half, off the crease
    CGRect la = _foldActive && across ? _top : b;
    CGSize ls = _logo.image.size; CGFloat lw = fmin(b.size.width * 0.62, fmin(la.size.height, b.size.height * 0.5) * 0.80 * (ls.height > 0 ? ls.width / ls.height : 1.4));
    CGFloat lh = ls.width > 0 ? lw * ls.height / ls.width : lw;
    if (_launchSize.width > 0) { lw = _launchSize.width; lh = _launchSize.height; }   // 同啓動畫面的大小（啓動畫面把圖放在整個螢幕正中） | the launch screen's size (it centres the picture on the whole screen)
    _logo.frame = CGRectMake(round(CGRectGetMidX(la) - lw / 2), round(CGRectGetMidY(la) - lh / 2), lw, lh);
    if (!_overlay) [self updateReflection];
    [self updateLiveReflection:panelY];
    [self placeCard];
}

// 底部波浪：遊戲的波浪形狀填淺黃，裡面的塗鴉線用稍深的黃（同島名頁）；沒有遊戲檔案的圖時只留黃底
// The bottom wave: the game's wave shape filled in light yellow, its doodle lines in a slightly deeper yellow (as on the island-name page); without the game's pictures only the yellow remains
static CGImageRef grey_mask(UIImage *img, CGSize px, CGFloat tileH, CGFloat tileW, CGFloat x0) {   // 平鋪成一張 DeviceGray 遮罩（Quartz 坐標，向上為正） | tile into one DeviceGray mask (Quartz coordinates, y up)
    CGColorSpaceRef g = CGColorSpaceCreateDeviceGray();
    CGContextRef c = CGBitmapContextCreate(NULL, (size_t)px.width, (size_t)px.height, 8, 0, g, (CGBitmapInfo)kCGImageAlphaNone);
    CGColorSpaceRelease(g);
    if (!c) return NULL;
    for (CGFloat y = px.height - tileH; y > -tileH; y -= tileH)
        for (CGFloat x = x0 - ceil(x0 / tileW) * tileW; x < px.width; x += tileW) CGContextDrawImage(c, CGRectMake(x, y, tileW, tileH), img.CGImage);
    CGImageRef m = CGBitmapContextCreateImage(c); CGContextRelease(c); return m;
}
UIImage *duo_band_image(CGSize size, CGFloat s) {   // 介面在 duo_shell.h；設定面板也用 | interface in duo_shell.h; the settings panel uses it too
    UIImage *wave = duo_asset(@"wave"), *doodle = duo_asset(@"doodle");
    if (!wave || size.width <= 0 || size.height <= 0) return nil;
    CGSize px = CGSizeMake(round(size.width * s), round(size.height * s));
    CGFloat ww = wave.size.width * px.height / wave.size.height;   // 波浪貼圖的高 = 帶高 | the wave texture's height = the band's height
    CGImageRef wm = grey_mask(wave, px, px.height, ww, (px.width - ww) / 2);
    CGImageRef dm = doodle ? grey_mask(doodle, px, px.height * 3.5, px.height * 3.5, 0) : NULL;   // 塗鴉圖案約帶高的 3.5 倍一格 | one doodle tile is about 3.5 times the band's height
    UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat]; f.scale = s; f.opaque = NO;
    UIImage *out = [[[UIGraphicsImageRenderer alloc] initWithSize:size format:f] imageWithActions:^(UIGraphicsImageRendererContext *rc) {
        CGContextRef c = rc.CGContext; CGRect r = CGRectMake(0, 0, size.width, size.height);
        CGContextTranslateCTM(c, 0, size.height); CGContextScaleCTM(c, 1, -1);   // 遮罩是 Quartz 坐標：翻過來再剪 | the masks are in Quartz coordinates: flip before clipping
        if (wm) CGContextClipToMask(c, r, wm);
        CGContextSetFillColorWithColor(c, duo_rgb(DUO_WAVE).CGColor); CGContextFillRect(c, r);
        if (dm) { CGContextClipToMask(c, r, dm); CGContextSetFillColorWithColor(c, duo_rgb(DUO_DOODLE).CGColor); CGContextFillRect(c, r); }
    }];
    CGImageRelease(wm); CGImageRelease(dm);
    return out;
}

- (UIImage *)bandImage:(CGSize)size { return duo_band_image(size, self.traitCollection.displayScale); }

// 倒影：靜態圖時從圖算（遊戲接上後改由呈現時每幀畫，見 ios-plan） | the reflection: computed from the still image (once the game is wired in it is drawn at present time each frame, see ios-plan)
- (void)setStill:(UIImage *)image {
    _still = image; _reflSize = CGSizeZero;
    if ([_picture isKindOfClass:UIImageView.class]) {   // 換圖時交叉淡入（預覽裡模擬遊戲換頁） | cross-fade when the picture changes (the preview's stand-in for the game changing pages)
        UIImageView *iv = (UIImageView *)_picture; iv.contentMode = UIViewContentModeScaleAspectFill;
        if (iv.image && iv.window) [UIView transitionWithView:iv duration:0.3 options:UIViewAnimationOptionTransitionCrossDissolve animations:^{ iv.image = image; } completion:nil];
        else iv.image = image;
    }
    [self.view setNeedsLayout];
}
- (void)updateReflection {
    CGFloat W = _top.size.width, topH = _top.size.height, depth = round(_panel.bounds.size.height * k_refl_depth);
    _reflection.frame = CGRectMake(0, 0, W, depth); _reflMask.frame = _reflection.bounds;
    NSMutableArray *cols = [NSMutableArray array]; NSMutableArray *locs = [NSMutableArray array];
    for (int i = 0; i <= 6; i++) {   // 不透明度 0.30 ×（1 − t）^1.6，前快後慢地淡出 | opacity 0.30 × (1 − t)^1.6, fading fast at first then slowly
        double t = i / 6.0; [locs addObject:@(t)]; [cols addObject:(__bridge id)[UIColor colorWithWhite:0 alpha:k_refl_alpha * pow(1 - t, 1.6)].CGColor];
    }
    _reflMask.colors = cols; _reflMask.locations = locs;
    CGSize want = CGSizeMake(W, depth);
    if (!_still || CGSizeEqualToSize(want, _reflSize) || W <= 0 || topH <= 0 || depth <= 0) { if (!_still) _reflection.image = nil; return; }
    _reflSize = want;
    CGFloat s = self.traitCollection.displayScale, k = fmax(W / _still.size.width, topH / _still.size.height);   // 與畫面一樣按填滿縮放 | scaled to fill, like the picture
    CGRect img = CGRectMake((W - _still.size.width * k) / 2, (topH - _still.size.height * k) / 2, _still.size.width * k, _still.size.height * k);
    UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat]; f.scale = s;
    UIImage *flip = [[[UIGraphicsImageRenderer alloc] initWithSize:want format:f] imageWithActions:^(UIGraphicsImageRendererContext *rc) {
        CGContextTranslateCTM(rc.CGContext, 0, topH); CGContextScaleCTM(rc.CGContext, 1, -1);   // 以畫面底邊為鏡 | mirror about the picture's bottom edge
        [self->_still drawInRect:img];
    }];
    CIImage *ci = [[CIImage imageWithCGImage:flip.CGImage] imageByClampingToExtent];
    ci = [[ci imageByApplyingGaussianBlurWithSigma:k_refl_blur * s] imageByCroppingToRect:CGRectMake(0, 0, want.width * s, want.height * s)];
    CGImageRef cg = [_ci createCGImage:ci fromRect:ci.extent];
    _reflection.image = cg ? [UIImage imageWithCGImage:cg scale:s orientation:UIImageOrientationUp] : nil;
    CGImageRelease(cg);
}

- (CGRect)pictureFrameIn:(CGRect)b {   // 靜態圖鋪滿（填滿縮放）；遊戲畫面按出圖形狀等比置中，多的地方是黑邊 | a still fills (scaled to fill); the game picture is centred at its output shape, with black bars for the rest
    if ([_picture isKindOfClass:UIImageView.class] || _contentAspect <= 0 || b.size.height <= 0) return b;
    CGFloat w = b.size.width, h = b.size.height;
    if (w / h > _contentAspect) w = round(h * _contentAspect); else h = round(w / _contentAspect);
    return CGRectMake(round(CGRectGetMidX(b) - w / 2), round(CGRectGetMidY(b) - h / 2), w, h);
}
- (void)updateLiveReflection:(CGFloat)panelY {   // 遊戲畫面的倒影：複本以畫面底邊為鏡、移到摺痕下方，遮罩只留畫面和摺痕下的淡出段 | the game picture's reflection: the copy mirrored about the picture's bottom edge and moved below the fold, masked to the picture plus the fade below the fold
    CAReplicatorLayer *rl = (CAReplicatorLayer *)_pictureHost.layer;
    static int off = -1; if (off < 0) { const char *e = getenv("DM_DUO_REFLECT"); off = e && !strcmp(e, "0"); }   // DM_DUO_REFLECT=0 關掉遊戲畫面的倒影（真機上有問題時用） | DM_DUO_REFLECT=0 turns the game picture's reflection off (for when it misbehaves on a device)
    BOOL live = _picture && !_overlay && ![_picture isKindOfClass:UIImageView.class] && !off;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    if (!live) { rl.instanceCount = 1; rl.mask = nil; [CATransaction commit]; return; }
    CGFloat W = _top.size.width, H = _top.size.height, depth = round(_panel.bounds.size.height * k_refl_depth), bottom = CGRectGetMaxY(_picture.frame);
    rl.instanceCount = 2;
    rl.instanceAlphaOffset = -(1 - k_refl_alpha_live);   // 第二份 22% 不透明：活圖層沒法模糊，鏡像清楚，比靜態圖的淡一點 | the second copy at 22% opacity: a live layer cannot be blurred, so the sharper mirror is kept fainter than the still's
    // 對著容器中心翻轉後下移：畫面底邊（y = bottom）落在摺痕下緣（panelY） | flip about the container's centre, then move down: the picture's bottom edge (y = bottom) lands on the fold's lower edge (panelY)
    rl.instanceTransform = CATransform3DConcat(CATransform3DMakeScale(1, -1, 1), CATransform3DMakeTranslation(0, panelY + bottom - H, 0));
    CALayer *m = [CALayer layer]; m.frame = CGRectMake(0, 0, W, panelY + depth);
    CALayer *solid = [CALayer layer]; solid.backgroundColor = UIColor.blackColor.CGColor; solid.frame = CGRectMake(0, 0, W, H); [m addSublayer:solid];
    CAGradientLayer *g = [CAGradientLayer layer]; g.frame = CGRectMake(0, panelY, W, depth);
    NSMutableArray *cols = [NSMutableArray array], *locs = [NSMutableArray array];
    for (int i = 0; i <= 6; i++) { double t = i / 6.0; [locs addObject:@(t)]; [cols addObject:(__bridge id)[UIColor colorWithWhite:0 alpha:pow(1 - t, 1.6)].CGColor]; }
    g.colors = cols; g.locations = locs; [m addSublayer:g];
    rl.mask = m;
    [CATransaction commit];
}
- (void)setContentAspect:(double)a { _contentAspect = a; [self.view setNeedsLayout]; }

// 暫停（HOME）：打開設定面板（DuoPanel.m），蓋住整個畫面；面板的「繼續」或再按 HOME 收起並繼續
// Paused (HOME): open the settings panel (DuoPanel.m) over the whole screen; the panel's Resume, or HOME again, closes it and resumes
- (void)setPaused:(BOOL)on {
    _paused = on;
    if (on && !_settings) {
        _settings = [[DuoPanel alloc] initWithFrame:self.view.bounds]; _settings.alpha = 0;
        __weak DuoShellVC *ws = self;
        _settings.resume = ^{ [ws resume]; };
        _settings.padChanged = ^{ [ws applyPadPrefs]; };
    }
    DuoPanel *v = _settings;
    if (on) {
        [self.view addSubview:v]; v.frame = self.view.bounds;
        if (_wantPage >= 0) { v.page = _wantPage; _wantPage = -1; } else [v refresh];
    }
    [UIView animateWithDuration:0.2 animations:^{ v.alpha = on ? 1 : 0; } completion:^(BOOL f) { (void)f; if (!self->_paused) [v removeFromSuperview]; }];
    if (on) UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, v);
}
- (void)showPage:(NSInteger)page {   // 打開設定面板到某一頁（介面見 duo_shell_show_page） | open the settings panel at a page (see duo_shell_show_page)
    if (page < 0 || _card) return;    // 打字時不開 | not while typing
    _wantPage = page;
    if (_splash) return;              // 開場畫面收起後再開 | opened once the opening screen clears
    if (_paused) { _settings.page = page; _wantPage = -1; }
    else duo_home_pressed();          // 遊戲先暫停，暫停時（setPaused:）打開這頁 | pause the game first; setPaused: opens this page
}
// 螢幕按鍵：設定裡藏起來，或接著實體手把，就只留 HOME 與截圖；不透明度只用在浮在畫面上時 | on-screen buttons: hidden in settings or with a physical controller connected, only HOME and capture remain; the opacity applies only while floating over the picture
- (void)applyPadPrefs {
    NSUserDefaults *u = NSUserDefaults.standardUserDefaults;
    duo_pad_set_hidden(_pad, _ctrl || [u boolForKey:@"PadHidden"]);
    duo_pad_set_opacity(_pad, [u objectForKey:@"PadOpacity"] ? [u doubleForKey:@"PadOpacity"] : 0.6);
}
- (void)resume { duo_home_pressed(); }
- (void)resetInput { duo_pad_reset(_pad); }
- (void)setPadHidden:(BOOL)h { _ctrl = h; [self applyPadPrefs]; }

// ---- 開場 | Opening ----
- (void)splashDone {
    if (!_splash) return;
    UIView *sp = _splash; _splash = nil;
    [UIView animateWithDuration:0.5 delay:0 options:UIViewAnimationOptionCurveEaseInOut animations:^{ sp.alpha = 0; } completion:^(BOOL f) { (void)f; [sp removeFromSuperview]; }];
    if (_wantPage < 0) _wantPage = duo_boot_guard_page();   // 開機保護改回了設定：打開那頁說明 | the safe-boot guard put settings back: open that page to say so
    if (_wantPage >= 0) [self showPage:_wantPage];   // 快速動作、-DuoPanel 或開機保護等著的那頁 | the page a quick action, -DuoPanel or the safe-boot guard is waiting for
}

// ---- 打字 | Typing ----
- (void)typeWithTitle:(NSString *)title sub:(NSString *)sub guide:(NSString *)guide initial:(NSString *)initial max:(int)max done:(void (^)(BOOL, NSString *))done {
    if (_card) { done(NO, @""); return; }
    duo_pad_reset(_pad); _pad.userInteractionEnabled = NO;
    __weak DuoShellVC *ws = self;
    _card = [[DuoTypeCard alloc] initWithTitle:title sub:sub guide:guide initial:initial maxChars:max done:^(BOOL ok, NSString *text) {
        DuoShellVC *s = ws; if (!s) { done(ok, text); return; }
        DuoTypeCard *c = s->_card; s->_card = nil; s->_pad.userInteractionEnabled = YES;
        [UIView animateWithDuration:0.22 animations:^{ c.alpha = 0; s->_pad.alpha = 1; } completion:^(BOOL f) { (void)f; [c removeFromSuperview]; }];
        done(ok, text);
    }];
    _card.alpha = 0; [self.view addSubview:_card]; [self placeCard];
    [UIView animateWithDuration:0.22 animations:^{ self->_card.alpha = 1; self->_pad.alpha = 0; }];
    [_card begin];
}
- (void)placeCard {   // 整寬，貼在上半底邊（摺痕上方）；鍵盤更高時改貼在鍵盤上 | full width, on the top half's bottom edge (above the fold); on top of the keyboard when the keyboard reaches higher
    if (!_card) return;
    CGFloat W = self.view.bounds.size.width, h = [_card sizeThatFits:CGSizeMake(W, 0)].height, bottom = fmin(CGRectGetMaxY(_top), _kbTop);
    if (bottom >= self.view.bounds.size.height - 1) h += self.view.safeAreaInsets.bottom;   // 貼在螢幕最底時讓出 home 指示條 | at the very bottom of the screen, leave room for the home indicator
    _card.frame = CGRectMake(0, bottom - h, W, h);
    [_card setNeedsLayout];   // 只挪位置不會重排：相機遮擋要按新位置重算 | moving alone does not relayout: the camera occlusion must be recomputed for the new position
}
- (void)keyboard:(NSNotification *)n {
    UIScreen *screen = self.view.window.screen; if (!screen) return;
    CGRect kb = [n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    kb = [self.view convertRect:kb fromCoordinateSpace:screen.coordinateSpace];
    _kbTop = [n.name isEqualToString:UIKeyboardWillHideNotification] || CGRectGetMinY(kb) >= self.view.bounds.size.height ? CGFLOAT_MAX : CGRectGetMinY(kb);
    NSTimeInterval d = [n.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    [UIView animateWithDuration:d animations:^{ [self placeCard]; }];
}

// ---- 鉸鏈：闔上時的喀聲（只用本機的聲音檔，沒有就不出聲） | Hinge: the click on closing (local sound files only; silent without one) ----
- (void)loadClick {
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *duo = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"duo"];
    for (NSString *dir in @[doc ?: @"", duo]) for (NSString *ext in @[@"caf", @"wav", @"aif", @"m4a", @"mp3"]) {
        NSString *p = [[dir stringByAppendingPathComponent:@"hinge_close"] stringByAppendingPathExtension:ext];
        if (![NSFileManager.defaultManager fileExistsAtPath:p]) continue;
        if (AudioServicesCreateSystemSoundID((__bridge CFURLRef)[NSURL fileURLWithPath:p], &_click) == kAudioServicesNoError) { fprintf(stderr, "duo: 闔上的聲音 %s\n", p.UTF8String); return; }
        fprintf(stderr, "【紅】duo: 闔上的聲音讀不了 %s\n", p.UTF8String); _click = 0;
    }
}
- (void)hingeChanged:(UIHinge *)h API_AVAILABLE(ios(27.1)) {
    NSInteger s = h ? h.status : UIHingeStatusUnknown;
    if (s == _hinge) return;
    fprintf(stderr, "duo: 鉸鏈 %ld → %ld（%.0f°）\n", (long)_hinge, (long)s, h ? h.angle * 180 / M_PI : 0.0);
    if (s == UIHingeStatusClosed && _hinge != UIHingeStatusUnknown && _click) AudioServicesPlaySystemSound(_click);
    _hinge = s;
    [self.view setNeedsLayout];   // 摺痕可能跟著變 | the fold may change with it
}
- (double)pictureAspect { return g_picture_aspect; }
@end

UIViewController *duo_shell_new(UIView *picture) { DuoShellVC *v = [[DuoShellVC alloc] initWithPicture:picture]; g_shell = v; duo_shell_install_shortcuts(); return v; }
void duo_shell_set_still(UIImage *image) { dispatch_async(dispatch_get_main_queue(), ^{ [g_shell setStill:image]; }); }
void duo_shell_splash_done(void) { dispatch_async(dispatch_get_main_queue(), ^{ [g_shell splashDone]; }); }
double duo_shell_picture_aspect(void) { return g_picture_aspect; }
__attribute__((weak)) void duo_shell_ready(void) {}
void duo_shell_set_content_aspect(double a) { dispatch_async(dispatch_get_main_queue(), ^{ [g_shell setContentAspect:a]; }); }
void duo_shell_set_paused(BOOL on) { dispatch_async(dispatch_get_main_queue(), ^{ [g_shell setPaused:on]; }); }
void duo_shell_reset_input(void) { dispatch_async(dispatch_get_main_queue(), ^{ [g_shell resetInput]; }); }
void duo_shell_set_pad_hidden(BOOL hidden) { dispatch_async(dispatch_get_main_queue(), ^{ [g_shell setPadHidden:hidden]; }); }

int duo_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (NSThread.isMainThread || !g_shell) return -1;   // 要在主線程以外等 | must wait off the main thread
    __block int r = 0; __block NSString *text = nil; dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSString *t = title ? @(title) : @"", *s = sub ? @(sub) : @"", *g = guide ? @(guide) : @"", *i = initial ? @(initial) : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        DuoShellVC *v = g_shell;
        if (!v) { r = -1; dispatch_semaphore_signal(done); return; }
        [v typeWithTitle:t sub:s guide:g initial:i max:max_chars done:^(BOOL ok, NSString *x) { r = ok; text = x; dispatch_semaphore_signal(done); }];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (r == 1) snprintf(out, (size_t)out_cap, "%s", text.UTF8String ?: "");
    return r;
}

// ---- 設定頁與主畫面快速動作 | Settings pages and Home Screen quick actions ----
static NSString *const k_page_names[DUO_PAGE_N] = {@"language", @"display", @"controls", @"save"};
int duo_shell_page_named(NSString *name) { for (int i = 0; i < DUO_PAGE_N; i++) if ([name isEqualToString:k_page_names[i]]) return i; return -1; }
void duo_shell_show_page(int page) { dispatch_async(dispatch_get_main_queue(), ^{ [g_shell showPage:page]; }); }
// 長按圖示的選單：每個設定頁一項，點了直接打開那頁（遊戲先暫停）；語言那項的副標是目前選的遊戲語言。快速動作的字跟著系統語言
// The long-press menu: one item per settings page, each opening that page directly (pausing the game first); the language item's subtitle is the chosen game language. Quick-action text follows the system language
void duo_shell_install_shortcuts(void) {
    BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];
    UIApplicationShortcutItem *(^item)(int, NSString *, NSString *, NSString *) = ^(int page, NSString *title, NSString *sub, NSString *icon) {
        return [[UIApplicationShortcutItem alloc] initWithType:k_page_names[page] localizedTitle:title localizedSubtitle:sub icon:[UIApplicationShortcutIcon iconWithSystemImageName:icon] userInfo:nil];
    };
    UIApplication.sharedApplication.shortcutItems = @[
        item(DUO_PAGE_LANGUAGE, zh ? @"遊戲語言" : @"Game Language", duo_lang_name(duo_lang_code()), @"globe"),
        item(DUO_PAGE_SAVE, zh ? @"存檔" : @"Save Data", zh ? @"備份、還原、從頭開始" : @"Back up, restore, start over", @"archivebox"),
        item(DUO_PAGE_DISPLAY, zh ? @"畫面" : @"Display", nil, @"display"),
        item(DUO_PAGE_CONTROLS, zh ? @"螢幕上的按鍵" : @"On-Screen Buttons", nil, @"gamecontroller"),
    ];
}
void duo_shell_shortcut(UIApplicationShortcutItem *item) {
    int page = duo_shell_page_named(item.type);
    fprintf(stderr, "duo: 快速動作 %s\n", item.type.UTF8String);
    if (page >= 0) duo_shell_show_page(page);
}

