// Several boards in one home. Every board says who it is on the local network a few times a second-ish (one UDP
// broadcast every 2 s, and at once when a rule changes). Boards that were given the same group secret also say which
// of their rules are on, signed with that secret, so a rule on one board can depend on a rule of another
// ({"q":"board:decidio-ab12","o":"occupied"}) and one board's page can show and set up all of them (web.c forwards
// /api/via/<board>/... to that board). Nothing leaves the local network and there is no central board: each one keeps
// working on its own when the others are off.
//
// Alerts of a group go out from one board, so that two boards that see the same thing do not both send it, and so that
// alert.c can put the alerts of all the boards into one message. The sender is the board with the lowest name among the
// members heard lately that have somewhere to send alerts ("al":1), this board included; each board works this out
// alone from the same broadcasts. A board in low power (power.c) says "al":0: it sleeps, so it is never the sender.
// Another board gives the sender its alert (one UDP message straight to it, signed) and waits up to 3 s for the sender
// to say it has it; with no answer, it sends the alert itself.
//
// A broadcast:  decidio1 <16 hex of HMAC-SHA256(key, json), or -> <json>
//   {"h":"decidio-ab12","room":"Kitchen","fw":"0.3.0","boot":123,"seq":7,"al":1,"rules":[["occupied",1]]}
// An alert for the sender (signed):  {"k":"a","h":"decidio-ab12","room":"Kitchen","boot":123,"id":4,"r":"person",
//   "t":"Someone is at the door","on":1,"p":0.91,"ph":0,"u":0,"tm":1763300000}
// Its answer (signed):  {"k":"k","h":"<sender>","to":"decidio-ab12","boot":123,"id":4}
// key = SHA-256("decidio-group:" + secret). Rule states are only sent by, and only believed from, boards in the group.
// A newer "seq" is required within one "boot", so a recorded packet cannot be played again later in the same run.
// The secret itself crosses the network once, in clear, when a board is added from another board's page: like the
// rest of the page, this trusts your own network.
//
// A board in a group also sends each broadcast straight to the boards near it with ESP-NOW (a Wi-Fi frame from radio to
// radio, not through the router). It arrives in a few milliseconds, so a rule that depends on another board follows it
// almost at once. The same broadcast still goes through the router for boards that are too far apart. Only signed
// ESP-NOW frames are believed: a radio that is not on your network can send them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
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
    char host[20], room[24], ip[16], fw[12], id[10];
    int64_t now_us;             // last heard straight from its radio (ESP-NOW), 0 = never
    int8_t rssi;                // the signal of that frame
    int64_t first_us;           // when the first copy of its last broadcast came, and was it the copy from the radio
    bool first_now;
    int16_t lead_ms;            // how long the copy from the radio came before the copy through the router (< 0: after)
    bool member;                // same group, and its signature checked out
    bool alerts;                // it has somewhere to send alerts
    uint32_t boot, seq;
    uint32_t aboot, aid;        // the last alert it gave this board
    int64_t seen_us;
    int n;
    uint16_t on;
    char rule[RT_MAX_ACT][RT_ID_LEN];
} peer_t;

static const char *TAG = "home";
static peer_t *s_peer;
static SemaphoreHandle_t s_lock;
static char s_room[24], s_secret[40], s_token[33];
static bool s_open;                // the group has no password: its secret is a random key that a board made
static uint8_t s_key[32];
static bool s_grouped;
static uint32_t s_boot, s_seq;
static volatile bool s_changed;
static int s_fd = -1;
static SemaphoreHandle_t s_ack;   // given when the sender says it has our alert
static volatile uint32_t s_ack_want;
static uint32_t s_aid;
static char *s_out, *s_in;        // 1300 and 1400 bytes, in external memory
static char s_fwid[10];           // which firmware build this is (the start of its SHA-256)

typedef struct {
    uint16_t len;
    int8_t rssi;
    char *msg;
} now_rx_t;
static QueueHandle_t s_nowq;      // frames from the radios of other boards, for the home task
static bool s_now;
static uint32_t s_now_tx, s_now_rx;

static void tag_of(const char *json, size_t n, char out[17])
{
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), s_key, 32, (const uint8_t *)json, n, mac);
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 2, 3, "%02x", mac[i]);
    }
}

