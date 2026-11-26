// decidio runtime: shared types and module interfaces.
//
// The firmware is generic. Everything model-specific (inputs, questions, audio frontend, actions) comes from the
// model package (.dcd) in the `model` flash partition, so a new model is flashed with esptool - no rebuild.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_VERSION "0.5.0"
#define RT_CAM 96            // every frame is 96x96 (center square of the sensor, like training data)
#define RT_MAX_Q 16
#define RT_MAX_OPT 16
#define RT_MAX_ACT 16
#define RT_MAX_COND 3       // conditions per rule, all must hold
#define RT_Q_RADIO (-2)      // a condition on the radio movement score instead of a model answer
#define RT_Q_PEER (-3)       // a condition on a rule of another board (o = which one, see rules.c)
#define RT_Q_INPUT (-4)      // a condition on a named input (o = which one, see inputs.c)
#define RT_MAX_INPUT 24
#define RT_MAX_DEV 8         // Smart Life / Tuya devices a board can switch
#define RT_MAX_LOGITS 128
#define RT_ID_LEN 32

typedef enum { Q_NOUL = 0, Q_CHOICE = 1, Q_SCORE = 2 } rt_qtype_t;

typedef struct {
    char id[RT_ID_LEN];
    rt_qtype_t type;
    char mod;                   // 'i' the camera answers it, 'a' the microphone, 0 = not stated (older packages)
    bool na;                    // this board lacks the sensor: the answer means nothing and rules on it stay off
    int offset, count;
    char opt[RT_MAX_OPT][RT_ID_LEN];
} rt_question_t;

typedef struct {
    int16_t q, o;               // question and option index, or q = RT_Q_RADIO / RT_Q_PEER / RT_Q_INPUT
    bool below;                 // true: value < p, false: value >= p
    float p;
} rt_cond_t;

enum { RT_EDGE_START = 0, RT_EDGE_END = 1, RT_EDGE_BOTH = 2 };
// what a rule does to its device: on while the rule is on | off while it is on | switch on / off when it starts
enum { RT_DEV_FOLLOW = 0, RT_DEV_INVERSE = 1, RT_DEV_ON = 2, RT_DEV_OFF = 3 };

typedef struct {                // a rule (rules.c describes the JSON form)
    char name[RT_ID_LEN];
    char text[72];              // the alert's first line
    rt_cond_t c[RT_MAX_COND];
    int n_c;
    int for_ms, hold_ms, gpio, level, cooldown_s;
    int16_t from_min, to_min;   // active hours as minutes of the day, -1 = always
    uint8_t edge;               // which change alerts
    int8_t dev;                 // the device this rule switches (slot in devices.c), -1 = none
    uint8_t dev_do;             // RT_DEV_*
    bool notify, photo, count, urgent;   // urgent: its alert is never held back to be grouped
} rt_action_t;

typedef struct {
    int64_t since_us;           // conditions true since (0 = false)
    int64_t until_us;           // on until
    int64_t on_since_us, last_note_us;
    bool active, noted;
    uint32_t events, on_s;      // times it turned on, seconds it was on (since the last summary)
    float p;                    // the first condition's value at the last decision
} rt_rule_state_t;

typedef struct {
    bool changed, on, notify;
} rt_rule_out_t;

typedef struct {
    bool loaded;
    char name[RT_ID_LEN];
    // image
    bool has_image;
    int img_size, colors, motion, ego, lag, frame_ms;   // ego: take the camera's own shift out of the motion channel
    // audio
    bool has_audio;
    int sr, clip, win, hop, nfft, n_mels, n_frames;
    float fmin, fmax, mean, std, eps;
    // decisions
    int n_q, n_logits, arena_kb, n_act;
    rt_question_t q[RT_MAX_Q];
    rt_action_t act[RT_MAX_ACT];
    const uint8_t *tflite;
    size_t tflite_len;
    uint32_t crc;               // the package's checksum: tells one flashed package from another
} rt_model_t;

extern rt_model_t g_model;

