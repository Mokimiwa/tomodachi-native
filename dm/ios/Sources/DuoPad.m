// Duo 下半的手把：左大右小兩根可拖的搖桿（輕點一下 = L3 / R3）、十字鍵、ABXY（A 是遊戲的確認橘）、ZL / L、R / ZR、− / +、HOME、截圖。
//   位置照草圖 duo_v4：頂行肩鍵和 − / +，中行左搖桿與菱形，下行十字鍵與右搖桿（同 Pro 手柄），最底 HOME 與截圖。左側量自左緣、右側量自右緣、中間量自中線，
//   大小以 626 × 431 點的下半為 1（草圖的尺寸），兩向取小的那個縮放；行高按下半的高度分配。
//   觸控沿用 DmGamepad.m 的做法：按下時先看鎖定的鍵（肩鍵、− / +、HOME、截圖），再在菱形、十字鍵、兩根搖桿裡取「距離 / 抓取半徑」最小的；菱形裡滑動會換鍵，十字鍵 8 向。
//   搖桿環固定，推量按環心算（小死區，推到環半徑 0.8 倍算滿）。輕點搖桿（不出死區、0.3 秒內鬆手）送一下 L3 / R3。
//   按鍵狀態經 duo_pad_publish 交給宿主（缺省直接 dm_input_set；遊戲 App 換成與實體手把合併的版本）；HOME 與截圖不是 Switch 手柄的位，鬆手時調 duo_home_pressed / duo_capture_pressed。
// The controller in the Duo's lower half: two draggable sticks, large on the left and small on the right (a light tap = L3 / R3), a D-pad, ABXY (A in the game's confirm orange), ZL / L, R / ZR, − / +, HOME and capture.
//   Positions follow the duo_v4 mockup: shoulders and − / + on the top row, the left stick and the diamond in the middle row, the D-pad and the right stick in the lower row (as on a Pro Controller), HOME and capture at the bottom.
//   The left side is measured from the left edge, the right side from the right edge and the middle from the midline; sizes take a 626 × 431 point lower half as 1 (the mockup's size), scaled by the smaller of the two directions; rows are spread over the lower half's height.
//   Touch works as in DmGamepad.m: at touch-down the locked keys come first (shoulders, − / +, HOME, capture), then whichever of the diamond, the D-pad and the two sticks has the smallest distance / grab radius; sliding inside the diamond switches keys, the D-pad has 8 directions.
//   Stick rings stay put and tilt is measured from the ring centre (small dead zone, full at 0.8 of the ring radius). A light tap on a stick (never leaving the dead zone, released within 0.3 s) sends one L3 / R3.
//   Key state goes to the host through duo_pad_publish (dm_input_set by default; the game app swaps in a version merged with physical controllers); HOME and capture are not Switch controller bits and call duo_home_pressed / duo_capture_pressed on release.
#import <UIKit/UIKit.h>
#include "dm_input.h"
#include "duo_shell.h"

// ---- 字型、包內圖、鍵的樣式 | Font, bundled pictures, key style ----
UIFont *duo_font(CGFloat size, UIFontWeight weight) {
    UIFont *f = [UIFont systemFontOfSize:size weight:weight];
    UIFontDescriptor *d = [f.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
    return d ? [UIFont fontWithDescriptor:d size:size] : f;
}
UIImage *duo_asset(NSString *name) {
    NSString *p = [[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"duo"] stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"png"]];
    return [UIImage imageWithContentsOfFile:p];
}

