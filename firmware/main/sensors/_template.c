// _template.c: copy this file to add a sensor. A file whose name starts with "_" is not built, so give the copy a
// name without it (for example sht31.c). No other file needs a change: every .c file in this folder is built in.
//
// This example reads an SHT30 / SHT31 temperature and humidity part on I2C. It was compiled, but it was not run with
// the part connected: check the numbers against the datasheet of your part before you trust them.
//
// What a sensor file does:
//   1. A start function. It sets the hardware up. If the part answers, it registers one input for each number
//      (rt_input_register). If the part does not answer, it registers nothing: rules that name the input are then
//      refused, and the page does not show it.
//   2. One getter for each input. A getter gives the value now, or NAN when there is no value. The decision task and
//      the web server call the getters many times a second, so a getter must return immediately. Read slow hardware
//      in a task of your own and keep the last value, as this file does.
//   3. RT_SENSOR(start function) on the last line. The firmware then calls the start function one time at start.
//
// The inputs show on the page (Live tab, "Other inputs") and on the console (`inputs`). A rule tests an input with
// {"q":"in:<name>","op":">=" or "<","p":<number>}; in a recipe file that is `in:<name> > 25`.
//
// Pins: use only pins for which rt_board_pin_free() is true. The camera, the microphone, the console, USB and the
// flash memory have pins that you must not use. The pins carry 3.3 V: a part with 5 V signals damages the board.
// The I2C lines need a pull-up resistor to 3.3 V on each line (most breakout boards have them).
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rt.h"

#define SDA_PIN 5                 // XIAO: D4 is GPIO5, D5 is GPIO6. Change these for your board and wiring.
#define SCL_PIN 6
#define ADDR 0x44                 // SHT30 / SHT31 (0x45 when its ADDR pin is high). `i2c scan 5 6` shows what answers.

static volatile float s_temp = NAN, s_hum = NAN;

static float get_temp(void)
{
    return s_temp;
}

static float get_hum(void)
{
    return s_hum;
}

// one measurement: false = the part did not answer
static bool measure(void)
{
    const uint8_t cmd[2] = { 0x24, 0x00 };                  // single shot, high repeatability, no clock stretching
    uint8_t b[6];
    if (!rt_i2c_begin(SDA_PIN, SCL_PIN) || !rt_i2c_write(ADDR, cmd, 2)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));                          // the datasheet gives 15 ms for this mode
    if (!rt_i2c_read(ADDR, b, 6)) {
        return false;
    }
    s_temp = -45.0f + 175.0f * (float)(b[0] << 8 | b[1]) / 65535.0f;       // bytes 2 and 5 are checksums (not checked here)
    s_hum = 100.0f * (float)(b[3] << 8 | b[4]) / 65535.0f;
    return true;
}

static void task(void *arg)
{
    int failed = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (measure()) {
            failed = 0;
        } else if (++failed >= 3) {                         // unplugged: no value, so rules on it go off
            s_temp = s_hum = NAN;
        }
    }
}

static void template_start(void)
{
    if (!rt_board_pin_free(SDA_PIN) || !rt_board_pin_free(SCL_PIN) || !measure()) {
        return;                                             // wrong pins for this board, or no part: no inputs
    }
    rt_input_register("room_temp", "C", 0, 40, get_temp);   // name (letters, digits, _), unit, the usual range, getter
    rt_input_register("room_humidity", "%", 0, 100, get_hum);
    xTaskCreatePinnedToCore(task, "sht31", 3072, NULL, 2, NULL, 0);
}

RT_SENSOR(template_start)
