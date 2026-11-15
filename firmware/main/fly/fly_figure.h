// Part of decidio's fly eye (firmware/main/fly). From the fruitfly firmware, adapted for this runtime: see fly/README.md.
// fly_figure.h — figure-ground: what is moving in the scene that is not the scene itself.
//
// The lobula has cells that answer exactly this question.  Figure-detection (FD) cells and the small-object
// detectors LC11 / LC10 respond to a small patch moving against the background and are inhibited by wide-field
// motion — the flow the fly's own movement makes.  Here that is built on top of the optic lobe (fly_eye.h):
//
//   1. the tangential cells' self-motion estimate and the depth map predict the flow every column should show
//      if nothing in the world moved; the residual, |measured − predicted|, is the figure signal (FD cells),
//      inhibited as wide-field motion grows;
//   2. the residual is thresholded (adaptively: the field's own noise floor) and grouped into blobs — figures;
//   3. figures are tracked from frame to frame (LC10 keeps its gaze on an object): position, velocity, size,
//      growth (looming), where the figure entered the view and where it left;
//   4. a mushroom body sniffs each figure's residual pattern.  Motion that keeps happening in the same place — a
//      fan, a curtain, a screen — depresses its Kenyon-cell synapses and stops being novel; motion that passes
//      through does not accumulate.  The depression fades when the pattern stops (rest);
//   5. a slow background memory of the retina (frozen wherever a figure is) notices when the scene has changed
//      after the motion stopped: something was placed, or taken away.
// Light changes (the mean brightness jumping) and the fly's own movement gate everything: nothing is a figure
// while the whole world moves.
//
// A camera that travels (flyeye::moving — a rail, a train, a slider) cannot gate on its own motion: it moves all the
// time and must still see what moves in the scene.  For it the FD cells compare the measured flow with the flow the
// correlators WOULD report for the camera's motion at that column — the self-motion fit's translation, scaled by the
// depth there, seen through the column's texture orientation (the aperture problem: an oblique texture sliding
// sideways reports a vertical flow too, fly_eye.h orientation()).  Where the depth is not yet known (content that has
// just entered the view) the depth that best explains the measured flow is assumed, so only flow that NO depth could
// explain — motion across the camera's direction, or against it — counts there; as the map learns the depth (it
// travels with the scene, fly_eye.h advect()) the map's word replaces the assumption and speed differences count too.
// The tolerance grows with the predicted speed (correlator error does).  The world layer freezes its compass and
// panorama meanwhile, and the room-changed memory only accumulates while the camera is still.  No Arduino dependencies.
#pragma once
#include "fly_eye.h"
#include "fly_core.h"

