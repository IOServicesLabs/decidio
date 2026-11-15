"""The board's rule engine (firmware/main/rules.c) compiled for the PC and driven with a made-up clock:
timing (for_ms, hold_ms), "and" / "not", which change alerts, cooldown, hours, the radio score, counting, and that
what `decidio` writes (boardrules.wire) is what the firmware reads back, a rule of another board as a condition, and a
rule that switches a device.

Needs gcc and cJSON's source (CJSON_DIR, e.g. <esp-idf>/components/json/cJSON); skipped without them."""
import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from decidio import boardrules

ROOT = Path(__file__).resolve().parent.parent
CJSON = Path(os.environ.get("CJSON_DIR", "/nonexistent"))
QUESTIONS = [{"id": "person_present", "type": "noul", "options": ["false", "true"]},
             {"id": "home_sound", "type": "choice", "options": ["none", "appliance", "water"]}]

pytestmark = pytest.mark.skipif(not shutil.which("gcc") or not (CJSON / "cJSON.c").exists(),
                                reason="needs gcc and CJSON_DIR pointing at cJSON's source")


@pytest.fixture(scope="module")
def exe(tmp_path_factory):
    out = tmp_path_factory.mktemp("rules") / "rules"
    subprocess.run(["gcc", "-O1", "-std=gnu11", "-Wall", "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "firmware/main"),
                    "-I", str(CJSON), str(ROOT / "tests/host/rules_main.c"), str(ROOT / "firmware/main/rules.c"),
                    str(CJSON / "cJSON.c"), "-lm", "-o", str(out)], check=True)
    return out


def run(exe, tmp_path, rules, steps, wired=False, env=None):
    """steps: (ms, minute, radio, p_person, p_appliance). Returns (what the firmware read, [per step [per rule (changed,
    on, notify, events, on_s)]])."""
    f = tmp_path / "rules.json"
    f.write_text(json.dumps(rules if wired else boardrules.wire_all(rules, QUESTIONS)))
    text = "".join(" ".join(str(x) for x in step) + "\n" for step in steps)      # a sixth number: the other board's rule
    lines = subprocess.run([str(exe), str(f)], input=text, capture_output=True, check=True, text=True,
                           env={**os.environ, **(env or {})}).stdout.splitlines()
    return json.loads(lines[0]), [json.loads(x) for x in lines[1:]]


def ticks(until_ms, p_person=0.0, p_app=0.0, start=0, step=100, minute=-1, radio=-1):
    return [(t, minute, radio, p_person, p_app) for t in range(start, until_ms, step)]


def on_at(steps, out, rule=0):
    """times (ms) at which the rule turned on / off / alerted"""
    on = [s[0] for s, o in zip(steps, out) if o[rule][0] and o[rule][1]]
    off = [s[0] for s, o in zip(steps, out) if o[rule][0] and not o[rule][1]]
    note = [s[0] for s, o in zip(steps, out) if o[rule][2]]
    return on, off, note


def test_firmware_reads_what_decidio_writes(exe, tmp_path):
    rules = [
        {"name": "washer_done", "when": ["home_sound = appliance"], "above": 0.7, "for_ms": 120000, "hold_ms": 60000,
         "on": "end", "hours": "07:00-22:00", "notify": {"text": "The washer finished", "cooldown_s": 600}, "count": "daily"},
        {"name": "tv_on_alone", "when": ["person_present != true", "home_sound = water @ 0.5", "radio_motion < 0.045"],
         "gpio": 3, "level": 0, "notify": False},
        {"name": "legacy", "when": "person_present = yes"},
    ]
    wired = boardrules.wire_all(rules, QUESTIONS)
    head, _ = run(exe, tmp_path, rules, [])
    assert head["asked"] == head["n"] == 3
    for mine, theirs in zip(wired, head["json"]):
        for c, d in zip(mine["when"], theirs["when"]):
            assert (c["q"], c.get("o"), c["op"]) == (d["q"], d.get("o"), d["op"]) and abs(c["p"] - d["p"]) < 1e-3
        assert {k: v for k, v in mine.items() if k != "when"} == {k: v for k, v in theirs.items() if k != "when"}
    assert wired[0]["hours"] == [420, 1320] and wired[1]["when"][0] == {"q": "person_present", "o": "true", "op": "<", "p": 0.2}


