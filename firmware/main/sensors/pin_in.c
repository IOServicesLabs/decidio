// pin_in: header pins as inputs, for parts that just switch a pin: a PIR movement sensor, a door reed switch, a button,
// a float switch. Up to four pins. Each becomes the input "pin<N>": 1 while the pin is high, 0 while it is low.
//
//   pin in <gpio> [up|down|none]   read this pin (default: pull-up, so a switch to ground reads 0 when closed)
//   pin in off <gpio>              stop reading it
//   pin in                         list
// The list is kept in the settings store ("pins_in") and takes effect at the next start, because inputs must exist
// before rules are read. Only pins the board offers to rules can be chosen, and not one a rule drives.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "rt.h"

#define MAX_PINS 4
static int s_pin[MAX_PINS], s_n;

static float level(int i)
{
    return i < s_n ? (float)gpio_get_level(s_pin[i]) : __builtin_nanf("");
}

static float pin0(void) { return level(0); }
static float pin1(void) { return level(1); }
static float pin2(void) { return level(2); }
static float pin3(void) { return level(3); }

static bool usable(int gpio)
{
    return rt_board_pin_free(gpio);
}

static void pin_in_start(void)
{
    static const rt_input_fn FN[MAX_PINS] = { pin0, pin1, pin2, pin3 };
    char list[64];
    if (!rt_store_get("pins_in", list, sizeof(list))) {
        return;
    }
    for (char *tok = strtok(list, ","); tok && s_n < MAX_PINS; tok = strtok(NULL, ",")) {
        const int gpio = atoi(tok);                         // "5u" = GPIO5 with pull-up; d = pull-down; n = none
        const char pull = tok[strlen(tok) - 1];
        if (!usable(gpio)) {
            continue;
        }
        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_INPUT);
        gpio_set_pull_mode(gpio, pull == 'd' ? GPIO_PULLDOWN_ONLY : pull == 'n' ? GPIO_FLOATING : GPIO_PULLUP_ONLY);
        char name[12];
        snprintf(name, sizeof(name), "pin%d", gpio);
        s_pin[s_n] = gpio;
        if (rt_input_register(name, "", 0, 1, FN[s_n]) >= 0) {
            s_n++;
        }
    }
}

// pin in ... on the console and POST /api/inputs on the page. Returns false with a reason in `why`.
bool rt_pin_in_set(int gpio, const char *pull, bool off, char *why, size_t cap)
{
    char list[64] = "", out[64] = "";
    rt_store_get("pins_in", list, sizeof(list));
    int n = 0, len = 0;
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        if (atoi(tok) != gpio) {
            len += snprintf(out + len, sizeof(out) - len, "%s%s", len ? "," : "", tok);
            n++;
        }
    }
    if (!off) {
        if (!usable(gpio)) {
            snprintf(why, cap, "pin %d is not one of this board's free pins", gpio);
            return false;
        }
        if (n >= MAX_PINS) {
            snprintf(why, cap, "at most %d input pins", MAX_PINS);
            return false;
        }
        const char p = pull && pull[0] == 'd' ? 'd' : pull && pull[0] == 'n' ? 'n' : 'u';
        snprintf(out + len, sizeof(out) - len, "%s%d%c", len ? "," : "", gpio, p);
    }
    rt_store_set("pins_in", out[0] ? out : NULL);
    snprintf(why, cap, "%s", out);
    return true;
}

RT_SENSOR(pin_in_start)
