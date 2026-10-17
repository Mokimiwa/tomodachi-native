// 存檔備份：遊戲的存檔在 Documents/save，備份在 Documents/Backups/<時間>[ (種類)]（「檔案」App 看得到）。介面在 duo_shell.h。
//   這裡只搬、只拷，從不刪存檔：「從頭開始」和「還原」都先把目前的存檔整個搬進備份，再在下次開機、宿主掛上存檔之前做（main.m 調 duo_save_apply_pending）。
//   遊戲執行中拍備份時，經 duo_game_save_snapshot 由宿主持著提交鎖拍已提交版（hle_fs.c dm_save_snapshot），不會拍到提交做到一半；宿主還沒掛存檔時直接拷 Documents/save。
//   要開機才做的事記在本 App 設定 SavePending：@"fresh" 或 @"restore:<備份名>"。
// Save backups: the game's save is in Documents/save and the backups in Documents/Backups/<time>[ (kind)] (visible in the Files app). Interface in duo_shell.h.
//   Nothing here ever deletes a save, it only moves and copies: Start Over and Restore first move the whole current save into a backup, and run at the next boot, before the host mounts the save (main.m calls duo_save_apply_pending).
//   A backup taken while the game runs goes through duo_game_save_snapshot, where the host snapshots the committed version under the commit lock (hle_fs.c dm_save_snapshot), so it never catches a commit halfway; before the host has mounted the save, Documents/save is copied directly.
//   What waits for the next boot is kept in this app's setting SavePending: @"fresh" or @"restore:<backup name>".
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <errno.h>
#include <sys/clonefile.h>
#include "duo_shell.h"

// 宿主還沒掛存檔（或預覽 App）時返回 -1，由這裡自己拷；遊戲 App 給強實現（front_ios.m → dm_save_snapshot）
// Returns -1 when the host has not mounted the save yet (or in the preview app), and the copy is made here; the game app provides the strong version (front_ios.m -> dm_save_snapshot)
__attribute__((weak)) int duo_game_save_snapshot(const char *dst) { (void)dst; return -1; }

static NSString *doc_dir(void) { return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject; }
static NSString *save_dir(void) { return [doc_dir() stringByAppendingPathComponent:@"save"]; }
static NSString *backup_dir(void) { return [doc_dir() stringByAppendingPathComponent:@"Backups"]; }
static NSString *const k_kind_restore = @" (before restore)", *const k_kind_fresh = @" (before starting over)";   // 資料夾名的尾巴，「檔案」App 裡看得到 | folder name suffixes, seen in the Files app

static NSDateFormatter *stamp_fmt(void) {
    static NSDateFormatter *f; static dispatch_once_t once;
    dispatch_once(&once, ^{ f = [NSDateFormatter new]; f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]; f.dateFormat = @"yyyy-MM-dd HH.mm.ss"; });
    return f;
}
static BOOL has_files(NSString *dir) {   // 目錄裡有沒有任何一般檔案（空的存檔目錄不值得備份） | whether the directory holds any regular file (an empty save directory is not worth a backup)
    NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtURL:[NSURL fileURLWithPath:dir] includingPropertiesForKeys:@[NSURLIsRegularFileKey] options:0 errorHandler:nil];
    for (NSURL *u in e) { NSNumber *reg = nil; [u getResourceValue:&reg forKey:NSURLIsRegularFileKey error:nil]; if (reg.boolValue) return YES; }
    return NO;
}
static NSString *fresh_name(NSString *kind) {   // 同一秒裡第二份加序號 | a second one within the same second gets a number
    [NSFileManager.defaultManager createDirectoryAtPath:backup_dir() withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *base = [[stamp_fmt() stringFromDate:NSDate.date] stringByAppendingString:kind], *name = base;
    for (int i = 2; [NSFileManager.defaultManager fileExistsAtPath:[backup_dir() stringByAppendingPathComponent:name]]; i++) name = [NSString stringWithFormat:@"%@ %d", base, i];
    return name;
}
static int copy_tree(NSString *from, NSString *to) {   // APFS 寫時複製，不行就一般複製 | APFS copy-on-write, else a plain copy
    if (!clonefile(from.fileSystemRepresentation, to.fileSystemRepresentation, 0)) return 0;
    int e = errno; NSError *err = nil;
    if ([NSFileManager.defaultManager copyItemAtPath:from toPath:to error:&err]) return 0;
    fprintf(stderr, "【紅】存檔備份：拷不了 %s → %s（clonefile errno=%d；%s）\n", from.UTF8String, to.UTF8String, e, err.localizedDescription.UTF8String);
    return e ? e : EIO;
}

NSArray<NSDictionary *> *duo_backups(void) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:backup_dir() error:nil]) {
        if (n.length < 19 || [n hasPrefix:@"."]) continue;
        NSDate *d = [stamp_fmt() dateFromString:[n substringToIndex:19]]; if (!d) continue;   // 只認本 App 起的名字 | only names this app made
        BOOL dir = NO; if (![NSFileManager.defaultManager fileExistsAtPath:[backup_dir() stringByAppendingPathComponent:n] isDirectory:&dir] || !dir) continue;
        NSString *kind = [n containsString:k_kind_restore] ? @"restore" : [n containsString:k_kind_fresh] ? @"fresh" : @"";
        [out addObject:@{@"name": n, @"date": d, @"kind": kind}];
    }
    [out sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) { return [b[@"name"] compare:a[@"name"]]; }];   // 名字以時間開頭：新的在前 | names start with the time: newest first
    return out;
}

