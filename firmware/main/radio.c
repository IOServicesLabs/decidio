// Radio sensing: Wi-Fi channel state (CSI) of frames the board hears.
//
// For every frame from the chosen sender (normally your router's beacons, about 10 a second) the radio reports how each
// subcarrier arrived. A body moving through the path between router and board changes those numbers; a still room
// leaves them steady.
//
// Two uses:
//   capture   `radio <channel>` streams the numbers to the PC (tools/radio_record.py). If the board has not joined a
//             network it only listens and sends nothing; if it has, only that network's channel can be heard.
//   movement  `radio motion on` (needs a joined network): a score from the last 2 seconds of the router's beacons,
//             0 = perfectly still. Rules can test it as "radio_motion". It sees movement; it cannot tell a still person
//             from an empty room.
//
// Console:  radio scan            list senders heard on each channel (address, channel, signal) - no network names
//           radio <channel>       stream every sender on that channel
//           radio <channel> <aa:bb:cc:dd:ee:ff>   stream that sender only
//           radio off
//           radio motion on|off   (kept across restarts)
// Lines:    @R <ms> <sender> <rssi> <channel> <base64 of int8 pairs (imaginary, real) per subcarrier>
// The Wi-Fi stack starts on the first `radio` command, so a board that never uses it pays nothing.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "rt.h"

#define CSI_MAX 384                  // bytes kept per frame (HT40 gives up to 384)
#define MIN_GAP_US 10000             // at most 100 lines a second

static const char *TAG = "radio";

typedef struct {
    uint32_t ms;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    uint16_t len;
    int8_t buf[CSI_MAX];
} csi_item_t;

static QueueHandle_t s_q;
static bool s_started, s_on, s_filter;
static uint8_t s_mac[6];
static int64_t s_last_us;
static uint32_t s_seen, s_dropped;

// movement score: a ring of amplitudes, one frame at most every 80 ms, from the joined network's access point
#define MOT_N 25
#define MOT_SC 64
#define MOT_GAP_US 80000
#define MOT_WINDOW_US 2200000
static float *s_mot;                 // MOT_N x MOT_SC
static int64_t s_mot_t[MOT_N], s_mot_last_us, s_mot_calc_us;
static int s_mot_i;
static uint8_t s_mot_mac[6];
static bool s_motion, s_want, s_want_read;
static float s_mot_score = -1;
static SemaphoreHandle_t s_mot_lock;

static void motion_frame(const wifi_csi_info_t *info, int64_t now)
{
    if (info->len < 2 * MOT_SC || memcmp(info->mac, s_mot_mac, 6) || now - s_mot_last_us < MOT_GAP_US ||
        xSemaphoreTake(s_mot_lock, 0) != pdTRUE) {
        return;
    }
    s_mot_last_us = now;
    float *row = s_mot + (size_t)s_mot_i * MOT_SC;
    for (int k = 0; k < MOT_SC; k++) {
        const float im = info->buf[2 * k], re = info->buf[2 * k + 1];
        row[k] = (info->first_word_invalid && k < 2) ? 0 : sqrtf(im * im + re * re);
    }
    s_mot_t[s_mot_i] = now;
    s_mot_i = (s_mot_i + 1) % MOT_N;
    xSemaphoreGive(s_mot_lock);
}

