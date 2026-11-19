// One board gives the firmware it runs to the other boards of its group, so that you update one board and the
// others follow. The firmware is the same file for every board (board.c finds the sensors at start).
//
// When new firmware comes from the page of a board (POST /api/firmware from a browser), that board writes a mark in
// its settings and restarts. After the restart it runs the new firmware, joins the network and hears the group. Then
// it sends its own app image, read from its flash, to each member that runs a different build: POST /api/firmware
// with the group's token, the same request that the page makes. That board checks the image, restarts with it and
// says its new build in its broadcasts. A board that gets firmware from another board does not write the mark, so the
// firmware does not go round the group for ever. The button on the Group tab starts the same push by hand.
//
// A build is known by the first 8 hex characters of the SHA-256 of its ELF file (rt_home_fwid). Boards with an older
// firmware do not say their build: they count as different, so they get the update.
// A board that sleeps (low power) is not heard, so it is not updated: push again while it is awake.
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "rt.h"

#define MAX_ITEM 8
#define WINDOW_US (6 * 60 * 1000000LL)      // a push gives up after this long
#define SETTLE_US (25 * 1000000LL)          // after a start, wait this long for the group to be heard
#define RESTART_US (75 * 1000000LL)         // a board that took the image has this long to come back with it

enum { ST_WAIT = 0, ST_SEND, ST_SENT, ST_DONE, ST_SAME, ST_FAIL };
static const char *const ST_NAME[] = { "waiting", "sending", "restarting", "updated", "same", "failed" };

typedef struct {
    char host[20];
    uint8_t st, tries;
    int64_t at_us;
} item_t;

static const char *TAG = "push";
static item_t s_it[MAX_ITEM];
static int s_n;
static volatile int s_state;                // 0 idle, 1 running, 2 done
static const void *s_img;                   // this board's own app image, mapped from flash
static uint32_t s_len;
static TaskHandle_t s_task;

// Find this board's app image in flash. Called from a task with its stack in internal memory (flash is read).
static bool prepare(void)
{
    if (s_img) {
        return true;
    }
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) {
        return false;
    }
    const esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
    esp_image_metadata_t meta;
    esp_partition_mmap_handle_t h;
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len < 1024 || meta.image_len > p->size ||
        esp_partition_mmap(p, 0, meta.image_len, ESP_PARTITION_MMAP_DATA, &s_img, &h) != ESP_OK) {
        s_img = NULL;
        return false;
    }
    s_len = meta.image_len;
    return true;
}