static int frame(char *msg, size_t cap, const char *js)   // "decidio1 <tag> <json>", signed
{
    char tag[17];
    tag_of(js, strlen(js), tag);
    return snprintf(msg, cap, "decidio1 %s %s", tag, js);
}

static void send_to(int fd, const char *ip, const char *msg, int len)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(HOME_PORT) };
    if (inet_aton(ip, &to.sin_addr)) {
        sendto(fd, msg, len, 0, (const struct sockaddr *)&to, sizeof(to));
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
    char open[4] = "";
    s_open = s_grouped && rt_store_get("group_open", open, sizeof(open)) && open[0] == '1';
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
    rt_web_auth_reload();                                  // the group password opens the page
}

const char *rt_home_fwid(void)
{
    if (!s_fwid[0]) {
        esp_app_get_elf_sha256(s_fwid, 9);                // 8 hex characters
    }
    return s_fwid;
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

bool rt_home_open(void)
{
    return s_grouped && s_open;
}

// start a group that has no password, when this board is not in a group: the secret is a random key
void rt_home_begin(void)
{
    if (s_grouped) {
        return;
    }
    char key[33];
    for (int i = 0; i < 4; i++) {
        snprintf(key + 8 * i, 9, "%08lx", (unsigned long)esp_random());
    }
    rt_store_set("group", key);
    rt_store_set("group_open", "1");
    rt_home_reload();
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

const char *rt_home_label(void)
{
    if (!s_lock && !s_room[0]) {
        rt_store_get("room", s_room, sizeof(s_room));     // low power: an alert before the home task started
    }
    return s_room[0] ? s_room : rt_wifi_host();
}

// which board sends the group's alerts. true: another board, at ip. false: this board (or no group).
bool rt_home_sender(char *ip, size_t ip_cap, char *host, size_t host_cap)
{
    if (!s_lock || !s_peer || !s_grouped) {
        return false;
    }
    const char *best = rt_alert_ready() && !rt_power_on() ? rt_wifi_host() : NULL;   // a board in low power sleeps
    const peer_t *bp = NULL;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_PEER; i++) {
        const peer_t *p = &s_peer[i];
        if (p->host[0] && p->member && p->alerts && now - p->seen_us < FRESH_US && (!best || strcmp(p->host, best) < 0)) {
            best = p->host;
            bp = p;
        }
    }
    if (bp) {
        snprintf(ip, ip_cap, "%s", bp->ip);
        snprintf(host, host_cap, "%s", bp->host);
    }
    xSemaphoreGive(s_lock);
    return bp != NULL;
}

// give the sender an alert (from the alert task): up to 3 tries, 1 s apart. true when it said it has it.
bool rt_home_handoff(const cJSON *alert, const char *ip)
{
    if (!s_grouped || !s_ack) {
        return false;
    }
    cJSON *m = cJSON_Duplicate(alert, true);
    char *msg = malloc(1400);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    bool ok = false;
    if (m && msg && fd >= 0) {
        if (++s_aid == 0) {
            s_aid = 1;
        }
        const uint32_t id = s_aid;
        cJSON_AddStringToObject(m, "k", "a");
        cJSON_AddStringToObject(m, "h", rt_wifi_host());
        cJSON_AddStringToObject(m, "room", s_room);
        cJSON_AddNumberToObject(m, "boot", s_boot);
        cJSON_AddNumberToObject(m, "id", id);
        char *js = cJSON_PrintUnformatted(m);
        if (js && strlen(js) < 1300) {
            const int len = frame(msg, 1400, js);
            xSemaphoreTake(s_ack, 0);                      // an answer that came too late last time
            s_ack_want = id;
            for (int t = 0; t < 3 && !ok; t++) {
                send_to(fd, ip, msg, len);
                ok = xSemaphoreTake(s_ack, pdMS_TO_TICKS(1000)) == pdTRUE;
            }
            s_ack_want = 0;
        }
        free(js);
    }
    if (fd >= 0) {
        close(fd);
    }
    free(msg);
    cJSON_Delete(m);
    return ok;
}

// the other members heard lately: their addresses and names
int rt_home_members(char ips[][16], char hosts[][20], int max)
{
    int n = 0;
    if (!s_lock || !s_peer || !s_grouped) {
        return 0;
    }
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_PEER && n < max; i++) {
        if (s_peer[i].host[0] && s_peer[i].member && now - s_peer[i].seen_us < FRESH_US) {
            snprintf(ips[n], 16, "%s", s_peer[i].ip);
            snprintf(hosts[n], 20, "%s", s_peer[i].host);
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

// the other members heard lately, with the firmware build each one runs ("" = an older firmware that does not say)
int rt_home_fleet(char ips[][16], char hosts[][20], char ids[][10], int max)
{
    int n = 0;
    if (!s_lock || !s_peer || !s_grouped) {
        return 0;
    }
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_PEER && n < max; i++) {
        if (s_peer[i].host[0] && s_peer[i].member && now - s_peer[i].seen_us < FRESH_US) {
            snprintf(ips[n], 16, "%s", s_peer[i].ip);
            snprintf(hosts[n], 20, "%s", s_peer[i].host);
            snprintf(ids[n], 10, "%s", s_peer[i].id);
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
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

// an alert from another board for this one to send ("a"), or the sender's answer to ours ("k"); the signature is good
static void alert_msg(const cJSON *j, const char *kind, const char *from)
{
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(j, "h"), *boot = cJSON_GetObjectItemCaseSensitive(j, "boot");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    if (!cJSON_IsString(h) || !word(h->valuestring, 19) || !strcmp(h->valuestring, rt_wifi_host()) || !cJSON_IsNumber(boot) ||
        !cJSON_IsNumber(id)) {
        return;
    }
    const uint32_t b = (uint32_t)boot->valuedouble, n = (uint32_t)id->valuedouble;
    if (!strcmp(kind, "k")) {
        const cJSON *to = cJSON_GetObjectItemCaseSensitive(j, "to");
        if (cJSON_IsString(to) && !strcmp(to->valuestring, rt_wifi_host()) && b == s_boot && n && n == s_ack_want && s_ack) {
            s_ack_want = 0;
            xSemaphoreGive(s_ack);
        }
        return;
    }
    if (strcmp(kind, "a")) {
        return;
    }
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "r"), *t = cJSON_GetObjectItemCaseSensitive(j, "t");
    if (!cJSON_IsString(r) || !word(r->valuestring, RT_ID_LEN - 1) || !cJSON_IsString(t) || strlen(t->valuestring) > 199) {
        return;
    }
    // A newer alert: take it. The last one again (our answer was lost): answer again, but send it only once.
    // An older one (a recorded packet played again): ignore it.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    peer_t *pe = find(h->valuestring);
    const bool known = pe && !strcmp(pe->host, h->valuestring);
    const bool repeat = known && b == pe->aboot && n == pe->aid, old = known && b == pe->aboot && n < pe->aid;
    if (known && !repeat && !old) {
        pe->aboot = b;
        pe->aid = n;
    }
    xSemaphoreGive(s_lock);
    if (!known || old) {
        return;
    }
    if (!repeat) {
        const cJSON *room = cJSON_GetObjectItemCaseSensitive(j, "room"), *p = cJSON_GetObjectItemCaseSensitive(j, "p");
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on"), *ph = cJSON_GetObjectItemCaseSensitive(j, "ph");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "u"), *tm = cJSON_GetObjectItemCaseSensitive(j, "tm");
        const char *label = cJSON_IsString(room) && room->valuestring[0] && word(room->valuestring, 23) ? room->valuestring
                                                                                                        : h->valuestring;
        if (!rt_alert_from_peer(h->valuestring, label, from, r->valuestring, t->valuestring,
                                cJSON_IsNumber(on) && on->valueint, cJSON_IsNumber(p) ? (float)p->valuedouble : 0,
                                cJSON_IsNumber(ph) && ph->valueint, cJSON_IsNumber(u) && u->valueint,
                                cJSON_IsNumber(tm) ? (long long)tm->valuedouble : 0)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (pe->aboot == b && pe->aid == n) {
                pe->aid = n - 1;                           // not taken: no answer, so that board sends it itself
            }
            xSemaphoreGive(s_lock);
            return;
        }
    }
    char js[120], msg[160];
    snprintf(js, sizeof(js), "{\"k\":\"k\",\"h\":\"%s\",\"to\":\"%s\",\"boot\":%lu,\"id\":%lu}", rt_wifi_host(), h->valuestring,
             (unsigned long)b, (unsigned long)n);
    const int len = frame(msg, sizeof(msg), js);
    if (s_fd >= 0) {
        send_to(s_fd, from, msg, len);
    }
}

// from: the address it came from on the network, or NULL for a frame straight from a radio (ESP-NOW)
static void heard(char *msg, int n, const char *from, int rssi)
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
    if (!from && !signed_ok) {
        return;                                            // any radio in range can send a frame: only the group's count
    }
    cJSON *j = cJSON_Parse(js);
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(j, "k");
    if (cJSON_IsString(kind)) {
        if (signed_ok && from) {
            alert_msg(j, kind->valuestring, from);
        }
        cJSON_Delete(j);
        return;
    }
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
    const cJSON *jip = cJSON_GetObjectItemCaseSensitive(j, "ip");
    struct in_addr a4;
    const char *ip = from ? from
                     : cJSON_IsString(jip) && strlen(jip->valuestring) < 16 && inet_aton(jip->valuestring, &a4) ? jip->valuestring : NULL;
    if (!from) {
        p->now_us = esp_timer_get_time();                  // the same broadcast also comes through the router
        p->rssi = (int8_t)rssi;
        s_now_rx++;
    }
    if (b == p->boot && q == p->seq) {                     // the second copy of a broadcast
        const int64_t d = (esp_timer_get_time() - p->first_us) / 1000;
        if (from && p->first_now) {
            p->lead_ms = (int16_t)(d > 30000 ? 30000 : d);
            snprintf(p->ip, sizeof(p->ip), "%s", from);    // the copy through the router gives the address
        } else if (!from && !p->first_now) {
            p->lead_ms = (int16_t)(d > 30000 ? -30000 : -d);
        }
    }
    if (!(b == p->boot && q <= p->seq)) {                  // else: an old packet again
        p->boot = b;
        p->seq = q;
        p->seen_us = esp_timer_get_time();
        p->first_us = p->seen_us;
        p->first_now = !from;
        p->member = signed_ok;
        const cJSON *al = cJSON_GetObjectItemCaseSensitive(j, "al");
        p->alerts = signed_ok && cJSON_IsNumber(al) && al->valueint;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
        snprintf(p->id, sizeof(p->id), "%s", cJSON_IsString(id) && word(id->valuestring, 9) ? id->valuestring : "");
        if (ip) {
            snprintf(p->ip, sizeof(p->ip), "%s", ip);
        }
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
    char js[1200], own[16] = "";
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(HOME_PORT), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        to.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;   // this network's broadcast address
        const struct in_addr me = { .s_addr = ip.ip.addr };
        inet_ntoa_r(me, own, sizeof(own));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = snprintf(js, sizeof(js),
                     "{\"h\":\"%s\",\"room\":\"%s\",\"ip\":\"%s\",\"fw\":\"%s\",\"id\":\"%s\",\"boot\":%lu,\"seq\":%lu,\"al\":%d,\"rules\":",
                     rt_wifi_host(), s_room, own, RT_VERSION, rt_home_fwid(), (unsigned long)s_boot, (unsigned long)++s_seq,
                     s_grouped && rt_alert_ready() && !rt_power_on());
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
    if (grouped && s_now && len <= ESP_NOW_MAX_DATA_LEN_V2) {  // first: straight to the radios of the boards in range
        static const uint8_t all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        if (esp_now_send(all, (const uint8_t *)msg, len) == ESP_OK) {
            s_now_tx++;
        }
    }
    if (sendto(s_fd, msg, len, 0, (const struct sockaddr *)&to, sizeof(to)) < 0) {
        close(s_fd);                                       // the network went away: open it again next time
        s_fd = -1;
    }
}

// Wi-Fi task: a frame from the radio of another board. The home task reads it.
static void now_rx(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 20 || len > 1399 || !s_nowq) {
        return;
    }
    now_rx_t m = { .len = (uint16_t)len, .rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0 };
    m.msg = heap_caps_malloc_prefer(len + 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!m.msg) {
        return;
    }
    memcpy(m.msg, data, len);
    if (xQueueSend(s_nowq, &m, 0) != pdTRUE) {
        free(m.msg);
        return;
    }
    rt_dev_wake();                                         // the home task: do not wait
}

// ESP-NOW on the channel of the joined network. Not in low power: the board is awake only for moments.
static void now_start(void)
{
    if (s_now || rt_power_on() || !s_nowq) {
        return;
    }
    if (esp_now_init() != ESP_OK) {
        return;
    }
    esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
    memset(p.peer_addr, 0xff, 6);
    if (esp_now_register_recv_cb(now_rx) != ESP_OK || esp_now_add_peer(&p) != ESP_OK) {
        esp_now_deinit();
        return;
    }
    s_now = true;
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
        now_start();
        now_rx_t m;
        for (int burst = 0; burst < 8 && s_nowq && xQueueReceive(s_nowq, &m, 0) == pdTRUE; burst++) {
            heard(m.msg, m.len, NULL, m.rssi);
            free(m.msg);
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
            heard(in, n, ip, 0);
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
    s_ack = xSemaphoreCreateBinary();
    s_nowq = xQueueCreate(6, sizeof(now_rx_t));
    rt_home_reload();
    rt_push_boot();                                        // this board got new firmware from its page: give it to the group
    // the stack lives in external memory when there is some (internal memory is scarce); this task never touches flash
    if (xTaskCreatePinnedToCoreWithCaps(home_task, "home", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS &&
        xTaskCreatePinnedToCore(home_task, "home", 5120, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the home task");
    }
}

// {"room":"Kitchen"} | {"group":"secret"} ("" leaves the group) | {"group":"key","open":true} (a group with no
// password: another board gives its random key) | {"start":true} (start a group with no password)
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
        rt_store_set("group_open", n && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "open")) ? "1" : "0");
    }
    rt_home_reload();
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "start"))) {
        rt_home_begin();
    }
    return ESP_OK;
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

