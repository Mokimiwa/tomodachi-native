// iCloud 同步核心單測（秒級）：dm_cloudsync.c。兩個本地目錄當兩台設備、一個本地目錄當雲端；期望都是按「後寫覆蓋先寫」手推的，不跑被測實現得出。
// iCloud sync core unit test (takes seconds): dm_cloudsync.c. Two local directories act as two devices and one local directory as the cloud; all expectations are derived by hand from "later write overwrites earlier write", not obtained by running the implementation under test.
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
int dm_cloud_sync(const char *save, const char *cloud, const char *bak, const char *statef, int allow_down, uint64_t now_ms, int (*ready)(const char *unit));
int dm_cloud_conflict_take(void);
#if __has_include("../stash_seed.h")   // 公開倉庫不帶這份預置（它是遊戲生成的一份存檔的字節）：不在時跳過用到它的 ⑱ ⑲ 兩組 | the public repository does not ship this seed (it is the bytes of a save the game generated): when it is absent, the two groups that use it, 18 and 19, are skipped
#include "../stash_seed.h"
#define T_HAVE_STASH_SEED 1
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char R[512];
static void path(char *o, const char *a, const char *b) { snprintf(o, 1024, "%s/%s/%s", R, a, b); }
static void put(const char *dev, const char *rel, const char *txt, long sec) {   // 寫一個文件並把修改時間定成 sec 秒 | write a file and set its modification time to sec seconds
    char p[1024]; path(p, dev, rel); char d[1024]; strcpy(d, p); for (char *s = d + strlen(R) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(d, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); fputs(txt, f); fclose(f); struct timeval tv[2] = { { sec, 0 }, { sec, 0 } }; utimes(p, tv);
}
static const char *get(const char *dev, const char *rel) { static char b[256]; char p[1024]; path(p, dev, rel); FILE *f = fopen(p, "r"); if (!f) return NULL; size_t n = fread(b, 1, 255, f); b[n] = 0; fclose(f); return b; }
static long mtime(const char *dev, const char *rel) { char p[1024]; struct stat st; path(p, dev, rel); return stat(p, &st) ? -1 : st.st_mtimespec.tv_sec; }
static int dsync(const char *dev, int down, uint64_t now) { char s[1024], c[1024], b[1024], t[1024]; path(s, dev, "save"); snprintf(c, sizeof c, "%s/cloud", R); path(b, dev, "bak"); path(t, dev, "state"); return dm_cloud_sync(s, c, b, t, down, now, NULL); }
static int count_bak(const char *dev, const char *pre) { char d[1024]; path(d, dev, "bak"); DIR *dp = opendir(d); struct dirent *e; int n = 0; while (dp && (e = readdir(dp))) n += !strncmp(e->d_name, pre, strlen(pre)); if (dp) closedir(dp); return n; }
static int no_skip(const char *u) { return strcmp(u, "Alice"); }
// 共享倉庫檔：一頁物品（items 件，只寫頁頭與件數）+ 一段編年史（chron 字節）；側文件：n 件。只造結構，同步那邊只看結構（dm_cloudsync.c rich_of）
// Shared stash file: one page of items (items pieces, only the page header and item count are written) + one chronicle section (chron bytes); side file: n pieces. Only the structure is built, as the sync side only looks at structure (dm_cloudsync.c rich_of)
static void put_bin(const char *dev, const char *rel, const uint8_t *d, size_t n, long sec) {
    char p[1024]; path(p, dev, rel); char dd[1024]; strcpy(dd, p); for (char *x = dd + strlen(R) + 1; (x = strchr(x, '/')); x++) { *x = 0; mkdir(dd, 0755); *x = '/'; }
    FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); struct timeval tv[2] = { { sec, 0 }, { sec, 0 } }; utimes(p, tv);
}
static void put_d2i(const char *dev, int items, int chron, long sec) {
    static uint8_t b[8192]; memset(b, 0, sizeof b); uint32_t l1 = 68, l2 = (uint32_t)chron;
    memcpy(b, "\x55\xaa\x55\xaa", 4); memcpy(b + 16, &l1, 4); memcpy(b + 64, "JM", 2); b[66] = (uint8_t)items;
    memcpy(b + 68, "\x55\xaa\x55\xaa", 4); memcpy(b + 68 + 16, &l2, 4); b[68 + 64] = 0xc0;
    put_bin(dev, "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i", b, 68 + l2, sec);
}
static void put_vault(const char *dev, int n, long sec) { uint8_t b[8] = { 'S', 'W', 'V', 'T', 1, 0, (uint8_t)n, 0 }; put_bin(dev, "save/ModernSharedStashSoftCoreV2.swvault", b, 8, sec); }
static int d2i_items(const char *dev, const char *rel) { char p[1024]; path(p, dev, rel); FILE *f = fopen(p, "rb"); if (!f) return -1; uint8_t b[70]; size_t n = fread(b, 1, 70, f); fclose(f); return n >= 67 ? b[66] : -1; }

