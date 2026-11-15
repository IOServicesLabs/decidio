// The fly eye, beside the model. The model answers "what is in the picture"; the eye answers "did something move, and
// where". It is the visual system of the fruitfly firmware (fly/): a 40x30 retina, motion between neighbouring
// columns, the camera's own motion taken out, what is left grouped into figures and tracked, and a mushroom body that
// stops reacting to what it keeps seeing (a fan, a curtain). It costs a few milliseconds a frame and no model.
//
//   camera task: every 160x120 sensor frame is pooled 4x4 into the retina (rt_eye_frame565)
//   eye task:    eye -> figures -> lock -> detectors; publishes what the page and the rules read
//
// Rules read it through named inputs (inputs.c):
//   eye_moving       1 while a figure is being tracked, else 0
//   eye_figures      how many figures are tracked (0-4)
//   eye_approach     the strongest looming among them (about 0.012 = something is coming closer)
//   eye_new          1 for 3 s after "new object", else 0
//   eye_changed      1 for 3 s after "the room changed" (something was put down or taken away), else 0
//   eye_self_motion  how much the whole view moves (above about 0.2 the board itself is moving and figures are held back)
// All of them have no value while the eye is off. `eye on|off` on the console and the page's Eye tab switch it (stored).
//
// The eye sees motion only: a person who stands still disappears from it. Its arrays live in PSRAM; the fruitfly
// firmware kept them in internal RAM and ran at 18-23 frames a second, so here it is slower (see `eye` on the console).
#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "rt.h"

static void *fly_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
#define FLY_MALLOC(n) fly_alloc(n)
#define FLY_MALLOC_FAST(n) fly_alloc(n)
#include "fly/fly_detectors.h"

static const char *TAG = "eye";
#define EW flyeye::EW
#define EH flyeye::EH
#define NE flyeye::NE
#define LOG_N 16

static bool s_on;
static volatile int64_t s_read_us;       // when the page last asked for the picture
static volatile bool s_busy;                 // the eye task is working on a frame: the camera skips this one
static float *s_in;                          // the next retina, written by the camera task
static uint8_t *s_full;                      // the same frame in full (160x120 RGB565), kept while the page looks: the fovea is cut from it
static volatile bool s_full_ok;
#define FOV 64                               // the fovea: FOV x FOV pixels, RGB565 as the sensor sends it
// The sharp close-up: where the sensor can, it zooms onto the followed figure for one frame (camera.c).
#define SHARP_BYTES (2 * flyeye::W * flyeye::H)
#define SHARP_EVERY_US 4000000               // at most this often: each one takes the wide view away for about 0.6 seconds
static uint8_t *s_sharp;
static struct { uint8_t seq; uint16_t id; uint8_t x, y, w, h; int64_t at_us; uint16_t took_ms; } s_sharp_info;
static int64_t s_sharp_asked_us;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;             // guards everything below (written by the eye task, read by the web server and the rules)
static uint8_t *s_pkt;                       // the page's packet, see rt_eye_packet
static size_t s_pkt_len;
static struct {
    float moving, figures, approach, self_motion;
    int64_t new_us, changed_us;
    uint32_t frames, skipped;
    float ms, fps;
} s_now;
static struct { uint32_t ms; uint8_t kind, x, y; uint16_t id; char text[64]; } s_log[LOG_N];
static uint32_t s_n_log;
static fly::Fly s_nose(20260913);

static void on_event(const flyvision::Event &e)
{
    const int64_t now = esp_timer_get_time();
    if (e.kind == flyvision::NEW_OBJECT) {
        s_now.new_us = now;
    } else if (e.kind == flyvision::ROOM_CHANGED) {
        s_now.changed_us = now;
    }
    auto &l = s_log[s_n_log % LOG_N];
    l.ms = e.ms;
    l.kind = e.kind;
    l.x = e.x;
    l.y = e.y;
    l.id = e.id;
    snprintf(l.text, sizeof(l.text), "%s", e.text);
    s_n_log++;
    printf("{\"eye\":\"%s\",\"text\":\"%s\",\"x\":%d,\"y\":%d,\"bearing\":%.0f,\"ms\":%lu}\n",
           e.kind < flyvision::N_KINDS ? flyvision::KIND_NAME[e.kind] : "event", e.text, e.x, e.y, e.azimuth, (unsigned long)e.ms);
}

