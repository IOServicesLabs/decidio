// chip_temp: the temperature of the ESP32-S3 chip itself, in degrees Celsius. It needs no extra part, so it is the
// working example of a sensor file. The chip runs warmer than the room (about 15-25 C above it here), so use it for
// "the board is getting hot" or for changes, not as a room thermometer.
//
// A sensor file has three parts:
//   1. a start function that sets the hardware up and registers one input per number (rt_input_register);
//   2. a getter per input: the value now, or NAN while there is none. Getters are called from the decision task several
//      times a second and from the web server, so they must return at once: read slow hardware elsewhere and cache;
//   3. RT_SENSOR(start function) at the end, so the firmware calls it at start. No other file needs a change.
#include <math.h>

#include "driver/temperature_sensor.h"
#include "esp_timer.h"
#include "rt.h"

static temperature_sensor_handle_t s_h;

static float chip_temp(void)
{
    static float last = NAN;
    static int64_t at;
    const int64_t now = esp_timer_get_time();
    if (s_h && (isnan(last) || now - at > 1000000)) {     // read at most once a second
        float c;
        if (temperature_sensor_get_celsius(s_h, &c) == ESP_OK) {
            last = c;
            at = now;
        }
    }
    return last;
}

static void chip_temp_start(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&cfg, &s_h) != ESP_OK || temperature_sensor_enable(s_h) != ESP_OK) {
        s_h = NULL;
        return;                                             // no input: rules that name it are refused, the page omits it
    }
    rt_input_register("chip_temp", "C", 20, 80, chip_temp);
}

RT_SENSOR(chip_temp_start)
