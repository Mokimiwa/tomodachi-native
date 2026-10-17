// iOS 前端（front.h 的 iOS 實現）。缺省是 Duo 外殼（DuoShell.m）：直拿上半畫面、下半手把，橫拿畫面滿版、按鍵浮在上面；iPhone、iPhone Duo、iPad 同一套，
// 按畫面區的寬高和摺痕排版，不看設備類型。遊戲出圖形狀取畫面區的形狀（dm_aspect.h 夾在遊戲檔案的上限內），畫面按出圖形狀等比置中。
// DM_FRONT=classic（環境變量或 Documents/env.txt）換回原先的橫向前端：16:9 居中的 Metal 視圖 + 全屏虛擬手柄（DmGamepad.m），真機出問題時對照用。
// 宿主在 main.m 起的大棧線程上跑；front_run 直接在調用線程跑客體。
// iOS front end (the iOS implementation of front.h). The default is the Duo shell (DuoShell.m): upright, the picture on top and the controller below; sideways, a full-screen picture with floating keys; one layout for iPhone, iPhone Duo and iPad,
// laid out from the picture area's size and the fold, never from the device type. The game's output shape is the picture area's shape (clamped to the game profile's caps by dm_aspect.h), and the picture is centred at that shape.
// DM_FRONT=classic (environment variable or Documents/env.txt) brings back the previous landscape front end: a Metal view centred at 16:9 + the full-screen virtual gamepad (DmGamepad.m), for comparison when something goes wrong on a device.
// The host runs on the big-stack thread started by main.m; front_run runs the guest directly on the calling thread.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <AVFAudio/AVFAudio.h>
#include <errno.h>
#include "front.h"
#include "hle.h"
#include "dm.h"
#include "dm_aspect.h"
#include "duo_shell.h"

UIView *dm_gamepad_view(CGRect frame);
void dm_gamepad_reset(void);
void dm_gamepad_watch(void (^changed)(BOOL any));
void front_ios_boot(void);
int front_ios_boot_ok(void);
uint64_t mtl_present_count(void);                       // nvn_mtl.m：實際出圖次數 | nvn_mtl.m: actual present count
void dm_gfx_shot_at(long k, const char *path);          // nvn_mtl.m：k 次上屏後把遊戲畫面存成 PNG | nvn_mtl.m: save the game picture as PNG after k presents
void mtl_set_upscale(int on);                           // nvn_mtl.m：MetalFX 放大開關 | nvn_mtl.m: MetalFX upscaling on / off

void front_ios_audio_session(void);
static CAMetalLayer *g_layer;
static int g_classic;   // 1 = 原先的橫向前端 | 1 = the previous landscape front end
static int g_paused;    // HOME 暫停中（只在主線程讀寫） | paused by HOME (main thread only)

// 畫面寬高比（dm_aspect.h）：環境變量 DM_ASPECT 優先，其次本 App 的設定 GameAspect；都沒給時，外殼 = 畫面區的形狀，原先的前端 = 16:9。「screen」= 畫面區（外殼）或這台設備橫放（原先的前端）的形狀。
// 第一次在主線程問（外殼：第一次版面排好後的 duo_shell_ready；原先的前端：viewDidLoad），都在宿主起跑之前；之後各線程只讀這個定好的值
// Picture aspect ratio (dm_aspect.h): the environment variable DM_ASPECT first, then this app's setting GameAspect; with neither, the shell uses the picture area's shape and the previous front end 16:9. "screen" = the shape of the picture area (shell) or of this device held sideways (previous front end).
// First asked on the main thread (shell: duo_shell_ready after the first layout; previous front end: viewDidLoad), both before the host starts; afterwards every thread only reads the settled value
static double game_aspect(void) {
    static double a; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("DM_ASPECT"); NSString *pref = e ? nil : [NSUserDefaults.standardUserDefaults stringForKey:@"GameAspect"];
        double screen = 0;
        if (!g_classic) screen = duo_shell_picture_aspect();
        else if (NSThread.isMainThread) { CGSize n = UIScreen.mainScreen.nativeBounds.size; screen = fmin(n.width, n.height) > 0 ? fmax(n.width, n.height) / fmin(n.width, n.height) : 0; }
        const char *spec = e ? e : pref ? pref.UTF8String : g_classic ? NULL : "screen";
        a = dm_aspect_parse(spec, screen);
        LOG("前端：畫面寬高比「%s」→ %.4f（%s）", spec ? spec : "16:9", a, g_classic ? "原先的前端" : "Duo 外殼");
    });
    return a;
}

