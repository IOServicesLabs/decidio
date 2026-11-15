// Runs firmware/main/rules.c on a PC with a made-up clock: reads rules (JSON array, argv[1]) for a small fixed model,
// then one line per decision on stdin: "<time ms> <minute of day or -1> <radio score or -1> <p person> <p appliance>",
// with an optional sixth number: the rule "occupied" of another board "decidio-hall" (1 on, 0 off, -1 not heard).
// A seventh number is the named input "eye_moving" (-1 = no value now). The board has one switchable device, "lamp".
// With the environment variable NA_SOUND set, the board has no microphone: home_sound cannot be answered.
// Prints what the firmware understood, then per decision what every rule did.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "rt.h"

static float s_peer = -1, s_input = -1;

int rt_input_index(const char *name)
{
    return !strcmp(name, "eye_moving") ? 3 : -1;
}

const char *rt_input_name(int i)
{
    return i == 3 ? "eye_moving" : "";
}

float rt_input_value(int i)
{
    return i == 3 && s_input >= 0 ? s_input : __builtin_nanf("");
}

float rt_peer_value(const char *host, const char *rule)
{
    return !strcmp(host, "decidio-hall") && !strcmp(rule, "occupied") ? s_peer : -1;
}

int rt_dev_index(const char *name)
{
    return !strcmp(name, "lamp") ? 2 : -1;
}

const char *rt_dev_name(int slot)
{
    return slot == 2 ? "lamp" : "";
}

int main(int argc, char **argv)
{
    static rt_model_t m;
    static rt_action_t act[RT_MAX_ACT];
    static rt_rule_state_t st[RT_MAX_ACT];
    static char text[1 << 16], out[1 << 16];
    m.n_q = 2;
    snprintf(m.q[0].id, sizeof(m.q[0].id), "person_present");
    m.q[0].offset = 0; m.q[0].count = 2;
    snprintf(m.q[0].opt[0], sizeof(m.q[0].opt[0]), "false");
    snprintf(m.q[0].opt[1], sizeof(m.q[0].opt[1]), "true");
    snprintf(m.q[1].id, sizeof(m.q[1].id), "home_sound");
    m.q[1].offset = 2; m.q[1].count = 3;
    snprintf(m.q[1].opt[0], sizeof(m.q[1].opt[0]), "none");
    snprintf(m.q[1].opt[1], sizeof(m.q[1].opt[1]), "appliance");
    snprintf(m.q[1].opt[2], sizeof(m.q[1].opt[2]), "water");
    m.q[1].na = getenv("NA_SOUND") != NULL;

    FILE *f = fopen(argc > 1 ? argv[1] : "", "rb");
    if (!f) {
        return 2;
    }
    const size_t len = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[len] = 0;
    cJSON *arr = cJSON_Parse(text);
    const int n = rt_rules_parse(arr, &m, act, RT_MAX_ACT);
    const int k = rt_rules_json(&m, act, n, out, sizeof(out));
    printf("{\"asked\":%d,\"n\":%d,\"json\":%s}\n", cJSON_GetArraySize(arr), n, k < 0 ? "null" : out);
    cJSON_Delete(arr);

    long long ms;
    int minute;
    float radio, pp, pa;
    char line[200];
    while (fgets(line, sizeof(line), stdin)) {
        const int got = sscanf(line, "%lld %d %f %f %f %f %f", &ms, &minute, &radio, &pp, &pa, &s_peer, &s_input);
        if (got < 5) {
            break;
        }
        if (got < 6) {
            s_peer = -1;
        }
        if (got < 7) {
            s_input = -1;
        }
        const float probs[5] = { 1 - pp, pp, 1 - pa, pa, 0 };
        printf("[");
        for (int i = 0; i < n; i++) {
            rt_rule_out_t o;
            rt_rule_step(&act[i], &st[i], &m, probs, radio, ms * 1000, minute, &o);
            printf("%s[%d,%d,%d,%lu,%lu]", i ? "," : "", o.changed, o.on, o.notify, (unsigned long)st[i].events,
                   (unsigned long)st[i].on_s);
        }
        printf("]\n");
    }
    return 0;
}
