DM_ROOT ?= $(HOME)/dm-work
# 遊戲檔案（dm/games/<名>/）：宿主按它編（make GAME=基礎版標題號，必給，沒有缺省遊戲）。game_profile.h 由 game.tsv 生成在 gen/<名>/，dmsh_chain.h 在檔案目錄
# Game profile (dm/games/<id>/): the host is built for it (make GAME=base game title ID, required: there is no default game). game_profile.h is generated from game.tsv into gen/<id>/, dmsh_chain.h lives in the profile folder
# 沒設 GAME 時只有 clean 和不點名目標（缺省目標只編 libvpx）放行，其餘報錯並列出檔案 | without GAME only clean and a bare make (whose default goal only builds libvpx) pass; anything else stops and lists the profiles
ifeq ($(strip $(GAME)),)
ifneq ($(filter-out clean,$(MAKECMDGOALS)),)
$(error 沒設 GAME | GAME is not set: make GAME=<基礎版標題號 | base game title ID>: $(shell python3 ../games/game.py list | awk -F'\t' '{printf "%s (%s)  ", $$1, $$3}'))
endif
endif
GAME_DIR = ../games/$(GAME)
GAME_H = gen/$(GAME)/game_profile.h
CFLAGS=-O1 -g -Wall -Wno-unused-function
# 最低 macOS 26（圖形回放用 Metal 4）；不設的話跟著編譯機的系統版本走，新系統上編出的宿主在 26 上開不起來
# Minimum macOS 26 (graphics replay uses Metal 4); unset, it follows the build machine's OS, and a host built on a newer system would not launch on 26
export MACOSX_DEPLOYMENT_TARGET ?= 26.0
# 服務層接管 hle_*.c 按通配納入：新增文件不改這一行（免合併衝突）
# Service-layer takeover hle_*.c files are included by wildcard: adding a file does not touch this line (avoids merge conflicts)
SRC=main.c loader.c hle.c host_stubs.c crash.c probe.c thread.c memstat.m dm_bind.c memsnap.c dm_aspect.c ../ios/Sources/dm_touch_controls.c $(sort $(wildcard hle_*.c)) front_mac.m entry.S dm_i18n.c
# NVN 空設備 + nn::vi（dm/gfx/）
# 按通配納入 dm/gfx 與 dm/gfx/nvn 下的源（tests/ metal/ 不進宿主）：新增文件不改這一行
# NVN null device + nn::vi (dm/gfx/)
# Sources under dm/gfx and dm/gfx/nvn are included by wildcard (tests/ and metal/ do not go into the host): adding a file does not touch this line
GFX=$(sort $(wildcard ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m))
GFX_H=$(sort $(wildcard ../gfx/nvn/*.h ../gfx/nvn/include/*.h ../gfx/tex/*.h))
# 第三方：libdeflate 只編解壓相關源（TACT BLTE zlib 塊，hle_tact.c）
# Third party: only the decompression-related sources of libdeflate are built (TACT BLTE zlib blocks, hle_tact.c)
LIBDEFLATE=../third_party/libdeflate/lib/deflate_decompress.c ../third_party/libdeflate/lib/zlib_decompress.c ../third_party/libdeflate/lib/adler32.c ../third_party/libdeflate/lib/utils.c ../third_party/libdeflate/lib/arm/cpu_features.c
# 第三方：libvpx 只帶 VP9 解碼器的源（影片解碼，hle_movie.c），配置頭在 config/（arm64，Mac 與 iOS 共用）。單獨編成靜態庫、按文件增量編，改宿主不重編它
# Third party: libvpx ships only the VP9 decoder's sources (movie decoding, hle_movie.c), with its configuration headers in config/ (arm64, shared by Mac and iOS). Built separately into a static library, file by file, so host changes do not recompile it
VPX=../third_party/libvpx
VPX_SRC=$(sort $(shell find $(VPX) -name '*.c'))
VPX_OBJ=$(patsubst $(VPX)/%.c,gen/vpx/%.o,$(VPX_SRC))
VPX_LIB=gen/vpx/libvpx.a
gen/vpx/%.o: $(VPX)/%.c $(wildcard $(VPX)/config/*.h)
	@mkdir -p $(dir $@)
	clang -O2 -g -I$(VPX) -I$(VPX)/config -c $< -o $@
$(VPX_LIB): $(VPX_OBJ)
	rm -f $@; ar rcs $@ $^
.DEFAULT_GOAL := dmhost   # 下面先寫了表頭生成規則，缺省目標要明說（否則 make 只生成表頭、不編宿主） | The header generation rules are written first below, so the default goal must be stated explicitly (otherwise make only generates the headers and does not build the host)
# 由原版表生成的頭文件：不進倉庫，構建時從玩家自己的遊戲文件（Switch 版 3.3.94012 的 Update/RomFS）生成；路徑用環境變量 DM_ROMFS_SRC 指定
# Headers generated from the original game's tables: not checked into the repository, generated at build time from the player's own game files (Update/RomFS of Switch version 3.3.94012); the path is given by the environment variable DM_ROMFS_SRC
ROMFS?=$(or $(DM_ROMFS_SRC),$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS)
GENH=dm_vault_tab.h dm_hud_exp.h dm_bright_levels.inc
NEED_ROMFS=@test -f "$(ROMFS)/data/build_manifest.psv" || { echo "🔴 沒找到遊戲文件：$(ROMFS)/data/build_manifest.psv。把 Switch 版 3.3.94012 的 Update/RomFS 放到這裡，或用環境變量 DM_ROMFS_SRC 指過去（見 dm/README.md「從你的遊戲文件生成資源」）"; exit 1; }
dm_vault_tab.h: ../ios/tools/gen_vault_table.py
	$(NEED_ROMFS)
	python3 ../ios/tools/gen_vault_table.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
dm_hud_exp.h: ../ios/tools/gen_hud_exp.py ../ios/tools/romfs_art.py
	$(NEED_ROMFS)
	python3 ../ios/tools/gen_hud_exp.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
dm_bright_levels.inc: ../tools/gen_bright_levels.py ../ios/tools/touch_layout.py
	$(NEED_ROMFS)
	python3 ../tools/gen_bright_levels.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
.PHONY: game_profile
game_profile: $(GAME_H)
$(GAME_H): $(GAME_DIR)/game.tsv $(GAME_DIR)/shader_map.tsv ../games/game.py
	python3 ../games/game.py header $(GAME) $@
# 記下宿主是按哪個遊戲編的：換 GAME 時戳記內容變、宿主重編（否則 make 看 dmhost 比兩份表頭都新，照舊用另一個遊戲的宿主）。
# 換遊戲時順手刪掉舊宿主：時間只精確到秒的盤上，戳記與上一個遊戲的 dmhost 可能同一秒，make 會當宿主已是最新
# Records which game the host was built for: changing GAME changes the stamp's content and rebuilds the host (otherwise make sees dmhost as newer than both headers and keeps the other game's host).
# Switching games also deletes the old host: on a disk with one-second timestamps the stamp and the previous game's dmhost can share a second, and make would take the host as up to date
GAME_STAMP = gen/game.stamp
$(GAME_STAMP): FORCE
	@mkdir -p gen; echo $(GAME) | cmp -s - $@ || { echo $(GAME) > $@; rm -f dmhost; }
.PHONY: FORCE
FORCE:
dmhost: $(SRC) $(GFX) dm.h hle.h dm_input.h dm_bind.h front.h dm_aspect.h touch_mac.h ../ios/Sources/dm_touch_controls.[ch] $(GFX_H) $(GAME_H) $(GAME_DIR)/dmsh_chain.h $(GAME_STAMP) $(VPX_LIB)
	clang $(CFLAGS) -I../gfx/nvn/include -I$(dir $(GAME_H)) -I$(GAME_DIR) -I$(VPX) -fobjc-arc -o $@ $(SRC) $(GFX) $(VPX_LIB) -framework Cocoa -framework QuartzCore -framework Metal -framework MetalFX -framework AudioToolbox -framework ImageIO -framework UniformTypeIdentifiers
