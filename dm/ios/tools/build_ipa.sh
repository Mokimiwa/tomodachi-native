#!/bin/bash
# 出一個未簽名的整合 IPA：改寫後的模塊編進可執行文件、iOS 著色器庫、遊戲文件（RomFS）放在 DmIOS.app/romfs。裝機時由簽名工具（AltStore / Sideloadly 等）換成自己的證書與包名。
# 前提：已按 README 跑過改寫器（patched 目錄）與 iOS 著色器出包（IOSOUT），裝了 Xcode 與 xcodegen。
# 用法：dm/ios/tools/build_ipa.sh [輸出.ipa]；環境：GAME=遊戲檔案（dm/games/<基礎版標題號>，必給）、W=工作區（缺省 ~/<標題號>-work）、PATCHED / ROMFS / MSL_IOS 可分別指定
#   AOC=追加內容目錄（缺省 $W/data/aoc，有就放進 DmIOS.app/aoc/<序號>/；設成空串 = 不放）
#   App 名取基礎版 NSP 控制數據裡的遊戲名（$W/data/v0/control.nacp，沒有就用 RomFS 旁邊的），包名 = org.nxnative.<基礎版標題號小寫>（DM_DISPLAY_NAME / DM_BUNDLE_ID 可改）；
#   IPA 名、代碼模塊、RomFS 認定路徑、著色器表取自遊戲檔案
# Build one unsigned all-in-one IPA: the rewritten modules compiled into the executable, iOS shader libraries, and the game files (RomFS) at DmIOS.app/romfs. At install time a signing tool (AltStore / Sideloadly etc.) swaps in your own certificate and bundle ID.
# Prerequisites: the rewriter (patched directory) and the iOS shader build (IOSOUT) have been run as in the README; Xcode and xcodegen are installed.
# Usage: dm/ios/tools/build_ipa.sh [output.ipa]; environment: GAME=game profile (dm/games/<base game title ID>, required), W=workspace (default ~/<title ID>-work), PATCHED / ROMFS / MSL_IOS can each be overridden
#   AOC=add-on content directory (default $W/data/aoc; when it exists it goes to DmIOS.app/aoc/<index>/; an empty string = none)
#   The app name is the game name in the base NSP's control data ($W/data/v0/control.nacp, else the one next to the RomFS), the bundle ID is org.nxnative.<base game title ID in lowercase> (DM_DISPLAY_NAME / DM_BUNDLE_ID override them);
#   the IPA name, code modules, RomFS marker and shader tables come from the game profile
# SIGN_TEAM=<團隊 ID>：改出用自己的開發者團隊自動簽名的 .app（不打 IPA，RomFS 不進包），自己的設備用 devicectl 裝（SIGN_DEVICE=<UDID> 把那台設備註冊進團隊的描述檔）；
#   RomFS 另外拷進 App 的 Documents/romfs 一次就好（main.m 認那裡），之後每次更新 App 只傳幾百 MB。描述檔由 Xcode 自動簽名產生（-allowProvisioningUpdates，用 Xcode 裡已登入的帳號）
# SIGN_TEAM=<team ID>: build a .app automatically signed by your own developer team instead (no IPA, RomFS left out), installed on your own device with devicectl (SIGN_DEVICE=<UDID> registers that device in the team's profile);
#   the RomFS goes into the app's Documents/romfs once (main.m looks there), so later updates send only a few hundred MB. The profile comes from Xcode's automatic signing (-allowProvisioningUpdates, with the account already signed in to Xcode)
set -u
cd "$(dirname "$0")/.." || exit 1
IOS=$PWD; DM=$(cd .. && pwd)
t() { echo "[$(date +%T)] $*"; }; die() { t "FAIL: $*"; exit 1; }
[ -n "${GAME:-}" ] || die "GAME is not set: GAME=<base game title ID> $0; profiles: $(python3 "$DM/games/game.py" list | awk -F'\t' '{printf "%s (%s)  ", $1, $3}')"
export GAME; GD=$DM/games/$GAME   # gen_build.py 跑的 make 也按這個 GAME | the make that gen_build.py runs uses the same GAME
gp() { python3 "$DM/games/game.py" "$GAME" "$1" || die "game profile $GD: $1"; }   # 取遊戲檔案的鍵 | read a key of the game profile
[ -f "$GD/game.tsv" ] || die "no game profile $GD/game.tsv"
W=${W:-$HOME/$GAME-work}; PKG=$(gp package)
PATCHED=${PATCHED:-$W/build/patched_$PKG}; ROMFS=${ROMFS:-$W/data/$PKG/RomFS}; MSL_IOS=${MSL_IOS:-$W/build/ios/dmsh_b}; AOC=${AOC-$W/data/aoc}
OUT_IPA=${1:-$HOME/Desktop/$(gp ipa)}; O=$W/build/ios/pkg; DD=$O/dd; SIGN_TEAM=${SIGN_TEAM:-}
MODS=$(gp modules); MARKER=$(gp romfs_marker)
NACP=$W/data/v0/control.nacp; [ -f "$NACP" ] || NACP=$(dirname "$ROMFS")/control.nacp
[ -n "${DM_DISPLAY_NAME:-}" ] || DM_DISPLAY_NAME=$(python3 "$DM/tools/nacp.py" "$NACP") || die "no game name in $NACP (re-extract with nx_extract, or set DM_DISPLAY_NAME)"
export LC_ALL=C DM_BUNDLE_ID=${DM_BUNDLE_ID:-org.nxnative.$(echo "$GAME" | tr 'A-Z' 'a-z')} DM_DISPLAY_NAME DM_APP_CATEGORY=$(gp app_category) DM_TEAM_ID=${DM_TEAM_ID:-0000000000}   # 佔位身份：未簽名，裝機工具會替換 | placeholder identity: unsigned, the install tool replaces it
for f in $MODS; do [ -f "$PATCHED/$f.patched.bin" ] && [ -f "$PATCHED/$f.fixups.tsv" ] || die "no rewritten module $PATCHED/$f (run dm/tools/patch_nso.py first)"; done
[ -e "$ROMFS/$MARKER" ] || die "no RomFS at $ROMFS (missing $MARKER)"
V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' "$GD/dmsh_chain.h")
[ -n "$V" ] && ls "$MSL_IOS/b-$V" 2> /dev/null | grep -q '_b.metallib$' || die "no iOS shader libraries $MSL_IOS/b-$V (IOSOUT=$MSL_IOS dmsh_build.py ...)"
# 只出了一部分的庫（例如中斷過的出包）照樣能打包，但真機上大半著色器會載不了：與 Mac 出包緩存的鍵數對一下，少了就停（MSL_PARTIAL=1 照樣打）
# A partly built set (an interrupted build, say) still packages, but most shaders would then fail to load on the device: compare with the key count of the Mac build cache and stop when short (MSL_PARTIAL=1 packages anyway)
NI=$(ls "$MSL_IOS/b-$V" | grep -c '_b.metallib$'); NM=$(ls "$W/build/cache/metal_shader_cache/b-$V" 2> /dev/null | grep -c '_b.metallib$')
if [ "$NM" -eq 0 ]; then t "no Mac build cache to compare with: $NI iOS shader libraries, not checked"
else [ "$NI" -ge "$NM" ] || [ -n "${MSL_PARTIAL:-}" ] || die "iOS shader libraries incomplete: $NI of $NM in $MSL_IOS/b-$V (finish IOSOUT=... dmsh_build.py, or MSL_PARTIAL=1)"; t "iOS shader libraries: $NI (Mac cache $NM)"; fi

