// The board's own page (http://decidio-XXXX.local, or http://192.168.4.1 on the setup access point) and its JSON API.
// Plain HTTP on your own network. If a page password is set, every request needs it (user name: decidio).
// A board in a group also takes the group password (the group secret) with the same user name, and it always asks for
// a password: one password opens the page of every board of the group.
//
//   GET  /               the page (www/index.html)
//   GET  /api/info       board, model, Wi-Fi, alert counters, memory
//   GET  /api/scan       networks in range (setup)
//   POST /api/wifi       {"ssid","pass"} | {"forget":true}
//   POST /api/camera     {"rotate":0|90|180|270}: turn the camera picture
//   POST /api/alerts     any of tg_token tg_chat hook_url hook_hdr hook_fmt mqtt_url mqtt_user mqtt_pass alert_window
//   POST /api/alerts/share  write this board's alert settings on the other boards of the group -> {"boards":[...]}
//   POST /api/test       {"dest":"telegram|webhook|mqtt|probe"} -> {"ok","status","ms","detail"}
//   GET  /api/state      the last decision, the rules and whether each is on, the radio score
//   POST /api/rules      a JSON array of rules (rules.c) | {"reset":true}: back to the model package's own
//   POST /api/radio      {"motion":true|false}
//   POST /api/time       {"tz":"<+01>-1"} (POSIX time zone, sent by the page from the browser's clock), {"summary_at":1260}
//   GET  /api/frame.jpg  what the camera sees now (to aim it)
//   GET  /api/recipes    the ideas built into this firmware (recipes.json), each with its rules
//   POST /api/model      a model package (model.dcd) as the request body: written to flash, then the board restarts
//   POST /api/firmware   an app image (decidio_rt.bin) as the request body: written to the other app slot, then restart
//   POST /api/reset      forget every setting (Wi-Fi, alerts, edited rules, password) and restart
//   POST /api/password   {"pass"}  ("" removes it)
//   POST /api/reboot
//   GET  /api/devices    Smart Life / Tuya devices on this board's list, and the ones heard on the network
//   POST /api/devices    {"name","id","key","ip","ver":"3.5","dp":1,"slot"} | {"slot":n,"delete":true}
//   POST /api/devices/test  {"slot":n,"do":"on|off|status"} -> {"ok","ms","detail","answer"}
//   POST /api/devices/try   {"id","ip","ver","key","dp","do":"on|off|status|blink"}: a heard device, not saved
//   GET  /api/home       this board's room, and the other boards heard (their rooms and which rules are on)
//   POST /api/home       {"room":"Kitchen"} | {"group":"secret"} | {"start":true} (a group with no password)
//   POST /api/home/add   {"host","pass"}: give another board this board's group secret (starts a group if necessary)
//   POST /api/home/push  send the firmware this board runs to the other boards of the group (push.c)
//   GET  /api/power      low-power mode: its settings, what the board does now, and the counts for the battery estimate
//   POST /api/power      {"on","wakes","from","to","look","checkin","pir","gap","usb_sleep","mah","i_sleep","i_awake",
//                        "i_wifi","bat_pin","bat_ratio"} (any of them) | {"stay":true|false} | {"reset_counts":true}
//   ANY  /api/via/<board>/api/...   the same request passed on to another board of the group
// A request from another board of the group carries the group's token (X-Decidio-Group) instead of the password.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "rt.h"

static const char *TAG = "web";
static httpd_handle_t s_srv;
static char s_auth[120];                 // "Basic <base64 of decidio:password>", empty = no password
static char s_gauth[120];                // the same for the group password, empty = not in a group

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char recipes_json_start[] asm("_binary_recipes_json_start");
extern const char recipes_json_end[] asm("_binary_recipes_json_end");

static void basic(char *dst, size_t cap, const char *pass)   // "" for an empty password
{
    char plain[80];
    dst[0] = 0;
    if (pass && pass[0]) {
        const int n = snprintf(plain, sizeof(plain), "decidio:%s", pass);
        size_t o = 0;
        strcpy(dst, "Basic ");
        mbedtls_base64_encode((unsigned char *)dst + 6, cap - 7, &o, (const unsigned char *)plain, n);
        dst[6 + o] = 0;
    }
}

static void load_auth(void)
{
    char pass[64] = "";
    rt_store_get("page_pass", pass, sizeof(pass));
    basic(s_auth, sizeof(s_auth), pass);
    basic(s_gauth, sizeof(s_gauth), rt_home_open() ? "" : rt_home_secret());   // a group can be without a password
}

void rt_web_auth_reload(void)
{
    load_auth();
}

// did another board of the group send this request (it carries the group's token)
static bool from_peer(httpd_req_t *req)
{
    char got[40];
    return httpd_req_get_hdr_value_str(req, "X-Decidio-Group", got, sizeof(got)) == ESP_OK && rt_home_token_ok(got);
}

