// 打字列：遊戲要文字時（swkbd）貼在上半底邊、摺痕正上方，遊戲的輸入頁在它上面照常顯示；系統鍵盤佔下半。
//   樣子跟下半的手把同一套：黃底（上緣圓角），白色圓角鍵帶灰色下緣和深黃落影，確定是遊戲的確認橘。像是手把往上長出一條輸入列。
//   第一行：標題（遊戲請求的標題）、字數 / 上限。第二行：取消（✕ 圓鍵）、輸入框（白色長鍵，遊戲給的提示字當佔位，右端是貼上）、確定。
//   字數按 UTF-16 單元算，與 hle_swkbd.c 的截斷一致；正在選字（注音等的組字）時不截。貼上用 UIPasteControl，不會跳出「允許貼上」的詢問。
// The typing bar: when the game asks for text (swkbd) it sits on the bottom edge of the top half, just above the fold, with the game's input page still showing above it; the system keyboard takes the lower half.
//   It looks like the controller below: yellow (rounded top corners), white rounded keys with a grey lower edge and a darker-yellow shadow, OK in the game's confirm orange. As if the controller grew an input row upward.
//   First row: the title (from the game's request), count / limit. Second row: Cancel (a round ✕ key), the field (a long white key, the game's guide text as its placeholder, Paste at its right end), OK.
//   Counting is in UTF-16 units, matching the truncation in hle_swkbd.c; nothing is cut while a composition is in progress (Zhuyin etc.). Paste uses UIPasteControl, so no "allow paste" prompt appears.
#import <UIKit/UIKit.h>
#include "duo_shell.h"

// 列上的鍵（介面在 duo_shell.h） | a key on the bar (interface in duo_shell.h)
@implementation DuoButton
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) { _key = [DuoKeyLayer layer]; [self.layer addSublayer:_key]; }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect b = self.bounds; b.size.height -= 2 * _key.depth;   // 下緣與落影佔在框內 | the lower edge and shadow fit inside the frame
    _key.contentsScale = self.traitCollection.displayScale; _key.frame = b;
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(0, 0, b.size.width, b.size.height), b.size.height / 2, b.size.height / 2, NULL); [_key setShape:p]; CGPathRelease(p);
}
- (void)setHighlighted:(BOOL)h { [super setHighlighted:h]; _key.pressed = h; }
@end

// 輸入框：文字左右留白，右端讓給貼上鍵 | the field: text padded left and right, the right end left for Paste
@interface DuoField : UITextField
@property (nonatomic) CGFloat rightRoom;
@end
@implementation DuoField
- (CGRect)textRectForBounds:(CGRect)b { CGFloat h = b.size.height * 0.42; return UIEdgeInsetsInsetRect(b, UIEdgeInsetsMake(0, h, 0, h + _rightRoom)); }
- (CGRect)editingRectForBounds:(CGRect)b { return [self textRectForBounds:b]; }
- (CGRect)placeholderRectForBounds:(CGRect)b { return [self textRectForBounds:b]; }
@end