@implementation DuoKeyLayer {
    CAShapeLayer *_drop, *_lip, *_face; CALayer *_content, *_icon; CATextLayer *_text; CGPathRef _path; CGSize _iconSize;
}
- (instancetype)init {
    if ((self = [super init])) {
        _drop = [CAShapeLayer layer]; _lip = [CAShapeLayer layer]; _face = [CAShapeLayer layer]; _content = [CALayer layer];
        [self addSublayer:_drop]; [self addSublayer:_lip]; [self addSublayer:_face]; [_face addSublayer:_content];
        _depth = 3; self.faceColor = UIColor.whiteColor; self.lipColor = duo_rgb(DUO_LIP); self.dropColor = duo_rgb(DUO_DROP);
    }
    return self;
}
- (void)dealloc { CGPathRelease(_path); }
- (CALayer *)content { return _content; }
- (void)setFaceColor:(UIColor *)c { _faceColor = c; _face.fillColor = c.CGColor; }
- (void)setLipColor:(UIColor *)c { _lipColor = c; _lip.fillColor = c.CGColor; }
- (void)setDropColor:(UIColor *)c { _dropColor = c; _drop.fillColor = c.CGColor; }
- (void)setDepth:(CGFloat)d { _depth = d; [self setNeedsLayout]; }
- (void)setShape:(CGPathRef)p { CGPathRetain(p); CGPathRelease(_path); _path = p; [self setNeedsLayout]; }
- (void)setContentsScale:(CGFloat)s { [super setContentsScale:s]; for (CALayer *l in @[_drop, _lip, _face, _content]) l.contentsScale = s; _text.contentsScale = s; }
- (void)layoutSublayers {
    [super layoutSublayers];
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (CAShapeLayer *l in @[_drop, _lip, _face]) { l.transform = CATransform3DIdentity; l.frame = self.bounds; l.path = _path; }
    _lip.transform = CATransform3DMakeTranslation(0, _depth, 0);
    _drop.transform = CATransform3DMakeTranslation(0, 2 * _depth, 0);
    _face.transform = CATransform3DMakeTranslation(0, _pressed ? _depth : 0, 0);
    _content.frame = self.bounds;
    if (_text) { CGFloat h = ((UIFont *)_text.font).lineHeight; _text.frame = CGRectMake(0, (self.bounds.size.height - h) / 2, self.bounds.size.width, h); }
    if (_icon) _icon.frame = CGRectMake((self.bounds.size.width - _iconSize.width) / 2, (self.bounds.size.height - _iconSize.height) / 2, _iconSize.width, _iconSize.height);
    [CATransaction commit];
}
- (void)setPressed:(BOOL)p {   // 按下即時沉下去，鬆開 0.08 秒彈回 | sink at once on press, spring back over 0.08 s on release
    if (p == _pressed) return;
    _pressed = p;
    [CATransaction begin]; [CATransaction setDisableActions:p]; [CATransaction setAnimationDuration:0.08];
    _face.transform = CATransform3DMakeTranslation(0, p ? _depth : 0, 0);
    [CATransaction commit];
}
- (CATextLayer *)setText:(NSString *)text font:(UIFont *)font color:(UIColor *)color {
    if (!_text) { _text = [CATextLayer layer]; _text.alignmentMode = kCAAlignmentCenter; _text.contentsScale = self.contentsScale; [_content addSublayer:_text]; }
    _text.string = text; _text.font = (__bridge CFTypeRef)font; _text.fontSize = font.pointSize; _text.foregroundColor = color.CGColor;
    [self setNeedsLayout];
    return _text;
}
- (void)setSymbol:(NSString *)name size:(CGFloat)size color:(UIColor *)color {   // SF Symbol 圖標，按當前縮放畫好放進 content | an SF Symbol icon, rendered at the current scale into content
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:size weight:UIImageSymbolWeightBold];
    UIImage *img = [[UIImage systemImageNamed:name withConfiguration:cfg] imageWithTintColor:color renderingMode:UIImageRenderingModeAlwaysOriginal];
    if (!img) return;
    UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat]; f.scale = self.contentsScale > 0 ? self.contentsScale : 3;
    if (!_icon) { _icon = [CALayer layer]; [_content addSublayer:_icon]; }
    _icon.contents = (__bridge id)[[[UIGraphicsImageRenderer alloc] initWithSize:img.size format:f] imageWithActions:^(UIGraphicsImageRendererContext *c) { (void)c; [img drawAtPoint:CGPointZero]; }].CGImage;
    _iconSize = img.size; [self setNeedsLayout];
}
@end

// ---- 手把 | The controller ----
__attribute__((weak)) void duo_pad_publish(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { dm_input_set(b, lx, ly, rx, ry); }
enum { K_A, K_B, K_X, K_Y, K_ZL, K_L, K_R, K_ZR, K_MINUS, K_PLUS, K_HOME, K_CAP, K_UP, K_RIGHT, K_DOWN, K_LEFT, K_N };
static const uint64_t k_bit[K_N] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_ZL, DM_BTN_L, DM_BTN_R, DM_BTN_ZR, DM_BTN_MINUS, DM_BTN_PLUS, 0, 0,
                                    DM_BTN_UP, DM_BTN_RIGHT, DM_BTN_DOWN, DM_BTN_LEFT};
static NSString *const k_name[K_N] = {@"A", @"B", @"X", @"Y", @"ZL", @"L", @"R", @"ZR", @"−", @"+"};
enum { C_NONE = 0, C_BTN, C_FACE, C_DPAD, C_STICK };   // 觸點佔用的控件 | which control a touch holds

static const CGFloat   // 草圖單位（626 × 431 的下半） | mockup units (a 626 × 431 lower half)
    K_REF_W = 626, K_REF_H = 431,
    K_ROW_Y = 49, K_SH_H = 38,                       // 頂行中心離下半頂、肩鍵高 | top row centre from the half's top, shoulder height
    K_ZL_X = 53, K_ZL_W = 70, K_L_X = 129, K_L_W = 62,   // 中心離外側、寬 | centre from the outer edge, width
    K_PM_DX = 34, K_PM_R = 17,                       // − / +：中心離中線、半徑 | − / +: centre from the midline, radius
    K_MID_FY = 0.40, K_LOW_FY = 0.74,                // 中行、下行中心（下半高度的比例） | middle and lower row centres (fractions of the half's height)
    K_STICK_X = 112, K_STICK_R = 72, K_KNOB_R = 30,  // 左搖桿 | left stick
    K_FACE_X = 112, K_FACE_GAP = 40, K_FACE_R = 23,  // 菱形：中心離右緣、鍵離中心、鍵半徑 | diamond: centre from the right edge, key distance from the centre, key radius
    K_DPAD_X = 238, K_DPAD_K = 30,                   // 十字鍵：中心離左緣、每臂的格距與大小 | D-pad: centre from the left edge, each arm's spacing and size
    K_RSTICK_X = 238, K_RSTICK_R = 52, K_RKNOB_R = 24,   // 右搖桿：中心離右緣 | right stick: centre from the right edge
    K_SYS_DX = 30, K_SYS_DY = 34, K_SYS_R = 16,      // HOME / 截圖：中心離中線、離底 | HOME / capture: centre from the midline and from the bottom
    K_DEPTH = 3,                                     // 鍵的下緣高 | a key's lower-edge height
    K_STICK_GRAB = 1.35, K_DPAD_GRAB = 1.5, K_FULL = 0.8, K_DEAD = 0.1, K_TAP_S = 0.3;