static bool allowed(httpd_req_t *req)
{
    char got[120];
    const bool peer = from_peer(req);
    if (!peer && (req->method != HTTP_GET || !strcmp(req->uri, "/"))) {
        rt_power_touch();                                // low power: a person opened the page or changed something
    }
    if (!s_auth[0] && !s_gauth[0]) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", got, sizeof(got)) == ESP_OK &&
        ((s_auth[0] && !strcmp(got, s_auth)) || (s_gauth[0] && !strcmp(got, s_gauth)))) {
        return true;
    }
    if (peer) {
        return true;                                     // another board of the group
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"decidio\"");
    httpd_resp_sendstr(req, "password needed (user name: decidio)");
    return false;
}

static esp_err_t json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s);
}

// the request body as text the caller frees (NULL: none, too long, or cut off)
static char *text(httpd_req_t *req, size_t max)
{
    const size_t n = req->content_len;
    if (n == 0 || n > max) {
        return NULL;
    }
    char *b = heap_caps_malloc_prefer(n + 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return NULL;
    }
    size_t got = 0;
    while (got < n) {
        const int r = httpd_req_recv(req, b + got, n - got);
        if (r <= 0) {
            free(b);
            return NULL;
        }
        got += r;
    }
    b[n] = 0;
    return b;
}

static cJSON *body(httpd_req_t *req)
{
    const size_t n = req->content_len;
    if (n == 0 || n > 4096) {
        return NULL;
    }
    char *b = malloc(n + 1);
    if (!b) {
        return NULL;
    }
    size_t got = 0;
    while (got < n) {
        const int r = httpd_req_recv(req, b + got, n - got);
        if (r <= 0) {
            free(b);
            return NULL;
        }
        got += r;
    }
    b[n] = 0;
    cJSON *j = cJSON_Parse(b);
    free(b);
    return j;
}

static const char *str(const cJSON *j, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static esp_err_t bad(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "400 Bad Request");
    char b[120];
    snprintf(b, sizeof(b), "{\"error\":\"%s\"}", why);
    return json(req, b);
}

static esp_err_t h_index(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);    // embedded with a final 0
}

static esp_err_t h_info(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = heap_caps_malloc_prefer(3072 + 3500, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    rt_wifi_status_t w;
    rt_wifi_status(&w);
    int n = rt_info_json(b, 3500);
    n--;                                                  // reopen the outer object
    const int cap = n + 3072;
    n += snprintf(b + n, cap - n, ",\"wifi\":{\"state\":\"%s\",\"setup_open\":%s,\"ssid\":\"", w.state,
                  w.setup_open ? "true" : "false");
    for (const char *c = w.ssid; *c; c++) {
        if (*c == '"' || *c == '\\') {
            b[n++] = '\\';
        }
        if ((unsigned char)*c >= 0x20) {
            b[n++] = *c;
        }
    }
    n += snprintf(b + n, cap - n, "\",\"ip\":\"%s\",\"host\":\"%s\",\"rssi\":%d,\"channel\":%d,\"reason\":%d},\"alerts\":",
                  w.ip, w.host, w.rssi, w.channel, w.reason);
    n += rt_alert_json(b + n, cap - n);
    char tz[40];
    rt_store_get("tz", tz, sizeof(tz));
    char at[8];
    snprintf(b + n, cap - n, ",\"password\":%s,\"group_pass\":%s,\"cam_rot\":%d,\"tz\":\"%s\",\"summary_at\":%d,\"heap_min\":%u}",
             s_auth[0] ? "true" : "false", s_gauth[0] ? "true" : "false", rt_cam_rotation(), tz, rt_store_get("summary_at", at, sizeof(at)) ? atoi(at) : 21 * 60,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    const esp_err_t err = json(req, b);
    free(b);
    return err;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = malloc(2048);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    rt_wifi_scan_json(b, 2048);
    const esp_err_t err = json(req, b);
    free(b);
    return err;
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    if (!j) {
        return bad(req, "expected JSON");
    }
    esp_err_t err = ESP_OK;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "forget"))) {
        json(req, "{\"ok\":true}");
        vTaskDelay(pdMS_TO_TICKS(300));
        rt_wifi_forget(false);
    } else if (str(j, "ssid")) {
        const char *ssid = str(j, "ssid"), *pass = str(j, "pass");
        if (strlen(ssid) > 32 || (pass && strlen(pass) > 64)) {
            err = bad(req, "name or password too long");
        } else {
            char s2[33], p2[65];
            strlcpy(s2, ssid, sizeof(s2));
            strlcpy(p2, pass ? pass : "", sizeof(p2));
            json(req, "{\"ok\":true}");                     // answer first: joining moves the setup access point's channel
            vTaskDelay(pdMS_TO_TICKS(300));
            rt_wifi_set(s2, p2);
        }
    } else {
        err = bad(req, "ssid missing");
    }
    cJSON_Delete(j);
    return err;
}

