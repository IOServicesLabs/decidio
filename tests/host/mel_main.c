// reads: int32 sr clip win hop nfft n_mels, float fmin fmax mean std, then int16 pcm[clip]; writes n_frames*n_mels floats
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"
rt_model_t g_model;
int main(void) {
    int hdr[6]; float f[4];
    if (fread(hdr, 4, 6, stdin) != 6 || fread(f, 4, 4, stdin) != 4) return 1;
    rt_model_t *m = &g_model;
    m->sr = hdr[0]; m->clip = hdr[1]; m->win = hdr[2]; m->hop = hdr[3]; m->nfft = hdr[4]; m->n_mels = hdr[5];
    m->fmin = f[0]; m->fmax = f[1]; m->mean = f[2]; m->std = f[3]; m->eps = 1e-6f;
    m->n_frames = 1 + (m->clip - m->win) / m->hop;
    if (rt_mel_init(m) != ESP_OK) return 2;
    short *pcm = malloc(sizeof(short) * m->clip);
    if (fread(pcm, 2, m->clip, stdin) != (size_t)m->clip) return 3;
    float *out = malloc(sizeof(float) * m->n_mels);
    for (int t = 0; t < m->n_frames; t++) {          // end-aligned frames, like the streaming firmware
        int start = (m->clip - m->win) - m->hop * (m->n_frames - 1 - t);
        rt_mel_frame(pcm + start, out);
        fwrite(out, 4, m->n_mels, stdout);
    }
    return 0;
}