@interface DuoPad : UIView
@end

@implementation DuoPad {
    NSMapTable<UITouch *, NSNumber *> *_owner;      // 觸點 → 控件（C_BTN 附鍵號、C_STICK 附搖桿號，<< 8） | touch → control (with the key for C_BTN or the stick for C_STICK, << 8)
    NSMapTable<UITouch *, NSNumber *> *_faceOf;     // 菱形裡每個觸點按著哪些鍵 | which keys each touch on the diamond holds
    UITouch *_stick[2]; CGPoint _stickNow[2], _stickP0[2]; CFTimeInterval _stickT0[2]; BOOL _stickMoved[2];   // _stickMoved：離按下點移動過、或不是按在手柄頭上（不算輕點） | _stickMoved: travelled from the touch-down point, or not pressed on the knob (not a tap)
    CGFloat _opacity;                               // 浮在畫面上時鍵的不透明度（設定「不透明度」，缺省 0.6） | key opacity while floating over the picture (the Opacity setting, 0.6 by default)
    BOOL _keysOff;                                  // 接著實體手把：只留 HOME 與截圖 | a physical controller is connected: only HOME and capture remain
    uint64_t _keys, _clicks;                        // 按著的鍵（K_ 位）、輕點送出的 L3 / R3 | keys held (K_ bits), L3 / R3 sent by a tap
    DuoKeyLayer *_key[K_N], *_knob[2];
    CAShapeLayer *_ring[2], *_arrow[4];
    UIImpactFeedbackGenerator *_haptic;
    BOOL _overlay; CGRect _fold;                    // 浮在畫面上、直的摺痕（本視圖坐標，沒有 = CGRectNull） | floating over the picture, the vertical fold (this view's coordinates, none = CGRectNull)
}

- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        self.multipleTouchEnabled = YES; self.opaque = NO; self.backgroundColor = UIColor.clearColor;
        _owner = [NSMapTable strongToStrongObjectsMapTable]; _faceOf = [NSMapTable strongToStrongObjectsMapTable];
        _haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight]; _fold = CGRectNull;
        for (int i = 0; i < 2; i++) {
            _ring[i] = [CAShapeLayer layer];
            _ring[i].fillColor = [UIColor colorWithWhite:1 alpha:0.43].CGColor; _ring[i].strokeColor = [UIColor colorWithWhite:1 alpha:0.9].CGColor;
            [self.layer addSublayer:_ring[i]];
        }
        for (int k = 0; k < K_N; k++) { _key[k] = [DuoKeyLayer layer]; [self.layer addSublayer:_key[k]]; }
        for (int i = 0; i < 2; i++) { _knob[i] = [DuoKeyLayer layer]; [self.layer addSublayer:_knob[i]]; }
        for (int i = 0; i < 4; i++) { _arrow[i] = [CAShapeLayer layer]; _arrow[i].fillColor = [duo_rgb(DUO_NAVY) colorWithAlphaComponent:0.45].CGColor; [_key[K_UP + i].content addSublayer:_arrow[i]]; }
        // HOME：房子圖標；截圖：方鍵上一個圈（同 Switch 的截圖鍵） | HOME: a house icon; capture: a ring on a square key (as on the Switch's capture button)
        CAShapeLayer *ring = [CAShapeLayer layer]; ring.fillColor = nil; ring.strokeColor = duo_rgb(DUO_NAVY).CGColor; [_key[K_CAP].content addSublayer:ring];
    }
    return self;
}