def test_old_packages_and_bad_rules(exe, tmp_path):
    old = [{"name": "person", "q": 0, "o": 1, "above": 0.8, "for_ms": 300, "gpio": -1, "level": 1, "hold_ms": 2000}]
    head, _ = run(exe, tmp_path, old, [], wired=True)
    assert head["n"] == 1 and head["json"][0]["when"] == [{"q": "person_present", "o": "true", "op": ">=", "p": 0.8}]
    bad = [{"name": "a", "when": [{"q": "nope", "o": "true", "op": ">=", "p": 0.5}]},
           {"name": "b", "when": [{"q": "person_present", "o": "maybe", "op": ">=", "p": 0.5}]},
           {"name": "c", "when": [{"q": "person_present", "o": "true", "op": ">=", "p": 0.5}] * 4},
           {"name": "d", "when": []}]
    head, _ = run(exe, tmp_path, bad, [], wired=True)
    assert head["asked"] == 4 and head["n"] == 0
    for text in ("nope = true", "person_present = maybe", "person_present > 0.5", "radio_motion = 1"):
        with pytest.raises(boardrules.RuleError):
            boardrules.wire({"when": text}, QUESTIONS)


def test_for_and_hold(exe, tmp_path):
    rule = [{"name": "r", "when": "person_present = true", "above": 0.8, "for_ms": 1000, "hold_ms": 2000}]
    steps = (ticks(500) + ticks(900, 0.9, start=500) + ticks(1000, 0.1, start=900)      # 400 ms: too short
             + ticks(3000, 0.9, start=1000) + ticks(7000, 0.1, start=3000))
    _, out = run(exe, tmp_path, rule, steps)
    on, off, note = on_at(steps, out)
    assert on == [2000] and note == [2000]             # 1 s after it started holding at 1000
    assert off == [4900]                               # last held at 2900, plus 2 s
    assert out[-1][0][3:] == [1, 2]                    # one event, on for 2 s (whole seconds)


def test_and_not(exe, tmp_path):
    rule = [{"name": "alone", "when": ["person_present != true", "home_sound = appliance"], "above": 0.7, "hold_ms": 0}]
    steps = (ticks(500, 0.9, 0.9) + ticks(1000, 0.1, 0.1, start=500) + ticks(1500, 0.5, 0.9, start=1000)
             + ticks(2000, 0.2, 0.9, start=1500))      # "not" needs the probability under 1 - 0.7
    _, out = run(exe, tmp_path, rule, steps)
    assert on_at(steps, out)[0] == [1500]


def test_edges_and_cooldown(exe, tmp_path):
    base = {"when": "person_present = true", "hold_ms": 0, "notify": {"cooldown_s": 3}}
    rules = [dict(base, name="start"), dict(base, name="end", on="end"), dict(base, name="both", on="both"),
             dict(base, name="quiet", notify=False)]
    steps = ticks(10000)                               # the board has been up for 10 s
    for k in range(5, 9):                              # then on for 1 s, off for 1 s, four times
        steps += ticks(2000 * k + 1000, 0.9, start=2000 * k) + ticks(2000 * k + 2000, 0.1, start=2000 * k + 1000)
    _, out = run(exe, tmp_path, rules, steps)
    assert on_at(steps, out, 0)[2] == [10000, 14000]   # every 2 s, but at most one alert per 3 s
    assert on_at(steps, out, 1)[2] == [11000, 15000]
    assert on_at(steps, out, 2)[2] == [10000, 13000, 16000]
    assert on_at(steps, out, 3)[2] == [] and on_at(steps, out, 3)[0] == [10000, 12000, 14000, 16000]
    assert out[-1][3][3] == 4                          # counted all the same


def test_hours(exe, tmp_path):
    rules = [{"name": "night", "when": "person_present = true", "hours": "22:00-07:00", "hold_ms": 0},
             {"name": "day", "when": "person_present = true", "hours": "07:00-22:00", "hold_ms": 0}]
    for minute, want in ((-1, [0, 0]), (23 * 60, [1, 0]), (3 * 60, [1, 0]), (7 * 60, [0, 1]), (12 * 60, [0, 1]),
                         (22 * 60, [1, 0]), (21 * 60 + 59, [0, 1])):
        _, out = run(exe, tmp_path, rules, ticks(300, 0.9, minute=minute))
        assert [r[1] for r in out[-1]] == want, minute      # without a clock (-1) neither runs


def test_radio(exe, tmp_path):
    rules = [{"name": "move", "when": "radio_motion > 0.055", "for_ms": 500, "hold_ms": 0},
             {"name": "still", "when": ["radio_motion < 0.045"], "hold_ms": 0}]
    steps = ticks(1000, radio=-1) + ticks(2000, radio=0.04, start=1000) + ticks(3000, radio=0.08, start=2000)
    _, out = run(exe, tmp_path, rules, steps)
    assert on_at(steps, out, 0)[0] == [2500]           # off while the score is not running, then after 0.5 s of movement
    assert on_at(steps, out, 1)[:2] == ([1000], [2000])


