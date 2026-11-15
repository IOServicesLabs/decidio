// Part of decidio's fly eye (firmware/main/fly). From the fruitfly firmware, adapted for this runtime: see fly/README.md.
// fly_eye.h — the fly's optic lobe on the XIAO's camera: a depth map of the scene from motion parallax.
//
// A fly has one way to see depth with a single fixed-focus eye: move, and watch how much each part of the
// world slides across the retina.  Near things slide fast, far things hardly move (the same peering trick a
// locust does before it jumps; the same reason a fly flies in straight segments between saccades — pure
// translation makes the flow field a depth map).  This file is that pathway, neuron type by neuron type:
//
//   photoreceptors R1-6   the 160x120 frame pooled into 40x30 ommatidia (each averages a 4x4 patch of pixels),
//                         then blurred by the ommatidium's acceptance angle (about one column)
//   lamina L1/L2          centre-minus-surround contrast, divided by the mean light (gain control): a Weber signal
//   medulla Mi1/Tm1 …     the signal split into an ON and an OFF channel, and a one-frame delayed copy of each
//   T4 (ON) / T5 (OFF)    Hassenstein-Reichardt correlators between neighbouring columns: delayed(here) x now(next)
//                         minus now(here) x delayed(next) — positive for motion toward the next column
//   lobula plate          the correlator outputs pooled over a patch of columns and divided by the patch's spatial
//                         contrast energy (divisive gain control, its floor adapting to the scene's typical contrast)
//                         — a local velocity in columns per frame;
//                         wide-field tangential cells (HS, VS, and expansion / roll matched filters) fit the fly's
//                         own motion: sideways, up/down, forward (expansion) and roll
//   depth                 for translation the flow at a column is (self-motion) / (distance): the ratio between the
//                         measured flow and the tangential cells' prediction is the column's inverse depth
//                         relative to the scene average.  A leaky integrator per column holds the 3D map
//                         between movements; a certainty value says how much parallax each column has seen.
//
// Three eyes at three scales (40x30, 20x15 and 10x8 columns) cover slow and fast hand movements: a correlator
// saturates past about a third of a column of displacement per frame, so once an eye's own estimate nears that
// limit the next coarser eye is trusted instead.
// No Arduino dependencies: eye_test.cpp runs this on a PC against a synthetic scene.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "optic_lobe_data.h"   // T4/T5 subtype counts from the male CNS connectome: the gains of the four directions and of ON vs OFF

#ifndef FLY_MALLOC
#include <stdlib.h>
#define FLY_MALLOC(n) malloc(n)
#endif
#ifndef FLY_MALLOC_FAST
#define FLY_MALLOC_FAST(n) FLY_MALLOC(n)   // the board: internal RAM when there is room (fly_claims.ino); a PC: the same as FLY_MALLOC
#endif

