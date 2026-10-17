// Duo 外殼：iPhone Duo 直拿時的一套版面。上半是遊戲畫面（Duo 形狀，Hor+），摺痕留空，下半是手把（遊戲配色）；另有開場 Logo、倒影、打字卡、闔上時的喀聲。
// 畫面視圖由調用方給：預覽 App 給一張靜態圖（UIImageView），遊戲給 Metal 視圖。外殼只管擺位置、觸控換算和周邊。
// 圖（Logo、波浪、塗鴉）在建置時從玩家自己的遊戲檔案取（tools/duo_assets.py），放在包內 duo/，不進版本庫。
// 手把的按鍵經 dm_input_set（dm_input.h）交給宿主，點畫面經 dm_touch_set（front.h），與遊戲讀的是同一條路。
// Duo shell: the one layout for iPhone Duo held upright. The game picture is on top (Duo shape, Hor+), the fold stays empty, and the controller (in the game's colours) is below; plus the opening logo, the reflection, the typing card and the click on closing.
// The caller supplies the picture view: the preview app gives a still image (UIImageView), the game gives the Metal view. The shell only places it and handles touch mapping and everything around it.
// The pictures (logo, wave, doodles) are taken from the player's own game files at build time (tools/duo_assets.py) into duo/ in the bundle and never enter the repository.
// Controller keys reach the host through dm_input_set (dm_input.h) and taps on the picture through dm_touch_set (front.h): the same path the game reads.
#pragma once
#import <UIKit/UIKit.h>

// 遊戲配色（島名頁取色） | the game's palette (sampled from the island-name page)
enum {
    DUO_YELLOW = 0xFFC100, DUO_ORANGE = 0xFF8000, DUO_NAVY = 0x002240,
    DUO_WAVE = 0xFFD978, DUO_DOODLE = 0xFFD05A,          // 底部波浪、波浪裡的塗鴉 | the bottom wave, the doodles inside it
    DUO_LIP = 0xE5E5E5, DUO_DROP = 0xE7AE00,             // 白鍵的灰色下緣、落在黃底上的深黃影 | a white key's grey lower edge, its darker-yellow shadow on the yellow
    DUO_ORANGE_LIP = 0xD96A00, DUO_FIELD = 0xFFECB3, DUO_GREY = 0x8C8C96,
};
static inline UIColor *duo_rgb(uint32_t c) { return [UIColor colorWithRed:(c >> 16 & 255) / 255.0 green:(c >> 8 & 255) / 255.0 blue:(c & 255) / 255.0 alpha:1]; }
UIFont *duo_font(CGFloat size, UIFontWeight weight);   // 圓體（遊戲字型的近似） | rounded system font (close to the game's typeface)
UIImage *duo_asset(NSString *name);                     // 包內 duo/<name>.png，沒有 = nil | duo/<name>.png in the bundle, nil when missing
UIImage *duo_band_image(CGSize size, CGFloat scale);   // 底部波浪帶（遊戲的波浪 + 塗鴉，取自遊戲檔案；沒有就是 nil） | the bottom wave band (the game's wave + doodles from the game files; nil without them)

// 遊戲那種鍵：白面、灰色下緣、深黃落影；按下時面往下沉、蓋住下緣。字或圖標放在 content 裡，跟著面一起動
// A key in the game's style: white face, grey lower edge, darker-yellow shadow; pressing sinks the face over the edge. Labels or icons go in content and move with the face
@interface DuoKeyLayer : CALayer
@property (nonatomic, strong) UIColor *faceColor, *lipColor, *dropColor;
@property (nonatomic) CGFloat depth;                    // 下緣高度（點）；落影再低一個 depth | lower-edge height (points); the shadow sits one more depth lower
@property (nonatomic) BOOL pressed;
@property (nonatomic, readonly) CALayer *content;
- (void)setShape:(CGPathRef)path;                       // 面的形狀（本層坐標），層會保留它 | the face's shape (in this layer's coordinates); the layer keeps it
- (CATextLayer *)setText:(NSString *)text font:(UIFont *)font color:(UIColor *)color;
- (void)setSymbol:(NSString *)name size:(CGFloat)size color:(UIColor *)color;   // SF Symbol 圖標 | an SF Symbol icon
@end