// ---- 佈局 | Layout ----
// 兩種排法：直拿時排在下半（panel，草圖單位、黃底上）；橫拿時浮在畫面上（overlay，同 DmGamepad.m 的 3D 佈局位置，以 430 點高為 1，鍵半透明），
// 中間的鍵（− / +、HOME / 截圖）避開直的摺痕，分在兩邊
// Two arrangements: upright, in the lower half (panel: mockup units, on the yellow); sideways, floating over the picture (overlay: the positions of DmGamepad.m's 3D layout, 430 points tall = 1, keys translucent),
// with the middle keys (− / +, HOME / capture) kept off the vertical fold, one on each side
static const CGFloat
    O_U_MAX = 1.15, O_REF_W = 720,                                // 底行（十字鍵、HOME / 截圖、右搖桿）要約 720 單位寬：外螢幕橫拿較窄，按寬度再縮 | the bottom row (D-pad, HOME / capture, right stick) needs about 720 units: the outer display held sideways is narrower, so also scale by width
    O_ROW_Y = 40, O_PAD_X = 100, O_PAD_Y = 110,                  // 頂行離頂；左搖桿 / 菱形中心離外側、離底 | top row from the top; left stick / diamond centre from the outer edge and the bottom
    O_LOW_X = 240, O_DPAD_Y = 58, O_RSTICK_Y = 62,               // 十字鍵 / 右搖桿：中心離外側、離底 | D-pad / right stick: centre from the outer edge and the bottom
    O_SYS_DY = 32, O_MID_GAP = 14;                // HOME / 截圖離底、中間的鍵離摺痕、浮在畫面上的鍵的不透明度 | HOME / capture from the bottom, middle keys from the fold, opacity of keys over the picture
// 下半只讓開底邊（home 指示條）：外螢幕直拿時右邊整條算安全區（相機和系統列在右上），但下半碰不到那裡，照算會把鍵擠向左邊
// The lower half only keeps clear of the bottom edge (the home indicator): on the outer display held upright the whole right strip counts as safe-area inset (the camera and system bars sit top right), but the lower half never reaches there, and honouring it would push the keys to the left
- (CGRect)area {
    UIEdgeInsets s = self.safeAreaInsets; if (!_overlay) s.top = s.left = s.right = 0;
    CGRect a = UIEdgeInsetsInsetRect(self.bounds, s);
    // 下半很高（一般 iPhone 直拿、iPad 的窄視窗）：只用寬度 0.85 倍高、貼底排，鍵不會散到拇指搆不到的地方
    // A very tall lower half (an ordinary iPhone upright, a narrow iPad window): use a height of 0.85 × the width at the bottom, so the keys do not spread beyond thumb reach
    if (!_overlay && a.size.height > a.size.width * 0.85) { CGFloat h = round(a.size.width * 0.85); a.origin.y += a.size.height - h; a.size.height = h; }
    return a;
}
- (CGFloat)u { CGRect a = self.area; return _overlay ? MIN(MIN(a.size.height / 430.0, O_U_MAX), a.size.width / O_REF_W) : MIN(a.size.width / K_REF_W, a.size.height / K_REF_H); }
static CGPoint at(CGRect a, int side, CGFloat dx, CGFloat y) {   // side：-1 量自左緣、0 自中線、1 自右緣 | side: -1 from the left edge, 0 from the midline, 1 from the right edge
    return CGPointMake(side < 0 ? CGRectGetMinX(a) + dx : side > 0 ? CGRectGetMaxX(a) - dx : CGRectGetMidX(a) + dx, y);
}
- (CGFloat)rowY:(CGFloat)fy { CGRect a = self.area; return CGRectGetMinY(a) + fy * a.size.height; }
- (CGFloat)midX:(BOOL)right r:(CGFloat)r dx:(CGFloat)dx {   // 中間的鍵：有直的摺痕就貼在它兩邊，否則按中線 | middle keys: beside a vertical fold when there is one, otherwise from the midline
    CGRect a = self.area; CGFloat u = self.u;
    if (!CGRectIsNull(_fold) && CGRectGetMinX(_fold) > CGRectGetMinX(a) && CGRectGetMaxX(_fold) < CGRectGetMaxX(a))
        return right ? CGRectGetMaxX(_fold) + O_MID_GAP * u + r : CGRectGetMinX(_fold) - O_MID_GAP * u - r;
    return CGRectGetMidX(a) + (right ? dx : -dx) * u;
}
- (CGRect)btnRect:(int)k {   // 鎖定的鍵的外框 | the frame of a locked key
    CGRect a = self.area; CGFloat u = self.u, y = CGRectGetMinY(a) + (_overlay ? O_ROW_Y : K_ROW_Y) * u, sy = CGRectGetMaxY(a) - (_overlay ? O_SYS_DY : K_SYS_DY) * u, w, r; CGPoint c;
    switch (k) {
        case K_ZL: case K_ZR: c = at(a, k == K_ZL ? -1 : 1, K_ZL_X * u, y); w = K_ZL_W * u; return CGRectMake(c.x - w / 2, y - K_SH_H * u / 2, w, K_SH_H * u);
        case K_L: case K_R: c = at(a, k == K_L ? -1 : 1, K_L_X * u, y); w = K_L_W * u; return CGRectMake(c.x - w / 2, y - K_SH_H * u / 2, w, K_SH_H * u);
        case K_MINUS: case K_PLUS: r = K_PM_R * u; c = CGPointMake([self midX:k == K_PLUS r:r dx:K_PM_DX], y); break;
        default: r = K_SYS_R * u; c = CGPointMake([self midX:k == K_CAP r:r dx:K_SYS_DX], sy); break;
    }
    return CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r);
}
- (CGPoint)faceCenter { CGRect a = self.area; CGFloat u = self.u;
    return _overlay ? at(a, 1, O_PAD_X * u, CGRectGetMaxY(a) - O_PAD_Y * u) : at(a, 1, K_FACE_X * u, [self rowY:K_MID_FY]); }
