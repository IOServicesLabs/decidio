// reads: int32 size, n; then n pairs of 96x96 luma (cur, prev); writes per pair int32 dx, dy and size*size motion bytes
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"
int main(void) {
    int hdr[2];
    if (fread(hdr, 4, 2, stdin) != 2) return 1;
    const int size = hdr[0], n = hdr[1];
    static uint8_t cur[RT_CAM * RT_CAM], prev[RT_CAM * RT_CAM], out[RT_CAM * RT_CAM];
    for (int k = 0; k < n; k++) {
        if (fread(cur, 1, sizeof cur, stdin) != sizeof cur || fread(prev, 1, sizeof prev, stdin) != sizeof prev) return 2;
        int dx, dy;
        rt_ego_shift(cur, prev, &dx, &dy);
        int i = 0;
        for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) out[i++] = rt_ego_diff(cur, prev, x * RT_CAM / size, y * RT_CAM / size, dx, dy);
        int d[2] = { dx, dy };
        fwrite(d, 4, 2, stdout); fwrite(out, 1, size * size, stdout);
    }
    return 0;
}
