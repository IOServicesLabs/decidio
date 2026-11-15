// Part of decidio's fly eye (firmware/main/fly). From the fruitfly firmware, adapted for this runtime: see fly/README.md.
// fly_core.h — a fly mushroom body in plain C++ for microcontrollers (glomeruli → Kenyon cells → MBONs, dopamine-gated
// depression, sleep), wired with the connectome's statistics from circuit_data.h. The eye and the ear use it to stop
// reacting to what they keep seeing or hearing (a fan, a television).
// No board dependencies: the same file is compiled by the firmware and by the PC tests.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "circuit_data.h"

#ifndef FLY_MALLOC
#include <stdlib.h>
#define FLY_MALLOC(n) malloc(n)
#endif
#ifndef FLY_MALLOC_FAST
#define FLY_MALLOC_FAST(n) FLY_MALLOC(n)
#endif

namespace fly {

struct Rng {                                     // mulberry32, the same generator the JavaScript uses
  uint32_t a;
  explicit Rng(uint32_t seed) : a(seed) {}
  float next() { a += 0x6D2B79F5u; uint32_t t = a; t = (t ^ (t >> 15)) * (1u | t); t ^= t + ((t ^ (t >> 7)) * (61u | t)); return (float)((t ^ (t >> 14)) >> 0) / 4294967296.0f; }
};

struct Config {
  uint16_t nKC = 512;      // model Kenyon cells (class proportions from the connectome)
  uint8_t claws = 4;       // PN inputs per Kenyon cell
  float sparsity = 0.05f;  // APL feedback: fraction of KCs allowed to fire
  float lr = 0.4f;         // dopamine-gated depression per pairing
  float forget = 0.002f;   // labile memory decays per document
  float recover = 0.0005f; // stable synapses regrow toward naive per document
  float consolidate = 0.4f;// fraction of labile memory made stable by sleep
  float noise = 0.005f;
  float adapt = 0.004f;
  float expectRate = 0.02f;
};

// weights are fixed-point: S (stable) and L (labile depression) in 1/255 units; effective weight = max(0, S - L)
class Fly {
 public:
  Config cfg; uint16_t nKC = 0, kActive = 0, nOn = 0;
  uint8_t *S = nullptr, *L = nullptr, *kcIn = nullptr, *kcW = nullptr, *kcClass = nullptr;
  float *pre = nullptr; uint16_t *active = nullptr; uint16_t *sel = nullptr;
  float pn[N_GLOM], mb[N_MBON];
  float approach = 0, avoid = 0, raw = 0, score = 0, baseline = 0, expectBad = 0.3f, dopamine = 0;
  uint32_t seen = 0, docsSinceDecay = 0; uint16_t hist[64]; uint8_t histN = 0, histP = 0; Rng rng;

