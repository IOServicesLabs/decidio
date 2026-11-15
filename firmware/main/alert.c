// Alerts: when a rule fires, a short message goes to Telegram, to a webhook and/or to an MQTT broker.
//
// Everything is sent from this file's own task, fed by a queue, so a slow or dead network never delays a decision.
// At most 20 messages a minute leave the board; what does not fit is dropped and counted. A message is text. A photo
// (the 96x96 frame the model saw, doubled to 192x192) is attached only for rules that ask for one, and only to Telegram.
//
// Settings (store.c): tg_token, tg_chat | hook_url, hook_hdr ("Name: value"), hook_fmt (json | text) |
//                     mqtt_url (mqtt:// or mqtts://), mqtt_user, mqtt_pass
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "mqtt_client.h"
#include "rt.h"

#define PER_MINUTE 20
#define TEXT_LEN 200

static const char *TAG = "alert";

typedef struct {
    bool on, photo, test;
    float p;
    time_t t;
    char rule[RT_ID_LEN], text[TEXT_LEN], dest[10];
    rt_alert_result_t *out;
    SemaphoreHandle_t done;
} job_t;

typedef struct {
    uint32_t sent, failed;
    char last[64];
} tally_t;

static struct {
    char tg_token[64], tg_chat[24], hook_url[200], hook_hdr[160], hook_fmt[8], mqtt_url[128], mqtt_user[48], mqtt_pass[64];
} s_cfg;
static QueueHandle_t s_q;
static volatile bool s_reload = true;
static tally_t s_tg, s_hook, s_mq;
static uint32_t s_dropped;
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_up;
static int64_t s_sent_us[PER_MINUTE];
static int s_sent_i;

static int esc(char *dst, size_t cap, const char *src)      // JSON string contents
{
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)src; *c && n + 7 < cap; c++) {
        if (*c == '"' || *c == '\\') {
            dst[n++] = '\\';
            dst[n++] = *c;
        } else if (*c == '\n') {
            dst[n++] = '\\';
            dst[n++] = 'n';
        } else if (*c >= 0x20) {
            dst[n++] = *c;
        }
    }
    dst[n] = 0;
    return (int)n;
}

static esp_err_t on_http(esp_http_client_event_t *e)
{
    rt_alert_result_t *r = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && r && e->data_len > 0) {         // keep the start of the answer
        const size_t have = strlen(r->detail);
        const size_t room = sizeof(r->detail) - 1 - have;
        const size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        memcpy(r->detail + have, e->data, n);
        r->detail[have + n] = 0;
    }
    return ESP_OK;
}

static esp_http_client_handle_t http_open(const char *url, esp_http_client_method_t method, rt_alert_result_t *r)
{
    const esp_http_client_config_t c = {
        .url = url, .method = method, .timeout_ms = 12000, .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024, .buffer_size_tx = 1024, .event_handler = on_http, .user_data = r,
    };
    return esp_http_client_init(&c);
}

static void http_done(esp_http_client_handle_t h, esp_err_t err, int64_t t0, rt_alert_result_t *r)
{
    // A server that answers 200 and hangs up without saying how long its answer is (HTTP/1.0 style) ends in an error
    // here although the message arrived; the status line decides, or the alert would be sent again three times.
    const int status = esp_http_client_get_status_code(h);
    r->status = status > 0 ? status : 0;
    r->ok = r->status >= 200 && r->status < 300;
    r->ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK && !r->ok) {
        snprintf(r->detail, sizeof(r->detail), "%s", esp_err_to_name(err));
    } else if (r->ok) {
        r->detail[0] = 0;
    }
    for (char *c = r->detail; *c; c++) {                 // the answer goes into a JSON string later
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) {
            *c = ' ';
        }
    }
    esp_http_client_cleanup(h);
}

