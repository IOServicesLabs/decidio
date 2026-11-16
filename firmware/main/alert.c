// Alerts: when a rule fires, a short message goes to Telegram, to a webhook and/or to an MQTT broker.
//
// Everything is sent from this file's own task, fed by a queue, so a slow or dead network never delays a decision.
// At most 20 messages a minute leave the board; what does not fit is dropped and counted. A message is text. A photo
// (the 96x96 frame the model saw, doubled to 192x192) is attached only for rules that ask for one, and only to Telegram.
//
// In a group of boards (home.c), one board sends the alerts of all of them. The other boards give it their alerts, and
// it gets the photo of such an alert from that board's page (/api/frame.jpg, with the group's token).
//
// Grouping: an alert goes out at once, then for "alert_window" seconds (60; 0 = never group) the alerts that follow
// are kept, and at the end of that time they go out together as one message ("the same rule on 2 boards, 3 times").
// After that message the next window starts, so that a storm of alerts makes one message a window. An urgent rule and
// the daily summary are never kept back.
//
// Settings (store.c): tg_token, tg_chat | hook_url, hook_hdr ("Name: value"), hook_fmt (json | text) |
//                     mqtt_url (mqtt:// or mqtts://), mqtt_user, mqtt_pass | alert_window (seconds)
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
#include "cJSON.h"
#include "rt.h"

#define PER_MINUTE 20
#define TEXT_LEN 200
#define MAX_FOLD 10             // different alerts kept in one window; more are only counted
#define BODY_LEN 520            // the longest message text (a grouped message)

static const char *TAG = "alert";

typedef struct {
    bool on, photo, test, urgent;
    bool remote;                // from another board of the group (host, label, ip are that board's)
    bool grouped;               // the grouped message: its text is in s_body
    float p;
    time_t t;
    char rule[RT_ID_LEN], text[TEXT_LEN], dest[10];
    char host[20], label[24], ip[16];
    rt_alert_result_t *out;
    SemaphoreHandle_t done;
} job_t;

typedef struct {                // alerts kept back in this window: one line for each rule and change
    char rule[RT_ID_LEN], text[72], where[64];
    bool on;
    uint16_t times;
} fold_t;

typedef struct {
    uint32_t sent, failed;
    char last[64];
} tally_t;

static struct {
    char tg_token[64], tg_chat[24], hook_url[200], hook_hdr[160], hook_fmt[8], mqtt_url[128], mqtt_user[48], mqtt_pass[64];
} s_cfg;
static QueueHandle_t s_q;
static volatile bool s_reload = true;
static volatile bool s_busy;    // the task has a job in hand (rt_alert_idle)
static tally_t s_tg, s_hook, s_mq;
static uint32_t s_dropped, s_handed, s_folded;
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_up;
static int64_t s_sent_us[PER_MINUTE];
static int s_sent_i;
static int s_window_s = 60;
static int64_t s_win_end;       // 0: no window open
static fold_t *s_fold;          // MAX_FOLD, in external memory
static int s_fold_n;
static uint32_t s_fold_more;
static char *s_body, *s_msg, *s_esc, *s_post;   // message buffers of the alert task, in external memory

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

#define MSG_LEN (BODY_LEN + 100)
#define ESC_LEN (2 * MSG_LEN)
#define POST_LEN (2 * ESC_LEN + 400)

