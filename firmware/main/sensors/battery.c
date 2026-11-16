// battery: the voltage of a LiPo battery through two resistors, for boards on a battery (low power, power.c).
//
// The XIAO ESP32S3 charges a battery on its BAT pads, but no pin of the chip sees the battery voltage. Connect two equal
// resistors (for example 2 x 220 kOhm) in series from BAT+ to GND, and their middle to a free pin from 1 to 10 (the
// chip's first ADC). The pin then sees half the battery voltage: the ratio is 2. Large resistors take little current
// (about 9 uA with 2 x 220 kOhm).
//
// The pin and the ratio are in the settings store ("bat_pin", "bat_ratio"; the Power tab sets them) and take effect at
// the next start. Inputs: "battery_v" (volts) and "battery_pct" (0-100, from the voltage of a resting LiPo cell: an
// estimate, it reads low while the board sends). A low-battery alert is a rule: {"q":"in:battery_pct","below":15}.
#include <math.h>
#include <stdlib.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "rt.h"

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static float s_ratio = 2;

static float volts(void)
{
    static float last = NAN;
    static int64_t at;
    const int64_t now = esp_timer_get_time();
    if (s_adc && (isnan(last) || now - at > 2000000)) {     // read at most every 2 s, 16 samples
        int sum = 0, n = 0;
        for (int i = 0; i < 16; i++) {
            int raw, mv;
            if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
                sum += mv;
                n++;
            }
        }
        if (n) {
            last = sum / (float)n / 1000.0f * s_ratio;
            at = now;
        }
    }
    return last;
}

static float percent(void)
{
    // a resting LiPo cell: volts at 100, 90, ..., 0 %
    static const float V[] = { 4.20f, 4.08f, 3.98f, 3.90f, 3.84f, 3.79f, 3.75f, 3.71f, 3.66f, 3.55f, 3.30f };
    const float v = volts();
    if (isnan(v)) {
        return NAN;
    }
    if (v >= V[0]) {
        return 100;
    }
    for (int i = 1; i < 11; i++) {
        if (v >= V[i]) {
            return 100 - 10 * (i - (v - V[i]) / (V[i - 1] - V[i]));
        }
    }
    return 0;
}

static void battery_start(void)
{
    char v[16];
    if (!rt_store_get("bat_pin", v, sizeof(v))) {
        return;
    }
    const int pin = atoi(v);
    if (pin < 1 || pin > 10 || !rt_board_pin_free(pin)) {
        return;
    }
    if (rt_store_get("bat_ratio", v, sizeof(v))) {
        const float r = strtof(v, NULL);
        s_ratio = r >= 1 && r <= 10 ? r : 2;
    }
    adc_unit_t unit;
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    const adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    const adc_cali_curve_fitting_config_t kcfg = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                                   .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_io_to_channel(pin, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1 ||
        adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        s_adc = NULL;
        return;
    }
    if (adc_oneshot_config_channel(s_adc, s_chan, &ccfg) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&kcfg, &s_cali) != ESP_OK) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return;
    }
    rt_input_register("battery_v", "V", 3.0f, 4.3f, volts);
    rt_input_register("battery_pct", "%", 0, 100, percent);
}

RT_SENSOR(battery_start)
