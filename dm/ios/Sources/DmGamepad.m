// 虛擬手柄（任天堂鍵位）+ 實體手柄（GameController）。兩路合併後經 dm_input_set 交給宿主（hle_hid.c，鍵位見 dm_input.h）。
//   上角：ZL 在上、L 在下（同一列），ZR / R 同，水滴形，尖朝屏角；頂邊正中：− / + 並排；
//   左下角：左搖桿（環固定，推量按環心算，帶小死區）；右下角：A（右）B（下）X（上）Y（左）菱形，手指在鍵間滑動會換鍵。
//   虛擬手柄沒有十字鍵和右搖桿，下角只各剩一個控件，所以貼屏角放（伸進側邊安全區一半）。控件都往邊角靠，給畫面多留地方；
//   按鍵疊在畫面上，每個控件一個 CAShapeLayer：按下只換填色，搖桿只挪手柄頭，不整屏重畫。
//   接上實體手柄時虛擬手柄隱藏，拔掉後重現。實體手柄全鍵都在（含十字鍵、右搖桿），按位置對應：下 = B、右 = A、左 = Y、上 = X（與 Switch 手柄的鍵位位置一致）。
// Virtual gamepad (Nintendo button positions) + physical controllers (GameController). Both are merged and handed to the host through dm_input_set (hle_hid.c, bit layout in dm_input.h).
//   Top corners: ZL above L (one column), ZR above R likewise, teardrops with the tip toward the screen corner; top edge, centred: − and + side by side;
//   bottom-left corner: the left stick (fixed ring, tilt measured from the ring centre, small dead zone); bottom-right corner: the A (right) B (bottom) X (top) Y (left) diamond, sliding a finger between keys switches keys.
//   The virtual gamepad has no D-pad and no right stick, so each bottom corner holds a single control and it sits close in (reaching halfway into the side safe-area inset). Everything hugs the edges and corners to leave the picture room;
//   controls sit over the picture, one CAShapeLayer each: a press only swaps the fill, the stick only moves its knob, nothing redraws the whole screen.
//   The virtual gamepad hides while a physical controller is connected and comes back when it is removed. Physical controllers keep every button (D-pad and right stick included) and map by position: bottom = B, right = A, left = Y, top = X (same positions as on a Switch controller).
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <CoreText/CoreText.h>
#include <pthread.h>
#include "dm_input.h"

enum { B_A = 0, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN, B_N };
enum { C_NONE = 0, C_STICK, C_FACE, C_BTN };   // 觸點佔用的控件 | which control a touch holds

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_touch_btn, g_pad_btn;
static int32_t g_touch_lx, g_touch_ly, g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;
static int tilted(int32_t x, int32_t y) { return (int64_t)x * x + (int64_t)y * y > (int64_t)4000 * 4000; }
static void publish(void) {
    pthread_mutex_lock(&g_mu);
    int32_t lx = g_touch_lx, ly = g_touch_ly;
    if (tilted(g_pad_lx, g_pad_ly)) { lx = g_pad_lx; ly = g_pad_ly; }   // 實體搖桿推著時用實體的 | a tilted physical stick wins
    dm_input_set(g_touch_btn | g_pad_btn, lx, ly, g_pad_rx, g_pad_ry);   // 右搖桿只有實體手柄有 | only a physical controller has a right stick
    pthread_mutex_unlock(&g_mu);
}

