// nninitInitializeSdkModule 調的服務層初始化：真機上各自連 Horizon 系統服務（IPC），宿主沒有這些服務進程。
// 原生接管：不建會話，直接返回；各服務的後續接口在用到時逐個接管成宿主實現（見同目錄 hle_*.c）。
// nn::os::InitializeVirtualAddressMemory 屬 SDK 的 os 層，不接管；fs::SetLocalApplicationAccessLog /
// SetResultHandledByApplication 只寫 SDK 內部開關，不接管。
// Service layer initialization called by nninitInitializeSdkModule: on the real device each connects to a Horizon system service (IPC), the host has no such service processes.
// Native takeover: create no session, return directly; each service's later interfaces are taken over one by one as host implementations when they come into use (see hle_*.c in the same directory).
// nn::os::InitializeVirtualAddressMemory belongs to the SDK's os layer and is not taken over; fs::SetLocalApplicationAccessLog /
// SetResultHandledByApplication only write SDK-internal switches and are not taken over.
#include "hle.h"

static void once(const char *what) { LOG("init 接管：%s（空操作）", what); }

HLE_FORCE("nndiagStartup", "連 lm 日誌服務；診斷日誌改由宿主直接輸出", void, h_nndiagStartup, void) { once("nndiagStartup"); }
HLE_FORCE("_ZN2nn2oe10InitializeEv", "連 am/oe 運行環境服務；運行環境查詢逐個接成宿主默認值", void, h_oe_Initialize, void) { once("oe::Initialize"); }
HLE_FORCE("_ZN2nn3aoc10InitializeEv", "追加內容(DLC)服務：不建會話（無服務進程）；已擁有清單由 hle_misc.c 的 aoc 接口給出（術士君臨，非空）", void, h_aoc_Initialize, void) { once("aoc::Initialize"); }
HLE_FORCE("_ZN2nn3err26InitializeContextWriterApiEv", "連 err 錯誤上下文服務；宿主不上報", void, h_err_InitCtx, void) { once("err::InitializeContextWriterApi"); }
HLE_FORCE("_ZN2nn2fs24InitializeForApplicationEv", "連 fsp-srv 文件服務；文件接口接宿主文件系統 + RomFS", void, h_fs_InitApp, void) { once("fs::InitializeForApplication"); }
HLE_FORCE("_ZN2nn4pctl10InitializeEv", "連 pctl 家長控制服務；單機無家長控制", void, h_pctl_Initialize, void) { once("pctl::Initialize"); }
HLE_FORCE("_ZN2nn4diag34InitializeApplicationAbortObserverEv", "註冊到系統的中止觀察；宿主崩潰由信號處理打印", void, h_diag_AbortObs, void) { once("diag::InitializeApplicationAbortObserver"); }
