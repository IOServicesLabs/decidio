// Several boards in one home. Every board says who it is on the local network a few times a second-ish (one UDP
// broadcast every 2 s, and at once when a rule changes). Boards that were given the same group secret also say which
// of their rules are on, signed with that secret, so a rule on one board can depend on a rule of another
// ({"q":"board:decidio-ab12","o":"occupied"}) and one board's page can show and set up all of them (web.c forwards
// /api/via/<board>/... to that board). Nothing leaves the local network and there is no central board: each one keeps
// working on its own when the others are off.
//
// A broadcast:  decidio1 <16 hex of HMAC-SHA256(key, json), or -> <json>
//   {"h":"decidio-ab12","room":"Kitchen","fw":"0.3.0","boot":123,"seq":7,"rules":[["occupied",1]]}
// key = SHA-256("decidio-group:" + secret). Rule states are only sent by, and only believed from, boards in the group.
// A newer "seq" is required within one "boot", so a recorded packet cannot be played again later in the same run.
// The secret itself crosses the network once, in clear, when a board is added from another board's page: like the
// rest of the page, this trusts your own network.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "rt.h"

#define HOME_PORT 42170
#define MAX_PEER 8
#define FRESH_US (9 * 1000000LL)

typedef struct {
    char host[20], room[24], ip[16], fw[12];
    bool member;                // same group, and its signature checked out
    uint32_t boot, seq;
    int64_t seen_us;
    int n;
    uint16_t on;
    char rule[RT_MAX_ACT][RT_ID_LEN];
} peer_t;

static const char *TAG = "home";
static peer_t *s_peer;
static SemaphoreHandle_t s_lock;
static char s_room[24], s_secret[40], s_token[33];
static uint8_t s_key[32];
static bool s_grouped;
static uint32_t s_boot, s_seq;
static volatile bool s_changed;
static int s_fd = -1;
static char *s_out, *s_in;        // 1300 and 1400 bytes, in external memory

static void tag_of(const char *json, size_t n, char out[17])
{
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), s_key, 32, (const uint8_t *)json, n, mac);
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 2, 3, "%02x", mac[i]);
    }
}

// read the room and the group secret from the settings (not from the home task: its stack is in external memory)
void rt_home_reload(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rt_store_get("room", s_room, sizeof(s_room));
    s_grouped = rt_store_get("group", s_secret, sizeof(s_secret)) && s_secret[0];
    s_token[0] = 0;
    if (s_grouped) {
        char seed[64];
        const int n = snprintf(seed, sizeof(seed), "decidio-group:%s", s_secret);
        mbedtls_sha256((const uint8_t *)seed, n, s_key, 0);
        char t[17];
        tag_of("api", 3, t);
        snprintf(s_token, sizeof(s_token), "%s", t);
    }
    for (int i = 0; s_peer && i < MAX_PEER; i++) {
        s_peer[i].member = false;                          // believed again when its next signed broadcast arrives
    }
    xSemaphoreGive(s_lock);
    s_changed = true;
}

bool rt_home_token_ok(const char *tok)
{
    return s_grouped && tok && s_token[0] && !strcmp(tok, s_token);
}

const char *rt_home_token(void)
{
    return s_token;
}

const char *rt_home_secret(void)
{
    return s_grouped ? s_secret : "";
}

static peer_t *find(const char *host)                      // by name, or by room
{
    for (int i = 0; s_peer && i < MAX_PEER; i++) {
        if (s_peer[i].host[0] && (!strcmp(s_peer[i].host, host) || (s_peer[i].room[0] && !strcasecmp(s_peer[i].room, host)))) {
            return &s_peer[i];
        }
    }
    return NULL;
}

