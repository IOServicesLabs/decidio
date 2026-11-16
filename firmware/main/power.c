// Low-power mode, for a board on a battery. It is off by default: nothing here changes a board until the user sets it.
//
// The board then sleeps (deep sleep) between short looks. Each wake starts the firmware as usual, lets the camera adjust
// its light for 1 s, runs the model and the rules for the look time (3 s by default), and goes back to sleep. Wi-Fi
// joins only when something must go out (an alert, a Smart Life command) and at a check-in. The board checks at points
// in time: it sees nothing between two wakes.
//
//   wake                     what it does                                                  Wi-Fi
//   power-on or reset        works as without low power for 5 minutes, then sleeps         yes
//   timer (wakes per day)    looks, sends what its rules say, sleeps                       only to send
//   motion pin (optional)    the same, at most once each `gap` seconds                     only to send
//   check-in (every N h)     looks, joins the group, sets the clock, stays 30 s for the page   yes
//
// It does not sleep while a computer is on its USB socket (a charger has no data, so it does not count), while the page
// was used in the last 60 s, or after "stay" (page or console) until "go".
//
// The rules' states are kept through deep sleep (actions.c), so cooldowns, `for`, `hold`, start / end alerts and the daily
// counts work across wakes. The time between two wakes counts as one piece: two wakes that see a condition count it as
// true for all the time between them.
//
// Settings (the `cfg` store): lp_on lp_wakes lp_from lp_to lp_look lp_checkin lp_pir lp_gap lp_usb, and for the page's
// battery estimate lp_mah lp_isleep lp_iawake lp_iwifi. The page computes the estimate; the board only counts.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rt.h"

#define MAGIC 0xdec1d102u
#define AWAKE_S 300             // after power-on or reset
#define CHECKIN_S 30            // awake after the check-in joined Wi-Fi
#define TOUCH_S 60              // awake after the page was used
#define WARM_MS 1000            // the camera adjusts its light after a cold start
#define SEND_S 25               // the longest wait for alerts and device commands to go out
#define BOOT_MS 250             // start-up before the timer counts (ROM and bootloader): an estimate

static const char *TAG = "power";

enum { M_NORMAL = 0, M_AWAKE, M_LOOK, M_CHECKIN };
static const char *const MODE[] = { "normal", "after start", "look", "check-in" };

static struct {
    bool on, usb_sleep;
    int wakes, from, to, look, checkin, pir, gap;
} s_cfg = { .wakes = 96, .from = -1, .to = -1, .look = 3, .checkin = 12, .pir = -1, .gap = 30 };

// kept through deep sleep and a software restart (checked with MAGIC), lost when the power goes
typedef struct {
    uint32_t magic;
    uint32_t wakes, timer_wakes, motion_wakes, short_wakes, checkins, net_wakes;
    uint64_t awake_ms, net_ms, asleep_ms;
    int64_t since;              // wall clock (s) when the counts started, 0 = not known
    int64_t next_look_us, next_checkin_us, rearm_us, slept_at_us;   // wall clock (it keeps counting in deep sleep)
    uint32_t last_sleep_s;
    uint8_t bssid[6], channel;
    char sender_ip[16], sender_host[20];
    bool rules_ok;
    rt_actions_keep_t rules;
} keep_t;
static RTC_NOINIT_ATTR keep_t R;

static int s_mode = M_NORMAL;
static bool s_deep, s_motion, s_stay, s_net, s_net_started, s_task, s_usb;
static volatile bool s_want_net;
static int64_t s_t0, s_touch, s_net_t0, s_online_at;
static int64_t s_count_from = -BOOT_MS * 1000LL;   // awake time counts from here (start, or the switch)

static int64_t wall_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static int get_int(const char *key, int def)
{
    char v[16];
    return rt_store_get(key, v, sizeof(v)) && v[0] ? atoi(v) : def;
}