static inline uint8_t to_byte(float v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// The page's packet (little-endian), about 5 KB:
//   "FE" u8 version=1 u8 flags (1 self-moving, 2 light change, 4 gated, 8 locked)
//   u16 frames(low) u16 fps*10 u16 ms*10 per frame  f32 hs vs expansion roll wide threshold
//   EW*EH bytes brightness; EW*EH flow x (128 + 64*v); EW*EH flow y; EW*EH figure residual (255 = twice the threshold)
//   u8 nTracks, per track 16 bytes: u8 state, u16 id, u8 x, y, x0, y0, x1, y1, u16 age, u16 size, u8 novelty*255, u8 loom (128 + 2000*loom), u8 bits (1 announced, 2 locked)
//   the lock: u8 active, u8 x0, y0, x1, y1, u8 match*255
//   the fovea: u8 present; if present u16 id, u8 x, y (top left, sensor pixels), u8 side (sensor pixels), u8 FOV,
//              then FOV*FOV pixels RGB565 (high byte first): the square around the followed figure at full detail

// Which figure to look at closely: the locked one, else the largest figure that is followed now. Gives the square
// (sensor pixels) around it; false = nothing moves.
static bool fovea_target(uint16_t *id, int *x, int *y, int *side)
{
    float x0, y0, x1, y1;
    if (flylock::active()) {
        x0 = flylock::x0; y0 = flylock::y0; x1 = flylock::x1; y1 = flylock::y1;
        *id = flylock::id;
    } else {
        const flyfig::Track *best = nullptr;
        for (int t = 0; t < flyfig::MAX_TRACKS; t++) {
            const flyfig::Track &tr = flyfig::tracks[t];
            if ((tr.state == flyfig::TRACKING || tr.state == flyfig::CONFIRMED) && tr.age >= 4 && !tr.missing &&
                (!best || tr.size > best->size)) {
                best = &tr;
            }
        }
        if (!best || flyfig::gated) {
            return false;
        }
        x0 = best->x0; y0 = best->y0; x1 = best->x1; y1 = best->y1;
        *id = best->id;
    }
    const int px = (int)(4 * x0), py = (int)(4 * y0), pw = (int)(4 * (x1 - x0 + 1)), ph = (int)(4 * (y1 - y0 + 1));
    int s = (pw > ph ? pw : ph) + 16;                       // a margin of two columns on each side
    s = s < 48 ? 48 : s > flyeye::H ? flyeye::H : s;
    int cx = px + pw / 2 - s / 2, cy = py + ph / 2 - s / 2;
    cx = cx < 0 ? 0 : cx > flyeye::W - s ? flyeye::W - s : cx;
    cy = cy < 0 ? 0 : cy > flyeye::H - s ? flyeye::H - s : cy;
    *x = cx; *y = cy; *side = s;
    return true;
}

static uint8_t *fovea(uint8_t *p)
{
    uint16_t id;
    int x, y, side;
    if (!s_full_ok || !fovea_target(&id, &x, &y, &side)) {
        *p++ = 0;
        return p;
    }
    *p++ = 1;
    memcpy(p, &id, 2);
    p += 2;
    *p++ = (uint8_t)x;
    *p++ = (uint8_t)y;
    *p++ = (uint8_t)side;
    *p++ = FOV;
    for (int j = 0; j < FOV; j++) {
        const uint8_t *row = s_full + 2 * ((y + j * side / FOV) * flyeye::W + x);
        for (int i = 0; i < FOV; i++) {
            const uint8_t *q = row + 2 * (i * side / FOV);
            *p++ = q[0];
            *p++ = q[1];
        }
    }
    return p;
}

// camera task: the zoomed frame has arrived
static void sharp_cb(const uint8_t *rgb565, uint16_t id, int x, int y, int w, int h, int took_ms)
{
    if (!s_sharp) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_sharp, rgb565, SHARP_BYTES);
    s_sharp_info.seq = (uint8_t)(s_sharp_info.seq + 1 ? s_sharp_info.seq + 1 : 1);       // 0 = none yet
    s_sharp_info.id = id;
    s_sharp_info.x = (uint8_t)x;
    s_sharp_info.y = (uint8_t)y;
    s_sharp_info.w = (uint8_t)w;
    s_sharp_info.h = (uint8_t)h;
    s_sharp_info.at_us = esp_timer_get_time();
    s_sharp_info.took_ms = (uint16_t)took_ms;
    xSemaphoreGive(s_lock);
}

