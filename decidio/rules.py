"""Turn dataset classes into answers for the project's questions.

In decidio.yaml, every source has `labels:` mapping question -> option -> dataset classes:

  labels:
    person_present: {true: [person]}                       # noul: true if any is visible, else false
    vehicle_type:   {car: [car], bus: [bus], none: []}     # choice/score: largest visible object wins; [] = when none
    sound:          {siren: [siren], other: "*"}           # "*" = every class not listed elsewhere
    my_cat:         {false: "*"}                           # none of this source shows it: all of it is "false"
    thing:          same                                   # every option is the class of the same name (none: no class)

COCO sources also take `caption_labels:` in the same shape, where the "classes" are words or phrases looked for in the
photo's captions:  indoors: {true: [kitchen, bedroom, indoors], false: [street, beach, outdoors]}  (see for_captions).

For detection sources, an object that is in view but too small or cut off makes the answer unknown
(masked in training) instead of false, so the model is never told "no person" about a tiny person.
"""
from __future__ import annotations

from dataclasses import dataclass

from .config import ConfigError, Project


@dataclass
class Obj:
    cls: str
    area: float          # in the crop, 0..1
    visible: bool        # big enough and mostly inside the crop


def _norm(rule) -> dict:
    out = {}
    for opt, classes in (rule or {}).items():
        key = str(opt).lower()
        if isinstance(classes, str):
            out[key] = "*" if classes.strip() == "*" else [classes.strip().lower()]
        else:
            out[key] = [str(c).strip().lower() for c in (classes or [])]
    return out


def check(p: Project, rules: dict) -> dict:
    qm = p.qmap()
    clean = {}
    for qid, rule in (rules or {}).items():
        if qid not in qm:
            raise ConfigError(f"labels: unknown question {qid!r}")
        if rule == "same":                     # every option is the dataset class of the same name; none = no class
            rule = {o: ([] if o == "none" else [o]) for o in qm[qid].options}
        r = _norm(rule)
        for opt in r:
            p.option_index(qm[qid], opt)
        clean[qid] = r
    return clean


def for_objects(p: Project, rules: dict, objs: list[Obj]) -> dict[str, str | None]:
    qm, out = p.qmap(), {}
    for qid, rule in rules.items():
        q = qm[qid]
        if q.qtype == "noul":
            t, f = rule.get("true", []), rule.get("false")
            t_vis = any(o.visible and _match(o.cls, t, rule) for o in objs)
            t_amb = any((not o.visible) and _match(o.cls, t, rule) for o in objs)
            if t_vis:
                out[qid] = "true"
            elif t_amb:
                out[qid] = None
            elif f is None or f == "*":
                out[qid] = "false"
            else:
                out[qid] = "false" if any(o.visible and _match(o.cls, f, rule) for o in objs) else None
            continue
        best, best_area, amb = None, -1.0, False
        for o in objs:
            opt = _option_for(o.cls, rule)
            if opt is None:
                continue
            if not o.visible:
                amb = True
            elif o.area > best_area:
                best, best_area = opt, o.area
        if best is not None:
            out[qid] = best
        elif amb:
            out[qid] = None
        else:
            out[qid] = next((k for k, v in rule.items() if v == []), None)
    return out


def for_class(p: Project, rules: dict, cls: str) -> dict[str, str | None]:
    out = {}
    for qid, rule in rules.items():
        if p.qmap()[qid].qtype == "noul" and "false" not in rule:
            out[qid] = "true" if _match(cls, rule.get("true", []), rule) else "false"
        else:
            out[qid] = _option_for(cls, rule)
    return out


def for_captions(p: Project, rules: dict, captions: list[str]) -> dict[str, str | None]:
    """Answers for a whole photo from its captions (COCO has five per photo). An option's classes are words or phrases;
    it gets one vote per caption that has any of them. An answer needs two votes and more than every other option;
    the "*" option (or a noul's unlisted false) is the answer when no caption votes at all; anything else is unknown."""
    import re
    text = [" " + re.sub(r"[^a-z0-9]+", " ", c.lower()) + " " for c in captions]
    out: dict[str, str | None] = {}
    for qid, rule in rules.items():
        votes = {o: sum(any(f" {k} " in c for k in ks) for c in text) for o, ks in rule.items() if ks != "*"}
        rest = next((o for o, ks in rule.items() if ks == "*"), None)
        if p.qmap()[qid].qtype == "noul" and "false" not in rule:
            rest = "false"
        ranked = sorted(votes.items(), key=lambda kv: -kv[1])
        top, second = ranked[0], (ranked[1][1] if len(ranked) > 1 else 0)
        if top[1] >= 2 and top[1] > second:
            out[qid] = top[0]
        elif top[1] == 0 and text:
            out[qid] = rest
        else:
            out[qid] = None
    return out


def _match(cls: str, classes, rule) -> bool:
    if classes == "*":
        listed = {c for v in rule.values() if v != "*" for c in v}
        return cls.lower() not in listed
    return cls.lower() in classes


def _option_for(cls: str, rule) -> str | None:
    for opt, classes in rule.items():
        if classes not in ("*", []) and cls.lower() in classes:
            return opt
    for opt, classes in rule.items():
        if classes == "*":
            return opt
    return None