static float get_float(const char *key, float def)
{
    char v[16];
    return rt_store_get(key, v, sizeof(v)) && v[0] ? strtof(v, NULL) : def;
}

static void load_cfg(void)
{
    s_cfg.on = get_int("lp_on", 0) == 1;
    s_cfg.usb_sleep = get_int("lp_usb", 0) == 1;
    s_cfg.wakes = get_int("lp_wakes", 96);
    s_cfg.from = get_int("lp_from", -1);
    s_cfg.to = get_int("lp_to", -1);
    s_cfg.look = get_int("lp_look", 3);
    s_cfg.checkin = get_int("lp_checkin", 12);
    s_cfg.pir = get_int("lp_pir", -1);
    s_cfg.gap = get_int("lp_gap", 30);
    s_cfg.wakes = s_cfg.wakes < 1 ? 1 : s_cfg.wakes > 1440 ? 1440 : s_cfg.wakes;
    s_cfg.look = s_cfg.look < 1 ? 1 : s_cfg.look > 30 ? 30 : s_cfg.look;
    s_cfg.checkin = s_cfg.checkin < 1 ? 1 : s_cfg.checkin > 48 ? 48 : s_cfg.checkin;
    s_cfg.gap = s_cfg.gap < 5 ? 5 : s_cfg.gap > 3600 ? 3600 : s_cfg.gap;
    if (s_cfg.from < 0 || s_cfg.to < 0 || s_cfg.from > 1439 || s_cfg.to > 1439 || s_cfg.from == s_cfg.to) {
        s_cfg.from = s_cfg.to = -1;
    }
    if (s_cfg.pir >= 0 && !rtc_gpio_is_valid_gpio(s_cfg.pir)) {
        s_cfg.pir = -1;
    }
}

static int minute_at(int64_t t_us, int *sec)
{
    const time_t t = (time_t)(t_us / 1000000);
    if (t < 1600000000) {
        return -1;                                  // the clock was never set: active hours do not apply
    }
    struct tm tm;
    localtime_r(&t, &tm);
    if (sec) {
        *sec = tm.tm_sec;
    }
    return tm.tm_hour * 60 + tm.tm_min;
}

static bool in_hours(int64_t t_us)
{
    const int m = s_cfg.from < 0 ? -1 : minute_at(t_us, NULL);
    if (m < 0) {
        return true;
    }
    return s_cfg.from < s_cfg.to ? (m >= s_cfg.from && m < s_cfg.to) : (m >= s_cfg.from || m < s_cfg.to);
}

static int64_t into_hours(int64_t t_us)             // t, or the start of the next active hours after it
{
    if (in_hours(t_us)) {
        return t_us;
    }
    int sec = 0;
    const int m = minute_at(t_us, &sec);
    const int d = (s_cfg.from - m + 1440) % 1440;
    return t_us + ((int64_t)d * 60 - sec) * 1000000 + 1000000;
}

static bool motion_high(void)
{
    if (s_cfg.pir < 0) {
        return false;
    }
    static bool ready;
    if (!ready) {
        rtc_gpio_deinit(s_cfg.pir);                 // after a wake by this pin it still belongs to the RTC
        gpio_reset_pin(s_cfg.pir);
        gpio_set_direction(s_cfg.pir, GPIO_MODE_INPUT);
        gpio_set_pull_mode(s_cfg.pir, GPIO_PULLDOWN_ONLY);
        ready = true;
    }
    return gpio_get_level(s_cfg.pir) == 1;
}