// ---- 佈局常數（單位 u；x 量自外側邊，右半邊鏡像） | Layout constants (in u; x measured from the outer edge, the right half mirrors) ----
static const CGFloat
    K_ZL_X = 44, K_ZL_Y = 40, K_ZL_R = 26,          // ZL / ZR：中心離外側、離頂，半徑 | ZL / ZR: centre from the outer edge and the top, radius
    K_L_X = 44, K_L_Y = 108, K_L_R = 24,            // L / R：與 ZL / ZR 同一列 | L / R: same column as ZL / ZR
    K_DROP_TIP = 1.45,                              // 水滴尖離圓心（半徑的倍數） | teardrop tip distance from the centre (times the radius)
    K_CORNER_IN = 0.5,                              // 下角控件伸進側邊安全區的比例（橫屏時那裡只有屏幕圓角，靈動島在邊的正中） | how far the bottom-corner controls reach into the side safe-area inset (in landscape there is only the rounded screen corner there; the Dynamic Island is mid-edge)
    K_FACE_X = 81, K_PAD_Y = 82,                    // 菱形中心離外側；菱形、搖桿中心離底（B 鍵下沿離底約 5） | diamond centre from the outer edge; diamond and stick centre from the bottom (B's lower edge about 5 above it)
    K_FACE_GAP = 50, K_FACE_R = 27,                 // 菱形：鍵離中心、鍵半徑 | diamond: key distance from the centre, key radius
    K_PILL_DX = 40, K_PILL_Y = 22, K_PILL_W = 50, K_PILL_H = 28,   // − / +：中心離屏幕中線、離頂，寬高 | − / +: centre from the screen's midline and from the top, size
    K_STICK_X = 81, K_STICK_R = 77, K_KNOB_R = 30,  // 左搖桿：與菱形左右對稱，環和整個菱形一樣大（鍵距 + 鍵半徑），手柄頭比菱形的鍵略大 | left stick: mirrors the diamond, the ring as large as the whole diamond (key distance + key radius), the knob a little larger than a diamond key
    K_STICK_GRAB = 1.4,                             // 按下點離環心多近算抓住搖桿（環半徑的倍數；左下角沒有別的控件和它搶） | how near the ring centre a touch-down grabs the stick (times the ring radius; nothing else in the bottom-left corner competes)
    K_FULL = 0.8,                                   // 推到環半徑的這麼多就算推滿（約 62，原先小環是 50；手柄頭仍能走到環邊） | full tilt at this fraction of the ring radius (about 62; the former small ring had 50; the knob still travels to the ring edge)
    K_DEAD = 0.1;                                   // 搖桿死區（環半徑的倍數） | stick dead zone (times the ring radius)
static const int k_btns[] = {B_ZL, B_L, B_ZR, B_R, B_MINUS, B_PLUS};

@interface DmGamepad : UIView
@end
static __weak DmGamepad *g_view;

@implementation DmGamepad {
    NSMapTable<UITouch *, NSNumber *> *_owner;     // 觸點 → 控件（C_BTN 時附帶鍵號，<< 8） | touch → control (with the key for C_BTN, << 8)
    UITouch *_stick; CGPoint _stickNow;            // 握著左搖桿的觸點、它現在的位置 | the touch holding the left stick and where it is now
    uint64_t _faceBits, _btnBits;
    NSMapTable<UITouch *, NSNumber *> *_faceOf;    // 每個落在菱形區的觸點各自按著哪些鍵 | which keys each touch on the diamond currently holds
    CAShapeLayer *_key[B_N]; CATextLayer *_label[B_N];   // 每個鍵一層（搖桿按下、十字鍵只有實體手柄有，位空著） | one layer per key (stick clicks and the D-pad exist only on physical controllers; those stay empty)
    CAShapeLayer *_ring, *_knob;
    BOOL _stickShown;                              // 上次刷新時搖桿按著 | the stick was held at the last refresh
}

static UIColor *k_idle, *k_on, *k_line, *k_ringFill, *k_text, *k_textOn;
static CAShapeLayer *shape(CALayer *parent) {
    CAShapeLayer *l = [CAShapeLayer layer];
    l.fillColor = k_idle.CGColor; l.strokeColor = k_line.CGColor; l.lineWidth = 1.5;
    l.shadowColor = UIColor.blackColor.CGColor; l.shadowOpacity = 0.2; l.shadowRadius = 3; l.shadowOffset = CGSizeMake(0, 1);
    [parent addSublayer:l]; return l;
}
static CATextLayer *label(CALayer *parent, NSString *s) {
    CATextLayer *t = [CATextLayer layer];
    t.string = s; t.alignmentMode = kCAAlignmentCenter; t.foregroundColor = k_text.CGColor; t.contentsScale = UIScreen.mainScreen.scale;
    [parent addSublayer:t]; return t;
}

- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        self.multipleTouchEnabled = YES; self.opaque = NO; self.backgroundColor = UIColor.clearColor;
        _owner = [NSMapTable strongToStrongObjectsMapTable]; _faceOf = [NSMapTable strongToStrongObjectsMapTable];
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            // 各色各自壓低不透明度，不調整個視圖的 alpha（那樣全屏層要多一遍離屏合成） | each colour carries its own low opacity instead of lowering the whole view's alpha (that would add an off-screen pass for the full-screen layer)
            k_idle = [UIColor colorWithWhite:1 alpha:0.14]; k_on = [UIColor colorWithWhite:1 alpha:0.45]; k_line = [UIColor colorWithWhite:1 alpha:0.38];
            k_ringFill = [UIColor colorWithWhite:1 alpha:0.06]; k_text = [UIColor colorWithWhite:1 alpha:0.6]; k_textOn = [UIColor colorWithWhite:0 alpha:0.7];
        });
        _ring = shape(self.layer); _ring.fillColor = k_ringFill.CGColor;
        _knob = shape(self.layer);
        CAShapeLayer *in = [CAShapeLayer layer]; in.fillColor = nil; in.strokeColor = k_line.CGColor; in.lineWidth = 1.5;   // 手柄頭內圈 | inner ring of the knob
        [_knob addSublayer:in];
        static NSString *const names[B_N] = {@"A", @"B", @"X", @"Y", nil, nil, @"L", @"R", @"ZL", @"ZR", @"+", @"−"};
        for (int b = 0; b < B_N; b++) {
            if (b == B_LS || b == B_RS || b >= B_LEFT) continue;
            _key[b] = shape(self.layer);
            if (names[b]) _label[b] = label(_key[b], names[b]);
        }
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserver:self selector:@selector(padsChanged) name:GCControllerDidConnectNotification object:nil];
        [nc addObserver:self selector:@selector(padsChanged) name:GCControllerDidDisconnectNotification object:nil];
        [self padsChanged];
    }
    return self;
}

// ---- 佈局（點；按安全區內的矩形算） | Layout (points; computed inside the safe area) ----
- (CGRect)safe { return UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets); }
- (CGRect)corner { UIEdgeInsets i = self.safeAreaInsets; i.left *= K_CORNER_IN; i.right *= K_CORNER_IN; return UIEdgeInsetsInsetRect(self.bounds, i); }   // 下角控件用的矩形 | the rectangle the bottom-corner controls use
- (CGFloat)u { return MIN(self.bounds.size.height / 430.0, 1.3); }   // 以 430 點高（iPhone Pro Max 橫屏）為 1 | 1 = 430 points tall (iPhone Pro Max landscape)
static CGPoint side(CGRect s, int right, CGFloat dx, CGFloat y) { return CGPointMake(right ? CGRectGetMaxX(s) - dx : CGRectGetMinX(s) + dx, y); }
- (CGPoint)faceCenter { CGRect s = self.corner; CGFloat u = self.u; return side(s, 1, K_FACE_X * u, CGRectGetMaxY(s) - K_PAD_Y * u); }
- (CGFloat)faceGap { return K_FACE_GAP * self.u; }
- (CGFloat)faceR { return K_FACE_R * self.u; }
- (CGPoint)facePos:(int)b { CGPoint c = self.faceCenter; CGFloat g = self.faceGap;
    switch (b) { case B_A: return CGPointMake(c.x + g, c.y); case B_B: return CGPointMake(c.x, c.y + g); case B_X: return CGPointMake(c.x, c.y - g); default: return CGPointMake(c.x - g, c.y); } }
- (CGPoint)stickHome { CGRect s = self.corner; CGFloat u = self.u; return side(s, 0, K_STICK_X * u, CGRectGetMaxY(s) - K_PAD_Y * u); }
- (CGFloat)stickR { return K_STICK_R * self.u; }
static BOOL isDrop(int b) { return b == B_ZL || b == B_L || b == B_ZR || b == B_R; }
- (CGPoint)dropCenter:(int)b { CGRect s = self.safe; CGFloat u = self.u; BOOL z = b == B_ZL || b == B_ZR;
    return side(s, b == B_ZR || b == B_R, (z ? K_ZL_X : K_L_X) * u, CGRectGetMinY(s) + (z ? K_ZL_Y : K_L_Y) * u); }
