// Horizon 內核系統調用（裝 SDK 後由 SDK 運行時觸發）。只實現 P1 啓動鏈要的：內存佈局 + 堆 + 查詢 + 調試輸出 + 計時。
// 寄存器約定：入參 w1.. / 出參 w0=Result、x1=值（GetInfo / SetHeapSize），與公開 Horizon ABI 一致。
// Horizon kernel system calls (triggered by the SDK runtime once the SDK is loaded). Only what the P1 boot chain needs is implemented: memory layout + heap + query + debug output + timing.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI.
#include "dm.h"
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>

// 預留一整塊客體地址空間，按 Horizon 分區（地址值直接用宿主 mmap 得到的真地址）。只佔地址、不佔內存。
// 大小 5 GB：遊戲要的是開頭 1 GB（別名區、棧）+ 一整塊堆（應用內存池 3285 MiB，見下面 APP_POOL），合計約 4.2 GB；
// iOS 上不帶「擴展虛擬尋址」權限時一次最多能預留 6 GB（真機實讀：6 GB 成、7 GB 不成），所以不能留得更寬——
// 留到 8 GB 的話，簽不了那項權限的開發者賬號（免費賬號）出的包在這一步就退出。
// Reserve one whole block of guest address space, partitioned the Horizon way (address values are the real addresses returned by the host's mmap).
// It only takes address space, not memory.
// Size 5 GB: the game needs the first 1 GB (alias region, stacks) + one whole heap block (application memory pool of 3285 MiB, see APP_POOL below),
// about 4.2 GB in total; on iOS without the "extended virtual addressing" entitlement a single reservation can be at most 6 GB
// (read on a real device: 6 GB succeeds, 7 GB fails), so it cannot be made any wider --
// with an 8 GB reservation, builds from developer accounts that cannot sign that entitlement (free accounts) would exit at this step.
#define RESERVE DM_GAME_MEM_RESERVE   // 遊戲檔案 mem_reserve。SMBW：堆只要 1.47 GB，整塊與 D2R 一樣 5 GB（真機已跑通的總量；6 GB 單塊雖能預留，但還要給 Metal 等留地址） | SMBW: the heap needs only 1.47 GB, so the block stays at 5 GB like D2R (the total proven on a real device; a single 6 GB block can be reserved, but Metal etc. still need address space)
static uint8_t *g_as;              // ASLR 區基址 = 整塊 | ASLR region base = the whole block
static uint8_t *g_heap; static size_t g_heap_cur, g_heap_cap;
// 地址段整個落在客體堆當前已開放（可讀寫）的範圍內：這種讀不用陷內核核對，直接 memcpy（diag.c tk_rd 等每拍讀客體內存的地方用）
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap: such reads need no kernel trap to check, memcpy directly (used where guest memory is read every tick, such as diag.c tk_rd)
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
static uint8_t *g_alias; static size_t g_alias_cap;
static _Atomic uint64_t *g_alias_bits;   // 別名區頁位圖（alias_pages） | alias region page bitmap (alias_pages)
static uint8_t *g_stack; static size_t g_stack_cap;