// 手把（DuoPad.m） | The controller (DuoPad.m)
UIView *duo_pad_new(void);
void duo_pad_reset(UIView *pad);                        // 鬆開所有鍵（失活、打字時） | release every key (on resign active, while typing)
// 排法：overlay = 浮在畫面上（橫拿），否則排在下半；fold = 直的摺痕（本視圖坐標，沒有 = CGRectNull），中間的鍵避開它
// Arrangement: overlay = floating over the picture (sideways), otherwise in the lower half; fold = the vertical fold (in the pad's coordinates, none = CGRectNull), kept clear by the middle keys
void duo_pad_arrange(UIView *pad, BOOL overlay, CGRect fold);
void duo_pad_set_hidden(UIView *pad, BOOL hidden);      // 藏起觸控的遊戲鍵（HOME、截圖留著）：接上實體手把時，或設定裡選了隱藏 | hide the touch game keys (HOME and capture stay): while a physical controller is connected, or when hidden in settings
void duo_pad_set_opacity(UIView *pad, CGFloat opacity); // 浮在畫面上（橫拿）時按鍵的不透明度 | the keys' opacity while they float over the picture (sideways)
// 手把狀態的出口（弱實現 = dm_input_set；遊戲 App 給強實現，與實體手把合併） | where the controller state goes (weak = dm_input_set; the game app gives a strong one that merges physical controllers)
void duo_pad_publish(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

// 外殼（DuoShell.m） | The shell (DuoShell.m)
UIViewController *duo_shell_new(UIView *picture);
void duo_shell_set_still(UIImage *image);               // 預覽：換上半的靜態圖（倒影跟著換） | preview: swap the still picture on top (the reflection follows)
void duo_shell_splash_done(void);                       // 開場畫面淡出（遊戲第一幀出來時調） | fade the opening screen out (call when the game's first frame is up)
double duo_shell_picture_aspect(void);                  // 上半畫面區的寬高比，遊戲按這個形狀出圖 | the top picture area's aspect ratio; the game renders this shape
// 第一次版面排好（畫面區形狀已知）時在主線程調一次。弱實現什麼都不做；遊戲 App 在這裡定出圖形狀、起宿主
// Called once on the main thread after the first layout (the picture area's shape is known). The weak version does nothing; the game app settles the output shape and starts the host here
void duo_shell_ready(void);
void duo_shell_set_content_aspect(double aspect);        // 遊戲出圖的寬高比：畫面按它等比置中、其餘黑邊（靜態圖不用） | the game output's aspect ratio: the picture is centred at it with black bars (not used for stills)
void duo_shell_set_paused(BOOL paused);                 // 暫停時打開設定面板、繼續時收起 | open the settings panel when paused, close it on resume
void duo_shell_reset_input(void);                       // 鬆開手把所有鍵（失活時） | release every controller key (on resign active)
void duo_shell_set_pad_hidden(BOOL hidden);             // 實體手把接上 / 拔掉 | a physical controller connected / removed
// 打字：遊戲要文字時（swkbd）在非主線程調，阻塞到確定 / 取消；語義同 front.h 的 front_text_input
// Typing: called off the main thread when the game asks for text (swkbd), blocks until OK / Cancel; same meaning as front_text_input in front.h
int duo_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);
// HOME 鍵與截圖鍵的去向（弱實現：只記日誌；遊戲接上時另給強實現） | where HOME and capture go (weak: log only; the game build provides strong ones)
void duo_home_pressed(void);
void duo_capture_pressed(void);

// 遊戲樣式的按鈕：DuoKeyLayer 做面，按下即沉（DuoTypeCard.m） | a button in the game's style: a DuoKeyLayer face that sinks while pressed (DuoTypeCard.m)
@interface DuoButton : UIControl
@property (nonatomic, readonly) DuoKeyLayer *key;
@end

// 打字卡（DuoTypeCard.m） | The typing card (DuoTypeCard.m)
@interface DuoTypeCard : UIView
- (instancetype)initWithTitle:(NSString *)title sub:(NSString *)sub guide:(NSString *)guide initial:(NSString *)initial maxChars:(int)maxChars
                         done:(void (^)(BOOL ok, NSString *text))done;
- (void)begin;                                          // 叫出鍵盤 | bring up the keyboard
@end

