// Check for updates: the board reads the version of the newest firmware from the installer website
// (builds/index.json) and, when the person asks, writes builds/app.bin to its other app partition over Wi-Fi, then
// restarts into it. After the restart it gives the new firmware to the other boards of the group, as an update from
// the page does. Nothing runs unless a person clicks the button: the board does not look for updates by itself.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rt.h"

#define UPDATE_BASE "https://ioserviceslabs.github.io/decidio/builds/"

static const char *TAG = "update";

static volatile bool s_busy;
static char s_state[16] = "idle";                       // idle, checking, checked, downloading, failed, restarting
static char s_latest[16];
static char s_error[64];
static volatile int s_got, s_total;
static bool s_install;

static void fail(const char *why)
{
    snprintf(s_error, sizeof(s_error), "%s", why);
    strcpy(s_state, "failed");
    ESP_LOGW(TAG, "%s", why);
}

// "0.5.1" -> 5001 style number for a comparison; parts above 999 do not happen
static long vnum(const char *v)
{
    long a = 0, b = 0, c = 0;
    sscanf(v, "%ld.%ld.%ld", &a, &b, &c);
    return a * 1000000L + b * 1000L + c;
}

static esp_http_client_handle_t open_get(const char *url)
{
    const esp_http_client_config_t c = {
        .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (h && esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0 &&
        esp_http_client_get_status_code(h) == 200) {
        return h;
    }
    if (h) {
        esp_http_client_cleanup(h);
    }
    return NULL;
}

// index.json starts with {"version": "x.y.z", ...}: the first bytes are enough
static bool check(void)
{
    esp_http_client_handle_t h = open_get(UPDATE_BASE "index.json");
    if (!h) {
        fail("the update site did not answer");
        return false;
    }
    char buf[256];
    int n = 0, r;
    while (n < (int)sizeof(buf) - 1 && (r = esp_http_client_read(h, buf + n, sizeof(buf) - 1 - n)) > 0) {
        n += r;
    }
    esp_http_client_cleanup(h);
    buf[n] = 0;
    const char *v = strstr(buf, "\"version\"");
    v = v ? strchr(v + 9, '"') : NULL;
    const char *e = v ? strchr(v + 1, '"') : NULL;
    if (!v || !e || e - v - 1 >= (int)sizeof(s_latest) || e - v - 1 < 1) {
        fail("the update site has no version");
        return false;
    }
    memcpy(s_latest, v + 1, e - v - 1);
    s_latest[e - v - 1] = 0;
    return true;
}

static bool install(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_http_client_handle_t h = next ? open_get(UPDATE_BASE "app.bin") : NULL;
    if (!h) {
        fail(next ? "could not download the firmware" : "no partition for an update");
        return false;
    }
    const int64_t len = esp_http_client_get_content_length(h);
    if (len < 1024 || len > (int64_t)next->size) {
        esp_http_client_cleanup(h);
        fail("the firmware file has a wrong size");
        return false;
    }
    s_total = (int)len;
    s_got = 0;
    strcpy(s_state, "downloading");
    esp_ota_handle_t o;
    if (esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &o) != ESP_OK) {
        esp_http_client_cleanup(h);
        fail("could not start the update");
        return false;
    }
    char *buf = malloc(4096);
    bool ok = buf != NULL;
    while (ok && s_got < s_total) {
        const int r = esp_http_client_read(h, buf, 4096);
        if (r <= 0) {
            ok = false;
            break;
        }
        if ((s_got == 0 && (uint8_t)buf[0] != 0xE9) || esp_ota_write(o, buf, r) != ESP_OK) {   // 0xE9: an app image
            ok = false;
            break;
        }
        s_got += r;
    }
    free(buf);
    esp_http_client_cleanup(h);
    if (!ok) {
        esp_ota_abort(o);
        fail("the download stopped; the running firmware is unchanged");
        return false;
    }
    if (esp_ota_end(o) != ESP_OK || esp_ota_set_boot_partition(next) != ESP_OK) {
        fail("the image did not pass its check; the running firmware is unchanged");
        return false;
    }
    return true;
}

static void update_task(void *arg)
{
    strcpy(s_state, "checking");
    if (check()) {
        const bool newer = vnum(s_latest) > vnum(RT_VERSION);
        if (!s_install || !newer) {
            strcpy(s_state, "checked");
        } else if (install()) {
            strcpy(s_state, "restarting");
            ESP_LOGI(TAG, "firmware %s written; restarting", s_latest);
            rt_push_mark();                                 // after the restart, the group gets it too
            vTaskDelay(pdMS_TO_TICKS(1500));                // the page sees "restarting" first
            rt_restart();
        }
    }
    s_busy = false;
    vTaskDelete(NULL);
}

bool rt_update_start(bool install_it)
{
    if (s_busy) {
        return false;
    }
    s_busy = true;
    s_install = install_it;
    s_error[0] = 0;
    s_got = s_total = 0;
    strcpy(s_state, "checking");                            // before the task runs, so the page never sees an old state
    if (xTaskCreate(update_task, "update", 8192, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        return false;
    }
    return true;
}

int rt_update_json(char *dst, size_t cap)
{
    const bool known = s_latest[0] != 0;
    return snprintf(dst, cap,
                    "{\"state\":\"%s\",\"current\":\"%s\",\"latest\":\"%s\",\"newer\":%s,\"got\":%d,\"total\":%d,"
                    "\"error\":\"%s\"}",
                    s_state, RT_VERSION, s_latest, known && vnum(s_latest) > vnum(RT_VERSION) ? "true" : "false",
                    s_got, s_total, s_error);
}
