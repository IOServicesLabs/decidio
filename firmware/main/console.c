// Line commands on the console. One firmware serves two kinds of board, so the console listens on both ports: the
// chip's own USB port (the XIAO's only socket) and UART0 at 921600 baud (the S3-CAM's USB-serial socket). Output goes to
// the port a command last came from; until then to USB if a computer is on it, else to UART0 if a USB-serial chip is
// on it. The console starts before the board is known, so UART0 is opened when its receive pin (GPIO44) is held high,
// as a USB-serial chip's idle line is; once the board turns out to be a XIAO (where GPIO43/44 are header pins D6/D7)
// UART0 is closed again (rt_console_board).
//   board | board auto | board <profile>      what was found at start; look again at the next start; force a profile
//   inputs                                   named inputs and their values (inputs.c)
//   eye | eye on | eye off                   the fly eye: speed and latest events; switch it (eye.cc)
//   eye zoom <x> <y> <w>                     a sharp close-up of that part of the 160x120 frame (OV3660)
//   pin in <gpio> [up|down|none] | pin in off <gpio> | pin in      header pins as inputs (sensors/pin_in.c)
//   i2c scan <sda> <scl>                     which I2C addresses answer on two pins (sensors/i2c_scan.c)
//   mic probe <clk> <din>                    is a PDM microphone on these pins? (not while the microphone runs)
//   info | run | stop | print <ms> | collect <ms> | collect off | orient <0-3|auto> | profile on|off | reboot
//   radio scan | radio <channel> [sender] | radio off      (radio.c)
//   wifi | wifi "<network name>" "<password>" | wifi forget | wifi off      (wifi.c)
//   alert test telegram|webhook|mqtt|probe | alert set <key> <value>        (alert.c; keys as in web.c)
//   power | power on|off | power stay|go | power wakes <n>          low-power mode (power.c)
//   mem
// An argument with spaces goes in double quotes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rt.h"
#include "sdkconfig.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_vfs.h"

#define FW_VERSION "decidio-rt " RT_VERSION

rt_state_t g_state = { .run = true, .print_ms = 250, .collect_ms = 0, .profile = false };

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

// {"info":{...}} into dst; returns its length
int rt_info_json(char *dst, size_t cap)
{
    const rt_model_t *m = &g_model;
    int n = 0;
    ADD("{\"info\":{\"fw\":\"%s\",\"board\":\"%s\",\"camera\":\"%s\",\"mic\":%s,\"sensors\":", FW_VERSION, g_board.name,
        rt_cam_sensor(), g_board.has_mic ? "true" : "false");
    n += rt_board_sensors_json(dst + n, n < (int)cap ? cap - n : 0);
    ADD(",\"model\":");
    if (!m->loaded) {
        ADD("null");
    } else {
        ADD("{\"name\":\"%s\",\"image\":", m->name);
        if (m->has_image) {
            ADD("{\"size\":%d,\"colors\":%d,\"motion\":%d}", m->img_size, m->colors, m->motion);
        } else {
            ADD("null");
        }
        ADD(",\"audio\":");
        if (m->has_audio) {
            ADD("{\"sr\":%d,\"clip\":%d}", m->sr, m->clip);
        } else {
            ADD("null");
        }
        ADD(",\"questions\":[");
        for (int i = 0; i < m->n_q; i++) {
            ADD("%s\"%s\"", i ? "," : "", m->q[i].id);
        }
        ADD("],\"options\":{");                           // every question's answers, for writing rules
        for (int i = 0; i < m->n_q; i++) {
            ADD("%s\"%s\":[", i ? "," : "", m->q[i].id);
            for (int j = 0; j < m->q[i].count; j++) {
                ADD("%s\"%s\"", j ? "," : "", m->q[i].opt[j]);
            }
            ADD("]");
        }
        ADD("},\"na\":[");                               // questions this board cannot answer (no such sensor)
        for (int i = 0, k = 0; i < m->n_q; i++) {
            if (m->q[i].na) {
                ADD("%s\"%s\"", k++ ? "," : "", m->q[i].id);
            }
        }
        ADD("],\"arena_used\":%u}", (unsigned)rt_decide_arena_used());
    }
    ADD(",\"pins\":[");                                   // pins a rule, an input or a sensor may use (board.c)
    for (int i = 0, k = 0; i < 64; i++) {
        if (rt_board_pin_free(i)) {
            ADD("%s%d", k++ ? "," : "", i);
        }
    }
    ADD("],\"collect\":{\"colors\":%d,\"audio\":%s},\"heap_internal\":%u,\"psram\":%u}}", rt_cam_colors(),
        (g_board.has_mic && rt_audio_seq() > 0) ? "true" : "false",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return n < (int)cap ? n : (int)cap - 1;
}

// one long-lived text buffer per caller, in PSRAM when there is some: internal RAM is the scarce kind
static char *big(char **slot, size_t n)
{
    if (!*slot) {
        *slot = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!*slot) {
            *slot = malloc(n);
        }
        configASSERT(*slot);
    }
    return *slot;
}

