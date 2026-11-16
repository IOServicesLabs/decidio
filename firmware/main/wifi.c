// Wi-Fi: joins your network, or opens a setup access point when it has no working name and password.
//
//   no credentials            -> setup access point "decidio-XXXX" (open), every address leads to the setup page
//   credentials, never joined -> tries 4 times, then opens the setup access point and keeps trying every 30 s
//                                while nobody is using it
//   joined once this boot     -> keeps retrying every 10 s (a router restart must not need a visit)
//   two minutes after joining -> the setup access point closes
//
// The driver is shared with radio.c. `wifi off` (console) stores "stay off": the board then never transmits.
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "rt.h"

static const char *TAG = "wifi";

static struct {
    bool driver, wanted, ap_on, online, ever_online, sntp;
    int fails, reason, ap_clients;
    char ssid[33], pass[65], ip[16], host[20];
    uint8_t channel;
} s;
static esp_timer_handle_t s_retry, s_ap_off;
static TaskHandle_t s_dns;

// the board's name, decidio-<last 4 hex of the MAC>; known before Wi-Fi starts (a low-power wake sends with it)
const char *rt_wifi_host(void)
{
    if (!s.host[0]) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s.host, sizeof(s.host), "decidio-%02x%02x", mac[4], mac[5]);
    }
    return s.host;
}
bool rt_wifi_online(void) { return s.online; }
bool rt_wifi_setup_open(void) { return s.ap_on; }
int rt_wifi_channel(void) { return s.online ? s.channel : 0; }

void rt_wifi_status(rt_wifi_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->state = !s.driver || !s.wanted ? "off" : s.online ? "online" : s.ssid[0] ? "connecting" : "setup";
    o->setup_open = s.ap_on;
    o->reason = s.reason;
    strlcpy(o->ssid, s.ssid, sizeof(o->ssid));
    strlcpy(o->ip, s.ip, sizeof(o->ip));
    strlcpy(o->host, s.host, sizeof(o->host));
    wifi_ap_record_t ap;
    if (s.online && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        o->rssi = ap.rssi;
        o->channel = ap.primary;
    }
}

// Every name asked for on the setup access point resolves to the board, so a phone opens the setup page by itself.
static void dns_task(void *arg)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "dns socket");
        vTaskDelete(NULL);
    }
    static uint8_t b[320];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(fd, b, sizeof(b) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 17 || !s.ap_on || (b[2] & 0x80) || b[4] != 0 || b[5] != 1) {     // one question, not a reply
            continue;
        }
        int p = 12;
        while (p < n && b[p]) {
            p += b[p] + 1;
        }
        p += 5;                                                                    // 0, type, class
        if (p > n) {
            continue;
        }
        b[2] = 0x81;
        b[3] = 0x80;
        b[6] = 0; b[7] = 1; b[8] = b[9] = b[10] = b[11] = 0;
        static const uint8_t ans[16] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
        memcpy(b + p, ans, sizeof(ans));
        sendto(fd, b, p + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
}

static void ap_start(void)
{
    if (s.ap_on) {
        return;
    }
    wifi_config_t c = { .ap = { .channel = 1, .authmode = WIFI_AUTH_OPEN, .max_connection = 2 } };
    strlcpy((char *)c.ap.ssid, s.host, sizeof(c.ap.ssid));
    c.ap.ssid_len = strlen(s.host);
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &c) != ESP_OK) {
        ESP_LOGE(TAG, "setup access point failed");
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    s.ap_on = true;
    if (!s_dns) {
        xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 3, &s_dns, 0);
    }
    printf("{\"wifi\":{\"state\":\"setup\",\"join\":\"%s\",\"open\":\"http://192.168.4.1\"}}\n", s.host);
}

static void ap_stop(void *arg)
{
    if (s.ap_on) {
        s.ap_on = false;
        s.ap_clients = 0;
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
}

static void join(void)
{
    wifi_config_t c = { .sta = { .scan_method = WIFI_ALL_CHANNEL_SCAN, .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                                 .pmf_cfg = { .capable = true } } };
    strlcpy((char *)c.sta.ssid, s.ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, s.pass, sizeof(c.sta.password));
    if (s.fails == 0 && rt_power_fast_ap(c.sta.bssid, &c.sta.channel)) {   // low power: straight to last time's router
        c.sta.bssid_set = true;
        c.sta.scan_method = WIFI_FAST_SCAN;
    }
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
}

static void retry(void *arg)
{
    if (!s.wanted || !s.ssid[0] || s.online) {
        return;
    }
    if (s.ap_on && s.ap_clients > 0 && s.fails >= 4) {      // somebody is on the setup page: do not hop channels
        esp_timer_start_once(s_retry, 30 * 1000000LL);
        return;
    }
    join();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s.online = false;
        s.ip[0] = 0;
        s.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        if (!s.wanted || !s.ssid[0]) {
            return;
        }
        s.fails++;
        if (!s.ever_online && s.fails >= 4) {
            ap_start();
        }
        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, (s.fails < 4 ? 1 : s.ap_on ? 30 : 10) * 1000000LL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s.ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        s.ap_clients = s.ap_clients > 0 ? s.ap_clients - 1 : 0;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&e->ip_info.ip));
        wifi_ap_record_t ap;
        s.channel = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.primary : 0;
        if (s.channel) {
            rt_power_joined(ap.bssid, s.channel);
        }
        s.online = s.ever_online = true;
        s.fails = s.reason = 0;
        if (!s.sntp) {                                       // the clock, for rules with hours and for alert times
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            s.sntp = esp_netif_sntp_init(&sc) == ESP_OK;
        }
        if (s.ap_on) {
            esp_timer_stop(s_ap_off);
            esp_timer_start_once(s_ap_off, 120 * 1000000LL);
        }
        printf("{\"wifi\":{\"state\":\"online\",\"ip\":\"%s\",\"open\":\"http://%s.local\"}}\n", s.ip, s.host);
    }
}