static esp_err_t h_alerts(httpd_req_t *req)
{
    static const char *KEYS[] = { "tg_token", "tg_chat", "hook_url", "hook_hdr", "hook_fmt", "mqtt_url", "mqtt_user",
                                  "mqtt_pass", "alert_window" };
    static const uint8_t MAXLEN[] = { 63, 23, 199, 159, 7, 127, 47, 63, 4 };
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    if (!j) {
        return bad(req, "expected JSON");
    }
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        const char *v = str(j, KEYS[i]);
        if (!v) {
            continue;                                      // not sent: left as it is
        }
        if (strlen(v) > MAXLEN[i] || strpbrk(v, "\"\\\r\n") ||
            (!strcmp(KEYS[i], "alert_window") && (strspn(v, "0123456789") != strlen(v) || atoi(v) > 3600))) {
            cJSON_Delete(j);
            return bad(req, !strcmp(KEYS[i], "alert_window") ? "alert_window: 0 to 3600 seconds" : KEYS[i]);
        }
        rt_store_set(KEYS[i], v);
    }
    cJSON_Delete(j);
    rt_alert_reload();
    return json(req, "{\"ok\":true}");
}

// write this board's alert settings on the other boards of the group
static esp_err_t h_alerts_share(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = malloc(900);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    const int n = snprintf(b, 900, "{\"boards\":");
    const int k = rt_alert_share(b + n, 900 - n - 2);
    if (k < 0) {
        strcpy(b + n, "[]}");
    } else {
        strcpy(b + n + k, "}");
    }
    const esp_err_t e = json(req, b);
    free(b);
    return e;
}

static esp_err_t h_test(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const char *d = j ? str(j, "dest") : NULL;
    if (!d || (strcmp(d, "telegram") && strcmp(d, "webhook") && strcmp(d, "mqtt") && strcmp(d, "probe"))) {
        cJSON_Delete(j);
        return bad(req, "dest: telegram | webhook | mqtt | probe");
    }
    rt_alert_result_t r;
    rt_alert_test(d, &r);
    cJSON_Delete(j);
    char b[240];
    snprintf(b, sizeof(b), "{\"ok\":%s,\"status\":%d,\"ms\":%d,\"detail\":\"%s\"}", r.ok ? "true" : "false", r.status, r.ms,
             r.detail);
    return json(req, b);
}

static esp_err_t h_state(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    const size_t cap = 12288;
    char *b = heap_caps_malloc_prefer(cap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    int n = snprintf(b, cap, "{\"run\":%s,\"decision\":", g_state.run && g_state.collect_ms == 0 ? "true" : "false");
    const int d = rt_decide_last(b + n, 2100);
    if (d == 0) {
        n += snprintf(b + n, cap - n, "null");
    } else {
        n += d;
    }
    n += snprintf(b + n, cap - n, ",\"rules\":");
    const int k = rt_actions_json(b + n, cap - n - 2);
    esp_err_t err;
    if (k < 0) {
        err = httpd_resp_send_500(req);
    } else {
        strcpy(b + n + k, "}");
        err = json(req, b);
    }
    free(b);
    return err;
}

static esp_err_t h_rules(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *t = text(req, 12000);
    if (!t) {
        return bad(req, "expected a JSON array of rules (up to 12 KB)");
    }
    const char *p = t;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        p++;
    }
    const esp_err_t e = *p == '[' ? rt_actions_set(p) : (strstr(p, "\"reset\"") ? rt_actions_set(NULL) : ESP_ERR_INVALID_ARG);
    free(t);
    if (e == ESP_OK) {
        return json(req, "{\"ok\":true}");
    }
    return bad(req, e == ESP_ERR_NOT_FOUND ? "a rule names a question, answer or device this board does not have"
                    : e == ESP_ERR_NOT_SUPPORTED ? "a rule uses a pin this board needs for something else"
                    : e == ESP_ERR_INVALID_STATE ? "no model is loaded" : "not a valid list of rules");
}

static esp_err_t h_radio(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const cJSON *m = j ? cJSON_GetObjectItemCaseSensitive(j, "motion") : NULL;
    if (!cJSON_IsBool(m)) {
        cJSON_Delete(j);
        return bad(req, "motion: true | false");
    }
    rt_radio_motion_want(cJSON_IsTrue(m));
    cJSON_Delete(j);
    return json(req, "{\"ok\":true}");
}

