// Part of decidio's fly eye (firmware/main/fly). From the fruitfly firmware, adapted for this runtime: see fly/README.md.
// fly_lock.h — the lock: the fovea holds one chosen target.
//
// A fly that has picked a target keeps it in the acute zone of its eye (the LC10 cells of the lobula drive that
// pursuit); here the fovea (fly_fovea.h) does the same for one chosen figure.  Once locked the target's window goes
// out at full resolution at every look, first, whatever else moves.
//
// The figure tracks are made of motion residual — of a person, that is an arm, a head, whatever moved this frame —
// so the lock does not follow the track: it follows the APPEARANCE.  When it is taken it keeps the retina patch under
// its window as a template, and every frame it finds where that patch went (normalised correlation over a small
// search window around where it expects it), so it holds the whole person, still or moving, and only drifts to the
// track's box slowly for size.  When the template is lost (the target left, or turned into something else) the lock
// coasts on its last velocity for a while, then releases.  A lock is taken by itself on the first figure announced
// (or the largest live one, with LOCK), or by hand on a track (LOCK <id>); UNLOCK releases; LOCK OFF / LOCK AUTO
// stop and resume the automatic lock.  No Arduino dependencies (lock_test.cpp).
#pragma once
#include "fly_figure.h"

namespace flylock {

static const int COAST_FRAMES = 40;      // frames the lock survives without its template or track (about 2 s)
static const float FOLLOW = 0.5f;        // how fast the window follows the TRACK's box when the appearance is lost (per frame); a found appearance is followed outright
static const float SIZE_FOLLOW = 0.15f;  // how fast the window's size follows the track's box (slowly: the track is only the moving part)
static const int MIN_SIZE = 8;           // columns: figures smaller than this are not worth a lock
static const int TW = 16, TH = 12;       // the template: the window resampled to at most this many columns
static const int SEARCH = 2, SEARCH_MAX = 5;   // columns either side of the expected place that the template is looked for in: this at rest, more with speed and when the target was just lost (wider still, and the best of many chance matches on texture passes for the target)
static const int ADOPT_RANGE = 6;        // columns: a live track this near the window is the target moving (the person's track dies and is reborn under new ids)
static const int RELEARN_AFTER = 3;      // frames without the appearance before the template is learned afresh from the window (a glance away must not wipe it)
static const float MATCH_MIN = 0.62f;    // correlation below which the template is not found this frame (the best of 25 chance matches on random texture reaches about 0.5)
static const float TEMPLATE_RATE = 0.08f;// how fast the template follows the target's changing appearance (per frame the match is confident: a head turns)

static uint16_t id = 0; static float x0 = 0, y0 = 0, x1 = 0, y1 = 0, vx = 0, vy = 0; static int coast = 0, held = 0, lost = 0; static bool automatic = true, manual = false; static uint32_t locks = 0, adoptions = 0;
static float tmpl[TW * TH]; static int tw = 0, th = 0; static float match = 0;   // the template (tw x th of it used), the last match score

static bool active() { return id != 0; }
static void unlock() { id = 0; coast = 0; held = 0; lost = 0; manual = false; match = 0; tw = th = 0; }
// the retina, bilinear, at a fractional column
static inline float retinaAt(float x, float y) {
  if (x < 0) x = 0; if (y < 0) y = 0; if (x > flyeye::EW - 1) x = flyeye::EW - 1; if (y > flyeye::EH - 1) y = flyeye::EH - 1;
  int ix = (int)x, iy = (int)y, jx = ix + 1 < flyeye::EW ? ix + 1 : ix, jy = iy + 1 < flyeye::EH ? iy + 1 : iy; float tx = x - ix, ty = y - iy; const float* r = flyeye::fine.r;
  return (1 - tx) * (1 - ty) * r[iy * flyeye::EW + ix] + tx * (1 - ty) * r[iy * flyeye::EW + jx] + (1 - tx) * ty * r[jy * flyeye::EW + ix] + tx * ty * r[jy * flyeye::EW + jx];
}
// the window's patch of the retina, whole columns only (interpolation would blur it, and a blurred patch matches any smooth
// texture), at most tw x th of them (a wider window is strided), into out (mean removed)
static void sample(float bx0, float by0, float bx1, float by1, float* out) {
  int ix0 = (int)floorf(bx0 + 0.5f), iy0 = (int)floorf(by0 + 0.5f), w = (int)floorf(bx1 - bx0 + 0.5f) + 1, h = (int)floorf(by1 - by0 + 0.5f) + 1;
  int sx = (w + TW - 1) / TW, sy = (h + TH - 1) / TH; if (sx < 1) sx = 1; if (sy < 1) sy = 1; tw = (w + sx - 1) / sx; th = (h + sy - 1) / sy; if (tw < 2) tw = 2; if (th < 2) th = 2;
  float mean = 0;
  for (int j = 0; j < th; j++) for (int i = 0; i < tw; i++) { float v = retinaAt((float)(ix0 + i * sx), (float)(iy0 + j * sy)); out[j * tw + i] = v; mean += v; }
  mean /= tw * th; for (int k = 0; k < tw * th; k++) out[k] -= mean;
}
// normalised correlation between the template and the window moved by (dx, dy)
static float correlate(float dx, float dy) {
  static float cand[TW * TH]; int stw = tw, sth = th; sample(x0 + dx, y0 + dy, x1 + dx, y1 + dy, cand); tw = stw; th = sth;
  double st = 0, sc = 0, stc = 0; for (int k = 0; k < tw * th; k++) { st += tmpl[k] * tmpl[k]; sc += cand[k] * cand[k]; stc += tmpl[k] * cand[k]; }
  return (st > 1e-6 && sc > 1e-6) ? (float)(stc / sqrt(st * sc)) : 0;
}
static void lockTrack(const flyfig::Track& tr, bool byHand) {
  id = tr.id; x0 = tr.x0; y0 = tr.y0; x1 = tr.x1; y1 = tr.y1; vx = tr.vx; vy = tr.vy; coast = 0; held = 0; lost = 0; manual = byHand; locks++;
  sample(x0, y0, x1, y1, tmpl); match = 1;
}
// LOCK <id>: a live track; LOCK: the announced one, else the largest live one.  false: nothing to lock
static bool lock(uint16_t wanted = 0) {
  const flyfig::Track* best = nullptr;
  for (int t = 0; t < flyfig::MAX_TRACKS; t++) { const flyfig::Track& tr = flyfig::tracks[t]; if (tr.state != flyfig::TRACKING && tr.state != flyfig::CONFIRMED) continue; if (wanted) { if (tr.id == wanted) best = &tr; } else if (!best || (tr.fired && !best->fired) || (tr.fired == best->fired && tr.size > best->size)) best = &tr; }
  if (!best) return false; lockTrack(*best, wanted != 0); return true;
}
static void keepInside() {
  float w = x1 - x0, h = y1 - y0;
  if (x0 < 0) { x0 = 0; x1 = w; } if (y0 < 0) { y0 = 0; y1 = h; } if (x1 > flyeye::EW - 1) { x1 = flyeye::EW - 1; x0 = x1 - w; } if (y1 > flyeye::EH - 1) { y1 = flyeye::EH - 1; y0 = y1 - h; }
}
// every frame, after the figure layer: find the template, follow it; the track shapes the window; coast without either
static void update() {
  if (id) {
    const flyfig::Track* tr = nullptr;
    for (int t = 0; t < flyfig::MAX_TRACKS; t++) { const flyfig::Track& c = flyfig::tracks[t]; if (c.id == id && (c.state == flyfig::TRACKING || c.state == flyfig::CONFIRMED)) tr = &c; }
    if (!tr) {                                                                                // the target's track died (a person's does, and is reborn): any live track on the window is it
      float best = ADOPT_RANGE + 1, cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
      for (int t = 0; t < flyfig::MAX_TRACKS; t++) { const flyfig::Track& c = flyfig::tracks[t]; if (c.state != flyfig::TRACKING && c.state != flyfig::CONFIRMED) continue; bool over = c.x0 <= x1 && c.x1 >= x0 && c.y0 <= y1 && c.y1 >= y0; float d = over ? 0 : fabsf(c.x - cx) + fabsf(c.y - cy); if (d < best) { best = d; tr = &c; } }
      if (tr) { id = tr->id; adoptions++; }
    }
    // where did the appearance go?  the best correlation around the expected place (a step of velocity ahead); the search
    // widens with speed and when the target was just lost (a hand crosses several columns a frame and turns back)
    float sp = fabsf(vx) > fabsf(vy) ? fabsf(vx) : fabsf(vy); int reach = SEARCH + (int)(sp + 0.5f) + (lost ? 2 : 0); if (reach > SEARCH_MAX) reach = SEARCH_MAX;
    float ex = floorf(vx + 0.5f), ey = floorf(vy + 0.5f), bestS = -1, bdx = 0, bdy = 0;   // whole columns: the template is sampled on them
    for (int dy = -reach; dy <= reach; dy++) for (int dx = -reach; dx <= reach; dx++) { float s = correlate(ex + dx, ey + dy); if (s > bestS) { bestS = s; bdx = ex + dx; bdy = ey + dy; } }
    match = bestS;
    if (bestS >= MATCH_MIN) {
      x0 += bdx; x1 += bdx; y0 += bdy; y1 += bdy;                                              // the appearance is there: go there (the correlation is the smoothing)
      vx = 0.7f * vx + 0.3f * bdx; vy = 0.7f * vy + 0.3f * bdy; coast = 0; lost = 0; held++;
      if (bestS >= 0.8f) { static float now[TW * TH]; int stw = tw, sth = th; sample(x0, y0, x1, y1, now); if (tw == stw && th == sth) for (int k = 0; k < tw * th; k++) tmpl[k] += TEMPLATE_RATE * (now[k] - tmpl[k]); }   // the appearance changes (a head turns): follow it while the match is sure
      if (tr && !tr->missing) {                                                                // the track: its box sets the window's size, slowly; its centre nudges the place
        float cw = x1 - x0, ch = y1 - y0, tw_ = tr->x1 - tr->x0, th_ = tr->y1 - tr->y0; if (tw_ < MIN_SIZE) tw_ = cw; if (th_ < MIN_SIZE) th_ = ch;
        float cx = 0.5f * (x0 + x1) + 0.15f * (tr->x - 0.5f * (x0 + x1)), cy = 0.5f * (y0 + y1) + 0.15f * (tr->y - 0.5f * (y0 + y1)); cw += SIZE_FOLLOW * (tw_ - cw); ch += SIZE_FOLLOW * (th_ - ch);
        x0 = cx - cw / 2; x1 = cx + cw / 2; y0 = cy - ch / 2; y1 = cy + ch / 2;
      }
    } else if (tr && !tr->missing) {                                                          // the appearance is lost but a track lives on it: go where the motion is (the window keeps its size), and after a few frames learn the look afresh
      float cw = x1 - x0, ch = y1 - y0, cx = 0.5f * (x0 + x1) + FOLLOW * (tr->x - 0.5f * (x0 + x1)), cy = 0.5f * (y0 + y1) + FOLLOW * (tr->y - 0.5f * (y0 + y1));
      x0 = cx - cw / 2; x1 = cx + cw / 2; y0 = cy - ch / 2; y1 = cy + ch / 2; vx = tr->vx; vy = tr->vy; coast = 0; held++;
      if (++lost >= RELEARN_AFTER) { keepInside(); sample(x0, y0, x1, y1, tmpl); lost = 0; }
    } else {
      if (++coast > COAST_FRAMES) { unlock(); return; }
      x0 += vx; x1 += vx; y0 += vy; y1 += vy; vx *= 0.9f; vy *= 0.9f; held++; lost++;         // coast, slowing
    }
    keepInside();
  }
  if (!id && automatic) {                                                                   // by itself: the announced figure, else nothing (a lock waits for something worth it)
    for (int t = 0; t < flyfig::MAX_TRACKS; t++) { const flyfig::Track& tr = flyfig::tracks[t]; if ((tr.state == flyfig::TRACKING || tr.state == flyfig::CONFIRMED) && tr.fired && tr.size >= MIN_SIZE) { lockTrack(tr, false); break; } }
  }
}
// the window, in columns (inclusive), for the fovea
static void box(int& bx0, int& by0, int& bx1, int& by1) { bx0 = (int)(x0 + 0.5f); by0 = (int)(y0 + 0.5f); bx1 = (int)(x1 + 0.5f); by1 = (int)(y1 + 0.5f); if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 >= flyeye::EW) bx1 = flyeye::EW - 1; if (by1 >= flyeye::EH) by1 = flyeye::EH - 1; }
static void reset() { unlock(); }

}  // namespace flylock