@interface DmGameView : UIView
@end
@implementation DmGameView
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        CAMetalLayer *l = (CAMetalLayer *)self.layer;
        l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.contentsScale = UIScreen.mainScreen.nativeScale; l.framebufferOnly = NO; l.opaque = YES;
        self.backgroundColor = UIColor.blackColor; self.userInteractionEnabled = NO;   // 觸控由外殼的觸屏層收 | touches go to the shell's touchscreen layer
    }
    return self;
}
- (void)didMoveToWindow {   // 按所在螢幕的倍率出圖（Duo 有兩塊螢幕，UIScreen.mainScreen 說不準是哪塊） | render at the scale of the screen it is on (the Duo has two screens, and UIScreen.mainScreen cannot say which)
    [super didMoveToWindow];
    UIScreen *sc = self.window.windowScene.screen; if (sc) { self.layer.contentsScale = sc.nativeScale; [self setNeedsLayout]; }
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    l.drawableSize = CGSizeMake(self.bounds.size.width * l.contentsScale, self.bounds.size.height * l.contentsScale);
}
@end

// 原先的橫向前端（DM_FRONT=classic），以及沒有遊戲文件時的提示 | The previous landscape front end (DM_FRONT=classic), and the hint when there are no game files
@interface DmRootVC : UIViewController
@property (nonatomic, strong) DmGameView *game;
@property (nonatomic, strong) UIView *pad;
@end
@implementation DmRootVC
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    if (!front_ios_boot_ok()) {   // 沒有遊戲文件：給一句提示，不起宿主 | No game files: show a hint and do not start the host
        UILabel *lb = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 40, 40)];
        lb.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        lb.numberOfLines = 0; lb.textAlignment = NSTextAlignmentCenter; lb.textColor = UIColor.whiteColor;
        lb.text = @"Game files not found.\nPut the RomFS folder into this App's Documents as \"romfs\" (Files app), then restart.\n\n沒找到遊戲文件：把 RomFS 文件夾以 romfs 為名放進本 App 的「文稿」後重開。";
        [self.view addSubview:lb]; return;
    }
    game_aspect();   // 在主線程定下畫面寬高比 | settle the picture aspect ratio on the main thread
    self.game = [[DmGameView alloc] initWithFrame:self.view.bounds];
    [self.view addSubview:self.game];
    g_layer = (CAMetalLayer *)self.game.layer;
    self.pad = dm_gamepad_view(self.view.bounds);
    self.pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.pad];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect b = self.view.bounds; if (!self.game) return;
    uint64_t r = front_game_res(1); CGFloat pa = (CGFloat)(uint32_t)r / (uint32_t)(r >> 32);   // 遊戲輸出的形狀等比放進整屏 | Fit the shape of the game's output into the whole screen
    CGFloat w = b.size.width, h = b.size.height;
    if (w / h > pa) w = h * pa; else h = w / pa;
    self.game.frame = CGRectMake(floor((b.size.width - w) / 2), floor((b.size.height - h) / 2), w, h);
}
- (void)viewDidAppear:(BOOL)a { [super viewDidAppear:a]; front_ios_boot(); }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }   // 邊緣手勢要滑兩次，免得誤觸退出 | Edge gestures need two swipes, to avoid accidental exits
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

UIViewController *front_ios_root(void) {
    const char *f = getenv("DM_FRONT"); g_classic = f && !strcmp(f, "classic");
    if (g_classic || !front_ios_boot_ok()) return [DmRootVC new];
    DmGameView *v = [[DmGameView alloc] initWithFrame:CGRectZero];
    g_layer = (CAMetalLayer *)v.layer;
    UIViewController *vc = duo_shell_new(v);
    dm_gamepad_watch(^(BOOL any) { duo_shell_set_pad_hidden(any); });   // 接上實體手把就藏起觸控鍵 | hide the touch keys while a physical controller is connected
    return vc;
}

