// TFLite Micro + ESP-NN inference for whatever model the package holds. Inputs are matched by shape.
#include <math.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "rt.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_profiler.h"
#if __has_include("tensorflow/lite/schema/schema_generated.h")      // esp-tflite-micro up to 1.4.1
#include "tensorflow/lite/schema/schema_generated.h"
#else
#include "tensorflow/lite/micro/schema/schema_generated.h"
#endif

namespace {

const char *TAG = "decide";
tflite::MicroInterpreter *g_interp = nullptr;
TfLiteTensor *g_img = nullptr, *g_mel = nullptr, *g_out = nullptr;
int8_t g_lut[256];
float g_mel_inv_scale = 1.0f;
int g_mel_zp = 0, g_idx[RT_CAM];
uint32_t g_prep_us = 0;
int g_ego_dx = 0, g_ego_dy = 0;
uint8_t g_lcur[RT_CAM * RT_CAM], g_lprev[RT_CAM * RT_CAM];   // luma of both frames (motion: ego, colour models)
tflite::MicroProfiler *g_profiler;       // 82 KB of event slots: kept in PSRAM, internal RAM is for Wi-Fi
bool g_profile = false;
size_t g_arena_size = 0;
bool g_arena_internal = false;

inline int8_t sat8(int v) { return (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v)); }

bool shape_is(const TfLiteTensor *t, int a, int b, int c)
{
    return t->dims->size == 4 && t->dims->data[1] == a && t->dims->data[2] == b && t->dims->data[3] == c;
}

void append(char *buf, size_t cap, size_t *n, const char *fmt, ...)
{
    if (*n >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (w > 0) {
        *n += (size_t)w;
    }
}

}  // namespace

extern "C" esp_err_t rt_decide_init(void)
{
    const rt_model_t &m = g_model;
    const uint8_t *bytes = m.tflite;
    static tflite::MicroMutableOpResolver<25> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddFullyConnected();
    resolver.AddMean();
    resolver.AddAveragePool2D();
    resolver.AddMaxPool2D();
    resolver.AddRelu();
    resolver.AddRelu6();
    resolver.AddLogistic();
    resolver.AddSoftmax();
    resolver.AddMul();
    resolver.AddAdd();
    resolver.AddSub();
    resolver.AddConcatenation();
    resolver.AddReshape();
    resolver.AddQuantize();
    resolver.AddDequantize();
    resolver.AddPad();
    resolver.AddStridedSlice();
    resolver.AddPack();
    resolver.AddShape();
    resolver.AddTranspose();
    resolver.AddSqueeze();
    resolver.AddTanh();
    resolver.AddHardSwish();

    const tflite::Model *model = tflite::GetModel(bytes);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "tflite schema %lu != %d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    // arena: internal SRAM if it fits and leaves 110 KB for Wi-Fi, HTTPS and the web page (fast), else PSRAM;
    // grow if the estimate was short
    size_t want = (size_t)(m.arena_kb > 16 ? m.arena_kb : 64) * 1024;
    for (int attempt = 0; attempt < 4; attempt++, want = want * 3 / 2) {
        // ask first, do not try: a trial block that is given back again still shows as the lowest free memory
        uint8_t *arena = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > want + 110 * 1024
                             ? (uint8_t *)heap_caps_aligned_alloc(16, want, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
        const bool internal = arena != nullptr;
        if (!arena) {
            arena = (uint8_t *)heap_caps_aligned_alloc(16, want, MALLOC_CAP_SPIRAM);
        }
        if (!arena) {
            ESP_LOGE(TAG, "no memory for a %u B arena", (unsigned)want);
            return ESP_ERR_NO_MEM;
        }
        static uint8_t interp_mem[sizeof(tflite::MicroInterpreter)] __attribute__((aligned(16)));
        if (!g_profiler) {
            void *pm = heap_caps_malloc(sizeof(tflite::MicroProfiler), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pm) {
                g_profiler = new (pm) tflite::MicroProfiler();
            }
        }
        auto *ip = new (interp_mem) tflite::MicroInterpreter(model, resolver, arena, want, nullptr, g_profiler);
        if (ip->AllocateTensors() == kTfLiteOk) {
            g_interp = ip;
            g_arena_size = want;
            g_arena_internal = internal;
            break;
        }
        ip->~MicroInterpreter();
        heap_caps_free(arena);
    }
    if (!g_interp) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < g_interp->inputs_size(); i++) {
        TfLiteTensor *t = g_interp->input(i);
        if (m.has_image && shape_is(t, m.img_size, m.img_size, m.colors + (m.motion ? 1 : 0))) {
            g_img = t;
        } else if (m.has_audio && shape_is(t, m.n_frames, m.n_mels, 1)) {
            g_mel = t;
        }
    }
    g_out = g_interp->output(0);
    if ((m.has_image && !g_img) || (m.has_audio && !g_mel) || g_out->type != kTfLiteInt8 ||
        g_out->dims->data[g_out->dims->size - 1] != m.n_logits) {
        ESP_LOGE(TAG, "model tensors do not match its header");
        return ESP_ERR_INVALID_STATE;
    }
    if (g_img) {
        for (int v = 0; v < 256; v++) {
            g_lut[v] = sat8((int)lroundf((v / 255.0f) / g_img->params.scale) + g_img->params.zero_point);
        }
        for (int i = 0; i < m.img_size; i++) {
            g_idx[i] = i * RT_CAM / m.img_size;
        }
    }
    if (g_mel) {
        g_mel_inv_scale = 1.0f / g_mel->params.scale;
        g_mel_zp = g_mel->params.zero_point;
    }
    ESP_LOGI(TAG, "arena %u / %u B (%s)", (unsigned)g_interp->arena_used_bytes(), (unsigned)g_arena_size,
             g_arena_internal ? "SRAM" : "PSRAM");
    return ESP_OK;
}

