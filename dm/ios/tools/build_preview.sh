#!/bin/bash
# 出 Duo 外殼的預覽 App（不帶遊戲）給模擬器：上半放一張靜態圖，其餘是真的外殼。給了模擬器就裝上去。
# 圖都在建置時取、只放在 GenPreview/（不進版本庫）：Logo 與底部波浪從遊戲檔案取（tools/duo_assets.py，取不到 Logo 就用遊戲圖標），
# 靜態圖與闔上的聲音從 DUO_LOCAL 目錄拷（picture.png 必須有；picture_type.png = 打字示範用的島名頁；其餘 picture_<名>.png 用 -DuoStill <名> 選；hinge_close.caf/wav/… 可選）。
# 用法：GAME=<基礎版標題號> W=<工作區> dm/ios/tools/build_preview.sh [模擬器 UDID 或名字]
#   DUO_LOCAL=只放本機的圖和聲音的目錄（缺省 $W/demo/preview）；輸出 $W/build/ios/preview/…/DuoPreview.app
# Build the Duo shell preview app (no game) for the simulator: a still image on top, the real shell everywhere else. With a simulator given, install it there.
# Every picture is taken at build time into GenPreview/ only (never the repository): the logo and the bottom wave from the game files (tools/duo_assets.py; the game icon when there is no logo),
# the stills and the closing sound from the DUO_LOCAL directory (picture.png is required; picture_type.png = the island-name page for the typing demo; other picture_<name>.png are picked with -DuoStill <name>; hinge_close.caf/wav/... optional).
# Usage: GAME=<base game title ID> W=<workspace> dm/ios/tools/build_preview.sh [simulator UDID or name]
#   DUO_LOCAL=directory of local-only pictures and sounds (default $W/demo/preview); output $W/build/ios/preview/.../DuoPreview.app
set -u
cd "$(dirname "$0")/.." || exit 1
DM=$(cd .. && pwd)
t() { echo "[$(date +%T)] $*"; }; die() { t "FAIL: $*"; exit 1; }
[ -n "${GAME:-}" ] || die "GAME is not set: GAME=<base game title ID> $0; profiles: $(python3 "$DM/games/game.py" list | awk -F'\t' '{printf "%s (%s)  ", $1, $3}')"
gp() { python3 "$DM/games/game.py" "$GAME" "$1" || die "game profile $GAME: $1"; }
W=${W:-$HOME/$GAME-work}; PKG=$(gp package); ROMFS=${ROMFS:-$W/data/$PKG/RomFS}; STILLS=${DUO_LOCAL:-$W/demo/preview}; O=$W/build/ios/preview
[ -f "$STILLS/picture.png" ] || die "no still image $STILLS/picture.png (a Duo-shape 1920x1350 screenshot from the Mac version)"
NACP=$W/data/v0/control.nacp; [ -f "$NACP" ] || NACP=$(dirname "$ROMFS")/control.nacp
[ -n "${DM_DISPLAY_NAME:-}" ] || DM_DISPLAY_NAME=$(python3 "$DM/tools/nacp.py" "$NACP" 2> /dev/null) || DM_DISPLAY_NAME="Duo Preview"
export DM_DISPLAY_NAME DM_BUNDLE_ID=${DM_BUNDLE_ID:-org.nxnative.$(echo "$GAME" | tr 'A-Z' 'a-z').preview}

t "generate GenPreview/ (pictures from the game files, stills, asset catalog)"
G=GenPreview; rm -rf $G && mkdir -p $G/duo || die "mkdir $G"
ICON=$(dirname "$ROMFS")/icon.jpg
DUO=$(python3 -I tools/duo_assets.py "$ROMFS" $G/duo "$ICON") || die "duo_assets.py"; t "game pictures: $DUO"
cp "$STILLS"/picture*.png $G/duo/ || die "copy stills"
for f in "$STILLS"/hinge_close.*; do [ -f "$f" ] && cp "$f" $G/duo/ && t "closing sound: $f"; done
A=$G/Assets.xcassets; mkdir -p $A/LaunchYellow.colorset && printf '{"info":{"author":"xcode","version":1}}\n' > $A/Contents.json
# 啓動畫面的黃 = 遊戲的 #FFC100 | the launch screen's yellow = the game's #FFC100
printf '{"colors":[{"color":{"color-space":"srgb","components":{"red":"1.000","green":"0.757","blue":"0.000","alpha":"1.000"}},"idiom":"universal"}],"info":{"author":"xcode","version":1}}\n' > $A/LaunchYellow.colorset/Contents.json
APPICON=
if [ -f "$ICON" ]; then
  mkdir -p $A/AppIcon.appiconset && sips -s format png -z 1024 1024 "$ICON" --out $A/AppIcon.appiconset/icon.png > /dev/null || die "sips: $ICON"
  printf '{"images":[{"filename":"icon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"xcode","version":1}}\n' > $A/AppIcon.appiconset/Contents.json
  APPICON=AppIcon
fi
xcodegen generate --spec preview.yml -q > /dev/null || die "xcodegen"

t "xcodebuild (Debug, iOS Simulator)"
mkdir -p "$O"
xcodebuild -project DuoPreview.xcodeproj -scheme DuoPreview -configuration Debug -destination 'generic/platform=iOS Simulator' -derivedDataPath "$O/dd" \
  DM_APPICON=$APPICON CODE_SIGNING_ALLOWED=NO build > "$O/build.log" 2>&1 \
  || { grep -E 'error:' "$O/build.log" | head -20; die "xcodebuild, log $O/build.log"; }
APP=$O/dd/Build/Products/Debug-iphonesimulator/DuoPreview.app; [ -d "$APP" ] || APP=$(grep -o '/[^ ]*/Debug-iphonesimulator/DuoPreview\.app' "$O/build.log" | tail -1)
[ -n "$APP" ] && [ -d "$APP" ] || die "no built app"; t "app: $APP"
if [ -n "${1:-}" ]; then
  xcrun simctl boot "$1" 2> /dev/null; xcrun simctl install "$1" "$APP" || die "simctl install on $1"
  t "installed on $1 as $DM_BUNDLE_ID (xcrun simctl launch --console-pty $1 $DM_BUNDLE_ID [-DuoDemo type])"
fi
