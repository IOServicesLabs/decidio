// Runs the rules after every decision: drives pins, prints event lines, queues alerts, keeps daily counts.
//
// The rules come with the model package. The web page can replace them (stored as "rules" in the settings); "reset"
// goes back to the package's own. Stored rules belong to the package they were saved with ("rules_for"): flashing
// another package over USB (an idea image, a new model) puts that package's own rules in charge again, while a model
// uploaded through the web page keeps them (web.c moves "rules_for"). What a rule is and when it is on: rules.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rt.h"

static rt_action_t s_act[RT_MAX_ACT], s_tmp[RT_MAX_ACT];
static rt_rule_state_t s_st[RT_MAX_ACT];
static int s_n;
static bool s_custom;
static SemaphoreHandle_t s_lock;
static int s_summary_min = 21 * 60, s_summary_day = -1;
static volatile bool s_settle;
static int64_t s_settle_us;

static bool pin_ok(int gpio)
{
    return gpio >= 0 && gpio < 64 && GPIO_IS_VALID_OUTPUT_GPIO(gpio) && rt_board_pin_free(gpio);
}

static void pins(bool attach)
{
    for (int i = 0; i < s_n; i++) {
        const rt_action_t *a = &s_act[i];
        if (!pin_ok(a->gpio)) {
            continue;
        }
        if (attach) {
            gpio_reset_pin(a->gpio);
            gpio_set_direction(a->gpio, GPIO_MODE_OUTPUT);
        }
        gpio_set_level(a->gpio, !a->level);              // off
    }
}

static void adopt(const rt_action_t *src, int n, bool custom)      // with the lock held
{
    pins(false);
    memcpy(s_act, src, sizeof(rt_action_t) * n);
    memset(s_st, 0, sizeof(s_st));
    s_n = n;
    s_custom = custom;
    pins(true);
    rt_dev_rules_reset();
    s_settle = true;
    rt_rules_refs_keep(s_act, s_n, g_model.act, g_model.n_act);
    rt_home_changed();
}

static int minute_of_day(int *day)
{
    const time_t t = time(NULL);
    if (t < 1600000000) {
        return -1;                                       // the clock has not been set from the network
    }
    struct tm tm;
    localtime_r(&t, &tm);
    if (day) {
        *day = tm.tm_yday;
    }
    return tm.tm_hour * 60 + tm.tm_min;
}

void rt_actions_clock(void)
{
    char v[40];
    if (rt_store_get("tz", v, sizeof(v))) {              // e.g. "<+01>-1": set by the web page from the browser's clock
        setenv("TZ", v, 1);
        tzset();
    }
    if (rt_store_get("summary_at", v, sizeof(v))) {
        s_summary_min = atoi(v);
    }
}

void rt_actions_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    rt_actions_clock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = -1;
    char *blob = rt_store_dup("rules", NULL);
    char owner[16];
    if (blob && rt_store_get("rules_for", owner, sizeof(owner)) && strtoul(owner, NULL, 10) != g_model.crc) {
        free(blob);                                      // saved for a package that is no longer on the board
        blob = NULL;
        rt_store_set("rules", NULL);
        rt_store_set("rules_for", NULL);
    }
    if (blob) {
        cJSON *arr = cJSON_Parse(blob);
        if (cJSON_IsArray(arr)) {
            n = rt_rules_parse(arr, &g_model, s_tmp, RT_MAX_ACT);
        }
        cJSON_Delete(arr);
        free(blob);
    }
    if (n >= 0) {
        adopt(s_tmp, n, true);
    } else {
        adopt(g_model.act, g_model.n_act, false);
    }
    xSemaphoreGive(s_lock);
}

// rules from the web page: a JSON array. Every rule must be valid for this model, or nothing changes.
esp_err_t rt_actions_set(const char *json)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!json || !json[0]) {
        rt_store_set("rules", NULL);
        rt_store_set("rules_for", NULL);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        adopt(s_tmp, rt_model_default_rules(s_tmp, RT_MAX_ACT), false);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > RT_MAX_ACT) {
        cJSON_Delete(arr);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = rt_rules_parse(arr, &g_model, s_tmp, RT_MAX_ACT);
    if (n != cJSON_GetArraySize(arr)) {
        err = ESP_ERR_NOT_FOUND;                         // a question or option this model does not have
    } else {
        for (int i = 0; i < n; i++) {
            if (s_tmp[i].gpio >= 0 && !pin_ok(s_tmp[i].gpio)) {
                err = ESP_ERR_NOT_SUPPORTED;             // a pin this board uses for something else
            }
        }
    }
    if (err == ESP_OK) {
        err = rt_store_set("rules", json[0] == '[' && json[1] == ']' ? "[ ]" : json);
    }
    if (err == ESP_OK) {
        char owner[16];
        snprintf(owner, sizeof(owner), "%lu", (unsigned long)g_model.crc);
        rt_store_set("rules_for", owner);
        adopt(s_tmp, n, true);
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(arr);
    return err;
}

// once a day: what the counting rules saw
static void summary(int minute, int day)
{
    if (minute < 0 || day == s_summary_day) {
        return;
    }
    if (s_summary_day < 0 && minute >= s_summary_min) {  // booted after today's time: wait for tomorrow
        s_summary_day = day;
        return;
    }
    if (minute < s_summary_min) {
        return;
    }
    s_summary_day = day;
    static char text[200];
    int n = snprintf(text, sizeof(text), "Today:");
    bool any = false;
    for (int i = 0; i < s_n; i++) {
        rt_rule_state_t *s = &s_st[i];
        if (!s_act[i].count) {
            continue;
        }
        any = true;
        if (n < (int)sizeof(text) - 1) {
            n += snprintf(text + n, sizeof(text) - n, " %s %lu times, %lu min;", s_act[i].name, (unsigned long)s->events,
                          (unsigned long)(s->on_s / 60));
        }
        s->events = s->on_s = 0;
    }
    if (any) {
        rt_alert_event("daily", text, true, 1, false, true);   // once a day: never held back
    }
}

void rt_actions_update(const rt_result_t *r, int64_t now)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;                                          // rules are being replaced: next decision
    }
    int day = 0;
    const int minute = minute_of_day(&day);
    const float radio = rt_radio_motion();
    if (s_settle) {                                      // new rules: give them 5 s to claim their devices
        s_settle = false;
        s_settle_us = now + 5000000;
    }
    for (int i = 0; i < s_n; i++) {
        const rt_action_t *a = &s_act[i];
        rt_rule_out_t o;
        rt_rule_step(a, &s_st[i], &g_model, r->probs, radio, now, minute, &o);
        if (!o.changed) {
            continue;
        }
        if (pin_ok(a->gpio)) {
            gpio_set_level(a->gpio, o.on ? a->level : !a->level);
        }
        printf("{\"event\":\"%s\",\"state\":\"%s\",\"p\":%.3f,\"ms\":%lld}\n", a->name, o.on ? "on" : "off", s_st[i].p,
               (long long)(now / 1000));
        if (o.notify) {
            rt_alert_event(a->name, a->text, o.on, s_st[i].p, a->photo, a->urgent);
        }
        if (a->dev >= 0) {
            rt_dev_rule(a->dev, i, a->dev_do, o.on);
        }
        rt_home_changed();
    }
    if (s_settle_us && now >= s_settle_us) {
        s_settle_us = 0;
        rt_dev_settle();
    }
    summary(minute, day);
    xSemaphoreGive(s_lock);
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

