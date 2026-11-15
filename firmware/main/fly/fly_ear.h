// Part of decidio's fly eye (firmware/main/fly). From the fruitfly firmware, adapted for this runtime: see fly/README.md.
// fly_ear.h — the fly's ear on the XIAO's microphone: sounds as smells for mushroom bodies.
//
// A fly hears with Johnston's organ: the arista on the antenna vibrates with the air, and a few hundred neurons
// tuned to different frequencies (JO-A, JO-B …) report how much of each is present.  That is a filter bank, and a
// filter bank's output is a smell — 56 bands, one per glomerulus, so the same mushroom body that sniffs claims
// and figures sniffs sounds:
//
//   resonators     56 second-order band-pass filters, 100 Hz … 7 kHz, log-spaced (the JO neurons' tuning)
//   spectrum       log energy per band, per 16 ms frame; the loudest bands within 30 dB of the peak are the smell
//   averaging      a 2 s running average of the spectrum and a slow (30 s) background level.  A sound is a rise of
//                  the level over the background; it stays one sound until the level falls back for 1.5 s.  A TV
//                  that plays for an hour is announced once when it starts and once when it stops — in between the
//                  ongoing sound is sniffed every half second and habituated (the KC→MBON depression), so only a
//                  DIFFERENT sound on top of it (a door over the TV) is worth a word, and no more than once per 5 s
//   habituation    a mushroom body with no teacher: the fridge, the fan and the TV stop being novel
//   classes        one mushroom body per class (like the departments of the claims centre): teach by naming the
//                  sound you just heard (TEACH n) — the last four seconds of spectra are rewarded for that class
//                  and punished for the others.  A few examples per class is enough; the flies say "unknown"
//                  when no class scores.
// No Arduino dependencies: ear_test.cpp runs it on synthetic sound.  fly_mic.h feeds it from the PDM microphone.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "fly_core.h"