// is that rule on, on that board? 1 / 0, or < 0 when the board is not in the group or has not been heard lately
float rt_peer_value(const char *host, const char *rule)
{
    float v = -1;
    if (!s_lock || !s_peer) {
        return v;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const peer_t *p = find(host);
    if (p && p->member && esp_timer_get_time() - p->seen_us < FRESH_US) {
        for (int i = 0; i < p->n; i++) {
            if (!strcmp(p->rule[i], rule)) {
                v = (p->on >> i) & 1;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return v;
}

bool rt_home_peer_ip(const char *host, char *ip, size_t cap)
{
    bool ok = false;
    if (!s_lock || !s_peer) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const peer_t *p = find(host);
    if (p && esp_timer_get_time() - p->seen_us < 4 * FRESH_US) {
        snprintf(ip, cap, "%s", p->ip);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void rt_home_changed(void)
{
    s_changed = true;
    rt_dev_wake();
}

static bool word(const char *s, size_t max)
{
    return s && s[0] && strlen(s) <= max && !strpbrk(s, "\"\\");
}

static void heard(char *msg, int n, const char *from)
{
    msg[n] = 0;
    if (n < 20 || strncmp(msg, "decidio1 ", 9)) {
        return;
    }
    char *tag = msg + 9, *js = strchr(tag, ' ');
    if (!js) {
        return;
    }
    *js++ = 0;
    char want[17];
    bool signed_ok = false;
    if (s_grouped && strlen(tag) == 16) {
        tag_of(js, strlen(js), want);
        signed_ok = !strcmp(want, tag);
    }
    cJSON *j = cJSON_Parse(js);
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(j, "h"), *room = cJSON_GetObjectItemCaseSensitive(j, "room");
    const cJSON *fw = cJSON_GetObjectItemCaseSensitive(j, "fw"), *boot = cJSON_GetObjectItemCaseSensitive(j, "boot");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(j, "seq"), *rules = cJSON_GetObjectItemCaseSensitive(j, "rules");
    if (!cJSON_IsString(h) || !word(h->valuestring, 19) || !strcmp(h->valuestring, rt_wifi_host()) || !cJSON_IsNumber(boot) ||
        !cJSON_IsNumber(seq)) {
        cJSON_Delete(j);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    peer_t *p = find(h->valuestring);
    if (p && strcmp(p->host, h->valuestring)) {
        p = NULL;                                          // found by room name only: another board
    }
    if (!p) {
        int oldest = 0;
        for (int i = 0; i < MAX_PEER; i++) {
            if (!s_peer[i].host[0]) {
                oldest = i;
                break;
            }
            if (s_peer[i].seen_us < s_peer[oldest].seen_us) {
                oldest = i;
            }
        }
        p = &s_peer[oldest];
        memset(p, 0, sizeof(*p));
        snprintf(p->host, sizeof(p->host), "%s", h->valuestring);
    }
    const uint32_t b = (uint32_t)boot->valuedouble, q = (uint32_t)seq->valuedouble;
    if (!(b == p->boot && q <= p->seq)) {                  // else: an old packet again
        p->boot = b;
        p->seq = q;
        p->seen_us = esp_timer_get_time();
        p->member = signed_ok;
        snprintf(p->ip, sizeof(p->ip), "%s", from);
        snprintf(p->room, sizeof(p->room), "%s", cJSON_IsString(room) && word(room->valuestring, 23) ? room->valuestring : "");
        snprintf(p->fw, sizeof(p->fw), "%s", cJSON_IsString(fw) && word(fw->valuestring, 11) ? fw->valuestring : "");
        p->n = 0;
        p->on = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, rules) {
            const cJSON *name = cJSON_GetArrayItem(r, 0), *on = cJSON_GetArrayItem(r, 1);
            if (signed_ok && p->n < RT_MAX_ACT && cJSON_IsString(name) && word(name->valuestring, RT_ID_LEN - 1)) {
                snprintf(p->rule[p->n], RT_ID_LEN, "%s", name->valuestring);
                if (cJSON_IsNumber(on) && on->valueint) {
                    p->on |= 1u << p->n;
                }
                p->n++;
            }
        }
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);
}

static void say(void)
{
    char *msg = s_out;
    char js[1200];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = snprintf(js, sizeof(js), "{\"h\":\"%s\",\"room\":\"%s\",\"fw\":\"%s\",\"boot\":%lu,\"seq\":%lu,\"rules\":", rt_wifi_host(), s_room,
                     RT_VERSION, (unsigned long)s_boot, (unsigned long)++s_seq);
    const bool grouped = s_grouped;
    xSemaphoreGive(s_lock);
    const int k = grouped ? rt_actions_brief(js + n, sizeof(js) - n - 2) : snprintf(js + n, sizeof(js) - n - 2, "[]");
    if (k < 0) {
        return;
    }
    n += k;
    js[n++] = '}';
    js[n] = 0;
    char tag[17] = "-";
    if (grouped) {
        tag_of(js, n, tag);
    }
    const int len = snprintf(msg, 1300, "decidio1 %s %s", tag, js);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(HOME_PORT), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        to.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;   // this network's broadcast address
    }
    if (sendto(s_fd, msg, len, 0, (const struct sockaddr *)&to, sizeof(to)) < 0) {
        close(s_fd);                                       // the network went away: open it again next time
        s_fd = -1;
    }
}

static void home_task(void *arg)
{
    char *in = s_in;
    int64_t last = 0;
    for (;;) {
        rt_dev_step(250);
        if (!rt_wifi_online()) {
            if (s_fd >= 0) {
                close(s_fd);
                s_fd = -1;
            }
            continue;
        }
        if (s_fd < 0) {
            s_fd = socket(AF_INET, SOCK_DGRAM, 0);
            const struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(HOME_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
            const int yes = 1;
            if (s_fd >= 0) {
                setsockopt(s_fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
                setsockopt(s_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                if (bind(s_fd, (const struct sockaddr *)&a, sizeof(a)) != 0) {
                    close(s_fd);
                    s_fd = -1;
                }
            }
            if (s_fd < 0) {
                continue;
            }
        }
        for (int burst = 0; burst < 8; burst++) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = recvfrom(s_fd, in, 1399, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if (n <= 0) {
                break;
            }
            char ip[16];
            inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
            heard(in, n, ip);
        }
        const int64_t now = esp_timer_get_time();
        if (s_changed || now - last > 2000000) {
            s_changed = false;
            last = now;
            say();
        }
    }
}

void rt_home_start(void)
{
    if (s_peer) {
        return;
    }
    s_peer = heap_caps_calloc_prefer(MAX_PEER, sizeof(peer_t), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    s_out = heap_caps_malloc_prefer(2700, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!s_peer || !s_out) {
        ESP_LOGE(TAG, "no memory for the list of boards");
        s_peer = NULL;
        return;
    }
    s_in = s_out + 1300;
    s_boot = esp_random();
    rt_home_reload();
    // the stack lives in external memory when there is some (internal memory is scarce); this task never touches flash
    if (xTaskCreatePinnedToCoreWithCaps(home_task, "home", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS &&
        xTaskCreatePinnedToCore(home_task, "home", 5120, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the home task");
    }
}

// {"room":"Kitchen"} | {"group":"secret"} ("" leaves the group)
esp_err_t rt_home_set(const cJSON *j, char *why, size_t cap)
{
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(j, "room"), *group = cJSON_GetObjectItemCaseSensitive(j, "group");
    if (cJSON_IsString(room)) {
        if (strlen(room->valuestring) > 23 || strpbrk(room->valuestring, "\"\\")) {
            snprintf(why, cap, "room: up to 23 characters");
            return ESP_ERR_INVALID_ARG;
        }
        rt_store_set("room", room->valuestring);
    }
    if (cJSON_IsString(group)) {
        const size_t n = strlen(group->valuestring);
        if ((n && n < 8) || n > 39 || strpbrk(group->valuestring, "\"\\")) {
            snprintf(why, cap, "group: a secret of 8 to 39 characters (empty leaves the group)");
            return ESP_ERR_INVALID_ARG;
        }
        rt_store_set("group", group->valuestring);
    }
    rt_home_reload();
    return ESP_OK;
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

int rt_home_json(char *dst, size_t cap)
{
    int n = 0;
    if (!s_lock) {
        return snprintf(dst, cap, "{\"host\":\"%s\",\"room\":\"\",\"group\":false,\"rules\":[],\"boards\":[]}", rt_wifi_host());
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ADD("{\"host\":\"%s\",\"room\":\"%s\",\"group\":%s,\"rules\":", rt_wifi_host(), s_room, s_grouped ? "true" : "false");
    xSemaphoreGive(s_lock);
    const int k = rt_actions_brief(dst + n, n < (int)cap ? cap - n : 0);      // this board's own rules
    n = k < 0 ? (int)cap : n + k;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ADD(",\"boards\":[");
    const int64_t now = esp_timer_get_time();
    bool any = false;
    for (int i = 0; s_peer && i < MAX_PEER; i++) {
        const peer_t *p = &s_peer[i];
        if (!p->host[0] || now - p->seen_us > 60 * 1000000LL) {
            continue;
        }
        ADD("%s{\"host\":\"%s\",\"room\":\"%s\",\"ip\":\"%s\",\"fw\":\"%s\",\"member\":%s,\"fresh\":%s,\"rules\":[", any ? "," : "", p->host, p->room,
            p->ip, p->fw, p->member ? "true" : "false", now - p->seen_us < FRESH_US ? "true" : "false");
        for (int k = 0; k < p->n; k++) {
            ADD("%s[\"%s\",%d]", k ? "," : "", p->rule[k], (p->on >> k) & 1);
        }
        ADD("]}");
        any = true;
    }
    xSemaphoreGive(s_lock);
    ADD("]}");
    return n < (int)cap ? n : -1;
}
