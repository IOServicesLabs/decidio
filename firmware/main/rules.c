// Rules: reading them, writing them back out, and deciding when one is on. No hardware in this file, so the same
// code is tested on a PC (tests/test_rules.py).
//
// A rule, as JSON (in the model package, in the settings store, and on the web page):
//   {"name":"washer_done",
//    "when":[{"q":"home_sound","o":"appliance","op":">=","p":0.7}],   up to 3 conditions, all must hold; op ">=" or "<"
//    "for_ms":120000,          the conditions must hold this long before the rule turns on
//    "hold_ms":60000,          it stays on this long after they last held
//    "on":"end",               which change sends an alert: start (default) | end | both
//    "hours":[420,1320],       only between these minutes of the day (may wrap past midnight); needs the clock
//    "gpio":-1,"level":1,      a pin driven while the rule is on
//    "text":"The washer finished","notify":true,"photo":false,"cooldown_s":600,"count":true,"urgent":false}
// {"q":"radio_motion","op":">=","p":0.05} tests the radio movement score instead of a model answer.
// {"q":"in:chip_temp","op":">=","p":45} tests a named input (inputs.c): a sensor's number, the fly eye, ...; the
//   condition is false while the input has no value.
// {"q":"board:decidio-ab12","o":"occupied","op":">=","p":0.5} tests a rule of another board in the group (home.c):
//   on counts as 1, off as 0, and the condition is false while that board has not been heard from.
// "device":"lamp","device_do":"follow" switches a Smart Life device (devices.c): follow = on while the rule is on,
//   inverse = off while it is on, on / off = switch it when the rule starts.
// Older packages wrote one condition as numbers ("q":0,"o":1,"above":0.8); those still load.
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "rt.h"

#define MAX_REF 12
static struct {                 // rules of other boards that conditions name (a condition holds an index)
    bool used;
    char host[24], rule[RT_ID_LEN];
} s_ref[MAX_REF];

// The board has these in home.c and devices.c; on a PC (tests) there are no other boards and no devices.
__attribute__((weak)) float rt_peer_value(const char *host, const char *rule)
{
    return -1;
}

__attribute__((weak)) int rt_dev_index(const char *name)
{
    return -1;
}

__attribute__((weak)) int rt_input_index(const char *name)
{
    return -1;
}

__attribute__((weak)) const char *rt_input_name(int i)
{
    return "";
}

__attribute__((weak)) float rt_input_value(int i)
{
    return __builtin_nanf("");
}

__attribute__((weak)) const char *rt_dev_name(int slot)
{
    return "";
}

static int ref(const char *host, const char *rule)
{
    int free_at = -1;
    for (int i = 0; i < MAX_REF; i++) {
        if (s_ref[i].used && !strcmp(s_ref[i].host, host) && !strcmp(s_ref[i].rule, rule)) {
            return i;
        }
        if (!s_ref[i].used && free_at < 0) {
            free_at = i;
        }
    }
    if (free_at >= 0) {
        s_ref[free_at].used = true;
        snprintf(s_ref[free_at].host, sizeof(s_ref[0].host), "%s", host);
        snprintf(s_ref[free_at].rule, sizeof(s_ref[0].rule), "%s", rule);
    }
    return free_at;
}

// keep only the references the rules in a and b still use
void rt_rules_refs_keep(const rt_action_t *a, int na, const rt_action_t *b, int nb)
{
    bool keep[MAX_REF] = { false };
    for (int pass = 0; pass < 2; pass++) {
        const rt_action_t *r = pass ? b : a;
        for (int i = 0; r && i < (pass ? nb : na); i++) {
            for (int k = 0; k < r[i].n_c; k++) {
                if (r[i].c[k].q == RT_Q_PEER && r[i].c[k].o >= 0 && r[i].c[k].o < MAX_REF) {
                    keep[r[i].c[k].o] = true;
                }
            }
        }
    }
    for (int i = 0; i < MAX_REF; i++) {
        s_ref[i].used = s_ref[i].used && keep[i];
    }
}

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : (cJSON_IsBool(v) ? cJSON_IsTrue(v) : def);
}

static float jnum(const cJSON *o, const char *k, float def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void jtext(const cJSON *o, const char *k, char *dst, size_t n)      // safe to print inside a JSON string later
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    snprintf(dst, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
    for (char *c = dst; *c; c++) {
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) {
            *c = ' ';
        }
    }
}