// ask for a sharp close-up of the followed figure, now and then, while somebody looks
// The lock keeps its window while the picture in it still looks the same, also when the figure has left and the
// window shows the wall (seen on the board: a lock sat on a corner for hours, and the camera zoomed onto it every
// 4 seconds). So a lock that nothing has moved in for a while is let go: 10 s if the eye chose it, 60 s if the user did.
static int64_t s_lock_moved_us;
static void lock_watch(int64_t now)
{
    if (!flylock::active()) {
        s_lock_moved_us = now;
        return;
    }
    for (int t = 0; t < flyfig::MAX_TRACKS; t++) {
        const flyfig::Track &c = flyfig::tracks[t];
        if ((c.state == flyfig::TRACKING || c.state == flyfig::CONFIRMED) && !c.missing && c.x0 <= flylock::x1 + 2 &&
            c.x1 >= flylock::x0 - 2 && c.y0 <= flylock::y1 + 2 && c.y1 >= flylock::y0 - 2) {
            s_lock_moved_us = now;
            return;
        }
    }
    if (now - s_lock_moved_us > (flylock::manual ? 60000000 : 10000000)) {
        flylock::unlock();                                  // an automatic lock then takes the next figure that is announced
        s_lock_moved_us = now;
    }
}

static void sharp_request(int64_t now)
{
    uint16_t id;
    int x, y, side;
    if (flylock::active() && now - s_lock_moved_us > 3000000) {
        return;                                             // nothing moves there now: no zoom
    }
    if (!s_sharp || !rt_cam_can_zoom() || now - s_sharp_asked_us < SHARP_EVERY_US || !fovea_target(&id, &x, &y, &side)) {
        return;
    }
    int w = side * 4 / 3;                                   // a 4:3 part that holds the square
    w = w > 78 ? 78 : w;                                    // the sensor's window is at most 1000 of its pixels wide
    const int h = w * 3 / 4;
    int px = x + side / 2 - w / 2, py = y + side / 2 - h / 2;
    px = px < 0 ? 0 : px > flyeye::W - w ? flyeye::W - w : px;
    py = py < 0 ? 0 : py > flyeye::H - h ? flyeye::H - h : py;
    if (rt_cam_zoom(px, py, w, id, sharp_cb)) {
        s_sharp_asked_us = now;
    }
}