// board.c - which board this is and what answered on it, found at start (one firmware for every board)
enum { RT_CARD_NONE = 0, RT_CARD_SPI = 1, RT_CARD_MMC = 2 };
typedef struct {
    const char *name;           // the profile: xiao_sense | s3cam | s3cam-lcd
    bool has_cam, has_mic, has_card;    // what was found
    bool uart_console;          // the board has a USB-serial socket on UART0 (GPIO43/44)
    bool saved;                 // the profile came from the settings store, not from looking
    int8_t cam_map;             // camera.c pin map
    int8_t orient;              // the picture's default orientation, -1 = by sensor
    int mic_clk, mic_din;       // PDM microphone, -1 = the board has none
    int8_t card_bus;            // RT_CARD_*
    int8_t card_pin[4];         // SPI: cs, sck, miso, mosi; SD/MMC 1-bit: clk, cmd, d0
    uint64_t pins;              // GPIOs a rule may drive (bit n = GPIO n); 0 = any valid output
} rt_board_t;
extern rt_board_t g_board;
void rt_board_detect(void);                            // before the model is read
bool rt_board_command(const char *arg);                // board | board auto | board <profile>
bool rt_board_pin_free(int gpio);                      // may a rule, an input or a sensor use this pin
int rt_board_sensors_json(char *dst, size_t cap);      // {"profile":..,"camera":..,"mic":..,"card":..,...}

// model.c - load and validate the package from the `model` partition
esp_err_t rt_model_load(void);

// camera.c - 96x96 frames (gray or RGB888) every frame_ms into a ring
bool rt_cam_uses_pin(int map, int gpio);               // is this pin one of that map's camera pins
int rt_cam_maps(void);                                 // how many pin maps are known
const char *rt_cam_map_name(int map);
bool rt_model_hw(bool *no_camera, char *profile, size_t cap);   // what the model package says about the board
esp_err_t rt_cam_probe(int map);                       // start the sensor on this pin map
esp_err_t rt_cam_start(int colors, int lag, int frame_ms);   // after a successful probe
bool rt_cam_acquire(const uint8_t **cur, const uint8_t **prev, uint32_t *seq);   // lock held until release
void rt_cam_release(void);
int rt_cam_colors(void);
const char *rt_cam_sensor(void);
void rt_cam_set_orient(int orient);
bool rt_cam_rotate(int deg);                           // the user's turn of the picture: 0, 90, 180 or 270; kept
int rt_cam_rotation(void);
// One frame of a part of the picture at the sensor's own resolution. x, y, w are in pixels of the 160x120 frame; the
// part is 4:3 (h = 3w/4). The answer comes later on the camera task: the frame (160x120 RGB565), the part that was
// really read, and how long the wide view was away. false = this sensor cannot, or a request is waiting already.
typedef void (*rt_cam_zoom_fn)(const uint8_t *rgb565, uint16_t id, int x, int y, int w, int h, int took_ms);
bool rt_cam_can_zoom(void);
bool rt_cam_zoom(int x, int y, int w, uint16_t id, rt_cam_zoom_fn fn);
void rt_restart(void);                                 // restart the chip; the camera sensor is put in standby first
void rt_cam_standby(void);
void rt_cam_off(void);                                 // before a restart: stop the camera interface of the chip
void rt_cam_quiet(void);                               // a start without the camera: reset that interface                             // stop taking frames and put the sensor in standby (before deep sleep)

// audio.c - PDM mic, streaming log-mel frames + raw PCM history
int rt_audio_probe(int clk, int din);                  // listen for 0.2 s: the spread of the samples, 0 = nothing there
esp_err_t rt_audio_start(int sr, int clip, bool mel);
bool rt_audio_mel(float *dst, uint32_t *seq);          // n_frames x n_mels, oldest first
uint32_t rt_audio_mel_seq(void);                       // mel frames produced so far (cheap, no copy)
bool rt_audio_pcm(int16_t *dst, int n);                // last n samples
int rt_audio_rate(void);                               // samples a second, 0 = the microphone does not run
int rt_audio_clip(void);                               // samples in the history (the model's clip)
uint32_t rt_audio_seq(void);

// mel.c
esp_err_t rt_mel_init(const rt_model_t *m);
void rt_mel_frame(const int16_t *win, float *out);

// ego.c - the camera's own shift between two 96x96 luma frames: cur(x, y) ~ prev(x + dx, y + dy)
void rt_ego_shift(const uint8_t *cur, const uint8_t *prev, int *dx, int *dy);
// the motion channel at frame pixel (sx, sy): |now - the earlier frame moved back by the camera's shift| (edges clamp)
static inline uint8_t rt_ego_diff(const uint8_t *cur, const uint8_t *prev, int sx, int sy, int dx, int dy)
{
    int px = sx + dx, py = sy + dy;
    px = px < 0 ? 0 : (px > RT_CAM - 1 ? RT_CAM - 1 : px);
    py = py < 0 ? 0 : (py > RT_CAM - 1 ? RT_CAM - 1 : py);
    const int a = cur[sy * RT_CAM + sx], b = prev[py * RT_CAM + px];
    return (uint8_t)(a > b ? a - b : b - a);
}

