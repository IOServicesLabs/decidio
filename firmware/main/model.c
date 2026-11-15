// Load the model package (.dcd) from the `model` partition: "DCD1" | u32 json_len | u32 model_len | u32 crc32 | json | pad | tflite
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "rt.h"

static const char *TAG = "model";
rt_model_t g_model;

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : (cJSON_IsBool(v) ? cJSON_IsTrue(v) : def);
}

static float jnum(const cJSON *o, const char *k, float def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void jstr(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    snprintf(dst, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static esp_err_t parse(const char *json, rt_model_t *m)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    jstr(root, "name", m->name, sizeof(m->name));
    if (jint(root, "format", 0) != 1) {
        ESP_LOGE(TAG, "unsupported package format");
        goto done;
    }
    const cJSON *im = cJSON_GetObjectItemCaseSensitive(root, "image");
    if (cJSON_IsObject(im)) {
        m->has_image = true;
        m->img_size = jint(im, "size", 96);
        m->colors = jint(im, "colors", 1);
        m->motion = jint(im, "motion", 0);
        m->ego = m->motion ? jint(im, "ego", 0) : 0;
        m->lag = jint(im, "lag", 0);
        m->frame_ms = jint(im, "frame_ms", 50);
        if (m->img_size < 8 || m->img_size > RT_CAM || (m->colors != 1 && m->colors != 3) || m->lag > 20) {
            goto done;
        }
    }
    const cJSON *au = cJSON_GetObjectItemCaseSensitive(root, "audio");
    if (cJSON_IsObject(au)) {
        m->has_audio = true;
        m->sr = jint(au, "sr", 16000);
        m->clip = jint(au, "clip", 16000);
        m->win = jint(au, "win", 480);
        m->hop = jint(au, "hop", 320);
        m->nfft = jint(au, "nfft", 512);
        m->n_mels = jint(au, "n_mels", 40);
        m->fmin = jnum(au, "fmin", 20);
        m->fmax = jnum(au, "fmax", 7600);
        m->mean = jnum(au, "mean", 0);
        m->std = jnum(au, "std", 1);
        m->eps = jnum(au, "eps", 1e-6f);
        m->n_frames = 1 + (m->clip - m->win) / m->hop;
        if (m->nfft & (m->nfft - 1) || m->nfft < m->win || m->nfft > 2048 || m->hop > m->win || m->n_mels > 128) {
            goto done;
        }
    }
    m->n_logits = jint(root, "n_logits", 0);
    m->arena_kb = jint(root, "arena_kb", 96);
    const cJSON *qs = cJSON_GetObjectItemCaseSensitive(root, "questions");
    int off = 0;
    const cJSON *q;
    cJSON_ArrayForEach(q, qs) {
        if (m->n_q >= RT_MAX_Q) {
            goto done;
        }
        rt_question_t *Q = &m->q[m->n_q++];
        jstr(q, "id", Q->id, sizeof(Q->id));
        char t[16];
        jstr(q, "type", t, sizeof(t));
        Q->type = !strcmp(t, "noul") ? Q_NOUL : (!strcmp(t, "choice") ? Q_CHOICE : Q_SCORE);
        jstr(q, "m", t, sizeof(t));
        Q->mod = t[0] == 'i' || t[0] == 'a' ? t[0] : 0;
        Q->offset = off;
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(q, "options")) {
            if (Q->count >= RT_MAX_OPT || !cJSON_IsString(o)) {
                goto done;
            }
            snprintf(Q->opt[Q->count++], RT_ID_LEN, "%s", o->valuestring);
        }
        off += Q->count;
    }
    if (off != m->n_logits || off > RT_MAX_LOGITS || m->n_q == 0) {
        goto done;
    }
    m->n_act = rt_rules_parse(cJSON_GetObjectItemCaseSensitive(root, "actions"), m, m->act, RT_MAX_ACT);
    err = (m->has_image || m->has_audio) ? ESP_OK : ESP_ERR_INVALID_ARG;
done:
    cJSON_Delete(root);
    return err;
}

static const uint8_t *s_json;        // the package header, inside the mapped partition
static uint32_t s_json_len;

// the rules written into the model package (the web page can replace them; this is what "reset" goes back to)
int rt_model_default_rules(rt_action_t *out, int max)
{
    int n = 0;
    char *json = s_json ? malloc(s_json_len + 1) : NULL;
    if (json) {
        memcpy(json, s_json, s_json_len);
        json[s_json_len] = 0;
        cJSON *root = cJSON_Parse(json);
        n = rt_rules_parse(cJSON_GetObjectItemCaseSensitive(root, "actions"), &g_model, out, max);
        cJSON_Delete(root);
        free(json);
    }
    return n;
}

esp_err_t rt_model_load(void)
{
    memset(&g_model, 0, sizeof(g_model));
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "model");
    if (!part) {
        ESP_LOGE(TAG, "no `model` partition (wrong partition table?)");
        return ESP_ERR_NOT_FOUND;
    }
    const void *map = NULL;
    esp_partition_mmap_handle_t h;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &h);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t *b = map;
    if (memcmp(b, "DCD1", 4) != 0) {
        ESP_LOGW(TAG, "no model flashed (decidio flash model --port COMx)");
        esp_partition_munmap(h);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t jl, ml, crc;
    memcpy(&jl, b + 4, 4);
    memcpy(&ml, b + 8, 4);
    memcpy(&crc, b + 12, 4);
    const uint32_t pad = (16 - ((16 + jl) % 16)) % 16;
    if (16 + jl + pad + ml > part->size) {
        ESP_LOGE(TAG, "package larger than the partition");
        esp_partition_munmap(h);
        return ESP_ERR_INVALID_SIZE;
    }
    if (esp_rom_crc32_le(0, b + 16, jl + pad + ml) != crc) {
        ESP_LOGE(TAG, "package checksum mismatch (re-flash the model)");
        esp_partition_munmap(h);
        return ESP_ERR_INVALID_CRC;
    }
    char *json = malloc(jl + 1);
    if (!json) {
        esp_partition_munmap(h);
        return ESP_ERR_NO_MEM;
    }
    memcpy(json, b + 16, jl);
    json[jl] = 0;
    err = parse(json, &g_model);
    free(json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "model header invalid");
        memset(&g_model, 0, sizeof(g_model));
        esp_partition_munmap(h);
        return err;
    }
    g_model.tflite = b + 16 + jl + pad;     // stays mapped for the life of the program
    s_json = b + 16;
    s_json_len = jl;
    g_model.tflite_len = ml;
    g_model.crc = crc;
    g_model.loaded = true;
    ESP_LOGI(TAG, "model '%s': %d questions, image %s, audio %s, %u B", g_model.name, g_model.n_q,
             g_model.has_image ? "yes" : "no", g_model.has_audio ? "yes" : "no", (unsigned)ml);
    return ESP_OK;
}
