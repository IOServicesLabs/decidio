// decidio runtime
//   core 0: camera task (frame ring), mic task (streaming log-mel), console
//   core 1: decision loop - runs as soon as new input lands (every audio hop, or every camera frame if image-only)
//   low power (power.c, off by default): the board sleeps between short looks and joins Wi-Fi only to send
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "rt.h"

static const char *TAG = "decidio";
static bool s_model_ok, s_cam_ok, s_mic_ok;
static bool s_blind;                // the model looks but the board has no camera: its picture input gets a black frame
static bool s_silent;               // the model listens but the board has no microphone: its sound input gets silence
static int s_clip = 16000, s_sr = 16000;

static void emit(const char *tag, const void *src, size_t len, unsigned char *b64, size_t cap)
{
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, cap, &olen, (const unsigned char *)src, len) == 0) {
        fputs(tag, stdout);
        fputc(' ', stdout);
        fwrite(b64, 1, olen, stdout);
        fputc('\n', stdout);
    }
}

static void collect_once(uint32_t seq)
{
    static uint8_t *cur, *prev, *b64;
    static int16_t *pcm;
    static size_t cap;
    const int colors = rt_cam_colors();
    const size_t fbytes = (size_t)RT_CAM * RT_CAM * (colors ? colors : 1);
    if (!b64) {
        cap = 4 * ((s_clip * 2 > (int)fbytes ? s_clip * 2 : fbytes) + 2) / 3 + 8;
        cur = heap_caps_malloc(fbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        prev = heap_caps_malloc(fbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        pcm = heap_caps_malloc(sizeof(int16_t) * s_clip, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cur || !prev || !pcm || !b64) {
            cur = heap_caps_malloc(fbytes, MALLOC_CAP_8BIT);
            prev = heap_caps_malloc(fbytes, MALLOC_CAP_8BIT);
            pcm = heap_caps_malloc(sizeof(int16_t) * s_clip, MALLOC_CAP_8BIT);
            b64 = heap_caps_malloc(cap, MALLOC_CAP_8BIT);
        }
        if (!cur || !prev || !pcm || !b64) {
            ESP_LOGE(TAG, "collect: out of memory");
            return;
        }
    }
    bool have_img = false, have_pcm = false;
    if (s_cam_ok) {
        const uint8_t *c, *p;
        if (rt_cam_acquire(&c, &p, NULL)) {
            memcpy(cur, c, fbytes);
            memcpy(prev, p, fbytes);
            rt_cam_release();
            have_img = true;
        }
    }
    if (s_mic_ok) {
        have_pcm = rt_audio_pcm(pcm, s_clip);
    }
    if (!have_img && !have_pcm) {
        return;
    }
    printf("@S %lu c=%d sr=%d\n", (unsigned long)seq, have_img ? colors : 0, have_pcm ? s_sr : 0);
    if (have_img) {
        emit("@I", cur, fbytes, b64, cap);
        emit("@P", prev, fbytes, b64, cap);
    }
    if (have_pcm) {
        emit("@A", pcm, sizeof(int16_t) * s_clip, b64, cap);
    }
    puts("@E");
    fflush(stdout);
}

static void decide_task(void *arg)
{
    const rt_model_t *m = &g_model;
    float *mel = NULL;
    if (s_model_ok && m->has_audio) {
        // external memory first: this copy is read once for each decision, and the internal memory is short on a
        // board with a microphone
        mel = heap_caps_malloc_prefer(sizeof(float) * m->n_frames * m->n_mels, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
        configASSERT(mel);
    }
    if (mel && s_silent) {                                // what the microphone path makes of no sound at all
        int16_t *win = heap_caps_calloc(m->win, sizeof(int16_t), MALLOC_CAP_8BIT);
        configASSERT(win);
        rt_mel_frame(win, mel);
        free(win);
        for (int f = 1; f < m->n_frames; f++) {
            memcpy(mel + (size_t)f * m->n_mels, mel, sizeof(float) * m->n_mels);
        }
    }
    uint8_t *blank = NULL;
    if (s_blind) {
        blank = heap_caps_calloc_prefer(1, (size_t)RT_CAM * RT_CAM * (m->colors == 3 ? 3 : 1), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
        configASSERT(blank);
    }
    const bool listen = m->has_audio && !s_silent, look = m->has_image && !s_blind;
    static rt_result_t r;
    uint32_t last = 0, runs = 0, cseq = 0;
    int64_t last_print = esp_timer_get_time(), last_collect = 0, last_tick = 0;
    for (;;) {
        if (g_state.collect_ms > 0) {
            const int64_t now = esp_timer_get_time();
            if (now - last_collect >= (int64_t)g_state.collect_ms * 1000) {
                last_collect = now;
                collect_once(cseq++);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!g_state.run || !s_model_ok) {
            // nothing to decide (no model, or the board lacks its sensor): rules on inputs, the radio and other boards
            // still run, five times a second, with every model answer at zero
            const int64_t now = esp_timer_get_time();
            if (g_state.run && now - last_tick >= 200000) {
                last_tick = now;
                memset(&r, 0, sizeof(r));
                rt_actions_update(&r, now);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t seq = 0;
        if (listen) {
            if (rt_audio_mel_seq() == last || !rt_audio_mel(mel, &seq)) {   // wait for the next 20 ms hop
                vTaskDelay(1);
                continue;
            }
        }
        const uint8_t *cur = blank, *prev = blank;
        if (look) {
            uint32_t fseq = 0;
            if (!rt_cam_acquire(&cur, &prev, &fseq)) {
                vTaskDelay(1);
                continue;
            }
            if (!listen) {
                seq = fseq;
                if (seq == last) {
                    rt_cam_release();
                    vTaskDelay(1);
                    continue;
                }
            }
        }
        last = seq;
        rt_decide_inputs(cur, prev, mel);
        if (look) {
            rt_cam_release();
        }
        if (rt_decide_invoke(&r) != ESP_OK) {
            ESP_LOGE(TAG, "inference failed");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        const int64_t now = esp_timer_get_time();
        rt_actions_update(&r, now);
        runs++;
        if (now - last_print >= (int64_t)g_state.print_ms * 1000) {
            rt_decide_print(&r, runs * 1e6f / (float)(now - last_print));
            last_print = now;
            runs = 0;
        }
    }
}

// A restart by software does not reset the camera interface of the chip. Stop it first, so that it does not continue
// to run when the next start does not use the camera (a package for a board without a camera).
void rt_restart(void)
{
    rt_cam_off();
    esp_restart();
}

void app_main(void)
{
    rt_console_start();           // first: everything after it can be read on the console
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    rt_store_init();
    rt_power_early();             // low power: a wake that only arms the motion pin again sleeps here at once
    rt_board_detect();            // which board, camera, microphone: before the model is read
    rt_console_board();
    rt_sensors_start();           // named inputs exist before any rule is read
    rt_eye_inputs();
    rt_dev_load();
    s_model_ok = rt_model_load() == ESP_OK;
    rt_model_t *m = &g_model;
    if (!s_model_ok) {
        m->n_q = m->n_act = 0;    // rules on inputs alone still work
    }
    for (int i = 0; i < m->n_q; i++) {
        m->q[i].na = (m->q[i].mod == 'a' && !g_board.has_mic) || (m->q[i].mod == 'i' && !g_board.has_cam);
    }
    if (s_model_ok && m->has_audio && !g_board.has_mic) {
        if (m->has_image && g_board.has_cam) {
            s_silent = true;      // the camera questions still answer (measured: they do not lean on the sound input)
            printf("{\"note\":\"no microphone: the model's sound input gets silence and its sound questions are off\"}\n");
        } else {
            printf("{\"error\":\"model needs a microphone; board %s has none\"}\n", g_board.name);
            s_model_ok = false;
        }
    }
    if (s_model_ok && m->has_image && !g_board.has_cam) {
        if (m->has_audio && g_board.has_mic) {
            s_blind = true;       // the sound questions still answer: the picture input gets a black frame
            printf("{\"note\":\"no camera: the model's picture input gets a black frame and its picture questions are off\"}\n");
        } else {
            printf("{\"note\":\"no camera and no microphone: the model is off; rules on the radio, inputs and other boards run\"}\n");
            s_model_ok = false;
        }
    }
    if (!s_model_ok) {
        for (int i = 0; i < m->n_q; i++) {
            m->q[i].na = true;    // the page then offers only what this board can do
        }
    }
    // camera: as the model wants it, or RGB for collecting when there is no model (the PC converts to gray if needed)
    const bool want_img = (!s_model_ok || m->has_image) && !s_blind;
    if (want_img) {
        s_cam_ok = g_board.has_cam && rt_cam_start(s_model_ok ? m->colors : 3, s_model_ok && m->motion ? m->lag : 4,
                                                   s_model_ok ? m->frame_ms : 50) == ESP_OK;
        if (!s_cam_ok && s_model_ok && m->has_image) {
            printf("{\"error\":\"camera not found: rules on the radio, inputs and other boards still run\"}\n");
            s_model_ok = false;
        }
    }
    if (g_board.has_mic && (!s_model_ok || m->has_audio)) {
        if (s_model_ok && m->has_audio) {
            s_sr = m->sr;
            s_clip = m->clip;
            s_mic_ok = rt_mel_init(m) == ESP_OK && rt_audio_start(m->sr, m->clip, true) == ESP_OK;
        } else {
            s_mic_ok = rt_audio_start(s_sr, s_clip, false) == ESP_OK;
        }
    } else if (s_silent) {
        s_model_ok = rt_mel_init(m) == ESP_OK;
    }
    if (s_model_ok) {
        s_model_ok = rt_decide_init() == ESP_OK;
    }
    rt_actions_init();
    if (s_cam_ok && !rt_power_on()) {
        rt_eye_start();           // the fly eye watches all the time: not in low power
    }
    rt_print_info();
    rt_console_ready();
    rt_power_start();         // low power: the rules' states from the last wake; decisions wait for the camera's light
    xTaskCreatePinnedToCore(decide_task, "decide", 8192, NULL, 3, NULL, 1);
    if (rt_power_net_now()) {
        // Not before 2.5 s after the start. A board with a camera gets there later than that. A board without one got
        // here in less than 1 s, and then an ESP32-S3-CAM could not log in to a router that it received at -48 dBm
        // (measured on one board; the cause is not known).
        while (esp_timer_get_time() < 2500000) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        rt_wifi_boot();       // after the model has its memory: join the network or open the setup access point
    }
}