// decide.cc
typedef struct {
    float probs[RT_MAX_LOGITS];
    uint32_t prep_us, invoke_us;
    int ego_dx, ego_dy;          // the camera's shift taken out of the motion channel (motion: ego models)
} rt_result_t;
esp_err_t rt_decide_init(void);
void rt_decide_inputs(const uint8_t *cur, const uint8_t *prev, const float *mel);
esp_err_t rt_decide_invoke(rt_result_t *r);
void rt_decide_print(const rt_result_t *r, float hz);
int rt_decide_last(char *dst, size_t cap);             // the last printed decision line ("" before the first)
void rt_decide_profile(bool on);
size_t rt_decide_arena_used(void);

// rules.c - rules as JSON, and when a rule is on (no hardware: also built and tested on a PC)
struct cJSON;
bool rt_rule_parse(const struct cJSON *a, const rt_model_t *m, rt_action_t *A);
int rt_rules_parse(const struct cJSON *arr, const rt_model_t *m, rt_action_t *out, int max);
int rt_rules_json(const rt_model_t *m, const rt_action_t *act, int n_act, char *dst, size_t cap);
void rt_rule_step(const rt_action_t *a, rt_rule_state_t *s, const rt_model_t *m, const float *probs, float radio,
                  int64_t now_us, int minute, rt_rule_out_t *o);
void rt_rules_refs_keep(const rt_action_t *a, int na, const rt_action_t *b, int nb);   // forget other boards' rules no rule names

// inputs.c - named numbers a rule can test besides the model's answers ("in:<name>"): sensors/*.c add theirs here
typedef float (*rt_input_fn)(void);                    // the value now; NAN = not available at the moment
int rt_input_register(const char *name, const char *unit, float lo, float hi, rt_input_fn fn);   // its index, -1 = full
int rt_input_index(const char *name);                  // -1 = no such input
const char *rt_input_name(int i);
float rt_input_value(int i);
int rt_inputs_json(char *dst, size_t cap);             // [{"name":..,"unit":..,"lo":..,"hi":..,"value":..},...]
void rt_sensors_start(void);                           // sensors/*.c: every sensor's start function, once
void rt_sensor_add(void (*start)(void));
// a sensor file ends with RT_SENSOR(its_start_function): the function runs once at start, before rules are read
#define RT_SENSOR(fn) static void __attribute__((constructor)) fn##_register(void) { rt_sensor_add(fn); }
// sensors/pin_in.c - header pins as inputs "pin<N>"; a change takes effect at the next start
bool rt_pin_in_set(int gpio, const char *pull, bool off, char *why, size_t cap);
// sensors/i2c_scan.c - an I2C bus on any two free pins, for sensor files, and a scan of it
bool rt_i2c_begin(int sda_pin, int scl_pin);
bool rt_i2c_write(uint8_t addr, const uint8_t *b, int n);
bool rt_i2c_read(uint8_t addr, uint8_t *b, int n);
bool rt_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *b, int n);
int rt_i2c_scan_json(int sda_pin, int scl_pin, char *dst, size_t cap);

// eye.cc - the fly eye: finds what moves and where, beside the model (fly/)
void rt_eye_inputs(void);                              // its named inputs; before the rules are read
void rt_eye_start(void);                               // after the camera started
void rt_eye_frame565(const uint8_t *rgb565);           // camera task: one 160x120 sensor frame
bool rt_eye_on(void);
bool rt_eye_wanted(void);
void rt_eye_want(bool on);                             // kept across restarts
size_t rt_eye_packet(uint8_t *dst, size_t cap);        // the page's picture of the eye, 0 = off
bool rt_eye_lock(int id);                              // follow this figure; 0 = let go, -1 = the eye chooses
int rt_eye_json(char *dst, size_t cap);                // speed and the latest events
#define RT_EYE_PACKET_MAX 13400
#define RT_EYE_SHARP_MAX (16 + 2 * 160 * 120)
size_t rt_eye_sharp(uint8_t *dst, size_t cap);         // the last sharp close-up for the page, 0 = none yet
size_t rt_eye_wide(uint8_t *dst, size_t cap);          // the last full frame (160x120 RGB565), while the page looks; for checks
bool rt_eye_zoom_test(int x, int y, int w);            // console: zoom onto this part now