// kind: 0 = no look on this wake (only the motion pin was armed again), 1 = a look on the timer, 2 = a look on motion
static void sleep_now(int kind)
{
    const int64_t now = wall_us();
    const int64_t period = 86400LL * 1000000 / s_cfg.wakes;
    if (!R.next_look_us || R.next_look_us < now - period || R.next_look_us > now + period + 60000000LL) {
        R.next_look_us = now + period;              // first sleep, or the clock was set since
    } else if (kind == 1) {
        R.next_look_us += period;                   // the same rhythm, whatever each look took
    }
    while (R.next_look_us < now + 5000000) {
        R.next_look_us += period;
    }
    R.next_look_us = into_hours(R.next_look_us);
    int64_t wake = R.next_look_us;
    bool arm = false;
    if (kind == 2) {
        R.rearm_us = now + (int64_t)s_cfg.gap * 1000000;
    }
    if (s_cfg.pir >= 0 && in_hours(now)) {
        if (now >= R.rearm_us && !motion_high()) {
            arm = true;
        } else {
            if (R.rearm_us <= now) {
                R.rearm_us = now + (int64_t)s_cfg.gap * 1000000;   // still high: look again later
            }
            wake = R.rearm_us < wake ? R.rearm_us : wake;
        }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)(wake - now));
    if (arm) {
        rtc_gpio_pullup_dis(s_cfg.pir);
        rtc_gpio_pulldown_en(s_cfg.pir);
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_sleep_enable_ext1_wakeup_io(1ULL << s_cfg.pir, ESP_EXT1_WAKEUP_ANY_HIGH);
    }
    R.slept_at_us = now;
    R.last_sleep_s = (uint32_t)((wake - now) / 1000000);
    printf("{\"power\":{\"sleep_s\":%lu,\"motion\":%s}}\n", (unsigned long)R.last_sleep_s, arm ? "true" : "false");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_deep_sleep_start();
}

void rt_power_early(void)
{
    load_cfg();
    const esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
    s_deep = why == ESP_SLEEP_WAKEUP_TIMER || why == ESP_SLEEP_WAKEUP_EXT1;
    if (R.magic != MAGIC) {
        memset(&R, 0, sizeof(R));
        R.magic = MAGIC;
    }
    const int64_t now = wall_us();
    if (R.slept_at_us && now > R.slept_at_us) {
        R.asleep_ms += (uint64_t)((now - R.slept_at_us) / 1000);
    }
    R.slept_at_us = 0;
    if (!R.since && now > 1600000000LL * 1000000) {
        R.since = now / 1000000;
    }
    if (!s_deep) {                                  // power-on or reset: start the rhythm again
        R.next_look_us = R.rearm_us = R.next_checkin_us = 0;
        R.rules_ok = false;
        return;
    }
    if (!s_cfg.on) {
        return;                                     // switched off while asleep (cannot be), or a stale wake
    }
    const bool early = why == ESP_SLEEP_WAKEUP_TIMER && R.next_look_us && now < R.next_look_us - 2000000;
    if (early || !in_hours(now)) {
        R.short_wakes++;                            // only to arm the motion pin again, or outside the active hours
        R.awake_ms += (uint64_t)(esp_timer_get_time() / 1000) + BOOT_MS;
        sleep_now(0);
    }
    s_motion = why == ESP_SLEEP_WAKEUP_EXT1;
    R.wakes++;
    if (s_motion) {
        R.motion_wakes++;
    } else {
        R.timer_wakes++;
    }
}

bool rt_power_on(void)
{
    return s_cfg.on;
}

static void net_task(void *arg)
{
    rt_wifi_boot();
    vTaskDelete(NULL);
}

static void net_up(void)
{
    s_want_net = false;
    if (s_net_started) {
        return;
    }
    s_net_started = true;
    if (!s_net) {
        s_net = true;
        s_net_t0 = esp_timer_get_time();
        R.net_wakes++;
    }
    // Wi-Fi, mDNS and the web server start in a task of their own with a large stack, freed when they run
    if (xTaskCreatePinnedToCore(net_task, "net_up", 6144, NULL, 4, NULL, 0) != pdPASS) {
        rt_wifi_boot();
    }
}

void rt_power_need_net(void)
{
    if (s_cfg.on && !s_net_started) {
        s_want_net = true;                          // the power task starts Wi-Fi within 0.1 s
    }
}

