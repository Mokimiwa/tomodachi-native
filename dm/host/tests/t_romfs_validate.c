#include "../romfs_validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int check(int ok, const char *s) { if (!ok) fprintf(stderr, "失敗：%s\n", s); return ok; }
int main(void) {
    const char *src = getenv("DM_ROMFS"); if (!src) src = getenv("HOME");
    if (!src) return 1;
    char cmd[4096], why[512], rep[2048], sd[4096]; snprintf(sd, sizeof sd, "%s/data", src);
    if (!check(dm_romfs_validate(sd, why, sizeof why), "原版 RomFS 合格")) { fprintf(stderr, "%s\n", why); return 1; }
    if (!check(dm_romfs_report(sd, rep, sizeof rep) == 0, "完整清單：原版 RomFS 無缺")) return 1;
    {   // 空目錄：18 個檔案一個都沒有。期望值取自 romfs_validate.c 的 need / size 兩張表：00-00000001.data 1073741824 字節 = 1.00 GB、17-00000002.data 4496514 字節 = 4 MB | Empty directory: none of the 18 archives is present. Expected values come from the need / size tables in romfs_validate.c: 00-00000001.data 1073741824 bytes = 1.00 GB, 17-00000002.data 4496514 bytes = 4 MB
        char r1[] = "/tmp/dm-romfs-test.XXXXXX"; if (!mkdtemp(r1)) return 1;
        if (!check(!dm_romfs_validate(r1, why, sizeof why) && !strcmp(why, "缺少 00-00000001.data"), "空目錄被拒，報第一個檔案")) { fprintf(stderr, "%s\n", why); return 1; }
        int bad = dm_romfs_report(r1, rep, sizeof rep);
        if (!check(bad == 18 && strstr(rep, "已有 0 / 18") && strstr(rep, "00-00000001.data（1.00 GB）") && strstr(rep, "17-00000002.data（4 MB）") && !strstr(rep, "沒拷完"), "完整清單：缺 18 個逐個列出")) { fprintf(stderr, "%s\n", rep); return 1; }
        // 配置、密鑰、清單這些小文件不在核對範圍裡：放一份內容不對的進去，結論與給玩家的清單都不變，清單裡不出現它們的名字
        // Small files such as the config, key and manifests are outside the scope of the check: put in a copy with wrong contents, and neither the verdict nor
        // the list shown to the player changes, and their names do not appear in the list
        snprintf(cmd, sizeof cmd, "printf bad > '%s/.build.config' && printf x > '%s/build_key.txt' && printf x > '%s/archive_manifest.psv'", r1, r1, r1); if (system(cmd)) return 1;
        bad = dm_romfs_report(r1, rep, sizeof rep);
        if (!check(bad == 18 && !strstr(rep, "build") && !strstr(rep, "psv"), "完整清單：不提小文件")) { fprintf(stderr, "%s\n", rep); return 1; }
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", r1); system(cmd);
    }
    {   // 只有檔案文件（沒有配置、密鑰、任何清單）就合格；再核字節數與剔零檔案：用符號鏈接搭一份，把一個小檔案截短 / 換成「剔零 + 區段表」 | Archive files alone (no config, key or any manifest) are enough to pass; then check byte counts and zero-stripped archives: build a copy with symlinks, then truncate one small archive / replace it with "zero-stripped + extent table"
        char r2[] = "/tmp/dm-romfs-size.XXXXXX"; if (!mkdtemp(r2)) return 1;
        snprintf(cmd, sizeof cmd, "for f in '%s'/data/*.data; do ln -s \"$f\" '%s/' || exit 1; done", src, r2); if (system(cmd)) return 1;
        if (!check(dm_romfs_validate(r2, why, sizeof why), "只有檔案文件的目錄合格")) { fprintf(stderr, "%s\n", why); return 1; }
        snprintf(cmd, sizeof cmd, "rm '%s/04-00000002.data' && head -c 1000000 '%s/data/04-00000002.data' > '%s/04-00000002.data'", r2, src, r2); if (system(cmd)) return 1;
        if (!check(!dm_romfs_validate(r2, why, sizeof why) && strstr(why, "還沒拷完"), "拷到一半的檔案被拒")) { fprintf(stderr, "%s\n", why); return 1; }
        int bad = dm_romfs_report(r2, rep, sizeof rep);
        if (!check(bad == 1 && strstr(rep, "已有 17 / 18") && strstr(rep, "沒拷完：04-00000002.data（1 MB / 9 MB）"), "完整清單：沒拷完的單列，帶現有 / 應有（1000000 字節 = 1 MB、8915855 字節 = 9 MB，按 1048576 折算取整）")) { fprintf(stderr, "%s\n", rep); return 1; }
        snprintf(cmd, sizeof cmd, "printf 'size 8915855\\n1000000 7915855\\n' > '%s/04-00000002.data.holes'", r2); if (system(cmd)) return 1;
        if (!check(dm_romfs_validate(r2, why, sizeof why), "剔零檔案（前 1000000 字節 + 區段表）合格")) { fprintf(stderr, "%s\n", why); return 1; }
        snprintf(cmd, sizeof cmd, "printf 'size 8915855\\n1000000 100\\n' > '%s/04-00000002.data.holes'", r2); if (system(cmd)) return 1;
        if (!check(!dm_romfs_validate(r2, why, sizeof why) && strstr(why, "還沒拷完"), "區段表與實際長度對不上被拒")) { fprintf(stderr, "%s\n", why); return 1; }
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", r2); system(cmd);
    }
    return 0;
}
