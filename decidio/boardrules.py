"""Board rules (`actions:`): from the way you write them in yaml to the JSON the board reads (firmware/main/rules.c).
(Not to be confused with rules.py, which turns dataset annotations into labels.)

In yaml (under `actions:` in decidio.yaml, or `rules:` in a recipe):

    - name: washer_done
      when: [home_sound = appliance]      # one condition or a list of up to 3; all must hold
      above: 0.7                          # the probability a condition needs (default 0.8)
      for_ms: 120000                      # must hold this long before the rule turns on
      hold_ms: 60000                      # stays on this long after it last held
      on: end                             # which change sends an alert: start (default) | end | both
      hours: "07:00-22:00"                # only in these hours (may wrap past midnight); needs Wi-Fi for the clock
      gpio: -1                            # a pin driven while the rule is on (-1: none); level: 1 = high when on
      notify: {text: "The washer finished", photo: false, cooldown_s: 600}     # or false: never alert
      count: daily                        # count events and minutes on, sent as one message a day

A condition is `question = option`, `question != option` (the option's probability is below 1 - above), either with
its own threshold (`subject = food @ 0.5`), or `radio_motion > 0.05` / `radio_motion < 0.045` for the radio movement
score. Any other named input of the firmware (firmware/main/inputs.c: the fly eye's `eye_moving`, `chip_temp`, an
input pin `pin5`, a sensor you added) is tested the same way, with > or <: `eye_moving > 0.5`, `in:my_sensor < 20`.
The old single string (`when: person_present = true`) still works.
"""
from __future__ import annotations

import re

MAX_RULES, MAX_COND, MAX_NAME, MAX_TEXT = 16, 3, 31, 71
EDGES = ("start", "end", "both")
_COND = re.compile(r"^\s*((?:in:)?[A-Za-z0-9_]+)\s*(!=|=|>|<)\s*([A-Za-z0-9_.\-]+)\s*(?:@\s*([0-9.]+))?\s*$")
# inputs the shipped firmware registers by itself; others are written with the prefix, `in:<name>`
INPUTS = ("chip_temp", "eye_moving", "eye_figures", "eye_approach", "eye_new", "eye_changed", "eye_self_motion")
_PIN = re.compile(r"^pin\d{1,2}$")


def _input(q: str) -> str | None:
    """The input's name when `q` names one (not radio_motion, which keeps its own spelling)."""
    if q.startswith("in:"):
        return q[3:]
    return q if q in INPUTS or _PIN.match(q) else None
_HOURS = re.compile(r"^\s*(\d{1,2}):(\d{2})\s*-\s*(\d{1,2}):(\d{2})\s*$")


class RuleError(ValueError):
    pass


def _clean(text: str, n: int) -> str:
    return "".join(" " if c in '"\\' or ord(c) < 32 else c for c in str(text))[:n]


def _cond(c, above: float, qmap: dict) -> dict:
    if isinstance(c, dict):                                   # already in the board's form
        c = f"{c['q']} {'<' if c.get('op') == '<' else '>'} {c['p']}" if c.get("q") == "radio_motion" or _input(str(c.get("q"))) else \
            f"{c['q']} = {c['o']} @ {c['p'] if c.get('op') != '<' else 1 - c['p']}" if c.get("op") != "<" else \
            f"{c['q']} != {c['o']} @ {1 - c['p']}"
    m = _COND.match(str(c))
    if not m:
        raise RuleError(f"condition {c!r} has an incorrect form. Use 'question = option', 'question != option' or "
                        "'radio_motion > 0.05'")
    q, op, o, own = m.groups()
    if q == "radio_motion":
        if op not in "<>":
            raise RuleError("radio_motion is a number. Use > or <, for example 'radio_motion > 0.05'")
        return {"q": q, "op": ">=" if op == ">" else "<", "p": round(float(o), 4)}
    if _input(q) and q not in qmap:
        if op not in "<>":
            raise RuleError(f"{q} is a number. Use > or <, for example '{q} > 0.5'")
        try:
            return {"q": "in:" + _input(q), "op": ">=" if op == ">" else "<", "p": round(float(o), 4)}
        except ValueError:
            raise RuleError(f"condition {c!r}: {o!r} is not a number") from None
    if op in "<>":
        raise RuleError(f"condition {c!r}: use = or != with an option. To give the condition its own threshold, add '@ 0.6'")
    if q not in qmap:
        raise RuleError(f"unknown question {q!r}. This model answers these questions: {', '.join(qmap)}")
    o = o.lower()
    if qmap[q].get("type") == "noul":
        o = {"yes": "true", "no": "false", "1": "true", "0": "false", "on": "true", "off": "false"}.get(o, o)
    if o not in qmap[q]["options"]:
        raise RuleError(f"{q} has no option {o!r}. Its options are: {', '.join(qmap[q]['options'])}")
    p = float(own) if own else above
    if not 0 < p < 1:
        raise RuleError(f"condition {c!r}: the threshold must be between 0 and 1")
    return {"q": q, "o": o, "op": ">=", "p": round(p, 4)} if op == "=" else {"q": q, "o": o, "op": "<", "p": round(1 - p, 4)}


