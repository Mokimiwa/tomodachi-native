# libvpx (VP9 decoder subset)

The VP9 decoder from libvpx v1.17.0 (upstream commit `6df3ec3`, 2026-08-20), unmodified. Upstream: <https://chromium.googlesource.com/webm/libvpx> (mirror <https://github.com/webmproject/libvpx>).
License: BSD 3-Clause, see `LICENSE`; the patent grant is in `PATENTS`, the authors in `AUTHORS`.
The host decodes the games' VP9 movies with it (`dm/host/hle_movie.c`, Fire Emblem Engage's cutscenes). `dm/host/Makefile` builds every `.c` here into `gen/vpx/libvpx.a`; the iOS build compiles the same files (`dm/ios/tools/gen_build.py` reads them from the Makefile).

Only the files that build needs are kept: the 71 sources libvpx's own build compiles for this configuration, and the headers they include. `config/` holds what `configure` generated (`vpx_config.h`, `vpx_version.h`, the `*_rtcd.h` dispatch headers). One arm64 configuration serves Mac and iOS; NEON is used directly, with no run-time CPU detection. To regenerate, run in an empty directory:

```
<libvpx>/configure --target=arm64-darwin24-gcc --disable-vp8 --enable-vp9-decoder --disable-vp9-encoder \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-runtime-cpu-detect \
  --disable-neon-dotprod --disable-neon-i8mm --disable-sve --disable-sve2 --disable-webm-io --disable-libyuv \
  --disable-install-docs --disable-install-bins
make libvpx.a V=1
```

The `-c` lines of that build give the source list (without the generated `vpx_config.c`), and the `.d` files give the headers.

libvpx v1.17.0 的 VP9 解碼器（上游提交 `6df3ec3`，2026-08-20），未做修改。許可證 BSD 3-Clause，見 `LICENSE`；專利授權在 `PATENTS`，作者在 `AUTHORS`。
宿主用它解遊戲的 VP9 影片（`dm/host/hle_movie.c`，火焰紋章 Engage 的過場動畫）。`dm/host/Makefile` 把這裡所有 `.c` 編成 `gen/vpx/libvpx.a`；iOS 構建編同一批文件（`dm/ios/tools/gen_build.py` 從 Makefile 取）。
只留這套構建要的文件：libvpx 自己的構建在上面的配置下編的 71 個源文件和它們包含的頭文件。`config/` 是 `configure` 生成的（`vpx_config.h`、`vpx_version.h`、各 `*_rtcd.h` 分派頭）。Mac 與 iOS 共用一套 arm64 配置，直接用 NEON，不做運行時 CPU 檢測。重新生成：在空目錄裡跑上面的命令，源文件清單取構建的 `-c` 行（去掉生成的 `vpx_config.c`），頭文件取 `.d` 文件。
