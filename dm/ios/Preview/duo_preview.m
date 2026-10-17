// Duo 外殼預覽 App：不帶遊戲，上半放一張靜態圖（包內 duo/picture.png，取自 Mac 版的截圖，只在本機），其餘都是真的外殼（DuoShell.m / DuoPad.m / DuoTypeCard.m）。
//   宿主的輸入口（dm_input_set / dm_touch_set）在這裡只記日誌，用來核對手把送出的鍵。
//   啓動參數（模擬器：xcrun simctl launch <設備> <包名> -鍵 值）：
//     -DuoSplash 秒    開場 Logo 停多久（缺省 1.6）
//     -DuoStill 名     上半換成 duo/picture_<名>.png（如 island）
//     -DuoDemo type    開場後換成島名頁並叫出打字列；-DuoDemoAfter 秒（開場後多久，缺省 0.9）、-DuoDemoTitle、-DuoDemoText、-DuoDemoMax 給時間、標題、預填字、上限
//     -DuoLogo 語言    指定 Logo（TWzh、WW…，見 DuoShell.m）
//     -DuoLive YES     圖當遊戲畫面用：等比置中、倒影由合成器即時複製（遊戲 App 的做法）
//     -DuoPanel 頁     開場後打開設定面板的那一頁（language、display、controls、save）
// Duo shell preview app: no game; the top shows a still image (duo/picture.png in the bundle, a Mac screenshot kept local) and everything else is the real shell (DuoShell.m / DuoPad.m / DuoTypeCard.m).
//   The host's input entry points (dm_input_set / dm_touch_set) only log here, to check the keys the controller sends.
//   Launch arguments (simulator: xcrun simctl launch <device> <bundle ID> -key value):
//     -DuoSplash seconds  how long the opening logo stays (default 1.6)
//     -DuoStill name      put duo/picture_<name>.png on top instead (e.g. island)
//     -DuoDemo type       after the opening, switch to the island-name page and bring up the typing bar; -DuoDemoAfter seconds (after the opening, default 0.9), -DuoDemoTitle, -DuoDemoText, -DuoDemoMax give the timing, title, prefilled text and limit
//     -DuoLogo lang       pick the logo (TWzh, WW..., see DuoShell.m)
//     -DuoLive YES        use the still as the game picture: centred at its shape, reflection copied live by the compositor (the game app's way)
//     -DuoPanel page      open that settings page after the opening (language, display, controls, save)
#import <UIKit/UIKit.h>
#include <stdio.h>
#include "dm_input.h"
#include "front.h"
#include "duo_shell.h"

void dm_input_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    static uint64_t last; static int q[4];   // 搖桿只在跨過四分之一格時記 | sticks are logged only when they cross a quarter step
    int nq[4] = {lx / 8192, ly / 8192, rx / 8192, ry / 8192};
    if (b == last && !memcmp(nq, q, sizeof q)) return;
    last = b; memcpy(q, nq, sizeof q);
    fprintf(stderr, "pad: 鍵 %#06llx 左搖桿 (%d, %d) 右搖桿 (%d, %d)\n", (unsigned long long)b, lx, ly, rx, ry);
}
void dm_touch_set(const dm_touch *t, int n) {
    static int last = -1;
    if (n != last) { last = n; if (n) fprintf(stderr, "touch: %d 指，第一指 (%.3f, %.3f)\n", n, t[0].x, t[0].y); else fprintf(stderr, "touch: 全抬起\n"); }
}

static NSUserDefaults *ud(void) { return NSUserDefaults.standardUserDefaults; }

@interface DuoPreviewApp : UIResponder <UIApplicationDelegate>
@end
@implementation DuoPreviewApp
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    return [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role];
}
@end

@interface DuoPreviewScene : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation DuoPreviewScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)o {
    NSString *still = [ud() stringForKey:@"DuoStill"];
    UIImageView *picture = [[UIImageView alloc] initWithImage:duo_asset(still ? [@"picture_" stringByAppendingString:still] : @"picture")];
    UIView *shown = picture;
    if ([ud() boolForKey:@"DuoLive"]) {   // -DuoLive YES：把圖包進普通視圖，外殼就當它是遊戲畫面（等比置中 + 合成器做的倒影） | -DuoLive YES: wrap the still in a plain view so the shell treats it as the game picture (centred at its shape + the compositor's reflection)
        shown = [UIView new]; picture.frame = shown.bounds; picture.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        picture.contentMode = UIViewContentModeScaleToFill; [shown addSubview:picture];
        CGSize is = picture.image.size; if (is.height > 0) duo_shell_set_content_aspect(is.width / is.height);
    }
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = duo_shell_new(shown);
    [self.window makeKeyAndVisible];
    if (o.shortcutItem) duo_shell_shortcut(o.shortcutItem);
    double hold = [ud() objectForKey:@"DuoSplash"] ? [ud() doubleForKey:@"DuoSplash"] : 1.6;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(hold * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ duo_boot_guard_ok(); duo_shell_splash_done(); });
    double after = [ud() objectForKey:@"DuoDemoAfter"] ? [ud() doubleForKey:@"DuoDemoAfter"] : 0.9;
    if ([[ud() stringForKey:@"DuoDemo"] isEqualToString:@"type"]) [self demoType:hold + after];
}
- (void)windowScene:(UIWindowScene *)ws performActionForShortcutItem:(UIApplicationShortcutItem *)item completionHandler:(void (^)(BOOL))done { duo_shell_shortcut(item); done(YES); }
- (void)demoType:(double)after {   // 示範：遊戲要島名時的樣子 | demo: what it looks like when the game asks for the island name
    BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];
    NSString *title = [ud() stringForKey:@"DuoDemoTitle"] ?: (zh ? @"島嶼名稱" : @"Island name"), *text = [ud() stringForKey:@"DuoDemoText"] ?: @"";
    int max = [ud() objectForKey:@"DuoDemoMax"] ? (int)[ud() integerForKey:@"DuoDemoMax"] : 10;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((after - 0.6) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ duo_shell_set_still(duo_asset(@"picture_type")); });   // 遊戲先換到輸入頁，再要文字 | the game switches to its input page first, then asks for text
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(after * NSEC_PER_SEC)), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char out[256];
        int r = duo_text_input(title.UTF8String, "", "", text.UTF8String, max, out, sizeof out);
        fprintf(stderr, "type: %s「%s」\n", r == 1 ? "確定" : r == 0 ? "取消" : "沒有外殼", r == 1 ? out : "");
        duo_shell_set_still(duo_asset(@"picture"));
    });
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {   // 日誌寫進 Documents/log.txt（同遊戲 App），模擬器上用 simctl get_app_container 取 | the log goes to Documents/log.txt (as in the game app); on the simulator fetch it with simctl get_app_container
        NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        if (doc && freopen([doc stringByAppendingPathComponent:@"log.txt"].fileSystemRepresentation, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
        duo_boot_guard_begin();     // 同遊戲 App 的開機保護，開場畫面收起時算開好 | the game app's safe boot, counting as up when the opening screen clears
        duo_save_apply_pending();   // 同遊戲 App 開機：設定裡排好的還原 / 從頭開始（這裡的存檔是假的，只看搬動對不對） | as the game app does at boot: Restore / Start Over scheduled in the settings (the save here is a stand-in, only the moves are checked)
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(DuoPreviewApp.class));
    }
}