t "app: $DM_DISPLAY_NAME ($DM_BUNDLE_ID)"
t "generate Gen/ (host sources, embedded modules, tables, shader libraries)"
rm -rf Gen && mkdir -p Gen/dm Gen/msl || die "mkdir Gen"
python3 tools/gen_build.py ../host Gen > Gen/dm.xcconfig || die "gen_build"
F=$(python3 tools/gen_embed.py "$PATCHED" Gen/embed.S $MODS) || die "gen_embed"
echo "OTHER_LDFLAGS = \$(inherited) \$(DM_FRAMEWORKS) $F" >> Gen/dm.xcconfig
cp "$PATCHED"/*.fixups.tsv "$PATCHED"/*.gates.tsv "$PATCHED"/*.params.tsv "$PATCHED"/*.cuts.tsv "$PATCHED"/*.obs.tsv Gen/dm/ || die "copy rewriter tables"
cp "$GD/shader_map.tsv" "$GD/pso_manifest.tsv" "$GD/c1_full.tsv" Gen/dm/ || die "copy shader tables"
# 診斷包：DM_ENV_FILE=文件 → 包內 dm/env.txt（main.m 啓動時按行設環境變量） | Diagnostic build: DM_ENV_FILE=file → dm/env.txt in the bundle (main.m sets one environment variable per line at boot)
if [ -n "${DM_ENV_FILE:-}" ]; then cp "$DM_ENV_FILE" Gen/dm/env.txt || die "copy DM_ENV_FILE"; t "env: $(grep -v '^#' "$DM_ENV_FILE" | tr '\n' ' ')"; fi
cp -cR "$MSL_IOS/b-$V" Gen/msl/ || die "copy shader libraries"
# 共享字體替身：標準 = macOS 的 Arial Unicode（iOS 沒有），任天堂擴展 = make_ext_font.py 生成的按鍵圖標（需 fontTools；FONT_PY 指一個裝了它的 python）
# Shared-font stand-ins: standard = macOS's Arial Unicode (absent on iOS), Nintendo extension = button icons generated by make_ext_font.py (needs fontTools; FONT_PY names a python that has it)
mkdir -p Gen/dm/font && cp "${STD_FONT:-/System/Library/Fonts/Supplemental/Arial Unicode.ttf}" Gen/dm/font/standard.ttf || die "copy standard font (STD_FONT=a TTF)"
${FONT_PY:-python3} "$DM/tools/make_ext_font.py" Gen/dm/font/nintendo_ext.ttf > /dev/null || die "make_ext_font.py (pip install fonttools, or FONT_PY=python with fontTools)"
# App 圖標：遊戲自己的圖標（nx_extract 從控制數據存下的 icon.jpg，英文版 256×256）。DM_ICON 可另指一張圖；缺省找 RomFS 旁邊的 icon.jpg。
# 放大成 1024×1024 的 PNG 放進資源目錄 Gen/Assets.xcassets（單尺寸 App 圖標，其餘尺寸由 actool 生成）；都沒有就不設圖標，主屏圖標是空白的。
# 資源目錄總要建（工程裡總列著它，見 project.yml），沒有圖標時是空的，DM_APPICON 不設
# App icon: the game's own icon (icon.jpg saved from the control data by nx_extract, the English 256x256 one). DM_ICON can point at another image; by default the icon.jpg next to the RomFS is used.
# It is scaled to a 1024x1024 PNG in the asset catalog Gen/Assets.xcassets (a single-size app icon; actool generates the other sizes); with neither, no icon is set and the home-screen icon is blank.
# The catalog is always created (the project always lists it, see project.yml); without an icon it stays empty and DM_APPICON is not set
ICON=${DM_ICON:-$(dirname "$ROMFS")/icon.jpg}
mkdir -p Gen/Assets.xcassets && printf '{"info":{"author":"xcode","version":1}}\n' > Gen/Assets.xcassets/Contents.json || die "asset catalog"
if [ -f "$ICON" ]; then
  AI=Gen/Assets.xcassets/AppIcon.appiconset; mkdir -p "$AI" || die "mkdir $AI"
  sips -s format png -z 1024 1024 "$ICON" --out "$AI/icon.png" > /dev/null || die "sips: cannot convert the icon $ICON"
  printf '{"images":[{"filename":"icon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"xcode","version":1}}\n' > "$AI/Contents.json"
  echo "DM_APPICON = AppIcon" >> Gen/dm.xcconfig
  t "app icon: $ICON"
else t "no app icon ($ICON not found): the home-screen icon stays blank"; fi
# Duo 外殼的圖：Logo 與底部波浪從遊戲檔案取（tools/duo_assets.py，取不到 Logo 就用遊戲圖標），闔上的聲音從 DUO_LOCAL（缺省 $W/demo/preview）拷；都只在 Gen/duo，不進版本庫。
# 啓動畫面的黃（#FFC100）放進資源目錄，Info.plist 的 UILaunchScreen 用它
# Duo shell pictures: the logo and the bottom wave from the game files (tools/duo_assets.py; the game icon when there is no logo), the closing sound copied from DUO_LOCAL (default $W/demo/preview); all only in Gen/duo, never in the repository.
# The launch screen's yellow (#FFC100) goes into the asset catalog for UILaunchScreen in Info.plist
mkdir -p Gen/duo Gen/Assets.xcassets/LaunchYellow.colorset || die "mkdir Gen/duo"
DUO=$(python3 -I tools/duo_assets.py "$ROMFS" Gen/duo "$ICON") || die "duo_assets.py"; t "duo pictures: $DUO"
for f in "${DUO_LOCAL:-$W/demo/preview}"/hinge_close.*; do [ -f "$f" ] && cp "$f" Gen/duo/ && t "closing sound: $f"; done
printf '{"colors":[{"color":{"color-space":"srgb","components":{"red":"1.000","green":"0.757","blue":"0.000","alpha":"1.000"}},"idiom":"universal"}],"info":{"author":"xcode","version":1}}\n' > Gen/Assets.xcassets/LaunchYellow.colorset/Contents.json
xcodegen generate -q > /dev/null || die "xcodegen"

mkdir -p "$O"
if [ -n "$SIGN_TEAM" ]; then
  # SIGN_DEVICE=<設備 UDID>：以那台設備為目標，第一次會把它註冊進團隊的描述檔（自動簽名做的） | SIGN_DEVICE=<device UDID>: target that device, registering it in the team's profile the first time (done by automatic signing)
  DEST='generic/platform=iOS'; REG=; [ -n "${SIGN_DEVICE:-}" ] && { DEST="id=$SIGN_DEVICE"; REG=-allowProvisioningDeviceRegistration; }
  t "xcodebuild (Release, automatic signing with team $SIGN_TEAM, $DEST)"
  xcodebuild -project DmIOS.xcodeproj -scheme DmIOS -configuration Release -destination "$DEST" -derivedDataPath "$DD" -allowProvisioningUpdates $REG \
    DEVELOPMENT_TEAM="$SIGN_TEAM" CODE_SIGN_STYLE=Automatic build > "$O/build.log" 2>&1 \
    || { grep -E 'error:' "$O/build.log" | head -20; die "xcodebuild, log $O/build.log"; }
else
  t "xcodebuild (Release, unsigned, iPhone + iPad)"
  xcodebuild -project DmIOS.xcodeproj -scheme DmIOS -configuration Release -destination 'generic/platform=iOS' -derivedDataPath "$DD" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= build > "$O/build.log" 2>&1 \
    || { grep -E 'error:' "$O/build.log" | head -20; die "xcodebuild, log $O/build.log"; }
fi
# Xcode 的 Locations 設成自定義位置時，產物不進 -derivedDataPath：從本次構建日誌裡取實際路徑
# When Xcode's Locations setting is a custom location, products ignore -derivedDataPath: take the actual path from this build's log
APP=$DD/Build/Products/Release-iphoneos/DmIOS.app; [ -d "$APP" ] || APP=$(grep -o '/[^ ]*/Release-iphoneos/DmIOS\.app' "$O/build.log" | tail -1)
[ -n "$APP" ] && [ -d "$APP" ] || die "no built app"; t "app: $APP"
if [ -n "$SIGN_TEAM" ]; then
  t "signed app ready. Install: xcrun devicectl device install app --device <device> \"$APP\""
  t "game files, once: xcrun devicectl device copy to --device <device> --domain-type appDataContainer --domain-identifier $DM_BUNDLE_ID --source \"$ROMFS\" --destination Documents/romfs"
  exit 0