namespace flyeye {

static const int W = 160, H = 120;                       // camera frame for see(): 4x4 pixels per ommatidium
static const int SW = 80, SH = 60;                       // camera frame for seeSmall(): 2x2 pixels per ommatidium (the board: one pixel per 8x8 JPEG block)
static const int EW = 40, EH = 30, NE = EW * EH;         // fine eye: the retina
static const int CW = 20, CH = 15, NC = CW * CH;         // coarse eye: 2x2 ommatidia each
static const int XW = 10, XH = 8, NX = XW * XH;          // coarsest eye: 4x4 ommatidia each (its bottom row is half filled)
static const int WIN_FINE = 2, WIN_COARSE = 1;           // correlators pooled over a (2*WIN+1)^2 patch of columns
static const float SAT_LO = 0.18f, SAT_HI = 0.30f;       // an eye's estimate (columns/frame) beyond which the next coarser eye is trusted
static const float DEN_FLOOR = 0.1f;                     // divisive normalisation floor, as a fraction of the scene's typical patch contrast
static const float NOISE_GREY = 1.0f;                    // the camera's pixel noise after 4x4 pooling, in grey levels: contrast below it is not seen
static const float FLOW_GAIN = 0.85f;                    // correlator ratio → columns/frame; 0.5 for a sinusoid, more for natural 1/f textures (calibrated in eye_test)
static const float FIT_CAP = 1.5f;                       // a column's weight in the self-motion fit saturates (a few dark objects must not steer the fit)
static const float DEPTH_RATE = 0.35f;                   // how fast a column's depth follows new parallax
static const float DEPTH_DECAY = 0.001f;                 // per frame, the map relaxes toward the scene average
static const float INFO_HALF = 0.003f;                   // parallax evidence (|P|^2 x relative contrast) at half update gain

struct Level {
  int w, h, n, win; float cfRef = 0;                     // running mean of the patch contrast energy (contrast gain control)
  float *r, *lam, *on, *off, *dOn, *dOff;                // photoreceptor, lamina, ON, OFF, delayed (previous frame) ON, OFF
  float *nx, *dx, *ny, *dy;                              // per-column correlator numerator / spatial-contrast denominator
  float *vx, *vy, *cf;                                   // pooled flow (columns per frame) and contrast energy
  float *tmp, *sum;                                      // scratch for blurs and box sums
  float *cxy, *jxx, *jyy, *jxy; float eps = 0;           // the correlators' pooled contrast tensor (x, y, cross) and their normalisation floor: what they see of a motion
  bool alloc(int w_, int h_, int win_, bool fast = false) {
    w = w_; h = h_; n = w * h; win = win_; float** arr[] = { &r, &lam, &on, &off, &dOn, &dOff, &nx, &dx, &ny, &dy, &vx, &vy, &cf, &tmp, &sum, &cxy, &jxx, &jyy, &jxy };
    for (float** a : arr) { *a = (float*)(fast ? FLY_MALLOC_FAST(sizeof(float) * n) : FLY_MALLOC(sizeof(float) * n)); if (!*a) return false; memset(*a, 0, sizeof(float) * n); }
    return true;
  }
};

static Level fine, coarse, coarsest; static bool ready = false;
static float* invD = nullptr; static float* cert = nullptr;   // the 3D map: inverse depth relative to the scene (1 = average), certainty 0..1
static float brightness = 0, hs = 0, vs = 0, expansion = 0, roll = 0, flowMag = 0, support = 0;   // tangential cells (fine columns per frame); support = share of the field agreeing with them
static uint32_t frames = 0;
static float *flowX = nullptr, *flowY = nullptr, *contrast = nullptr;   // the merged flow field (columns/frame) and relative patch contrast, per fine column
static float *rhoVar = nullptr;                        // per column: variance of the flow ratio about the settled depth (the map's noise)
static float *fitW = nullptr;                          // per column: its weight in the self-motion fit (scratch)
static bool moving = false;                            // a camera that travels (a rail, a train): the maps move with the scene (advection), see fly_figure.h
static const float ADVECT_TOL = 0.4f, ADVECT_SLACK = 0.03f;   // advection: a carried depth whose prediction misses the measured flow by more than this share (+ slack, columns) is dropped
static float advected = 0;                             // columns the map was shifted by last frame (RMS), for diagnostics
static float *scratch = nullptr;                       // the moving camera's scratch (allocated when the mode is first used): 9 maps
static float *fresh = nullptr, *prevR = nullptr;       // moving camera: content with no past — it does not match the previous frame where its flow says it came from (uncovered behind a near thing, or just entered): its flow is no evidence
static const float FRESH_GREY = 18.0f;                 // grey levels of mismatch that make content fresh
static const float CX0 = (EW - 1) * 0.5f, CY0 = (EH - 1) * 0.5f;
// a body that turns (a drone; decidio's fly_drone.h): its gyro says how the view rotates between two frames.  With
// derotate on, each eye's delay line is moved by that rotation before the correlators compare it with the new frame, so
// the flow they report is the translation alone - what the depth map, the figures and time-to-contact need - and a
// fast turn no longer saturates them.  rotUx / rotUy: the rotation's flow at the centre (fine columns per frame), rotR:
// roll (the fit's units), rotF: focal length in fine columns.  rotFX / rotFY: the rotation's flow at every fine column,
// added back wherever content is followed across the retina.  Off (the default), nothing changes.
static bool derotate = false; static float rotUx = 0, rotUy = 0, rotR = 0, rotF = 34.6f;
static float *rotFX = nullptr, *rotFY = nullptr;                // allocated the first time the eye derotates (PSRAM on the board)
static float* rotRemX[3] = { nullptr, nullptr, nullptr }; static float* rotRemY[3] = { nullptr, nullptr, nullptr };   // per eye: the rotation the whole-column shift left over
static inline int levelIndex(const Level& L) { return &L == &fine ? 0 : (&L == &coarse ? 1 : 2); }
static bool rotRem(Level& L) {   // the remainder maps, allocated the first time the eye derotates
  const int k = levelIndex(L); if (rotRemX[k]) return true;
  rotRemX[k] = (float*)FLY_MALLOC(sizeof(float) * L.n); rotRemY[k] = (float*)FLY_MALLOC(sizeof(float) * L.n);
  if (!rotRemX[k] || !rotRemY[k]) { rotRemX[k] = rotRemY[k] = nullptr; return false; }
  memset(rotRemX[k], 0, sizeof(float) * L.n); memset(rotRemY[k], 0, sizeof(float) * L.n); return true;
}
static inline void rotAt(float X, float Y, float& u, float& v) { const float f2 = rotF * rotF; u = rotUx * (1 + X * X / f2) + rotUy * (X * Y / f2) - rotR * Y / EW; v = rotUx * (X * Y / f2) + rotUy * (1 + Y * Y / f2) + rotR * X / EW; }
// the flow the tangential cells predict at column i for a surface at the scene's average distance (translation part only)
static inline void predict(int i, float& px, float& py) { float dx = (i % EW - CX0) / EW, dy = (i / EW - CY0) / EW; px = hs + expansion * dx; py = vs + expansion * dy; }
// the measured flow at column i with the roll component removed
static inline void measured(int i, float& mx, float& my) { float dx = (i % EW - CX0) / EW, dy = (i / EW - CY0) / EW; mx = flowX[i] + roll * dy; my = flowY[i] - roll * dx; }

static bool begin() {
  if (!fine.alloc(EW, EH, WIN_FINE, true) || !coarse.alloc(CW, CH, WIN_COARSE, true) || !coarsest.alloc(XW, XH, WIN_COARSE, true)) return false;   // the levels are the hot path: internal RAM when there is room
  invD = (float*)FLY_MALLOC(sizeof(float) * NE); cert = (float*)FLY_MALLOC(sizeof(float) * NE); if (!invD || !cert) return false;
  { float** maps[] = { &flowX, &flowY, &contrast, &rhoVar, &fitW };   // on the heap (the board: PSRAM), not in the scarce internal RAM
    for (float** a : maps) { *a = (float*)FLY_MALLOC(sizeof(float) * NE); if (!*a) return false; memset(*a, 0, sizeof(float) * NE); } }
  for (int i = 0; i < NE; i++) { invD[i] = 1; cert[i] = 0; }
  ready = true; return true;
}
static void reset() {
  if (!ready) return; for (int i = 0; i < NE; i++) { invD[i] = 1; cert[i] = 0; if (scratch) { fresh[i] = 0; prevR[i] = 0; } } frames = 0;
  for (Level* L : { &fine, &coarse, &coarsest }) { memset(L->dOn, 0, sizeof(float) * L->n); memset(L->dOff, 0, sizeof(float) * L->n); L->cfRef = 0; }
}

// the displacement of the scene content at column i last frame, from the self-motion fit and the depth map (fine columns)
static inline void displacement(int i, float& vx, float& vy) { float px, py; predict(i, px, py); float dx = (i % EW - CX0) / EW, dy = (i / EW - CY0) / EW; vx = invD[i] * px - roll * dy + (rotFX ? rotFX[i] : 0); vy = invD[i] * py + roll * dx + (rotFY ? rotFY[i] : 0); }
// a camera that travels: the depth map is a map of the scene, not of the retina, so when the scene slides across the
// retina the map slides with it.  Each column takes the value that was at the place its content came from (backward
// lookup, bilinear); a column whose content came from outside the view gets no certainty — its depth is unknown until
// parallax has been seen there.  Called before the depth update, so the update refines a map that is already aligned.
static bool scratchReady() { if (!scratch) { scratch = (float*)FLY_MALLOC(sizeof(float) * NE * 9); if (!scratch) return false; memset(scratch, 0, sizeof(float) * NE * 9); fresh = scratch + 4 * NE; prevR = scratch + 5 * NE; } return true; }
static void advect() {
  if (!scratchReady()) return; float *nd = scratch, *nc = scratch + NE, *nv = scratch + 2 * NE; double acc = 0;
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) {
    int i = y * EW + x; float vx, vy; displacement(i, vx, vy); acc += vx * vx + vy * vy;
    float sx = x - vx, sy = y - vy; bool inside = sx >= 0 && sy >= 0 && sx <= EW - 1 && sy <= EH - 1;
    float cx = sx < 0 ? 0 : sx > EW - 1 ? EW - 1 : sx, cy = sy < 0 ? 0 : sy > EH - 1 ? EH - 1 : sy;
    int x0 = (int)cx, y0 = (int)cy, x1 = x0 + 1 < EW ? x0 + 1 : x0, y1 = y0 + 1 < EH ? y0 + 1 : y0; float tx = cx - x0, ty = cy - y0;
    float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty; int i00 = y0 * EW + x0, i10 = y0 * EW + x1, i01 = y1 * EW + x0, i11 = y1 * EW + x1;
    nd[i] = w00 * invD[i00] + w10 * invD[i10] + w01 * invD[i01] + w11 * invD[i11];
    nv[i] = w00 * rhoVar[i00] + w10 * rhoVar[i10] + w01 * rhoVar[i01] + w11 * rhoVar[i11];
    nc[i] = inside ? w00 * cert[i00] + w10 * cert[i10] + w01 * cert[i01] + w11 * cert[i11] : 0;     // new content: nothing is known about its depth
    // occlusion: content uncovered behind a nearer thing (the wall behind a passing post) arrives with the near thing's
    // depth.  Where the carried depth's own prediction contradicts the flow actually measured here, the depth is not
    // this content's: forget it (the update learns the true one within a few frames)
    if (nc[i] > 0 && contrast[i] > 0.3f) {
      float px, py; predict(i, px, py); float dx = (x - CX0) / EW, dy = (y - CY0) / EW, wx = nd[i] * px - roll * dy, wy = nd[i] * py + roll * dx;
      float ex = flowX[i] - wx, ey = flowY[i] - wy, tol = ADVECT_TOL * sqrtf(wx * wx + wy * wy) + ADVECT_SLACK;
      if (ex * ex + ey * ey > tol * tol) nc[i] = 0;
    }
  }
  memcpy(invD, nd, NE * sizeof(float)); memcpy(cert, nc, NE * sizeof(float)); memcpy(rhoVar, nv, NE * sizeof(float)); advected = sqrtf((float)(acc / NE));   // (nd, nc, nv are scratch pointers: sizeof would be 8)
  // fresh content: the retina here vs the previous retina where the measured flow says this content was (bilinear)
  float* nf = scratch + 3 * NE;
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) {
    int i = y * EW + x; float sx = x - flowX[i] - (rotFX ? rotFX[i] : 0), sy = y - flowY[i] - (rotFY ? rotFY[i] : 0); float f;
    if (sx < 0 || sy < 0 || sx > EW - 1 || sy > EH - 1) f = 1;
    else { int x0 = (int)sx, y0 = (int)sy, x1 = x0 + 1 < EW ? x0 + 1 : x0, y1 = y0 + 1 < EH ? y0 + 1 : y0; float tx = sx - x0, ty = sy - y0;
      float was = (1 - tx) * (1 - ty) * prevR[y0 * EW + x0] + tx * (1 - ty) * prevR[y0 * EW + x1] + (1 - tx) * ty * prevR[y1 * EW + x0] + tx * ty * prevR[y1 * EW + x1];
      f = fabsf(fine.r[i] - was) > FRESH_GREY ? 1.0f : 0.0f; }
    // it stays fresh a few frames, travelling with the content
    float cx = x - (fine.vx[i] + (rotFX ? rotFX[i] : 0)), cy = y - (fine.vy[i] + (rotFY ? rotFY[i] : 0)); float carried = 0;
    if (cx >= 0 && cy >= 0 && cx <= EW - 1 && cy <= EH - 1) { int x0 = (int)cx, y0 = (int)cy, x1 = x0 + 1 < EW ? x0 + 1 : x0, y1 = y0 + 1 < EH ? y0 + 1 : y0; float tx = cx - x0, ty = cy - y0; carried = (1 - tx) * (1 - ty) * fresh[y0 * EW + x0] + tx * (1 - ty) * fresh[y0 * EW + x1] + (1 - tx) * ty * fresh[y1 * EW + x0] + tx * ty * fresh[y1 * EW + x1]; }
    nf[i] = f > 0.6f * carried ? f : 0.6f * carried;
  }
  memcpy(fresh, nf, sizeof(float) * NE);
}