bool rt_power_net_now(void)
{
    if (s_mode == M_LOOK) {
        return false;
    }
    s_net_started = true;                           // the caller starts it
    return true;
}

void rt_power_touch(void)
{
    s_touch = esp_timer_get_time();
}

bool rt_power_fast_ap(uint8_t bssid[6], uint8_t *channel)
{
    if (!s_cfg.on || !R.channel) {
        return false;
    }
    memcpy(bssid, R.bssid, 6);
    *channel = R.channel;
    return true;
}

void rt_power_joined(const uint8_t bssid[6], uint8_t channel)
{
    memcpy(R.bssid, bssid, 6);
    R.channel = channel;
    if (s_net && !s_online_at) {
        s_online_at = esp_timer_get_time();
    }
}

void rt_power_note_sender(const char *ip, const char *host)
{
    snprintf(R.sender_ip, sizeof(R.sender_ip), "%s", ip);
    snprintf(R.sender_host, sizeof(R.sender_host), "%s", host);
}

bool rt_power_sender(char *ip, size_t ip_cap, char *host, size_t host_cap)
{
    if (!s_cfg.on || !R.sender_ip[0]) {
        return false;
    }
    snprintf(ip, ip_cap, "%s", R.sender_ip);
    snprintf(host, host_cap, "%s", R.sender_host);
    return true;
}

static int64_t look_us(void)
{
    int64_t us = (int64_t)s_cfg.look * 1000000;
    if (g_model.has_audio && g_board.has_mic && g_model.sr > 0) {
        const int64_t clip = (int64_t)g_model.clip * 1000000 / g_model.sr + 300000;   // the sound questions hear a whole clip
        us = clip > us ? clip : us;
    }
    return us;
}

static void go_sleep(void)
{
    g_state.run = false;
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < 10 * SEND_S && s_net && (!rt_alert_idle() || rt_dev_pending()); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));             // an alert of the last decision
    }
    rt_actions_save(&R.rules);
    R.rules.offset_us = esp_timer_get_time() - wall_us();
    R.rules_ok = true;
    const int64_t wall = wall_us();
    if (s_mode == M_CHECKIN || s_mode == M_AWAKE) {
        // a check-in that never joined tries again in an hour, not at every wake
        R.next_checkin_us = wall + (s_online_at ? (int64_t)s_cfg.checkin * 3600 : 3600) * 1000000;
        if (s_mode == M_CHECKIN) {
            R.checkins++;
        }
    }
    if (s_net) {
        R.net_ms += (uint64_t)((esp_timer_get_time() - s_net_t0) / 1000);
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    rt_cam_standby();
    R.awake_ms += (uint64_t)((esp_timer_get_time() - s_count_from) / 1000);
    sleep_now(s_motion ? 2 : 1);
}

static void power_task(void *arg)
{
    int64_t look_from = 0, done_at = 0;
    int idle = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const int64_t t = esp_timer_get_time();
        if (!s_cfg.on) {                            // switched off from the page or the console
            s_mode = M_NORMAL;
            g_state.run = true;
            net_up();
            s_task = false;
            vTaskDelete(NULL);
        }
        if (s_want_net || rt_dev_pending()) {
            net_up();
        }
        if (!look_from && t >= WARM_MS * 1000LL) {
            g_state.run = true;
            look_from = t;
        }
        if (s_net && rt_wifi_online() && t % 2000000 < 100000) {
            char ip[16], host[20];
            if (rt_home_sender(ip, sizeof(ip), host, sizeof(host))) {
                rt_power_note_sender(ip, host);     // for the next wakes, which do not wait to hear the group
            }
        }
        s_usb = !s_cfg.usb_sleep && usb_serial_jtag_is_connected();
        if (s_usb || s_stay || (s_touch && t - s_touch < TOUCH_S * 1000000LL)) {
            if (s_usb || s_stay) {
                net_up();
            }
            done_at = 0;
            continue;
        }
        bool done;
        if (s_mode == M_AWAKE) {
            done = t - s_t0 >= AWAKE_S * 1000000LL;
        } else if (s_mode == M_CHECKIN) {
            done = s_online_at ? t - s_online_at >= CHECKIN_S * 1000000LL : t - s_t0 >= 30000000LL;
        } else {
            done = look_from && t - look_from >= look_us();
        }
        if (!done) {
            continue;
        }
        if (!done_at) {
            done_at = t;
        }
        if ((!rt_alert_idle() || rt_dev_pending() || s_want_net) && t - done_at < SEND_S * 1000000LL) {
            idle = 0;
            continue;
        }
        if (++idle >= 3) {
            go_sleep();
        }
    }
}