def wire(rule: dict, questions: list[dict]) -> dict:
    """One rule as the board reads it. `questions` is the model's list: [{"id", "type", "options"}, ...]."""
    qmap = {q["id"]: q for q in questions}
    above = float(rule.get("above", 0.8))
    when = rule.get("when")
    conds = when if isinstance(when, list) else [when]
    if when in (None, "", []) or len(conds) > MAX_COND:
        raise RuleError(f"`when` must have 1 to {MAX_COND} conditions")
    cs = [_cond(c, above, qmap) for c in conds]
    first = cs[0]
    name = _clean(rule.get("name") or (f"{first['q']}={first['o']}" if "o" in first else first["q"]), MAX_NAME)
    notify = rule.get("notify", True)
    n = notify if isinstance(notify, dict) else {}
    on = str(rule.get("on", "start"))
    if on not in EDGES:
        raise RuleError(f"{name}: `on` must be one of {', '.join(EDGES)}")
    out = {
        "name": name, "when": cs,
        "for_ms": int(rule.get("for_ms", 0)), "hold_ms": int(rule.get("hold_ms", 1000)), "on": on,
        "gpio": int(rule.get("gpio", -1)), "level": int(rule.get("level", 1)),
        "text": _clean(n.get("text", rule.get("text", "")), MAX_TEXT),
        "notify": bool(notify) if not isinstance(notify, dict) else True,
        "photo": bool(n.get("photo", rule.get("photo", False))),
        "cooldown_s": int(n.get("cooldown_s", rule.get("cooldown_s", 60))),
        "count": bool(rule.get("count", False)),
    }
    if out["for_ms"] < 0 or out["hold_ms"] < 0 or out["cooldown_s"] < 0:
        raise RuleError(f"{name}: each time must be 0 or more")
    hours = rule.get("hours")
    if hours:
        if isinstance(hours, (list, tuple)) and len(hours) == 2:
            a, b = int(hours[0]), int(hours[1])
        else:
            m = _HOURS.match(str(hours))
            if not m:
                raise RuleError(f"{name}: write the hours in this form: '22:00-07:00'")
            h1, m1, h2, m2 = map(int, m.groups())
            a, b = h1 * 60 + m1, h2 * 60 + m2
        if not (0 <= a < 1440 and 0 <= b <= 1440 and a != b):
            raise RuleError(f"{name}: the hours must be from 00:00 to 24:00, and the start must not be the same as the end")
        out["hours"] = [a, b]
    return out


def wire_all(rules: list[dict], questions: list[dict]) -> list[dict]:
    if len(rules) > MAX_RULES:
        raise RuleError(f"the board holds a maximum of {MAX_RULES} rules")
    return [wire(r, questions) for r in rules]


def needs(rule: dict) -> set[str]:
    """The questions a wired rule reads, "radio" if it reads the movement score, and "in:<name>" for other inputs."""
    return {"radio" if c["q"] == "radio_motion" else c["q"] for c in rule["when"]}


def simple(rule: dict, questions: list[dict]):
    """(question index, option index, threshold) when the rule is a single 'option is likely' test, else None."""
    if len(rule["when"]) != 1 or rule["when"][0]["op"] != ">=" or "o" not in rule["when"][0]:
        return None
    c = rule["when"][0]
    qi = [q["id"] for q in questions].index(c["q"])
    return qi, questions[qi]["options"].index(c["o"]), c["p"]
