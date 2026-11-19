// Which board this is. One firmware runs on every supported board: at start it looks for a camera on each known pin
// layout, then for a microphone, and keeps what it found. `board` on the console shows it; `board auto` looks again;
// `board <profile>` forces one (for a board whose camera is unplugged). To add a board: a row in PROFILES here, a pin
// map in camera.c (skills/add-board).
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rt.h"

static const char *TAG = "board";

// cam_map is the row in camera.c's PINS with the same order
static const rt_board_t PROFILES[] = {
    // Seeed XIAO ESP32S3 Sense: camera on the ribbon, PDM microphone on 42/41, SD card on SPI (its chip-select is the
    // user LED's pin, 21). Rules may drive the header pins D0-D10 and the LED (lit when low).
    { .name = "xiao_sense", .cam_map = 0, .orient = 3, .mic_clk = 42, .mic_din = 41,
      .card_bus = RT_CARD_SPI, .card_pin = { 21, 7, 8, 9 },
      .pins = 1ULL << 1 | 1ULL << 2 | 1ULL << 3 | 1ULL << 4 | 1ULL << 5 | 1ULL << 6 | 1ULL << 7 | 1ULL << 8 | 1ULL << 9 |
              1ULL << 21 | 1ULL << 43 | 1ULL << 44 },
    // ESP32-S3-CAM boards with the ESP32-S3-EYE pinout (Freenove, FORIOT, ...): no microphone, SD card on SD/MMC
    { .name = "s3cam", .cam_map = 1, .orient = -1, .mic_clk = -1, .mic_din = -1, .uart_console = true,
      .card_bus = RT_CARD_MMC, .card_pin = { 39, 38, 40, -1 } },
    // ESP32-S3-CAM boards with a display connector (camera on other pins): no microphone, no card
    { .name = "s3cam-lcd", .cam_map = 2, .orient = -1, .mic_clk = -1, .mic_din = -1, .uart_console = true },
    // Any other ESP32-S3 board: no camera, no microphone. Rules use the radio, the pins, add-on sensors and the
    // other boards. Never found by the search: the model package names it (rt_model_hw), or `board plain`.
    { .name = "plain", .cam_map = -1, .orient = -1, .mic_clk = -1, .mic_din = -1 },
};
#define N_PROFILES ((int)(sizeof(PROFILES) / sizeof(PROFILES[0])))

rt_board_t g_board = { .name = "unknown", .cam_map = -1, .orient = -1, .mic_clk = -1, .mic_din = -1 };
static int s_mic_spread = -1;       // what the microphone check measured, -1 = not run

static int by_name(const char *name)
{
    for (int i = 0; i < N_PROFILES; i++) {
        if (!strcmp(PROFILES[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static uint32_t flash_mb(void)
{
    uint32_t size = 0;
    esp_flash_get_physical_size(NULL, &size);             // the chip's, not the 8 MB the image was built for
    return size >> 20;
}

void rt_board_detect(void)
{
    char saved[16] = "";
    rt_store_get("board", saved, sizeof(saved));
    int order[N_PROFILES + 1], n = 0;
    const int keep = by_name(saved);
    // The two families differ in one thing that can be read without driving a pin: the S3-CAM boards have a
    // USB-serial chip on UART0, the XIAO has none (rt_console_has_uart). Only the camera layouts of that family are
    // tried, because trying a layout drives its clock pin, and on the other family that pin belongs to something else.
    const bool uart = rt_console_has_uart();
    if (keep >= 0) {
        order[n++] = keep;
    } else {
        for (int i = 0; i < N_PROFILES; i++) {
            if (PROFILES[i].uart_console == uart && PROFILES[i].cam_map >= 0) {
                order[n++] = i;
            }
        }
    }
    // the model package says that this board has no camera: do not look for one (that drives pins)
    bool no_cam = false;
    char want[16] = "";
    rt_model_hw(&no_cam, want, sizeof(want));
    if (no_cam) {
        n = 0;
        rt_cam_quiet();
    }
    int found = -1;
    for (int k = 0; k < n && found < 0; k++) {
        if (rt_cam_probe(PROFILES[order[k]].cam_map) == ESP_OK) {
            found = order[k];
        }
    }
    int use = found >= 0 ? found : no_cam && by_name(want) >= 0 ? by_name(want) : keep;
    if (use < 0) {
        use = by_name(uart ? "s3cam" : "xiao_sense");     // no camera answered: the family's first profile, without one
    }
    g_board = PROFILES[use];
    g_board.has_cam = found >= 0;
    g_board.saved = keep >= 0;
    if (g_board.cam_map < 0) {
        g_board.uart_console = uart;                     // an unknown board: keep the pins of its USB-serial chip free
    }
    if (g_board.mic_clk >= 0) {
        s_mic_spread = rt_audio_probe(g_board.mic_clk, g_board.mic_din);
        g_board.has_mic = s_mic_spread >= 4;
    }
    if (found >= 0 && keep < 0) {
        rt_store_set("board", g_board.name);            // next start goes straight to this layout
    }
    ESP_LOGI(TAG, "%s: camera %s, microphone %s (spread %d)", g_board.name, g_board.has_cam ? "yes" : "no",
             g_board.has_mic ? "yes" : "no", s_mic_spread);
}

// May a rule, an input pin or an add-on sensor use this pin? Not when the board itself needs it: the camera, the
// microphone, the console's UART, USB (19, 20), the flash and PSRAM (26-37), or the pins the chip reads at reset
// (0, 45, 46). A profile with a pin list (a board with a labelled header) allows only those.
bool rt_board_pin_free(int gpio)
{
    if (gpio < 1 || gpio > 48 || gpio == 19 || gpio == 20 || (gpio >= 22 && gpio <= 37) || gpio == 45 || gpio == 46) {
        return false;
    }
    if (rt_cam_uses_pin(g_board.cam_map, gpio)) {             // also when the camera is unplugged now
        return false;
    }
    if (g_board.mic_clk >= 0 && (gpio == g_board.mic_clk || gpio == g_board.mic_din)) {
        return false;
    }
    if (g_board.uart_console && (gpio == 43 || gpio == 44)) {
        return false;
    }
    return !g_board.pins || (g_board.pins >> gpio & 1);
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

int rt_board_sensors_json(char *dst, size_t cap)
{
    int n = 0;
    ADD("{\"profile\":\"%s\",\"saved\":%s,\"camera\":", g_board.name, g_board.saved ? "true" : "false");
    if (g_board.has_cam) {
        ADD("\"%s\"", rt_cam_sensor());
    } else {
        ADD("null");
    }
    ADD(",\"mic\":%s,\"mic_spread\":%d,\"card\":%s,\"radio\":true,\"psram_mb\":%u,\"flash_mb\":%u}",
        g_board.has_mic ? "true" : "false", s_mic_spread, g_board.has_card ? "true" : "false",
        (unsigned)((heap_caps_get_total_size(MALLOC_CAP_SPIRAM) + (1 << 19)) >> 20), (unsigned)flash_mb());
    return n < (int)cap ? n : (int)cap - 1;
}

// board | board auto | board <profile>: false = no such profile. A change takes effect at the next start.
bool rt_board_command(const char *arg)
{
    if (arg && !strcmp(arg, "auto")) {
        rt_store_set("board", NULL);
    } else if (arg) {
        if (by_name(arg) < 0) {
            return false;
        }
        rt_store_set("board", arg);
    }
    static char b[256];
    rt_board_sensors_json(b, sizeof(b));
    printf("{\"sensors\":%s}\n", b);
    return true;
}