// {"rotate":0|90|180|270}: turn the camera picture, for a board that is not upright
static esp_err_t h_camera(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const cJSON *r = j ? cJSON_GetObjectItemCaseSensitive(j, "rotate") : NULL;
    const bool ok = cJSON_IsNumber(r) && rt_cam_rotate(r->valueint);
    cJSON_Delete(j);
    return ok ? json(req, "{\"ok\":true}") : bad(req, "rotate: 0, 90, 180 or 270");
}

static esp_err_t h_time(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    if (!j) {
        return bad(req, "expected JSON");
    }
    const char *tz = str(j, "tz");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(j, "summary_at");
    if (tz && strlen(tz) < 32 && !strpbrk(tz, "\"\\")) {
        rt_store_set("tz", tz);
    }
    if (cJSON_IsNumber(at) && at->valueint >= 0 && at->valueint < 1440) {
        char v[8];
        snprintf(v, sizeof(v), "%d", at->valueint);
        rt_store_set("summary_at", v);
    }
    cJSON_Delete(j);
    rt_actions_clock();
    return json(req, "{\"ok\":true}");
}

static esp_err_t h_frame(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    size_t n = 0;
    uint8_t *jpg = rt_snapshot_jpg(&n);
    if (!jpg) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no camera frame");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, (const char *)jpg, n);
    free(jpg);
    return err;
}

static esp_err_t h_recipes(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, recipes_json_start, recipes_json_end - recipes_json_start - 1);
}

// The request body, piece by piece, into `write`. Returns the number of bytes handed over, or -1.
static int receive(httpd_req_t *req, esp_err_t (*write)(void *ctx, const void *data, size_t n, size_t at), void *ctx)
{
    const size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) {
        return -1;
    }
    size_t at = 0;
    while (at < req->content_len) {
        const int r = httpd_req_recv(req, buf, req->content_len - at < cap ? req->content_len - at : cap);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0 || write(ctx, buf, r, at) != ESP_OK) {
            free(buf);
            return -1;
        }
        at += r;
    }
    free(buf);
    return (int)at;
}

static uint32_t s_new_crc;

static esp_err_t model_write(void *ctx, const void *data, size_t n, size_t at)
{
    if (at == 0 && (n < 16 || memcmp(data, "DCD1", 4))) {
        return ESP_ERR_INVALID_ARG;                       // not a model package: nothing has been erased yet
    }
    const esp_partition_t *part = ctx;
    if (at == 0) {
        memcpy(&s_new_crc, (const uint8_t *)data + 12, 4);
        g_state.run = false;                              // the running model reads its weights from this partition
        vTaskDelay(pdMS_TO_TICKS(600));
        const esp_err_t e = esp_partition_erase_range(part, 0, part->size);
        if (e != ESP_OK) {
            return e;
        }
    }
    return esp_partition_write(part, at, data, n);
}

static esp_err_t h_model(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "model");
    if (!part || req->content_len < 64 || req->content_len > part->size) {
        return bad(req, "expected a model.dcd file of up to 1 MB");
    }
    if (receive(req, model_write, (void *)part) != (int)req->content_len) {
        if (!g_state.run) {                               // the old model is gone: restart to report it cleanly
            json(req, "{\"error\":\"upload failed; restarting without a model\"}");
            vTaskDelay(pdMS_TO_TICKS(500));
            rt_restart();
        }
        return bad(req, "that is not a decidio model package");
    }
    char owner[16];
    if (rt_store_get("rules_for", owner, sizeof(owner))) {   // rules edited on the page stay with the new model
        snprintf(owner, sizeof(owner), "%lu", (unsigned long)s_new_crc);
        rt_store_set("rules_for", owner);
    }
    json(req, "{\"ok\":true,\"restart\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    rt_restart();
    return ESP_OK;
}

static esp_err_t ota_write(void *ctx, const void *data, size_t n, size_t at)
{
    if (at == 0 && (n < 1 || *(const uint8_t *)data != 0xE9)) {
        return ESP_ERR_INVALID_ARG;                       // not an ESP32 app image
    }
    return esp_ota_write(*(esp_ota_handle_t *)ctx, data, n);
}

static esp_err_t h_firmware(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!next || req->content_len < 1024 || req->content_len > next->size) {
        return bad(req, "expected a decidio_rt.bin app image");
    }
    if (esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        return bad(req, "could not start the update");
    }
    if (receive(req, ota_write, &h) != (int)req->content_len) {
        esp_ota_abort(h);
        return bad(req, "upload failed; the running firmware is unchanged");
    }
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(next) != ESP_OK) {
        return bad(req, "the image did not pass its check; the running firmware is unchanged");
    }
    if (!from_peer(req)) {
        rt_push_mark();                                   // from a person: after the restart, the group gets it too
    }
    json(req, "{\"ok\":true,\"restart\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    rt_restart();
    return ESP_OK;
}

