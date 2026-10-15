// nn::ro（運行時裝 NRO 模塊）：真機上連 ldr:ro 服務。宿主沒有這個服務，原樣跑 SDK 的 Initialize 會在 InitializeWithPortName 裡讀空會話崩（Engage）。
// Engage（Unity）導入 8 個 ro 接口但 RomFS 裡沒有 .nro / .nrr：啓動時只 Initialize，查符號走 LookupSymbol。
// 接管：Initialize 空操作；LookupSymbol 在已裝模塊（main / subsdk* / sdk）裡按名字找定義；裝卸 / 登記模塊一律失敗並報紅一次（沒有可裝的東西）。
// 只看成敗：遊戲只判 IsSuccess，失敗碼取 ro 模塊號 22 下的一個非零值。
// nn::ro (loading NRO modules at run time): on the real device it connects to the ldr:ro service. The host has no such service; running the SDK's Initialize as is reads a null session in InitializeWithPortName and crashes (Engage).
// Engage (Unity) imports 8 ro interfaces but its RomFS has no .nro / .nrr: at startup it only calls Initialize, and symbol lookups go through LookupSymbol.
// Takeover: Initialize is a no-op; LookupSymbol finds the definition by name in the loaded modules (main / subsdk* / sdk); loading / unloading / registering modules always fails and reports red once (there is nothing to load).
// Only success matters: the game checks IsSuccess only, so the failure code is a non-zero value under the ro module number 22.
#include "hle.h"

#define RO_FAIL ((Result)(22 | (1 << 9)))   // ro 模塊 22 下的失敗 | a failure under ro module 22

HLE_FORCE("_ZN2nn2ro10InitializeEv", "連 ldr:ro 服務；宿主沒有，查符號直接查已裝模塊", Result, h_ro_Initialize, void) { LOG("init 接管：ro::Initialize（空操作）"); return 0; }

HLE_FORCE("_ZN2nn2ro12LookupSymbolEPmPKc", "按名字在已裝模塊裡找定義", Result, h_ro_LookupSymbol, uintptr_t *out, const char *name) {
    void *p = name ? guest_sym(name) : NULL;
    static int n; if (n++ < 40) LOG("ro::LookupSymbol %s → %p", name ? name : "(null)", p);
    if (!p) { static int red; if (!red++) LOG("【紅】ro::LookupSymbol 找不到 %s（首例）", name ? name : "(null)"); return RO_FAIL; }
    if (out) *out = (uintptr_t)p;
    return 0;
}

HLE_FORCE("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", "沒有裝過 NRO：按模塊查一律找不到", Result, h_ro_LookupModuleSymbol, uintptr_t *out, const void *m, const char *name) {
    (void)out; (void)m; static int red; if (!red++) LOG("【紅】ro::LookupModuleSymbol %s：沒有裝過 NRO", name ? name : "(null)"); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro13GetBufferSizeEPmPKv", "不裝 NRO", Result, h_ro_GetBufferSize, size_t *out, const void *image) {
    (void)image; if (out) *out = 0; static int red; if (!red++) LOG("【紅】ro::GetBufferSize：宿主不裝 NRO"); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", "不裝 NRO", Result, h_ro_LoadModule, void *m, const void *image, void *buf, size_t size, int flag) {
    (void)m; (void)image; (void)buf; (void)flag; static int red; if (!red++) LOG("【紅】ro::LoadModule：宿主不裝 NRO（緩衝 %#zx）", size); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", "不裝 NRO", void, h_ro_UnloadModule, void *m) { (void)m; }

HLE_FORCE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", "不登記 NRR", Result, h_ro_RegisterModuleInfo, void *info, const void *nrr) {
    (void)info; (void)nrr; static int red; if (!red++) LOG("【紅】ro::RegisterModuleInfo：宿主不登記 NRR"); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", "不登記 NRR", void, h_ro_UnregisterModuleInfo, void *info) { (void)info; }
// RELRO 改只讀：原版經 ldr:ro 把模塊的 .data.rel.ro 改成只讀（朋友收集 main 導入）。宿主的段權限裝載時已定，空操作
// Making RELRO read-only: on hardware ldr:ro changes a module's .data.rel.ro to read-only (imported by Tomodachi Life's main). The host sets segment permissions at load time, so this is a no-op
HLE_FORCE("_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_", "RELRO 改只讀；宿主裝載時已定段權限，空操作", Result, h_ro_ProtectRelro, const void *a, const void *b, const void *c, const void *d, const void *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return 0;
}