// actions.c - runs the rules after every decision: pins, event lines, alerts, daily counts
void rt_actions_init(void);
void rt_actions_update(const rt_result_t *r, int64_t now_us);
esp_err_t rt_actions_set(const char *json);            // rules from the web page ("" or NULL: back to the model's own)
int rt_actions_json(char *dst, size_t cap);            // {"rules":[...],"state":[...],"custom":bool}
int rt_actions_brief(char *dst, size_t cap);           // [["name",1],...]: which rules are on, for the other boards
bool rt_actions_uses_dev(int slot);
bool rt_actions_uses_radio(void);
typedef struct {                // the rules' states, kept through deep sleep (power.c)
    uint32_t crc;               // of the rules they belong to
    int n, summary_day;
    int64_t offset_us;          // esp_timer minus the wall clock when they were saved
    rt_rule_state_t st[RT_MAX_ACT];
} rt_actions_keep_t;
void rt_actions_save(rt_actions_keep_t *k);
bool rt_actions_restore(const rt_actions_keep_t *k);   // false: the rules changed since, nothing restored
void rt_actions_clock(void);                           // time zone or summary time changed
// model.c - the rules that came with the model package
int rt_model_default_rules(rt_action_t *out, int max);

// console.c - line commands on the USB / UART console
typedef struct {
    volatile bool run;          // decisions on
    volatile int print_ms;      // JSON line interval (0 = every decision)
    volatile int collect_ms;    // >0: stream samples to the PC every N ms
    volatile bool profile;
} rt_state_t;
extern rt_state_t g_state;
void rt_console_start(void);
void rt_console_ready(void);                           // at the end of start-up: commands are read from here on
void rt_console_board(void);                          // after the board is known
bool rt_console_has_uart(void);                       // a USB-serial chip holds UART0's receive pin high: an S3-CAM board

// radio.c - Wi-Fi channel state (CSI) capture for radio sensing; the Wi-Fi stack starts on first use
bool rt_radio_command(const char *arg, const char *sender);
float rt_radio_motion(void);                           // movement score over the last 2 s, < 0 = not running
void rt_radio_motion_want(bool on);                    // kept across restarts
void rt_print_info(void);
int rt_info_json(char *dst, size_t cap);               // the `info` object, also served by the web page

// store.c - settings in the `cfg` partition (Wi-Fi, page password, alert destinations, edited rules)
esp_err_t rt_store_init(void);
char *rt_store_dup(const char *key, size_t *len);      // caller frees; NULL = not set
bool rt_store_get(const char *key, char *dst, size_t cap);
esp_err_t rt_store_set(const char *key, const char *val);   // NULL or "" removes
esp_err_t rt_store_wipe(void);

// wifi.c - join a network, or open the setup access point
typedef struct {
    const char *state;          // off | setup | connecting | online
    bool setup_open;
    int reason, rssi, channel;
    char ssid[33], ip[16], host[20];
} rt_wifi_status_t;
esp_err_t rt_wifi_driver(void);
void rt_wifi_boot(void);
esp_err_t rt_wifi_set(const char *ssid, const char *pass);
void rt_wifi_forget(bool off);
void rt_wifi_status(rt_wifi_status_t *o);
bool rt_wifi_online(void);
bool rt_wifi_setup_open(void);
int rt_wifi_channel(void);                             // the joined network's channel, 0 when not joined
const char *rt_wifi_host(void);
int rt_wifi_scan_json(char *dst, size_t cap);

// devices.c - Smart Life / Tuya devices on the local network, switched by rules
void rt_dev_load(void);                                // before the rules are read: they name devices
int rt_dev_index(const char *name);                    // slot, or -1
const char *rt_dev_name(int slot);                     // "" for an empty slot
void rt_dev_rule(int slot, int rule, int mode, bool on);
void rt_dev_rules_reset(void);
void rt_dev_settle(void);
esp_err_t rt_dev_set(const struct cJSON *j, char *why, size_t cap);
int rt_dev_json(char *dst, size_t cap);
void rt_dev_step(int wait_ms);
void rt_dev_wake(void);
bool rt_dev_pending(void);                             // a device command waits to be sent