- (CGFloat)dropR:(int)b { return (b == B_ZL || b == B_ZR ? K_ZL_R : K_L_R) * self.u; }
- (CGRect)pillRect:(int)b { CGRect s = self.safe; CGFloat u = self.u, w = K_PILL_W * u, h = K_PILL_H * u;
    CGPoint c = CGPointMake(CGRectGetMidX(s) + (b == B_PLUS ? K_PILL_DX : -K_PILL_DX) * u, CGRectGetMinY(s) + K_PILL_Y * u);   // − 在左、+ 在右 | − on the left, + on the right
    return CGRectMake(c.x - w / 2, c.y - h / 2, w, h); }

static CGFloat dist(CGPoint a, CGPoint b) { return hypot(a.x - b.x, a.y - b.y); }
- (int)btnAt:(CGPoint)p {   // 肩鍵與 − / +（只在按下時判，之後鎖定） | shoulders and − / + (tested on touch-down only, then locked)
    CGFloat u = self.u;
    for (int i = 0; i < 6; i++) { int b = k_btns[i];
        if (isDrop(b) ? dist(p, [self dropCenter:b]) <= [self dropR:b] * 1.35 : CGRectContainsPoint(CGRectInset([self pillRect:b], -8 * u, -8 * u), p)) return b; }
    return -1;
}
- (uint64_t)faceAt:(CGPoint)p {   // 菱形區裡手指下的鍵（兩鍵之間可同時按） | keys under the finger in the diamond (between two keys both count)
    uint64_t m = 0; CGFloat r = self.faceR * 1.25;
    for (int b = B_A; b <= B_Y; b++) if (dist(p, [self facePos:b]) <= r) m |= 1ull << b;
    return m;
}
- (BOOL)inFace:(CGPoint)p { return dist(p, self.faceCenter) <= self.faceGap + self.faceR * 1.6; }
- (BOOL)inStick:(CGPoint)p { return dist(p, self.stickHome) <= self.stickR * K_STICK_GRAB; }
- (CGPoint)knobOffset {   // 手柄頭偏離環心（限在環內） | knob offset from the ring centre (clamped to the ring)
    if (!_stick) return CGPointZero;
    CGPoint h = self.stickHome; CGFloat dx = _stickNow.x - h.x, dy = _stickNow.y - h.y, d = hypot(dx, dy), R = self.stickR;
    if (d > R) { dx *= R / d; dy *= R / d; }
    return CGPointMake(dx, dy);
}

// ---- 觸摸 | Touch ----
- (void)recompute {
    uint64_t face = 0; for (UITouch *t in _faceOf) face |= [_faceOf objectForKey:t].unsignedLongLongValue;
    uint64_t btn = 0;
    for (UITouch *t in _owner) { uint64_t v = [_owner objectForKey:t].unsignedLongLongValue; if ((v & 0xff) == C_BTN) btn |= 1ull << (v >> 8); }
    int32_t ax = 0, ay = 0;
    CGPoint o = self.knobOffset; CGFloat R = self.stickR, d = hypot(o.x, o.y), dz = R * K_DEAD;
    if (d > dz) {
        CGFloat m = fmin((d - dz) / (R * K_FULL - dz), 1) / d;   // 死區外從 0 起算，到推滿處為滿 | outside the dead zone: 0 rising to full at the full-tilt radius
        ax = (int32_t)lrint(o.x * m * 32767); ay = (int32_t)lrint(-o.y * m * 32767);   // 宿主：上為正；四捨五入，推滿正好是 32767 | host: up is positive; rounded so full tilt is exactly 32767
    }
    BOOL changed = face != _faceBits || btn != _btnBits;
    _faceBits = face; _btnBits = btn;
    pthread_mutex_lock(&g_mu);
    g_touch_btn = face | btn; g_touch_lx = ax; g_touch_ly = ay;
    pthread_mutex_unlock(&g_mu);
    publish();
    BOOL held = _stick != nil;
    if (changed || held || _stickShown) [self refresh];   // 鬆開搖桿那次也要刷，手柄頭才回中 | also refresh on the stick release so the knob returns to the centre
    _stickShown = held;
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        CGPoint p = [t locationInView:self]; int hit = [self btnAt:p];
        if (hit >= 0) [_owner setObject:@(C_BTN | (uint64_t)hit << 8) forKey:t];
        else if ([self inFace:p]) { [_owner setObject:@(C_FACE) forKey:t]; [_faceOf setObject:@([self faceAt:p]) forKey:t]; }
        else if (!_stick && [self inStick:p]) { [_owner setObject:@(C_STICK) forKey:t]; _stick = t; _stickNow = p; }
    }
    [self recompute];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        int c = (int)([_owner objectForKey:t].unsignedLongLongValue & 0xff); CGPoint p = [t locationInView:self];
        if (c == C_FACE) [_faceOf setObject:@([self faceAt:p]) forKey:t];
        else if (c == C_STICK) _stickNow = p;
    }
    [self recompute];
}
- (void)endTouches:(NSSet<UITouch *> *)ts {
    for (UITouch *t in ts) { [_owner removeObjectForKey:t]; [_faceOf removeObjectForKey:t]; if (t == _stick) _stick = nil; }
    [self recompute];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts]; }