static void publish(float ms)
{
    uint8_t *p = s_pkt;
    *p++ = 'F';
    *p++ = 'E';
    *p++ = 1;
    *p++ = (uint8_t)((flyfig::selfMoving ? 1 : 0) | (flyfig::lightChange ? 2 : 0) | (flyfig::gated ? 4 : 0) | (flylock::active() ? 8 : 0));
    const uint16_t h[3] = { (uint16_t)flyeye::frames, (uint16_t)(s_now.fps * 10), (uint16_t)(ms * 10) };
    memcpy(p, h, 6);
    p += 6;
    const float f[6] = { flyeye::hs, flyeye::vs, flyeye::expansion, flyeye::roll, flyfig::wide, flyfig::threshold };
    memcpy(p, f, 24);
    p += 24;
    for (int i = 0; i < NE; i++) {
        *p++ = to_byte(flyeye::fine.r[i]);
    }
    for (int i = 0; i < NE; i++) {
        *p++ = to_byte(128 + 64 * flyeye::flowX[i]);
    }
    for (int i = 0; i < NE; i++) {
        *p++ = to_byte(128 + 64 * flyeye::flowY[i]);
    }
    const float th = 2 * flyfig::threshold + 1e-6f;
    for (int i = 0; i < NE; i++) {
        *p++ = to_byte(255 * flyfig::resid[i] / th);
    }
    *p++ = flyfig::MAX_TRACKS;
    for (int t = 0; t < flyfig::MAX_TRACKS; t++) {
        const flyfig::Track &tr = flyfig::tracks[t];
        *p++ = tr.state;
        memcpy(p, &tr.id, 2);
        p += 2;
        *p++ = (uint8_t)(tr.x + 0.5f);
        *p++ = (uint8_t)(tr.y + 0.5f);
        *p++ = tr.x0;
        *p++ = tr.y0;
        *p++ = tr.x1;
        *p++ = tr.y1;
        memcpy(p, &tr.age, 2);
        p += 2;
        memcpy(p, &tr.size, 2);
        p += 2;
        *p++ = to_byte(255 * tr.novelty);
        *p++ = to_byte(128 + 2000 * tr.loom);
        *p++ = (uint8_t)((tr.fired ? 1 : 0) | (flylock::id && flylock::id == tr.id ? 2 : 0));
    }
    *p++ = flylock::active() ? 1 : 0;
    *p++ = to_byte(flylock::x0);
    *p++ = to_byte(flylock::y0);
    *p++ = to_byte(flylock::x1);
    *p++ = to_byte(flylock::y1);
    *p++ = to_byte(255 * flylock::match);
    p = fovea(p);
    *p++ = s_sharp_info.seq;                                // changes when there is a new sharp close-up to fetch
    s_pkt_len = (size_t)(p - s_pkt);
}
#define PKT_CAP (4 + 6 + 24 + 4 * NE + 1 + flyfig::MAX_TRACKS * 16 + 6 + 7 + 2 * FOV * FOV + 1)

static void eye_task(void *arg)
{
    int64_t window_us = esp_timer_get_time();
    uint32_t window_n = 0;
    float ms_avg = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t t0 = esp_timer_get_time();
        memcpy(flyeye::fine.r, s_in, sizeof(float) * NE);
        xSemaphoreTake(s_lock, portMAX_DELAY);           // the lock, a click on the page, reaches into the tracks
        flyeye::process();
        flyvision::afterSee((uint32_t)(t0 / 1000));
        lock_watch(t0);
        const int64_t t1 = esp_timer_get_time();
        const float ms = (t1 - t0) / 1000.0f;
        ms_avg = ms_avg ? 0.9f * ms_avg + 0.1f * ms : ms;
        window_n++;
        if (t1 - window_us >= 2000000) {
            s_now.fps = window_n * 1e6f / (float)(t1 - window_us);
            window_us = t1;
            window_n = 0;
        }
        int n = 0;
        float loom = 0;
        for (int t = 0; t < flyfig::MAX_TRACKS; t++) {
            const flyfig::Track &tr = flyfig::tracks[t];
            if ((tr.state == flyfig::TRACKING || tr.state == flyfig::CONFIRMED) && tr.age >= 4 && !tr.missing) {
                n++;
                loom = tr.loom > loom ? tr.loom : loom;
            }
        }
        s_now.figures = flyfig::gated ? 0 : n;
        s_now.moving = s_now.figures > 0 ? 1 : 0;
        s_now.approach = flyfig::gated ? 0 : loom;
        s_now.self_motion = flyfig::wide;
        s_now.frames = flyeye::frames;
        s_now.ms = ms_avg;
        if (s_read_us && t1 - s_read_us < 3000000) {     // only while somebody looks at the page
            publish(ms_avg);
            sharp_request(t1);
        }
        xSemaphoreGive(s_lock);
        s_busy = false;
    }
}

