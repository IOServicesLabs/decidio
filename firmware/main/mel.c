// Log-mel frontend, parameters from the model. Mirrors decidio/media.py log_mel() (checked by tests/mel_parity).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "rt.h"

#define RT_PI 3.14159265358979323846

static int s_win, s_nfft, s_nmels, s_nbins;
static float s_mean, s_inv_std, s_eps;
static float *s_window, *s_cos, *s_sin, *s_re, *s_im, *s_pow, *s_w;
static uint16_t *s_start, *s_len;

static void *alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static double hz2mel(double f) { return 2595.0 * log10(1.0 + f / 700.0); }
static double mel2hz(double m) { return 700.0 * (pow(10.0, m / 2595.0) - 1.0); }

esp_err_t rt_mel_init(const rt_model_t *m)
{
    s_win = m->win;
    s_nfft = m->nfft;
    s_nmels = m->n_mels;
    s_nbins = m->nfft / 2 + 1;
    s_mean = m->mean;
    s_inv_std = 1.0f / m->std;
    s_eps = m->eps;
    s_window = alloc(sizeof(float) * s_win);
    s_cos = alloc(sizeof(float) * s_nfft / 2);
    s_sin = alloc(sizeof(float) * s_nfft / 2);
    s_re = alloc(sizeof(float) * s_nfft);
    s_im = alloc(sizeof(float) * s_nfft);
    s_pow = alloc(sizeof(float) * s_nbins);
    s_start = alloc(sizeof(uint16_t) * s_nmels);
    s_len = alloc(sizeof(uint16_t) * s_nmels);
    if (!s_window || !s_cos || !s_sin || !s_re || !s_im || !s_pow || !s_start || !s_len) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_win; i++) {
        s_window[i] = (float)(0.5 - 0.5 * cos(2.0 * RT_PI * i / s_win));
    }
    for (int k = 0; k < s_nfft / 2; k++) {
        s_cos[k] = (float)cos(2.0 * RT_PI * k / s_nfft);
        s_sin[k] = (float)-sin(2.0 * RT_PI * k / s_nfft);
    }
    // triangular HTK-mel filters, stored sparse (same math as media.mel_filterbank, in double)
    double *pts = malloc(sizeof(double) * (s_nmels + 2));
    float *dense = malloc(sizeof(float) * s_nbins);
    if (!pts || !dense) {
        free(pts);
        free(dense);
        return ESP_ERR_NO_MEM;
    }
    const double lo = hz2mel(m->fmin), hi = hz2mel(m->fmax);
    for (int i = 0; i < s_nmels + 2; i++) {
        pts[i] = mel2hz(lo + (hi - lo) * i / (s_nmels + 1));
    }
    size_t total = 0;
    float *weights = NULL;
    for (int pass = 0; pass < 2; pass++) {
        size_t w = 0;
        for (int b = 0; b < s_nmels; b++) {
            const double l = pts[b], c = pts[b + 1], h = pts[b + 2];
            int first = -1, last = -1;
            for (int k = 0; k < s_nbins; k++) {
                const double f = (double)k * m->sr / s_nfft;
                double v = fmin((f - l) / (c - l), (h - f) / (h - c));
                dense[k] = v > 0 ? (float)v : 0.0f;
                if (dense[k] > 0) {
                    if (first < 0) {
                        first = k;
                    }
                    last = k;
                }
            }
            if (first < 0) {
                first = last = 0;
            }
            if (pass == 1) {
                s_start[b] = (uint16_t)first;
                s_len[b] = (uint16_t)(last - first + 1);
                memcpy(weights + w, dense + first, sizeof(float) * (last - first + 1));
            }
            w += last - first + 1;
        }
        if (pass == 0) {
            total = w;
            weights = alloc(sizeof(float) * total);
            if (!weights) {
                free(pts);
                free(dense);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    s_w = weights;
    free(pts);
    free(dense);
    return ESP_OK;
}

static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                const float wr = s_cos[k * step], wi = s_sin[k * step];
                const int a = i + k, b = a + half;
                const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
}

void rt_mel_frame(const int16_t *win, float *out)
{
    for (int i = 0; i < s_win; i++) {
        s_re[i] = ((float)win[i] / 32768.0f) * s_window[i];
        s_im[i] = 0.0f;
    }
    for (int i = s_win; i < s_nfft; i++) {
        s_re[i] = s_im[i] = 0.0f;
    }
    fft(s_re, s_im, s_nfft);
    for (int k = 0; k < s_nbins; k++) {
        s_pow[k] = s_re[k] * s_re[k] + s_im[k] * s_im[k];
    }
    const float *w = s_w;
    for (int b = 0; b < s_nmels; b++) {
        const float *p = s_pow + s_start[b];
        float acc = 0.0f;
        for (int j = 0; j < s_len[b]; j++) {
            acc += w[j] * p[j];
        }
        w += s_len[b];
        out[b] = (logf(acc + s_eps) - s_mean) * s_inv_std;
    }
}