// 設定面板（DuoPanel.m）：HOME 打開（遊戲暫停），蓋在整個畫面上。頁號見下；開機才讀的設定（語言、畫面形狀、幀率、存檔還原 / 從頭開始）下次開啟遊戲時生效
// The settings panel (DuoPanel.m): opened by HOME (the game pauses), covering the whole screen. Page numbers below; settings read at boot (language, picture shape, frame rate, save restore / start over) take effect the next time the game opens
enum { DUO_PAGE_LANGUAGE, DUO_PAGE_DISPLAY, DUO_PAGE_CONTROLS, DUO_PAGE_SAVE, DUO_PAGE_N };
@interface DuoPanel : UIView
@property (nonatomic, copy) void (^resume)(void);       // 「繼續」 | Resume
@property (nonatomic, copy) void (^padChanged)(void);   // 螢幕按鍵的設定改了 | the on-screen button settings changed
@property (nonatomic) NSInteger page;
- (void)refresh;                                        // 重讀設定、重排（每次打開時） | reread the settings and lay out again (on every opening)
@end
int duo_shell_page_named(NSString *name);               // language / display / controls / save → 頁號；認不得 = -1 | page number; unknown = -1
void duo_shell_show_page(int page);                     // 打開設定面板到某一頁（遊戲沒暫停就先暫停；開場畫面還在就等它收起） | open the settings panel at a page (pausing the game first if needed; waiting for the opening screen to clear)
void duo_shell_install_shortcuts(void);                 // 主畫面上長按圖示的選單（動態快速動作，每頁一項） | the menu from long-pressing the Home Screen icon (dynamic quick actions, one per page)
void duo_shell_shortcut(UIApplicationShortcutItem *item);   // 場景收到快速動作時調 | called when a scene receives a quick action
// 遊戲語言：本 App 設定 GameLanguage > 系統首選語言，對到遊戲的 13 種之一（沒有的用美式英文）；main.m 開機時按它設 DM_LANG
// The game language: this app's GameLanguage setting > the system's preferred language, mapped to one of the game's 13 (US English when it has none); main.m sets DM_LANG from it at boot
NSString *duo_lang_code(void);
NSString *duo_lang_name(NSString *code);                // 語言碼 → 用那個語言寫的名字（English (US)、繁體中文…） | language code -> its name in that language (English (US), 繁體中文...)
// 開機保護（DuoPanel.m）：begin 在宿主起跑前調（main.m），ok 在遊戲畫出畫面後調（front_ios.m）；上次開機沒畫出畫面、開機設定又改過，就改回上次能開的
// Safe boot (DuoPanel.m): begin is called before the host starts (main.m), ok once the game draws (front_ios.m); when the last boot never drew and the boot settings had changed, they go back to the last ones that worked
void duo_boot_guard_begin(void);
void duo_boot_guard_ok(void);
int duo_boot_guard_page(void);                         // 改回過設定時開場後要打開的頁；沒有 = -1 | the page to open after the opening when settings were put back; none = -1

// 遊戲 App 接的口：弱實現在 DuoPanel.m / DuoSave.m（預覽 App 用），front_ios.m 給強實現
// Hooks the game app fills in: weak versions in DuoPanel.m / DuoSave.m (used by the preview app), strong ones in front_ios.m
int duo_game_option_known(const char *name);           // 這個遊戲有沒有某個可選剪口（如 fps60） | whether this game has an optional cut (such as fps60)
void duo_game_set_upscale(int on);                     // MetalFX 放大，即時生效 | MetalFX upscaling, effective immediately
void duo_game_quit(void);                              // 設定裡的「關閉遊戲」 | Close Game in the settings
int duo_game_save_snapshot(const char *dst);           // 存檔已提交版的快照到 dst：0 成功，-1 宿主還沒掛存檔，其他 = errno | snapshot of the committed save to dst: 0 done, -1 the host has not mounted the save, else errno

// 存檔備份（DuoSave.m）：只搬、只拷，不刪 | Save backups (DuoSave.m): moves and copies only, never deletes
NSArray<NSDictionary *> *duo_backups(void);            // 新的在前：name、date（NSDate）、kind（@"" 自己備份的 / @"restore" 還原前留的 / @"fresh" 從頭開始前留的） | newest first: name, date (NSDate), kind (@"" taken by the player / @"restore" kept before a restore / @"fresh" kept before starting over)
NSString *duo_backup_now(int *err);                    // 成功返回備份名；err：ENOENT = 還沒有存檔 | returns the backup's name; err: ENOENT = no save yet
NSURL *duo_save_export_copy(int *err);                 // 匯出用的暫存拷貝 | a temporary copy for exporting
NSString *duo_save_pending(void);                      // 下次開機要做的：nil / @"fresh" / @"restore:<備份名>" | what the next boot does: nil / @"fresh" / @"restore:<backup name>"
void duo_save_set_pending(NSString *action);
void duo_save_apply_pending(void);                     // main.m 開機時、宿主掛存檔之前調 | called by main.m at boot, before the host mounts the save
NSString *duo_save_take_failure(void);                 // 上次開機的待辦沒做成：@"restore" / @"fresh"，取一次就清掉；沒有 = nil | the last boot-time action failed: @"restore" / @"fresh", cleared once taken; none = nil
