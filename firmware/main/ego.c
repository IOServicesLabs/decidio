// ego.c - the camera's own shift between two frames, for models trained with `motion: ego` (a drone, a car, a hand).
//
// Block matching on luma: a 2x-downsampled search over +-6 px (+-12 px at 96x96), then +-1 px at full resolution
// around it. The motion channel is then |now - earlier frame moved back by that shift|, so a pan, tilt, drift or shake
// leaves it dark and only things that move on their own (or near things, by parallax) light it up.
// Integer arithmetic, same loop order and tie rule as decidio/ego.py (tests/test_ego_parity.py checks bit-exactness).
#include <stdint.h>
#include <stdlib.h>

#include "rt.h"

#define EGO_R 6
#define EGO_M1 (2 * EGO_R + 1)
#define HALF (RT_CAM / 2)

static uint8_t s_hc[HALF * HALF], s_hp[HALF * HALF];

static void half(const uint8_t *l, uint8_t *h)
{
    for (int y = 0; y < HALF; y++) {
        const uint8_t *a = l + (2 * y) * RT_CAM, *b = a + RT_CAM;
        for (int x = 0; x < HALF; x++) {
            h[y * HALF + x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1]) >> 2);
        }
    }
}

// sum |a(x, y) - b(x + dx, y + dy)| over the n x n square at (o, o)
static uint32_t sad(const uint8_t *a, const uint8_t *b, int stride, int o, int n, int dx, int dy)
{
    uint32_t s = 0;
    for (int y = o; y < o + n; y++) {
        const uint8_t *pa = a + y * stride + o, *pb = b + (y + dy) * stride + o + dx;
        for (int x = 0; x < n; x++) {
            s += (uint32_t)abs((int)pa[x] - (int)pb[x]);
        }
    }
    return s;
}

void rt_ego_shift(const uint8_t *cur, const uint8_t *prev, int *dx, int *dy)
{
    half(cur, s_hc);
    half(prev, s_hp);
    const int n = HALF - 2 * EGO_R;
    uint32_t best = sad(s_hc, s_hp, HALF, EGO_R, n, 0, 0);
    int bx = 0, by = 0;
    for (int y = -EGO_R; y <= EGO_R; y++) {
        for (int x = -EGO_R; x <= EGO_R; x++) {
            const uint32_t s = sad(s_hc, s_hp, HALF, EGO_R, n, x, y);
            if (s < best) {
                best = s;
                bx = x;
                by = y;
            }
        }
    }
    const int nf = RT_CAM - 2 * EGO_M1, cx = 2 * bx, cy = 2 * by;
    int rx = cx, ry = cy;
    best = sad(cur, prev, RT_CAM, EGO_M1, nf, cx, cy);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            const uint32_t s = sad(cur, prev, RT_CAM, EGO_M1, nf, cx + x, cy + y);
            if (s < best) {
                best = s;
                rx = cx + x;
                ry = cy + y;
            }
        }
    }
    *dx = rx;
    *dy = ry;
}