bool rt_rule_parse(const cJSON *a, const rt_model_t *m, rt_action_t *A)
{
    memset(A, 0, sizeof(*A));
    if (!cJSON_IsObject(a)) {
        return false;
    }
    jtext(a, "name", A->name, sizeof(A->name));
    jtext(a, "text", A->text, sizeof(A->text));
    const float above = jnum(a, "above", 0.8f);
    const cJSON *when = cJSON_GetObjectItemCaseSensitive(a, "when");
    if (cJSON_IsArray(when)) {
        const cJSON *c;
        cJSON_ArrayForEach(c, when) {
            if (A->n_c >= RT_MAX_COND) {
                return false;
            }
            rt_cond_t *C = &A->c[A->n_c];
            const cJSON *q = cJSON_GetObjectItemCaseSensitive(c, "q"), *o = cJSON_GetObjectItemCaseSensitive(c, "o");
            const cJSON *op = cJSON_GetObjectItemCaseSensitive(c, "op");
            if (!cJSON_IsString(q)) {
                return false;
            }
            C->below = cJSON_IsString(op) && op->valuestring[0] == '<';
            C->p = jnum(c, "p", above);
            if (!strcmp(q->valuestring, "radio_motion")) {
                C->q = RT_Q_RADIO;
            } else if (!strncmp(q->valuestring, "in:", 3)) {
                C->q = RT_Q_INPUT;
                C->o = (int16_t)rt_input_index(q->valuestring + 3);
                if (C->o < 0) {
                    return false;                           // an input this board does not have
                }
            } else if (!strncmp(q->valuestring, "board:", 6)) {
                const char *host = q->valuestring + 6;
                if (!cJSON_IsString(o) || !host[0] || strlen(host) > 23 || strlen(o->valuestring) >= RT_ID_LEN ||
                    strpbrk(host, "\"\\") || strpbrk(o->valuestring, "\"\\")) {
                    return false;
                }
                C->q = RT_Q_PEER;
                C->o = (int16_t)ref(host, o->valuestring);
                if (C->o < 0) {
                    return false;                           // too many different rules of other boards
                }
            } else {
                C->q = -1;
                for (int i = 0; i < m->n_q; i++) {
                    if (!strcmp(m->q[i].id, q->valuestring)) {
                        C->q = (int16_t)i;
                    }
                }
                if (C->q < 0 || !cJSON_IsString(o)) {
                    return false;
                }
                C->o = -1;
                for (int i = 0; i < m->q[C->q].count; i++) {
                    if (!strcmp(m->q[C->q].opt[i], o->valuestring)) {
                        C->o = (int16_t)i;
                    }
                }
                if (C->o < 0) {
                    return false;
                }
            }
            A->n_c++;
        }
    } else {
        const int q = jint(a, "q", -1), o = jint(a, "o", -1);
        if (q < 0 || q >= m->n_q || o < 0 || o >= m->q[q].count) {
            return false;
        }
        A->c[0] = (rt_cond_t){ .q = (int16_t)q, .o = (int16_t)o, .below = false, .p = above };
        A->n_c = 1;
    }
    if (A->n_c == 0) {
        return false;
    }
    A->for_ms = jint(a, "for_ms", 0);
    A->hold_ms = jint(a, "hold_ms", 1000);
    A->gpio = jint(a, "gpio", -1);
    A->level = jint(a, "level", 1);
    A->cooldown_s = jint(a, "cooldown_s", 60);
    A->notify = jint(a, "notify", 1) != 0;
    A->photo = jint(a, "photo", 0) != 0;
    A->count = jint(a, "count", 0) != 0;
    A->urgent = jint(a, "urgent", 0) != 0;
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(a, "on");
    A->edge = !cJSON_IsString(on) ? RT_EDGE_START : !strcmp(on->valuestring, "end") ? RT_EDGE_END
              : !strcmp(on->valuestring, "both") ? RT_EDGE_BOTH : RT_EDGE_START;
    A->dev = -1;
    const cJSON *dv = cJSON_GetObjectItemCaseSensitive(a, "device"), *dd = cJSON_GetObjectItemCaseSensitive(a, "device_do");
    if (cJSON_IsString(dv) && dv->valuestring[0]) {
        A->dev = (int8_t)rt_dev_index(dv->valuestring);
        if (A->dev < 0) {
            return false;                                   // a device that is not on this board's list
        }
        A->dev_do = !cJSON_IsString(dd) ? RT_DEV_FOLLOW : !strcmp(dd->valuestring, "inverse") ? RT_DEV_INVERSE
                    : !strcmp(dd->valuestring, "on") ? RT_DEV_ON : !strcmp(dd->valuestring, "off") ? RT_DEV_OFF : RT_DEV_FOLLOW;
    }
    A->from_min = A->to_min = -1;
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(a, "hours");
    if (cJSON_IsArray(h) && cJSON_GetArraySize(h) == 2) {
        const int f = cJSON_GetArrayItem(h, 0)->valueint, t = cJSON_GetArrayItem(h, 1)->valueint;
        if (f >= 0 && f < 1440 && t >= 0 && t <= 1440 && f != t) {
            A->from_min = (int16_t)f;
            A->to_min = (int16_t)t;
        }
    }
    if (!A->name[0]) {
        snprintf(A->name, sizeof(A->name), "rule");
    }
    return A->for_ms >= 0 && A->hold_ms >= 0;
}

// a JSON array of rules into `out`; returns how many were kept (unknown questions and options are skipped)
int rt_rules_parse(const cJSON *arr, const rt_model_t *m, rt_action_t *out, int max)
{
    int n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, arr) {
        if (n < max && rt_rule_parse(a, m, &out[n])) {
            n++;
        }
    }
    return n;
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