// the photo of an alert from another board: its frame now, from its page (the group's token opens it)
static uint8_t *peer_jpg(const char *ip, size_t *len)
{
    char url[48];
    snprintf(url, sizeof(url), "http://%s/api/frame.jpg", ip);
    const esp_http_client_config_t c = { .url = url, .timeout_ms = 4000, .buffer_size = 1024 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        return NULL;
    }
    esp_http_client_set_header(h, "X-Decidio-Group", rt_home_token());
    uint8_t *jpg = NULL;
    *len = 0;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        const int64_t cl = esp_http_client_fetch_headers(h);
        if (esp_http_client_get_status_code(h) == 200 && cl > 0 && cl < 65536) {
            jpg = heap_caps_malloc((size_t)cl, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int got = 0, n;
            while (jpg && got < cl && (n = esp_http_client_read(h, (char *)jpg + got, (int)cl - got)) > 0) {
                got += n;
            }
            if (jpg && got == cl) {
                *len = (size_t)cl;
            } else {
                free(jpg);
                jpg = NULL;
            }
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return jpg;
}

static void line(char *msg, size_t cap, const job_t *j)
{
    if (j->grouped) {
        snprintf(msg, cap, "%s", s_body);
        return;
    }
    if (j->p >= 0 && j->p <= 1) {
        snprintf(msg, cap, "%s\n%s %s, %d%% - %s", j->text, j->rule, j->on ? "on" : "off", (int)(j->p * 100 + 0.5f),
                 j->label);
    } else {                    // a rule on an input: its value, not a share of 1
        snprintf(msg, cap, "%s\n%s %s, %.4g - %s", j->text, j->rule, j->on ? "on" : "off", j->p, j->label);
    }
}

static void telegram(const job_t *j, rt_alert_result_t *r)
{
    static char url[160];
    char *body = s_post, *msg = s_msg, *e = s_esc;
    line(msg, MSG_LEN, j);
    uint8_t *jpg = NULL;
    size_t jl = 0;
    if (j->photo) {
        jpg = j->remote ? peer_jpg(j->ip, &jl) : rt_snapshot_jpg(&jl);
    }
    if (!jpg) {
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", s_cfg.tg_token);
        esc(e, ESC_LEN, msg);
        snprintf(body, POST_LEN, "{\"chat_id\":\"%s\",\"text\":\"%s\"}", s_cfg.tg_chat, e);
        http_post(url, "application/json", NULL, body, r);
        return;
    }
    // multipart: chat_id, caption, photo
    static const char *B = "decidioBoundary7c1f";
    static char tail[40];
    char *head = s_post;
    const int hl = snprintf(head, POST_LEN,
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
    char *body = s_post, *msg = s_msg, *e = s_esc;
    line(msg, MSG_LEN, j);
    if (!strcmp(s_cfg.hook_fmt, "text")) {
        http_post(s_cfg.hook_url, "text/plain; charset=utf-8", s_cfg.hook_hdr, msg, r);
        return;
    }
    esc(e, ESC_LEN, msg);
    snprintf(body, POST_LEN,
             "{\"text\":\"%s\",\"content\":\"%s\",\"device\":\"%s\",\"room\":\"%s\",\"event\":\"%s\",\"state\":\"%s\","
             "\"p\":%.3f,\"time\":%lld}",
             e, e, j->host, j->label, j->rule, j->on ? "on" : "off", j->p, (long long)j->t);
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
    static char topic[96];
    char *body = s_post, *e = s_esc;
    const int64_t t0 = esp_timer_get_time();
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < 50 && !s_mqtt_up && j->test; i++) {        // a test waits up to 5 s for the first connection
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_mqtt || !s_mqtt_up) {
        snprintf(r->detail, sizeof(r->detail), "not connected to the broker");
    } else {
        esc(e, ESC_LEN, j->grouped ? s_body : j->text);
        snprintf(topic, sizeof(topic), "decidio/%s/event/%s", j->host, j->rule);   // the board that saw it
        snprintf(body, POST_LEN, "{\"state\":\"%s\",\"p\":%.3f,\"text\":\"%s\",\"room\":\"%s\",\"time\":%lld}",
                 j->on ? "on" : "off", j->p, e, j->label, (long long)j->t);
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
    char w[8];
    const int ws = rt_store_get("alert_window", w, sizeof(w)) && w[0] ? atoi(w) : 60;
    s_window_s = ws < 0 ? 0 : ws > 3600 ? 3600 : ws;
    snprintf(now, sizeof(now), "%s|%s|%s", s_cfg.mqtt_url, s_cfg.mqtt_user, s_cfg.mqtt_pass);
    if (rt_wifi_online() && (strcmp(old, now) || (s_cfg.mqtt_url[0] && !s_mqtt))) {
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

static void send_all(const job_t *j, rt_alert_result_t *r)
{
    if (!budget()) {
        s_dropped++;
        return;
    }
    if (s_cfg.tg_token[0] && s_cfg.tg_chat[0]) {
        deliver(j, telegram, &s_tg, r);
    }
    if (s_cfg.hook_url[0]) {
        deliver(j, webhook, &s_hook, r);
    }
    if (s_cfg.mqtt_url[0]) {
        deliver(j, mqtt_send, &s_mq, r);
    }
}

static void add_where(char *where, size_t cap, const char *label)
{
    const size_t n = strlen(label);
    for (const char *s = where; (s = strstr(s, label)); s += n) {           // already in the list
        if ((s == where || (s >= where + 2 && s[-2] == ',')) && (s[n] == 0 || s[n] == ',')) {
            return;
        }
    }
    if (strlen(where) + n + 3 < cap) {
        if (where[0]) {
            strcat(where, ", ");
        }
        strcat(where, label);
    }
}

// keep an alert for the grouped message
static void fold(const job_t *j)
{
    s_folded++;
    for (int i = 0; i < s_fold_n; i++) {
        fold_t *f = &s_fold[i];
        if (f->on == j->on && !strcmp(f->rule, j->rule)) {
            f->times++;
            add_where(f->where, sizeof(f->where), j->label);
            return;
        }
    }
    if (s_fold_n >= MAX_FOLD) {
        s_fold_more++;
        return;
    }
    fold_t *f = &s_fold[s_fold_n++];
    memset(f, 0, sizeof(*f));
    strlcpy(f->rule, j->rule, sizeof(f->rule));
    strlcpy(f->text, j->text, sizeof(f->text));
    f->on = j->on;
    f->times = 1;
    add_where(f->where, sizeof(f->where), j->label);
}

// the end of a window: the alerts kept in it go out as one message, and the next window starts; else no window
static void flush(int64_t now)
{
    if (!s_win_end || now < s_win_end) {
        return;
    }
    if (!s_fold_n && !s_fold_more) {
        s_win_end = 0;
        return;
    }
    static job_t g;
    memset(&g, 0, sizeof(g));
    g.grouped = g.on = true;
    g.p = 1;
    g.t = time(NULL) > 1600000000 ? time(NULL) : 0;
    strlcpy(g.rule, "grouped", sizeof(g.rule));
    strlcpy(g.host, rt_wifi_host(), sizeof(g.host));
    strlcpy(g.label, rt_home_label(), sizeof(g.label));
    int n = snprintf(s_body, BODY_LEN, "More alerts in the last %d s:", s_window_s);
    for (int i = 0; i < s_fold_n && n < BODY_LEN; i++) {
        const fold_t *f = &s_fold[i];
        n += snprintf(s_body + n, BODY_LEN - n, "\n- %s%s (%s)", f->text[0] ? f->text : f->rule,
                      f->text[0] || f->on ? "" : " ended", f->where);
        if (f->times > 1 && n < BODY_LEN) {
            n += snprintf(s_body + n, BODY_LEN - n, ", %u times", f->times);
        }
    }
    if (s_fold_more && n < BODY_LEN) {
        snprintf(s_body + n, BODY_LEN - n, "\n- and %lu more", (unsigned long)s_fold_more);
    }
    s_fold_n = 0;
    s_fold_more = 0;
    s_win_end = now + (int64_t)s_window_s * 1000000;
    if (rt_wifi_online()) {
        rt_alert_result_t r = { 0 };
        send_all(&g, &r);
    }
}

// a rule of this board fired: is another board of the group the one that sends the alerts? Then give it this one.
static bool handoff(const job_t *j)
{
    char ip[16], host[20];
    if (rt_home_sender(ip, sizeof(ip), host, sizeof(host))) {
        rt_power_note_sender(ip, host);
    } else if (!rt_power_sender(ip, sizeof(ip), host, sizeof(host))) {   // low power: the sender of an earlier wake
        return false;
    }
    cJSON *a = cJSON_CreateObject();
    if (!a) {
        return false;
    }
    cJSON_AddStringToObject(a, "r", j->rule);
    cJSON_AddStringToObject(a, "t", j->text);
    cJSON_AddNumberToObject(a, "on", j->on);
    cJSON_AddNumberToObject(a, "p", (int)(j->p * 1000 + 0.5f) / 1000.0);
    cJSON_AddNumberToObject(a, "ph", j->photo);
    cJSON_AddNumberToObject(a, "u", j->urgent);
    cJSON_AddNumberToObject(a, "tm", (double)j->t);
    const bool ok = rt_home_handoff(a, ip);
    cJSON_Delete(a);
    if (ok) {
        s_handed++;
    } else {
        rt_power_note_sender("", "");
        ESP_LOGW(TAG, "%s: %s did not answer; sending it from this board", j->rule, host);
    }
    return ok;
}

static void alert_task(void *arg)
{
    static job_t j;
    for (;;) {
        const bool got = xQueueReceive(s_q, &j, pdMS_TO_TICKS(1000)) == pdTRUE;
        s_busy = got;
        if (got && !j.test && !rt_wifi_online() && rt_power_on() && rt_alert_ready()) {
            rt_power_need_net();                           // low power: Wi-Fi joins for this alert
            for (int i = 0; i < 200 && !rt_wifi_online(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (rt_wifi_online()) {
                vTaskDelay(pdMS_TO_TICKS(300));            // the address and the clock settle
            }
        }
        if (s_reload && (got || rt_wifi_online())) {
            s_reload = false;
            load();
        }
        flush(esp_timer_get_time());
        if (!got) {
            continue;
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
        } else if (!j.remote && !(j.photo && rt_power_on()) && handoff(&j)) {
            // (low power: a board that sleeps cannot give the sender its photo, so it sends photo alerts itself)
            // the group's sender has it
        } else if (!j.urgent && s_window_s > 0 && s_win_end && !rt_power_on()) {
            fold(&j);                                      // goes out with the others at the end of the window
        } else {
            if (!j.urgent && s_window_s > 0 && !rt_power_on()) {   // low power: the board is not awake for a window
                s_win_end = esp_timer_get_time() + (int64_t)s_window_s * 1000000;
            }
            send_all(&j, &r);
        }
        if (j.out) {
            *j.out = r;
        }
        if (j.done) {
            xSemaphoreGive(j.done);
        }
        s_busy = false;
    }
}

void rt_alert_start(void)
{
    if (s_q) {
        return;
    }
    const size_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_fold = heap_caps_calloc_prefer(MAX_FOLD, sizeof(fold_t), 2, caps, MALLOC_CAP_8BIT);
    s_body = heap_caps_malloc_prefer(BODY_LEN + MSG_LEN + ESC_LEN + POST_LEN, 2, caps, MALLOC_CAP_8BIT);
    if (!s_fold || !s_body) {
        ESP_LOGE(TAG, "no memory for the alert buffers");
        return;
    }
    s_msg = s_body + BODY_LEN;
    s_esc = s_msg + MSG_LEN;
    s_post = s_esc + ESC_LEN;
    load();                                               // the destinations are known before Wi-Fi joins (low power)
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
void rt_alert_event(const char *rule, const char *text, bool on, float p, bool photo, bool urgent)
{
    if (!s_q) {
        return;
    }
    static job_t j;                                       // one caller
    memset(&j, 0, sizeof(j));
    j.on = on;
    j.p = p;
    j.photo = photo;
    j.urgent = urgent;
    j.t = time(NULL) > 1600000000 ? time(NULL) : 0;
    strlcpy(j.rule, rule, sizeof(j.rule));
    strlcpy(j.text, text && text[0] ? text : rule, sizeof(j.text));
    strlcpy(j.host, rt_wifi_host(), sizeof(j.host));
    strlcpy(j.label, rt_home_label(), sizeof(j.label));
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        s_dropped++;
    } else if (rt_power_on() && rt_alert_ready()) {
        rt_power_need_net();
    }
}

bool rt_alert_idle(void)
{
    return !s_q || (!s_busy && uxQueueMessagesWaiting(s_q) == 0 && s_fold_n == 0 && s_fold_more == 0);
}

bool rt_alert_ready(void)
{
    return (s_cfg.tg_token[0] && s_cfg.tg_chat[0]) || s_cfg.hook_url[0] || s_cfg.mqtt_url[0];
}

// from the home task: never waits
bool rt_alert_from_peer(const char *host, const char *label, const char *ip, const char *rule, const char *text, bool on,
                        float p, bool photo, bool urgent, long long t)
{
    if (!s_q || !rt_alert_ready()) {
        return false;
    }
    static job_t j;                                       // one caller
    memset(&j, 0, sizeof(j));
    j.remote = true;
    j.on = on;
    j.p = p;
    j.photo = photo;
    j.urgent = urgent;
    j.t = t > 1600000000 ? (time_t)t : 0;
    strlcpy(j.rule, rule, sizeof(j.rule));
    strlcpy(j.text, text[0] ? text : rule, sizeof(j.text));
    strlcpy(j.host, host, sizeof(j.host));
    strlcpy(j.label, label, sizeof(j.label));
    strlcpy(j.ip, ip, sizeof(j.ip));
    return xQueueSend(s_q, &j, 0) == pdTRUE;
}

// from the web page: write this board's alert settings on each other board of the group. [{"host","ok"},...]
int rt_alert_share(char *dst, size_t cap)
{
    static const char *KEYS[] = { "tg_token", "tg_chat", "hook_url", "hook_hdr", "hook_fmt", "mqtt_url", "mqtt_user",
                                  "mqtt_pass", "alert_window" };
    char ips[8][16], hosts[8][20];
    const int m = rt_home_members(ips, hosts, 8);
    cJSON *b = cJSON_CreateObject();
    char *v = malloc(200);
    for (size_t i = 0; b && v && i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (!rt_store_get(KEYS[i], v, 200)) {
            v[0] = 0;
        }
        if (!strcmp(KEYS[i], "alert_window") && !v[0]) {
            continue;                                      // not set here: leave theirs
        }
        cJSON_AddStringToObject(b, KEYS[i], v);
    }
    free(v);
    char *body = b ? cJSON_PrintUnformatted(b) : NULL;
    cJSON_Delete(b);
    int n = snprintf(dst, cap, "[");
    for (int k = 0; body && k < m; k++) {
        char url[48];
        snprintf(url, sizeof(url), "http://%s/api/alerts", ips[k]);
        const esp_http_client_config_t c = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 6000, .buffer_size = 512 };
        esp_http_client_handle_t h = esp_http_client_init(&c);
        int status = 0;
        if (h) {
            esp_http_client_set_header(h, "X-Decidio-Group", rt_home_token());
            esp_http_client_set_header(h, "Content-Type", "application/json");
            esp_http_client_set_post_field(h, body, strlen(body));
            if (esp_http_client_perform(h) == ESP_OK) {
                status = esp_http_client_get_status_code(h);
            }
            esp_http_client_cleanup(h);
        }
        n += snprintf(dst + n, n < (int)cap ? cap - n : 0, "%s{\"host\":\"%s\",\"ok\":%s,\"status\":%d}", k ? "," : "", hosts[k],
                      status == 200 ? "true" : "false", status);
    }
    free(body);
    n += snprintf(dst + n, n < (int)cap ? cap - n : 0, "]");
    return n < (int)cap ? n : -1;
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
    strlcpy(j->host, rt_wifi_host(), sizeof(j->host));
    strlcpy(j->label, rt_home_label(), sizeof(j->label));
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
                    "\"mqtt\":{\"set\":%s,\"connected\":%s,\"sent\":%lu,\"failed\":%lu,\"last\":\"%s\"},\"dropped\":%lu,"
                    "\"window\":%d,\"handed\":%lu,\"grouped\":%lu}",
                    s_cfg.tg_token[0] && s_cfg.tg_chat[0] ? "true" : "false", s_cfg.tg_chat, (unsigned long)s_tg.sent,
                    (unsigned long)s_tg.failed, s_tg.last, s_cfg.hook_url[0] ? "true" : "false",
                    s_cfg.hook_fmt[0] ? s_cfg.hook_fmt : "json", (unsigned long)s_hook.sent, (unsigned long)s_hook.failed,
                    s_hook.last, s_cfg.mqtt_url[0] ? "true" : "false", s_mqtt_up ? "true" : "false",
                    (unsigned long)s_mq.sent, (unsigned long)s_mq.failed, s_mq.last, (unsigned long)s_dropped, s_window_s,
                    (unsigned long)s_handed, (unsigned long)s_folded);
}