// sum of src over the (2r+1)^2 window around every cell (windows clipped at the border)
static void boxsum(const float* src, float* tmp, float* dst, int w, int h, int r) {
  for (int y = 0; y < h; y++) { const float* s = src + y * w; float* t = tmp + y * w; float acc = 0; for (int x = 0; x <= r && x < w; x++) acc += s[x]; for (int x = 0; x < w; x++) { t[x] = acc; if (x + r + 1 < w) acc += s[x + r + 1]; if (x - r >= 0) acc -= s[x - r]; } }
  for (int x = 0; x < w; x++) { float acc = 0; for (int y = 0; y <= r && y < h; y++) acc += tmp[y * w + x]; for (int y = 0; y < h; y++) { dst[y * w + x] = acc; if (y + r + 1 < h) acc += tmp[(y + r + 1) * w + x]; if (y - r >= 0) acc -= tmp[(y - r) * w + x]; } }
}

// one eye: optics → lamina → ON/OFF → correlators against the previous frame → pooled, normalised flow
static void seeLevel(Level& L) {
  const int w = L.w, h = L.h, n = L.n;
  // the ommatidium's acceptance angle: a [1 2 1]/4 blur in x then y (the optics low-pass the image to about one column)
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int i = y * w + x; float l = L.r[x > 0 ? i - 1 : i], rr = L.r[x + 1 < w ? i + 1 : i]; L.tmp[i] = 0.25f * (l + 2 * L.r[i] + rr); }
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int i = y * w + x; float u = L.tmp[y > 0 ? i - w : i], d = L.tmp[y + 1 < h ? i + w : i]; L.sum[i] = 0.25f * (u + 2 * L.tmp[i] + d); }
  float mean = 0; for (int i = 0; i < n; i++) mean += L.sum[i]; mean /= n; float gain = 1.0f / (mean + 8.0f);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {                     // lamina: centre minus 5x5 surround, Weber-normalised
    float s = 0; int c = 0; for (int j = -2; j <= 2; j++) { int yy = y + j; if (yy < 0 || yy >= h) continue; for (int k = -2; k <= 2; k++) { int xx = x + k; if (xx < 0 || xx >= w) continue; s += L.sum[yy * w + xx]; c++; } }
    float v = (L.sum[y * w + x] - s / c) * gain; L.lam[y * w + x] = v; L.on[y * w + x] = v > 0 ? v : 0; L.off[y * w + x] = v < 0 ? -v : 0;
  }
  if (derotate && rotRem(L)) {                                                 // a turning body: the delay line follows the rotation
    // by whole columns (no interpolation: a smoothed delay line would itself look like motion to the correlators); the
    // fraction left over is what the correlators will see of the rotation - taken off their flow below
    const float s = (float)EW / w; float* rx = rotRemX[levelIndex(L)]; float* ry = rotRemY[levelIndex(L)];
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      float u, v; rotAt((x + 0.5f) * s - 0.5f - CX0, (y + 0.5f) * s - 0.5f - CY0, u, v); u /= s; v /= s; const int i = y * w + x;
      const int dxi = (int)lroundf(u), dyi = (int)lroundf(v), sx = x - dxi, sy = y - dyi; rx[i] = u - dxi; ry[i] = v - dyi;
      if (sx < 0 || sy < 0 || sx >= w || sy >= h) { L.tmp[i] = 0; L.sum[i] = 0; rx[i] = 1e9f; continue; }   // content that was outside the view: no past (flagged)
      L.tmp[i] = L.dOn[sy * w + sx]; L.sum[i] = L.dOff[sy * w + sx];
    }
    memcpy(L.dOn, L.tmp, sizeof(float) * n); memcpy(L.dOff, L.sum, sizeof(float) * n);
  }
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {                     // T4 / T5 correlators toward the right and the lower neighbour
    int i = y * w + x; float numx = 0, denx = 0, numy = 0, deny = 0, ax = 0, bx = 0, ay = 0, by = 0;
    // T4 (ON) and T5 (OFF) populations weight the two channels; the four lobula-plate layers (a right, b left, c up, d down) weight the directions
    if (x + 1 < w) { int j = i + 1; numx = OL_ON_GAIN * (L.dOn[i] * L.on[j] - L.on[i] * L.dOn[j]) + OL_OFF_GAIN * (L.dOff[i] * L.off[j] - L.off[i] * L.dOff[j]); numx *= numx > 0 ? OL_DIR_GAIN[0] : OL_DIR_GAIN[1]; float a = L.on[j] - L.on[i], b = L.off[j] - L.off[i]; denx = a * a + b * b; ax = a; bx = b; }
    if (y + 1 < h) { int j = i + w; numy = OL_ON_GAIN * (L.dOn[i] * L.on[j] - L.on[i] * L.dOn[j]) + OL_OFF_GAIN * (L.dOff[i] * L.off[j] - L.off[i] * L.dOff[j]); numy *= numy > 0 ? OL_DIR_GAIN[3] : OL_DIR_GAIN[2]; float a = L.on[j] - L.on[i], b = L.off[j] - L.off[i]; deny = a * a + b * b; ay = a; by = b; }
    L.nx[i] = numx; L.dx[i] = denx; L.ny[i] = numy; L.dy[i] = deny; L.cxy[i] = ax * ay + bx * by;
  }
  memcpy(L.dOn, L.on, sizeof(float) * n); memcpy(L.dOff, L.off, sizeof(float) * n);   // the delay line: one frame
  // lobula plate: pool and divide by the patch's contrast; the floor adapts to the scene's typical contrast (gain control)
  boxsum(L.dx, L.tmp, L.cf, w, h, L.win); boxsum(L.dy, L.tmp, L.sum, w, h, L.win);
  float tot = 0; for (int i = 0; i < n; i++) tot += L.cf[i] + L.sum[i]; tot /= n; L.cfRef = L.cfRef > 0 ? L.cfRef + 0.1f * (tot - L.cfRef) : tot;
  // the sensor's own noise (about NOISE_GREY grey levels per pooled pixel, Weber-scaled) also makes contrast; a patch must beat it, or its flow is noise
  float patch = (float)((2 * L.win + 1) * (2 * L.win + 1)); float noiseE = NOISE_GREY * gain; noiseE = noiseE * noiseE * patch;   // per direction
  float eps = DEN_FLOOR * 0.5f * L.cfRef + 2 * noiseE + 1e-7f; L.eps = eps;
  memcpy(L.jxx, L.cf, sizeof(float) * n); memcpy(L.jyy, L.sum, sizeof(float) * n); boxsum(L.cxy, L.tmp, L.jxy, w, h, L.win);   // the tensor the correlators normalise with
  boxsum(L.nx, L.tmp, L.vx, w, h, L.win); for (int i = 0; i < n; i++) L.vx[i] = FLOW_GAIN * L.vx[i] / (L.cf[i] + eps);
  boxsum(L.ny, L.tmp, L.vy, w, h, L.win); for (int i = 0; i < n; i++) { L.vy[i] = FLOW_GAIN * L.vy[i] / (L.sum[i] + eps); float c = L.cf[i] + L.sum[i] - noiseE; L.cf[i] = (c < 0 ? 0 : c) / (L.cfRef + 1e-7f); }   // cf: contrast above the noise, relative to the scene
  if (derotate && rotRemX[levelIndex(L)]) {
    const float* rx = rotRemX[levelIndex(L)]; const float* ry = rotRemY[levelIndex(L)];
    for (int i = 0; i < n; i++) if (rx[i] < 1e8f) { L.vx[i] -= rx[i]; L.vy[i] -= ry[i]; }
    // content that came in from outside the view this frame has no past: its correlators read nonsense - and so does every
    // patch that pooled them.  No contrast there, so no weight in the fit, the depth map or time-to-contact
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { if (rx[y * w + x] < 1e8f) continue;
      for (int j = -L.win; j <= L.win; j++) for (int k = -L.win; k <= L.win; k++) { int yy = y + j, xx = x + k; if (yy < 0 || yy >= h || xx < 0 || xx >= w) continue; int c = yy * w + xx; L.cf[c] = 0; L.vx[c] = 0; L.vy[c] = 0; } }
  }
}