// camera task: one 160x120 RGB565 frame (big-endian pixels, as the sensor sends them) into the retina, 4x4 pixels a column
extern "C" void rt_eye_frame565(const uint8_t *src)
{
    if (!s_on) {
        return;
    }
    if (s_busy) {
        s_now.skipped++;
        return;
    }
    for (int y = 0; y < EH; y++) {
        for (int x = 0; x < EW; x++) {
            uint32_t sum = 0;
            const uint8_t *p = src + 2 * ((y * 4) * flyeye::W + x * 4);
            for (int j = 0; j < 4; j++, p += 2 * flyeye::W) {
                for (int i = 0; i < 4; i++) {
                    const uint16_t v = (uint16_t)(p[2 * i] << 8 | p[2 * i + 1]);
                    // BT.601 luma of the 5-6-5 fields scaled to 0-255: the same grey the model's frames use
                    sum += ((v >> 11) & 0x1f) * 633 + ((v >> 5) & 0x3f) * 607 + (v & 0x1f) * 239;
                }
            }
            s_in[y * EW + x] = sum / (16.0f * 256.0f);
        }
    }
    const int64_t read = s_read_us;
    s_full_ok = s_full && read && esp_timer_get_time() - read < 3000000;      // the full frame only while the page looks
    if (s_full_ok) {
        memcpy(s_full, src, 2 * flyeye::W * flyeye::H);
    }
    s_busy = true;
    xTaskNotifyGive(s_task);
}

static float in_value(float v)
{
    return s_on ? v : NAN;
}
static float in_moving(void) { return in_value(s_now.moving); }
static float in_figures(void) { return in_value(s_now.figures); }
static float in_approach(void) { return in_value(s_now.approach); }
static float in_self(void) { return in_value(s_now.self_motion); }
static float in_new(void) { return in_value(s_now.new_us && esp_timer_get_time() - s_now.new_us < 3000000 ? 1 : 0); }
static float in_changed(void) { return in_value(s_now.changed_us && esp_timer_get_time() - s_now.changed_us < 3000000 ? 1 : 0); }

// before the rules are read: the inputs exist whether or not the eye runs (they have no value while it is off)
extern "C" void rt_eye_inputs(void)
{
    rt_input_register("eye_moving", "", 0, 1, in_moving);
    rt_input_register("eye_figures", "", 0, 4, in_figures);
    rt_input_register("eye_approach", "", 0, 0.05f, in_approach);
    rt_input_register("eye_new", "", 0, 1, in_new);
    rt_input_register("eye_changed", "", 0, 1, in_changed);
    rt_input_register("eye_self_motion", "", 0, 0.5f, in_self);
}

extern "C" bool rt_eye_wanted(void)
{
    char v[4];
    return !rt_store_get("eye", v, sizeof(v)) || v[0] != '0';       // on unless switched off
}