// the driver, started once; radio.c calls this too (then the board only listens until `wifi` is set up)
esp_err_t rt_wifi_driver(void)
{
    if (s.driver) {
        return ESP_OK;
    }
    rt_wifi_host();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    if (sta) {
        esp_netif_set_hostname(sta, s.host);
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    const esp_timer_create_args_t ra = { .callback = retry, .name = "wifi_retry" };
    const esp_timer_create_args_t aa = { .callback = ap_stop, .name = "wifi_ap_off" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ra, &s_retry), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&aa, &s_ap_off), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    esp_wifi_set_ps(WIFI_PS_NONE);
    s.driver = true;
    return ESP_OK;
}

// at boot: join the stored network, or open the setup access point
void rt_wifi_boot(void)
{
    char off[4];
    if (rt_store_get("wifi_off", off, sizeof(off))) {
        printf("{\"wifi\":{\"state\":\"off\"}}\n");
        return;
    }
    const esp_err_t err = rt_wifi_driver();
    if (err != ESP_OK) {
        printf("{\"error\":\"wifi: %s\"}\n", esp_err_to_name(err));
        return;
    }
    s.wanted = true;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s.host);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    rt_store_get("wifi_ssid", s.ssid, sizeof(s.ssid));
    rt_store_get("wifi_pass", s.pass, sizeof(s.pass));
    if (s.ssid[0]) {
        join();
    } else {
        ap_start();
    }
    rt_web_start();
}

// a new network name and password (from the setup page or the console): stored, then tried at once
esp_err_t rt_wifi_set(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(rt_store_set("wifi_ssid", ssid), TAG, "store");
    ESP_RETURN_ON_ERROR(rt_store_set("wifi_pass", pass ? pass : ""), TAG, "store");
    rt_store_set("wifi_off", NULL);
    strlcpy(s.ssid, ssid, sizeof(s.ssid));
    strlcpy(s.pass, pass ? pass : "", sizeof(s.pass));
    if (!s.wanted) {
        rt_wifi_boot();
        return ESP_OK;
    }
    s.fails = 0;
    s.ever_online = false;
    esp_timer_stop(s_retry);
    if (s.online) {
        esp_wifi_disconnect();              // the disconnect event joins the new network a second later
    } else {
        esp_wifi_disconnect();
        join();
    }
    return ESP_OK;
}

// forget the network; `off` also keeps Wi-Fi off after the next boot
void rt_wifi_forget(bool off)
{
    rt_store_set("wifi_ssid", NULL);
    rt_store_set("wifi_pass", NULL);
    rt_store_set("wifi_off", off ? "1" : NULL);
    s.ssid[0] = s.pass[0] = 0;
    if (s.driver && s.wanted) {
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
        if (off) {
            ap_stop(NULL);
            s.wanted = false;
        } else {
            ap_start();
        }
    }
}

// networks in range, strongest first, as a JSON array into dst (for the setup page: these do carry names)
int rt_wifi_scan_json(char *dst, size_t cap)
{
    static wifi_ap_record_t ap[16];
    uint16_t n = 16;
    wifi_scan_config_t sc = { 0 };
    if (!s.driver || esp_wifi_scan_start(&sc, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) {
        n = 0;
    }
    int len = snprintf(dst, cap, "[");
    for (int i = 0; i < n && len < (int)cap - 120; i++) {
        if (!ap[i].ssid[0]) {
            continue;
        }
        len += snprintf(dst + len, cap - len, "%s{\"ssid\":\"", len > 1 ? "," : "");
        for (const uint8_t *c = ap[i].ssid; *c && len < (int)cap - 40; c++) {      // JSON string: escape " \ and controls
            if (*c == '"' || *c == '\\') {
                dst[len++] = '\\';
                dst[len++] = *c;
            } else if (*c >= 0x20) {
                dst[len++] = *c;
            }
        }
        len += snprintf(dst + len, cap - len, "\",\"rssi\":%d,\"lock\":%s}", ap[i].rssi,
                        ap[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }
    len += snprintf(dst + len, cap - len, "]");
    return len;
}