// 目前的存檔拍一份到 dst（dst 不能已存在）；沒有存檔時返回 ENOENT。先看盤上有沒有檔案：宿主總會建好存檔目錄，空目錄也拍得成 | take a copy of the current save at dst (dst must not exist); ENOENT when there is no save. Check for files first: the host always creates the save directory, and an empty one snapshots fine
static int snapshot_to(NSString *dst) {
    if (!has_files(save_dir())) return ENOENT;
    int r = duo_game_save_snapshot(dst.fileSystemRepresentation);
    return r >= 0 ? r : copy_tree(save_dir(), dst);
}
// 先拷進點開頭的暫存名（列表不收），成了才改成正式名：拷到一半被殺，不會留下一份看起來完整的備份 | copy into a dot-prefixed staging name first (the list skips those) and rename only on success: a copy killed halfway never leaves a backup that looks complete
static int staged_copy(NSString *dst, int (^copy)(NSString *tmp)) {
    NSString *tmp = [dst.stringByDeletingLastPathComponent stringByAppendingPathComponent:[NSString stringWithFormat:@".%@.part", dst.lastPathComponent]];
    [NSFileManager.defaultManager removeItemAtPath:tmp error:nil];   // 上次沒拷完的暫存 | a staging copy left from an earlier attempt
    int r = copy(tmp);
    if (!r && rename(tmp.fileSystemRepresentation, dst.fileSystemRepresentation)) r = errno ? errno : EIO;
    if (r) [NSFileManager.defaultManager removeItemAtPath:tmp error:nil];
    return r;
}
NSString *duo_backup_now(int *err) {
    NSString *name = fresh_name(@""), *dst = [backup_dir() stringByAppendingPathComponent:name];
    int r = staged_copy(dst, ^int(NSString *tmp) { return snapshot_to(tmp); }); if (err) *err = r;
    fprintf(stderr, "存檔備份：%s %s（%d）\n", r ? "沒做成" : "已備份到", name.UTF8String, r);
    return r ? nil : name;
}
NSURL *duo_save_export_copy(int *err) {   // 匯出用的拷貝放在暫存目錄，匯出完由調用方刪（那是這份拷貝，不是存檔） | the copy for exporting sits in the temporary directory; the caller removes it after export (it is that copy, not the save)
    NSString *app = NSBundle.mainBundle.infoDictionary[@"CFBundleDisplayName"] ?: @"Save";
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"export"];
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@ save %@", [app stringByReplacingOccurrencesOfString:@"/" withString:@"-"], [stamp_fmt() stringFromDate:NSDate.date]]];
    int r = snapshot_to(dst); if (err) *err = r;
    return r ? nil : [NSURL fileURLWithPath:dst];
}

NSString *duo_save_pending(void) { return [NSUserDefaults.standardUserDefaults stringForKey:@"SavePending"]; }
void duo_save_set_pending(NSString *action) {
    if (action) [NSUserDefaults.standardUserDefaults setObject:action forKey:@"SavePending"]; else [NSUserDefaults.standardUserDefaults removeObjectForKey:@"SavePending"];
    fprintf(stderr, "存檔：下次開機%s\n", action ? action.UTF8String : "不做什麼");
}
// 開機時的待辦沒做成：記下來，存檔頁打開時告訴玩家一次 | a boot-time action that failed: noted, and told to the player once when the save page opens
static void note_failed(NSString *what) { [NSUserDefaults.standardUserDefaults setObject:what forKey:@"SaveFailed"]; }
NSString *duo_save_take_failure(void) {
    NSString *f = [NSUserDefaults.standardUserDefaults stringForKey:@"SaveFailed"];
    if (f) [NSUserDefaults.standardUserDefaults removeObjectForKey:@"SaveFailed"];
    return f;
}

