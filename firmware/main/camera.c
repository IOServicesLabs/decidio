// Camera: QQVGA (160x120) RGB565 from the sensor -> center 120x120 -> 96x96 (bilinear) -> ring of gray or RGB frames.
// Gray is the integer BT.601 luma of the same RGB the collect mode streams, so training and running see one pipeline.
// The ring holds lag+2 frames: the model reads "now" and "motion_ms ago" in place while the next frame is written
// into the slot nobody is reading.
#include <string.h>

#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rt.h"
#include "sdkconfig.h"

#define SRC_W 160
#define SRC_H 120
#define CROP 120
#define CROP_X ((SRC_W - CROP) / 2)

typedef struct { const char *name; int8_t xclk, siod, sioc, d7, d6, d5, d4, d3, d2, d1, d0, vsync, href, pclk; } pins_t;
// every camera pin layout the firmware knows; board.c's profiles name a row by its number
static const pins_t PINS[] = {
    { "xiao",       10, 40, 39, 48, 11, 12, 14, 16, 18, 17, 15, 38, 47, 13 },
    { "s3-eye",     15, 4, 5, 16, 17, 18, 12, 10, 8, 9, 11, 6, 7, 13 },
    { "s3-cam-lcd", 40, 17, 18, 39, 41, 42, 12, 3, 14, 47, 13, 21, 38, 11 },
};
#define N_PINS ((int)(sizeof(PINS) / sizeof(PINS[0])))

static const char *TAG = "cam";
static uint8_t *s_ring;
static int s_slots, s_colors, s_lag, s_period_ms, s_frame_bytes;
static volatile uint32_t s_count;
static SemaphoreHandle_t s_lock;
static uint16_t s_pid;
static bool s_probed;
static volatile bool s_stop, s_stopped;    // rt_cam_standby: the camera task takes no more frames
static int16_t s_map[RT_CAM][2];     // per output coordinate: source index (fixed 8.8) for bilinear

static int board_orient(void)
{
    if (g_board.orient >= 0) {
        return g_board.orient;                // the XIAO ribbon puts the picture upside down (flip + mirror)
    }
    return s_pid == OV3660_PID ? 1 : 0;      // the S3-CAM's OV3660 is mounted flipped
}

bool rt_cam_uses_pin(int map, int gpio)
{
    if (map < 0 || map >= N_PINS) {
        return false;
    }
    const int8_t *p = &PINS[map].xclk;                      // the 14 pin numbers follow the name
    for (int i = 0; i < 14; i++) {
        if (p[i] == gpio) {
            return true;
        }
    }
    return false;
}

int rt_cam_maps(void)
{
    return N_PINS;
}

const char *rt_cam_map_name(int map)
{
    return map >= 0 && map < N_PINS ? PINS[map].name : "";
}

esp_err_t rt_cam_probe(int map)
{
    if (map < 0 || map >= N_PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    const pins_t *p = &PINS[map];
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = p->xclk, .pin_sccb_sda = p->siod, .pin_sccb_scl = p->sioc,
        .pin_d7 = p->d7, .pin_d6 = p->d6, .pin_d5 = p->d5, .pin_d4 = p->d4,
        .pin_d3 = p->d3, .pin_d2 = p->d2, .pin_d1 = p->d1, .pin_d0 = p->d0,
        .pin_vsync = p->vsync, .pin_href = p->href, .pin_pclk = p->pclk,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QQVGA, .jpeg_quality = 12, .fb_count = 2,
        .fb_location = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    const esp_err_t err = esp_camera_init(&c);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "camera on pin map %s", p->name);
        sensor_t *s = esp_camera_sensor_get();
        s_pid = s ? s->id.PID : 0;
        s_probed = true;
    }
    return err;
}

void rt_cam_set_orient(int o)
{
    sensor_t *s = esp_camera_sensor_get();
    if (o < 0 || o > 3) {
        o = board_orient();
    }
    if (s) {
        s->set_vflip(s, o & 1);
        s->set_hmirror(s, (o >> 1) & 1);
    }
}