static void task_start(void)
{
    if (!s_task) {
        s_task = xTaskCreatePinnedToCore(power_task, "power", 4096, NULL, 4, NULL, 0) == pdPASS;
    }
}

void rt_power_start(void)
{
    rt_alert_start();                               // alerts can be queued before Wi-Fi joins
    if (!s_cfg.on) {
        s_mode = M_NORMAL;
        return;
    }
    const int64_t now = wall_us();
    s_t0 = esp_timer_get_time();
    if (!s_deep) {
        s_mode = M_AWAKE;
    } else if (!R.next_checkin_us || now >= R.next_checkin_us ||
               R.next_checkin_us > now + (int64_t)s_cfg.checkin * 3600 * 1000000 + 60000000LL) {
        s_mode = M_CHECKIN;
    } else {
        s_mode = M_LOOK;
    }
    if (s_deep && R.rules_ok && !rt_actions_restore(&R.rules)) {
        ESP_LOGI(TAG, "the rules changed: their states start again");
    }
    s_net = s_mode != M_LOOK;
    s_net_t0 = s_t0;
    if (s_net) {
        R.net_wakes++;
    }
    g_state.run = false;                            // the camera adjusts its light first
    printf("{\"power\":{\"mode\":\"%s\",\"wake\":\"%s\"}}\n", MODE[s_mode],
           !s_deep ? "start" : s_motion ? "motion" : "timer");
    task_start();
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

static void add_time(char *dst, size_t cap, int *np, const char *key, int64_t us)
{
    int n = *np;
    if (us > 1600000000LL * 1000000) {
        ADD(",\"%s\":%lld", key, (long long)(us / 1000000));
    } else {
        ADD(",\"%s\":null", key);
    }
    *np = n;
}

int rt_power_json(char *dst, size_t cap)
{
    int n = 0;
    ADD("{\"on\":%s,\"wakes\":%d,\"from\":%d,\"to\":%d,\"look\":%d,\"checkin\":%d,\"pir\":%d,\"gap\":%d,\"usb_sleep\":%s",
        s_cfg.on ? "true" : "false", s_cfg.wakes, s_cfg.from, s_cfg.to, s_cfg.look, s_cfg.checkin, s_cfg.pir, s_cfg.gap,
        s_cfg.usb_sleep ? "true" : "false");
    ADD(",\"mah\":%g,\"i_sleep\":%g,\"i_awake\":%g,\"i_wifi\":%g,\"bat_pin\":%d,\"bat_ratio\":%g",
        get_float("lp_mah", 2000), get_float("lp_isleep", 1.0f), get_float("lp_iawake", 110), get_float("lp_iwifi", 90),
        get_int("bat_pin", -1), get_float("bat_ratio", 2));
    const int64_t t = esp_timer_get_time();
    ADD(",\"now\":{\"mode\":\"%s\",\"usb\":%s,\"stay\":%s,\"wifi\":%s,\"up_s\":%lld,\"look_s\":%.1f,\"motion_pin\":%s}",
        MODE[s_mode], (s_task ? s_usb : usb_serial_jtag_is_connected()) ? "true" : "false", s_stay ? "true" : "false", s_net ? "true" : "false",
        (long long)(t / 1000000), look_us() / 1e6, s_cfg.pir >= 0 && motion_high() ? "true" : "false");
    ADD(",\"count\":{\"wakes\":%lu,\"timer\":%lu,\"motion\":%lu,\"short\":%lu,\"checkins\":%lu,\"wifi\":%lu,"
        "\"awake_s\":%llu,\"wifi_s\":%llu,\"asleep_s\":%llu,\"last_sleep_s\":%lu",
        (unsigned long)R.wakes, (unsigned long)R.timer_wakes, (unsigned long)R.motion_wakes, (unsigned long)R.short_wakes,
        (unsigned long)R.checkins, (unsigned long)R.net_wakes, (unsigned long long)(R.awake_ms / 1000),
        (unsigned long long)(R.net_ms / 1000), (unsigned long long)(R.asleep_ms / 1000), (unsigned long)R.last_sleep_s);
    add_time(dst, cap, &n, "since", R.since * 1000000);
    ADD("}");
    add_time(dst, cap, &n, "next_look", R.next_look_us);
    add_time(dst, cap, &n, "next_checkin", R.next_checkin_us);
    ADD(",\"sender\":\"%s\",\"battery\":", R.sender_host);
    const int iv = rt_input_index("battery_v"), ip = rt_input_index("battery_pct");
    const float v = iv >= 0 ? rt_input_value(iv) : NAN, pc = ip >= 0 ? rt_input_value(ip) : NAN;
    if (isnan(v)) {
        ADD("null}");
    } else {
        ADD("{\"v\":%.3f,\"pct\":%.0f}}", v, isnan(pc) ? 0 : pc);
    }
    return n < (int)cap ? n : -1;
}

static bool num(const cJSON *j, const char *k, double lo, double hi, double *out, char *why, size_t cap, bool *bad)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    if (!v) {
        return false;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        snprintf(why, cap, "%s: a number from %g to %g", k, lo, hi);
        *bad = true;
        return false;
    }
    *out = v->valuedouble;
    return true;
}