static void mem_init(void) {
    if (g_as) return;
    g_as = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest AS");
    // 固定劃分（都在預留塊內）：alias 2.875 GB、stack 256 MB、heap 其餘 1.625 GB。
    // 較新 SDK 的 AddressRegionManager 在別名區開頭固定佔 0x3fe00000（約 1 GB）作節點區（sdk 0x134728），之後才給遊戲分區域；別名區只有 512 MB 時第一次 AllocateAddressRegion 就失敗（0x1803）。
    // SMBW 還在 HeapDef（System/HeapSize）裡定了 1792000 KB（1.75 GB）的 VirtualAddressMemory：開堆時先從內存池裡扣掉它（SetHeapSize 只要 0x5be00000），
    //   資源加載再按需 MapPhysicalMemory 到別名區節點區之後。別名區只給 1.25 GB 時那裡只剩約 256 MB，3 MB 級的佈局歸檔（WipeCourseIn / WipeWarp）
    //   分不到地址就被跳過，用到時空指針崩（main +0x373e04）。所以別名區 = 節點區 + 1.75 GB + 餘量；堆只要 1.47 GB，給 1.625 GB 夠用。
    // Fixed partition (all inside the reserved block): alias 2.875 GB, stack 256 MB, heap the remaining 1.625 GB.
    // In newer SDKs the AddressRegionManager always takes 0x3fe00000 (~1 GB) at the start of the alias region as its node area (sdk 0x134728) before handing out regions; with a 512 MB alias region the very first AllocateAddressRegion fails (0x1803).
    // SMBW also defines a 1792000 KB (1.75 GB) VirtualAddressMemory in HeapDef (System/HeapSize): it is subtracted from the memory pool before the heap is opened (SetHeapSize asks for only 0x5be00000),
    //   and resource loading then MapPhysicalMemory's into the alias region after the node area on demand. With a 1.25 GB alias region only about 256 MB was left there, and 3 MB-class layout archives (WipeCourseIn / WipeWarp)
    //   got no address, were skipped, and crashed on a null pointer when used (main +0x373e04). So alias = node area + 1.75 GB + slack; the heap needs only 1.47 GB, and 2.625 GB is plenty.
    // 別名區與棧區的大小取自遊戲檔案（mem_alias / mem_stack），堆佔剩下的；馬力歐：別名 0xB8000000、棧 0x10000000 → 棧在 +0xC8000000、堆在 +0xD8000000
    // The alias and stack sizes come from the game profile (mem_alias / mem_stack), the heap takes the rest; Mario: alias 0xB8000000, stack 0x10000000 → stack at +0xC8000000, heap at +0xD8000000
    g_alias = g_as + 0x10000000; g_alias_cap = DM_GAME_MEM_ALIAS;
    g_stack = g_alias + g_alias_cap; g_stack_cap = DM_GAME_MEM_STACK;
    g_heap  = g_stack + g_stack_cap; g_heap_cap  = RESERVE - (size_t)(g_heap - g_as);
    if (!(g_alias_bits = calloc((g_alias_cap >> 18) + 1, sizeof *g_alias_bits))) die("別名區頁位圖 calloc");
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p", g_as, (unsigned long long)RESERVE, g_heap, g_alias, g_stack);
}

// 應用內存池 = Switch（4 GB 機型）給應用的 3285 MB。遊戲啓動（main+0x1b66160）把 QueryMemoryInfo 報的總量整塊設成堆、失敗就減 2 MB 重試，
// 整塊交給自己的 StandardAllocator（main+0x32137a0）——所以堆大小 = 池 − 映像佔用，宿主報多少它就只有多少。
// 之前這裡報 3 GB 且 SetHeapSize 從不失敗：堆比真機少約 150 MB，進局高峰主堆只剩約 18 MB，本機負載高時時序一偏申請 128 KB 返回空、
// 客體不判空直接寫（CRASH main+0x1b36138，合入後復核 inv_tap）。
// Application memory pool = the 3285 MB the Switch (4 GB model) gives applications. At startup (main+0x1b66160) the game sets the whole total reported by QueryMemoryInfo as the heap, retrying with 2 MB less on failure,
// and hands the whole block to its own StandardAllocator (main+0x32137a0) -- so heap size = pool − image footprint, it has only as much as the host reports.
// Previously 3 GB was reported here and SetHeapSize never failed: the heap was about 150 MB smaller than on the real device, at the run entry peak the main heap had only about 18 MB left, and under high local load a timing shift made a 128 KB request return null,
// which the guest writes to without a null check (CRASH main+0x1b36138, re-verified after merge with inv_tap).
#define APP_POOL DM_GAME_APP_POOL   // 遊戲檔案 app_pool（馬力歐 0xCD500000） | game profile app_pool (Mario 0xCD500000)
// 報給遊戲的「已用」= 堆 + 這個固定量（代碼映像、棧等）；SetHeapSize 的池上限按同一個量判，兩邊一致
// The "used" figure reported to the game = heap + this fixed amount (code images, stacks and so on); SetHeapSize's pool limit is judged by the same amount, so both sides agree
#define USED_BASE 0x4000000ull

