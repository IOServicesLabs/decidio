"""Recipes: ready-made rules for the ideas in USE_CASES.md.

The source is recipes/*.yaml in the repository. `python -m decidio.recipes` checks every recipe against the shipped
base models and writes two catalogs:

    decidio/recipes.json        everything (ready, radio and teach entries), used by `decidio builds` and `decidio install`
    firmware/main/recipes.json  the installable ones only, built into the firmware for the board's Ideas tab
"""
from __future__ import annotations

import json
from pathlib import Path

import yaml

from . import boardrules

PKG = Path(__file__).resolve().parent
SRC = PKG.parent / "recipes"
CATALOG = PKG / "recipes.json"
FW_CATALOG = PKG.parent / "firmware" / "main" / "recipes.json"
BASE_BOARD = {"general": "xiao_sense", "general-cam": "s3cam"}      # the board each shipped base is built for


def base_headers() -> dict[str, dict]:
    out = {}
    for d in sorted((PKG / "base").iterdir()):
        h = d / "out" / "model_header.json"
        if h.exists():
            out[d.name] = json.loads(h.read_text(encoding="utf-8"))
    return out


def source() -> list[dict]:
    if not SRC.is_dir():
        raise SystemExit(f"{SRC} not found. Edit the recipes in a checkout of the repository")
    out = []
    for f in sorted(SRC.glob("*.yaml")):
        for r in yaml.safe_load(f.read_text(encoding="utf-8")) or []:
            r["file"] = f.name
            out.append(r)
    return out


def check(raw: list[dict], headers: dict[str, dict]) -> list[dict]:
    """Validate and wire every recipe. Raises ValueError naming the recipe."""
    union = {}
    for h in headers.values():                               # `general` answers everything `general-cam` does
        for q in h["questions"]:
            union.setdefault(q["id"], q)
    audio = set()
    if "general" in headers and "general-cam" in headers:
        audio = {q["id"] for q in headers["general"]["questions"]} - {q["id"] for q in headers["general-cam"]["questions"]}
    seen, out = set(), []
    for r in raw:
        rid = str(r.get("id", ""))
        if not rid or rid in seen or not all(c.isalnum() or c == "-" for c in rid) or len(rid) > 31:
            raise ValueError(f"{r.get('file')}: id {rid!r} is missing, is a duplicate, is longer than 31 characters "
                             f"or has characters that are not letters, digits or dashes")
        seen.add(rid)
        status = r.get("status", "ready")
        e = {"id": rid, "idea": int(r["idea"]), "title": str(r["title"]), "status": status}
        if status == "teach":
            e.update(kind=r["kind"], question=str(r["question"]), record=str(r["record"]))
            out.append(e)
            continue
        if status not in ("ready", "radio"):
            raise ValueError(f"{rid}: status must be ready, radio or teach")
        try:
            rules = boardrules.wire_all(r.get("rules") or [], list(union.values()))
        except boardrules.RuleError as err:
            raise ValueError(f"{rid}: {err}") from None
        if not rules:
            raise ValueError(f"{rid}: no rules")
        if len({x["name"] for x in rules}) != len(rules):
            raise ValueError(f"{rid}: two rules share a name")
        need = sorted(set().union(*(boardrules.needs(x) for x in rules)))
        if ("radio" in need) != (status == "radio"):
            raise ValueError(f"{rid}: status {status} does not agree with its rules. Use status radio only for rules with radio_motion")
        qs = [n for n in need if n != "radio" and not n.startswith("in:")]
        e.update(setup=str(r["setup"]), needs=need, mic=bool(set(qs) & audio),
                 photo=any(x["photo"] for x in rules), alerts=any(x["notify"] for x in rules),
                 models=[b for b, h in headers.items() if set(qs) <= {q["id"] for q in h["questions"]}],
                 rules=rules)
        if not e["models"]:
            raise ValueError(f"{rid}: no shipped base model answers {qs}")
        out.append(e)
    return out


def build(log=print) -> list[dict]:
    cat = check(source(), base_headers())
    CATALOG.write_text(json.dumps(cat, indent=1) + "\n", encoding="utf-8", newline="\n")
    fw = [{k: e[k] for k in ("id", "idea", "title", "setup", "needs", "rules")} for e in cat if e["status"] != "teach"]
    FW_CATALOG.write_text(json.dumps(fw, separators=(",", ":")), encoding="utf-8", newline="\n")
    n = {s: sum(e["status"] == s for e in cat) for s in ("ready", "radio", "teach")}
    log(f"{n['ready']} ready, {n['radio']} radio, {n['teach']} teach -> {CATALOG.name}; "
        f"{len(fw)} in the firmware catalog ({FW_CATALOG.stat().st_size / 1024:.1f} KB)")
    return cat


def load() -> list[dict]:
    if not CATALOG.exists():
        return build(log=lambda *_: None)
    return json.loads(CATALOG.read_text(encoding="utf-8"))


def find(rid: str) -> dict:
    for e in load():
        if e["id"] == rid:
            return e
    raise SystemExit(f"there is no idea {rid!r}. To see the ideas, run  decidio builds")


if __name__ == "__main__":
    build()