// 外殼第一次排好版面：定出圖形狀、告訴外殼畫面怎麼擺、起宿主；開場畫面等遊戲真的出圖了才淡出（最多等 90 秒，免得開機失敗時一直蓋著）
// The shell's first layout is done: settle the output shape, tell the shell how to place the picture, start the host; the opening screen fades only once the game actually presents (at most 90 seconds, so a failed boot is not hidden forever)
void duo_shell_ready(void) {
    game_aspect();
    NSUserDefaults *u = NSUserDefaults.standardUserDefaults;
    if ([u objectForKey:@"Upscale"]) { mtl_set_upscale([u boolForKey:@"Upscale"]); LOG("前端：MetalFX 放大%s（設定）", [u boolForKey:@"Upscale"] ? "開" : "關"); }   // 沒設過時照 DM_NVN_UPSCALE（缺省開） | never set: follow DM_NVN_UPSCALE (on by default)
    uint64_t r = front_game_res(1); double w = (uint32_t)r, h = (uint32_t)(r >> 32);
    if (h > 0) duo_shell_set_content_aspect(w / h);
    LOG("前端：Duo 外殼，畫面區 %.4f，出圖 %.0fx%.0f", duo_shell_picture_aspect(), w, h);
    front_ios_boot();
    __block int ticks = 0;
    [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *t) {
        if (mtl_present_count() < 2 && ++ticks < 360) return;
        if (ticks >= 360) LOG("【紅】前端：90 秒還沒出圖，先拿掉開場畫面");
        else duo_boot_guard_ok();   // 畫出畫面了：這組開機設定能開 | the game draws: this set of boot settings works
        [t invalidate]; duo_shell_splash_done();
    }];
}

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
uint64_t front_game_res(int docked) { return dm_aspect_game_res(docked, game_aspect()); }   // 同 Mac：遊戲檔案的 16:9 尺寸，按畫面寬高比改形狀 | Same as Mac: the game profile's 16:9 sizes, reshaped for the picture aspect ratio
void front_run(void (*guest)(void)) { guest(); }   // 已在 main.m 的大棧線程上 | Already on main.m's big-stack thread

// 外殼的觸控手把與實體手把合併後交給宿主（DmGamepad.m 的 duo_pad_publish 是強實現）；HOME = 暫停並打開設定面板 / 收起並繼續（同 Mac 的暫停）；截圖 = 遊戲畫面原解析度存進 Documents/Screenshots（「檔案」App 看得到）
// The shell's touch controller merged with physical controllers goes to the host (DmGamepad.m's duo_pad_publish is the strong version); HOME = pause and open the settings panel / close it and resume (same as the Mac's pause); capture = the game picture at native resolution saved into Documents/Screenshots (visible in the Files app)
void duo_home_pressed(void) {
    g_paused = !g_paused;
    LOG("前端：%s", g_paused ? "暫停" : "繼續");
    if (g_paused) { duo_shell_reset_input(); dm_focus_set(2); dm_audio_suspend(1); guest_clock_freeze_for(3, 1); dm_gfx_suspend(1); }
    else { dm_gfx_suspend(0); guest_clock_freeze_for(3, 0); front_ios_audio_session(); dm_audio_suspend(0); dm_focus_set(1); }   // 暫停中可能進過後台，音頻會話要重設（同回前台） | it may have been in the background while paused, so reset the audio session (as on return to the foreground)
    duo_shell_set_paused(g_paused);
}
void duo_capture_pressed(void) {
    if (g_paused) { LOG("前端：暫停中不截圖（暫停時不出圖）"); return; }   // 同 Mac：截圖在下一次出圖時才拍，暫停中拍不到 | as on the Mac: the shot is taken at the next present, which never comes while paused
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *dir = [doc stringByAppendingPathComponent:@"Screenshots"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *df = [NSDateFormatter new]; df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]; df.dateFormat = @"yyyy-MM-dd HH.mm.ss";
    NSString *base = [NSString stringWithFormat:@"%@ %@", NSBundle.mainBundle.infoDictionary[@"CFBundleDisplayName"] ?: @"Screenshot", [df stringFromDate:NSDate.date]];
    NSString *path = [dir stringByAppendingPathComponent:[base stringByAppendingString:@".png"]];
    for (int i = 2; [NSFileManager.defaultManager fileExistsAtPath:path]; i++) path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@ %d.png", base, i]];   // 同一秒第二張不蓋掉第一張 | a second shot in the same second does not overwrite the first
    dm_gfx_shot_at(0, path.fileSystemRepresentation);
    LOG("前端：截圖 %s", path.fileSystemRepresentation);
}

