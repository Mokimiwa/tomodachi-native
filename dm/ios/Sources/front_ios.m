// iOS 前端（front.h 的 iOS 實現）：16:9 的 Metal 視圖居中（兩側留黑邊）+ 全屏的虛擬手柄層（DmGamepad.m，按鍵疊在畫面上）。
// 宿主在 main.m 起的大棧線程上跑；front_run 直接在調用線程跑客體。
// iOS front end (the iOS implementation of front.h): a centered 16:9 Metal view (black bars on both sides) + a full-screen virtual gamepad layer (DmGamepad.m, controls drawn over the picture).
// The host runs on the big-stack thread started by main.m; front_run runs the guest directly on the calling thread.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <AVFAudio/AVFAudio.h>
#include <errno.h>
#include "front.h"
#include "hle.h"
#include "dm.h"

UIView *dm_gamepad_view(CGRect frame);
void dm_gamepad_reset(void);
void front_ios_boot(void);
int front_ios_boot_ok(void);

static CAMetalLayer *g_layer;

@interface DmGameView : UIView
@end
@implementation DmGameView
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        CAMetalLayer *l = (CAMetalLayer *)self.layer;
        l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.contentsScale = UIScreen.mainScreen.nativeScale; l.framebufferOnly = NO; l.opaque = YES;
        self.backgroundColor = UIColor.blackColor;
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    l.drawableSize = CGSizeMake(self.bounds.size.width * l.contentsScale, self.bounds.size.height * l.contentsScale);
}
@end

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
    CGFloat w = b.size.width, h = b.size.height;   // 16:9 等比放進整屏 | Fit 16:9 into the whole screen
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;
    self.game.frame = CGRectMake(floor((b.size.width - w) / 2), floor((b.size.height - h) / 2), w, h);
}
- (void)viewDidAppear:(BOOL)a { [super viewDidAppear:a]; front_ios_boot(); }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }   // 邊緣手勢要滑兩次，免得誤觸退出 | Edge gestures need two swipes, to avoid accidental exits
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end
UIViewController *front_ios_root(void) { return [DmRootVC new]; }

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
uint64_t front_game_res(int docked) { return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280; }   // 同 Mac：原版 16:9 常數 | Same as Mac: the original 16:9 constants
void front_run(void (*guest)(void)) { guest(); }   // 已在 main.m 的大棧線程上 | Already on main.m's big-stack thread

// 軟鍵盤輸入框：主線程彈 UIAlertController，調用線程等結果 | Software keyboard: show a UIAlertController on the main thread, the calling thread waits for the result
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)max_chars;
    if (!g_layer) return -1;
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

// 場景生命週期（main.m 的 SceneDelegate 調）：0 失活 1 進後台 2 回前台 3 激活。進後台時凍結客體時鐘、等在途 GPU 完成後掛起（後台提交 GPU 會被系統殺）
// Scene lifecycle (called by SceneDelegate in main.m): 0 resign active, 1 enter background, 2 return to foreground, 3 become active. On entering background, freeze the guest clock and suspend after in-flight GPU work completes (submitting GPU work in background gets the process killed)
void front_ios_focus_event(int ev) {
    if (!g_layer) return;
    switch (ev) {
        case 0: dm_gamepad_reset(); dm_focus_set(2); break;
        case 1: dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); break;
        case 2: dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); front_ios_audio_session(); dm_audio_suspend(0); dm_focus_set(2); break;
        case 3: dm_focus_set(1); break;
    }
}