static void http_post(const char *url, const char *ctype, const char *hdr, const char *body, rt_alert_result_t *r)
{
    const int64_t t0 = esp_timer_get_time();
    memset(r, 0, sizeof(*r));
    esp_http_client_handle_t h = http_open(url, body ? HTTP_METHOD_POST : HTTP_METHOD_GET, r);
    if (!h) {
        snprintf(r->detail, sizeof(r->detail), "bad address");
        return;
    }
    if (body) {
        esp_http_client_set_header(h, "Content-Type", ctype);
        esp_http_client_set_post_field(h, body, strlen(body));
    }
    char name[48];
    const char *colon = hdr ? strchr(hdr, ':') : NULL;
    if (colon && colon > hdr && (size_t)(colon - hdr) < sizeof(name)) {
        memcpy(name, hdr, colon - hdr);
        name[colon - hdr] = 0;
        const char *v = colon + 1;
        while (*v == ' ') {
            v++;
        }
        esp_http_client_set_header(h, name, v);
    }
    http_done(h, esp_http_client_perform(h), t0, r);
}

// the frame the model is looking at, as a JPEG the caller frees (NULL: no camera, or no memory)
uint8_t *rt_snapshot_jpg(size_t *len)
{
    const int colors = rt_cam_colors();
    const uint8_t *cur, *prev;
    if (!colors || !rt_cam_acquire(&cur, &prev, NULL)) {
        return NULL;
    }
    const int W = RT_CAM * 2;
    uint8_t *big = heap_caps_malloc((size_t)W * W * colors, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (big) {
        for (int y = 0; y < W; y++) {
            for (int x = 0; x < W; x++) {
                const uint8_t *s = cur + ((y / 2) * RT_CAM + x / 2) * colors;
                uint8_t *d = big + (y * W + x) * colors;
                if (colors == 3) {                       // the encoder reads blue first
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                } else {
                    d[0] = s[0];
                }
            }
        }
    }
    rt_cam_release();
    if (!big) {
        return NULL;
    }
    uint8_t *jpg = NULL;
    const bool ok = fmt2jpg(big, (size_t)W * W * colors, W, W, colors == 3 ? PIXFORMAT_RGB888 : PIXFORMAT_GRAYSCALE, 80,
                            &jpg, len);
    free(big);
    return ok ? jpg : NULL;
}

static void line(char *msg, size_t cap, const job_t *j)
{
    snprintf(msg, cap, "%s\n%s %s, %d%% - %s", j->text, j->rule, j->on ? "on" : "off", (int)(j->p * 100 + 0.5f),
             rt_wifi_host());
}

static void telegram(const job_t *j, rt_alert_result_t *r)
{
    static char url[160], body[520], msg[300], e[400];
    line(msg, sizeof(msg), j);
    uint8_t *jpg = NULL;
    size_t jl = 0;
    if (j->photo) {
        jpg = rt_snapshot_jpg(&jl);
    }
    if (!jpg) {
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", s_cfg.tg_token);
        esc(e, sizeof(e), msg);
        snprintf(body, sizeof(body), "{\"chat_id\":\"%s\",\"text\":\"%s\"}", s_cfg.tg_chat, e);
        http_post(url, "application/json", NULL, body, r);
        return;
    }
    // multipart: chat_id, caption, photo
    static const char *B = "decidioBoundary7c1f";
    static char head[640], tail[40];
    const int hl = snprintf(head, sizeof(head),
                            "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n"
                            "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n"
                            "--%s\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"frame.jpg\"\r\n"
                            "Content-Type: image/jpeg\r\n\r\n", B, s_cfg.tg_chat, B, msg, B);
    const int tl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", B);
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", s_cfg.tg_token);
    const int64_t t0 = esp_timer_get_time();
    memset(r, 0, sizeof(*r));
    esp_http_client_handle_t h = http_open(url, HTTP_METHOD_POST, r);
    if (!h) {
        free(jpg);
        return;
    }
    char ct[64];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", B);
    esp_http_client_set_header(h, "Content-Type", ct);
    esp_err_t err = esp_http_client_open(h, hl + (int)jl + tl);
    if (err == ESP_OK) {
        if (esp_http_client_write(h, head, hl) < 0 || esp_http_client_write(h, (const char *)jpg, (int)jl) < 0 ||
            esp_http_client_write(h, tail, tl) < 0 || esp_http_client_fetch_headers(h) < 0) {
            err = ESP_FAIL;
        } else {
            char buf[96];
            const int n = esp_http_client_read(h, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = 0;
                snprintf(r->detail, sizeof(r->detail), "%s", buf);
            }
        }
    }
    free(jpg);
    http_done(h, err, t0, r);
}

static void webhook(const job_t *j, rt_alert_result_t *r)
{
    static char body[1100], msg[300], e[400];
    line(msg, sizeof(msg), j);
    if (!strcmp(s_cfg.hook_fmt, "text")) {
        http_post(s_cfg.hook_url, "text/plain; charset=utf-8", s_cfg.hook_hdr, msg, r);
        return;
    }
    esc(e, sizeof(e), msg);
    snprintf(body, sizeof(body),
             "{\"text\":\"%s\",\"content\":\"%s\",\"device\":\"%s\",\"event\":\"%s\",\"state\":\"%s\",\"p\":%.3f,\"time\":%lld}",
             e, e, rt_wifi_host(), j->rule, j->on ? "on" : "off", j->p, (long long)j->t);
    http_post(s_cfg.hook_url, "application/json", s_cfg.hook_hdr, body, r);
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == MQTT_EVENT_CONNECTED) {
        char topic[64];
        snprintf(topic, sizeof(topic), "decidio/%s/status", rt_wifi_host());
        esp_mqtt_client_publish(s_mqtt, topic, "online", 0, 1, 1);
        s_mqtt_up = true;
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_up = false;
    }
}