// after the camera has started. Does nothing without a camera, without PSRAM, or when switched off.
extern "C" void rt_eye_start(void)
{
    if (s_on || !g_board.has_cam || !rt_eye_wanted() || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 600 * 1024) {
        return;
    }
    fly::Config cfg;
    s_in = (float *)fly_alloc(sizeof(float) * NE);
    s_pkt = (uint8_t *)fly_alloc(PKT_CAP);
    s_full = (uint8_t *)heap_caps_malloc(2 * flyeye::W * flyeye::H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // without it: no fovea
    s_sharp = (uint8_t *)heap_caps_malloc(SHARP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);                // without it: no sharp close-up
    s_lock = xSemaphoreCreateMutex();
    if (!s_in || !s_pkt || !s_lock || !s_nose.init(cfg) || !flyvision::begin(&s_nose)) {
        ESP_LOGE(TAG, "not enough memory for the eye");
        return;
    }
    flydet::addDefaults();
    flyvision::onEvent = on_event;
    // the stack is in PSRAM too: this task never writes flash (the settings store is written by whoever switches the eye)
    if (xTaskCreatePinnedToCoreWithCaps(eye_task, "eye", 8192, NULL, 5, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "eye task");
        return;
    }
    s_on = true;
}

extern "C" bool rt_eye_on(void)
{
    return s_on;
}

// kept across restarts; switching on needs a restart only if the eye never started
extern "C" void rt_eye_want(bool on)
{
    rt_store_set("eye", on ? NULL : "0");
    if (on) {
        rt_eye_start();
    } else {
        s_on = false;
    }
}

// the page's packet; 0 = the eye is off. The first call after a pause returns the packet of the next frame.
extern "C" size_t rt_eye_packet(uint8_t *dst, size_t cap)
{
    if (!s_on) {
        return 0;
    }
    s_read_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = s_pkt_len <= cap ? s_pkt_len : 0;
    memcpy(dst, s_pkt, n);
    xSemaphoreGive(s_lock);
    return n;
}

// The last sharp close-up for the page:
//   "FS" u8 version=1 u8 seq  u16 id  u8 x, y, w, h (the part of the 160x120 frame it shows)  u32 age ms  u16 ms the
//   wide view was away  u16 0, then 160x120 pixels RGB565 (high byte first). 0 = none yet.
extern "C" size_t rt_eye_sharp(uint8_t *dst, size_t cap)
{
    if (!s_on || !s_sharp || cap < 16 + SHARP_BYTES) {
        return 0;
    }
    s_read_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = 0;
    if (s_sharp_info.seq) {
        const uint32_t age = (uint32_t)((esp_timer_get_time() - s_sharp_info.at_us) / 1000);
        const uint16_t zero = 0;
        dst[0] = 'F'; dst[1] = 'S'; dst[2] = 1; dst[3] = s_sharp_info.seq;
        memcpy(dst + 4, &s_sharp_info.id, 2);
        dst[6] = s_sharp_info.x; dst[7] = s_sharp_info.y; dst[8] = s_sharp_info.w; dst[9] = s_sharp_info.h;
        memcpy(dst + 10, &age, 4);
        memcpy(dst + 14, &s_sharp_info.took_ms, 2);
        (void)zero;
        memcpy(dst + 16, s_sharp, SHARP_BYTES);
        n = 16 + SHARP_BYTES;
    }
    xSemaphoreGive(s_lock);
    return n;
}

// the last full frame as the sensor sent it, for checking the zoom against the wide view; 0 = nobody looks
extern "C" size_t rt_eye_wide(uint8_t *dst, size_t cap)
{
    if (!s_on || !s_full || !s_full_ok || cap < (size_t)SHARP_BYTES) {
        return 0;
    }
    memcpy(dst, s_full, SHARP_BYTES);                       // the camera task may be writing it: a check, not a record
    return SHARP_BYTES;
}

// console: zoom onto this part of the frame now (x, y, w in frame pixels)
extern "C" bool rt_eye_zoom_test(int x, int y, int w)
{
    return s_on && s_sharp && rt_cam_zoom(x, y, w, 0, sharp_cb);
}

// lock onto a figure: id > 0 that track, 0 = let go, -1 = the eye chooses (the first figure it announces)
extern "C" bool rt_eye_lock(int id)
{
    if (!s_on) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = true;
    if (id > 0) {
        flylock::automatic = false;
        ok = flylock::lock((uint16_t)id);
    } else {
        flylock::unlock();
        flylock::automatic = id < 0;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

// {"on":true,"fps":..,"ms":..,"frames":..,"skipped":..,"figures":..,"events":[{"ms":..,"kind":"new object","text":"..","x":..,"y":..},...]} newest last
extern "C" int rt_eye_json(char *dst, size_t cap)
{
    int n = 0;
    ADD("{\"on\":%s,\"wanted\":%s", s_on ? "true" : "false", rt_eye_wanted() ? "true" : "false");
    if (s_on) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ADD(",\"fps\":%.1f,\"ms\":%.1f,\"frames\":%lu,\"skipped\":%lu,\"figures\":%d,\"self_motion\":%.3f,\"now_ms\":%lu,\"events\":[",
            s_now.fps, s_now.ms, (unsigned long)s_now.frames, (unsigned long)s_now.skipped, (int)s_now.figures, s_now.self_motion,
            (unsigned long)(esp_timer_get_time() / 1000));
        const uint32_t first = s_n_log > LOG_N ? s_n_log - LOG_N : 0;
        for (uint32_t k = first; k < s_n_log; k++) {
            const auto &l = s_log[k % LOG_N];
            ADD("%s{\"ms\":%lu,\"kind\":\"%s\",\"text\":\"%s\",\"x\":%d,\"y\":%d,\"id\":%d}", k > first ? "," : "", (unsigned long)l.ms,
                l.kind < flyvision::N_KINDS ? flyvision::KIND_NAME[l.kind] : "event", l.text, l.x, l.y, l.id);
        }
        xSemaphoreGive(s_lock);
        ADD("]");
    }
    ADD("}");
    return n < (int)cap ? n : (int)cap - 1;
}