int rt_rules_json(const rt_model_t *m, const rt_action_t *act, int n_act, char *dst, size_t cap)
{
    static const char *EDGE[] = { "start", "end", "both" }, *DO[] = { "follow", "inverse", "on", "off" };
    int n = 0;
    ADD("[");
    for (int i = 0; i < n_act; i++) {
        const rt_action_t *a = &act[i];
        ADD("%s{\"name\":\"%s\",\"when\":[", i ? "," : "", a->name);
        for (int k = 0; k < a->n_c; k++) {
            const rt_cond_t *c = &a->c[k];
            if (c->q == RT_Q_RADIO) {
                ADD("%s{\"q\":\"radio_motion\"", k ? "," : "");
            } else if (c->q == RT_Q_INPUT) {
                ADD("%s{\"q\":\"in:%s\"", k ? "," : "", rt_input_name(c->o));
            } else if (c->q == RT_Q_PEER) {
                ADD("%s{\"q\":\"board:%s\",\"o\":\"%s\"", k ? "," : "", s_ref[c->o].host, s_ref[c->o].rule);
            } else {
                ADD("%s{\"q\":\"%s\",\"o\":\"%s\"", k ? "," : "", m->q[c->q].id, m->q[c->q].opt[c->o]);
            }
            ADD(",\"op\":\"%s\",\"p\":%.*f}", c->below ? "<" : ">=", c->p > 10 || c->p < -10 ? 1 : 3, c->p);
        }
        ADD("],\"for_ms\":%d,\"hold_ms\":%d,\"on\":\"%s\",\"gpio\":%d,\"level\":%d,\"text\":\"%s\",\"notify\":%s,"
            "\"photo\":%s,\"cooldown_s\":%d,\"count\":%s,\"urgent\":%s", a->for_ms, a->hold_ms, EDGE[a->edge], a->gpio, a->level,
            a->text, a->notify ? "true" : "false", a->photo ? "true" : "false", a->cooldown_s, a->count ? "true" : "false",
            a->urgent ? "true" : "false");
        if (a->from_min >= 0) {
            ADD(",\"hours\":[%d,%d]", a->from_min, a->to_min);
        }
        if (a->dev >= 0 && rt_dev_name(a->dev)[0]) {
            ADD(",\"device\":\"%s\",\"device_do\":\"%s\"", rt_dev_name(a->dev), DO[a->dev_do & 3]);
        }
        ADD("}");
    }
    ADD("]");
    return n < (int)cap ? n : -1;
}

// One decision's worth of time for one rule. probs: the model's answers; radio: the movement score, < 0 = not running;
// minute: minute of the day, -1 = the clock is not set. `o` says what changed.
void rt_rule_step(const rt_action_t *a, rt_rule_state_t *s, const rt_model_t *m, const float *probs, float radio,
                  int64_t now_us, int minute, rt_rule_out_t *o)
{
    bool all = a->n_c > 0;
    for (int k = 0; k < a->n_c && all; k++) {
        const rt_cond_t *c = &a->c[k];
        float v;
        if (c->q == RT_Q_RADIO) {
            if (radio < 0) {
                all = false;
                break;
            }
            v = radio;
        } else if (c->q == RT_Q_PEER) {
            v = rt_peer_value(s_ref[c->o].host, s_ref[c->o].rule);
            if (v < 0) {
                all = false;                                // that board is not there: the condition does not hold
                break;
            }
        } else if (c->q == RT_Q_INPUT) {
            v = rt_input_value(c->o);
            if (v != v) {
                all = false;                                // no value now
                break;
            }
        } else {
            if (m->q[c->q].na) {
                all = false;                                // the board lacks the sensor behind this question
                break;
            }
            v = probs[m->q[c->q].offset + c->o];
        }
        if (k == 0) {
            s->p = v;
        }
        all = c->below ? v < c->p : v >= c->p;
    }
    if (all && a->from_min >= 0) {
        all = minute >= 0 && (a->from_min < a->to_min ? (minute >= a->from_min && minute < a->to_min)
                                                       : (minute >= a->from_min || minute < a->to_min));
    }
    if (all) {
        if (!s->since_us) {
            s->since_us = now_us ? now_us : 1;
        }
    } else {
        s->since_us = 0;
    }
    const bool fire = s->since_us && (now_us - s->since_us) >= (int64_t)a->for_ms * 1000;
    if (fire) {
        s->until_us = now_us + (int64_t)a->hold_ms * 1000;
    }
    const bool on = fire || now_us < s->until_us;
    o->changed = on != s->active;
    o->on = on;
    o->notify = false;
    if (!o->changed) {
        return;
    }
    s->active = on;
    if (on) {
        s->events++;
        s->on_since_us = now_us;
    } else {
        s->on_s += (uint32_t)((now_us - s->on_since_us) / 1000000);
    }
    const bool wanted = a->notify && (a->edge == RT_EDGE_BOTH || (a->edge == RT_EDGE_START) == on);
    if (wanted && (!s->noted || now_us - s->last_note_us >= (int64_t)a->cooldown_s * 1000000)) {
        s->noted = true;
        s->last_note_us = now_us;
        o->notify = true;
    }
}
