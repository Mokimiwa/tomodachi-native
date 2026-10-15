# libdeflate（第三方，MIT 許可證，見 COPYING）
- 上游：https://github.com/ebiggers/libdeflate  提交 92e6a0d（2026-08-22）
- 用途：TACT 存儲 BLTE 塊的 zlib 解碼（宿主 hle_tact.c，經剪口在 main 0x1b0ee60 入口接管）
- 平台線 10-01 離線對拍：選角前真被解壓的 4,979 個文件全部 MD5 == 清單 CKey；同機比遊戲自帶 zlib 1.2.11 快 2.42×
- 只編解壓相關源：lib/deflate_decompress.c、lib/zlib_decompress.c、lib/adler32.c、lib/utils.c、lib/arm/cpu_features.c
- 未改動上游代碼