namespace flyfig {

using flyeye::EW; using flyeye::EH; using flyeye::NE;
static const int MAX_TRACKS = 4, MAX_BLOBS = 8, MIN_BLOB = 6, BORDER = 3;   // columns: the smallest figure (before dilation), and the untrusted rim
static const float SELF_SOFT = 0.08f, SELF_HARD = 0.2f;       // wide-field motion (RMS columns/frame over the field): FD inhibition scale, and the gate
static const float LIGHT_JUMP = 6.0f;                          // mean brightness change per frame (grey levels) that counts as a light change
static const float BG_ALPHA = 0.003f;                          // background memory: ~15 s at 23 fps
static const float BG_DIFF = 40.0f;                            // grey levels that count as "the scene changed here" (a lamp's slow drift stays below it)
static const int ASSOC_RADIUS = 6, MAX_MISSING = 6, SNIFF_EVERY = 4;
static const float FLOOR = 0.035f;                             // residual (columns/frame) below which nothing is ever a figure

enum State : uint8_t { FREE = 0, TRACKING = 1, CONFIRMED = 2, GONE = 3 };
enum Edge : uint8_t { CENTRE = 0, LEFT = 1, RIGHT = 2, TOP = 3, BOTTOM = 4 };
static const char* const EDGE_NAME[5] = { "the middle of the view", "the left", "the right", "the top", "the bottom" };

struct Track {
  uint8_t state, bornEdge, lastEdge, x0, y0, x1, y1;   // bounding box in columns
  uint16_t id, age, missing, size, peakSize, birthSize;
  float x, y, vx, vy, novelty, growth, loom;            // centre, velocity (columns/frame), mushroom-body novelty, size growth per frame, outward flow (looming)
  float strength, firstNovelty;                         // the figure's residual at its strongest column, relative to the threshold (smoothed): how clearly it stands out; the first sniff's novelty
  uint32_t bornMs; bool fired; uint8_t flags;          // fired: a detector has already announced it; flags: free for detectors (bit per kind)
};
struct Blob { float cx, cy, vx, vy, div; int size, x0, y0, x1, y1; int cx0, cy0, cx1, cy1; };   // div: outward expansion of the residual flow about the centre (looming); c*: the box of the core columns alone

static float *resid = nullptr, *bg = nullptr, *changed = nullptr, *rvx = nullptr, *rvy = nullptr; static uint8_t* mask = nullptr; static uint8_t* core = nullptr; static int16_t* stack = nullptr; static int16_t* label = nullptr;
static Track tracks[MAX_TRACKS]; static Blob blobs[MAX_BLOBS]; static int nBlobs = 0; static uint16_t nextId = 1;
static float wide = 0, threshold = 0, lastBright = -1, residMean = 0; static int lightHold = 0, busyHold = 0; static bool selfMoving = false, lightChange = false, busy = false, gated = false;
static const float BUSY_SHARE = 0.3f;                          // share of the view above threshold beyond which it is flicker, not figures
static const float MOVE_TOL = 0.35f;                           // moving camera: the residual's tolerance, as a share of the predicted flow (correlator error grows with speed)
static const float MOVE_FLOOR = 0.12f;                         // moving camera: residual (columns/frame) below which nothing is ever a figure (a crossing object makes 0.3-0.5)
static const float MOVE_FRESH = 0.3f;                          // moving camera: content this fresh (flyeye::fresh) is not figure
static const int MOVE_MIN_SIDE = 3;                            // moving camera: a figure's core must span this many rows and columns (the streaks a passing edge leaves are one row high)
static const int MOVE_SNIFF_AGE = 9; static const float MOVE_SNIFF_STRENGTH = 1.5f;   // moving camera: a figure is only sniffed (and so only taught to the mushroom body) once it has lasted this long and stood out this clearly — the noise blobs must not make everything small familiar
static const float MOVE_XTOL = 1.0f;                           // moving camera: tolerance on the cross-talk part of the prediction (the texture's leak of one axis into the other), as a share of it
static const float MOVE_DMAX = 4.0f;                           // moving camera: the nearest depth the map allows (x the scene mean): flow along the camera's direction faster than this is no depth
static const float MOVE_SPEED_TOL = 0.5f, MOVE_TRUST = 0.2f;   // moving camera: tolerance on the speed the map's depth predicts, and the certainty at which the map's depth (not the prior) sets the bound
static const float MOVE_STILL = 0.08f;                         // moving camera: supported ego-flow (columns/frame x the field's support) below which the camera counts as still and any motion is figure
static const int RIM_IN = 8;                                   // moving camera: columns of the edge that content enters through where nothing is trusted (the coarsest eye's window reads appearance as motion)
static uint32_t frames = 0; static fly::Fly* nose = nullptr; static uint8_t glom[N_GLOM];

static bool begin(fly::Fly* habituation) {
  resid = (float*)FLY_MALLOC(sizeof(float) * NE); bg = (float*)FLY_MALLOC(sizeof(float) * NE); changed = (float*)FLY_MALLOC(sizeof(float) * NE); rvx = (float*)FLY_MALLOC(sizeof(float) * NE); rvy = (float*)FLY_MALLOC(sizeof(float) * NE);
  mask = (uint8_t*)FLY_MALLOC(NE); core = (uint8_t*)FLY_MALLOC(NE); stack = (int16_t*)FLY_MALLOC(sizeof(int16_t) * NE); label = (int16_t*)FLY_MALLOC(sizeof(int16_t) * NE);
  if (!resid || !bg || !changed || !rvx || !rvy || !mask || !core || !stack || !label) return false;
  memset(resid, 0, sizeof(float) * NE); memset(rvx, 0, sizeof(float) * NE); memset(rvy, 0, sizeof(float) * NE); memset(changed, 0, sizeof(float) * NE); memset(mask, 0, NE); memset(tracks, 0, sizeof tracks);
  nose = habituation; lastBright = -1; return true;
}
static void reset() { memset(resid, 0, sizeof(float) * NE); memset(changed, 0, sizeof(float) * NE); memset(tracks, 0, sizeof tracks); lastBright = -1; frames = 0; }

static int rimL = BORDER, rimR = BORDER, rimT = BORDER, rimB = BORDER;   // the untrusted rim, per side (a travelling camera widens the side its content enters through)
static Edge edgeOf(int x0, int y0, int x1, int y1) {   // a figure touching the untrusted rim came from that edge (a travelling camera's residual takes a few frames to build: two columns of grace)
  int m = flyeye::moving ? 2 : 0; if (x0 <= rimL + m) return LEFT; if (x1 >= EW - 1 - rimR - m) return RIGHT; if (y0 <= rimT + m) return TOP; if (y1 >= EH - 1 - rimB - m) return BOTTOM; return CENTRE;
}

// blobs: 4-connected components of the mask, largest first
static void findBlobs() {
  nBlobs = 0; for (int i = 0; i < NE; i++) label[i] = -1;
  for (int s = 0; s < NE; s++) {
    if (!mask[s] || label[s] >= 0) continue;
    int sp = 0; stack[sp++] = (int16_t)s; label[s] = 0; Blob b = { 0, 0, 0, 0, 0, 0, EW, EH, 0, 0, EW, EH, 0, 0 }; double sx = 0, sy = 0, svx = 0, svy = 0, srx = 0, sry = 0, scx = 0, scy = 0; int coreN = 0;
    while (sp) {
      int i = stack[--sp]; int x = i % EW, y = i / EW; b.size++; if (core[i]) { coreN++; srx += rvx[i]; sry += rvy[i]; scx += x; scy += y; if (x < b.cx0) b.cx0 = x; if (x > b.cx1) b.cx1 = x; if (y < b.cy0) b.cy0 = y; if (y > b.cy1) b.cy1 = y; } sx += x; sy += y; svx += flyeye::flowX[i]; svy += flyeye::flowY[i];
      if (x < b.x0) b.x0 = x; if (x > b.x1) b.x1 = x; if (y < b.y0) b.y0 = y; if (y > b.y1) b.y1 = y;
      int nb[4] = { x > 0 ? i - 1 : -1, x + 1 < EW ? i + 1 : -1, y > 0 ? i - EW : -1, y + 1 < EH ? i + EW : -1 };
      for (int k = 0; k < 4; k++) { int j = nb[k]; if (j >= 0 && mask[j] && label[j] < 0) { label[j] = 0; stack[sp++] = (int16_t)j; } }
    }
    if (coreN < MIN_BLOB) continue;
    if (flyeye::moving && (b.cx1 - b.cx0 + 1 < MOVE_MIN_SIDE || b.cy1 - b.cy0 + 1 < MOVE_MIN_SIDE)) continue;   // a streak, not a thing
    b.cx = (float)(sx / b.size); b.cy = (float)(sy / b.size); b.vx = (float)(svx / b.size); b.vy = (float)(svy / b.size);
    // looming (LPLC2): how much the flow inside the figure, relative to the figure's own motion, points away from its centre
    float mrx = (float)(srx / coreN), mry = (float)(sry / coreN), ccx = (float)(scx / coreN), ccy = (float)(scy / coreN); double num = 0, den = 0;
    for (int y = b.y0; y <= b.y1; y++) for (int x = b.x0; x <= b.x1; x++) { int i = y * EW + x; if (!core[i]) continue; float dx = x - ccx, dy = y - ccy; num += (rvx[i] - mrx) * dx + (rvy[i] - mry) * dy; den += dx * dx + dy * dy; }
    b.div = den > 0 ? (float)(num / den) : 0;
    int k = nBlobs < MAX_BLOBS ? nBlobs++ : MAX_BLOBS - 1;          // insert, keeping the array sorted by size
    if (k == MAX_BLOBS - 1 && nBlobs == MAX_BLOBS && b.size <= blobs[k].size) continue;
    while (k > 0 && blobs[k - 1].size < b.size) { blobs[k] = blobs[k - 1]; k--; }
    blobs[k] = b;
  }
}

// a travelling camera: the background memory and the residual maps slide with the scene (the depth map already has, in flyeye::see)
static void advectMaps() {
  if (!flyeye::scratchReady()) return; float *nb = flyeye::scratch + 6 * NE, *nr = flyeye::scratch + 7 * NE, *nx = flyeye::scratch + 8 * NE; static float *ny = nullptr, *nch = nullptr;
  if (!ny) { ny = (float*)FLY_MALLOC(sizeof(float) * NE * 2); if (!ny) return; nch = ny + NE; }
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) {
    int i = y * EW + x; float vx, vy; flyeye::displacement(i, vx, vy);
    float sx = x - vx, sy = y - vy; bool inside = sx >= 0 && sy >= 0 && sx <= EW - 1 && sy <= EH - 1;
    float cx = sx < 0 ? 0 : sx > EW - 1 ? EW - 1 : sx, cy = sy < 0 ? 0 : sy > EH - 1 ? EH - 1 : sy;
    int x0 = (int)cx, y0 = (int)cy, x1 = x0 + 1 < EW ? x0 + 1 : x0, y1 = y0 + 1 < EH ? y0 + 1 : y0; float tx = cx - x0, ty = cy - y0;
    float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty; int i00 = y0 * EW + x0, i10 = y0 * EW + x1, i01 = y1 * EW + x0, i11 = y1 * EW + x1;
    #define LERP4(m) (w00 * m[i00] + w10 * m[i10] + w01 * m[i01] + w11 * m[i11])
    nb[i] = inside ? LERP4(bg) : flyeye::fine.r[i]; nr[i] = inside ? LERP4(resid) : 0; nx[i] = inside ? LERP4(rvx) : 0; ny[i] = inside ? LERP4(rvy) : 0; nch[i] = inside ? LERP4(changed) : 0;
    #undef LERP4
  }
  memcpy(bg, nb, sizeof(float) * NE); memcpy(resid, nr, sizeof(float) * NE); memcpy(rvx, nx, sizeof(float) * NE); memcpy(rvy, ny, sizeof(float) * NE); memcpy(changed, nch, sizeof(float) * NE);
}