// the same number tools/radio_record.py prints: each frame scaled to mean 1 (the receiver's gain moves on its own),
// then the spread over time, averaged over the subcarriers that carry signal
static float motion_score(int64_t now)
{
    static float mean_sc[MOT_SC], sum[MOT_SC], sq[MOT_SC], scale[MOT_N];
    static bool use[MOT_N];
    int frames = 0;
    float all = 0;
    memset(mean_sc, 0, sizeof(mean_sc));
    for (int i = 0; i < MOT_N; i++) {
        use[i] = s_mot_t[i] && now - s_mot_t[i] < MOT_WINDOW_US;
        if (use[i]) {
            frames++;
            for (int k = 0; k < MOT_SC; k++) {
                mean_sc[k] += s_mot[i * MOT_SC + k];
            }
        }
    }
    if (frames < 8) {
        return -1;
    }
    for (int k = 0; k < MOT_SC; k++) {
        mean_sc[k] /= frames;
        all += mean_sc[k];
    }
    all /= MOT_SC;
    int live = 0;
    for (int k = 0; k < MOT_SC; k++) {
        live += mean_sc[k] > 0.2f * all;
    }
    if (!live || all <= 0) {
        return -1;
    }
    for (int i = 0; i < MOT_N; i++) {                    // each frame's own mean over the live subcarriers
        if (use[i]) {
            float m = 0;
            for (int k = 0; k < MOT_SC; k++) {
                if (mean_sc[k] > 0.2f * all) {
                    m += s_mot[i * MOT_SC + k];
                }
            }
            scale[i] = m > 0 ? live / m : 0;
        }
    }
    memset(sum, 0, sizeof(sum));
    memset(sq, 0, sizeof(sq));
    for (int i = 0; i < MOT_N; i++) {
        if (use[i]) {
            for (int k = 0; k < MOT_SC; k++) {
                const float v = s_mot[i * MOT_SC + k] * scale[i];
                sum[k] += v;
                sq[k] += v * v;
            }
        }
    }
    float total = 0;
    for (int k = 0; k < MOT_SC; k++) {
        if (mean_sc[k] > 0.2f * all) {
            const float mu = sum[k] / frames, var = sq[k] / frames - mu * mu;
            total += var > 0 ? sqrtf(var) : 0;
        }
    }
    return total / live;
}

static void on_csi(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len <= 0) {
        return;
    }
    if (s_motion) {
        motion_frame(info, esp_timer_get_time());
    }
    if (!s_on) {
        return;
    }
    if (s_filter && memcmp(info->mac, s_mac, 6)) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - s_last_us < MIN_GAP_US) {
        return;
    }
    s_last_us = now;
    static csi_item_t it;            // the Wi-Fi task is the only caller
    it.ms = (uint32_t)(now / 1000);
    memcpy(it.mac, info->mac, 6);
    it.rssi = info->rx_ctrl.rssi;
    it.channel = info->rx_ctrl.channel;
    it.len = info->len > CSI_MAX ? CSI_MAX : info->len;
    memcpy(it.buf, info->buf, it.len);
    s_seen++;
    if (xQueueSend(s_q, &it, 0) != pdTRUE) {
        s_dropped++;
    }
}

static void on_packet(void *buf, wifi_promiscuous_pkt_type_t type)
{
    // nothing to do: promiscuous mode is on only so that CSI is produced for frames not addressed to this board
}

static void print_task(void *arg)
{
    static csi_item_t it;
    static unsigned char b64[CSI_MAX * 4 / 3 + 8];
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t n = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64), &n, (const unsigned char *)it.buf, it.len) != 0) {
            continue;
        }
        b64[n] = 0;
        printf("@R %lu %02x:%02x:%02x:%02x:%02x:%02x %d %d %s\n", (unsigned long)it.ms, it.mac[0], it.mac[1], it.mac[2],
               it.mac[3], it.mac[4], it.mac[5], it.rssi, it.channel, (const char *)b64);
    }
}

static esp_err_t start_once(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_q = xQueueCreate(16, sizeof(csi_item_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(rt_wifi_driver(), TAG, "wifi");          // shared with wifi.c
    const wifi_promiscuous_filter_t pf = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&pf);
    esp_wifi_set_promiscuous_rx_cb(on_packet);
    wifi_csi_config_t cc = {
        .lltf_en = true, .htltf_en = true, .stbc_htltf2_en = true, .ltf_merge_en = true, .channel_filter_en = false,
        .manu_scale = false, .shift = 0,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_config(&cc), TAG, "csi config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_rx_cb(on_csi, NULL), TAG, "csi callback");
    if (xTaskCreatePinnedToCore(print_task, "radio", 4096, NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

static esp_err_t listen(int channel, bool on)
{
    s_on = false;
    esp_wifi_set_csi(s_motion);                       // the movement score keeps listening to the joined network
    esp_wifi_set_promiscuous(false);
    if (!on) {
        return ESP_OK;
    }
    if (rt_wifi_setup_open()) {
        return ESP_ERR_INVALID_STATE;                 // the setup access point is in use
    }
    const int joined = rt_wifi_channel();             // joined to a network: its channel is the only one we can hear
    if (joined && joined != channel) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "promiscuous");
    if (!joined) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), TAG, "channel");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi(true), TAG, "csi");
    s_seen = s_dropped = 0;
    s_on = true;
    return ESP_OK;
}

static void scan(void)
{
    listen(0, false);
    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        printf("{\"error\":\"radio scan failed\"}\n");
        return;
    }
    uint16_t n = 24;
    static wifi_ap_record_t ap[24];
    if (esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) {
        n = 0;
    }
    printf("{\"radio_scan\":[");
    for (int i = 0; i < n; i++) {        // strongest first; names are left out on purpose
        printf("%s{\"sender\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"channel\":%d,\"rssi\":%d}", i ? "," : "", ap[i].bssid[0],
               ap[i].bssid[1], ap[i].bssid[2], ap[i].bssid[3], ap[i].bssid[4], ap[i].bssid[5], ap[i].primary, ap[i].rssi);
    }
    printf("]}\n");
}