- (void)reset { [_owner removeAllObjects]; [_faceOf removeAllObjects]; _stick = nil; [self recompute]; }

// ---- 形狀 | Shapes ----
static CGPathRef circlePath(CGPoint c, CGFloat r) { return CGPathCreateWithEllipseInRect(CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r), NULL); }
static CGPathRef dropPath(CGPoint c, CGFloat r, CGFloat tipAngle, CGFloat round) {   // 水滴：圓 + 兩條切線交於尖，尖略圓 | teardrop: circle + two tangents meeting at the tip, tip slightly rounded
    CGFloat d = r * K_DROP_TIP, th = acos(r / d);
    CGPoint tip = CGPointMake(c.x + d * cos(tipAngle), c.y + d * sin(tipAngle));
    CGPoint p1 = CGPointMake(c.x + r * cos(tipAngle + th), c.y + r * sin(tipAngle + th)), p2 = CGPointMake(c.x + r * cos(tipAngle - th), c.y + r * sin(tipAngle - th));
    CGMutablePathRef p = CGPathCreateMutable();
    CGPathMoveToPoint(p, NULL, p2.x, p2.y);
    CGPathAddArcToPoint(p, NULL, tip.x, tip.y, p1.x, p1.y, round);
    CGPathAddArc(p, NULL, c.x, c.y, r, tipAngle + th, tipAngle - th + 2 * M_PI, false);
    CGPathCloseSubpath(p); return p;
}
static void setPath(CAShapeLayer *l, CGPathRef p) { l.path = p; l.shadowPath = p; CGPathRelease(p); }
static void placeLabel(CATextLayer *t, CGPoint c, CGFloat fs) {
    CTFontRef f = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, fs, NULL);
    t.font = f; t.fontSize = fs;
    CGFloat h = CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f), w = fs * 3;
    t.frame = CGRectMake(c.x - w / 2, c.y - h / 2, w, h);
    CFRelease(f);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat u = self.u;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int b = B_A; b <= B_Y; b++) { CGPoint c = [self facePos:b]; setPath(_key[b], circlePath(c, self.faceR)); placeLabel(_label[b], c, 20 * u); }
    for (int i = 0; i < 6; i++) { int b = k_btns[i];
        if (isDrop(b)) {
            BOOL right = b == B_ZR || b == B_R; CGPoint c = [self dropCenter:b];
            setPath(_key[b], dropPath(c, [self dropR:b], right ? -M_PI_4 : -3 * M_PI_4, 4 * u)); placeLabel(_label[b], c, 16 * u);   // 尖朝同側上角 | tip toward the near top corner
        } else {
            CGRect rc = [self pillRect:b];
            setPath(_key[b], CGPathCreateWithRoundedRect(rc, rc.size.height / 2, rc.size.height / 2, NULL)); placeLabel(_label[b], CGPointMake(CGRectGetMidX(rc), CGRectGetMidY(rc)), 18 * u);
        }
    }
    setPath(_ring, circlePath(self.stickHome, self.stickR));
    CGFloat k = K_KNOB_R * u; _knob.bounds = CGRectMake(0, 0, 2 * k, 2 * k);
    setPath(_knob, circlePath(CGPointMake(k, k), k));
    CAShapeLayer *in = (CAShapeLayer *)_knob.sublayers.firstObject; CGPathRef ip = circlePath(CGPointMake(k, k), k * 0.62);
    in.frame = _knob.bounds; in.path = ip; CGPathRelease(ip);
    [CATransaction commit];
    [self refresh];
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (void)refresh {   // 按下狀態 → 填色，搖桿 → 手柄頭位置 | pressed state → fill, sticks → knob position
    uint64_t all = _faceBits | _btnBits;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int b = 0; b < B_N; b++) if (_key[b]) { BOOL on = (all >> b) & 1;
        _key[b].fillColor = (on ? k_on : k_idle).CGColor; _label[b].foregroundColor = (on ? k_textOn : k_text).CGColor; }
    CGPoint h = self.stickHome, o = self.knobOffset;
    _knob.position = CGPointMake(h.x + o.x, h.y + o.y); _knob.fillColor = (_stick ? k_on : k_idle).CGColor;
    [CATransaction commit];
}