// the strongest residual inside a box
static float peakIn(int x0, int y0, int x1, int y1) { float pk = 0; for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (resid[y * EW + x] > pk) pk = resid[y * EW + x]; return pk; }

// the residual pattern inside a track's box, pooled into the 8 x 7 glomeruli — the figure's smell
static void figureGlom(const Track& t, uint8_t* g) {
  uint32_t acc[N_GLOM] = { 0 }; uint16_t cnt[N_GLOM] = { 0 };
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { int gi = (y * 7 / EH) * 8 + x * 8 / EW; cnt[gi]++; if (x >= t.x0 && x <= t.x1 && y >= t.y0 && y <= t.y1) { float v = resid[y * EW + x] / (2 * threshold + 1e-6f); acc[gi] += (uint32_t)(255 * (v > 1 ? 1 : v)); } }
  for (int i = 0; i < N_GLOM; i++) g[i] = (uint8_t)(cnt[i] ? acc[i] / cnt[i] : 0);
}

// one frame, after flyeye::see
static void update(uint32_t ms) {
  frames++;
  // gates: the fly's own motion (wide-field flow) and light changes
  // wide-field motion only counts as the fly's own when most of the field agrees with it (support); a lone object does not turn the fly
  float sup = (flyeye::support - 0.2f) / 0.4f; if (sup < 0) sup = 0; if (sup > 1) sup = 1;
  { double pp = 0; for (int i = 0; i < NE; i += 3) { float px, py; flyeye::predict(i, px, py); float dx = (i % EW - flyeye::CX0) / EW, dy = (i / EW - flyeye::CY0) / EW; px -= flyeye::roll * dy; py += flyeye::roll * dx; pp += px * px + py * py; } wide = sqrtf((float)(pp / (NE / 3))) * sup; }   // RMS of the self-motion flow over the field
  selfMoving = wide > SELF_HARD; const bool moving = flyeye::moving;
  float b = flyeye::brightness; lightChange = lastBright >= 0 && fabsf(b - lastBright) > LIGHT_JUMP;
  if (lastBright < 0) for (int i = 0; i < NE; i++) bg[i] = flyeye::fine.r[i];
  lastBright = b; if (lightChange) lightHold = 6; else if (lightHold) lightHold--;
  gated = (selfMoving && !moving) || lightHold > 0;                                  // a travelling camera is never gated by its own motion
  if (moving) advectMaps();                                                          // the background memory travels with the scene too
  // FD cells: the flow left over once self-motion through the depth map is subtracted, inhibited by wide-field motion
  float inhib = moving ? 1.0f : 1.0f / (1.0f + (wide / SELF_SOFT) * (wide / SELF_SOFT)); double sum = 0, sum2 = 0;
  for (int i = 0; i < NE; i++) {
    float mx, my, px, py; flyeye::measured(i, mx, my); flyeye::predict(i, px, py);
    float rx = mx - flyeye::invD[i] * px, ry = my - flyeye::invD[i] * py, c = flyeye::contrast[i];
    float g = (c / (c + 0.3f)) * inhib, e;
    if (!moving) e = sqrtf(rx * rx + ry * ry) * g;
    else {
      // the moving camera: only the direction cues.  Along the camera's own direction any speed can be some depth (the
      // speed cue is what parallax errors look like), so flow AGAINST that direction, and flow ACROSS it beyond what the
      // column's texture leaks (the correlators' cross-talk, fly_eye.h seen()), is figure; the rest is scene.
      float P = sqrtf(px * px + py * py);
      if (P * sup < MOVE_STILL) { e = sqrtf(mx * mx + my * my) * g; }                    // the camera is (nearly) still, or the fit is a lone object's and not the field's: any motion is figure
      else {
        float sx, sy, kx, ky, relx, rely; flyeye::seen(i, px, py, sx, sy, kx, ky, relx, rely);
        mx *= relx; my *= rely;                                                               // each axis's flow only as far as its own contrast supports it
        float ex = px / P, ey = py / P, along = mx * ex + my * ey, acx = mx - along * ex, acy = my - along * ey, across = sqrtf(acx * acx + acy * acy);
        float sAlong = sx * ex + sy * ey, spx = sx - sAlong * ex, spy = sy - sAlong * ey, leak = sqrtf(spx * spx + spy * spy);
        float dhat = along / P; if (dhat < 0) dhat = 0; if (dhat > 4) dhat = 4;             // the depth the along-flow implies: the leak scales with it
        float against = -along - MOVE_TOL * P; if (against < 0) against = 0;                  // moving the other way (past the noise of a slow scene)
        float ct = flyeye::cert[i], trust = ct / (ct + MOVE_TRUST);                              // the speed the map allows here: its depth (with tolerance) as far as it is known, else the nearest depth there is
        float allowed = P * (trust * flyeye::invD[i] * (1 + MOVE_SPEED_TOL) + (1 - trust) * (MOVE_DMAX + MOVE_TOL));
        float fast = along - allowed; if (fast < 0) fast = 0;                                     // moving the same way, but faster than any depth here could
        float acr = across - MOVE_XTOL * dhat * leak - MOVE_TOL * P; if (acr < 0) acr = 0;    // moving across, beyond the leak
        float al = fast - against; e = sqrtf(al * al + acr * acr) * g; float k = across > 1e-6f ? acr / across : 0; rx = al * ex + acx * k; ry = al * ey + acy * k;
      }
    }
    resid[i] = 0.6f * resid[i] + 0.4f * e; rvx[i] = 0.6f * rvx[i] + 0.4f * rx * g; rvy[i] = 0.6f * rvy[i] + 0.4f * ry * g; sum += resid[i]; sum2 += resid[i] * resid[i];
  }
  residMean = (float)(sum / NE); (void)sum2;
  // the noise floor is the field's 75th percentile (a histogram; an object covering a tenth of the view cannot inflate it), the threshold three times that
  { uint16_t hist[64] = { 0 }; for (int i = 0; i < NE; i++) { int b = (int)(resid[i] * 200); hist[b > 63 ? 63 : b]++; } int acc = 0, b = 0; for (; b < 64; b++) { acc += hist[b]; if (acc >= NE * 3 / 4) break; } threshold = 3.0f * (b + 1) / 200.0f + 0.01f; float fl = moving ? MOVE_FLOOR : FLOOR; if (threshold < fl) threshold = fl; }
  // flicker (a screen, a fluorescent tube, sensor noise in the dark) is residual motion everywhere at once: not a figure, and a gate
  { int over = 0; for (int i = 0; i < NE; i++) if (resid[i] > threshold) over++; float share = (float)over / NE; busy = share > BUSY_SHARE; if (busy) busyHold = 6; else if (busyHold) busyHold--; gated = gated || busyHold > 0; }
  // the rim: no flow estimate is trusted there; a travelling camera also distrusts the edge its content enters through, wider the faster it goes
  rimL = rimR = rimT = rimB = BORDER;
  if (moving) {
    float sp = fabsf(flyeye::hs) > fabsf(flyeye::vs) ? fabsf(flyeye::hs) : fabsf(flyeye::vs); float k = (sp - 0.12f) / 0.2f; if (k < 0) k = 0; if (k > 1) k = 1; int rim = BORDER + (int)(k * (RIM_IN - BORDER) + 0.5f);
    if (flyeye::hs < -0.05f) rimR = rim; else if (flyeye::hs > 0.05f) rimL = rim;
    if (flyeye::vs < -0.05f) rimB = rim; else if (flyeye::vs > 0.05f) rimT = rim;
    if (flyeye::expansion > 0.05f) { float ke = (flyeye::expansion - 0.05f) / 0.2f; if (ke > 1) ke = 1; int re = BORDER + (int)(ke * (RIM_IN - BORDER) + 0.5f); if (re > rimL) rimL = re; if (re > rimR) rimR = re; if (re > rimT) rimT = re; if (re > rimB) rimB = re; }   // forward travel: content enters at every edge
  }
  for (int i = 0; i < NE; i++) { int x = i % EW, y = i / EW; bool border = x < rimL || x >= EW - rimR || y < rimT || y >= EH - rimB; if (moving && (border || (flyeye::fresh && flyeye::fresh[i] > MOVE_FRESH))) { resid[i] = 0; rvx[i] = rvy[i] = 0; } core[i] = (!gated && !border && resid[i] > threshold) ? 1 : 0; }   // fresh content (uncovered, just entered) has no flow worth the name
  // a figure's columns are joined across one-column gaps (dilation), so one object is one blob
  for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) { uint8_t m = 0; for (int j = -1; j <= 1 && !m; j++) { int yy = y + j; if (yy < 0 || yy >= EH) continue; for (int k = -1; k <= 1; k++) { int xx = x + k; if (xx < 0 || xx >= EW) continue; if (core[yy * EW + xx]) { m = 1; break; } } } mask[y * EW + x] = m; }
  // background memory, frozen under figures; reset outright when the light changes.  A travelling camera's memory
  // only accumulates "changed" while the camera is still (a moving view is not a changed room)
  const bool still = !moving || wide < SELF_SOFT;
  if (lightChange) { for (int i = 0; i < NE; i++) { bg[i] = flyeye::fine.r[i]; changed[i] = 0; } }
  else for (int i = 0; i < NE; i++) { if (!mask[i]) bg[i] += (still ? BG_ALPHA : 0.05f) * (flyeye::fine.r[i] - bg[i]); float d = fabsf(flyeye::fine.r[i] - bg[i]); changed[i] += 0.05f * (((d > BG_DIFF && !mask[i] && still) ? 1.0f : 0.0f) - changed[i]); }
  findBlobs();
  // tracks: match blobs to the nearest live track, else start one; unmatched tracks go missing, then GONE for one frame
  for (int t = 0; t < MAX_TRACKS; t++) { Track& tr = tracks[t]; if (tr.state == GONE) tr.state = FREE; }
  bool used[MAX_BLOBS] = { false };
  for (int t = 0; t < MAX_TRACKS; t++) {
    Track& tr = tracks[t]; if (tr.state == FREE) continue;
    // every blob near the track's predicted position, or overlapping its box, is a piece of the same figure: take their union
    Blob u = { 0, 0, 0, 0, 0, 0, EW, EH, 0, 0, EW, EH, 0, 0 }; int n = 0; float px = tr.x + tr.vx, py = tr.y + tr.vy;
    for (int k = 0; k < nBlobs; k++) {
      if (used[k]) continue; const Blob& bl = blobs[k];
      float dx = bl.cx - px, dy = bl.cy - py, d = sqrtf(dx * dx + dy * dy), r = ASSOC_RADIUS + 0.5f * (tr.x1 - tr.x0 + bl.x1 - bl.x0);
      bool touches = bl.x0 <= tr.x1 + 3 && bl.x1 + 3 >= tr.x0 && bl.y0 <= tr.y1 + 3 && bl.y1 + 3 >= tr.y0;
      if (d >= r && !touches) continue;
      used[k] = true; n++; u.cx += bl.cx * bl.size; u.cy += bl.cy * bl.size; u.vx += bl.vx * bl.size; u.vy += bl.vy * bl.size; u.div += bl.div * bl.size; u.size += bl.size;
      if (bl.x0 < u.x0) u.x0 = bl.x0; if (bl.y0 < u.y0) u.y0 = bl.y0; if (bl.x1 > u.x1) u.x1 = bl.x1; if (bl.y1 > u.y1) u.y1 = bl.y1;
    }
    if (n) {
      u.cx /= u.size; u.cy /= u.size; u.vx /= u.size; u.vy /= u.size; u.div /= u.size; tr.missing = 0; tr.age++;
      tr.vx = 0.6f * tr.vx + 0.4f * (u.cx - tr.x); tr.vy = 0.6f * tr.vy + 0.4f * (u.cy - tr.y); tr.x = 0.5f * (tr.x + u.cx); tr.y = 0.5f * (tr.y + u.cy);
      tr.growth = 0.8f * tr.growth + 0.2f * ((float)u.size / (tr.size > 0 ? tr.size : u.size)); tr.loom = 0.7f * tr.loom + 0.3f * u.div; tr.size = (uint16_t)u.size; if (u.size > tr.peakSize) tr.peakSize = (uint16_t)u.size;
      tr.x0 = (uint8_t)u.x0; tr.y0 = (uint8_t)u.y0; tr.x1 = (uint8_t)u.x1; tr.y1 = (uint8_t)u.y1; tr.lastEdge = edgeOf(u.x0, u.y0, u.x1, u.y1);
      tr.strength = 0.7f * tr.strength + 0.3f * peakIn(u.x0, u.y0, u.x1, u.y1) / (threshold + 1e-6f);
    } else if (++tr.missing > MAX_MISSING) tr.state = GONE;
  }
  for (int k = 0; k < nBlobs; k++) {
    if (used[k] || gated) continue; const Blob& bl = blobs[k];
    bool overlaps = false;                                            // a piece of an object already tracked is not a new object
    for (int t = 0; t < MAX_TRACKS && !overlaps; t++) { const Track& o = tracks[t]; if (o.state != TRACKING && o.state != CONFIRMED) continue; overlaps = bl.x0 <= o.x1 + 3 && bl.x1 + 3 >= o.x0 && bl.y0 <= o.y1 + 3 && bl.y1 + 3 >= o.y0; }
    if (overlaps) continue;
    int slot = -1; for (int t = 0; t < MAX_TRACKS; t++) if (tracks[t].state == FREE) { slot = t; break; } if (slot < 0) break;
    Track& tr = tracks[slot]; memset(&tr, 0, sizeof tr);
    tr.state = TRACKING; tr.id = nextId++; tr.x = bl.cx; tr.y = bl.cy; tr.vx = bl.vx; tr.vy = bl.vy; tr.size = tr.peakSize = tr.birthSize = (uint16_t)bl.size; tr.growth = 1; tr.loom = bl.div; tr.age = 1; tr.bornMs = ms; tr.novelty = tr.firstNovelty = 1;
    tr.x0 = (uint8_t)bl.x0; tr.y0 = (uint8_t)bl.y0; tr.x1 = (uint8_t)bl.x1; tr.y1 = (uint8_t)bl.y1; tr.bornEdge = tr.lastEdge = edgeOf(bl.x0, bl.y0, bl.x1, bl.y1);
    tr.strength = peakIn(bl.x0, bl.y0, bl.x1, bl.y1) / (threshold + 1e-6f);
  }
  // habituation: every few frames each live figure is sniffed; seeing it makes it familiar, and rest lets that fade
  if (nose) {
    for (int t = 0; t < MAX_TRACKS; t++) {
      Track& tr = tracks[t]; if (tr.state != TRACKING && tr.state != CONFIRMED) continue; if (tr.missing) continue;
      const int start = moving ? MOVE_SNIFF_AGE : 1; if (tr.age < start || (tr.age - start) % SNIFF_EVERY != 0) continue; if (moving && tr.strength < MOVE_SNIFF_STRENGTH) continue;
      figureGlom(tr, glom); nose->smell(glom); float nov = nose->approach; bool first = tr.age == start; if (first) tr.firstNovelty = nov; tr.novelty = first ? nov : 0.85f * tr.novelty + 0.15f * nov; nose->learn(-1);   // the first sniff (before any learning) decides; later ones only trickle in
    }
    if (frames % 100 == 0) nose->rest();
  }
}

// columns where the scene now differs from the remembered background (motion over): count and centroid
static int changedRegion(float& cx, float& cy) { int n = 0; double sx = 0, sy = 0; for (int i = 0; i < NE; i++) if (changed[i] > 0.8f) { n++; sx += i % EW; sy += i / EW; } if (n) { cx = (float)(sx / n); cy = (float)(sy / n); } return n; }
static void absorb() { for (int i = 0; i < NE; i++) if (changed[i] > 0.5f) { bg[i] = flyeye::fine.r[i]; changed[i] = 0; } }   // accept the new scene as the background
static int liveTracks() { int n = 0; for (int t = 0; t < MAX_TRACKS; t++) if (tracks[t].state == TRACKING || tracks[t].state == CONFIRMED) n++; return n; }
static const char* dirName(float vx, float vy) { if (fabsf(vx) < 0.05f && fabsf(vy) < 0.05f) return "slowly"; if (fabsf(vx) >= fabsf(vy)) return vx > 0 ? "right" : "left"; return vy > 0 ? "down" : "up"; }

}  // namespace flyfig