- (CGPoint)facePos:(int)k { CGPoint c = self.faceCenter; CGFloat g = K_FACE_GAP * self.u;
    switch (k) { case K_A: return CGPointMake(c.x + g, c.y); case K_B: return CGPointMake(c.x, c.y + g); case K_X: return CGPointMake(c.x, c.y - g); default: return CGPointMake(c.x - g, c.y); } }
- (CGPoint)dpadCenter { CGRect a = self.area; CGFloat u = self.u;
    return _overlay ? at(a, -1, O_LOW_X * u, CGRectGetMaxY(a) - O_DPAD_Y * u) : at(a, -1, K_DPAD_X * u, [self rowY:K_LOW_FY]); }
- (CGPoint)stickHome:(int)i { CGRect a = self.area; CGFloat u = self.u;
    if (_overlay) return i ? at(a, 1, O_LOW_X * u, CGRectGetMaxY(a) - O_RSTICK_Y * u) : at(a, -1, O_PAD_X * u, CGRectGetMaxY(a) - O_PAD_Y * u);
    return i ? at(a, 1, K_RSTICK_X * u, [self rowY:K_LOW_FY]) : at(a, -1, K_STICK_X * u, [self rowY:K_MID_FY]); }
- (CGFloat)stickR:(int)i { return (i ? K_RSTICK_R : K_STICK_R) * self.u; }
- (void)setOverlay:(BOOL)overlay fold:(CGRect)fold {
    if (overlay == _overlay && CGRectEqualToRect(fold, _fold)) return;
    [self reset]; _overlay = overlay; _fold = fold; [self setNeedsLayout];
}
- (BOOL)claims:(CGPoint)p {   // 這一點歸不歸某個鍵（浮在畫面上時，其餘的點交給畫面當觸屏） | whether a key owns this point (over the picture, every other point goes to the picture as touch)
    CGFloat u = self.u;
    if (_keysOff) return [self btnAt:p] >= 0;
    if ([self btnAt:p] >= 0 || hypot(p.x - self.dpadCenter.x, p.y - self.dpadCenter.y) <= K_DPAD_K * 1.5 * u * K_DPAD_GRAB) return YES;
    if (hypot(p.x - self.faceCenter.x, p.y - self.faceCenter.y) <= (K_FACE_GAP + K_FACE_R * 1.6) * u) return YES;
    for (int i = 0; i < 2; i++) { CGPoint h = [self stickHome:i]; if (hypot(p.x - h.x, p.y - h.y) <= [self stickR:i] * K_STICK_GRAB) return YES; }
    return NO;
}
- (BOOL)pointInside:(CGPoint)p withEvent:(UIEvent *)e { return _overlay ? [self claims:p] : [super pointInside:p withEvent:e]; }