static void motion_enable(bool on)
{
    if (!on) {
        s_motion = false;
        s_mot_score = -1;
        if (s_started && !s_on) {
            esp_wifi_set_csi(false);
        }
        return;
    }
    wifi_ap_record_t ap;
    if (s_motion || !rt_wifi_online() || start_once() != ESP_OK || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    if (!s_mot_lock) {
        s_mot_lock = xSemaphoreCreateMutex();
        s_mot = heap_caps_malloc(sizeof(float) * MOT_N * MOT_SC, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mot) {
            s_mot = malloc(sizeof(float) * MOT_N * MOT_SC);
        }
    }
    if (!s_mot_lock || !s_mot) {
        return;
    }
    memset(s_mot_t, 0, sizeof(s_mot_t));
    memcpy(s_mot_mac, ap.bssid, 6);
    s_motion = esp_wifi_set_csi(true) == ESP_OK;
}

// The movement score over the last 2 seconds; < 0 when it is off, the board has not joined a network, or too few
// frames arrived. Called after every decision: it also starts the capture once Wi-Fi is up, and follows a new router.
float rt_radio_motion(void)
{
    if (!s_want_read) {
        char v[4];
        s_want_read = true;
        s_want = rt_store_get("radio_motion", v, sizeof(v));
    }
    if (!s_want) {
        return -1;
    }
    const int64_t now = esp_timer_get_time();
    if (now - s_mot_calc_us < 250000) {
        return s_mot_score;
    }
    s_mot_calc_us = now;
    if (!rt_wifi_online()) {
        motion_enable(false);
        return -1;
    }
    if (!s_motion) {
        motion_enable(true);
        return -1;
    }
    if (xSemaphoreTake(s_mot_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return s_mot_score;
    }
    s_mot_score = motion_score(now);
    xSemaphoreGive(s_mot_lock);
    if (s_mot_score < 0 && now - s_mot_last_us > 10000000) {      // silence for 10 s: maybe another access point now
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_mot_mac, ap.bssid, 6);
        }
    }
    return s_mot_score;
}

void rt_radio_motion_want(bool on)
{
    s_want_read = true;
    s_want = on;
    rt_store_set("radio_motion", on ? "1" : NULL);
    if (!on) {
        motion_enable(false);
    }
}

// `radio <arg> [sender]` from the console; false = bad arguments
bool rt_radio_command(const char *arg, const char *sender)
{
    if (!arg) {
        return false;
    }
    if (!strcmp(arg, "motion")) {
        if (!sender || (strcmp(sender, "on") && strcmp(sender, "off"))) {
            return false;
        }
        rt_radio_motion_want(!strcmp(sender, "on"));
        return true;
    }
    const esp_err_t err = start_once();
    if (err != ESP_OK) {
        printf("{\"error\":\"radio: %s\"}\n", esp_err_to_name(err));
        return true;
    }
    if (!strcmp(arg, "off")) {
        listen(0, false);
    } else if (!strcmp(arg, "scan")) {
        scan();
    } else {
        const int ch = atoi(arg);
        if (ch < 1 || ch > 13) {
            return false;
        }
        s_filter = false;
        if (sender) {
            unsigned m[6];
            if (sscanf(sender, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
                return false;
            }
            for (int i = 0; i < 6; i++) {
                s_mac[i] = (uint8_t)m[i];
            }
            s_filter = true;
        }
        const esp_err_t e = listen(ch, true);
        if (e == ESP_ERR_NOT_SUPPORTED) {
            printf("{\"error\":\"radio: joined to a network on channel %d; only that channel can be heard\"}\n",
                   rt_wifi_channel());
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("{\"error\":\"radio: finish the Wi-Fi setup first (or `wifi off`)\"}\n");
        } else if (e != ESP_OK) {
            printf("{\"error\":\"radio: could not listen on channel %d\"}\n", ch);
        }
    }
    return true;
}