// 開機時（宿主掛存檔之前）。每一步都是改名（同一卷上是原子的）或拷進暫存名，被殺在任何一步，存檔要麼是原來的、要麼是換好的：
//   還原：先把備份拷成 save.restoring；成了才把目前的存檔改名搬進 Backups，再把 save.restoring 改名成 save（失敗就把原存檔搬回來）。
//   從頭開始：把目前的存檔改名搬進 Backups（沒有檔案、只有空資料夾的也搬，用點開頭的名字，列表不收）。
//   待辦做完（或確定做不成）才清；做到一半被殺，下次開機再做一次（最多多留一份備份，不會丟）
// At boot (before the host mounts the save). Every step is a rename (atomic on one volume) or a copy into a staging name, so a kill at any step leaves either the original save or the replaced one:
//   Restore: first copy the backup into save.restoring; only then rename the current save into Backups and rename save.restoring to save (moving the original back on failure).
//   Start Over: rename the current save into Backups (one holding only empty folders too, under a dot-prefixed name the list skips).
//   The pending action is cleared only once done (or certain to fail); killed halfway, the next boot does it again (at worst one extra backup, never a loss)
void duo_save_apply_pending(void) {
    NSString *p = duo_save_pending(); if (!p) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL restore = [p hasPrefix:@"restore:"];
    NSString *src = restore ? [backup_dir() stringByAppendingPathComponent:[p substringFromIndex:8]] : nil, *stage = [doc_dir() stringByAppendingPathComponent:@"save.restoring"];
    if ((restore && ![fm fileExistsAtPath:src]) || (!restore && ![p isEqualToString:@"fresh"])) {
        fprintf(stderr, "【紅】存檔：待辦 %s 做不了（備份不在了？），存檔不動\n", p.UTF8String); note_failed(restore ? @"restore" : @"fresh"); duo_save_set_pending(nil); return;
    }
    if (restore) {
        [fm removeItemAtPath:stage error:nil];   // 上次沒拷完的暫存（備份本身還在 Backups） | a staging copy left from an earlier attempt (the backup itself is still in Backups)
        if (copy_tree(src, stage)) { [fm removeItemAtPath:stage error:nil]; fprintf(stderr, "【紅】存檔：備份拷不出來，存檔不動\n"); note_failed(@"restore"); duo_save_set_pending(nil); return; }
    }
    NSString *kept = nil;
    if ([fm fileExistsAtPath:save_dir()]) {
        BOOL any = has_files(save_dir());
        NSString *name = fresh_name(restore ? k_kind_restore : k_kind_fresh); if (!any) name = [@".empty " stringByAppendingString:name];
        kept = [backup_dir() stringByAppendingPathComponent:name];
        if (rename(save_dir().fileSystemRepresentation, kept.fileSystemRepresentation)) {
            fprintf(stderr, "【紅】存檔：搬不動目前的存檔（errno %d），存檔不動\n", errno);
            [fm removeItemAtPath:stage error:nil]; note_failed(restore ? @"restore" : @"fresh"); duo_save_set_pending(nil); return;
        }
        fprintf(stderr, "存檔：目前的存檔搬到 %s\n", name.UTF8String);
    }
    if (restore && rename(stage.fileSystemRepresentation, save_dir().fileSystemRepresentation)) {
        int e = errno;
        if (kept && !rename(kept.fileSystemRepresentation, save_dir().fileSystemRepresentation)) fprintf(stderr, "【紅】存檔：還原沒做成（errno %d），原來的存檔已搬回\n", e);
        else fprintf(stderr, "【紅】存檔：還原沒做成（errno %d），原來的存檔在 %s\n", e, kept ? kept.lastPathComponent.UTF8String : "（本來就沒有）");
        [fm removeItemAtPath:stage error:nil]; note_failed(@"restore"); duo_save_set_pending(nil); return;
    }
    fprintf(stderr, restore ? "存檔：已還原 %s\n" : "存檔：從頭開始%s\n", restore ? src.lastPathComponent.UTF8String : "");
    duo_save_set_pending(nil);
}