// 內存賬（memstat.m）用：客體地址空間整塊範圍（未初始化時 base = NULL）
// For memory accounting (memstat.m): the range of the whole guest address space block (base = NULL when not initialized)
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = RESERVE; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23 };

static uint8_t g_seen[256];

// 已映射區間登記（MapMemory 棧、MapPhysicalMemory alias 頁），QueryMemory 據此回報；SDK 靠它找空位
// Registry of mapped ranges (MapMemory stacks, MapPhysicalMemory alias pages), QueryMemory reports from it; the SDK relies on it to find free space
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {       // 返回 1 基下標，0 = 無重疊 | returns a 1-based index, 0 = no overlap
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {
    pthread_mutex_lock(&g_map_lock);
    if (g_nmap >= MAX_MAP) die("映射表滿");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
    pthread_mutex_unlock(&g_map_lock);
}
static void map_del(int i) { pthread_mutex_lock(&g_map_lock); g_map[i] = g_map[--g_nmap]; pthread_mutex_unlock(&g_map_lock); }

// 別名區每個 4 KB 頁一位（已映射 = 1）：Map 置位、Unmap 清位，返回狀態真變了的頁數（帶符號：Map 為正、Unmap 為負）
// One bit per 4 KB page of the alias region (mapped = 1): Map sets bits, Unmap clears them; returns the number of pages whose state really changed (signed: positive for Map, negative for Unmap)
static int64_t alias_pages(uint64_t off, uint64_t n, int map) {
    int64_t ch = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        uint64_t old = map ? atomic_fetch_or(&g_alias_bits[p >> 6], m) : atomic_fetch_and(&g_alias_bits[p >> 6], ~m);
        ch += __builtin_popcountll((map ? ~old : old) & m);
        p += k;
    }
    return map ? ch : -ch;
}