int rt_home_json(char *dst, size_t cap)
{
    int n = 0;
    if (!s_lock) {
        return snprintf(dst, cap, "{\"host\":\"%s\",\"room\":\"\",\"group\":false,\"fw\":\"%s\",\"id\":\"%s\",\"rules\":[],\"boards\":[]}",
                        rt_wifi_host(), RT_VERSION, rt_home_fwid());
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ADD("{\"host\":\"%s\",\"room\":\"%s\",\"group\":%s,\"open\":%s,\"fw\":\"%s\",\"id\":\"%s\",\"radio\":{\"on\":%s,\"sent\":%lu,\"heard\":%lu},\"rules\":",
        rt_wifi_host(), s_room, s_grouped ? "true" : "false", s_grouped && s_open ? "true" : "false", RT_VERSION, rt_home_fwid(), s_now ? "true" : "false",
        (unsigned long)s_now_tx, (unsigned long)s_now_rx);
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
        const bool direct = p->now_us && now - p->now_us < FRESH_US;
        ADD("%s{\"host\":\"%s\",\"room\":\"%s\",\"ip\":\"%s\",\"fw\":\"%s\",\"id\":\"%s\",\"member\":%s,\"fresh\":%s,\"alerts\":%s,"
            "\"direct\":%s,\"rssi\":%d,\"lead\":%d,\"rules\":[",
            any ? "," : "", p->host, p->room, p->ip, p->fw, p->id, p->member ? "true" : "false",
            now - p->seen_us < FRESH_US ? "true" : "false", p->alerts ? "true" : "false", direct ? "true" : "false",
            direct ? p->rssi : 0, direct ? p->lead_ms : 0);
        for (int k = 0; k < p->n; k++) {
            ADD("%s[\"%s\",%d]", k ? "," : "", p->rule[k], (p->on >> k) & 1);
        }
        ADD("]}");
        any = true;
    }
    xSemaphoreGive(s_lock);
    char sip[16], shost[20] = "";                          // who sends the group's alerts ("" = nobody)
    rt_home_sender(sip, sizeof(sip), shost, sizeof(shost));
    ADD("],\"sender\":\"%s\",\"push\":", shost[0] ? shost : s_grouped && rt_alert_ready() && !rt_power_on() ? rt_wifi_host() : "");
    if (n < (int)cap) {
        n += rt_push_json(dst + n, cap - n);
    }
    ADD("}");
    return n < (int)cap ? n : -1;
}