static inline float ramp(float m) { float w = (m - SAT_LO) / (SAT_HI - SAT_LO); return w < 0 ? 0 : w > 1 ? 1 : w; }
// the aperture problem, in the correlators' own terms: an x-correlator pair reports (Jxx vx + Jxy vy) / (Jxx + eps) for a
// true motion (vx, vy) — the full x-speed, plus the y-speed leaking through the texture's cross term (an oblique
// texture sliding sideways seems to move vertically too) — and the y pair the same the other way.  So this is what the
// level would report for a given true displacement; merged across the eyes with the flow's own weights (see merged()).
// (mx, my): what the level reports; (kx, ky): how much of that is the other axis leaking through the cross term — the
// correlators get this part only roughly right, so the figure layer tolerates it rather than trusting it
// (relx, rely): how much of each axis's contrast is above the floor — the reliability of that axis's flow (a vertical
// edge has no vertical structure: its y-flow is noise over the floor)
static inline void seenAt(const Level& L, int c, float vx, float vy, float* o) {
  float ix = 1.0f / (L.jxx[c] + L.eps), iy = 1.0f / (L.jyy[c] + L.eps);
  o[0] = (L.jxx[c] * vx + L.jxy[c] * vy) * ix; o[1] = (L.jxy[c] * vx + L.jyy[c] * vy) * iy; o[2] = fabsf(L.jxy[c] * vy * ix); o[3] = fabsf(L.jxy[c] * vx * iy); o[4] = L.jxx[c] * ix; o[5] = L.jyy[c] * iy;
}
static void seen(int i, float vx, float vy, float& mx, float& my, float& kx, float& ky, float& relx, float& rely) {
  int x = i % EW, y = i / EW, c = (y / 2) * CW + x / 2, e = (y / 4) * XW + x / 4;
  float m[3][6]; seenAt(fine, i, vx, vy, m[0]); seenAt(coarse, c, vx, vy, m[1]); seenAt(coarsest, e, vx, vy, m[2]);
  float w2 = ramp(sqrtf(coarse.vx[c] * coarse.vx[c] + coarse.vy[c] * coarse.vy[c])); if (y / 4 >= XH - 1) w2 = 0;
  float w1 = ramp(sqrtf(fine.vx[i] * fine.vx[i] + fine.vy[i] * fine.vy[i])); float o[6];
  for (int k = 0; k < 6; k++) { float mm = w2 * m[2][k] + (1 - w2) * m[1][k]; o[k] = w1 * mm + (1 - w1) * m[0][k]; }
  mx = o[0]; my = o[1]; kx = o[2]; ky = o[3]; relx = o[4]; rely = o[5];
}
// the flow at fine column (x, y), in fine columns per frame, merged across the three eyes
static void merged(int x, int y, float& ox, float& oy) {
  int i = y * EW + x, c = (y / 2) * CW + x / 2, e = (y / 4) * XW + x / 4;
  float cx = 2 * coarse.vx[c], cy = 2 * coarse.vy[c], ex = 4 * coarsest.vx[e], ey = 4 * coarsest.vy[e];
  float w2 = ramp(sqrtf(coarse.vx[c] * coarse.vx[c] + coarse.vy[c] * coarse.vy[c]));      // the coarse eye saturating → the coarsest
  if (y / 4 >= XH - 1) w2 = 0;                                                              // the coarsest eye's bottom row is half filled: not trusted
  float mx = w2 * ex + (1 - w2) * cx, my = w2 * ey + (1 - w2) * cy;
  float w1 = ramp(sqrtf(fine.vx[i] * fine.vx[i] + fine.vy[i] * fine.vy[i]));              // the fine eye saturating → the coarse
  ox = w1 * mx + (1 - w1) * fine.vx[i]; oy = w1 * my + (1 - w1) * fine.vy[i];
}