static BOOL zh(void) { return [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"]; }

@interface DuoTypeCard () <UITextFieldDelegate>
@end
@implementation DuoTypeCard {
    CAShapeLayer *_bg; DuoKeyLayer *_fieldKey; UILabel *_title, *_sub, *_count; DuoField *_field; DuoButton *_ok, *_cancel; UIPasteControl *_paste;
    int _max; void (^_done)(BOOL, NSString *);
}
- (instancetype)initWithTitle:(NSString *)title sub:(NSString *)sub guide:(NSString *)guide initial:(NSString *)initial maxChars:(int)maxChars
                         done:(void (^)(BOOL ok, NSString *text))done {
    if ((self = [super initWithFrame:CGRectZero])) {
        _max = maxChars; _done = [done copy];
        UIColor *navy = duo_rgb(DUO_NAVY), *orange = duo_rgb(DUO_ORANGE);
        _bg = [CAShapeLayer layer]; _bg.fillColor = duo_rgb(DUO_YELLOW).CGColor;
        _bg.shadowColor = UIColor.blackColor.CGColor; _bg.shadowOpacity = 0.16; _bg.shadowRadius = 14; _bg.shadowOffset = CGSizeMake(0, -2);
        [self.layer addSublayer:_bg];
        _title = [UILabel new]; _title.text = title; _title.textColor = navy; [self addSubview:_title];
        _sub = [UILabel new]; _sub.text = sub; _sub.textColor = [navy colorWithAlphaComponent:0.6]; _sub.hidden = !sub.length; [self addSubview:_sub];
        _count = [UILabel new]; _count.textAlignment = NSTextAlignmentRight; _count.hidden = maxChars <= 0; [self addSubview:_count];
        _fieldKey = [DuoKeyLayer layer]; [self.layer addSublayer:_fieldKey];
        _field = [DuoField new]; _field.text = initial; _field.textColor = navy; _field.tintColor = orange; _field.delegate = self;
        _field.borderStyle = UITextBorderStyleNone; _field.returnKeyType = UIReturnKeyDone;
        if (guide.length) _field.attributedPlaceholder = [[NSAttributedString alloc] initWithString:guide attributes:@{NSForegroundColorAttributeName: [navy colorWithAlphaComponent:0.32]}];
        [_field addTarget:self action:@selector(edited) forControlEvents:UIControlEventEditingChanged];
        [self addSubview:_field];
        UIPasteControlConfiguration *pc = [UIPasteControlConfiguration new];
        pc.displayMode = UIPasteControlDisplayModeIconOnly; pc.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
        pc.baseBackgroundColor = duo_rgb(DUO_FIELD); pc.baseForegroundColor = navy;
        _paste = [[UIPasteControl alloc] initWithConfiguration:pc]; _paste.target = _field; [self addSubview:_paste];
        _ok = [DuoButton new]; _ok.key.faceColor = orange; _ok.key.lipColor = duo_rgb(DUO_ORANGE_LIP);
        [_ok addTarget:self action:@selector(confirm) forControlEvents:UIControlEventTouchUpInside]; [self addSubview:_ok];
        _cancel = [DuoButton new];
        [_cancel addTarget:self action:@selector(cancel) forControlEvents:UIControlEventTouchUpInside]; [self addSubview:_cancel];
        _cancel.accessibilityLabel = zh() ? @"取消" : @"Cancel";
        [self edited];
    }
    return self;
}
static CGFloat bar_u(CGFloat w) { return MIN(MAX(w / 626.0, 0.85), 1.3); }   // 以 626 點寬為 1；外螢幕上不再縮小，字才看得清 | 626 points wide = 1; no smaller on the outer display, so the text stays readable
- (CGFloat)heightForWidth:(CGFloat)w { return (_sub.hidden ? 124 : 144) * bar_u(w); }
- (CGSize)sizeThatFits:(CGSize)s { return CGSizeMake(s.width, [self heightForWidth:s.width]); }
- (void)layoutSubviews {
    [super layoutSubviews];
    [CATransaction begin]; [CATransaction setDisableActions:YES];   // 圖層跟著視圖一起到位，不各自補間 | layers land together with the views instead of animating on their own
    CGFloat w = self.bounds.size.width, u = bar_u(w), pad = 18 * u, L = pad, R = pad, y = 14 * u, kh = 52 * u, depth = 3 * u, sc = self.traitCollection.displayScale;
    // 內容讓開壓在列上的相機（iOS 27.1 的 occlusion 保留區；外螢幕橫拿、列貼在最底時相機在右下），底色照樣鋪滿整寬。
    // 不用左右安全區：外螢幕直拿時右邊整條都算安全區，照算會把列擠窄
    // The contents keep clear of a camera over the bar (the iOS 27.1 occlusion reserved region; on the outer display held sideways with the bar at the very bottom, the camera is bottom right); the background still spans the full width.
    // Not the side safe areas: on the outer display held upright the whole right strip counts as safe area, which would squeeze the bar
    if (@available(iOS 27.1, *)) {
        for (UIViewReservedRegion *r in [self reservedRegionsOfKind:UIViewReservedRegionKind.occlusionRegionKind]) {
            CGRect f = CGRectIntersection(r.frame, self.bounds); if (CGRectIsEmpty(f)) continue;
            if (CGRectGetMidX(f) > w / 2) R = MAX(R, w - CGRectGetMinX(f) + 8 * u); else L = MAX(L, CGRectGetMaxX(f) + 8 * u);
        }
    }
    UIColor *navy = duo_rgb(DUO_NAVY);
    _bg.frame = self.bounds;
    CGFloat r = 26 * u; UIBezierPath *bp = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, w, self.bounds.size.height + r) byRoundingCorners:UIRectCornerTopLeft | UIRectCornerTopRight cornerRadii:CGSizeMake(r, r)];
    _bg.path = bp.CGPath; _bg.shadowPath = bp.CGPath;   // 下緣伸出框外一截，貼著摺痕看不到圓角 | the bottom reaches past the frame, so no corners show against the fold
    _title.font = duo_font(19 * u, UIFontWeightHeavy); _count.font = [UIFont monospacedDigitSystemFontOfSize:16 * u weight:UIFontWeightBold];
    _title.frame = CGRectMake(L + 4 * u, y, w - L - R - 100 * u, 26 * u); _count.frame = CGRectMake(w - R - 100 * u, y, 96 * u, 26 * u); y += 30 * u;
    if (!_sub.hidden) { _sub.font = duo_font(15 * u, UIFontWeightSemibold); _sub.frame = CGRectMake(L + 4 * u, y, w - L - R, 20 * u); y += 22 * u; }
    y += 4 * u;
    // 第二行：✕、輸入框、確定，同高 | second row: ✕, field, OK, all one height
    _cancel.key.depth = depth; _cancel.frame = CGRectMake(L, y, kh, kh + 2 * depth);
    [_cancel.key setSymbol:@"xmark" size:17 * u color:navy];
    CGFloat okW = 118 * u;
    _ok.key.depth = depth; _ok.frame = CGRectMake(w - R - okW, y, okW, kh + 2 * depth);
    [_ok.key setText:zh() ? @"確定" : @"OK" font:duo_font(21 * u, UIFontWeightHeavy) color:UIColor.whiteColor];
    CGFloat fx = L + kh + 10 * u, fw = w - fx - okW - R - 10 * u;
    _fieldKey.contentsScale = sc; _fieldKey.depth = depth; _fieldKey.frame = CGRectMake(fx, y, fw, kh);
    CGPathRef fp = CGPathCreateWithRoundedRect(CGRectMake(0, 0, fw, kh), kh / 2, kh / 2, NULL); [_fieldKey setShape:fp]; CGPathRelease(fp);
    CGFloat ps = MAX(kh - 14 * u, 36);   // 貼上鍵是系統的安全控件，太小就不顯示也不能按 | Paste is a system secure control: too small and it neither shows nor works
    _paste.frame = CGRectMake(fx + fw - ps - (kh - ps) / 2, y + (kh - ps) / 2, ps, ps);
    _field.rightRoom = ps; _field.frame = CGRectMake(fx, y, fw, kh); _field.font = duo_font(22 * u, UIFontWeightBold);
    [CATransaction commit];
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (void)begin { [_field becomeFirstResponder]; }

- (void)edited {   // 組字結束後超出上限就截掉，不截半個字 | once composition ends, cut what is over the limit without splitting a character
    NSString *t = _field.text ?: @"";
    if (_max > 0 && !_field.markedTextRange && (int)t.length > _max) {
        NSUInteger cut = (NSUInteger)_max; NSRange last = [t rangeOfComposedCharacterSequenceAtIndex:cut - 1];
        if (NSMaxRange(last) > cut) cut = last.location;   // 最後一個字跨過上限就整個不要 | drop the last character entirely when it crosses the limit
        _field.text = t = [t substringToIndex:cut];
    }
    int n = (int)t.length;
    _count.text = [NSString stringWithFormat:@"%d / %d", n, _max];
    _count.textColor = n >= _max ? duo_rgb(DUO_ORANGE) : [duo_rgb(DUO_NAVY) colorWithAlphaComponent:0.5];
}
- (BOOL)textField:(UITextField *)f shouldChangeCharactersInRange:(NSRange)r replacementString:(NSString *)s {
    if (_max <= 0 || f.markedTextRange || !s.length) return YES;   // 組字中、刪字一律放行 | always allow during composition and for deletions
    return (int)(f.text.length - r.length) < _max;                    // 已滿就不再收（貼上等多出的由 edited 截） | refuse once full (anything extra from a paste is cut in edited)
}
- (BOOL)textFieldShouldReturn:(UITextField *)f { [self confirm]; return NO; }
- (void)finish:(BOOL)ok {
    if (!_done) return;
    void (^d)(BOOL, NSString *) = _done; _done = nil;
    [_field resignFirstResponder];
    d(ok, _field.text ?: @"");
}
- (void)confirm { [self finish:YES]; }
- (void)cancel { [self finish:NO]; }
@end
