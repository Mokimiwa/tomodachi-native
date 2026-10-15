// 單測（crash.c 備用棧隨線程釋放）：起 2000 個短命線程各調 crash_thread_init，期望堆不隨線程數增長（每線程調 2 次，各 64 KiB；修前實測洩漏 250 MiB）
// Unit test (crash.c alternate stack is freed with the thread): start 2000 short-lived threads each calling crash_thread_init, and expect the heap not to grow with the thread count (2 calls per thread, 64 KiB each; before the fix a 250 MiB leak was measured)
#include <pthread.h>
#include <stdio.h>
#include <malloc/malloc.h>
void crash_thread_init(void);
static void *body(void *a) { crash_thread_init(); crash_thread_init(); return a; }
static size_t inuse(void) { malloc_statistics_t st; malloc_zone_statistics(NULL, &st); return st.size_in_use; }
int main(void) {
    size_t before = inuse();
    for (int i = 0; i < 2000; i++) { pthread_t t; pthread_create(&t, 0, body, 0); pthread_join(t, 0); }
    double grew = (inuse() - (double)before) / 1048576.0;
    printf("2000 個短命線程後堆增長 %.2f MiB（期望 < 1；修前實測 250）%s\n", grew, grew < 1 ? "✓" : "✗");
    return !(grew < 1);
}
