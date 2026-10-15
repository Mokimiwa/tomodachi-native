// 存檔寫與存檔事務單測（秒級）：直接包含 hle_fs.c 調它的靜態實現；pwrite 換成可編排的假實現。
// 期望值全部按語義手寫，不調被測實現算：
//   ① pwrite 返回 0（盤滿時可能出現）→ WriteFile 一次調用後返回失敗（原先在此死循環）；
//   ② pwrite 分段寫（先 3 字節再其餘）→ 成功且文件內容完整；
//   ③ 事務：①的失敗使這個事務的 Commit 被拒、工作目錄回到已提交版；下一個事務寫在工作目錄，Commit 前已提交目錄仍是舊版，Commit 後是新版；
//   ④ DM_TEST_FS_FAIL=write:N:errno 只讓存檔第 N 次寫失敗，前後的寫照常；該事務 Commit 被拒、已提交版不變、前端收到一次通知；
//      下一個事務照常提交，最終存檔 = 下一個事務的內容；
//   ⑤ 重新掛載丟棄未提交內容。
// Save write and save transaction unit test (takes seconds): includes hle_fs.c directly and calls its static implementation; pwrite is replaced by a scriptable fake.
// All expected values are hand-written from the semantics, not computed by calling the implementation under test:
//   (1) pwrite returns 0 (can happen when the disk is full) -> WriteFile returns failure after one call (it used to loop forever here);
//   (2) pwrite writes in pieces (3 bytes first, then the rest) -> success and the file content is complete;
//   (3) transaction: the failure in (1) causes this transaction's Commit to be rejected and the working directory to return to the committed version; the next transaction writes in the working directory, the committed directory is still the old version before Commit and the new version after Commit;
//   (4) DM_TEST_FS_FAIL=write:N:errno makes only the Nth save write fail, with writes before and after proceeding normally; that transaction's Commit is rejected, the committed version is unchanged, and the front end receives one notification;
//      the next transaction commits normally, and the final save = the content of the next transaction;
//   (5) remounting discards uncommitted content.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off);
#define pwrite fake_pwrite
#include "../hle_fs.c"
#undef pwrite
ssize_t pwrite(int, const void *, size_t, off_t);

struct dm_flags g_flag; uint64_t g_t0_ns; uint64_t now_ns(void) { return 0; }   // 日誌時間戳與讀路徑的樁（寫路徑不用） | stubs for log timestamps and the read path (not used by the write path)
void dm_prof_add(int id, uint64_t ns, uint64_t b) { (void)id; (void)ns; (void)b; } void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail, g_lasterr;                                        // 前端通知計數（t_fs_stub.c 的強實現替掉 hle_fs.c 的弱實現） | front-end notification count (the strong implementation in t_fs_stub.c replaces the weak one in hle_fs.c)
void dm_fs_save_poison(int e);
static int g_mode, g_calls;                   // 0 照常寫；1 一律返回 0；2 第一次只寫 3 字節 | 0 write normally; 1 always return 0; 2 write only 3 bytes the first time
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off) {
    g_calls++;
    if (g_mode == 1) { if (g_calls > 1000) { printf("  ✗ pwrite 返回 0 後被重試超過 1000 次（死循環）\n"); exit(1); } return 0; }
    if (g_mode == 2 && g_calls == 1 && n > 3) n = 3;
    return pwrite(fd, buf, n, off);
}
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(s) && !memcmp(b, s, k);
}
static Result write_save(const char *path, const char *s) {           // 打開 → 定長 → 寫 → 關 | open -> set size -> write -> close
    uint64_t h; Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    h_fs_SetFileSize(h, (int64_t)strlen(s));
    r = h_fs_WriteFile(h, 0, s, strlen(s), NULL); h_fs_CloseFile(h); return r;
}
static int read_fs(const char *path, char *out, size_t n) { uint64_t h; size_t got = 0; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0; Result r = h_fs_ReadFile(&got, h, 0, out, n - 1); h_fs_CloseFile(h); out[got] = 0; return !r; }