static void mqtt_setup(void)
{
    static char will[64];
    if (s_mqtt) {
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
        s_mqtt_up = false;
    }
    if (!s_cfg.mqtt_url[0]) {
        return;
    }
    snprintf(will, sizeof(will), "decidio/%s/status", rt_wifi_host());
    const esp_mqtt_client_config_t c = {
        .broker = { .address.uri = s_cfg.mqtt_url, .verification.crt_bundle_attach = esp_crt_bundle_attach },
        .credentials = { .username = s_cfg.mqtt_user[0] ? s_cfg.mqtt_user : NULL,
                         .authentication.password = s_cfg.mqtt_pass[0] ? s_cfg.mqtt_pass : NULL },
        .session = { .last_will = { .topic = will, .msg = "offline", .qos = 1, .retain = 1 } },
        .network = { .reconnect_timeout_ms = 10000, .timeout_ms = 8000 },
        .task = { .stack_size = 5120 },
        .buffer = { .size = 1024 },
    };
    s_mqtt = esp_mqtt_client_init(&c);
    if (s_mqtt) {
        esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, on_mqtt, NULL);
        esp_mqtt_client_start(s_mqtt);
    }
}

static void mqtt_send(const job_t *j, rt_alert_result_t *r)
{
    static char topic[96], body[420], e[300];
    const int64_t t0 = esp_timer_get_time();
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < 50 && !s_mqtt_up && j->test; i++) {        // a test waits up to 5 s for the first connection
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_mqtt || !s_mqtt_up) {
        snprintf(r->detail, sizeof(r->detail), "not connected to the broker");
    } else {
        esc(e, sizeof(e), j->text);
        snprintf(topic, sizeof(topic), "decidio/%s/event/%s", rt_wifi_host(), j->rule);
        snprintf(body, sizeof(body), "{\"state\":\"%s\",\"p\":%.3f,\"text\":\"%s\",\"time\":%lld}", j->on ? "on" : "off",
                 j->p, e, (long long)j->t);
        r->ok = esp_mqtt_client_publish(s_mqtt, topic, body, 0, 1, 0) >= 0;
        if (!r->ok) {
            snprintf(r->detail, sizeof(r->detail), "publish failed");
        }
    }
    r->ms = (int)((esp_timer_get_time() - t0) / 1000);
}