static inline void px_rgb(const uint8_t *src, int x, int y, int *r, int *g, int *b)
{
    const uint16_t v = (uint16_t)(src[2 * (y * SRC_W + x)] << 8 | src[2 * (y * SRC_W + x) + 1]);   // RGB565 big endian
    *r = ((v >> 11) & 0x1f) * 255 / 31;
    *g = ((v >> 5) & 0x3f) * 255 / 63;
    *b = (v & 0x1f) * 255 / 31;
}

static void resample(const uint8_t *src, uint8_t *dst)
{
    for (int oy = 0; oy < RT_CAM; oy++) {
        const int sy = s_map[oy][0], fy = s_map[oy][1];
        const int y0 = sy, y1 = sy + 1 < SRC_H ? sy + 1 : sy;
        for (int ox = 0; ox < RT_CAM; ox++) {
            const int sx = CROP_X + s_map[ox][0], fx = s_map[ox][1];
            const int x1 = sx + 1 < SRC_W ? sx + 1 : sx;
            int r[4], g[4], b[4];
            px_rgb(src, sx, y0, &r[0], &g[0], &b[0]);
            px_rgb(src, x1, y0, &r[1], &g[1], &b[1]);
            px_rgb(src, sx, y1, &r[2], &g[2], &b[2]);
            px_rgb(src, x1, y1, &r[3], &g[3], &b[3]);
            if (s_colors == 1) {
                int v[4];
                for (int k = 0; k < 4; k++) {
                    v[k] = (r[k] * 77 + g[k] * 150 + b[k] * 29) >> 8;
                }
                const int top = v[0] * (256 - fx) + v[1] * fx, bot = v[2] * (256 - fx) + v[3] * fx;
                *dst++ = (uint8_t)((top * (256 - fy) + bot * fy + (1 << 15)) >> 16);
            } else {
                const int *ch[3] = { r, g, b };
                for (int k = 0; k < 3; k++) {
                    const int *v = ch[k];
                    const int top = v[0] * (256 - fx) + v[1] * fx, bot = v[2] * (256 - fx) + v[3] * fx;
                    *dst++ = (uint8_t)((top * (256 - fy) + bot * fy + (1 << 15)) >> 16);
                }
            }
        }
    }
}

// ---- the zoom ---------------------------------------------------------------------------------------------------
// The wide view reads the whole sensor (2048x1536), takes every second pixel and scales that to 160x120: one frame
// pixel is 12.8 sensor pixels. For a close-up the sensor reads only a window, every pixel of it, and scales that to
// the same 160x120, so the frame size (which the camera driver cannot change while it runs) stays as it is. The line
// and frame timing stay the wide view's, so the frame rate and the exposure do not change. Only the OV3660 is
// driven this way; with another sensor the close-up stays the enlarged cut of the wide frame.
#define SENSOR_PER_PX 12.8f
#define ZOOM_SKIP 3                      // frames thrown away after each change of the window (frames in flight, exposure)
static struct { volatile bool want; int x, y, w; uint16_t id; rt_cam_zoom_fn fn; } s_zoom;

bool rt_cam_can_zoom(void)
{
    return s_pid == OV3660_PID;
}

bool rt_cam_zoom(int x, int y, int w, uint16_t id, rt_cam_zoom_fn fn)
{
    if (!rt_cam_can_zoom() || s_zoom.want || !fn) {
        return false;
    }
    s_zoom.x = x;
    s_zoom.y = y;
    s_zoom.w = w;
    s_zoom.id = id;
    s_zoom.fn = fn;
    s_zoom.want = true;
    return true;
}

// a test zoom (id 0, from the console) prints the mean colour of each frame around it: exposure and colour checks
static void trace(const char *what, const camera_fb_t *fb)
{
    if (s_zoom.id || !fb) {
        return;
    }
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (size_t i = 0; i + 1 < fb->len; i += 2 * 7) {
        const uint16_t v = (uint16_t)(fb->buf[i] << 8 | fb->buf[i + 1]);
        r += v >> 11;
        g += v >> 5 & 63;
        b += v & 31;
        n++;
    }
    printf("{\"zoomtrace\":\"%s\",\"rgb\":[%d,%d,%d],\"ms\":%d}\n", what, (int)(r * 255 / 31 / n), (int)(g * 255 / 63 / n),
           (int)(b * 255 / 31 / n), (int)(esp_timer_get_time() / 1000));
}