  explicit Fly(uint32_t seed) : rng(seed) {}
  size_t bytesNeeded(const Config& c) const { return (size_t)c.nKC * N_MBON * 2 + (size_t)c.nKC * c.claws * 2 + c.nKC + c.nKC * sizeof(float) + c.nKC * sizeof(uint16_t) * 2; }
  bool init(const Config& c) {
    cfg = c; nKC = c.nKC; kActive = (uint16_t)fmaxf(1, roundf(c.sparsity * nKC));
    S = (uint8_t*)FLY_MALLOC((size_t)nKC * N_MBON); L = (uint8_t*)FLY_MALLOC((size_t)nKC * N_MBON);
    kcIn = (uint8_t*)FLY_MALLOC((size_t)nKC * c.claws); kcW = (uint8_t*)FLY_MALLOC((size_t)nKC * c.claws); kcClass = (uint8_t*)FLY_MALLOC(nKC);
    pre = (float*)FLY_MALLOC(nKC * sizeof(float)); active = (uint16_t*)FLY_MALLOC(kActive * sizeof(uint16_t)); sel = (uint16_t*)FLY_MALLOC(nKC * sizeof(uint16_t));
    if (!S || !L || !kcIn || !kcW || !kcClass || !pre || !active || !sel) return false;
    // Kenyon-cell classes in the connectome's proportions
    uint32_t total = 0; for (int c2 = 0; c2 < N_KC_CLASS; c2++) total += KC_CLASS_N[c2];
    uint16_t k = 0;
    for (int c2 = 0; c2 < N_KC_CLASS && k < nKC; c2++) { uint16_t n = (uint16_t)fmaxf(1, roundf((float)nKC * KC_CLASS_N[c2] / total)); for (uint16_t i = 0; i < n && k < nKC; i++) kcClass[k++] = (uint8_t)c2; }
    while (k < nKC) kcClass[k++] = 0;
    // claws sampled in proportion to the PN→KC synapse counts of the cell's class
    for (uint16_t i = 0; i < nKC; i++) {
      int c2 = kcClass[i]; uint32_t sum = 0; for (int g = 0; g < N_GLOM; g++) sum += PN_KC[g][c2]; if (!sum) sum = 1;
      for (int j = 0; j < cfg.claws; j++) {
        float u = rng.next() * sum; uint32_t acc = 0; int gi = N_GLOM - 1;
        for (int g = 0; g < N_GLOM; g++) { acc += PN_KC[g][c2]; if (u < acc) { gi = g; break; } }
        kcIn[i * cfg.claws + j] = (uint8_t)gi; kcW[i * cfg.claws + j] = (uint8_t)(rng.next() * 255);  // 0.7 … 1.3 when decoded
      }
    }
    resetMemory(); return true;
  }
  void resetMemory() {
    for (uint16_t k = 0; k < nKC; k++) for (int m = 0; m < N_MBON; m++) S[k * N_MBON + m] = (MBON_MASK[m] >> kcClass[k]) & 1 ? 255 : 0;
    memset(L, 0, (size_t)nKC * N_MBON); baseline = 0; expectBad = 0.3f; seen = 0; histN = histP = 0;
  }
  inline int weight(uint16_t k, int m) const { int w = (int)S[k * N_MBON + m] - (int)L[k * N_MBON + m]; return w > 0 ? w : 0; }

