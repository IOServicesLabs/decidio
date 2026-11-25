// Microphone: PDM at the model's rate. One log-mel frame per hop (streamed, so a full window of features is always
// ready), plus a raw PCM history for collect mode.
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rt.h"

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx;
static SemaphoreHandle_t s_lock;
static bool s_do_mel;
static int s_hop, s_win, s_frames, s_mels, s_hist, s_sr, s_clip;
static int16_t *s_pcm;            // history ring, s_hist samples
static uint32_t s_pcm_head, s_pcm_total;
static float *s_mel;              // ring s_frames x s_mels
static uint32_t s_mel_head, s_mel_seq;

static void *alloc(size_t n, bool fast)
{
    void *p = heap_caps_malloc(n, fast ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static void read_exact(int16_t *dst, size_t n)
{
    size_t got = 0;
    while (got < n * sizeof(int16_t)) {
        size_t r = 0;
        if (i2s_channel_read(s_rx, (uint8_t *)dst + got, n * sizeof(int16_t) - got, &r, portMAX_DELAY) == ESP_OK) {
            got += r;
        }
    }
}

// PDM microphones sit on a constant offset (about 1350 counts on the XIAO Sense, far above room noise). Training sound
// has none, and the model hears the offset as a low thud. A one-pole high-pass (corner ~13 Hz at 16 kHz) takes it out
// of everything downstream: the mel frames and the PCM that collect mode sends.
static void dc_block(int16_t *x, int n)
{
    static float prev_in, prev_out;
    static bool primed;
    if (!primed) {
        prev_in = x[0];
        primed = true;
    }
    for (int i = 0; i < n; i++) {
        const float in = x[i];
        prev_out = 0.995f * (prev_out + in - prev_in);
        prev_in = in;
        const float v = prev_out < -32768.0f ? -32768.0f : (prev_out > 32767.0f ? 32767.0f : prev_out);
        x[i] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
    }
}

static void mic_task(void *arg)
{
    int16_t *win = alloc(sizeof(int16_t) * s_win, true);
    int16_t *hop = alloc(sizeof(int16_t) * s_hop, true);
    float *frame = alloc(sizeof(float) * (s_mels > 0 ? s_mels : 1), true);
    configASSERT(win && hop && frame);
    memset(win, 0, sizeof(int16_t) * s_win);
    int have = 0;
    for (;;) {
        read_exact(hop, s_hop);
        dc_block(hop, s_hop);
        memmove(win, win + s_hop, sizeof(int16_t) * (s_win - s_hop));
        memcpy(win + s_win - s_hop, hop, sizeof(int16_t) * s_hop);
        if (have < s_win) {
            have += s_hop;
        }
        const bool ok = s_do_mel && have >= s_win;
        if (ok) {
            rt_mel_frame(win, frame);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_hop; i++) {
            s_pcm[s_pcm_head] = hop[i];
            s_pcm_head = (s_pcm_head + 1) % s_hist;
        }
        s_pcm_total += s_hop;
        if (ok) {
            memcpy(s_mel + (size_t)s_mel_head * s_mels, frame, sizeof(float) * s_mels);
            s_mel_head = (s_mel_head + 1) % s_frames;
            s_mel_seq++;
        }
        xSemaphoreGive(s_lock);
    }
}

// Is a PDM microphone on these pins? Listen for 0.2 s with the data pin pulled low: a microphone's samples wander by
// tens of counts even in a quiet room, an empty pin gives one value. Returns the spread (largest - smallest sample),
// -1 if the input could not be started.
int rt_audio_probe(int clk, int din)
{
    enum { N = 1600 };                                    // 0.1 s
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) {
        return -1;
    }
    i2s_pdm_rx_config_t pc = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .clk = clk, .din = din, .invert_flags = { .clk_inv = false } },
    };
    int spread = -1;
    int16_t *buf = heap_caps_malloc(sizeof(int16_t) * N, MALLOC_CAP_8BIT);
    if (buf && i2s_channel_init_pdm_rx_mode(rx, &pc) == ESP_OK && i2s_channel_enable(rx) == ESP_OK) {
        gpio_set_pull_mode(din, GPIO_PULLDOWN_ONLY);
        int lo = 32767, hi = -32768;
        for (int k = 0; k < 5; k++) {                     // the first 0.3 s is the input settling
            size_t got = 0;
            if (i2s_channel_read(rx, buf, sizeof(int16_t) * N, &got, pdMS_TO_TICKS(400)) != ESP_OK || k < 3) {
                continue;
            }
            for (size_t i = 0; i < got / sizeof(int16_t); i++) {
                lo = buf[i] < lo ? buf[i] : lo;
                hi = buf[i] > hi ? buf[i] : hi;
            }
        }
        spread = hi >= lo ? hi - lo : -1;
        i2s_channel_disable(rx);
        gpio_set_pull_mode(din, GPIO_FLOATING);
    }
    free(buf);
    i2s_del_channel(rx);
    return spread;
}