// home.c - the other boards in the home: who is there, which of their rules are on
void rt_home_start(void);
void rt_home_reload(void);
void rt_home_changed(void);                            // a rule of this board changed: tell the others now
float rt_peer_value(const char *host, const char *rule);   // 1 / 0, < 0 = not known
bool rt_home_peer_ip(const char *host, char *ip, size_t cap);
bool rt_home_sender(char *ip, size_t ip_cap, char *host, size_t host_cap);   // true: another board sends the alerts
bool rt_home_handoff(const struct cJSON *alert, const char *ip);  // give it an alert; true when it said it has it
const char *rt_home_label(void);                       // this board's room, or its name when it has no room
int rt_home_members(char ips[][16], char hosts[][20], int max);  // the other boards of the group heard lately
int rt_home_fleet(char ips[][16], char hosts[][20], char ids[][10], int max);   // the same, with each board's firmware build
const char *rt_home_fwid(void);                        // this board's firmware build: 8 hex characters
bool rt_home_token_ok(const char *tok);
const char *rt_home_token(void);
const char *rt_home_secret(void);
bool rt_home_open(void);                               // the group has no password
void rt_home_begin(void);                              // start a group with no password, when not in a group
esp_err_t rt_home_set(const struct cJSON *j, char *why, size_t cap);
int rt_home_json(char *dst, size_t cap);

// push.c - one board gives the firmware it runs to the other boards of the group
void rt_push_boot(void);                               // at start: new firmware came from the page before the restart
void rt_push_mark(void);                               // new firmware came from the page: push it after the restart
bool rt_push_start(char *why, size_t cap);             // push now (the page's button)
int rt_push_json(char *dst, size_t cap);               // {"state":"idle|run|done","boards":[{"host":..,"state":..}]}

// web.c - the board's own page and its JSON API
void rt_web_start(void);
void rt_web_auth_reload(void);                         // the page password or the group password changed

// alert.c - messages to Telegram, a webhook, MQTT and ntfy, sent from their own task
typedef struct {
    bool ok;
    int status;                 // HTTP status, 0 when the request never got an answer
    int ms;
    char detail[96];
} rt_alert_result_t;
void rt_alert_start(void);
uint8_t *rt_snapshot_jpg(size_t *len);                 // the model's current frame, 192x192; caller frees
uint8_t *rt_sound_wav(size_t *len);                    // the last clip of the microphone as a WAV file; caller frees
void rt_alert_reload(void);                            // settings changed
void rt_alert_event(const char *rule, const char *text, bool on, float p, bool photo, bool urgent);
bool rt_alert_ready(void);                             // at least one destination is set
bool rt_alert_idle(void);                              // nothing waits to be sent (power.c, before deep sleep)
// an alert from another board of the group, for this board to send (home.c); false: no room for it now
bool rt_alert_from_peer(const char *host, const char *label, const char *ip, const char *rule, const char *text, bool on,
                        float p, bool photo, bool urgent, long long t);
int rt_alert_share(char *dst, size_t cap);             // copy this board's alert settings to the group (web.c)
bool rt_alert_test(const char *dest, rt_alert_result_t *out);    // "telegram" | "webhook" | "mqtt" | "ntfy" | "probe"; waits
int rt_alert_json(char *dst, size_t cap);              // what is configured, and counters (never the secrets)
bool rt_dev_test(int slot, const char *what, rt_alert_result_t *out, char *dps, size_t dps_cap);   // devices.c: on | off | status
bool rt_dev_try(const struct cJSON *j, rt_alert_result_t *out, char *dps, size_t dps_cap);  // devices.c: a device not saved yet

// power.c - low-power mode, off by default: deep sleep between short looks (checks at points in time)
void rt_power_early(void);                             // after the settings store: may go back to sleep at once
void rt_power_start(void);                             // after start-up, before the decision task
bool rt_power_on(void);                                // low-power mode is set
bool rt_power_net_now(void);                           // join Wi-Fi at start (false: only when something must go out)
void rt_power_need_net(void);                          // something must go out: join Wi-Fi now
void rt_power_touch(void);                             // the page was used: stay awake a little longer (web.c)
bool rt_power_fast_ap(uint8_t bssid[6], uint8_t *channel);   // the network joined last time (wifi.c)
void rt_power_joined(const uint8_t bssid[6], uint8_t channel);
void rt_power_note_sender(const char *ip, const char *host);  // the group's sender ("": it did not answer)
bool rt_power_sender(char *ip, size_t ip_cap, char *host, size_t host_cap);   // the sender heard at an earlier wake
esp_err_t rt_power_set(const struct cJSON *j, char *why, size_t cap);
int rt_power_json(char *dst, size_t cap);
bool rt_power_command(const char *arg, const char *arg2);   // console: power [on|off|stay|go|wakes <n>]

#ifdef __cplusplus
}
#endif