// 設定面板接的口（duo_shell.h；弱實現在 DuoPanel.m / DuoSave.m） | hooks for the settings panel (duo_shell.h; weak versions in DuoPanel.m / DuoSave.m)
int duo_game_option_known(const char *name) { return dm_option_known(name); }
void duo_game_set_upscale(int on) { mtl_set_upscale(on); LOG("前端：MetalFX 放大%s", on ? "開" : "關"); }
int duo_game_save_snapshot(const char *dst) { return dm_save_snapshot(dst); }
// 「關閉遊戲」：玩家在設定裡確認過（會丟掉上次存檔之後的進度）；已提交的存檔在盤上是完整的（hle_fs.c 的事務），直接結束 | Close Game: the player confirmed it in the settings (progress since the last save is lost); the committed save on disk is complete (hle_fs.c transactions), so just exit
void duo_game_quit(void) { LOG("前端：玩家從設定關閉遊戲"); fflush(stderr); exit(0); }

// 軟鍵盤輸入框：外殼用打字列（DuoTypeCard.m），原先的前端彈 UIAlertController；主線程顯示，調用線程等結果，等的時候凍住客體時鐘
// Software keyboard: the shell uses its typing bar (DuoTypeCard.m), the previous front end shows a UIAlertController; shown on the main thread while the calling thread waits for the result, with the guest clock frozen meanwhile
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    if (!g_classic) {
        guest_clock_freeze_for(2, 1);
        int r = duo_text_input(title, sub, guide, initial, max_chars, out, out_cap);
        guest_clock_freeze_for(2, 0);
        if (r < 0) LOG("【紅】前端：打字列沒開起來，當作取消");
        return r > 0;
    }
    __block int r = 0; __block NSString *text = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSString *t = title && *title ? @(title) : @"", *m = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
    NSString *init = initial ? @(initial) : @"", *ph = guide ? @(guide) : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *ac = [UIAlertController alertControllerWithTitle:t message:m preferredStyle:UIAlertControllerStyleAlert];
        [ac addTextFieldWithConfigurationHandler:^(UITextField *f) { f.text = init; f.placeholder = ph; }];
        [ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; r = 1; text = ac.textFields.firstObject.text; dispatch_semaphore_signal(done); }]];
        [ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x; dispatch_semaphore_signal(done); }]];
        UIViewController *vc = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow) vc = w.rootViewController;
        if (!vc) { dispatch_semaphore_signal(done); return; }
        [vc presentViewController:ac animated:YES completion:nil];
    });
    guest_clock_freeze_for(2, 1);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    guest_clock_freeze_for(2, 0);
    if (!r) return 0;
    snprintf(out, out_cap, "%s", text.UTF8String ? text.UTF8String : "");
    return 1;
}

// 存檔寫失敗（多半是手機存儲滿了）：主線程彈提示，不擋遊戲（取自 d2r-mobile 的做法）。hle_fs.c 在存檔事務被拒、或掛載時只能改成只讀時調；
// 原先 iOS 前端沒接，只有日誌裡一行，玩家不知道進度沒存上
// Save write failed (most likely the phone's storage is full): show a notice on the main thread without blocking the game (as d2r-mobile does). hle_fs.c calls this when a save transaction is refused or a mount has to fall back to read-only;
// the iOS front end used not to implement it, so there was only a log line and the player never learned that progress was not being saved
void front_save_failed(int err) {
    LOG("前端：存檔寫失敗 errno=%d，提示玩家", err);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *vc = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow) vc = w.rootViewController;
        while (vc.presentedViewController) vc = vc.presentedViewController;
        if (!vc) { LOG("【紅】前端：存檔失敗提示沒有窗口 errno=%d", err); return; }
        NSString *m = err == ENOSPC ? @"存儲空間不足，進度沒有存上。請清理空間後再存檔。\nStorage is full, so progress was not saved. Free up space and save again."
                                    : [NSString stringWithFormat:@"存檔寫入失敗（errno %d），進度沒有存上。\nSaving failed (errno %d); progress was not saved.", err, err];
        UIAlertController *a = [UIAlertController alertControllerWithTitle:@"存檔失敗 / Save failed" message:m preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:a animated:YES completion:nil];
    });
}

