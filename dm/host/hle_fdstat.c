// 診斷 DM_FD_LOG=<秒>：每隔這麼多秒記一行「進程在用的文件描述符數與構成」（長時間遊玩會不會撞打開文件數上限：iOS 軟上限缺省 256，main.m 啓動時已提到 4096）。
// 逐個描述符 fstat 判類型；普通文件 / 目錄用 fcntl(F_GETPATH) 取路徑後按去處歸類。只讀、不動任何描述符；不設變量時不起線程。
// Diagnostic DM_FD_LOG=<seconds>: every this many seconds log one line with "the number and breakdown of file descriptors the process has in use" (whether long play sessions will hit the open file limit: the iOS soft limit defaults to 256, already raised to 4096 at startup in main.m).
// fstat each descriptor to determine its type; for regular files / directories get the path with fcntl(F_GETPATH) and classify by where it points. Read-only, touches no descriptor; no thread is started when the variable is unset.
#include "hle.h"
#include "dm.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static void *fdstat_main(void *arg) {
    int every = (int)(intptr_t)arg; char prev[1024] = "";
    for (unsigned round = 1;; round++) {
        sleep((unsigned)every);
        struct rlimit rl = {0}; getrlimit(RLIMIT_NOFILE, &rl); int max = getdtablesize(); if (max > 8192) max = 8192;
        int total = 0, romfs = 0, save = 0, logf = 0, cache = 0, dev = 0, sock = 0, pipe_ = 0, kq = 0, dir = 0, other = 0, hi = -1; char sample[400] = ""; int sk = 0;
        for (int fd = 0; fd < max; fd++) { struct stat st; if (fstat(fd, &st)) continue; total++; hi = fd;
            if (S_ISSOCK(st.st_mode)) { sock++; continue; } if (S_ISFIFO(st.st_mode)) { pipe_++; continue; } if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) { dev++; continue; }
            char p[MAXPATHLEN] = ""; if (fcntl(fd, F_GETPATH, p) || !p[0]) { if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) other++; else kq++; continue; }
            if (S_ISDIR(st.st_mode)) dir++;
            if (strstr(p, "romfs") || strstr(p, "RomFS") || strstr(p, "/data/hd/") || strstr(p, ".casc") || strstr(p, "/Data/data/")) romfs++;
            else if (strstr(p, "/save") || strstr(p, ".d2s") || strstr(p, ".d2i") || strstr(p, "/user/")) save++;
            else if (strstr(p, ".log") || strstr(p, "boot.txt") || strstr(p, ".jsonl") || strstr(p, ".tsv")) logf++;
            else if (strstr(p, "/Caches/") || strstr(p, "/cache/") || strstr(p, ".metallib") || strstr(p, "/tmp/") || strstr(p, "/var/folders/")) cache++;
            else { other++; if (sk < 300) { const char *b = strrchr(p, '/'); sk += snprintf(sample + sk, sizeof sample - sk, " %s", b ? b + 1 : p); } } }
        char line[1024]; snprintf(line, sizeof line, "在用 %d（最大號 %d，軟上限 %llu）：資源檔 %d、存檔 %d、日誌 %d、緩存 / 臨時 %d、設備 %d、套接字 %d、管道 %d、其它內核對象 %d、其它文件 %d%s%s；其中目錄 %d",
                                   total, hi, (unsigned long long)rl.rlim_cur, romfs, save, logf, cache, dev, sock, pipe_, kq, other, other ? "：" : "", other ? sample : "", dir);
        LOG("描述符 第 %u 次（每 %d 秒）%s%s", round, every, line, strcmp(line, prev) ? "" : "（同上次）"); strlcpy(prev, line, sizeof prev);
    }
    return NULL;
}
__attribute__((constructor)) static void fdstat_init(void) {
    const char *e = getenv("DM_FD_LOG"); int every = e ? atoi(e) : 0; if (every <= 0) return;
    pthread_t t; if (!pthread_create(&t, NULL, fdstat_main, (void *)(intptr_t)every)) pthread_detach(t);
}
