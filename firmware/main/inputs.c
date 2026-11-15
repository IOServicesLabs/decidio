// Named inputs: numbers a rule can test besides the model's answers. A rule condition {"q":"in:<name>","op":">=","p":x}
// reads one. Anything can add an input with rt_input_register(): a sensor (sensors/*.c), the radio, the fly eye.
// The page lists them with their live values (GET /api/inputs), so a new input needs no change to the page.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rt.h"

typedef struct {
    char name[24], unit[8];
    float lo, hi;                   // the usual range, for the page's threshold box
    rt_input_fn fn;
} input_t;
static input_t s_in[RT_MAX_INPUT];
static int s_n;

// name: letters, digits, '_' and ':' only (it goes into JSON and into rule conditions as written)
int rt_input_register(const char *name, const char *unit, float lo, float hi, rt_input_fn fn)
{
    if (!name || !fn || !name[0] || strlen(name) >= sizeof(s_in[0].name) || rt_input_index(name) >= 0 || s_n >= RT_MAX_INPUT) {
        return -1;
    }
    for (const char *c = name; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == ':')) {
            return -1;
        }
    }
    input_t *x = &s_in[s_n];
    snprintf(x->name, sizeof(x->name), "%s", name);
    snprintf(x->unit, sizeof(x->unit), "%s", unit ? unit : "");
    x->lo = lo;
    x->hi = hi;
    x->fn = fn;
    return s_n++;
}

int rt_input_index(const char *name)
{
    for (int i = 0; i < s_n; i++) {
        if (!strcmp(s_in[i].name, name)) {
            return i;
        }
    }
    return -1;
}

const char *rt_input_name(int i)
{
    return i >= 0 && i < s_n ? s_in[i].name : "";
}

float rt_input_value(int i)
{
    return i >= 0 && i < s_n ? s_in[i].fn() : NAN;
}

static void (*s_start[16])(void);
static int s_n_start;

void rt_sensor_add(void (*start)(void))
{
    if (s_n_start < (int)(sizeof(s_start) / sizeof(s_start[0]))) {
        s_start[s_n_start++] = start;
    }
}

static float radio_in(void)
{
    const float v = rt_radio_motion();
    return v < 0 ? NAN : v;
}

void rt_sensors_start(void)
{
    rt_input_register("radio_motion", "", 0, 0.5f, radio_in);
    for (int i = 0; i < s_n_start; i++) {
        s_start[i]();
    }
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

int rt_inputs_json(char *dst, size_t cap)
{
    int n = 0;
    ADD("[");
    for (int i = 0; i < s_n; i++) {
        const float v = s_in[i].fn();
        ADD("%s{\"name\":\"%s\",\"unit\":\"%s\",\"lo\":%g,\"hi\":%g,\"value\":", i ? "," : "", s_in[i].name, s_in[i].unit,
            s_in[i].lo, s_in[i].hi);
        if (isnan(v)) {
            ADD("null}");
        } else {
            ADD("%.4g}", v);
        }
    }
    ADD("]");
    return n < (int)cap ? n : -1;
}