// a new grey frame: photoreceptors (see: 160x120 pooled 4x4; seeSmall: 80x60 pooled 2x2), the three eyes, the tangential cells, then the depth map
static void process();
static void see(const uint8_t* frame) {
  if (!ready) return;
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { uint32_t s = 0; const uint8_t* p = frame + (y * 4) * W + x * 4; for (int j = 0; j < 4; j++) { s += p[0] + p[1] + p[2] + p[3]; p += W; } fine.r[y * EW + x] = s / 16.0f; }
  process();
}
static void seeSmall(const uint8_t* frame) {
  if (!ready) return;
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { const uint8_t* p = frame + (y * 2) * SW + x * 2; fine.r[y * EW + x] = (p[0] + p[1] + p[SW] + p[SW + 1]) / 4.0f; }
  process();
}
static void process() {
  for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) { const float* p = fine.r + (y * 2) * EW + x * 2; coarse.r[y * CW + x] = 0.25f * (p[0] + p[1] + p[EW] + p[EW + 1]); }
  for (int y = 0; y < XH; y++) for (int x = 0; x < XW; x++) { int y0 = y * 2, y1 = y0 + 1 < CH ? y0 + 1 : y0; const float* p0 = coarse.r + y0 * CW + x * 2; const float* p1 = coarse.r + y1 * CW + x * 2; coarsest.r[y * XW + x] = 0.25f * (p0[0] + p0[1] + p1[0] + p1[1]); }
  float b = 0; for (int i = 0; i < NE; i++) b += fine.r[i]; brightness = b / NE;
  if (derotate && !rotFX) { rotFX = (float*)FLY_MALLOC(sizeof(float) * NE); rotFY = (float*)FLY_MALLOC(sizeof(float) * NE); if (!rotFX || !rotFY) { rotFX = rotFY = nullptr; derotate = false; } }
  if (derotate) for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) rotAt(x - CX0, y - CY0, rotFX[y * EW + x], rotFY[y * EW + x]);
  else if (rotFX) { memset(rotFX, 0, sizeof(float) * NE); memset(rotFY, 0, sizeof(float) * NE); }
  seeLevel(fine); seeLevel(coarse); seeLevel(coarsest);
  frames++; if (frames < 2) return;                                            // the delay lines need one frame of history
  float *fx = flowX, *fy = flowY, *cf = contrast;
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { int i = y * EW + x; merged(x, y, fx[i], fy[i]); cf[i] = fine.cf[i] + 0.25f * coarse.cf[(y / 2) * CW + x / 2]; }
  // tangential cells: weighted least squares of the flow field to (a + D*dx - R*dy, b + D*dy + R*dx)
  const float cx0 = (EW - 1) * 0.5f, cy0 = (EH - 1) * 0.5f;
  float* wf = fitW; for (int i = 0; i < NE; i++) wf[i] = cf[i] > FIT_CAP ? FIT_CAP : cf[i];
  float sw = 0, sx = 0, sy = 0; for (int i = 0; i < NE; i++) { sw += wf[i]; sx += wf[i] * fx[i]; sy += wf[i] * fy[i]; }
  float a = sw > 1e-6f ? sx / sw : 0, bb = sw > 1e-6f ? sy / sw : 0; float sd = 0, sr = 0, srr = 0, mag = 0;
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { int i = y * EW + x; float dx = (x - cx0) / EW, dy = (y - cy0) / EW, rx = fx[i] - a, ry = fy[i] - bb; sd += wf[i] * (rx * dx + ry * dy); sr += wf[i] * (ry * dx - rx * dy); srr += wf[i] * (dx * dx + dy * dy); mag += wf[i] * sqrtf(fx[i] * fx[i] + fy[i] * fy[i]); }
  float D = srr > 1e-6f ? sd / srr : 0, R = srr > 1e-6f ? sr / srr : 0;
  // a travelling camera: a thing moving on its own must not steer the fit (it is what the fit is for).  Two more passes
  // with the columns that disagree with the current fit nearly silenced (an M-estimator), as the support test judges them
  if (moving) for (int pass = 0; pass < 2; pass++) {
    float sw2 = 0, sx2 = 0, sy2 = 0;
    for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { int i = y * EW + x; float dx = (x - cx0) / EW, dy = (y - cy0) / EW, px = a + D * dx - R * dy, py = bb + D * dy + R * dx, ex = fx[i] - px, ey = fy[i] - py; float tol = 0.6f * sqrtf(px * px + py * py) + 0.06f; float w = wf[i] * (ex * ex + ey * ey < tol * tol ? 1.0f : 0.05f); sw2 += w; sx2 += w * fx[i]; sy2 += w * fy[i]; }
    float a2 = sw2 > 1e-6f ? sx2 / sw2 : 0, b2 = sw2 > 1e-6f ? sy2 / sw2 : 0; float sd2 = 0, sr2 = 0, srr2 = 0;
    for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { int i = y * EW + x; float dx = (x - cx0) / EW, dy = (y - cy0) / EW, px = a + D * dx - R * dy, py = bb + D * dy + R * dx, ex = fx[i] - px, ey = fy[i] - py; float tol = 0.6f * sqrtf(px * px + py * py) + 0.06f; float w = wf[i] * (ex * ex + ey * ey < tol * tol ? 1.0f : 0.05f); float rx = fx[i] - a2, ry = fy[i] - b2; sd2 += w * (rx * dx + ry * dy); sr2 += w * (ry * dx - rx * dy); srr2 += w * (dx * dx + dy * dy); }
    a = a2; bb = b2; D = srr2 > 1e-6f ? sd2 / srr2 : 0; R = srr2 > 1e-6f ? sr2 / srr2 : 0;
  }
  hs = a; vs = bb; expansion = D; roll = R; flowMag = sw > 1e-6f ? mag / sw : 0;
  if (moving) advect();                                                          // a travelling camera: the map keeps up with the scene
  // support: the share of contrast-bearing columns whose flow agrees with the fitted self-motion — a wide-field cell only
  // answers to motion of the whole field, a single object moving does not count as the fly turning
  { int agree = 0, tot = 0; for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { int i = y * EW + x; if (cf[i] < 0.08f) continue; float dx = (x - cx0) / EW, dy = (y - cy0) / EW, px = a + D * dx - R * dy, py = bb + D * dy + R * dx, ex = fx[i] - px, ey = fy[i] - py; float tol = 0.6f * sqrtf(px * px + py * py) + 0.06f; tot++; if (ex * ex + ey * ey < tol * tol) agree++; } support = tot ? (float)agree / tot : 0; }
  // depth: measured flow (roll removed) against the translation prediction for the average depth
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) {
    int i = y * EW + x; float dx = (x - cx0) / EW, dy = (y - cy0) / EW;
    float px = a + D * dx, py = bb + D * dy, mx = fx[i] + R * dy, my = fy[i] - R * dx; float pp = px * px + py * py;
    float rho = (mx * px + my * py) / (pp + 1e-4f); if (rho < 0) rho = 0; if (rho > 4) rho = 4;
    float info = pp * cf[i]; float g = info / (info + INFO_HALF);                 // no self-motion or no contrast → no evidence
    float err = rho - invD[i]; rhoVar[i] += DEPTH_RATE * g * (err * err - rhoVar[i]);   // how much this column's ratio jitters about its settled value
    invD[i] += DEPTH_RATE * g * err; invD[i] += DEPTH_DECAY * (1 - invD[i]);
    cert[i] += DEPTH_RATE * g * (1 - cert[i]); cert[i] -= DEPTH_DECAY * cert[i];
  }
  if (moving && scratchReady()) memcpy(prevR, fine.r, sizeof(float) * NE);
}

