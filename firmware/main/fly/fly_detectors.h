// Part of decidio's fly eye (firmware/main/fly). From the fruitfly firmware, adapted for this runtime: see fly/README.md.
// fly_detectors.h — the built-in detectors, and the pattern for writing your own.
//
// A detector reads one Percept per frame (see fly_vision.h) and may raise one Event.  Each of these is a few lines
// over the figure tracks, the gates and the background memory; add your own the same way and register it with
// flyvision::add().  The connectome has a lobula columnar population for each of these jobs: LC11 for a small
// object moving in (OL_N_LC11 cells), LPLC2 / LC4 for something looming toward the eye (OL_N_LPLC2, OL_N_LC4),
// LC10 for keeping an object in view (OL_N_LC10a …).
#pragma once
#include "fly_vision.h"
#include <stdio.h>

namespace flydet {

using flyvision::Percept; using flyvision::Event; using flyvision::Detector; using flyfig::Track;

// something new moved into the room: a figure that has persisted, that the mushroom body has not habituated to,
// while the board itself was still and the light did not change.  Fires once per figure.
struct NewObject : Detector {
  int minAge = 8; float minNovelty = 0.5f;
  int movingMinAge = 14; float movingMinStrength = 2.0f;   // a travelling camera: the figure must persist longer (a small-target detector facilitates along a path) and stand out clearly above the residual floor
  NewObject() : Detector("new object") {}
  bool detect(const Percept& p, Event& e) override {
    if (p.gated) return false;
    const bool moving = flyeye::moving; const int needAge = moving ? movingMinAge : minAge;
    for (int t = 0; t < p.nTracks; t++) {
      Track& tr = p.tracks[t]; float nov = moving ? tr.firstNovelty : tr.novelty;   // a travelling camera waits longer, so the first sniff (before habituation) is the one that counts
      if (tr.state != flyfig::TRACKING || tr.fired || tr.age < needAge || tr.missing || nov < minNovelty || (moving && tr.strength < movingMinStrength)) continue;
      tr.fired = true; tr.state = flyfig::CONFIRMED; e.kind = flyvision::NEW_OBJECT; e.id = tr.id; e.x = (uint8_t)tr.x; e.y = (uint8_t)tr.y; e.strength = tr.novelty;
      snprintf(e.text, sizeof e.text, "new object #%u entered from %s, moving %s (%u columns)", tr.id, flyfig::EDGE_NAME[tr.bornEdge], flyfig::dirName(tr.vx, tr.vy), tr.size);
      return true;
    }
    return false;
  }
};

// a confirmed object is no longer seen: it left by an edge, or it stopped moving inside the view
struct ObjectLeft : Detector {
  ObjectLeft() : Detector("object left") {}
  bool detect(const Percept& p, Event& e) override {
    for (int t = 0; t < p.nTracks; t++) {
      Track& tr = p.tracks[t]; if (tr.state != flyfig::GONE || !tr.fired) continue;
      tr.fired = false; e.kind = flyvision::OBJECT_LEFT; e.id = tr.id; e.x = (uint8_t)tr.x; e.y = (uint8_t)tr.y; e.strength = 1;
      if (tr.lastEdge != flyfig::CENTRE) snprintf(e.text, sizeof e.text, "object #%u left via %s", tr.id, flyfig::EDGE_NAME[tr.lastEdge]);
      else snprintf(e.text, sizeof e.text, "object #%u stopped moving in the view", tr.id);
      return true;
    }
    return false;
  }
};

// looming: a figure growing frame after frame is coming toward the camera (LPLC2 / LC4 — what makes a fly jump)
struct Approach : Detector {
  float minLoom = 0.012f, grown = 1.3f; int minSize = 24, minAge = 12; static const uint8_t FLAG = 1;   // loom: outward flow per column of radius (columns/frame), sustained, with the figure larger than at birth; once per figure
  Approach() : Detector("approach") {}
  bool detect(const Percept& p, Event& e) override {
    if (p.gated) return false;
    for (int t = 0; t < p.nTracks; t++) {
      Track& tr = p.tracks[t]; if ((tr.state != flyfig::TRACKING && tr.state != flyfig::CONFIRMED) || (tr.flags & FLAG) || tr.age < minAge || tr.missing || tr.size < minSize || tr.loom < minLoom || tr.size < grown * tr.birthSize) continue;
      tr.flags |= FLAG; e.kind = flyvision::APPROACH; e.id = tr.id; e.x = (uint8_t)tr.x; e.y = (uint8_t)tr.y; e.strength = tr.loom;
      snprintf(e.text, sizeof e.text, "object #%u is approaching (expanding %.1f%% per frame, %u columns)", tr.id, 100 * tr.loom, tr.size);
      return true;
    }
    return false;
  }
};

// the scene is different from the remembered background after the motion stopped: something was placed or removed
struct RoomChanged : Detector {
  int minColumns = 20, quietFrames = 20; int quiet = 0;
  RoomChanged() : Detector("room changed") {}
  bool detect(const Percept& p, Event& e) override {
    if (p.gated || flyfig::liveTracks()) { quiet = 0; return false; }
    if (++quiet < quietFrames) return false;
    float cx = 0, cy = 0; int n = flyfig::changedRegion(cx, cy); if (n < minColumns) return false;
    if (n > flyeye::NE * 0.4f) { flyfig::absorb(); return false; }               // most of the view changed: light or a new viewpoint, not an object
    e.kind = flyvision::ROOM_CHANGED; e.x = (uint8_t)cx; e.y = (uint8_t)cy; e.strength = (float)n / flyeye::NE;
    snprintf(e.text, sizeof e.text, "the room changed: %d columns near (%d,%d) differ from what was there", n, (int)cx, (int)cy);
    flyfig::absorb(); quiet = 0; return true;
  }
};

static NewObject newObject; static ObjectLeft objectLeft; static Approach approach; static RoomChanged roomChanged;
static void addDefaults() { flyvision::add(&newObject); flyvision::add(&objectLeft); flyvision::add(&approach); flyvision::add(&roomChanged); }

}  // namespace flydet