static void skip_frames(int n, const char *what)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            trace(what, fb);
            esp_camera_fb_return(fb);
        }
    }
}

static void zoom_once(void)
{
    sensor_t *s = esp_camera_sensor_get();
    const int64_t t0 = esp_timer_get_time();
    // The window on the sensor, 4:3. Its width is a multiple of 16, thus its height is a multiple of 12: with an odd
    // number of rows the sensor's colour pattern shifts and red things come out blue (seen on the board).
    int ww = (int)(s_zoom.w * SENSOR_PER_PX) & ~15;
    ww = ww < SRC_W ? SRC_W : ww > 992 ? 992 : ww;          // 160 = one sensor pixel per frame pixel; above 1000 the frame timing would not fit
    const int wh = ww * 3 / 4;
    const float fw = ww / SENSOR_PER_PX, fh = wh / SENSOR_PER_PX;         // its size in frame pixels
    float fx = s_zoom.x + (s_zoom.w - fw) / 2, fy = s_zoom.y + (s_zoom.w * 3 / 4 - fh) / 2;
    fx = fx < 0 ? 0 : fx > SRC_W - fw ? SRC_W - fw : fx;
    fy = fy < 0 ? 0 : fy > SRC_H - fh ? SRC_H - fh : fy;
    // a mirrored or flipped picture is read from the other end of the sensor
    const float ax = s->status.hmirror ? SRC_W - fx - fw : fx, ay = s->status.vflip ? SRC_H - fy - fh : fy;
    // the window starts on a multiple of 4 sensor pixels, for the same reason
    const int sx = 16 + ((int)(ax * SENSOR_PER_PX) & ~3), sy = 6 + ((int)(ay * SENSOR_PER_PX) & ~3);
    skip_frames(s_zoom.id ? 0 : 2, "before");
    const bool ok = s && s->set_res_raw &&
                    s->set_res_raw(s, sx - 16, sy - 6, sx + ww + 15, sy + wh + 5, 16, 6, 2300, 783, SRC_W, SRC_H, ww != SRC_W, false) == 0;
    if (ok) {
        skip_frames(ZOOM_SKIP, "zoom skip");
        camera_fb_t *fb = esp_camera_fb_get();
        trace("zoom", fb);
        if (fb && fb->width == SRC_W && fb->height == SRC_H && fb->len >= (size_t)SRC_W * SRC_H * 2) {
            s_zoom.fn(fb->buf, s_zoom.id, (int)(fx + 0.5f), (int)(fy + 0.5f), (int)(fw + 0.5f), (int)(fh + 0.5f),
                      (int)((esp_timer_get_time() - t0) / 1000));
        }
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
    // back to the wide view, exactly as the driver sets QQVGA: the whole sensor, every second pixel
    if (s && s->set_res_raw) {
        s->set_res_raw(s, 0, 0, 2079, 1547, 8, 2, 2300, 783, SRC_W, SRC_H, true, true);
        skip_frames(ZOOM_SKIP + 3, "back skip");      // measured: the exposure of the wide view is steady again after 3 (XIAO) to 5 (S3-CAM)
        skip_frames(s_zoom.id ? 0 : 10, "back");
    }
    s_zoom.want = false;
}

static void cam_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        if (s_stop) {
            s_stopped = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_zoom.want) {
            zoom_once();                          // the model and the eye get no frame meanwhile (about 0.6 seconds)
            last = xTaskGetTickCount();
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            const size_t want = (size_t)SRC_W * SRC_H * 2;
            if (fb->width == SRC_W && fb->height == SRC_H && fb->len >= want) {
                rt_eye_frame565(fb->buf);                 // the fly eye takes the whole frame (it returns at once if it is off or busy)
                resample(fb->buf, s_ring + (size_t)(s_count % s_slots) * s_frame_bytes);   // free slot
                xSemaphoreTake(s_lock, portMAX_DELAY);    // publish: the decision task may be reading older slots
                s_count++;
                xSemaphoreGive(s_lock);
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(s_period_ms));
    }
}