void rt_print_info(void)
{
    static char *slot;
    char *b = big(&slot, 3500);
    rt_info_json(b, 3500);
    puts(b);
    fflush(stdout);
}

// the next argument of a command line: words, or "a few words" in double quotes; NULL at the end
static char *next_arg(char **p)
{
    char *c = *p;
    while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') {
        c++;
    }
    if (!*c) {
        *p = c;
        return NULL;
    }
    char *start;
    if (*c == '"') {
        start = ++c;
        while (*c && *c != '"') {
            c++;
        }
    } else {
        start = c;
        while (*c && *c != ' ' && *c != '\t' && *c != '\r' && *c != '\n') {
            c++;
        }
    }
    if (*c) {
        *c++ = 0;
    }
    *p = c;
    return start;
}

static void wifi_command(char *arg, char *arg2)
{
    if (!arg) {
        rt_wifi_status_t w;
        rt_wifi_status(&w);
        printf("{\"wifi\":{\"state\":\"%s\",\"setup_open\":%s,\"ip\":\"%s\",\"host\":\"%s\",\"rssi\":%d,\"channel\":%d,"
               "\"reason\":%d}}\n", w.state, w.setup_open ? "true" : "false", w.ip, w.host, w.rssi, w.channel, w.reason);
    } else if (!strcmp(arg, "forget") && !arg2) {
        rt_wifi_forget(false);
    } else if (!strcmp(arg, "off") && !arg2) {
        rt_wifi_forget(true);
    } else {
        const esp_err_t err = rt_wifi_set(arg, arg2);
        if (err != ESP_OK) {
            printf("{\"error\":\"wifi: %s\"}\n", esp_err_to_name(err));
        }
    }
}