  // glomerulus activations 0..255 → PN normalisation → KC (top-k) → MBON → approach/avoid → score
  float smell(const uint8_t* glom) {
    float sum = 0; for (int g = 0; g < N_GLOM; g++) sum += glom[g] / 255.0f;
    float norm = 1.0f / (0.25f + sum / 6.0f);
    for (int g = 0; g < N_GLOM; g++) pn[g] = glom[g] / 255.0f * norm;
    for (uint16_t k = 0; k < nKC; k++) { float s = 0; const uint8_t* in = kcIn + k * cfg.claws; const uint8_t* w = kcW + k * cfg.claws; for (int j = 0; j < cfg.claws; j++) s += (0.7f + 0.6f * w[j] / 255.0f) * pn[in[j]]; pre[k] = s; }
    // threshold = k-th largest pre-activation (partial selection)
    for (uint16_t k = 0; k < nKC; k++) sel[k] = k;
    uint16_t lo = 0, hi = nKC - 1, want = kActive - 1;                 // quickselect on indices, descending
    while (lo < hi) {
      float pivot = pre[sel[(lo + hi) / 2]]; uint16_t i = lo, j = hi;
      while (i <= j) { while (pre[sel[i]] > pivot) i++; while (pre[sel[j]] < pivot) j--; if (i <= j) { uint16_t t = sel[i]; sel[i] = sel[j]; sel[j] = t; i++; if (j == 0) break; j--; } }
      if (want <= j) hi = j; else if (want >= i) lo = i; else break;
    }
    float th = pre[sel[want]]; if (th < 1e-3f) th = 1e-3f;
    nOn = 0; for (uint16_t k = 0; k < nKC && nOn < kActive; k++) if (pre[k] >= th) active[nOn++] = k;
    for (int m = 0; m < N_MBON; m++) mb[m] = 0;
    for (uint16_t j = 0; j < nOn; j++) { uint16_t k = active[j]; for (int m = 0; m < N_MBON; m++) mb[m] += weight(k, m); }
    float ap = 0, av = 0, nAp = 0, nAv = 0, div = nOn ? 255.0f * nOn : 255.0f;
    for (int m = 0; m < N_MBON; m++) { mb[m] /= div; float s = MBON_SAY[m]; if (MBON_GATE[m] == 2) { ap += s * mb[m]; nAp += s; } else if (MBON_GATE[m] == 1) { av += s * mb[m]; nAv += s; } }
    approach = ap / (nAp + 1e-6f); avoid = av / (nAv + 1e-6f);
    raw = (approach - avoid) / (approach + avoid + 1e-6f);
    score = raw - baseline + cfg.noise * (rng.next() * 2 - 1);
    return score;
  }
  // the auditor's answer: +1 fine (sugar → PAM compartments), −1 bad (shock → PPL1 compartments)
  void learn(int valence) {
    uint8_t gate = valence > 0 ? 1 : 2;
    float pred = raw - baseline; if (pred > 1) pred = 1; if (pred < -1) pred = -1;
    float rpe = valence > 0 ? (1 - pred) / 2 : (1 + pred) / 2;
    float lr = cfg.lr * rpe; dopamine = valence * rpe;
    expectBad += cfg.expectRate * ((valence < 0 ? 1.0f : 0.0f) - expectBad);
    float a = fmaxf(cfg.adapt, 0.03f / (1 + seen / 60.0f));
    baseline += a * ((raw > baseline ? 1.0f : 0.0f) - (1 - expectBad));
    for (int m = 0; m < N_MBON; m++) {
      if (MBON_GATE[m] != gate) continue;
      for (uint16_t j = 0; j < nOn; j++) { uint32_t i = (uint32_t)active[j] * N_MBON + m; int w = (int)S[i] - (int)L[i]; if (w <= 0) continue; float d = lr * w; int di = (int)d; if (rng.next() < d - di) di++; int nl = L[i] + di; L[i] = (uint8_t)(nl > S[i] ? S[i] : nl); }
    }
    seen++;
    // forgetting and slow recovery, applied in batches of 25 documents (fixed-point cannot decay by 0.2 %)
    if (++docsSinceDecay >= 25) { docsSinceDecay = 0; decay(); }
  }
  // time passing without anything to learn: one batch of forgetting and recovery (used by the habituating noses of the eye)
  void rest() { docsSinceDecay = 0; decay(); }
  void decay() {
    float f = 1 - powf(1 - cfg.forget, 25), r = 1 - powf(1 - cfg.recover, 25); uint32_t n = (uint32_t)nKC * N_MBON;
    for (uint32_t i = 0; i < n; i++) { if (L[i]) { float d = L[i] * f; int di = (int)d; if (rng.next() < d - di) di++; L[i] -= (uint8_t)di; } uint8_t k = (uint8_t)(i / N_MBON); int m = i % N_MBON; if (((MBON_MASK[m] >> kcClass[k]) & 1) && S[i] < 255) { float d = (255 - S[i]) * r; int di = (int)d; if (rng.next() < d - di) di++; S[i] += (uint8_t)di; } }
  }
  void sleep() { uint32_t n = (uint32_t)nKC * N_MBON; for (uint32_t i = 0; i < n; i++) { if (!L[i]) continue; int d = (int)(cfg.consolidate * L[i] + 0.5f); int s = (int)S[i] - d; S[i] = (uint8_t)(s < 0 ? 0 : s); L[i] -= (uint8_t)d; } }
  // persistent memory: the stable synapses (labile part consolidated first) and the decision baseline
  size_t memoryBytes() const { return (size_t)nKC * N_MBON + 12; }
  void exportMemory(uint8_t* out) { sleep(); memcpy(out, S, (size_t)nKC * N_MBON); float* f = (float*)(out + (size_t)nKC * N_MBON); f[0] = baseline; f[1] = expectBad; uint32_t* u = (uint32_t*)(f + 2); *u = seen; }
  void importMemory(const uint8_t* in) { memcpy(S, in, (size_t)nKC * N_MBON); memset(L, 0, (size_t)nKC * N_MBON); const float* f = (const float*)(in + (size_t)nKC * N_MBON); baseline = f[0]; expectBad = f[1]; seen = *(const uint32_t*)(f + 2); }
  void record(bool correct) { hist[histP] = correct; histP = (histP + 1) % 64; if (histN < 64) histN++; }
  float recentAccuracy() const { if (!histN) return -1; int s = 0; for (int i = 0; i < histN; i++) s += hist[i]; return (float)s / histN; }
  float memoryLoad() const { uint32_t n = (uint32_t)nKC * N_MBON, cnt = 0; float s = 0; for (uint32_t i = 0; i < n; i++) if (S[i] || L[i]) { s += 1 - weight(i / N_MBON, i % N_MBON) / 255.0f; cnt++; } return cnt ? s / cnt : 0; }
};

}  // namespace fly