// which rules are on, for the other boards: [["name",1],...]
int rt_actions_brief(char *dst, size_t cap)
{
    int n = 0;
    ADD("[");
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_n; i++) {
            ADD("%s[\"%s\",%d]", i ? "," : "", s_act[i].name, s_st[i].active ? 1 : 0);
        }
        xSemaphoreGive(s_lock);
    }
    ADD("]");
    return n < (int)cap ? n : -1;
}

static uint32_t rules_crc(void)                          // with the lock held
{
    return esp_rom_crc32_le(0, (const uint8_t *)s_act, sizeof(rt_action_t) * s_n) ^ (uint32_t)s_n;
}

// low power (power.c): the rules' states go into memory that deep sleep keeps, and come back at the next wake
void rt_actions_save(rt_actions_keep_t *k)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    k->crc = rules_crc();
    k->n = s_n;
    k->summary_day = s_summary_day;
    memcpy(k->st, s_st, sizeof(rt_rule_state_t) * s_n);
    xSemaphoreGive(s_lock);
}

// The times in the states count from the start of the run that saved them. offset_us gives the wall clock of that
// run (the wall clock keeps counting in deep sleep), so each time moves to the same moment of this run.
bool rt_actions_restore(const rt_actions_keep_t *k)
{
    if (!s_lock) {
        return false;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t shift = esp_timer_get_time() - ((int64_t)tv.tv_sec * 1000000 + tv.tv_usec) - k->offset_us;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = k->n == s_n && k->crc == rules_crc();
    if (ok) {
        memcpy(s_st, k->st, sizeof(rt_rule_state_t) * s_n);
        for (int i = 0; i < s_n; i++) {
            int64_t *t[] = { &s_st[i].since_us, &s_st[i].until_us, &s_st[i].on_since_us, &s_st[i].last_note_us };
            for (size_t j = 0; j < sizeof(t) / sizeof(t[0]); j++) {
                if (*t[j]) {
                    *t[j] += shift;
                    *t[j] = *t[j] ? *t[j] : -1;           // 0 means "not set"
                }
            }
        }
        s_summary_day = k->summary_day;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool rt_actions_uses_dev(int slot)
{
    bool used = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_n; i++) {
            used = used || s_act[i].dev == slot;
        }
        xSemaphoreGive(s_lock);
    }
    return used;
}

int rt_actions_json(char *dst, size_t cap)
{
    int n = 0;
    if (!s_lock) {
        return snprintf(dst, cap, "{\"custom\":false,\"rules\":[],\"state\":[],\"radio\":null,\"minute\":null}");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ADD("{\"custom\":%s,\"rules\":", s_custom ? "true" : "false");
    const int k = rt_rules_json(&g_model, s_act, s_n, dst + n, n < (int)cap ? cap - n : 0);
    n = k < 0 ? (int)cap : n + k;
    ADD(",\"state\":[");
    for (int i = 0; i < s_n; i++) {
        ADD("%s{\"on\":%s,\"p\":%.3f,\"events\":%lu,\"on_s\":%lu}", i ? "," : "", s_st[i].active ? "true" : "false", s_st[i].p,
            (unsigned long)s_st[i].events, (unsigned long)s_st[i].on_s);
    }
    xSemaphoreGive(s_lock);
    const float radio = rt_radio_motion();
    const int minute = minute_of_day(NULL);
    ADD("],\"radio\":");
    if (radio < 0) {
        ADD("null");
    } else {
        ADD("%.4f", radio);
    }
    ADD(",\"minute\":");
    if (minute < 0) {
        ADD("null}");
    } else {
        ADD("%d}", minute);
    }
    return n < (int)cap ? n : -1;
}