int main(void) {
    snprintf(R, sizeof R, "%s/t_cloudsync.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(R, 0755);
    char c[1024]; snprintf(c, sizeof c, "%s/cloud", R); mkdir(c, 0755);
    // ① A 有角色 Alice（時刻 1000 秒）、共享倉庫、Settings；B 空 → A 上傳，B 啓動拿到 Alice 與共享倉庫，Settings 不同步，修改時間照 A 的
    // (1) A has character Alice (timestamp 1000 seconds), the shared stash and Settings; B is empty -> A uploads, B gets Alice and the shared stash at startup, Settings is not synced, and modification times follow A's
    put("A", "save/Alice/Alice.d2s", "a1", 1000); put("A", "save/Alice/Alice.key", "k", 900);
    put("A", "save/SharedStashSoftCoreV2.d2i", "s1", 1100); put("A", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i", "m1", 1100); put("A", "save/Settings/Settings.json", "{}", 1100);
    put("B", "save/Settings/Settings.json", "{b}", 1200);
    CHECK(dsync("A", 0, 0) == 2, "① A 上傳應為 2 個單位（Alice、_stash）");
    CHECK(dsync("B", 1, 0) == 2, "① B 下載應為 2 個單位");
    CHECK(get("B", "save/Alice/Alice.d2s") && !strcmp(get("B", "save/Alice/Alice.d2s"), "a1"), "① B 的 Alice 應是 a1");
    CHECK(mtime("B", "save/Alice/Alice.d2s") == 1000, "① 修改時間應還原成 1000，實為 %ld", mtime("B", "save/Alice/Alice.d2s"));
    CHECK(get("B", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i") && !strcmp(get("B", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"), "m1"), "① B 的共享倉庫應是 m1");
    CHECK(!strcmp(get("B", "save/Settings/Settings.json"), "{b}"), "① B 的 Settings 不該被動");
    CHECK(dsync("A", 1, 0) == 0 && dsync("B", 1, 0) == 0, "① 再同步一次兩邊都應無動作");
    // ② 只有 B 改（2000）→ B 運行中上傳；A 啓動拿到，A 原來那份進備份
    // (2) Only B changed (2000) -> B uploads while running; A gets it at startup, and A's former copy goes to backup
    put("B", "save/Alice/Alice.d2s", "b2", 2000);
    CHECK(dsync("B", 0, 0) == 1, "② B 應上傳 1 個");
    CHECK(dsync("A", 0, 0) == 0 && !strcmp(get("A", "save/Alice/Alice.d2s"), "a1"), "② A 運行中不該改本地");
    CHECK(dsync("A", 1, 0) == 1 && !strcmp(get("A", "save/Alice/Alice.d2s"), "b2"), "② A 啓動後應是 b2");
    CHECK(get("A", "bak/Alice-1000000/Alice/Alice.d2s") && !strcmp(get("A", "bak/Alice-1000000/Alice/Alice.d2s"), "a1"), "② A 被覆蓋的 a1 應在備份 Alice-1000000");
    // ③ 兩邊都改，B 後寫（A 3000 先傳，B 3500）→ B 運行中照樣上傳（後寫贏），雲端原來的 a3 進 B 的備份；A 啓動拿到 b3
    // (3) Both sides changed, B wrote later (A's 3000 uploaded first, B 3500) -> B still uploads while running (last write wins), the cloud's former a3 goes to B's backup; A gets b3 at startup
    put("A", "save/Alice/Alice.d2s", "a3", 3000); put("B", "save/Alice/Alice.d2s", "b3", 3500);
    CHECK(dsync("A", 0, 0) == 1, "③ A 先上傳");
    CHECK(dsync("B", 0, 0) == 1, "③ B 後寫也上傳");
    CHECK(get("B", "bak/Alice-3000000/Alice/Alice.d2s") && !strcmp(get("B", "bak/Alice-3000000/Alice/Alice.d2s"), "a3"), "③ 被覆蓋的雲端 a3 應在 B 的備份");
    CHECK(dsync("A", 1, 0) == 1 && !strcmp(get("A", "save/Alice/Alice.d2s"), "b3"), "③ A 啓動後應是 b3");
    // ④ 兩邊都改，A 後寫（A 5000 已傳；B 4000 沒傳就重啓）→ B 啓動拿 A 的 a4，b4 進備份 Alice-4000000
    // (4) Both sides changed, A wrote later (A's 5000 uploaded; B's 4000 not uploaded before restarting) -> B gets A's a4 at startup, b4 goes to backup Alice-4000000
    put("A", "save/Alice/Alice.d2s", "a4", 5000); CHECK(dsync("A", 0, 0) == 1, "④ A 上傳");
    put("B", "save/Alice/Alice.d2s", "b4", 4000);
    CHECK(dsync("B", 1, 0) == 1 && !strcmp(get("B", "save/Alice/Alice.d2s"), "a4"), "④ B 啓動後應是 a4（雲端 5000 > 本機 4000）");
    CHECK(get("B", "bak/Alice-4000000/Alice/Alice.d2s") && !strcmp(get("B", "bak/Alice-4000000/Alice/Alice.d2s"), "b4"), "④ B 被覆蓋的 b4 應在備份");
    // ⑤ 雲端的包還沒下載好（ready 返回 0）→ 跳過，本地不動
    // (5) The cloud package has not finished downloading (ready returns 0) -> skip, local files untouched
    put("A", "save/Alice/Alice.d2s", "a5", 6000); CHECK(dsync("A", 0, 0) == 1, "⑤ A 上傳");
    { char s[1024], b[1024], t[1024]; path(s, "B", "save"); path(b, "B", "bak"); path(t, "B", "state"); CHECK(dm_cloud_sync(s, c, b, t, 1, 0, no_skip) == 0 && !strcmp(get("B", "save/Alice/Alice.d2s"), "a4"), "⑤ 沒下載好時 B 應不動"); }
    CHECK(dsync("B", 1, 0) == 1 && !strcmp(get("B", "save/Alice/Alice.d2s"), "a5"), "⑤ 下載好後 B 應是 a5");
    // ⑥ A 刪 Alice → 雲端刪除標記；B 啓動也刪，B 那份進備份；B 的備份只留 3 份（此前有 1000000 不在 B、3000000、4000000，再加 6000000 = 3 份）
    // (6) A deletes Alice -> deletion marker in the cloud; B also deletes at startup and B's copy goes to backup; B's backup keeps only 3 copies (before this there were 1000000 not on B, 3000000, 4000000, plus 6000000 = 3 copies)
    { char p[1024]; path(p, "A", "save/Alice/Alice.d2s"); unlink(p); path(p, "A", "save/Alice/Alice.key"); unlink(p); path(p, "A", "save/Alice"); rmdir(p); }
    CHECK(dsync("A", 0, 7000000) == 1, "⑥ A 應寫刪除標記");
    CHECK(dsync("B", 1, 0) == 1 && !get("B", "save/Alice/Alice.d2s"), "⑥ B 啓動後 Alice 應被刪");
    CHECK(get("B", "bak/Alice-6000000/Alice/Alice.d2s") && !strcmp(get("B", "bak/Alice-6000000/Alice/Alice.d2s"), "a5"), "⑥ B 被刪的 a5 應在備份");
    CHECK(count_bak("B", "Alice-") == 3, "⑥ B 的 Alice 備份應留 3 份，實為 %d", count_bak("B", "Alice-"));
    for (long t = 8; t <= 9; t++) { char v[8]; snprintf(v, sizeof v, "x%ld", t); put("B", "save/Bob/Bob.d2s", v, t * 1000); }   // 無關角色不受影響 | unrelated characters are not affected
    CHECK(dsync("B", 1, 0) == 1, "⑥ B 的新角色 Bob 上傳");
    // ⑦ 刪除之後 A 又建了同名角色（9500）→ 上傳（復活）；B 啓動拿到
    // (7) After the deletion A created a character with the same name again (9500) -> upload (revive); B gets it at startup
    put("A", "save/Alice/Alice.d2s", "a7", 9500);
    CHECK(dsync("A", 1, 0) == 2 && !strcmp(get("A", "save/Bob/Bob.d2s"), "x9"), "⑦ A：Alice 上傳 + Bob 下載");
    CHECK(dsync("B", 1, 0) == 1 && get("B", "save/Alice/Alice.d2s") && !strcmp(get("B", "save/Alice/Alice.d2s"), "a7"), "⑦ B 啓動後應有新建的 Alice a7");
    // ⑧ 共享倉庫：B 改（10000）→ A 啓動拿到
    // (8) Shared stash: B changes it (10000) -> A gets it at startup
    put("B", "save/SharedStashSoftCoreV2.d2i", "s8", 10000);
    CHECK(dsync("B", 0, 0) == 1 && dsync("A", 1, 0) == 1 && !strcmp(get("A", "save/SharedStashSoftCoreV2.d2i"), "s8"), "⑧ A 的共享倉庫應是 s8");
    CHECK(!strcmp(get("A", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"), "m1"), "⑧ 共享倉庫的另一個文件應保留 m1");
    // ⑨ 存檔目錄被遊戲以外的原因清空（狀態文件還在）→ 啓動時不算刪除，從雲端全拿回來，雲端不出現刪除標記
    // (9) The save directory was emptied by something other than the game (the state file is still there) -> at startup it does not count as a deletion, everything is fetched back from the cloud, and no deletion marker appears in the cloud
    { char p[1024]; path(p, "A", "save"); char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", p); system(cmd); mkdir(p, 0755); }
    CHECK(dsync("A", 1, 0) == 3, "⑨ A 啓動應下載 3 個（Alice、Bob、_stash）");
    CHECK(get("A", "save/Alice/Alice.d2s") && !strcmp(get("A", "save/Alice/Alice.d2s"), "a7") && get("A", "save/Bob/Bob.d2s") && !strcmp(get("A", "save/SharedStashSoftCoreV2.d2i"), "s8"), "⑨ A 應拿回 Alice a7、Bob、共享倉庫 s8");
    CHECK(dsync("B", 1, 0) == 0 && get("B", "save/Alice/Alice.d2s"), "⑨ B 不受影響（沒有刪除標記傳過來）");
    // ⑩ 備份目錄寫不進（這裡用同名普通文件把它堵住）→ 覆蓋和刪除都不動本地；解開後照常進行（原先備份寫失敗仍照樣覆蓋 / 刪）
    // (10) The backup directory cannot be written (here it is blocked with a regular file of the same name) -> neither overwrite nor delete touches local files; after unblocking it proceeds as usual (previously it still overwrote / deleted when the backup write failed)
    { char s[1024], c2[1024], b[1024], t2[1024], bf[1024]; snprintf(c2, sizeof c2, "%s/cloud2", R); mkdir(c2, 0755);
      #define SY(dev, down, now) (path(s, dev, "save"), path(b, dev, "bak"), path(t2, dev, "state"), dm_cloud_sync(s, c2, b, t2, down, now, NULL))
      put("F", "save/Settings/Settings.json", "{}", 1);   // F 是一台已有存檔目錄的設備 | F is a device that already has a save directory
      put("E", "save/Eve/Eve.d2s", "e1", 1000); CHECK(SY("E", 0, 0) == 1 && SY("F", 1, 0) == 1 && get("F", "save/Eve/Eve.d2s") && !strcmp(get("F", "save/Eve/Eve.d2s"), "e1"), "⑩ 準備：F 拿到 Eve e1");
      put("E", "save/Eve/Eve.d2s", "e2", 2000); CHECK(SY("E", 0, 0) == 1, "⑩ E 上傳 e2");
      path(bf, "F", "bak"); FILE *f = fopen(bf, "w"); fclose(f);          // F 的備份目錄位置是個普通文件：建不了目錄 | F's backup directory location is a regular file: the directory cannot be created
      CHECK(SY("F", 1, 0) == 0 && get("F", "save/Eve/Eve.d2s") && !strcmp(get("F", "save/Eve/Eve.d2s"), "e1"), "⑩ 備份寫不進時 F 不應覆蓋，仍是 e1");
      unlink(bf);
      CHECK(SY("F", 1, 0) == 1 && get("F", "save/Eve/Eve.d2s") && !strcmp(get("F", "save/Eve/Eve.d2s"), "e2") && get("F", "bak/Eve-1000000/Eve/Eve.d2s") && !strcmp(get("F", "bak/Eve-1000000/Eve/Eve.d2s"), "e1"), "⑩ 解開後 F 換成 e2，e1 整份在備份裡");
      { char p2[1024]; path(p2, "E", "save/Eve/Eve.d2s"); unlink(p2); path(p2, "E", "save/Eve"); rmdir(p2); }
      CHECK(SY("E", 0, 3000000) == 1, "⑩ E 運行中刪了 Eve，寫刪除標記");
      char bk[1100]; snprintf(bk, sizeof bk, "%s.keep", bf); rename(bf, bk); f = fopen(bf, "w"); fclose(f);   // 已有的備份目錄先挪開，再用普通文件堵住 | move the existing backup directory aside first, then block it with a regular file
      CHECK(SY("F", 1, 0) == 0 && get("F", "save/Eve/Eve.d2s"), "⑩ 備份寫不進時 F 不應刪 Eve");
      unlink(bf); rename(bk, bf);
      CHECK(SY("F", 1, 0) == 1 && !get("F", "save/Eve/Eve.d2s") && get("F", "bak/Eve-2000000/Eve/Eve.d2s"), "⑩ 解開後 F 刪 Eve，e2 整份在備份裡");
      #undef SY
    }
    // ⑪–⑮ 編年史被整份換空：新設備首次同步以雲端為準；共享倉庫不許被「明顯更少」的一份覆蓋（另一個雲端目錄 cloud3，設備 G / H）
    // (11)-(15) Chronicle replaced wholesale by an empty one: a new device's first sync takes the cloud as authoritative; the shared stash must not be overwritten by a copy with "clearly less" (another cloud directory cloud3, devices G / H)
    { char s3[1024], c3[1024], b3[1024], t3[1024]; snprintf(c3, sizeof c3, "%s/cloud3", R); mkdir(c3, 0755);
      #define S3(dev, down) (path(s3, dev, "save"), path(b3, dev, "bak"), path(t3, dev, "state"), dm_cloud_sync(s3, c3, b3, t3, down, 0, NULL))
      #define D2I "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"
      put_d2i("G", 5, 300, 1000); put_vault("G", 3, 1000);
      CHECK(S3("G", 0) == 1, "⑫ 雲端為空：G 首次同步上傳共享倉庫");
      dm_cloud_conflict_take();
      put_d2i("H", 0, 148, 5000);   // H 新裝：遊戲剛建的空共享倉庫，時刻比雲端新 | H freshly installed: the empty shared stash the game just created, with a timestamp newer than the cloud's
      CHECK(S3("H", 0) == 0 && d2i_items("H", D2I) == 0, "⑪ 新設備運行中首次同步：不傳不改（不能拿空倉庫蓋雲端）");
      CHECK(S3("H", 1) == 1 && d2i_items("H", D2I) == 5 && get("H", "save/ModernSharedStashSoftCoreV2.swvault"), "⑪ 新設備啓動首次同步：以雲端為準，拿到 5 件與編年史倉庫");
      CHECK(count_bak("H", "_stash-") == 1, "⑪ 新設備本來那份空倉庫進備份");
      CHECK(!dm_cloud_conflict_take(), "⑪ 正常首次同步不算衝突");
      put_d2i("G", 6, 400, 6000); CHECK(S3("G", 0) == 1, "⑭ G 編年史變長（400）照常上傳");
      CHECK(S3("H", 1) == 1 && d2i_items("H", D2I) == 6, "⑭ H 啓動拿到 G 的新版");
      put_d2i("H", 0, 148, 7000);   // H 上的共享倉庫不知為何變空（編年史 148、0 件），時刻更新 | the shared stash on H somehow became empty (chronicle 148, 0 items), with a newer timestamp
      CHECK(S3("H", 0) == 0, "⑬ H 運行中要上傳變空的那份：攔下");
      CHECK(dm_cloud_conflict_take() == 1, "⑬ 攔下記衝突（進局彈提示）");
      CHECK(S3("G", 1) == 0 && d2i_items("G", D2I) == 6, "⑬ 雲端沒被蓋：G 仍是 6 件");
      put_d2i("H", 7, 500, 7500); CHECK(S3("H", 0) == 1, "⑮ H 正常玩：編年史 500 上傳");
      put_d2i("G", 6, 450, 7600);   // G 沒同步到 H 的 500 就自己寫了 450（兩邊都改，G 後寫） | G wrote 450 itself without having synced H's 500 (both sides changed, G wrote later)
      CHECK(S3("G", 0) == 0, "⑮ G 運行中要拿編年史 450 蓋雲端的 500：攔下");
      CHECK(S3("G", 1) == 0 && d2i_items("G", D2I) == 6, "⑮ G 啓動：雲端 500 比本機 450 時刻早但編年史長——按後寫贏會上傳，被攔；本機不動");
      CHECK(dm_cloud_conflict_take() == 1, "⑮ 記衝突");
      { char p3[1024]; path(p3, "H", "save/ModernSharedStashSoftCoreV2.swvault"); unlink(p3); } put_d2i("H", 7, 500, 8000);
      CHECK(S3("H", 0) == 0 && dm_cloud_conflict_take() == 1, "⑬ 編年史倉庫側文件沒了（雲端有 3 件）：攔下");
      put_vault("H", 0, 8100); put_d2i("H", 0, 500, 8100);
      CHECK(S3("H", 0) == 1 && !dm_cloud_conflict_take(), "⑯ 正常玩法：共享倉庫物品取空、編年史倉庫取空（文件還在，0 件），編年史沒變短——照常上傳");
      // ⑰ rf3 那個形狀：雲端專家倉庫 2 件、編年史還是新建的 148；本機這份被換成遊戲新建的空檔（0 件、148），狀態記錄還在、雲端沒變 = 走「只本機改了」的上傳
      // (17) That rf3 shape: cloud hardcore stash has 2 items and the chronicle is still the freshly created 148; the local copy was replaced by the empty file the game freshly creates (0 items, 148), the state record is still there and the cloud is unchanged = takes the "only local changed" upload
      { char cd[1024]; snprintf(cd, sizeof cd, "%s/cloud4", R); mkdir(cd, 0755);
        #define S4(dev, down) (path(s3, dev, "save"), path(b3, dev, "bak"), path(t3, dev, "state"), dm_cloud_sync(s3, cd, b3, t3, down, 0, NULL))
        put_d2i("J", 2, 148, 1000); CHECK(S4("J", 1) == 1, "⑰ 準備：J 上傳 2 件、編年史 148 的倉庫");
        put_d2i("J", 0, 148, 2000); dm_cloud_conflict_take();
        CHECK(S4("J", 0) == 0 && dm_cloud_conflict_take() == 1, "⑰ 運行中要拿新建空檔（0 件、148）蓋 2 件的那份：攔下");
        CHECK(S4("J", 1) == 0, "⑰ 啓動時同樣攔下（不上傳）");
        put_d2i("J", 0, 300, 3000); CHECK(S4("J", 0) == 1, "⑰ 編年史長過新建大小的倉庫取空：照常上傳");
        // ⑱ rf3 真形狀：雲端專家倉庫是另一台的預置（stash_seed.h），本機還沒有專家倉庫檔（啓動同步跑在寫預置之前）→ 不攔、不彈；本機寫了預置後上傳也不攔
        // (18) The real rf3 shape: the cloud hardcore stash is another device's preset (stash_seed.h), and the local device has no hardcore stash file yet (startup sync runs before the preset is written) -> not blocked, no prompt; uploading after the local device writes the preset is not blocked either
#ifdef T_HAVE_STASH_SEED
        { char cd5[1024]; snprintf(cd5, sizeof cd5, "%s/cloud5", R); mkdir(cd5, 0755);
          #define S5(dev, down) (path(s3, dev, "save"), path(b3, dev, "bak"), path(t3, dev, "state"), dm_cloud_sync(s3, cd5, b3, t3, down, 0, NULL))
          #define HC "save/ModernSharedStashHardCoreV2/ModernSharedStashHardCoreV2.d2i"
          put_bin("K", HC, g_stash_seed, sizeof g_stash_seed, 1000); put_d2i("K", 3, 400, 1000); CHECK(S5("K", 1) == 1, "⑱ 準備：K 上傳（專家倉庫是預置）");
          { char p5[1024]; path(p5, "K", HC); unlink(p5); } put_d2i("K", 3, 400, 2000); dm_cloud_conflict_take();
          S5("K", 0); CHECK(!dm_cloud_conflict_take(), "⑱ 本機還沒有專家倉庫檔、雲端只是預置：不算丟東西，不彈");
          put_bin("K", HC, g_stash_seed, sizeof g_stash_seed, 3000); S5("K", 0); CHECK(!dm_cloud_conflict_take(), "⑱ 本機寫了預置再上傳：不攔");
          // ⑲ 反過來：雲端有真東西，本機是預置 → 照攔
          // (19) The reverse: the cloud has real content and the local copy is the preset -> still blocked
          put_d2i("K", 5, 600, 4000); S5("K", 0); dm_cloud_conflict_take();
          { uint8_t big[700]; memset(big, 0, sizeof big); uint32_t l1 = 68, l2 = 300; memcpy(big, "\x55\xaa\x55\xaa", 4); memcpy(big + 16, &l1, 4); memcpy(big + 64, "JM", 2); big[66] = 9; memcpy(big + 68, "\x55\xaa\x55\xaa", 4); memcpy(big + 84, &l2, 4); big[132] = 0xc0;
            put_bin("K", HC, big, 68 + 300, 5000); CHECK(S5("K", 0) == 1, "⑲ 準備：專家倉庫有 9 件、編年史 300 上傳"); }
          put_bin("K", HC, g_stash_seed, sizeof g_stash_seed, 6000);
          CHECK(S5("K", 0) == 0 && dm_cloud_conflict_take() == 1, "⑲ 本機變回預置要蓋 9 件的那份：攔下");
          #undef HC
          #undef S5
        }
#else
        printf("跳過 ⑱ ⑲：沒有共享倉庫預置 stash_seed.h（公開倉庫不帶）\n");
#endif
        #undef S4
      }
      #undef D2I
      #undef S3
    }
    printf("t_cloudsync: %d 項，%d 失敗\n", checks, fails);
    return fails != 0;
}