static void load(void)
{
    rt_store_get("tg_token", s_cfg.tg_token, sizeof(s_cfg.tg_token));
    rt_store_get("tg_chat", s_cfg.tg_chat, sizeof(s_cfg.tg_chat));
    rt_store_get("hook_url", s_cfg.hook_url, sizeof(s_cfg.hook_url));
    rt_store_get("hook_hdr", s_cfg.hook_hdr, sizeof(s_cfg.hook_hdr));
    rt_store_get("hook_fmt", s_cfg.hook_fmt, sizeof(s_cfg.hook_fmt));
    char old[sizeof(s_cfg.mqtt_url) + sizeof(s_cfg.mqtt_user) + sizeof(s_cfg.mqtt_pass)], now[sizeof(old)];
    snprintf(old, sizeof(old), "%s|%s|%s", s_cfg.mqtt_url, s_cfg.mqtt_user, s_cfg.mqtt_pass);
    rt_store_get("mqtt_url", s_cfg.mqtt_url, sizeof(s_cfg.mqtt_url));
    rt_store_get("mqtt_user", s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user));
    rt_store_get("mqtt_pass", s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass));
    snprintf(now, sizeof(now), "%s|%s|%s", s_cfg.mqtt_url, s_cfg.mqtt_user, s_cfg.mqtt_pass);
    if (strcmp(old, now) || (s_cfg.mqtt_url[0] && !s_mqtt)) {
        mqtt_setup();
    }
}

static void note(tally_t *t, const rt_alert_result_t *r)
{
    if (r->ok) {
        t->sent++;
        t->last[0] = 0;
    } else {
        t->failed++;
        if (r->status) {
            snprintf(t->last, sizeof(t->last), "%d %.56s", r->status, r->detail);
        } else {
            snprintf(t->last, sizeof(t->last), "%.63s", r->detail);
        }
    }
}

// one destination, with up to three tries for a real event when the network or the server is the problem
static void deliver(const job_t *j, void (*fn)(const job_t *, rt_alert_result_t *), tally_t *t, rt_alert_result_t *r)
{
    for (int attempt = 0; attempt < (j->test ? 1 : 3); attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(attempt == 1 ? 2000 : 5000));
        }
        fn(j, r);
        if (r->ok || (r->status >= 400 && r->status < 500 && r->status != 429)) {
            break;
        }
    }
    note(t, r);
    if (!r->ok) {
        ESP_LOGW(TAG, "%s: %d %s", j->rule, r->status, r->detail);
    }
}

static bool budget(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_sent_us[s_sent_i] && now - s_sent_us[s_sent_i] < 60 * 1000000LL) {
        return false;
    }
    s_sent_us[s_sent_i] = now;
    s_sent_i = (s_sent_i + 1) % PER_MINUTE;
    return true;
}

static void alert_task(void *arg)
{
    static job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
            if (s_reload && rt_wifi_online()) {
                s_reload = false;
                load();
            }
            continue;
        }
        if (s_reload) {
            s_reload = false;
            load();
        }
        rt_alert_result_t r = { 0 };
        if (!rt_wifi_online()) {
            snprintf(r.detail, sizeof(r.detail), "not connected to Wi-Fi");
            if (!j.test) {
                s_dropped++;
            }
        } else if (j.test && !strcmp(j.dest, "probe")) {          // can this board reach an HTTPS server at all?
            http_post("https://api.telegram.org/", NULL, NULL, NULL, &r);
            r.ok = r.status > 0;
        } else if (j.test) {
            if (!strcmp(j.dest, "telegram")) {
                if (s_cfg.tg_token[0] && s_cfg.tg_chat[0]) {
                    deliver(&j, telegram, &s_tg, &r);
                } else {
                    snprintf(r.detail, sizeof(r.detail), "token or chat id not set");
                }
            } else if (!strcmp(j.dest, "webhook")) {
                if (s_cfg.hook_url[0]) {
                    deliver(&j, webhook, &s_hook, &r);
                } else {
                    snprintf(r.detail, sizeof(r.detail), "address not set");
                }
            } else if (!strcmp(j.dest, "mqtt")) {
                if (s_cfg.mqtt_url[0]) {
                    deliver(&j, mqtt_send, &s_mq, &r);
                } else {
                    snprintf(r.detail, sizeof(r.detail), "broker not set");
                }
            }
        } else if (!budget()) {
            s_dropped++;
        } else {
            if (s_cfg.tg_token[0] && s_cfg.tg_chat[0]) {
                deliver(&j, telegram, &s_tg, &r);
            }
            if (s_cfg.hook_url[0]) {
                deliver(&j, webhook, &s_hook, &r);
            }
            if (s_cfg.mqtt_url[0]) {
                deliver(&j, mqtt_send, &s_mq, &r);
            }
        }
        if (j.out) {
            *j.out = r;
        }
        if (j.done) {
            xSemaphoreGive(j.done);
        }
    }
}