// ---- 實體手柄 | Physical controllers ----
static int32_t axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * 32767); }
static void read_pad(GCExtendedGamepad *g) {
    uint64_t m = 0;
    #define K(btn, bit) if (btn.isPressed) m |= 1ull << bit
    K(g.buttonB, B_A); K(g.buttonA, B_B); K(g.buttonY, B_X); K(g.buttonX, B_Y);   // 按位置：右 A、下 B、上 X、左 Y | by position: right A, bottom B, top X, left Y
    K(g.leftShoulder, B_L); K(g.rightShoulder, B_R); K(g.leftTrigger, B_ZL); K(g.rightTrigger, B_ZR);
    K(g.buttonMenu, B_PLUS); if (g.buttonOptions) K(g.buttonOptions, B_MINUS);
    if (g.leftThumbstickButton) K(g.leftThumbstickButton, B_LS); if (g.rightThumbstickButton) K(g.rightThumbstickButton, B_RS);
    K(g.dpad.left, B_LEFT); K(g.dpad.up, B_UP); K(g.dpad.right, B_RIGHT); K(g.dpad.down, B_DOWN);
    #undef K
    pthread_mutex_lock(&g_mu);
    g_pad_btn = m; g_pad_lx = axis(g.leftThumbstick.xAxis.value); g_pad_ly = axis(g.leftThumbstick.yAxis.value);
    g_pad_rx = axis(g.rightThumbstick.xAxis.value); g_pad_ry = axis(g.rightThumbstick.yAxis.value);
    pthread_mutex_unlock(&g_mu);
    publish();
}
- (void)padsChanged {
    BOOL any = NO;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad) {
        any = YES; c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; read_pad(g); };
        read_pad(c.extendedGamepad);
    }
    if (!any) { pthread_mutex_lock(&g_mu); g_pad_btn = 0; g_pad_lx = g_pad_ly = g_pad_rx = g_pad_ry = 0; pthread_mutex_unlock(&g_mu); publish(); }
    dispatch_async(dispatch_get_main_queue(), ^{ self.hidden = any; if (any) [self reset]; });
}
@end

UIView *dm_gamepad_view(CGRect frame) { DmGamepad *v = [[DmGamepad alloc] initWithFrame:frame]; g_view = v; return v; }
void dm_gamepad_reset(void) {   // 失活時：鬆開所有鍵（按著鍵切走、在後台鬆手，回來不會補「鬆開」） | On resign active: release everything (a key held while switching away and released in background gets no "release")
    dispatch_async(dispatch_get_main_queue(), ^{ [g_view reset]; });
    pthread_mutex_lock(&g_mu); g_pad_btn = 0; g_pad_lx = g_pad_ly = g_pad_rx = g_pad_ry = 0; pthread_mutex_unlock(&g_mu); publish();
}