// the map as bytes: depth 128 = scene average, +64 per doubling of nearness; certainty 0..255; flow in 1/64 column per frame
static inline uint8_t depthByte(int i) { float d = invD[i]; if (d < 0.25f) d = 0.25f; if (d > 4) d = 4; int v = (int)(128 + 64 * log2f(d) + 0.5f); return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline uint8_t certByte(int i) { int v = (int)(cert[i] * 255 + 0.5f); return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline int8_t flowByte(float v) { int x = (int)(v * 64 + (v > 0 ? 0.5f : -0.5f)); return (int8_t)(x < -127 ? -127 : x > 127 ? 127 : x); }

// the depth scene as a smell: the 40x30 map pooled into 8 x 7 glomeruli (near = strong), for a mushroom body to sniff
static void sceneGlom(uint8_t* glom) {
  uint32_t acc[56] = { 0 }; uint16_t cnt[56] = { 0 };
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { int g = (y * 7 / EH) * 8 + x * 8 / EW; acc[g] += depthByte(y * EW + x); cnt[g]++; }
  for (int g = 0; g < 56; g++) glom[g] = (uint8_t)(cnt[g] ? acc[g] / cnt[g] : 0);
}

// the streamed packet:  A5 5A type version | u16 len | payload | u8 checksum (sum of payload bytes)
// eye payload: u32 seq, u32 ms, u8 w, u8 h, u8 flags, u8 pad, f32 fps motion novelty sceneNovelty hs vs expansion roll brightness,
//              then w*h bytes each: brightness, depth, certainty, vx (int8), vy (int8)
static const int PACKET_BYTES = 4 + 2 + 8 + 4 + 36 + 5 * NE + 1;
static uint8_t* packetHeader(uint8_t* p, uint8_t type, uint8_t version) { *p++ = 0xA5; *p++ = 0x5A; *p++ = type; *p++ = version; *p++ = 0; *p++ = 0; return p; }
static uint8_t* eyePayload(uint8_t* p, uint32_t ms, float fps, float motion, float novelty, float sceneNovelty) {
  memcpy(p, &frames, 4); p += 4; memcpy(p, &ms, 4); p += 4; *p++ = EW; *p++ = EH; *p++ = 0; *p++ = 0;
  float f[9] = { fps, motion, novelty, sceneNovelty, hs, vs, expansion, roll, brightness }; memcpy(p, f, 36); p += 36;
  for (int i = 0; i < NE; i++) { float r = fine.r[i]; *p++ = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r + 0.5f); }
  for (int i = 0; i < NE; i++) *p++ = depthByte(i);
  for (int i = 0; i < NE; i++) *p++ = certByte(i);
  for (int i = 0; i < NE; i++) *p++ = (uint8_t)flowByte(flowX[i]);
  for (int i = 0; i < NE; i++) *p++ = (uint8_t)flowByte(flowY[i]);
  return p;
}
static size_t packetFinish(uint8_t* out, uint8_t* p) { uint16_t len = (uint16_t)(p - out - 6); out[4] = len & 255; out[5] = len >> 8; uint8_t sum = 0; for (uint8_t* q = out + 6; q < p; q++) sum += *q; *p++ = sum; return p - out; }
static size_t packet(uint8_t* out, uint32_t ms, float fps, float motion, float novelty, float sceneNovelty) { uint8_t* p = packetHeader(out, 'E', 1); p = eyePayload(p, ms, fps, motion, novelty, sceneNovelty); return packetFinish(out, p); }

}  // namespace flyeye
