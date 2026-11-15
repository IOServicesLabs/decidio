// i2c: which addresses answer on a pair of pins, and plain read / write for sensor files. The bus is driven pin by
// pin (about 50 kHz), not by the chip's I2C controller, so it works on any two free pins, takes no controller away
// from the camera, and needs no set-up. It is slow (a 6-byte read is about 2 ms): fine for a sensor read once a second.
//
//   i2c scan <sda> <scl>     on the console; the page's System tab has the same as a button
// Nothing here runs at start. A scan drives the two pins, so only pins the board offers to rules can be used.
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "rt.h"

static int s_sda = -1, s_scl = -1;

static inline void wait(void)
{
    esp_rom_delay_us(5);
}

static inline void sda(int v)
{
    gpio_set_level(s_sda, v);                               // open drain: 1 lets the pull-up raise the line
}

static inline void scl(int v)
{
    gpio_set_level(s_scl, v);
    for (int i = 0; v && i < 200 && !gpio_get_level(s_scl); i++) {
        esp_rom_delay_us(5);                                // a slow part may hold the clock low
    }
}

static void start(void)
{
    sda(1); scl(1); wait();
    sda(0); wait();
    scl(0); wait();
}

static void stop(void)
{
    sda(0); wait();
    scl(1); wait();
    sda(1); wait();
}

static bool put(uint8_t b)                                  // true = the part acknowledged
{
    for (int i = 7; i >= 0; i--) {
        sda(b >> i & 1); wait();
        scl(1); wait();
        scl(0);
    }
    sda(1); wait();
    scl(1); wait();
    const bool ack = !gpio_get_level(s_sda);
    scl(0); wait();
    return ack;
}

static uint8_t get(bool more)
{
    uint8_t b = 0;
    sda(1);
    for (int i = 0; i < 8; i++) {
        wait();
        scl(1); wait();
        b = (uint8_t)(b << 1 | gpio_get_level(s_sda));
        scl(0);
    }
    sda(more ? 0 : 1); wait();
    scl(1); wait();
    scl(0); wait();
    sda(1);
    return b;
}

static bool usable(int pin)
{
    return rt_board_pin_free(pin);
}

// choose the two pins (with the chip's weak pull-ups; a real bus has its own, stronger ones on the sensor board)
bool rt_i2c_begin(int sda_pin, int scl_pin)
{
    if (!usable(sda_pin) || !usable(scl_pin) || sda_pin == scl_pin) {
        return false;
    }
    s_sda = sda_pin;
    s_scl = scl_pin;
    const int pins[2] = { sda_pin, scl_pin };
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_level(pins[i], 1);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }
    esp_rom_delay_us(200);
    return gpio_get_level(sda_pin) && gpio_get_level(scl_pin);     // both lines must rest high
}

// write `n` bytes to the part at `addr` (7-bit); false = no answer
bool rt_i2c_write(uint8_t addr, const uint8_t *b, int n)
{
    start();
    bool ok = put((uint8_t)(addr << 1));
    for (int i = 0; ok && i < n; i++) {
        ok = put(b[i]);
    }
    stop();
    return ok;
}

// read `n` bytes from the part at `addr`; false = no answer
bool rt_i2c_read(uint8_t addr, uint8_t *b, int n)
{
    start();
    const bool ok = put((uint8_t)(addr << 1 | 1));
    for (int i = 0; ok && i < n; i++) {
        b[i] = get(i < n - 1);
    }
    stop();
    return ok;
}

// set a register number, then read from it: what most sensors want
bool rt_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *b, int n)
{
    return rt_i2c_write(addr, &reg, 1) && rt_i2c_read(addr, b, n);
}

// {"sda":5,"scl":6,"found":["0x44","0x76"]} or {"error":"..."}
int rt_i2c_scan_json(int sda_pin, int scl_pin, char *dst, size_t cap)
{
    if (!rt_i2c_begin(sda_pin, scl_pin)) {
        return snprintf(dst, cap, "{\"error\":\"%s\"}", usable(sda_pin) && usable(scl_pin) && sda_pin != scl_pin
                        ? "a line is held low: check the wiring and that the sensor has power"
                        : "use two different pins from the board's free pins");
    }
    int n = snprintf(dst, cap, "{\"sda\":%d,\"scl\":%d,\"found\":[", sda_pin, scl_pin);
    for (int a = 0x08, k = 0; a < 0x78 && n < (int)cap - 12; a++) {
        if (rt_i2c_write((uint8_t)a, NULL, 0)) {
            n += snprintf(dst + n, cap - n, "%s\"0x%02x\"", k++ ? "," : "", a);
        }
    }
    n += snprintf(dst + n, n < (int)cap ? cap - n : 0, "]}");
    return n;
}