int svc_thread(int num, uint64_t *regs);   // hle_thread.c：線程與同步類 | hle_thread.c: thread and synchronization kind

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    { void hlecount_svc(int); hlecount_svc(num); }   // DM_HLECOUNT：svc 按號計數 | DM_HLECOUNT: svc counts by number
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    { uint64_t svc_host_call(int, uint64_t *); if (num >= 0x80 && svc_host_call(num, regs)) return regs[0]; }   // 0x80 起：剪口約定的宿主調用（hle_tact.c） | from 0x80: host calls by cut convention (hle_tact.c)
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD401; return 0; }
        // 超出應用內存池：ResultLimitReached。遊戲啓動時按 2 MB 遞減重試直到放得下（main+0x1b66184）。
        // 按報給遊戲的同一本賬判（USED_BASE，見 I_UsedMem）：朋友收集的 nninitStartup（main+0x854744）按 總量 − 已用 設堆、不重試，映像 98 MB 超過 64 MB，按映像判就被拒，
        // 隨後 AllocateMemoryBlock 失敗、讀了棧上沒寫過的指針（CRASH sdk+0x582b00）。馬力歐映像小於 64 MB，首次請求兩種判法都過
        // Exceeds the application memory pool: ResultLimitReached. At startup the game retries in 2 MB decrements until it fits (main+0x1b66184).
        // Judged by the same books reported to the game (USED_BASE, see I_UsedMem): Tomodachi Life's nninitStartup (main+0x854744) sets the heap to total − used without retrying, and its 98 MB of images exceed 64 MB, so judging by the images rejected it;
        // AllocateMemoryBlock then failed and the game read a pointer from the stack that was never written (CRASH sdk+0x582b00). Mario's images are under 64 MB, so its first request passes either way
        if (size + USED_BASE > APP_POOL) { regs[0] = 0x10801; return 0; }
        int rc = 0;
        if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) rc = mprotect(g_heap + size, g_heap_cur - size, PROT_NONE);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        else if (size > g_heap_cur) LOG("[內存] 堆 %.2f GB（堆區 %.2f GB）", size / 1073741824.0, g_heap_cap / 1073741824.0);
        g_heap_cur = size;
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: regs[0] = 0; return 0;   // SetMemoryPermission
    case 0x03: regs[0] = 0; return 0;   // SetMemoryAttribute
    case 0x04: {  // MapMemory(dst, src, size)：SDK 建線程時把堆上的棧搬進棧區 | MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        if ((uint8_t *)dst < g_stack || (uint8_t *)dst + size > g_stack + g_stack_cap || map_find(dst, size)) {
            LOG("MapMemory 目標非法或已佔用 %#llx+%#llx", dst, size); regs[0] = 0xDC01; return 0; }
        // 蘋果頁 16K、Horizon 頁 4K：可訪問區擴到 16K 邊界（多放開的是同區未用頁，不影響語義）
        // Apple pages are 16K, Horizon pages 4K: the accessible region is extended to a 16K boundary (the extra pages opened are unused pages of the same region, semantics unaffected)
        uint64_t lo = dst & ~0x3FFFull, hi = (dst + size + 0x3FFF) & ~0x3FFFull;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        memcpy((void *)dst, (void *)src, size);   // 搬移語義：映射期間源區不被訪問（Horizon 把它設為不可訪問） | move semantics: the source region is not accessed while mapped (Horizon makes it inaccessible)
        map_add(dst, size, src, 0x0B /*MappedMemory*/);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size)：內容搬回源區；16K 頁可能與相鄰映射共用，不收回權限 | UnmapMemory(dst, src, size): contents are moved back to the source region; a 16K page may be shared with an adjacent mapping, permissions are not revoked
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            LOG("UnmapMemory 與映射不符 %#llx+%#llx", dst, size); regs[0] = 0xD401; return 0; }
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        int k;
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (a >= g_as && a < g_as + RESERVE) {   // 空閒：到相鄰映射/堆邊界為止 | free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + RESERVE;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else if (module_of(a)) {
            // 已裝模塊：照 Horizon 的佈局回報分區——代碼 [0, ro) Code+RX、只讀 [ro, data) Code+R、數據與 bss [data, 補丁區) CodeData+RW。
            // SDK 的 C++ 異常展開（nn::ro::detail::GetExceptionInfo）從 PC 起往回並「Code+RX」區找模塊首、往前並 Code 再並 CodeData 找模塊尾，再從模塊首讀 MOD0 取 .eh_frame_hdr。
            // 原先回報「所在 4 KiB 頁、Code+RX」：往回一直並到別的內存，讀不到 MOD0 就中止（Engage 的 IL2CPP 每次托管異常都拋 C++ 異常）。
            // 改寫器的補丁區報 CodeData+RX：模塊在宿主裡首尾相接，補丁區（可執行）若報 Code+RX，往回找模塊首會並進前一個模塊
            // Loaded module: report the regions in Horizon's layout -- code [0, ro) Code+RX, read-only [ro, data) Code+R, data and bss [data, patch area) CodeData+RW.
            // The SDK's C++ exception unwinding (nn::ro::detail::GetExceptionInfo) walks back from the PC over "Code+RX" regions to find the module start, forward over Code then CodeData to find its end, then reads MOD0 at the module start for .eh_frame_hdr.
            // Previously "the 4 KiB page around it, Code+RX" was reported: the backward walk ran on into other memory, found no MOD0 and aborted (Engage's IL2CPP throws a C++ exception for every managed exception).
            // The rewriter's patch area is reported as CodeData+RX: modules sit back to back in the host, and an executable patch area reported as Code+RX would merge the previous module into the backward walk
            module_t *mm = module_of(a); uint64_t off = (uint64_t)(a - mm->base), mb = (uint64_t)mm->base;
            uint64_t ro = mm->ro_off ? mm->ro_off : mm->text_end, da = mm->data_off ? mm->data_off : ro, pa = mm->patch_off ? mm->patch_off : mm->size;
            if (off < ro)      { base = mb;      size = ro;            state = 3/*Code*/;     perm = 5; }
            else if (off < da) { base = mb + ro; size = da - ro;       state = 3/*Code*/;     perm = 1; }
            else if (off < pa) { base = mb + da; size = pa - da;       state = 4/*CodeData*/; perm = 3; }
            else               { base = mb + pa; size = mm->size - pa; state = 4/*CodeData*/; perm = 5; }
        }
        else {
            // 地址落在「當前宿主線程的棧」裡：回報整段棧（狀態 0xB = 棧）。SDK 初始化主線程記錄時拿當時的 sp 查這裡，把回報的區間當主線程棧；
            // 之後 nn::os::GetCurrentStackInfo 會斷言 sp 仍在那個區間裡，不在就中止。原先對客體地址空間之外的地址一律回報「它所在的 4 KiB 頁」，
            // 主線程棧於是被記成一頁——初始化時的 sp 與後來的 sp 是否同頁，取決於宿主棧頂的絕對位置，而它隨環境變量 / 啓動參數總長移動（換個環境偶發啓動即中止）。
            // 暗黑這一版不調 GetCurrentStackInfo（main / subsdk0 沒有導入它，SDK 內部也沒有調用點；宿主墊 12 檔環境變量長度都起得到主菜單），所以沒踩到；回報「一頁」本身是錯的，照改。
            // The address lies in "the current host thread's stack": report the whole stack (state 0xB = stack). When the SDK initializes the main thread record it queries here with the sp at that moment and takes the reported range as the main thread stack;
            // later nn::os::GetCurrentStackInfo asserts that sp is still inside that range and aborts if not. Previously every address outside the guest address space was reported as "the 4 KiB page containing it",
            // so the main thread stack was recorded as one page. Whether the sp at init and a later sp share a page depends on the absolute position of the host stack top, which moves with the total length of environment variables / launch arguments (a different environment occasionally aborts at startup).
            // This Diablo build does not call GetCurrentStackInfo (main / subsdk0 do not import it and the SDK has no internal call site; the host reaches the main menu with 12 different environment padding lengths), so it was never hit; reporting "one page" is wrong in itself, so fix it anyway.
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            // 其餘宿主內存：照 Horizon 對未映射地址的回報給「空閒、無權限」（所在 4 KiB 頁）。原先報 Code+RX，GetExceptionInfo 從 main 往回並區時會一路並進宿主內存直到 0
            // Other host memory: report "free, no permissions" (the 4 KiB page around it) as Horizon does for unmapped addresses. It used to report Code+RX, and GetExceptionInfo walking back from main merged on through host memory down to 0
            else { base=addr & ~0xFFFull; size=0x1000; state=0/*Free*/; perm=0; } }
        if (mi) { uint64_t *m=(uint64_t*)mi; m[0]=base; m[1]=size; ((uint32_t*)mi)[4]=state; ((uint32_t*)mi)[5]=0; ((uint32_t*)mi)[6]=perm; ((uint32_t*)mi)[7]=0; ((uint32_t*)mi)[8]=0; }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size)：只許在 alias 區內（SDK 的 VammManager 用） | MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uint8_t *a = (uint8_t *)regs[0]; size_t size = regs[1];
        if (a < g_alias || a + size > g_alias + g_alias_cap) { LOG("SVC %#x 越出 alias 區 %p+%#zx", num, a, size); regs[0] = 0xD401; return 0; }
        // 宿主頁 16 KB、Horizon 頁 4 KB：映射向外取整到宿主頁（多開的相鄰 4K 頁在別名區內，只是提前可寫，無害）；
        // 解除只收回整頁都落在區間裡的宿主頁（部分覆蓋的頁可能還有相鄰映射在用，保持可寫）
        // Host pages are 16 KB, Horizon pages 4 KB: a map is rounded out to host pages (the extra neighboring 4K pages are inside the alias region and merely become writable early, harmless);
        // an unmap only reclaims host pages lying entirely inside the range (a partly covered page may still be used by an adjacent mapping, so it stays writable)
        uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)a, hi = (uintptr_t)a + size;
        uintptr_t mlo = num == 0x2C ? lo & ~(pg - 1) : (lo + pg - 1) & ~(pg - 1), mhi = num == 0x2C ? (hi + pg - 1) & ~(pg - 1) : hi & ~(pg - 1);
        int rc = 0;
        if (mhi > mlo) rc = num == 0x2C ? mprotect((void *)mlo, mhi - mlo, PROT_READ | PROT_WRITE)
                                        : (madvise((void *)mlo, mhi - mlo, MADV_FREE), mprotect((void *)mlo, mhi - mlo, PROT_NONE));
        if (rc) die("SVC %#x mprotect %p+%#zx errno=%d", num, a, size, errno);
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", num == 0x2C ? "Map" : "Unmap", a, size);
        // 別名區用到多深（每過 256 MB 記一行）：定預留塊與別名區大小用（iOS 單塊預留上限 6 GB） | how deep the alias region is used (one line per 256 MB crossed): for sizing the reservation and alias region (iOS caps a single reservation at 6 GB)
        if (num == 0x2C) { static _Atomic uint64_t hw; uint64_t end = (uint64_t)(a + size - g_alias), old = atomic_load(&hw);
            while (end > old && !atomic_compare_exchange_weak(&hw, &old, end)) {}
            if (end > old && end >> 28 != old >> 28) LOG("[內存] 別名區映射到 +%.2f GB（別名區 %.2f GB）", end / 1073741824.0, g_alias_cap / 1073741824.0); }
        // 別名區實際映射的總量（每跨 256 MB 記一行，漲跌都記）：看遊戲用了多少物理內存、有沒有一直漲。按頁位圖計，只數狀態真變了的頁——
        // SDK 會對已映射的頁再 Map、對比映射更大的範圍 Unmap（Engage 先解 0x1000000 再解同址 0x103f000），按請求大小計會算成負數
        // Total actually mapped in the alias region (one line per 256 MB crossed, up or down): how much physical memory the game uses and whether it keeps growing. Counted with a page bitmap, only pages whose state really changed --
        // the SDK maps pages that are already mapped and unmaps ranges larger than what it mapped (Engage unmaps 0x1000000 and then 0x103f000 at the same address), so counting requested sizes went negative
        { static _Atomic int64_t tot; int64_t d = alias_pages((uint64_t)(a - g_alias), size, num == 0x2C) << 12, before = atomic_fetch_add(&tot, d), after = before + d;
          if (before >> 28 != after >> 28) LOG("[內存] 別名區已映射 %.2f GB（應用池 %.2f GB）", after / 1073741824.0, APP_POOL / 1073741824.0); }
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size)：SDK 致命中止 | Break(reason, ptr, size): SDK fatal abort
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc 位於 %s+%#llx", ++nb, regs[0], res, mm ? mm->name : "?", orig_addr);
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // 僅通知，可繼續 | notification only, can continue
        _exit(42);
    }
    case 0x1E: regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = RESERVE;     break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = APP_POOL; break;
        case I_UsedMem:  case I_UsedNonSys:  v = g_heap_cur + USED_BASE; break;
        case I_SysResTotal: v = 0x2000000; break; case I_SysResUsed: v = 0x400000; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t dm_entropy64(void); v = dm_entropy64(); break; }   // DM_DETCLOCK 開時為確定流（hle_os.c） | a deterministic stream when DM_DETCLOCK is on (hle_os.c)
        case I_ProgramId: { static uint64_t pid; if (!pid) { const char *e = getenv("DM_PROGRAM_ID"); pid = e ? strtoull(e, NULL, 16) : DM_GAME_PROGRAM_ID; } v = pid; break; }   // 缺省取遊戲檔案 program_id；DM_PROGRAM_ID=十六進制 可改 | default from the game profile's program_id; DM_PROGRAM_ID=hex overrides
        case I_IsApplication: v = 1; break;
        case I_UserExcAddr: { static _Alignas(0x1000) uint8_t ctx[0x1000]; v = (uint64_t)ctx; break; }   // 用戶異常上下文區：較新 SDK 的 SetDyingMessageRegion 往 +0x1c0 寫（給 0 會寫空指針）| user exception context area: newer SDKs' SetDyingMessageRegion writes to +0x1c0 (0 would write through a null pointer)
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx (佔位返回成功)", num, regs[0], regs[1], regs[2]); }
        regs[0] = 0; return 0;
    }
}
