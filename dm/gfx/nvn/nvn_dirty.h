// 客體內存寫保護髒頁跟蹤（nvn_dirty.c）
// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // 登記一個內存池為跟蹤區間（首次調用時註冊缺頁鈎子與宿主寫前鈎子） | Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // 在冊的內存池數、峰值、跟蹤表容量（內存讀數行用） | number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // 池銷毀：受保護頁還原可寫並標失效 | Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // 覆蓋頁設只讀 | Set the covered pages read-only
uint64_t nvn_dirty_now(void);                                      // 當前代號：上傳前取，作為該份內容的戳 | Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp); // 覆蓋頁在戳之後被寫過 → 1 | Covered pages were written after the stamp -> 1
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // 累計：遊戲寫觸發的缺頁次數、宿主寫前解保護的頁數 | Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // 累計：本模塊 mprotect 調用次數、覆蓋頁數、耗時 | Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // 當前存活槽、槽表高水位、登記總次數 | Currently live slots, slot table high-water mark, total number of registrations