esp_err_t rt_audio_start(int sr, int clip, bool mel)
{
    ESP_RETURN_ON_FALSE(g_board.has_mic, ESP_ERR_NOT_SUPPORTED, TAG, "this board has no microphone");
    s_do_mel = mel;
    s_hop = mel ? g_model.hop : 320;
    s_win = mel ? g_model.win : 480;
    s_frames = mel ? g_model.n_frames : 1;
    s_mels = mel ? g_model.n_mels : 0;
    s_hist = clip + s_hop;
    s_pcm = alloc(sizeof(int16_t) * s_hist, false);
    s_mel = alloc(sizeof(float) * s_frames * (s_mels > 0 ? s_mels : 1), false);   // one row is written for each hop: external memory is fast enough
    ESP_RETURN_ON_FALSE(s_pcm && s_mel, ESP_ERR_NO_MEM, TAG, "buffers");
    memset(s_pcm, 0, sizeof(int16_t) * s_hist);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = s_hop;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, NULL, &s_rx), TAG, "i2s channel");
    i2s_pdm_rx_config_t pc = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sr),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .clk = g_board.mic_clk, .din = g_board.mic_din, .invert_flags = { .clk_inv = false } },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_rx, &pc), TAG, "pdm init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s enable");
    s_sr = sr;
    s_clip = clip;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    return xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 7, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool rt_audio_mel(float *dst, uint32_t *seq)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_do_mel && s_mel_seq >= (uint32_t)s_frames;
    if (ok) {
        const size_t first = s_frames - s_mel_head;                 // oldest frame sits at s_mel_head
        memcpy(dst, s_mel + (size_t)s_mel_head * s_mels, sizeof(float) * first * s_mels);
        memcpy(dst + first * s_mels, s_mel, sizeof(float) * s_mel_head * s_mels);
    }
    if (seq) {
        *seq = s_mel_seq;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool rt_audio_pcm(int16_t *dst, int n)
{
    if (!s_lock || n > s_hist) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_pcm_total >= (uint32_t)n;
    if (ok) {
        const uint32_t start = (s_pcm_head + s_hist - n) % s_hist;
        const int first = (s_hist - start) < (uint32_t)n ? (int)(s_hist - start) : n;
        memcpy(dst, s_pcm + start, sizeof(int16_t) * first);
        memcpy(dst + first, s_pcm, sizeof(int16_t) * (n - first));
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int rt_audio_rate(void)
{
    return s_lock ? s_sr : 0;
}

int rt_audio_clip(void)
{
    return s_lock ? s_clip : 0;
}

uint32_t rt_audio_mel_seq(void)
{
    return s_mel_seq;
}

uint32_t rt_audio_seq(void)
{
    return s_pcm_total / (s_hop ? s_hop : 1);
}