extern "C" size_t rt_decide_arena_used(void)
{
    return g_interp ? g_interp->arena_used_bytes() : 0;
}

extern "C" void rt_decide_inputs(const uint8_t *cur, const uint8_t *prev, const float *mel)
{
    const int64_t t0 = esp_timer_get_time();
    const rt_model_t &m = g_model;
    if (g_img && cur && m.motion && m.ego) {
        // a moving camera: find its own shift between the frames, then difference against the shifted earlier frame
        const int C = m.colors;
        const uint8_t *lc = cur, *lp = prev;
        if (C == 3) {
            for (int i = 0; i < RT_CAM * RT_CAM; i++) {   // integer BT.601 luma, as media.luma()
                g_lcur[i] = (uint8_t)((cur[3 * i] * 77 + cur[3 * i + 1] * 150 + cur[3 * i + 2] * 29) >> 8);
                g_lprev[i] = (uint8_t)((prev[3 * i] * 77 + prev[3 * i + 1] * 150 + prev[3 * i + 2] * 29) >> 8);
            }
            lc = g_lcur;
            lp = g_lprev;
        }
        rt_ego_shift(lc, lp, &g_ego_dx, &g_ego_dy);
        int8_t *d = g_img->data.int8;
        for (int y = 0; y < m.img_size; y++) {
            const int sy = g_idx[y];
            for (int x = 0; x < m.img_size; x++) {
                const int sx = g_idx[x], k = (sy * RT_CAM + sx) * C;
                for (int c = 0; c < C; c++) {
                    *d++ = g_lut[cur[k + c]];
                }
                *d++ = g_lut[rt_ego_diff(lc, lp, sx, sy, g_ego_dx, g_ego_dy)];
            }
        }
    } else if (g_img && cur) {
        int8_t *d = g_img->data.int8;
        const int C = m.colors;
        for (int y = 0; y < m.img_size; y++) {
            const int row = g_idx[y] * RT_CAM;
            for (int x = 0; x < m.img_size; x++) {
                const int k = (row + g_idx[x]) * C;
                for (int c = 0; c < C; c++) {
                    *d++ = g_lut[cur[k + c]];
                }
                if (m.motion) {
                    int a, b;
                    if (C == 1) {
                        a = cur[k];
                        b = prev[k];
                    } else {   // integer BT.601 luma, as media.luma()
                        a = (cur[k] * 77 + cur[k + 1] * 150 + cur[k + 2] * 29) >> 8;
                        b = (prev[k] * 77 + prev[k + 1] * 150 + prev[k + 2] * 29) >> 8;
                    }
                    *d++ = g_lut[a > b ? a - b : b - a];
                }
            }
        }
    }
    if (g_mel && mel) {
        int8_t *d = g_mel->data.int8;
        const int n = m.n_frames * m.n_mels;
        for (int i = 0; i < n; i++) {
            d[i] = sat8((int)lroundf(mel[i] * g_mel_inv_scale) + g_mel_zp);
        }
    }
    g_prep_us = (uint32_t)(esp_timer_get_time() - t0);
}

