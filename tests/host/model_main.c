// loads a .dcd through firmware/main/model.c and prints what the firmware understood
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_partition.h"
#include "rt.h"
static esp_partition_t s_part; static unsigned char *s_buf;
const esp_partition_t *esp_partition_find_first(int t, int s, const char *l) { (void)t; (void)s; (void)l; return &s_part; }
esp_err_t esp_partition_mmap(const esp_partition_t *p, size_t off, size_t size, int m, const void **out, esp_partition_mmap_handle_t *h)
{ (void)p; (void)size; (void)m; *out = s_buf + off; *h = 0; return ESP_OK; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t size)
{ if (off + size > p->size) return -1; memcpy(dst, s_buf + off, size); return 0; }
void esp_partition_munmap(esp_partition_mmap_handle_t h) { (void)h; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); s_part.size = 0x100000; s_buf = calloc(1, s_part.size);
    size_t n = fread(s_buf, 1, s_part.size, f); fclose(f); (void)n;
    if (argc > 2) s_buf[atoi(argv[2])] ^= 0xff;   // corrupt one byte
    esp_err_t e = rt_model_load(); const rt_model_t *m = &g_model;
    if (e != ESP_OK) { printf("{\"err\":%d}\n", e); return 0; }
    printf("{\"err\":0,\"name\":\"%s\",\"img\":[%d,%d,%d,%d,%d,%d],\"aud\":[%d,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.6f,%.6f],",
           m->name, m->has_image, m->img_size, m->colors, m->motion, m->lag, m->ego, m->has_audio, m->sr, m->clip, m->win, m->hop,
           m->nfft, m->n_mels, m->fmin, m->fmax, m->mean, m->std);
    printf("\"n_logits\":%d,\"arena_kb\":%d,\"q\":[", m->n_logits, m->arena_kb);
    for (int i = 0; i < m->n_q; i++) {
        printf("%s[\"%s\",%d,%d,%d,[", i ? "," : "", m->q[i].id, m->q[i].type, m->q[i].offset, m->q[i].count);
        for (int j = 0; j < m->q[i].count; j++) printf("%s\"%s\"", j ? "," : "", m->q[i].opt[j]);
        printf("]]");
    }
    printf("],\"act\":[");
    for (int i = 0; i < m->n_act; i++) printf("%s[\"%s\",%d,%d,%.3f,%d,%d,%d,%d]", i ? "," : "", m->act[i].name, m->act[i].c[0].q, m->act[i].c[0].o,
        m->act[i].c[0].p, m->act[i].for_ms, m->act[i].gpio, m->act[i].level, m->act[i].hold_ms);
    printf("],\"tfl_off\":%ld,\"tfl_len\":%zu}\n", (long)(m->tflite - s_buf), m->tflite_len);
    return 0;
}