fi

t "assemble Payload (app + RomFS)"
rm -rf "$O/Payload" && mkdir -p "$O/Payload" && cp -cR "$APP" "$O/Payload/DmIOS.app" || die "copy app"
A=$O/Payload/DmIOS.app; rm -rf "$A/_CodeSignature" "$A/embedded.mobileprovision"
cp -cR "$ROMFS" "$A/romfs" || die "copy RomFS"
# 追加內容：玩家 DLC NSP 解出的 RomFS，按序號一個子目錄（App 啓動時設 DM_AOC_DIR，宿主據此報告已擁有並掛載）
# Add-on content: the RomFS extracted from the player's DLC NSPs, one subdirectory per index (the app sets DM_AOC_DIR at launch; the host reports them as owned and mounts them from it)
if [ -n "$AOC" ] && [ -d "$AOC" ]; then cp -cR "$AOC" "$A/aoc" || die "copy add-on content"; t "add-on content: $(ls "$AOC" | tr '\n' ' ')"; fi

t "zip (already-compressed game files stored, everything else deflated)"
rm -f "$O/out.ipa"
( cd "$O" && zip -qry -n "$(gp zip_store)" out.ipa Payload ) || die "zip"
mv "$O/out.ipa" "$OUT_IPA" || die "move ipa"
rm -rf "$O/Payload"
t "done: $OUT_IPA ($(du -h "$OUT_IPA" | cut -f1))"