extern "C" esp_err_t rt_decide_invoke(rt_result_t *r)
{
    const int64_t t1 = esp_timer_get_time();
    if (g_profiler) {
        g_profiler->ClearEvents();       // every run: the profiler keeps at most 4096 events and complains beyond that
    }
    if (g_interp->Invoke() != kTfLiteOk) {
        return ESP_FAIL;
    }
    const int64_t t2 = esp_timer_get_time();
    const float scale = g_out->params.scale;
    const int zp = g_out->params.zero_point;
    const int8_t *lo = g_out->data.int8;
    for (int q = 0; q < g_model.n_q; q++) {
        const rt_question_t &Q = g_model.q[q];
        float *p = &r->probs[Q.offset], mx = -INFINITY, sum = 0.0f;
        for (int j = 0; j < Q.count; j++) {
            p[j] = (float)(lo[Q.offset + j] - zp) * scale;
            mx = fmaxf(mx, p[j]);
        }
        for (int j = 0; j < Q.count; j++) {
            p[j] = expf(p[j] - mx);
            sum += p[j];
        }
        for (int j = 0; j < Q.count; j++) {
            p[j] /= sum;
        }
    }
    r->prep_us = g_prep_us;
    r->invoke_us = (uint32_t)(t2 - t1);
    r->ego_dx = g_ego_dx;
    r->ego_dy = g_ego_dy;
    return ESP_OK;
}

static char g_last[2048];                 // the last decision line, for the web page
static portMUX_TYPE g_last_mux = portMUX_INITIALIZER_UNLOCKED;

extern "C" int rt_decide_last(char *dst, size_t cap)
{
    portENTER_CRITICAL(&g_last_mux);
    const size_t n = strnlen(g_last, sizeof(g_last) - 1);
    const size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(dst, g_last, k);
    portEXIT_CRITICAL(&g_last_mux);
    dst[k] = 0;
    return (int)k;
}

extern "C" void rt_decide_print(const rt_result_t *r, float hz)
{
    static char buf[2048];
    size_t n = 0;
    const size_t cap = sizeof(buf);
    append(buf, cap, &n, "{\"answers\":{");
    for (int q = 0; q < g_model.n_q; q++) {
        const rt_question_t &Q = g_model.q[q];
        const float *p = &r->probs[Q.offset];
        int best = 0;
        for (int j = 1; j < Q.count; j++) {
            if (p[j] > p[best]) {
                best = j;
            }
        }
        append(buf, cap, &n, "%s\"%s\":", q ? "," : "", Q.id);
        if (Q.type == Q_NOUL) {
            append(buf, cap, &n, "{\"p_true\":%.3f}", p[1]);
        } else if (Q.type == Q_CHOICE) {
            append(buf, cap, &n, "{\"choice\":\"%s\",\"confidence\":%.3f,\"probabilities\":{", Q.opt[best], p[best]);
            for (int j = 0; j < Q.count; j++) {
                append(buf, cap, &n, "%s\"%s\":%.3f", j ? "," : "", Q.opt[j], p[j]);
            }
            append(buf, cap, &n, "}}");
        } else {
            float e = 0;
            for (int j = 0; j < Q.count; j++) {
                e += j * p[j];
            }
            append(buf, cap, &n, "{\"score\":%.2f,\"confidence\":%.3f,\"probabilities\":[", e, p[best]);
            for (int j = 0; j < Q.count; j++) {
                append(buf, cap, &n, "%s%.3f", j ? "," : "", p[j]);
            }
            append(buf, cap, &n, "]}");
        }
    }
    append(buf, cap, &n, "}");
    if (g_model.ego) {
        append(buf, cap, &n, ",\"ego\":[%d,%d]", r->ego_dx, r->ego_dy);
    }
    append(buf, cap, &n, ",\"prep_us\":%lu,\"invoke_us\":%lu,\"hz\":%.1f}", (unsigned long)r->prep_us,
           (unsigned long)r->invoke_us, hz);
    portENTER_CRITICAL(&g_last_mux);
    memcpy(g_last, buf, n + 1);
    portEXIT_CRITICAL(&g_last_mux);
    puts(buf);
    if (g_profile && g_profiler) {
        g_profiler->LogTicksPerTagCsv();
    }
}

extern "C" void rt_decide_profile(bool on)
{
    g_profile = on;
}