static CGPathRef circle(CGRect r) { return CGPathCreateWithEllipseInRect(r, NULL); }
static void place(DuoKeyLayer *k, CGRect frame, CGPathRef (^shape)(CGRect bounds)) {
    k.frame = frame; CGPathRef p = shape(CGRectMake(0, 0, frame.size.width, frame.size.height)); [k setShape:p]; CGPathRelease(p);
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat u = self.u, s = self.traitCollection.displayScale;
    UIColor *navy = duo_rgb(DUO_NAVY), *orange = duo_rgb(DUO_ORANGE);
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    CGFloat op = _overlay ? (_opacity > 0 ? _opacity : 0.6) : 1;   // 浮在畫面上時整個鍵半透明（每鍵一個小的離屏合成） | over the picture each whole key is translucent (one small off-screen pass per key)
    UIColor *drop = _overlay ? [UIColor colorWithWhite:0 alpha:0.28] : duo_rgb(DUO_DROP);   // 落影：黃底上是深黃，畫面上是淡黑 | the shadow: darker yellow on the yellow, faint black over the picture
    for (int k = 0; k < K_N; k++) { _key[k].contentsScale = s; _key[k].depth = K_DEPTH * u; _key[k].opacity = op; _key[k].dropColor = drop; }
    for (int i = 0; i < 2; i++) _knob[i].dropColor = drop;
    for (int k = K_ZL; k <= K_CAP; k++) {
        CGRect r = [self btnRect:k]; BOOL round = k == K_MINUS || k == K_PLUS || k == K_HOME;
        place(_key[k], r, ^CGPathRef(CGRect b) {
            if (k == K_CAP) return CGPathCreateWithRoundedRect(CGRectInset(b, 1.5 * u, 1.5 * u), 6 * u, 6 * u, NULL);
            return round ? circle(b) : CGPathCreateWithRoundedRect(b, b.size.height / 2, b.size.height / 2, NULL);
        });
        if (k_name[k]) [_key[k] setText:k_name[k] font:duo_font((k <= K_ZR ? 21 : 22) * u, UIFontWeightBold) color:navy];
    }
    for (int k = K_A; k <= K_Y; k++) {
        CGPoint c = [self facePos:k]; CGFloat r = K_FACE_R * u;
        place(_key[k], CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r), ^CGPathRef(CGRect b) { return circle(b); });
        [_key[k] setText:k_name[k] font:duo_font(22 * u, UIFontWeightBold) color:k == K_A ? orange : navy];
    }
    // A：鍵面內一圈橘線（遊戲確認鍵的樣子） | A: an orange ring inside the face (the look of the game's confirm button)
    CAShapeLayer *aring = (CAShapeLayer *)[_key[K_A].content.sublayers filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"name == 'aring'"]].firstObject;
    if (!aring) { aring = [CAShapeLayer layer]; aring.name = @"aring"; aring.fillColor = nil; aring.strokeColor = orange.CGColor; [_key[K_A].content insertSublayer:aring atIndex:0]; }
    aring.frame = _key[K_A].bounds; aring.lineWidth = 2.5 * u;
    CGPathRef ap = circle(CGRectInset(_key[K_A].bounds, 4 * u, 4 * u)); aring.path = ap; CGPathRelease(ap);
    // 十字鍵：四個小方鍵，各帶一個三角 | D-pad: four small square keys, each with a triangle
    CGPoint dc = self.dpadCenter; CGFloat dk = K_DPAD_K * u;
    static const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    for (int i = 0; i < 4; i++) {
        CGRect r = CGRectMake(dc.x + dx[i] * dk - dk / 2, dc.y + dy[i] * dk - dk / 2, dk, dk);
        place(_key[K_UP + i], r, ^CGPathRef(CGRect b) { return CGPathCreateWithRoundedRect(b, 7 * u, 7 * u, NULL); });
        CGFloat t = 5 * u; CGMutablePathRef tri = CGPathCreateMutable(); CGPoint m = CGPointMake(dk / 2, dk / 2);
        CGPoint p0 = CGPointMake(m.x + dx[i] * t, m.y + dy[i] * t), p1 = CGPointMake(m.x - dx[i] * t + dy[i] * t, m.y - dy[i] * t + dx[i] * t), p2 = CGPointMake(m.x - dx[i] * t - dy[i] * t, m.y - dy[i] * t - dx[i] * t);
        CGPathMoveToPoint(tri, NULL, p0.x, p0.y); CGPathAddLineToPoint(tri, NULL, p1.x, p1.y); CGPathAddLineToPoint(tri, NULL, p2.x, p2.y); CGPathCloseSubpath(tri);
        _arrow[i].frame = CGRectMake(0, 0, dk, dk); _arrow[i].path = tri; CGPathRelease(tri);
    }
    // HOME 圖標、截圖鍵的圈 | the HOME icon, the capture key's ring
    [_key[K_HOME] setSymbol:@"house.fill" size:17 * u color:navy];
    CAShapeLayer *cring = _key[K_CAP].content.sublayers.firstObject; CGFloat cr = 5.5 * u; CGSize cb = _key[K_CAP].bounds.size;
    CGPathRef cp = circle(CGRectMake(cb.width / 2 - cr, cb.height / 2 - cr, 2 * cr, 2 * cr)); cring.path = cp; cring.lineWidth = 2.5 * u; CGPathRelease(cp);
    // 搖桿：淡白圓盤 + 白色手柄頭 | sticks: a pale disc and a white knob
    for (int i = 0; i < 2; i++) {
        CGPoint h = [self stickHome:i]; CGFloat R = [self stickR:i], kr = (i ? K_RKNOB_R : K_KNOB_R) * u;
        _ring[i].frame = self.bounds; _ring[i].lineWidth = 3 * u; _knob[i].opacity = op;
        _ring[i].fillColor = [UIColor colorWithWhite:1 alpha:_overlay ? 0.18 : 0.43].CGColor; _ring[i].strokeColor = [UIColor colorWithWhite:1 alpha:_overlay ? 0.6 : 0.9].CGColor;
        CGPathRef rp = circle(CGRectMake(h.x - R, h.y - R, 2 * R, 2 * R)); _ring[i].path = rp; CGPathRelease(rp);
        _knob[i].contentsScale = s; _knob[i].depth = K_DEPTH * u; _knob[i].bounds = CGRectMake(0, 0, 2 * kr, 2 * kr);
        CGPathRef kp = circle(_knob[i].bounds); [_knob[i] setShape:kp]; CGPathRelease(kp);
    }
    [CATransaction commit];
    [self refresh];
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }

// ---- 觸控 | Touch ----
static CGFloat dist(CGPoint a, CGPoint b) { return hypot(a.x - b.x, a.y - b.y); }
- (int)btnAt:(CGPoint)p {   // 鎖定的鍵：外框四周各放寬 8 點 | locked keys: each frame widened by 8 points on every side
    for (int k = _keysOff ? K_HOME : K_ZL; k <= K_CAP; k++) if (CGRectContainsPoint(CGRectInset([self btnRect:k], -8 * self.u, -8 * self.u), p)) return k;
    return -1;
}
- (uint64_t)faceAt:(CGPoint)p {   // 手指下的菱形鍵（兩鍵之間兩個都算） | diamond keys under the finger (between two keys both count)
    uint64_t m = 0; CGFloat r = K_FACE_R * self.u * 1.3;
    for (int k = K_A; k <= K_Y; k++) if (dist(p, [self facePos:k]) <= r) m |= 1ull << k;
    return m;
}
- (uint64_t)dpadAt:(CGPoint)p {   // 8 向，每向 45°，斜向按兩鍵；中心小圈不算 | 8 directions of 45°, diagonals press two keys; a small centre circle counts as none
    CGPoint c = self.dpadCenter; CGFloat dx = p.x - c.x, dy = p.y - c.y;
    if (hypot(dx, dy) < K_DPAD_K * self.u * 0.35) return 0;
    double a = atan2(-dy, dx) * 180 / M_PI; uint64_t m = 0;
    if (a > -67.5 && a < 67.5) m |= 1ull << K_RIGHT;
    if (a > 22.5 && a < 157.5) m |= 1ull << K_UP;
    if (a > 112.5 || a < -112.5) m |= 1ull << K_LEFT;
    if (a < -22.5 && a > -157.5) m |= 1ull << K_DOWN;
    return m;
}
- (CGPoint)knobOffset:(int)i {   // 手柄頭偏離環心，限在環內 | knob offset from the ring centre, kept inside the ring
    if (!_stick[i]) return CGPointZero;
    CGPoint h = [self stickHome:i]; CGFloat dx = _stickNow[i].x - h.x, dy = _stickNow[i].y - h.y, d = hypot(dx, dy), R = [self stickR:i];
    if (d > R) { dx *= R / d; dy *= R / d; }
    return CGPointMake(dx, dy);
}
- (void)recompute {
    uint64_t keys = 0;
    for (UITouch *t in _faceOf) keys |= [_faceOf objectForKey:t].unsignedLongLongValue;
    for (UITouch *t in _owner) { uint64_t v = [_owner objectForKey:t].unsignedLongLongValue; int c = (int)(v & 0xff);
        if (c == C_BTN) keys |= 1ull << (v >> 8);
        else if (c == C_DPAD) keys |= [self dpadAt:[t locationInView:self]]; }
    int32_t ax[4] = {0};
    for (int i = 0; i < 2; i++) {
        CGPoint o = [self knobOffset:i]; CGFloat R = [self stickR:i], d = hypot(o.x, o.y), dz = R * K_DEAD;
        if (_stick[i] && dist(_stickNow[i], _stickP0[i]) > 10 * self.u) _stickMoved[i] = YES;   // 輕點要按著不動：看離按下點的移動，不看離環心（按在環心旁邊也算輕點） | a tap means staying put: measured from the touch-down point, not the ring centre (a press beside the centre still counts as a tap)
        if (d <= dz) continue;
        CGFloat m = fmin((d - dz) / (R * K_FULL - dz), 1) / d;   // 死區外從 0 起，推到 0.8 倍環半徑為滿 | from 0 outside the dead zone to full at 0.8 of the ring radius
        ax[i * 2] = (int32_t)lrint(o.x * m * DM_STICK_MAX); ax[i * 2 + 1] = (int32_t)lrint(-o.y * m * DM_STICK_MAX);   // 宿主：上為正 | host: up is positive
    }
    uint64_t newly = keys & ~_keys & ~((1ull << K_HOME) | (1ull << K_CAP));
    if (newly) [_haptic impactOccurred];
    _keys = keys;
    uint64_t btn = 0; for (int k = 0; k < K_N; k++) if ((keys >> k) & 1) btn |= k_bit[k];
    btn |= _clicks;
    duo_pad_publish(btn, ax[0], ax[1], ax[2], ax[3]);
    [self refresh];
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        CGPoint p = [t locationInView:self]; int hit = [self btnAt:p];
        if (hit >= 0) { [_owner setObject:@(C_BTN | (uint64_t)hit << 8) forKey:t]; continue; }
        if (_keysOff) continue;
        CGFloat u = self.u, best = 1; int kind = C_NONE, idx = 0;
        CGFloat f = dist(p, self.faceCenter) / ((K_FACE_GAP + K_FACE_R * 1.6) * u); if (f <= best) { best = f; kind = C_FACE; }
        CGFloat d = dist(p, self.dpadCenter) / (K_DPAD_K * 1.5 * u * K_DPAD_GRAB); if (d < best) { best = d; kind = C_DPAD; }
        for (int i = 0; i < 2; i++) if (!_stick[i]) {
            CGFloat s = dist(p, [self stickHome:i]) / ([self stickR:i] * K_STICK_GRAB); if (s < best) { best = s; kind = C_STICK; idx = i; } }
        if (kind == C_FACE) { [_owner setObject:@(C_FACE) forKey:t]; [_faceOf setObject:@([self faceAt:p]) forKey:t]; }
        else if (kind == C_DPAD) [_owner setObject:@(C_DPAD) forKey:t];
        else if (kind == C_STICK) {
            [_owner setObject:@(C_STICK | (uint64_t)idx << 8) forKey:t];
            _stick[idx] = t; _stickNow[idx] = _stickP0[idx] = p; _stickT0[idx] = t.timestamp;
            _stickMoved[idx] = dist(p, [self stickHome:idx]) > (idx ? K_RKNOB_R : K_KNOB_R) * u * 1.3;   // 只有按在手柄頭上才可能是 L3 / R3；按在環邊是要推 | only a press on the knob can be L3 / R3; a press at the ring's edge means a push
            // 按在環邊上：手柄頭直接跳到指下，從環心起算 | touching near the ring edge: the knob jumps under the finger, measured from the centre
        }
    }
    [self recompute];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        uint64_t v = [_owner objectForKey:t].unsignedLongLongValue; int c = (int)(v & 0xff); CGPoint p = [t locationInView:self];
        if (c == C_FACE) [_faceOf setObject:@([self faceAt:p]) forKey:t];
        else if (c == C_STICK) _stickNow[v >> 8] = p;
    }
    [self recompute];
}
- (void)endTouches:(NSSet<UITouch *> *)ts ended:(BOOL)ended {
    for (UITouch *t in ts) {
        uint64_t v = [_owner objectForKey:t].unsignedLongLongValue; int c = (int)(v & 0xff); int k = (int)(v >> 8);
        if (ended && c == C_BTN && (k == K_HOME || k == K_CAP) && CGRectContainsPoint(CGRectInset([self btnRect:k], -8 * self.u, -8 * self.u), [t locationInView:self]))
            k == K_HOME ? duo_home_pressed() : duo_capture_pressed();   // 在鍵上鬆手才算 | counts only when released on the key
        for (int i = 0; i < 2; i++) if (t == _stick[i]) {
            if (ended && !_stickMoved[i] && t.timestamp - _stickT0[i] < K_TAP_S) [self click:i];
            _stick[i] = nil;
        }
        [_owner removeObjectForKey:t]; [_faceOf removeObjectForKey:t];
    }
    [self recompute];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts ended:YES]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts ended:NO]; }