// the HTTP status of the board's answer, 0 = no answer
static int send_image(const char *ip)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s/api/firmware", ip);
    const esp_http_client_config_t c = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 20000, .buffer_size = 1024,
                                         .buffer_size_tx = 768 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        return 0;
    }
    esp_http_client_set_header(h, "X-Decidio-Group", rt_home_token());
    esp_http_client_set_header(h, "Content-Type", "application/octet-stream");
    int status = 0;
    if (esp_http_client_open(h, s_len) == ESP_OK) {
        uint32_t at = 0;
        while (at < s_len) {
            const int n = s_len - at < 2048 ? (int)(s_len - at) : 2048;
            const int w = esp_http_client_write(h, (const char *)s_img + at, n);
            if (w <= 0) {
                break;
            }
            at += w;
        }
        if (at == s_len && esp_http_client_fetch_headers(h) >= 0) {
            status = esp_http_client_get_status_code(h);
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

static item_t *item(const char *host)
{
    for (int i = 0; i < s_n; i++) {
        if (!strcmp(s_it[i].host, host)) {
            return &s_it[i];
        }
    }
    if (s_n >= MAX_ITEM) {
        return NULL;
    }
    item_t *it = &s_it[s_n++];
    memset(it, 0, sizeof(*it));
    snprintf(it->host, sizeof(it->host), "%s", host);
    return it;
}

static void run(void)
{
    static char ips[MAX_ITEM][16], hosts[MAX_ITEM][20], ids[MAX_ITEM][10];
    const int64_t start = esp_timer_get_time();
    s_n = 0;
    s_state = 1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        const int64_t now = esp_timer_get_time();
        if (now - start > WINDOW_US) {
            break;
        }
        if (!rt_wifi_online() || !rt_home_token()[0]) {
            continue;
        }
        const int n = rt_home_fleet(ips, hosts, ids, MAX_ITEM);
        for (int i = 0; i < n; i++) {
            item_t *it = item(hosts[i]);
            if (!it) {
                continue;
            }
            if (!strcmp(ids[i], rt_home_fwid())) {
                it->st = it->tries ? ST_DONE : ST_SAME;
            } else if (it->st == ST_SENT && esp_timer_get_time() - it->at_us < RESTART_US) {
                // it took the image and restarts: its broadcasts say the old build until it has started again
            } else if (it->tries >= 3) {
                it->st = ST_FAIL;
            } else {
                it->st = ST_SEND;
                it->tries++;
                ESP_LOGI(TAG, "sending the firmware to %s (%s)", hosts[i], ips[i]);
                const int status = send_image(ips[i]);
                ESP_LOGI(TAG, "%s answered %d", hosts[i], status);
                it->st = status == 200 ? ST_SENT : ST_WAIT;
                it->at_us = esp_timer_get_time();
            }
        }
        bool todo = false;
        for (int i = 0; i < s_n; i++) {
            if (s_it[i].st == ST_WAIT || s_it[i].st == ST_SEND) {
                todo = true;
            } else if (s_it[i].st == ST_SENT) {
                if (esp_timer_get_time() - s_it[i].at_us < RESTART_US) {
                    todo = true;
                } else if (s_it[i].tries >= 3) {
                    s_it[i].st = ST_FAIL;                  // it did not come back with this build
                } else {
                    todo = true;
                }
            }
        }
        if (!todo && esp_timer_get_time() - start > SETTLE_US) {
            break;
        }
    }
    s_state = 2;
}

static void push_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run();
    }
}

bool rt_push_start(char *why, size_t cap)
{
    if (!rt_home_secret()[0]) {
        snprintf(why, cap, "this board is not in a group");
        return false;
    }
    if (rt_power_on()) {
        snprintf(why, cap, "switch low power off first");
        return false;
    }
    if (s_state == 1) {
        snprintf(why, cap, "a push is in progress");
        return false;
    }
    if (!prepare()) {
        snprintf(why, cap, "could not read this board's own firmware");
        return false;
    }
    // the stack lives in external memory when there is some: the task reads the image through the mapping, not with
    // flash commands
    if (!s_task && xTaskCreatePinnedToCoreWithCaps(push_task, "push", 6144, NULL, 2, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS &&
        xTaskCreatePinnedToCore(push_task, "push", 5120, NULL, 2, &s_task, 0) != pdPASS) {
        s_task = NULL;
        snprintf(why, cap, "no memory");
        return false;
    }
    s_state = 1;
    xTaskNotifyGive(s_task);
    return true;
}

void rt_push_mark(void)
{
    if (rt_home_secret()[0] && !rt_power_on()) {
        rt_store_set("fw_push", "1");
    }
}

void rt_push_boot(void)
{
    char v[4], why[48];
    if (rt_store_get("fw_push", v, sizeof(v))) {
        rt_store_set("fw_push", NULL);                     // one try: a push that fails does not start again at each restart
        if (!rt_push_start(why, sizeof(why))) {
            ESP_LOGW(TAG, "no push: %s", why);
        }
    }
}

int rt_push_json(char *dst, size_t cap)
{
    int n = snprintf(dst, cap, "{\"state\":\"%s\",\"boards\":[", s_state == 1 ? "run" : s_state == 2 ? "done" : "idle");
    for (int i = 0; i < s_n && n < (int)cap; i++) {
        n += snprintf(dst + n, cap - n, "%s{\"host\":\"%s\",\"state\":\"%s\"}", i ? "," : "", s_it[i].host, ST_NAME[s_it[i].st]);
    }
    if (n < (int)cap) {
        n += snprintf(dst + n, cap - n, "]}");
    }
    return n;
}
