// fly_vision.h — the framework: the fly's visual system as layers, and detectors that read them.
// (decidio's version of the fruitfly firmware's file: no world compass or panorama, no streamed packet - eye.cc makes
// the page's packet. A fixed camera needs no compass; "azimuth" is the bearing from the centre of the view.)
//
//   frame ──▶ optic lobe (fly_eye.h)      retina, contrast, flow, self-motion, depth map, certainty
//         ──▶ figure-ground (fly_figure.h) residual object motion, blobs, tracks, habituation, background change
//         ──▶ lock (fly_lock.h)            one chosen figure followed by its appearance
//         ──▶ detectors                    each reads one Percept per frame and may raise one Event
//
// A detector is a few lines: subclass Detector, look at the Percept, fill an Event and return true. Built-in ones
// live in fly_detectors.h (new object entered, object left, object approaching, room changed). Everything here is
// plain C++ with no board dependencies, so tests/host/fly_test.cpp runs the whole stack on a PC against synthetic scenes.
#pragma once
#include "fly_eye.h"
#include "fly_figure.h"
#include "fly_lock.h"

namespace flyvision {

enum Kind : uint8_t { NONE = 0, NEW_OBJECT = 1, OBJECT_LEFT = 2, APPROACH = 3, ROOM_CHANGED = 4, SOUND = 5, ITEM = 6, CUSTOM = 16 };
static const char* const KIND_NAME[7] = { "none", "new object", "object left", "approach", "room changed", "sound", "item" };
static const int N_KINDS = 7;
static const float VIEW_DEG = 62.0f;                                 // the camera's horizontal field of view, roughly (OV2640 / OV3660 modules)

struct Event { uint8_t kind; uint16_t id; uint8_t x, y; float strength, azimuth; uint32_t ms; char text[64]; };

// everything a detector may look at, one frame's worth — all maps are flyeye::EW x flyeye::EH columns
struct Percept {
  uint32_t frame, ms;
  const float *bright, *contrast, *flowX, *flowY, *depth, *cert;   // optic lobe: photoreceptors, relative contrast, flow (col/frame), inverse depth (1 = average), certainty
  const float *figure, *bg, *changed;                               // figure-ground: residual object motion, remembered background, changed-scene evidence
  float hs, vs, expansion, roll, wide, brightness;                  // tangential cells
  bool selfMoving, lightChange, gated;                              // the gates
  flyfig::Track* tracks; int nTracks;                               // figures (detectors may annotate them: fired, state)
};

struct Detector {
  const char* name; explicit Detector(const char* n) : name(n) {} virtual ~Detector() {}
  virtual bool detect(const Percept& p, Event& e) = 0;
};

static const int MAX_DETECTORS = 8, RING = 8;
static int WARMUP_FRAMES = 80;                                       // ~4 s at the camera's rate before detectors may speak (the camera's exposure settles); the tests lower it
static Detector* detectors[MAX_DETECTORS]; static int nDetectors = 0;
static Event ring[RING]; static uint32_t nEvents = 0; static void (*onEvent)(const Event&) = nullptr;
static Percept percept; static bool ready = false;

static inline float azimuthOf(float x) { return (x - (flyeye::EW - 1) * 0.5f) / flyeye::EW * VIEW_DEG; }   // degrees right of the centre of the view

static bool begin(fly::Fly* habituation) {
  if (!flyeye::ready && !flyeye::begin()) return false;
  if (!flyfig::begin(habituation)) return false;
  memset(&percept, 0, sizeof percept); ready = true; return true;
}
static bool add(Detector* d) { if (nDetectors >= MAX_DETECTORS) return false; detectors[nDetectors++] = d; return true; }
// an event from outside the frame pipeline (the ear, a custom sensor): same ring, same callback
static void emit(const Event& ev) { Event e = ev; ring[nEvents % RING] = e; nEvents++; if (onEvent) onEvent(e); }
static void reset() { flyeye::reset(); flyfig::reset(); flylock::reset(); nEvents = 0; }
// a camera that travels (a rail, a train, a slider): figures are found against the camera's own motion instead of being gated by it
static void setMoving(bool m) { flyeye::moving = m; }

// after the eye has seen a frame (flyeye::see / seeSmall, or the retina filled directly and flyeye::process()):
// every other layer, then every detector
static void afterSee(uint32_t ms) {
  if (!ready || flyeye::frames < 2) return;
  flyfig::update(ms); flylock::update();
  Percept& p = percept; p.frame = flyeye::frames; p.ms = ms;
  p.bright = flyeye::fine.r; p.contrast = flyeye::contrast; p.flowX = flyeye::flowX; p.flowY = flyeye::flowY; p.depth = flyeye::invD; p.cert = flyeye::cert;
  p.figure = flyfig::resid; p.bg = flyfig::bg; p.changed = flyfig::changed;
  p.hs = flyeye::hs; p.vs = flyeye::vs; p.expansion = flyeye::expansion; p.roll = flyeye::roll; p.wide = flyfig::wide; p.brightness = flyeye::brightness;
  p.selfMoving = flyfig::selfMoving; p.lightChange = flyfig::lightChange; p.gated = flyfig::gated; p.tracks = flyfig::tracks; p.nTracks = flyfig::MAX_TRACKS;
  if ((int)flyeye::frames < WARMUP_FRAMES) return;                               // the camera's exposure and the eye's gain control settle first
  for (int d = 0; d < nDetectors; d++) {
    Event e; memset(&e, 0, sizeof e);
    if (!detectors[d]->detect(p, e)) continue;
    e.ms = ms; e.azimuth = azimuthOf(e.x); ring[nEvents % RING] = e; nEvents++;
    if (onEvent) onEvent(e);
  }
}
static void onFrame(const uint8_t* frame, uint32_t ms) { if (!ready) return; flyeye::see(frame); afterSee(ms); }
static void onSmallFrame(const uint8_t* frame, uint32_t ms) { if (!ready) return; flyeye::seeSmall(frame); afterSee(ms); }

}  // namespace flyvision