- (void)click:(int)i {   // 輕點搖桿：L3 / R3 按住 0.12 秒（遊戲至少讀到幾幀） | a tap on a stick: hold L3 / R3 for 0.12 s (the game reads it for a few frames at least)
    uint64_t bit = i ? DM_BTN_RS : DM_BTN_LS;
    _clicks |= bit; _knob[i].pressed = YES; [_haptic impactOccurred];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.12 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ self->_clicks &= ~bit; [self recompute]; });
}
- (void)reset { [_owner removeAllObjects]; [_faceOf removeAllObjects]; _stick[0] = _stick[1] = nil; _clicks = 0; [self recompute]; }
- (void)setOpacity:(CGFloat)a { if (a == _opacity) return; _opacity = a; [self setNeedsLayout]; }
- (void)setKeysOff:(BOOL)off {   // 實體手把接上：藏起遊戲鍵，留 HOME 與截圖（手把上沒有能給 App 用的這兩個鍵） | a physical controller connected: hide the game keys, keep HOME and capture (the controller has no buttons the app may use for these two)
    if (off == _keysOff) return;
    [self reset]; _keysOff = off;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int k = 0; k < K_N; k++) _key[k].hidden = off && k != K_HOME && k != K_CAP;
    for (int i = 0; i < 2; i++) _ring[i].hidden = _knob[i].hidden = off;
    [CATransaction commit];
}

- (void)refresh {   // 按下狀態 → 鍵面沉下，搖桿 → 手柄頭位置 | pressed state → sunken faces, sticks → knob position
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int k = 0; k < K_N; k++) _key[k].pressed = (_keys >> k) & 1;
    for (int i = 0; i < 2; i++) {
        CGPoint h = [self stickHome:i], o = [self knobOffset:i];
        _knob[i].position = CGPointMake(h.x + o.x, h.y + o.y);
        _knob[i].pressed = (_clicks & (i ? DM_BTN_RS : DM_BTN_LS)) != 0;
    }
    [CATransaction commit];
}
@end

UIView *duo_pad_new(void) { return [[DuoPad alloc] initWithFrame:CGRectZero]; }
void duo_pad_set_opacity(UIView *pad, CGFloat a) { if (![pad isKindOfClass:DuoPad.class]) return; DuoPad *p = (DuoPad *)pad; [p setOpacity:a]; }
void duo_pad_arrange(UIView *pad, BOOL overlay, CGRect fold) { if ([pad isKindOfClass:DuoPad.class]) [(DuoPad *)pad setOverlay:overlay fold:fold]; }
void duo_pad_reset(UIView *pad) { if ([pad isKindOfClass:DuoPad.class]) [(DuoPad *)pad reset]; }
void duo_pad_set_hidden(UIView *pad, BOOL hidden) { if ([pad isKindOfClass:DuoPad.class]) [(DuoPad *)pad setKeysOff:hidden]; }   // 接上實體手把時藏起遊戲鍵 | hide the game keys while a physical controller is connected
