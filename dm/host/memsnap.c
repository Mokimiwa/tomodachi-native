// 診斷：記憶體快照。DM_MEMSNAP=幀號[,幀號…]:路徑前綴 —— 遊戲出到這幾幀時，把客體位址空間（堆、別名區、棧）與各模組資料段裡
// 實際用過的分頁（mincore 常駐）寫成 <前綴>_<幀號>.bin，供離線比對：例如推搖桿前後各拍一次，找出差值等於小手位移的那個游標變數。
// 檔案格式：一串記錄 { u64 位址; u64 長度; 長度位元組 }，位址是宿主位址（客體與宿主同一位址空間）。拍快照時遊戲照跑，拍之前讓輸入靜止。
// Diagnostics: memory snapshots. DM_MEMSNAP=frame[,frame...]:path prefix -- when the game reaches these frames, the pages actually in use (resident per mincore) in the
// guest address space (heap, alias region, stack) and in each module's data segment are written to <prefix>_<frame>.bin for offline comparison: e.g. one snapshot before and one after pushing the stick, to find the cursor variable whose change equals the hand's movement.
// File format: a series of records { u64 address; u64 length; length bytes }; addresses are host addresses (guest and host share one address space). The game keeps running while a snapshot is taken, so keep the input still around it.
#include "dm.h"
#include <mach/mach.h>   // 用 vm_region_64，不用 mach_vm.h（iOS SDK 不給）；64 位系統上兩者同寬 | vm_region_64 instead of mach_vm.h (the iOS SDK does not provide it); both are the same width on 64-bit systems
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

void dm_guest_as(uint8_t **base, size_t *size);
static long g_frames[16]; static int g_nframes; static char g_prefix[900];

// 只寫常駐分頁組成的連續段 | write only runs of resident pages
static size_t dump_range(FILE *f, uint8_t *a, size_t n) {
    size_t pg = (size_t)getpagesize(), np = n / pg, written = 0;
    if (!np) return 0;
    char *vec = malloc(np); if (!vec) return 0;
    if (mincore(a, np * pg, vec)) { free(vec); return 0; }
    for (size_t i = 0; i < np;) {
        if (!(vec[i] & 1)) { i++; continue; }
        size_t j = i; while (j < np && (vec[j] & 1)) j++;
        uint64_t addr = (uint64_t)(uintptr_t)(a + i * pg), len = (j - i) * pg;
        fwrite(&addr, 8, 1, f); fwrite(&len, 8, 1, f); fwrite(a + i * pg, 1, len, f); written += len;
        i = j;
    }
    free(vec);
    return written;
}
// 一個範圍裡可讀的 VM 區段逐一寫出 | write each readable VM region within a range
static size_t dump_regions(FILE *f, uint8_t *lo, uint8_t *hi) {
    size_t total = 0; vm_address_t at = (vm_address_t)(uintptr_t)lo;
    while (at < (vm_address_t)(uintptr_t)hi) {
        vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        if (vm_region_64(mach_task_self(), &at, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
        if (at >= (vm_address_t)(uintptr_t)hi) break;
        vm_address_t a0 = at, a1 = at + sz; if (a1 > (vm_address_t)(uintptr_t)hi) a1 = (vm_address_t)(uintptr_t)hi;
        if (info.protection & VM_PROT_READ) total += dump_range(f, (uint8_t *)(uintptr_t)a0, (size_t)(a1 - a0));
        at += sz;
    }
    return total;
}
static void snap(long frame) {
    char path[1024]; snprintf(path, sizeof path, "%s_%ld.bin", g_prefix, frame);
    FILE *f = fopen(path, "wb"); if (!f) { LOG("【紅】DM_MEMSNAP 寫不出 %s", path); return; }
    uint8_t *as; size_t as_n; dm_guest_as(&as, &as_n);
    uint64_t t0 = now_ns(); size_t n = as ? dump_regions(f, as, as + as_n) : 0;
    for (int i = 0; i < g_nmod; i++) {   // 模組的資料段（.data / .bss） | the modules' data segments (.data / .bss)
        module_t *m = &g_mod[i]; if (!m->base || m->data_off >= m->size) continue;
        n += dump_regions(f, m->base + m->data_off, m->base + m->size);
    }
    fclose(f);
    LOG("DM_MEMSNAP：幀 %ld 存到 %s（%.1f MB，%.1f 秒）", frame, path, n / 1048576.0, (now_ns() - t0) / 1e9);
}
static void *snap_main(void *arg) {
    (void)arg;
    for (int k = 0; k < g_nframes;) {
        if ((long)dm_nvn_frame_count() >= g_frames[k]) snap(g_frames[k++]); else usleep(2000);
    }
    return NULL;
}
__attribute__((constructor)) static void memsnap_init(void) {
    const char *e = getenv("DM_MEMSNAP"), *c = e ? strchr(e, ':') : NULL;
    if (!c) return;
    for (const char *q = e; q < c && g_nframes < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) g_frames[g_nframes++] = atol(q);
    snprintf(g_prefix, sizeof g_prefix, "%s", c + 1);
    pthread_t th; if (!pthread_create(&th, NULL, snap_main, NULL)) pthread_detach(th);
}