def test_other_board_and_device(exe, tmp_path):
    cond = lambda op: [{"q": "board:decidio-hall", "o": "occupied", "op": op, "p": 0.5}]      # noqa: E731
    rules = [{"name": "hall", "when": cond(">="), "hold_ms": 0, "device": "lamp", "device_do": "inverse"},
             {"name": "empty", "when": cond("<"), "hold_ms": 0, "device": "lamp"},
             {"name": "no_such_device", "when": cond(">="), "device": "heater"},
             {"name": "plain", "when": [{"q": "person_present", "o": "true", "op": ">=", "p": 0.5}], "hold_ms": 0}]
    steps = ([t + (-1,) for t in ticks(500, 0.9)] + [t + (0,) for t in ticks(1000, 0.9, start=500)]
             + [t + (1,) for t in ticks(1500, 0.9, start=1000)] + [t + (-1,) for t in ticks(2000, 0.9, start=1500)])
    head, out = run(exe, tmp_path, rules, steps, wired=True)
    assert head["asked"] == 4 and head["n"] == 3                  # a device that is not on the board's list: refused
    back = head["json"]
    assert back[0]["when"] == cond(">=") and (back[0]["device"], back[0]["device_do"]) == ("lamp", "inverse")
    assert (back[1]["device"], back[1]["device_do"]) == ("lamp", "follow") and "device" not in back[2]
    assert on_at(steps, out, 0)[:2] == ([1000], [1500])           # on while the other board's rule is on
    assert on_at(steps, out, 1)[:2] == ([500], [1000])            # "is off" holds only while that board is heard
    assert on_at(steps, out, 2)[0] == [100]                       # a rule without another board does not care



def test_named_inputs(exe, tmp_path):
    """`eye_moving > 0.5` becomes {"q":"in:eye_moving"}; it holds on the input's value and never without one; a rule on
    an input the board does not have is refused, the others stay."""
    rules = [{"name": "moving", "when": "eye_moving > 0.5", "for_ms": 200, "hold_ms": 300, "notify": False},
             {"name": "still_person", "when": ["person_present = true", "in:eye_moving < 0.5"], "for_ms": 0, "hold_ms": 0, "notify": False},
             {"name": "no_such", "when": "in:not_there > 1", "notify": False}]
    wired = boardrules.wire_all(rules, QUESTIONS)
    assert wired[0]["when"] == [{"q": "in:eye_moving", "op": ">=", "p": 0.5}] and boardrules.needs(wired[0]) == {"in:eye_moving"}
    with pytest.raises(boardrules.RuleError):
        boardrules.wire({"name": "x", "when": "eye_moving = true"}, QUESTIONS)
    steps = ([(t, -1, -1, 0.9, 0.0, -1, 0) for t in range(0, 500, 100)] +          # a person, nothing moves
             [(t, -1, -1, 0.9, 0.0, -1, 1) for t in range(500, 1000, 100)] +       # the person moves
             [(t, -1, -1, 0.9, 0.0) for t in range(1000, 2000, 100)])              # the eye is off: no value
    head, out = run(exe, tmp_path, rules, steps)
    assert head["asked"] == 3 and head["n"] == 2
    assert [r["when"] for r in head["json"]] == [w["when"] for w in wired[:2]]
    assert on_at(steps, out, 0)[:2] == ([700], [1200])            # 200 ms after it started; 300 ms after the last value
    assert on_at(steps, out, 1)[:2] == ([100], [500])             # "below" needs a value: it is not on while the eye is off


def test_question_without_its_sensor(exe, tmp_path):
    """On a board without a microphone a sound question is never true and never false: rules on it stay off."""
    rules = [{"name": "washer", "when": "home_sound = appliance", "for_ms": 0, "hold_ms": 0, "notify": False},
             {"name": "quiet", "when": "home_sound != appliance", "for_ms": 0, "hold_ms": 0, "notify": False},
             {"name": "person", "when": "person_present = true", "for_ms": 0, "hold_ms": 0, "notify": False}]
    steps = ticks(500, p_person=0.9, p_app=0.9) + ticks(1000, p_person=0.9, p_app=0.0, start=500)
    _, out = run(exe, tmp_path, rules, steps)
    assert on_at(steps, out, 0)[0] == [100] and on_at(steps, out, 1)[0] == [500]
    _, out = run(exe, tmp_path, rules, steps, env={"NA_SOUND": "1"})
    assert on_at(steps, out, 0)[0] == [] and on_at(steps, out, 1)[0] == [] and on_at(steps, out, 2)[0] == [100]