// 剔零檔案的偏移換算（dm_data_pread）：原始內容 = 按位置算的固定字節序列，零段位置填 0；剔零文件 = 去掉零段。
// 期望值由「原始序列」直接取，不經被測實現。覆蓋：跨保留段 / 零段邊界、整段落在零段、開頭結尾、越過文件末尾。
// Offset translation for zero-stripped archives (dm_data_pread): original content = a fixed byte sequence computed from position, with 0 filled in at zero-run positions; zero-stripped file = zero runs removed.
// Expected values are taken directly from the "original sequence", not through the implementation under test. Covers: crossing kept-run / zero-run boundaries, a whole read falling in a zero run, start and end, reading past end of file.
static int test_holes(void) {
    enum { N = 100000 }; static uint8_t orig[N], got[N]; const long hole[3][2] = {{0, 1000}, {30000, 25000}, {99000, 1000}};   // 開頭、中間、結尾各一個零段 | one zero run each at the start, middle and end
    for (long i = 0; i < N; i++) orig[i] = (uint8_t)(i * 131 + (i >> 8) * 7 + 1);
    for (int h = 0; h < 3; h++) memset(orig + hole[h][0], 0, (size_t)hole[h][1]);
    char path[] = "/tmp/dm-holes.XXXXXX"; int tfd = mkstemp(path); if (tfd < 0) return 1; char hp[64]; snprintf(hp, sizeof hp, "%s.holes", path);
    long pos = 0; for (int h = 0; h <= 3; h++) { long end = h < 3 ? hole[h][0] : N; if (end > pos && write(tfd, orig + pos, (size_t)(end - pos)) != end - pos) return 1; if (h < 3) pos = hole[h][0] + hole[h][1]; }
    close(tfd); FILE *f = fopen(hp, "w"); fprintf(f, "size %d\n", N); for (int h = 0; h < 3; h++) fprintf(f, "%ld %ld\n", hole[h][0], hole[h][1]); fclose(f);
    int fd = open(path, O_RDONLY); dm_data_note_fd(fd, path); int bad = 0;
    const long q[][2] = {{0, 500}, {500, 1000}, {999, 2}, {1000, 29000}, {29990, 20}, {30000, 25000}, {40000, 100}, {54990, 20}, {55000, 44000}, {98990, 1010}, {99500, 500}, {99990, 100}, {0, N}, {100000, 10}, {123456, 10}};
    for (size_t k = 0; k < sizeof q / sizeof *q; k++) {
        long off = q[k][0], n = q[k][1], want = off >= N ? 0 : (off + n > N ? N - off : n);
        memset(got, 0xAA, sizeof got); ssize_t r = dm_data_pread(fd, got, (size_t)n, off);
        if (r != want || (want && memcmp(got, orig + off, (size_t)want))) { fprintf(stderr, "失敗：剔零讀 off=%ld n=%ld 返回 %zd（期望 %ld）\n", off, n, r, want); bad++; }
    }
    if (dm_data_logical_size(path, 49000) != N) { fprintf(stderr, "失敗：剔零檔案的原始大小\n"); bad++; }
    close(fd); g_hm[fd] = NULL; unlink(path); unlink(hp);
    printf("%s 剔零檔案偏移換算 15 種讀法逐字節等於原始內容\n", bad ? "✗" : "✓");
    return bad;
}
int main(void) {
    if (test_holes()) return 1;
    char root[1024], save[1100], work[1200], p[1200];
    const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_fs.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char rom[1100], overlay[1100], rdata[1200], odata[1200], fn[1300], got[64];
    snprintf(rom, sizeof rom, "%s/rom", root); snprintf(overlay, sizeof overlay, "%s/overlay", root); snprintf(rdata, sizeof rdata, "%s/data", rom); snprintf(odata, sizeof odata, "%s/data", overlay);
    mkdir(rom, 0755); mkdir(overlay, 0755); mkdir(rdata, 0755); mkdir(odata, 0755);
    snprintf(fn, sizeof fn, "%s/same", rdata); put(fn, "base"); snprintf(fn, sizeof fn, "%s/same", odata); put(fn, "overlay"); snprintf(fn, sizeof fn, "%s/base", rdata); put(fn, "only-base"); snprintf(fn, sizeof fn, "%s/over", odata); put(fn, "only-overlay");
    setenv("DM_ROMFS", rom, 1); setenv("DM_ROMFS_OVERLAY", overlay, 1); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "掛載 RomFS 兩層 overlay");
    CHECK(read_fs("rom:/data/same", got, sizeof got) && !strcmp(got, "overlay"), "overlay 文件優先"); CHECK(read_fs("rom:/data/base", got, sizeof got) && !strcmp(got, "only-base"), "overlay 缺失時回退基底");
    uint64_t rh; CHECK(h_fs_OpenFile(&rh, "rom:/data/same", MODE_WRITE) == RES_FAIL, "兩層掛載均拒絕寫入");
    {   // 覆蓋層裡找文件（dm_overlay_file）：兩層用冒號連著（老機型 = 包內覆蓋層 + 補充包），逐層找、前面的優先；把整串當一個目錄就哪個也找不到 | Find a file in the overlay (dm_overlay_file): two layers joined by a colon (old devices = in-bundle overlay + Legacy Device Pack), searched layer by layer with earlier ones taking priority; treating the whole string as one directory finds nothing
        char l2[1100], two[2300], want[1300]; snprintf(l2, sizeof l2, "%s/pack", root); mkdir(l2, 0755); snprintf(two, sizeof two, "%s:%s", overlay, l2); setenv("DM_ROMFS_OVERLAY", two, 1);
        snprintf(fn, sizeof fn, "%s/mark", overlay); put(fn, "1"); snprintf(fn, sizeof fn, "%s/only2", l2); put(fn, "2"); snprintf(fn, sizeof fn, "%s/mark", l2); put(fn, "2");
        snprintf(want, sizeof want, "%s/mark", overlay); CHECK(dm_overlay_file("mark", p, sizeof p) == 1 && !strcmp(p, want), "兩層都有：取第一層的");
        snprintf(want, sizeof want, "%s/only2", l2); CHECK(dm_overlay_file("only2", p, sizeof p) == 1 && !strcmp(p, want), "只有第二層有：取第二層的");
        snprintf(want, sizeof want, "%s/data/over", overlay); CHECK(dm_overlay_file("data/over", p, sizeof p) == 1 && !strcmp(p, want), "帶子目錄的相對路徑");
        CHECK(dm_overlay_file("none", p, sizeof p) == 0 && !p[0], "哪層都沒有：返回 0、輸出清空");
        setenv("DM_ROMFS_OVERLAY", overlay, 1); snprintf(want, sizeof want, "%s/mark", overlay); CHECK(dm_overlay_file("mark", p, sizeof p) == 1 && !strcmp(p, want), "單層（新機型）照舊");
        unsetenv("DM_ROMFS_OVERLAY"); CHECK(dm_overlay_file("mark", p, sizeof p) == 0, "沒設覆蓋層：返回 0"); setenv("DM_ROMFS_OVERLAY", overlay, 1); }
    g_nmnt = 0;
    snprintf(save, sizeof save, "%s/save", root); snprintf(work, sizeof work, "%s.work", save);
    mkdir(save, 0755); snprintf(p, sizeof p, "%s/a", save); put(p, "old");
    setenv("DM_SAVE", save, 1); setenv("DM_TEST_FS_FAIL", "write:5:28", 1);   // 存檔第 5 次寫失敗，errno 28 = ENOSPC | the 5th save write fails, errno 28 = ENOSPC
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "掛載存檔");

    // 存檔寫計數：①=第 1 次 ②=第 2 次 ③=第 3 次 ④=第 4/5/6/7 次 ⑤=第 8 次
    // Save write count: (1)=1st (2)=2nd (3)=3rd (4)=4th/5th/6th/7th (5)=8th
    printf("① pwrite 返回 0\n");
    g_mode = 1; g_calls = 0;
    CHECK(write_save("save:/a", "xyz") == RES_FAIL, "WriteFile 返回失敗 0x2A2");
    CHECK(g_calls == 1, "只調了 1 次 pwrite（實際 %d 次；原先無限重試）", g_calls);

    printf("② pwrite 分段寫\n");
    g_mode = 2; g_calls = 0;
    CHECK(write_save("save:/a", "abcdefg") == RES_OK, "WriteFile 成功");
    CHECK(g_calls == 2, "pwrite 調 2 次（3 字節 + 4 字節；實際 %d）", g_calls);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "abcdefg"), "工作目錄裡是完整的 abcdefg");
    g_mode = 0;

    printf("③ 事務\n");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 1 && g_lasterr == ENOSPC, "①②所在事務 Commit 被拒，前端通知 1 次 errno=ENOSPC（實際 %d 次 errno=%d）", g_nfail, g_lasterr);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "old"), "工作目錄丟棄、回到已提交版 old");
    CHECK(write_save("save:/a", "new") == RES_OK, "下一個事務：寫 new");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "old"), "Commit 前已提交目錄仍是 old");
    CHECK(h_fs_Commit("save") == RES_OK, "Commit");
    CHECK(same(p, "new"), "Commit 後已提交目錄是 new");
    snprintf(p, sizeof p, "%s.new", save); CHECK(access(p, F_OK) != 0, "換下來的舊版已刪（無 .new 殘留）");

    printf("④ DM_TEST_FS_FAIL=write:5:28\n");
    CHECK(write_save("save:/b", "4") == RES_PATH_NOT_FOUND, "（b 不存在，打開即失敗，不計寫）");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "4") == RES_OK, "第 4 次寫成功");
    errno = 0; CHECK(write_save("save:/b", "5") == RES_FAIL && errno == 28, "第 5 次寫失敗 errno=28（實際 errno=%d）", errno);
    CHECK(write_save("save:/b", "6") == RES_OK, "第 6 次寫成功");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 2 && g_lasterr == 28, "該事務 Commit 被拒，前端通知 errno=28（累計 %d 次）", g_nfail);
    snprintf(p, sizeof p, "%s/b", save); CHECK(access(p, F_OK) != 0, "已提交目錄不變（沒有 b）");
    snprintf(p, sizeof p, "%s/b", work); CHECK(access(p, F_OK) != 0, "工作目錄丟棄（沒有 b）");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "已提交目錄 a 仍是 new");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "7") == RES_OK && h_fs_Commit("save") == RES_OK, "下一個事務：寫 b=7，Commit 成功");
    snprintf(p, sizeof p, "%s/b", save); CHECK(same(p, "7"), "最終已提交 b = 7（下一個事務的內容）");
    CHECK(g_nfail == 2, "成功的事務不發通知（累計仍 %d 次）", g_nfail);

    printf("⑤ 重新掛載丟棄未提交內容\n");
    CHECK(write_save("save:/a", "v9") == RES_OK, "寫 a=v9 不提交");
    g_nmnt = 0;                                                        // 模擬進程重啓：掛載表清空 | simulate a process restart: clear the mount table
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "重新掛載");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "工作目錄 a 回到已提交版 new");
    snprintf(p, sizeof p, "%s/b", work); CHECK(same(p, "7"), "工作目錄 b = 已提交的 7");

    printf("⑥ 寫失敗收場不退出（DM_TEST_FS_FAILAT）\n");
    unsetenv("DM_TEST_FS_FAIL");
    // ⑥a 提交被拒後重新克隆失敗：不退出、工作目錄不動、已提交版不變；下次提交照樣拒絕並重新克隆成功；再下一個事務照常提交
    // 6a Recloning after a refused commit fails: do not exit, the work directory is untouched and the committed version unchanged; the next commit is refused again and the reclone succeeds; the transaction after that commits normally
    setenv("DM_TEST_FS_FAILAT", "reclone:1", 1);
    CHECK(write_save("save:/a", "bad") == RES_OK, "寫 a=bad");
    dm_fs_save_poison(ENOSPC); int nf0 = g_nfail;
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "本事務被拒（重新克隆注入失敗），進程還在，前端通知 1 次");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "已提交目錄 a 仍是 new");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "bad"), "工作目錄沒刪（a 還是 bad）");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 2, "仍是已污染：下一次提交照樣拒絕");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "這次重新克隆成了：工作目錄 a 回到 new");
    snprintf(p, sizeof p, "%s.re", work); CHECK(access(p, F_OK) != 0, "沒有 .re 殘留");
    CHECK(write_save("save:/a", "good") == RES_OK && h_fs_Commit("save") == RES_OK, "下一個事務寫 a=good，提交成功");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "已提交 a = good");
    // ⑥b 改名後寫客體名失敗、回退也失敗：不退出；文件在事務存檔裡 → 本事務提交被拒、已提交版不變；重新克隆後照常
    // 6b Writing the guest name after a rename fails and the rollback fails too: do not exit; the file is in a transactional save, so this transaction's commit is refused and the committed version is unchanged; after recloning everything works
    setenv("DM_TEST_FS_FAILAT", "guestname:1,renameback:1", 1);
    h_fs_CreateFile("save:/c", 0); CHECK(write_save("save:/c", "c") == RES_OK, "寫 c");
    CHECK(h_fs_RenameFile("save:/c", "save:/d") != RES_OK, "改名 c → d 返回失敗（兩步都注入失敗），進程還在");
    nf0 = g_nfail; CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "本事務提交被拒");
    snprintf(p, sizeof p, "%s/c", save); CHECK(access(p, F_OK) != 0, "已提交目錄沒有 c");
    snprintf(p, sizeof p, "%s/d", save); CHECK(access(p, F_OK) != 0, "已提交目錄沒有 d");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "已提交 a 仍是 good");
    CHECK(write_save("save:/a", "ok2") == RES_OK && h_fs_Commit("save") == RES_OK, "下一個事務照常提交");
    // ⑥c 掛載時克隆工作目錄失敗：不退出，改成只讀掛載已提交目錄——讀得到、寫不進、提交是空操作、已提交版不變、前端通知 1 次；下次正常掛載照舊
    // 6c Cloning the work directory at mount fails: do not exit; mount the committed directory read-only instead. Reads work, writes fail, commits are no-ops, the committed version is unchanged, and the front end is notified once; the next normal mount works as before
    setenv("DM_TEST_FS_FAILAT", "mountclone:1", 1); g_nmnt = 0; nf0 = g_nfail;
    snprintf(p, sizeof p, "rm -rf '%s'", work); system(p);
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && g_nfail == nf0 + 1, "掛載返回成功、前端通知 1 次（實際 %d 次）", g_nfail - nf0);
    CHECK(read_fs("save:/a", got, sizeof got) && !strcmp(got, "ok2"), "只讀掛載下讀得到已提交的 a=ok2（讀到 %s）", got);
    CHECK(write_save("save:/a", "zzz") != RES_OK, "寫不進去");
    CHECK(h_fs_Commit("save") == RES_OK, "提交是空操作");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok2"), "已提交 a 仍是 ok2");
    unsetenv("DM_TEST_FS_FAILAT"); g_nmnt = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && write_save("save:/a", "ok3") == RES_OK && h_fs_Commit("save") == RES_OK, "去掉注入重新掛載：事務式照常，寫 a=ok3 提交成功");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok3"), "已提交 a = ok3");


    // 語言開關改寫設置文件（settings_lang_apply）：期望值按規則手算——只在「與開關不符」時把 Language / VoiceLanguage 改成 0（英文）或 8（簡中），別的鍵和別的語言不動
    // Language switch rewriting the settings file (settings_lang_apply): expected values computed by hand from the rule -- only when it "does not match the switch" are Language / VoiceLanguage changed to 0 (English) or 8 (Simplified Chinese); other keys and other languages are untouched
    { extern int g_t_lang; char sd[1200], sf[1300], gotj[512]; snprintf(sd, sizeof sd, "%s/langt", root); mkdir(sd, 0755); snprintf(sf, sizeof sf, "%s/Settings", sd); mkdir(sf, 0755); snprintf(sf, sizeof sf, "%s/Settings/Settings.json", sd);
      #define LANGCASE(dm, in, want, what) do { g_t_lang = (dm); put(sf, (in)); settings_lang_apply(sd); FILE *lf = fopen(sf, "rb"); size_t ln_ = lf ? fread(gotj, 1, sizeof gotj - 1, lf) : 0; if (lf) fclose(lf); gotj[ln_] = 0; CHECK(!strcmp(gotj, (want)), "語言開關：%s", (what)); } while (0)
      LANGCASE(-1, "{ \"Language\": 8, \"VoiceLanguage\": 8 }", "{ \"Language\": 8, \"VoiceLanguage\": 8 }", "沒點過應用：一個字不動");
      LANGCASE(1, "{ \"A\": 1, \"Language\": 8, \"VoiceLanguage\": 8, \"B\": 2 }", "{ \"A\": 1, \"Language\": 0, \"VoiceLanguage\": 0, \"B\": 2 }", "開關 = 英文、文件是簡中：文字和語音都改成 0，別的鍵不動");
      LANGCASE(0, "{ \"Language\": 0, \"VoiceLanguage\": 0 }", "{ \"Language\": 8, \"VoiceLanguage\": 8 }", "開關 = 中文、文件是英文：都改成 8");
      LANGCASE(0, "{ \"Language\": 9, \"VoiceLanguage\": 9 }", "{ \"Language\": 9, \"VoiceLanguage\": 9 }", "開關 = 中文、玩家在原版裡選了繁中：不改回簡中");
      LANGCASE(1, "{ \"Language\": 0, \"VoiceLanguage\": 0 }", "{ \"Language\": 0, \"VoiceLanguage\": 0 }", "開關 = 英文、文件已是英文：不動");
      LANGCASE(1, "{ \"Language\": 0, \"VoiceLanguage\": 8 }", "{ \"Language\": 0, \"VoiceLanguage\": 0 }", "開關 = 英文、文字已英文但語音還是中文：語音跟上");
      LANGCASE(1, "{ \"Language\": 12 }", "{\n    \"VoiceLanguage\": 0, \"Language\": 0 }", "開關 = 英文、兩位數的語言號且沒有語音鍵：改寫並補上語音鍵");
      g_t_lang = -1; }

    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); system(cmd);
    printf("== %d 項，失敗 %d ==\n", checks, fails);
    return fails != 0;
}