void rt_alert_start(void)
{
    if (s_q) {
        return;
    }
    s_q = xQueueCreate(8, sizeof(job_t));
    if (!s_q || xTaskCreatePinnedToCore(alert_task, "alert", 7168, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the alert task");
        s_q = NULL;
    }
}

void rt_alert_reload(void)
{
    s_reload = true;
}

// from the decision task: never waits
void rt_alert_event(const char *rule, const char *text, bool on, float p, bool photo)
{
    if (!s_q) {
        return;
    }
    static job_t j;                                       // one caller
    memset(&j, 0, sizeof(j));
    j.on = on;
    j.p = p;
    j.photo = photo;
    j.t = time(NULL) > 1600000000 ? time(NULL) : 0;
    strlcpy(j.rule, rule, sizeof(j.rule));
    strlcpy(j.text, text && text[0] ? text : rule, sizeof(j.text));
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        s_dropped++;
    }
}

// from the web page or the console: waits for the answer (up to 30 s)
bool rt_alert_test(const char *dest, rt_alert_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_q) {
        snprintf(out->detail, sizeof(out->detail), "alerts are not running");
        return false;
    }
    job_t *j = calloc(1, sizeof(job_t));
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    static rt_alert_result_t res;                         // outlives a timed-out wait
    if (!j || !done) {
        free(j);
        return false;
    }
    j->test = j->on = true;
    j->p = 1;
    j->t = time(NULL) > 1600000000 ? time(NULL) : 0;
    j->out = &res;
    j->done = done;
    strlcpy(j->dest, dest, sizeof(j->dest));
    strlcpy(j->rule, "test", sizeof(j->rule));
    strlcpy(j->text, "Test alert from your decidio board", sizeof(j->text));
    const bool queued = xQueueSend(s_q, j, pdMS_TO_TICKS(500)) == pdTRUE;
    free(j);
    if (queued && xSemaphoreTake(done, pdMS_TO_TICKS(30000)) == pdTRUE) {
        *out = res;
        vSemaphoreDelete(done);
    } else {
        snprintf(out->detail, sizeof(out->detail), queued ? "no answer in 30 s" : "busy");   // the semaphore is left for the task
    }
    return out->ok;
}

int rt_alert_json(char *dst, size_t cap)
{
    return snprintf(dst, cap,
                    "{\"telegram\":{\"set\":%s,\"chat\":\"%s\",\"sent\":%lu,\"failed\":%lu,\"last\":\"%s\"},"
                    "\"webhook\":{\"set\":%s,\"format\":\"%s\",\"sent\":%lu,\"failed\":%lu,\"last\":\"%s\"},"
                    "\"mqtt\":{\"set\":%s,\"connected\":%s,\"sent\":%lu,\"failed\":%lu,\"last\":\"%s\"},\"dropped\":%lu}",
                    s_cfg.tg_token[0] && s_cfg.tg_chat[0] ? "true" : "false", s_cfg.tg_chat, (unsigned long)s_tg.sent,
                    (unsigned long)s_tg.failed, s_tg.last, s_cfg.hook_url[0] ? "true" : "false",
                    s_cfg.hook_fmt[0] ? s_cfg.hook_fmt : "json", (unsigned long)s_hook.sent, (unsigned long)s_hook.failed,
                    s_hook.last, s_cfg.mqtt_url[0] ? "true" : "false", s_mqtt_up ? "true" : "false",
                    (unsigned long)s_mq.sent, (unsigned long)s_mq.failed, s_mq.last, (unsigned long)s_dropped);
}
