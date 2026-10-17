// 畫面寬高比的解析與輸出尺寸放寬（見 dm_aspect.h）。純函數，不碰遊戲檔案：檔案的尺寸與上限由前端傳進來，單測直接調
// Parsing the picture aspect ratio and widening the output size (see dm_aspect.h). Pure functions that do not touch the game profile: the front end passes in the profile's sizes and cap, and the unit test calls them directly
#include "dm_aspect.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

double dm_aspect_parse(const char *s, double screen) {
    if (!s || !*s) return 0;
    if (!strcasecmp(s, "screen")) return screen > 0 ? screen : 0;
    double a, b; char sep, end;
    if (sscanf(s, "%lf%c%lf%c", &a, &sep, &b, &end) == 3 && (sep == ':' || sep == 'x' || sep == 'X') && a > 0 && b > 0) return a / b;
    if (sscanf(s, "%lf%c", &a, &end) == 1 && a > 0) return a;
    return 0;
}

uint64_t dm_aspect_res(unsigned w, unsigned h, double max_aspect, double min_aspect, double aspect) {
    unsigned ow = w, oh = h;
    if (h && aspect * h > w && max_aspect * h > w) {   // 比 16:9 寬：加寬 | wider than 16:9: widen
        double cap = fmin(aspect, max_aspect) * h;
        ow = (unsigned)((cap + 1e-6) / 4) * 4;   // 往下取 4 的倍數，不超上限；1e-6 吸收 2580 / 1080 × 1080 這類浮點誤差 | round down to a multiple of 4 so the cap is never exceeded; 1e-6 absorbs float error such as 2580 / 1080 × 1080
        if (ow < w) ow = w;
    } else if (h && aspect > 0 && aspect * h < w && min_aspect > 0 && min_aspect * h < w) {   // 比 16:9 高：加高 | taller than 16:9: grow taller
        double cap = w / fmax(aspect, min_aspect);
        oh = (unsigned)((cap + 1e-6) / 2) * 2;
        if (oh < h) oh = h;
    }
    return (uint64_t)oh << 32 | ow;
}