static void put_int(const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    rt_store_set(key, b);
}

static void put_float(const char *key, double v)
{
    char b[16];
    snprintf(b, sizeof(b), "%g", v);
    rt_store_set(key, b);
}

static void reset_counts(void)
{
    memset(&R, 0, offsetof(keep_t, next_look_us));   // the counts; the schedule and the rules' states stay
    R.magic = MAGIC;
    R.since = wall_us() > 1600000000LL * 1000000 ? wall_us() / 1000000 : 0;
}

static void switch_on(void)
{
    if (s_mode != M_NORMAL) {
        return;
    }
    reset_counts();                                 // new counts: the time before low power is not in them
    s_count_from = esp_timer_get_time();
    // switched on while the board runs: it sleeps when the page has not been used for a minute
    s_t0 = esp_timer_get_time() - (AWAKE_S - TOUCH_S) * 1000000LL;
    s_mode = M_AWAKE;
    s_net = s_net_started = true;
    s_net_t0 = esp_timer_get_time();
    R.net_wakes = 1;
    R.next_look_us = R.rearm_us = 0;
    R.next_checkin_us = wall_us() + (int64_t)s_cfg.checkin * 3600 * 1000000;
    task_start();
}

esp_err_t rt_power_set(const cJSON *j, char *why, size_t cap)
{
    bool bad = false;
    double v;
    // first check everything, then store: a request with one bad value changes nothing
    struct { const char *k, *key; double lo, hi; bool f; } F[] = {
        { "wakes", "lp_wakes", 1, 1440, false }, { "look", "lp_look", 1, 30, false },
        { "checkin", "lp_checkin", 1, 48, false }, { "gap", "lp_gap", 5, 3600, false },
        { "from", "lp_from", -1, 1439, false }, { "to", "lp_to", -1, 1439, false },
        { "mah", "lp_mah", 50, 100000, true }, { "i_sleep", "lp_isleep", 0.001, 500, true },
        { "i_awake", "lp_iawake", 1, 1000, true }, { "i_wifi", "lp_iwifi", 0, 1000, true },
        { "bat_ratio", "bat_ratio", 1, 10, true },
    };
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        num(j, F[i].k, F[i].lo, F[i].hi, &v, why, cap, &bad);
    }
    if (num(j, "pir", -1, 48, &v, why, cap, &bad) && (int)v >= 0 &&
        (!rtc_gpio_is_valid_gpio((int)v) || !rt_board_pin_free((int)v))) {
        snprintf(why, cap, "pir: pin %d cannot wake the board (a free pin from 1 to 21)", (int)v);
        bad = true;
    }
    if (num(j, "bat_pin", -1, 48, &v, why, cap, &bad) && (int)v >= 0 && ((int)v > 10 || !rt_board_pin_free((int)v))) {
        snprintf(why, cap, "bat_pin: a free pin from 1 to 10 (the first ADC of the chip)");
        bad = true;
    }
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on"), *usb = cJSON_GetObjectItemCaseSensitive(j, "usb_sleep");
    const cJSON *stay = cJSON_GetObjectItemCaseSensitive(j, "stay"), *reset = cJSON_GetObjectItemCaseSensitive(j, "reset_counts");
    if ((on && !cJSON_IsBool(on)) || (usb && !cJSON_IsBool(usb)) || (stay && !cJSON_IsBool(stay))) {
        snprintf(why, cap, "on, usb_sleep and stay are true or false");
        bad = true;
    }
    if (bad) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        if (num(j, F[i].k, F[i].lo, F[i].hi, &v, why, cap, &bad)) {
            if (F[i].f) {
                put_float(F[i].key, v);
            } else {
                put_int(F[i].key, (int)v);
            }
        }
    }
    if (num(j, "pir", -1, 48, &v, why, cap, &bad)) {
        put_int("lp_pir", (int)v);
    }
    if (num(j, "bat_pin", -1, 48, &v, why, cap, &bad)) {
        put_int("bat_pin", (int)v);                 // the battery input starts at the next start (sensors/battery.c)
    }
    if (usb) {
        rt_store_set("lp_usb", cJSON_IsTrue(usb) ? "1" : NULL);
    }
    if (stay) {
        s_stay = cJSON_IsTrue(stay);
    }
    if (cJSON_IsTrue(reset)) {
        reset_counts();
    }
    if (on) {
        rt_store_set("lp_on", cJSON_IsTrue(on) ? "1" : NULL);
    }
    load_cfg();
    if (s_cfg.on) {
        switch_on();
    }
    snprintf(why, cap, "ok");
    return ESP_OK;
}

// console: power | power on | power off | power stay | power go | power wakes <n>
bool rt_power_command(const char *arg, const char *arg2)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return false;
    }
    if (!arg) {
        // status only
    } else if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
        cJSON_AddBoolToObject(j, "on", !strcmp(arg, "on"));
    } else if (!strcmp(arg, "stay") || !strcmp(arg, "go")) {
        cJSON_AddBoolToObject(j, "stay", !strcmp(arg, "stay"));
    } else if (!strcmp(arg, "wakes") && arg2) {
        cJSON_AddNumberToObject(j, "wakes", atoi(arg2));
    } else {
        printf("{\"error\":\"power | power on|off | power stay|go | power wakes <1-1440>\"}\n");
        cJSON_Delete(j);
        return false;
    }
    char why[100] = "";
    if (cJSON_GetArraySize(j) && rt_power_set(j, why, sizeof(why)) != ESP_OK) {
        printf("{\"error\":\"%s\"}\n", why);
        cJSON_Delete(j);
        return false;
    }
    cJSON_Delete(j);
    static char b[1400];
    if (rt_power_json(b, sizeof(b)) > 0) {
        printf("{\"power\":%s}\n", b);
    }
    return true;
}