// 音頻會話：RemoteIO 每次回調要的幀數跟著 I/O 緩衝時長走，iOS 缺省約 21 ms（48 kHz 下 1024 幀）。遊戲在途只有 4 塊 × 5 ms = 20 ms，
// 每次回調都差 1.3 ms、補零，每秒約 47 次斷續（爆破音；Mac 上把回調改成 1024 幀可復現：2000 次回調欠載 1999 次）。要 5 ms，系統取它支持的最近值；
// 類別仍是缺省的 SoloAmbient（靜音鍵、打斷其他 App 聲音的行為不變）。啓動時設一次，回前台再設（打斷之後系統可能復原）
// Audio session: the frames RemoteIO asks for in each callback follow the I/O buffer duration, about 21 ms by default on iOS (1024 frames at 48 kHz). The game keeps only 4 blocks × 5 ms = 20 ms in flight,
// so every callback came up 1.3 ms short and was zero-filled, about 47 gaps per second (crackling; reproducible on the Mac by forcing 1024-frame callbacks: 1999 underruns in 2000 callbacks). Ask for 5 ms and the system picks the nearest it supports;
// the category stays the default SoloAmbient (silent switch and interrupting other apps' audio behave as before). Set once at boot and again on return to the foreground (the system may reset it after an interruption)
void front_ios_audio_session(void) {
    AVAudioSession *s = AVAudioSession.sharedInstance; NSError *e = nil;
    if (![s setCategory:AVAudioSessionCategorySoloAmbient error:&e]) LOG("【紅】音頻會話類別設不上：%s", e.localizedDescription.UTF8String);
    [s setPreferredSampleRate:48000 error:nil];
    if (![s setPreferredIOBufferDuration:0.005 error:&e]) LOG("【紅】音頻會話 I/O 緩衝時長設不上：%s", e.localizedDescription.UTF8String);
    if (![s setActive:YES error:&e]) LOG("【紅】音頻會話激活失敗：%s", e.localizedDescription.UTF8String);
    LOG("音頻會話：採樣率 %.0f Hz，I/O 緩衝 %.2f ms（要 5 ms）", s.sampleRate, s.IOBufferDuration * 1000);
}

// 場景生命週期（main.m 的 SceneDelegate 調）：0 失活 1 進後台 2 回前台 3 激活。進後台時凍結客體時鐘、等在途 GPU 完成後掛起（後台提交 GPU 會被系統殺）。
// HOME 暫停中時聲音和圖形本來就停著，進出後台只管焦點，回來仍是暫停（同 Mac 縮到 Dock）
// Scene lifecycle (called by SceneDelegate in main.m): 0 resign active, 1 enter background, 2 return to foreground, 3 become active. On entering background, freeze the guest clock and suspend after in-flight GPU work completes (submitting GPU work in background gets the process killed).
// While paused by HOME, sound and graphics are already stopped: going to and from the background only touches focus, and it is still paused on return (as when the Mac app is minimized)
void front_ios_focus_event(int ev) {
    if (!g_layer) return;
    switch (ev) {
        case 0: dm_gamepad_reset(); duo_shell_reset_input(); dm_focus_set(2); break;
        case 1: dm_focus_set(3); if (!g_paused) { dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); } break;
        case 2: if (!g_paused) { dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); front_ios_audio_session(); dm_audio_suspend(0); } dm_focus_set(2); break;
        case 3: if (!g_paused) dm_focus_set(1); break;
    }
}
