// nn::codec::HardwareOpusDecoder：真機經 hwopus 服務用硬件解碼（遊戲的音頻與過場音軌都是 Opus，不是聯網功能）。
// 宿主：構造函數改為在同一塊內存上構造 SDK 的軟件 OpusDecoder（sdk 自帶 libopus，ARM 原生）。
// 依據：兩者同為 nn::codec::IOpusDecoder 的實現，虛表 10 個槽位順序與簽名逐一相同（析構×2、GetWorkBufferSize、IsInitialized、
// Initialize、Finalize、GetSampleRate、GetChannelCount、Reset、DecodeInterleaved）；遊戲按 0x40 字節分配，軟件版對象 0x30 字節。
// nn::codec::HardwareOpusDecoder: the real device decodes in hardware via the hwopus service (the game's audio and cutscene tracks are all Opus; this is not an online feature).
// Host: the constructor instead constructs the SDK's software OpusDecoder in the same memory (the sdk bundles libopus, native ARM).
// Basis: both are implementations of nn::codec::IOpusDecoder, with the 10 vtable slots identical one by one in order and signature (destructor×2, GetWorkBufferSize, IsInitialized,
// Initialize, Finalize, GetSampleRate, GetChannelCount, Reset, DecodeInterleaved); the game allocates 0x40 bytes and the software object is 0x30 bytes.
#include "hle.h"

HLE_FORCE("_ZN2nn5codec19HardwareOpusDecoderC1Ei", "hwopus 服務；換成 SDK 軟件 OpusDecoder（同一 IOpusDecoder 虛接口）",
          void, h_codec_HwOpusCtor, void *self, int option) {
    (void)option;
    static void (*soft_ctor)(void *);
    if (!soft_ctor && !(soft_ctor = guest_sym("_ZN2nn5codec11OpusDecoderC1Ev"))) die("缺 OpusDecoder 構造");
    soft_ctor(self);
}