esp_err_t rt_cam_start(int colors, int lag, int frame_ms)
{
    s_colors = colors == 3 ? 3 : 1;
    s_lag = lag;
    s_slots = lag + 2;
    s_period_ms = frame_ms > 0 ? frame_ms : 50;
    s_frame_bytes = RT_CAM * RT_CAM * s_colors;
    for (int i = 0; i < RT_CAM; i++) {
        const int fp = ((2 * i + 1) * CROP * 256) / (2 * RT_CAM) - 128;     // centre-aligned sample position, 8.8
        const int c = fp < 0 ? 0 : fp;
        s_map[i][0] = (int16_t)(c >> 8);
        s_map[i][1] = (int16_t)(c & 0xff);
    }
    ESP_RETURN_ON_FALSE(s_probed, ESP_ERR_NOT_FOUND, TAG, "no camera found");
    sensor_t *s = esp_camera_sensor_get();
    if (s_pid == OV3660_PID && s) {
        s->set_brightness(s, 1);              // the OV3660 comes up dark
    }
    int orient = -1;
    nvs_handle_t nv;
    if (nvs_open("decidio", NVS_READONLY, &nv) == ESP_OK) {
        int8_t v;
        if (nvs_get_i8(nv, "orient", &v) == ESP_OK) {
            orient = v;
        }
        nvs_close(nv);
    }
    rt_cam_set_orient(orient);
    for (int i = 0; i < 6; i++) {             // let auto-exposure settle
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
    const size_t ring = (size_t)s_slots * s_frame_bytes;
    const bool big = ring > 64 * 1024;        // big rings (RGB, long lag) prefer PSRAM to keep SRAM for the arena
    s_ring = heap_caps_malloc(ring, big ? MALLOC_CAP_SPIRAM : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_ring) {
        s_ring = heap_caps_malloc(ring, big ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : MALLOC_CAP_SPIRAM);
    }
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "frame ring");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    return xTaskCreatePinnedToCore(cam_task, "cam", 4096, NULL, 6, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool rt_cam_acquire(const uint8_t **cur, const uint8_t **prev, uint32_t *seq)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t n = s_count;
    if (n <= (uint32_t)s_lag) {
        xSemaphoreGive(s_lock);
        return false;
    }
    *cur = s_ring + (size_t)((n - 1) % s_slots) * s_frame_bytes;
    *prev = s_ring + (size_t)((n - 1 - s_lag) % s_slots) * s_frame_bytes;
    if (seq) {
        *seq = n;
    }
    return true;
}

void rt_cam_release(void)
{
    xSemaphoreGive(s_lock);
}

// Before deep sleep. The XIAO and S3-CAM pin maps have no power-down pin, so the sensor stays powered in deep sleep;
// its standby mode cuts most of its current (OV2640: COM2 bit 4 in the sensor bank; OV3660 / OV5640: 0x3008 bit 6).
void rt_cam_standby(void)
{
    if (!s_probed) {
        return;
    }
    s_stop = true;
    for (int i = 0; i < 30 && s_lock && !s_stopped; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_reg) {
        return;
    }
    if (s_pid == OV2640_PID) {
        s->set_reg(s, 0x100 | 0x09, 0x10, 0x10);
    } else if (s_pid == OV3660_PID || s_pid == OV5640_PID) {
        s->set_reg(s, 0x3008, 0x40, 0x40);
    }
}

int rt_cam_colors(void)
{
    return s_ring ? s_colors : 0;
}

const char *rt_cam_sensor(void)
{
    return s_pid == OV2640_PID ? "OV2640" : s_pid == OV3660_PID ? "OV3660" : s_pid == OV5640_PID ? "OV5640" : (s_pid ? "other" : "none");
}