namespace flyear {

static const float PI_F = 3.14159265f;
static const int RATE = 16000, FRAME = 256, NB = N_GLOM;       // 16 ms frames; one band per glomerulus (56)
static const int NCLASS = 6, TEACH_RING = 8;                     // classes, and how many recent sniffs a lesson covers (8 x 0.5 s)
static const float F_LO = 100, F_HI = 7000, Q = 10;              // the resonators
static const float ONSET_DB = 0.5f, OFFSET_DB = 0.25f;           // log10 units above the background: 5 dB to start a sound, back within 2.5 dB to end it
static const int ONSET_FRAMES = 3, OFFSET_FRAMES = 94;           // 48 ms to start, 1.5 s of quiet to end
static const int SNIFF_EVERY = 31;                                // ~0.5 s between sniffs of an ongoing sound
static const float DIFFERENT_NOVELTY = 0.45f, DIFFERENT_DB = 0.3f; // a different sound over an ongoing one: novel, and 3 dB above the ongoing average
static const uint32_t DIFFERENT_GAP_MS = 5000;
static const float NEW_MARGIN = 0.1f;                             // a sound is news when its novelty beats the familiar world's by this much

enum State : uint8_t { SILENT = 0, SOUND = 1 };
enum Kind : uint8_t { NONE = 0, STARTED = 1, DIFFERENT = 2, STOPPED = 3 };
struct Band { float b0, a1, a2, z1, z2, e; float f; };
struct Event { uint8_t kind; float novelty, level; int8_t cls; float score; uint32_t ms, durationMs; int peakHz; char text[64]; };

static Band bands[NB]; static float spec[NB], avg[NB]; static uint8_t glom[NB], avgGlom[NB];
static float level = -4, bgLevel = -4, soundLevel = -4, novelty = 0, typNov = 0.2f, scores[NCLASS]; static int8_t predicted = -1;   // typNov: what the familiar world scores now
static State state = SILENT; static int above = 0, below = 0, frames = 0, sinceSniff = 0; static uint32_t soundStartMs = 0, lastDifferentMs = 0, lastMs = 0;
static bool announced = false; static fly::Fly* habit = nullptr; static fly::Fly* cls[NCLASS]; static int nClass = 0;
static uint8_t teachRing[TEACH_RING][NB]; static int teachN = 0, teachP = 0; static uint16_t taught[NCLASS];   // lessons per class: a class nobody has taught does not compete

static void design(Band& b, float f) {
  float w = 2 * PI_F * f / RATE, alpha = sinf(w) / (2 * Q), a0 = 1 + alpha;       // RBJ band-pass, constant skirt gain
  b.b0 = alpha / a0; b.a1 = -2 * cosf(w) / a0; b.a2 = (1 - alpha) / a0; b.z1 = b.z2 = 0; b.e = 0; b.f = f;
}
static bool begin(fly::Fly* habituation, fly::Fly** classes, int n) {
  for (int k = 0; k < NB; k++) design(bands[k], F_LO * powf(F_HI / F_LO, (float)k / (NB - 1)));
  memset(spec, 0, sizeof spec); memset(avg, 0, sizeof avg); memset(scores, 0, sizeof scores); memset(taught, 0, sizeof taught);
  habit = habituation; nClass = n > NCLASS ? NCLASS : n; for (int i = 0; i < nClass; i++) cls[i] = classes[i];
  return true;
}
static void reset() { state = SILENT; above = below = 0; announced = false; bgLevel = level; teachN = teachP = 0; }
static int peakHz() { int k = 0; for (int i = 1; i < NB; i++) if (spec[i] > spec[k]) k = i; return (int)bands[k].f; }

// the smell: bands within 15 dB of the loudest, 0..255
static void toGlom(const float* s, uint8_t* g) { float mx = -99; for (int k = 0; k < NB; k++) if (s[k] > mx) mx = s[k]; for (int k = 0; k < NB; k++) { float v = (s[k] - (mx - 1.5f)) / 1.5f; g[k] = (uint8_t)(255 * (v < 0 ? 0 : v > 1 ? 1 : v)); } }
// what is new: the bands louder now than in the running average (the JO neurons adapt to an ongoing sound and report the change), 10 dB range
static void toDeltaGlom(const float* s, const float* a, uint8_t* g) { for (int k = 0; k < NB; k++) { float v = (s[k] - a[k]) / 1.0f; g[k] = (uint8_t)(255 * (v < 0 ? 0 : v > 1 ? 1 : v)); } }
static void classify(const uint8_t* g) {
  predicted = -1; float best = 0.05f;
  for (int i = 0; i < nClass; i++) { scores[i] = cls[i]->smell(g); if (taught[i] && scores[i] > best) { best = scores[i]; predicted = (int8_t)i; } }
}
static void remember(const uint8_t* g) { memcpy(teachRing[teachP], g, NB); teachP = (teachP + 1) % TEACH_RING; if (teachN < TEACH_RING) teachN++; }
// the sound of the last few seconds was class c: reward that fly, punish the others
static int teach(int c) {
  if (c < 0 || c >= nClass || !teachN) return 0;
  for (int j = 0; j < teachN; j++) { const uint8_t* g = teachRing[j]; for (int i = 0; i < nClass; i++) { cls[i]->smell(g); cls[i]->learn(i == c ? 1 : -1); } }
  for (int i = 0; i < nClass; i++) cls[i]->sleep();
  taught[c]++; return teachN;
}

// one frame of 16-bit samples; returns true and fills e when there is something to say
static bool processFrame(const int16_t* s, uint32_t ms, Event& e) {
  frames++; lastMs = ms; memset(&e, 0, sizeof e);
  for (int k = 0; k < NB; k++) bands[k].e = 0;
  for (int n = 0; n < FRAME; n++) {                                                        // the resonators (transposed direct form II)
    float x = s[n] / 32768.0f;
    for (int k = 0; k < NB; k++) { Band& b = bands[k]; float y = b.b0 * x + b.z1; b.z1 = -b.b0 * x - b.a1 * y + b.z2; b.z2 = -b.a2 * y; b.e += y * y; }
  }
  float tot = 1e-9f; for (int k = 0; k < NB; k++) { spec[k] = log10f(bands[k].e / FRAME + 1e-9f); tot += bands[k].e / FRAME; }
  level = log10f(tot);
  for (int k = 0; k < NB; k++) avg[k] += 0.008f * (spec[k] - avg[k]);                        // ~2 s
  if (state == SILENT) bgLevel += (level > bgLevel ? 0.002f : 0.02f) * (level - bgLevel);   // the background follows quiet quickly, loudness slowly
  if (frames < 60) { bgLevel = level; return false; }                                       // settle
  bool loud = level > bgLevel + ONSET_DB;
  if (state == SILENT) {
    if (++sinceSniff >= SNIFF_EVERY * 4) { sinceSniff = 0; typNov += 0.05f * (0.2f - typNov); }   // in quiet the familiar world slowly stops being a reference
    above = loud ? above + 1 : 0;
    if (above >= ONSET_FRAMES) {
      state = SOUND; soundStartMs = ms; soundLevel = level; below = 0; sinceSniff = 0;
      toGlom(spec, glom); remember(glom); classify(glom);
      if (habit) { habit->smell(glom); novelty = habit->approach; habit->learn(-1); }
      announced = novelty > typNov + NEW_MARGIN && novelty > 0.12f;                       // news = clearly newer than what the fly is used to (the TV back from an ad break is not)
      if (announced) { e.kind = STARTED; e.novelty = novelty; e.level = level; e.cls = predicted; e.score = predicted >= 0 ? scores[predicted] : 0; e.ms = ms; e.peakHz = peakHz();
        if (predicted >= 0) snprintf(e.text, sizeof e.text, "sound started: class %d, loudest near %d Hz, %.0f dB over quiet", predicted + 1, e.peakHz, 20 * (level - bgLevel));
        else snprintf(e.text, sizeof e.text, "sound started: unknown, loudest near %d Hz, %.0f dB over quiet", e.peakHz, 20 * (level - bgLevel));
        return true; }
    }
    return false;
  }
  // an ongoing sound
  soundLevel += 0.01f * (level - soundLevel);
  below = level < bgLevel + OFFSET_DB ? below + 1 : 0;
  if (below >= OFFSET_FRAMES) {
    state = SILENT; above = 0; uint32_t dur = ms - soundStartMs;
    if (announced) { e.kind = STOPPED; e.ms = ms; e.durationMs = dur; e.cls = predicted; snprintf(e.text, sizeof e.text, "sound stopped after %lu s", (unsigned long)(dur / 1000)); return true; }
    return false;
  }
  // a jump in level over the ongoing sound is checked at once (a door slam lasts 60 ms): is it something new?
  if (level > soundLevel + DIFFERENT_DB && ms - lastDifferentMs > DIFFERENT_GAP_MS && ms - soundStartMs > 1000 && habit) {
    toDeltaGlom(spec, avg, glom); habit->smell(glom); novelty = habit->approach;
    if (novelty >= DIFFERENT_NOVELTY || novelty > typNov + NEW_MARGIN) {
      lastDifferentMs = ms; habit->learn(-1); toGlom(spec, glom); remember(glom); classify(glom);
      e.kind = DIFFERENT; e.novelty = novelty; e.level = level; e.cls = predicted; e.score = predicted >= 0 ? scores[predicted] : 0; e.ms = ms; e.peakHz = peakHz();
      if (predicted >= 0) snprintf(e.text, sizeof e.text, "a different sound over the ongoing one: class %d, near %d Hz", predicted + 1, e.peakHz);
      else snprintf(e.text, sizeof e.text, "a different sound over the ongoing one, near %d Hz", e.peakHz);
      return true;
    }
  }
  if (++sinceSniff >= SNIFF_EVERY) {                                                         // every half second: sniff the average and habituate to it
    sinceSniff = 0; toGlom(avg, avgGlom); remember(avgGlom);
    if (habit) { habit->smell(avgGlom); typNov += 0.1f * (habit->approach - typNov); uint8_t d[NB]; toDeltaGlom(spec, avg, d); habit->smell(d); habit->learn(-1); }   // the ongoing sound's own fluctuations become familiar; typNov tracks how familiar the world is
    classify(avgGlom);
  }
  return false;
}

// the streamed packet:  A5 5A 'A' 01 | u16 len | u32 ms, u8 state, u8 announced, u8 predicted (255 = none), u8 nClass, f32 level, bgLevel, soundLevel, novelty,
//                       f32 scores[6], u32 soundStartMs, u8 spec[56] (0..255 over -9..0 log10), u8 avgGlom[56] | checksum
static const int PACKET_BYTES = 6 + 4 + 4 + 16 + 24 + 4 + 2 * NB + 1;
static size_t packet(uint8_t* out) {
  uint8_t* p = out; *p++ = 0xA5; *p++ = 0x5A; *p++ = 'A'; *p++ = 1; uint16_t len = 4 + 4 + 16 + 24 + 4 + 2 * NB; *p++ = len & 255; *p++ = len >> 8; uint8_t* pay = p;
  memcpy(p, &lastMs, 4); p += 4; *p++ = state; *p++ = announced ? 1 : 0; *p++ = predicted < 0 ? 255 : (uint8_t)predicted; *p++ = (uint8_t)nClass;
  float f[4] = { level, bgLevel, soundLevel, novelty }; memcpy(p, f, 16); p += 16; memcpy(p, scores, 24); p += 24; memcpy(p, &soundStartMs, 4); p += 4;
  for (int k = 0; k < NB; k++) { float v = (spec[k] + 9) / 9; *p++ = (uint8_t)(255 * (v < 0 ? 0 : v > 1 ? 1 : v)); }
  uint8_t g[NB]; toGlom(avg, g); memcpy(p, g, NB); p += NB;
  uint8_t sum = 0; for (uint8_t* q = pay; q < p; q++) sum += *q; *p++ = sum; return p - out;
}

}  // namespace flyear