static esp_err_t h_reset(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    json(req, "{\"ok\":true,\"restart\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    rt_store_wipe();
    rt_restart();
    return ESP_OK;
}

static esp_err_t h_password(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const char *p = j ? str(j, "pass") : NULL;
    if (!p || strlen(p) > 60) {
        cJSON_Delete(j);
        return bad(req, "pass: up to 60 characters");
    }
    rt_store_set("page_pass", p);
    cJSON_Delete(j);
    load_auth();
    return json(req, "{\"ok\":true}");
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    json(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    rt_restart();
    return ESP_OK;
}

// ---- Smart Life / Tuya devices (devices.c) ----

static esp_err_t h_devices(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = heap_caps_malloc_prefer(6144, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    const esp_err_t err = rt_dev_json(b, 6144) < 0 ? httpd_resp_send_500(req) : json(req, b);
    free(b);
    return err;
}

static esp_err_t h_device_set(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    if (!j) {
        return bad(req, "expected JSON");
    }
    char why[96] = "not saved";
    const esp_err_t e = rt_dev_set(j, why, sizeof(why));
    cJSON_Delete(j);
    return e == ESP_OK ? json(req, "{\"ok\":true}") : bad(req, why);
}

static esp_err_t h_device_test(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const char *what = j ? str(j, "do") : NULL;
    const cJSON *slot = j ? cJSON_GetObjectItemCaseSensitive(j, "slot") : NULL;
    if (!what || !cJSON_IsNumber(slot) || (strcmp(what, "on") && strcmp(what, "off") && strcmp(what, "status"))) {
        cJSON_Delete(j);
        return bad(req, "slot, and do: on | off | status");
    }
    char *b = malloc(1400);
    if (!b) {
        cJSON_Delete(j);
        return httpd_resp_send_500(req);
    }
    char *dps = b + 400;
    rt_alert_result_t r;
    rt_dev_test(slot->valueint, what, &r, dps, 900);
    cJSON_Delete(j);
    const bool obj = dps[0] == '{' && !strpbrk(dps, "\r\n");
    snprintf(b, 400, "{\"ok\":%s,\"ms\":%d,\"detail\":\"%s\",\"answer\":", r.ok ? "true" : "false", r.ms, r.detail);
    const size_t n = strlen(b);
    memmove(b + n, obj ? dps : "null", obj ? strlen(dps) + 1 : 5);
    strcat(b, "}");
    const esp_err_t err = json(req, b);
    free(b);
    return err;
}

static esp_err_t h_device_try(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    if (!j) {
        return bad(req, "expected JSON");
    }
    char *b = malloc(1400);
    if (!b) {
        cJSON_Delete(j);
        return httpd_resp_send_500(req);
    }
    char *dps = b + 400;
    rt_alert_result_t r;
    rt_dev_try(j, &r, dps, 900);
    cJSON_Delete(j);
    const bool obj = dps[0] == '{' && !strpbrk(dps, "\r\n");
    snprintf(b, 400, "{\"ok\":%s,\"ms\":%d,\"detail\":\"%s\",\"answer\":", r.ok ? "true" : "false", r.ms, r.detail);
    const size_t n = strlen(b);
    memmove(b + n, obj ? dps : "null", obj ? strlen(dps) + 1 : 5);
    strcat(b, "}");
    const esp_err_t err = json(req, b);
    free(b);
    return err;
}

// ---- the other boards (home.c) ----

static esp_err_t h_home(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = heap_caps_malloc_prefer(8192, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    const esp_err_t err = rt_home_json(b, 8192) < 0 ? httpd_resp_send_500(req) : json(req, b);
    free(b);
    return err;
}

static esp_err_t h_power(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = heap_caps_malloc_prefer(2048, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    const esp_err_t err = rt_power_json(b, 2048) < 0 ? httpd_resp_send_500(req) : json(req, b);
    free(b);
    return err;
}

static esp_err_t h_power_set(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    if (!j) {
        return bad(req, "expected JSON");
    }
    char why[100] = "not saved";
    const esp_err_t e = rt_power_set(j, why, sizeof(why));
    cJSON_Delete(j);
    return e == ESP_OK ? json(req, "{\"ok\":true}") : bad(req, why);
}

static int peer_call(const char *ip, const char *path, bool post, const char *data, const char *pass, httpd_req_t *relay,
                     char *out, size_t cap);

static esp_err_t h_home_set(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    if (!j) {
        return bad(req, "expected JSON");
    }
    // a new group password on a board of a group: the other boards get it first, while they still know this board
    const char *g = str(j, "group");
    if (g && strlen(g) >= 8 && strlen(g) <= 39 && !strpbrk(g, "\"\\") && rt_home_secret()[0] && !from_peer(req) &&
        strcmp(g, rt_home_secret())) {
        char ips[8][16], hosts[8][20], data[80];
        const int k = rt_home_members(ips, hosts, 8);
        snprintf(data, sizeof(data), "{\"group\":\"%s\"}", g);
        for (int i = 0; i < k; i++) {
            peer_call(ips[i], "/api/home", true, data, NULL, NULL, NULL, 0);
        }
    }
    char why[96] = "not saved";
    const esp_err_t e = rt_home_set(j, why, sizeof(why));
    cJSON_Delete(j);
    return e == ESP_OK ? json(req, "{\"ok\":true}") : bad(req, why);
}

// A request to another board. With `relay` its answer goes straight to that browser request; else the first bytes of
// it go to `out`. pass: that board's page password (when it is not in the group yet); else the group's token is sent.
// Returns the HTTP status, 0 when the board did not answer.
static int peer_call(const char *ip, const char *path, bool post, const char *data, const char *pass, httpd_req_t *relay,
                     char *out, size_t cap)
{
    char url[200];
    snprintf(url, sizeof(url), "http://%s%s", ip, path);
    esp_http_client_config_t c = { .url = url, .timeout_ms = 8000, .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
                                   .buffer_size = 1024, .buffer_size_tx = 768 };
    if (pass && pass[0]) {
        c.username = "decidio";
        c.password = pass;
        c.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        return 0;
    }
    if (rt_home_token()[0]) {
        esp_http_client_set_header(h, "X-Decidio-Group", rt_home_token());
    }
    const int len = data ? (int)strlen(data) : 0;
    if (post) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
    }
    int status = 0;
    if (esp_http_client_open(h, len) == ESP_OK && (len == 0 || esp_http_client_write(h, data, len) == len) &&
        esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
        char *buf = malloc(1024);
        if (relay && buf) {
            char line[24];
            snprintf(line, sizeof(line), "%d %s", status, status == 200 ? "OK" : "Error");
            httpd_resp_set_status(relay, line);
            httpd_resp_set_type(relay, strstr(path, ".jpg") ? "image/jpeg" : strstr(path, ".bin") ? "application/octet-stream" : "application/json");
            httpd_resp_set_hdr(relay, "Cache-Control", "no-store");
            int r;
            while ((r = esp_http_client_read(h, buf, 1024)) > 0) {
                if (httpd_resp_send_chunk(relay, buf, r) != ESP_OK) {
                    break;
                }
            }
            httpd_resp_send_chunk(relay, NULL, 0);
        } else if (out && cap) {
            const int r = esp_http_client_read(h, out, cap - 1);
            out[r > 0 ? r : 0] = 0;
        }
        free(buf);
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

// ---- named inputs, add-on sensors, the fly eye, the board ------------------------------------------------------------

// GET: {"inputs":[...],"pins_in":"5u,6d"}
static esp_err_t h_inputs(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = heap_caps_malloc_prefer(3000, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    char pins[64] = "";
    rt_store_get("pins_in", pins, sizeof(pins));
    int n = snprintf(b, 3000, "{\"pins_in\":\"%s\",\"inputs\":", pins);
    const int k = rt_inputs_json(b + n, 3000 - n - 2);
    strcpy(b + n + (k < 0 ? 0 : k), k < 0 ? "[]}" : "}");
    const esp_err_t err = json(req, b);
    free(b);
    return err;
}

// POST {"pin_in":5,"pull":"up|down|none"} | {"pin_off":5} | {"i2c_scan":[sda,scl]}
static esp_err_t h_inputs_set(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const cJSON *in = j ? cJSON_GetObjectItemCaseSensitive(j, "pin_in") : NULL;
    const cJSON *off = j ? cJSON_GetObjectItemCaseSensitive(j, "pin_off") : NULL;
    const cJSON *scan = j ? cJSON_GetObjectItemCaseSensitive(j, "i2c_scan") : NULL;
    char why[100] = "", out[760];
    esp_err_t err;
    if (cJSON_IsArray(scan) && cJSON_GetArraySize(scan) == 2) {
        rt_i2c_scan_json(cJSON_GetArrayItem(scan, 0)->valueint, cJSON_GetArrayItem(scan, 1)->valueint, out, sizeof(out));
        err = json(req, out);
    } else if (cJSON_IsNumber(in) || cJSON_IsNumber(off)) {
        const bool ok = rt_pin_in_set(cJSON_IsNumber(in) ? in->valueint : off->valueint, str(j, "pull"), !cJSON_IsNumber(in), why, sizeof(why));
        snprintf(out, sizeof(out), "{\"ok\":true,\"pins_in\":\"%s\",\"restart\":true}", why);
        err = ok ? json(req, out) : bad(req, why);
    } else {
        err = bad(req, "pin_in | pin_off | i2c_scan");
    }
    cJSON_Delete(j);
    return err;
}

static esp_err_t h_eye(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char *b = heap_caps_malloc_prefer(2600, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    rt_eye_json(b, 2600);
    const esp_err_t err = json(req, b);
    free(b);
    return err;
}

// the eye's picture for the page: retina, flow, figures, tracks (eye.cc describes the bytes); 204 while the eye is off
static esp_err_t h_eye_bin(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    uint8_t *b = heap_caps_malloc_prefer(RT_EYE_PACKET_MAX, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    const size_t n = rt_eye_packet(b, RT_EYE_PACKET_MAX);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err;
    if (!n) {
        httpd_resp_set_status(req, "204 No Content");
        err = httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
        err = httpd_resp_send(req, (const char *)b, n);
    }
    free(b);
    return err;
}

// the last sharp close-up (eye.cc describes the bytes); 204 while there is none. ?wide gives the last full frame.
static esp_err_t h_eye_sharp(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    uint8_t *b = heap_caps_malloc_prefer(RT_EYE_SHARP_MAX, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    char q[16] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    const size_t n = strstr(q, "wide") ? rt_eye_wide(b, RT_EYE_SHARP_MAX) : rt_eye_sharp(b, RT_EYE_SHARP_MAX);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err;
    if (!n) {
        httpd_resp_set_status(req, "204 No Content");
        err = httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
        err = httpd_resp_send(req, (const char *)b, n);
    }
    free(b);
    return err;
}

// POST {"on":true|false} | {"lock":<track id, 0 = let go, -1 = the eye chooses>}
static esp_err_t h_eye_set(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const cJSON *on = j ? cJSON_GetObjectItemCaseSensitive(j, "on") : NULL;
    const cJSON *lock = j ? cJSON_GetObjectItemCaseSensitive(j, "lock") : NULL;
    esp_err_t err;
    if (cJSON_IsBool(on)) {
        rt_eye_want(cJSON_IsTrue(on));
        err = json(req, rt_eye_on() == cJSON_IsTrue(on) ? "{\"ok\":true}" : "{\"ok\":true,\"restart\":true}");
    } else if (cJSON_IsNumber(lock)) {
        err = rt_eye_lock(lock->valueint) ? json(req, "{\"ok\":true}") : bad(req, "that figure is gone");
    } else {
        err = bad(req, "on | lock");
    }
    cJSON_Delete(j);
    return err;
}

// POST {"profile":"auto"|"xiao_sense"|"s3cam"|"s3cam-lcd"}: look for the sensors again, or force a board; at the next start
static esp_err_t h_board(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const char *p = j ? str(j, "profile") : NULL;
    const esp_err_t err = p && rt_board_command(p) ? json(req, "{\"ok\":true,\"restart\":true}") : bad(req, "profile: auto, xiao_sense, s3cam or s3cam-lcd");
    cJSON_Delete(j);
    return err;
}

// {"host":"decidio-ab12","pass":"its page password, if it has one"}: give that board this board's group secret
static esp_err_t h_home_add(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    cJSON *j = body(req);
    const char *host = j ? str(j, "host") : NULL;
    char ip[16], data[80], out[120];
    esp_err_t err;
    if (!host) {
        err = bad(req, "host missing");
    } else if (!rt_home_peer_ip(host, ip, sizeof(ip))) {
        err = bad(req, "that board has not been heard on the network");
    } else {
        rt_home_begin();                                 // not in a group: this starts one that has no password
        snprintf(data, sizeof(data), "{\"group\":\"%s\",\"open\":%s}", rt_home_secret(), rt_home_open() ? "true" : "false");
        const int status = peer_call(ip, "/api/home", true, data, str(j, "pass"), NULL, out, sizeof(out));
        err = status == 200 ? json(req, "{\"ok\":true}")
              : bad(req, status == 401 ? "that board has a page password: enter it" : status ? "that board refused" : "that board did not answer");
    }
    cJSON_Delete(j);
    return err;
}

// send the firmware this board runs to the other boards of the group
static esp_err_t h_home_push(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char why[64] = "not started";
    return rt_push_start(why, sizeof(why)) ? json(req, "{\"ok\":true}") : bad(req, why);
}

// /api/via/<board>/api/...: the same request, sent on to another board of the group
static esp_err_t h_via(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    const char *p = req->uri + strlen("/api/via/"), *rest = strchr(p, '/');
    char host[24], ip[16];
    if (!rest || rest - p >= (int)sizeof(host) || strncmp(rest, "/api/", 5) || !strncmp(rest, "/api/via", 8) ||
        !strncmp(rest, "/api/model", 10) || !strncmp(rest, "/api/firmware", 13)) {
        return bad(req, "not something that can be passed on (uploads go to the board itself)");
    }
    memcpy(host, p, rest - p);
    host[rest - p] = 0;
    if (!rt_home_token()[0] || !rt_home_peer_ip(host, ip, sizeof(ip))) {
        httpd_resp_set_status(req, "404 Not Found");
        return json(req, "{\"error\":\"that board is not in the group, or has not been heard lately\"}");
    }
    char *data = NULL;
    if (req->method == HTTP_POST && req->content_len) {
        data = text(req, 12000);
        if (!data) {
            return bad(req, "body too long");
        }
    }
    const int status = peer_call(ip, rest, req->method == HTTP_POST, data, NULL, req, NULL, 0);
    free(data);
    if (status == 0) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        return json(req, "{\"error\":\"that board did not answer\"}");
    }
    return ESP_OK;
}

// anything else: on the setup access point a phone's "is this the internet?" request is sent to the page
static esp_err_t h_other(httpd_req_t *req, httpd_err_code_t err)
{
    if (rt_wifi_setup_open()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

void rt_web_start(void)
{
    if (s_srv) {
        return;
    }
    load_auth();
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 6144;
    c.max_uri_handlers = 44;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.max_open_sockets = 4;
    c.lru_purge_enable = true;
    c.core_id = 0;
    if (httpd_start(&s_srv, &c) != ESP_OK) {
        ESP_LOGE(TAG, "the web server did not start");
        s_srv = NULL;
        return;
    }
    static const httpd_uri_t U[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_index },
        { .uri = "/api/info", .method = HTTP_GET, .handler = h_info },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = h_scan },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi },
        { .uri = "/api/alerts", .method = HTTP_POST, .handler = h_alerts },
        { .uri = "/api/alerts/share", .method = HTTP_POST, .handler = h_alerts_share },
        { .uri = "/api/test", .method = HTTP_POST, .handler = h_test },
        { .uri = "/api/state", .method = HTTP_GET, .handler = h_state },
        { .uri = "/api/rules", .method = HTTP_POST, .handler = h_rules },
        { .uri = "/api/radio", .method = HTTP_POST, .handler = h_radio },
        { .uri = "/api/time", .method = HTTP_POST, .handler = h_time },
        { .uri = "/api/camera", .method = HTTP_POST, .handler = h_camera },
        { .uri = "/api/frame.jpg", .method = HTTP_GET, .handler = h_frame },
        { .uri = "/api/recipes", .method = HTTP_GET, .handler = h_recipes },
        { .uri = "/api/model", .method = HTTP_POST, .handler = h_model },
        { .uri = "/api/firmware", .method = HTTP_POST, .handler = h_firmware },
        { .uri = "/api/reset", .method = HTTP_POST, .handler = h_reset },
        { .uri = "/api/password", .method = HTTP_POST, .handler = h_password },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/api/devices", .method = HTTP_GET, .handler = h_devices },
        { .uri = "/api/devices", .method = HTTP_POST, .handler = h_device_set },
        { .uri = "/api/devices/test", .method = HTTP_POST, .handler = h_device_test },
        { .uri = "/api/devices/try", .method = HTTP_POST, .handler = h_device_try },
        { .uri = "/api/home", .method = HTTP_GET, .handler = h_home },
        { .uri = "/api/home", .method = HTTP_POST, .handler = h_home_set },
        { .uri = "/api/home/add", .method = HTTP_POST, .handler = h_home_add },
        { .uri = "/api/home/push", .method = HTTP_POST, .handler = h_home_push },
        { .uri = "/api/inputs", .method = HTTP_GET, .handler = h_inputs },
        { .uri = "/api/inputs", .method = HTTP_POST, .handler = h_inputs_set },
        { .uri = "/api/eye", .method = HTTP_GET, .handler = h_eye },
        { .uri = "/api/eye", .method = HTTP_POST, .handler = h_eye_set },
        { .uri = "/api/eye.bin", .method = HTTP_GET, .handler = h_eye_bin },
        { .uri = "/api/eye/sharp.bin", .method = HTTP_GET, .handler = h_eye_sharp },
        { .uri = "/api/board", .method = HTTP_POST, .handler = h_board },
        { .uri = "/api/power", .method = HTTP_GET, .handler = h_power },
        { .uri = "/api/power", .method = HTTP_POST, .handler = h_power_set },
        { .uri = "/api/via/*", .method = HTTP_GET, .handler = h_via },
        { .uri = "/api/via/*", .method = HTTP_POST, .handler = h_via },
    };
    for (size_t i = 0; i < sizeof(U) / sizeof(U[0]); i++) {
        httpd_register_uri_handler(s_srv, &U[i]);
    }
    httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, h_other);
    rt_alert_start();
    rt_home_start();
}