static void handle(char *line)
{
    char *p = line;
    char *cmd = next_arg(&p);
    char *arg = next_arg(&p);
    if (!cmd) {
        return;
    }
    if (!strcmp(cmd, "info")) {
        rt_print_info();
    } else if (!strcmp(cmd, "run")) {
        g_state.collect_ms = 0;
        g_state.run = true;
    } else if (!strcmp(cmd, "stop")) {
        g_state.run = false;
    } else if (!strcmp(cmd, "print") && arg) {
        g_state.print_ms = atoi(arg);
    } else if (!strcmp(cmd, "collect")) {
        const int ms = (arg && strcmp(arg, "off")) ? atoi(arg) : 0;
        g_state.collect_ms = ms > 0 ? (ms < 200 ? 200 : ms) : 0;
        g_state.run = g_state.collect_ms == 0;
    } else if (!strcmp(cmd, "orient") && arg) {
        const int o = strcmp(arg, "auto") ? atoi(arg) : -1;
        rt_cam_set_orient(o);
        nvs_handle_t nv;
        if (nvs_open("decidio", NVS_READWRITE, &nv) == ESP_OK) {
            if (o < 0) {
                nvs_erase_key(nv, "orient");
            } else {
                nvs_set_i8(nv, "orient", (int8_t)o);
            }
            nvs_commit(nv);
            nvs_close(nv);
        }
    } else if (!strcmp(cmd, "profile") && arg) {
        g_state.profile = !strcmp(arg, "on");
        rt_decide_profile(g_state.profile);
    } else if (!strcmp(cmd, "radio")) {
        if (!rt_radio_command(arg, next_arg(&p))) {
            printf("{\"error\":\"radio scan | radio <channel 1-13> [sender] | radio off\"}\n");
            return;
        }
    } else if (!strcmp(cmd, "wifi")) {
        wifi_command(arg, next_arg(&p));
    } else if (!strcmp(cmd, "alert") && arg && !strcmp(arg, "test")) {
        const char *dest = next_arg(&p);
        rt_alert_result_t r;
        rt_alert_test(dest ? dest : "probe", &r);
        printf("{\"alert_test\":{\"ok\":%s,\"status\":%d,\"ms\":%d,\"detail\":\"%s\"},\"heap_internal\":%u,\"heap_min\":%u}\n",
               r.ok ? "true" : "false", r.status, r.ms, r.detail, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    } else if (!strcmp(cmd, "alert") && arg && !strcmp(arg, "set")) {
        const char *key = next_arg(&p);
        const char *val = next_arg(&p);
        if (!key || strlen(key) > 15 || (val && strpbrk(val, "\"\\"))) {
            printf("{\"error\":\"alert set <key> <value>\"}\n");
            return;
        }
        rt_store_set(key, val);
        rt_alert_reload();
    } else if (!strcmp(cmd, "board")) {
        if (!rt_board_command(arg)) {
            printf("{\"error\":\"board | board auto | board xiao_sense|s3cam|s3cam-lcd\"}\n");
            return;
        }
    } else if (!strcmp(cmd, "inputs")) {
        static char *slot;
        char *b = big(&slot, 2400);
        rt_inputs_json(b, 2400);
        printf("{\"inputs\":%s}\n", b);
    } else if (!strcmp(cmd, "eye")) {
        static char *slot;
        char *b = big(&slot, 2400);
        if (arg && !strcmp(arg, "zoom")) {                // eye zoom <x> <y> <w>: a sharp close-up of that part of the frame
            const char *x = next_arg(&p), *y = next_arg(&p), *w = next_arg(&p);
            printf("{\"zoom\":%s}\n", x && y && w && rt_eye_zoom_test(atoi(x), atoi(y), atoi(w)) ? "true" : "false");
        } else if (arg) {
            rt_eye_want(!strcmp(arg, "on"));
        }
        rt_eye_json(b, 2400);
        printf("{\"eye_state\":%s}\n", b);
    } else if (!strcmp(cmd, "i2c") && arg && !strcmp(arg, "scan")) {
        const char *a = next_arg(&p), *b = next_arg(&p);
        static char *slot;
        char *out = big(&slot, 700);
        if (!a || !b) {
            printf("{\"error\":\"i2c scan <sda pin> <scl pin>\"}\n");
            return;
        }
        rt_i2c_scan_json(atoi(a), atoi(b), out, 700);
        printf("{\"i2c\":%s}\n", out);
    } else if (!strcmp(cmd, "pin") && arg && !strcmp(arg, "in")) {
        const char *a = next_arg(&p), *b = next_arg(&p);
        char why[80] = "";
        const bool off = a && !strcmp(a, "off");
        if (a && !rt_pin_in_set(atoi(off ? (b ? b : "-1") : a), off ? NULL : b, off, why, sizeof(why))) {
            printf("{\"error\":\"%s\"}\n", why);
            return;
        }
        if (!a) {
            rt_store_get("pins_in", why, sizeof(why));
        }
        printf("{\"pins_in\":\"%s\",\"note\":\"takes effect at the next start (reboot)\"}\n", why);
    } else if (!strcmp(cmd, "mic") && arg && !strcmp(arg, "probe")) {
        const char *clk = next_arg(&p), *din = next_arg(&p);
        if (!clk || !din || rt_audio_seq() > 0) {
            printf("{\"error\":\"mic probe <clk pin> <data pin> (only while the microphone is not running)\"}\n");
            return;
        }
        printf("{\"mic_probe\":{\"clk\":%d,\"din\":%d,\"spread\":%d}}\n", atoi(clk), atoi(din), rt_audio_probe(atoi(clk), atoi(din)));
    } else if (!strcmp(cmd, "power")) {
        if (!rt_power_command(arg, next_arg(&p))) {
            return;
        }
    } else if (!strcmp(cmd, "mem")) {
        printf("{\"mem\":{\"heap_internal\":%u,\"heap_min\":%u,\"largest\":%u,\"psram\":%u}}\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (!strcmp(cmd, "reboot")) {
        esp_restart();
    } else {
        printf("{\"error\":\"unknown command %s\"}\n", cmd);
        return;
    }
    printf("{\"ok\":\"%s\"}\n", cmd);
    fflush(stdout);
}

// ---- the two ports -----------------------------------------------------------------------------------------------
enum { OUT_NONE = 0, OUT_USB = 1, OUT_UART = 2 };
static volatile int s_out = OUT_NONE;
static bool s_usb_ok, s_uart_ok, s_usb_stalled;
static volatile bool s_ready;                 // start-up is over: commands are answered from here on

static void usb_write(const char *b, size_t n)
{
    // nobody reading (the port is not open on the computer): give up quickly, and stop waiting until it drains again
    while (n) {
        const int w = usb_serial_jtag_write_bytes(b, n, s_usb_stalled ? 0 : pdMS_TO_TICKS(60));
        if (w <= 0) {
            s_usb_stalled = true;
            return;
        }
        s_usb_stalled = false;
        b += w;
        n -= w;
    }
}

static ssize_t con_write(int fd, const void *data, size_t size)
{
    const char *b = data;
    const int out = s_out;
    if (out == OUT_NONE) {
        return size;
    }
    size_t from = 0;
    for (size_t i = 0; i <= size; i++) {                  // "\n" goes out as "\r\n", as the stock console does
        if (i == size || b[i] == '\n') {
            if (out == OUT_USB) {
                usb_write(b + from, i - from);
                if (i < size) {
                    usb_write("\r\n", 2);
                }
            } else {
                uart_write_bytes(UART_NUM_0, b + from, i - from);
                if (i < size) {
                    uart_write_bytes(UART_NUM_0, "\r\n", 2);
                }
            }
            from = i + 1;
        }
    }
    return size;
}

static int con_open(const char *path, int flags, int mode)
{
    return 0;
}

static int con_close(int fd)
{
    return 0;
}

static int con_fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static ssize_t con_read(int fd, void *dst, size_t size)
{
    return 0;                                             // input is read from the two drivers directly (console_task)
}

static const esp_vfs_t CON_VFS = { .flags = ESP_VFS_FLAG_DEFAULT, .write = con_write, .open = con_open, .close = con_close,
                                   .fstat = con_fstat, .read = con_read };

typedef struct {
    char line[256];
    int n;
} line_t;

// bytes from one port into its line; a finished line is run and answers go back out of that port
static void feed(line_t *l, const uint8_t *b, int n, int port)
{
    for (int i = 0; i < n; i++) {
        if (b[i] == '\n' || b[i] == '\r') {
            if (l->n) {
                l->line[l->n] = 0;
                l->n = 0;
                s_out = port;
                handle(l->line);
            }
        } else if (l->n < (int)sizeof(l->line) - 1) {
            l->line[l->n++] = (char)b[i];
        }
    }
}

static void console_task(void *arg)
{
    static line_t usb, uart;
    uint8_t b[64];
    for (;;) {
        int got = 0;
        if (!s_ready) {                                   // what was typed waits in the drivers' buffers
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_usb_ok) {
            const int n = usb_serial_jtag_read_bytes(b, sizeof(b), 0);
            if (n > 0) {
                feed(&usb, b, n, OUT_USB);
                got += n;
            }
        }
        if (s_uart_ok) {
            const int n = uart_read_bytes(UART_NUM_0, b, sizeof(b), 0);
            if (n > 0) {
                feed(&uart, b, n, OUT_UART);
                got += n;
            }
        }
        if (s_out == OUT_NONE && s_usb_ok && usb_serial_jtag_is_connected()) {
            s_out = OUT_USB;                              // a computer was plugged in later
        }
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

// start-up is over: the board, the model and the rules are known, so a command gets a true answer
void rt_console_ready(void)
{
    s_ready = true;
}

bool rt_console_has_uart(void)
{
    return s_uart_ok;
}

// after rt_board_detect(): a board without a USB-serial socket gets its two pins back
void rt_console_board(void)
{
    if (s_uart_ok && !g_board.uart_console) {
        s_uart_ok = false;
        if (s_out == OUT_UART) {
            s_out = OUT_NONE;
        }
        vTaskDelay(pdMS_TO_TICKS(40));                    // let the reader task finish its pass
        uart_driver_delete(UART_NUM_0);
        gpio_reset_pin(43);
        gpio_reset_pin(44);
    }
}

// first thing at start, so that everything after it can be read
void rt_console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    cfg.rx_buffer_size = 512;
    s_usb_ok = usb_serial_jtag_driver_install(&cfg) == ESP_OK;
    gpio_set_direction(44, GPIO_MODE_INPUT);
    gpio_set_pull_mode(44, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(3));
    const bool rx_held_high = gpio_get_level(44) == 1;
    gpio_set_pull_mode(44, GPIO_FLOATING);
    if (rx_held_high) {
        const uart_config_t uc = { .baud_rate = 921600, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                                   .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                   .source_clk = UART_SCLK_DEFAULT };
        s_uart_ok = uart_driver_install(UART_NUM_0, 1024, 8192, 0, NULL, 0) == ESP_OK &&
                    uart_param_config(UART_NUM_0, &uc) == ESP_OK &&
                    uart_set_pin(UART_NUM_0, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(30));                        // a connected computer shows within a few USB frames
    s_out = (s_usb_ok && usb_serial_jtag_is_connected()) ? OUT_USB : (s_uart_ok ? OUT_UART : OUT_NONE);
    if (esp_vfs_register("/dev/con", &CON_VFS, NULL) == ESP_OK) {
        fflush(stdout);
        if (freopen("/dev/con", "w", stdout)) {
            setvbuf(stdout, NULL, _IOLBF, 0);
        }
        if (freopen("/dev/con", "w", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 4, NULL, 0);
}
