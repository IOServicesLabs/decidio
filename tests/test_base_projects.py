"""Base projects, auxiliary questions, label rules and the starter lists (no training, no downloads)."""
import textwrap
from pathlib import Path

import pytest

from decidio import rules as R
from decidio import starter
from decidio.config import ConfigError, load

BASE = """
name: b
board: s3cam
inputs: {image: {size: 96, color: rgb, motion: true}}
model: {preset: small, backbone: tiny, emb: 32}
questions:
  big: {type: choice, modality: image, aux: true, criteria: {none: n, a: a, b: b, c: c, d: d, e: e, f: f, g: g, h: h, i: i,
        j: j, k: k, l: l, m: m, n2: n, o: o, p: p, q: q}}
  person_present: {type: noul, modality: image}
  kind: {type: choice, modality: image, criteria: {none: nothing, cat: a cat, dog: a dog}}
"""


def project(tmp_path: Path, name: str, text: str) -> Path:
    d = tmp_path / name
    d.mkdir()
    (d / "decidio.yaml").write_text(textwrap.dedent(text), encoding="utf-8")
    return d


def test_aux_questions_come_last_and_are_not_flashed(tmp_path):
    p = load(project(tmp_path, "b", BASE))
    assert [q.qid for q in p.questions] == ["person_present", "kind", "big"]
    assert [q.qid for q in p.deployed] == ["person_present", "kind"]
    assert p.n_deploy == 5 and p.n_logits == 5 + 18
    assert p.qmap()["big"].offset == 5


def test_too_many_options_need_aux(tmp_path):
    with pytest.raises(ConfigError, match="aux"):
        load(project(tmp_path, "b", BASE.replace("aux: true, ", "")))


def test_project_on_a_base_takes_inputs_model_and_kept_questions(tmp_path):
    project(tmp_path, "b", BASE)
    child = project(tmp_path, "c", """
        name: c
        base: ../b
        keep: [person_present]
        questions:
          mine: {type: noul, modality: image}
        actions:
          - {name: seen, when: mine = true}
    """)
    p = load(child)
    assert p.base == (tmp_path / "b").resolve()
    assert p.image.color == "rgb" and p.model["emb"] == 32 and p.board == "s3cam"
    assert [q.qid for q in p.questions] == ["person_present", "mine"]


def test_base_errors(tmp_path):
    project(tmp_path, "b", BASE)
    with pytest.raises(ConfigError, match="keep"):
        load(project(tmp_path, "c1", "base: ../b\nkeep: [big]\nquestions: {mine: {type: noul}}\n"))
    with pytest.raises(ConfigError, match="comes from the base"):
        load(project(tmp_path, "c2", "base: ../b\ninputs: {image: {size: 64}}\nquestions: {mine: {type: noul}}\n"))
    with pytest.raises(ConfigError, match="not a project folder"):
        load(project(tmp_path, "c3", "base: nowhere\nquestions: {mine: {type: noul}}\n"))
    with pytest.raises(ConfigError, match="needs base"):
        load(project(tmp_path, "c4", BASE + "keep: [kind]\n"))


def test_rules_same_and_all_false(tmp_path):
    p = load(project(tmp_path, "b", BASE))
    r = R.check(p, {"kind": "same", "person_present": {"false": "*"}})
    assert r["kind"] == {"none": [], "cat": ["cat"], "dog": ["dog"]}
    assert R.for_objects(p, r, [R.Obj("dog", 0.3, True), R.Obj("cat", 0.1, True)]) == {"kind": "dog", "person_present": "false"}
    assert R.for_objects(p, r, []) == {"kind": "none", "person_present": "false"}
    assert R.for_class(p, r, "anything")["person_present"] == "false"


def test_starter_lists():
    coco, esc = starter.lists()
    assert len(coco["images"]) == 1000 and len({im["file_name"] for im in coco["images"]}) == 1000
    assert all(im["coco_url"].startswith("http://images.cocodataset.org/val2017/") for im in coco["images"])
    ids = {im["id"] for im in coco["images"]}
    assert all(a["image_id"] in ids for a in coco["annotations"])
    assert len({c["name"] for c in coco["categories"]}) == 80
    assert len(esc) == 400 and len({r["category"] for r in esc}) == 50 and {r["fold"] for r in esc} == {"1"}


def test_caption_rules_need_two_captions_to_agree(tmp_path):
    p = load(project(tmp_path, "b", BASE + """
  indoors: {type: noul, modality: image}
  place: {type: choice, modality: image, criteria: {other: o, kitchen: k, water: w}}
"""))
    rules = R.check(p, {"indoors": {"true": ["kitchen", "living room"], "false": ["beach", "street"]},
                        "place": {"other": "*", "kitchen": ["kitchen"], "water": ["beach", "ocean"]}})
    kitchen = ["A kitchen with a stove.", "A clean kitchen", "Some cabinets.", "A fridge.", "Counter tops."]
    assert R.for_captions(p, rules, kitchen) == {"indoors": "true", "place": "kitchen"}
    beach = ["A man on a beach.", "Surfer at the beach!", "The ocean.", "A person.", "Waves."]
    assert R.for_captions(p, rules, beach) == {"indoors": "false", "place": "water"}
    dog = ["A dog.", "A brown dog.", "Dog close up.", "A puppy.", "A dog looking up."]
    assert R.for_captions(p, rules, dog) == {"indoors": None, "place": "other"}      # no word at all: unknown / the rest
    one = ["A bus near a kitchen.", "A bus.", "A bus.", "A bus.", "A bus."]
    assert R.for_captions(p, rules, one) == {"indoors": None, "place": None}         # one caption is not enough


def test_spoken_words_answer_by_word_or_as_speech(tmp_path):
    from decidio.importers import _speech_labels
    p = load(project(tmp_path, "b", """
        name: b
        board: xiao_sense
        inputs: {audio: {sample_rate: 16000, clip_seconds: 1.0}}
        questions:
          speech_present: {type: noul, modality: audio}
          word: {type: choice, modality: audio, criteria: {none: none, "yes": "yes", "no": "no"}}
    """))
    rules = R.check(p, {"speech_present": {"true": ["speech"], "false": ["noise"]}, "word": "same"})
    assert _speech_labels(p, rules, "yes") == {"speech_present": "true", "word": "yes"}
    assert _speech_labels(p, rules, "marvin") == {"speech_present": "true", "word": None}
    assert _speech_labels(p, rules, "noise") == {"speech_present": "false", "word": None}


def test_flashed_model_bounds_logits_and_joins_at_most_ten_heads(tmp_path):
    """What a real board showed: TFLite-Micro refuses a CONCATENATION of more than 10 tensors, and one very sure head
    stretches the int8 output scale of every question."""
    np = pytest.importorskip("numpy")
    pytest.importorskip("tensorflow")
    from decidio import model as M

    qs = "\n".join(f"  q{i:02d}: {{type: noul, modality: image}}" for i in range(14))
    p = load(project(tmp_path, "many", BASE.split("questions:")[0] + "questions:\n" + qs + "\n"))
    full = M.build(p)
    w, b = full.get_layer("q03_logits").get_weights()
    full.get_layer("q03_logits").set_weights([w, b + np.array([90.0, -90.0], np.float32)])
    flashed = M.deployed(p, full)
    joins = [l for l in flashed.layers if l.__class__.__name__ == "Concatenate"]
    assert joins and max(len(l.input) for l in joins) <= 10
    x = np.random.default_rng(0).random((2, 96, 96, p.image.channels), np.float32)
    raw, out = full.predict(x, verbose=0), flashed.predict(x, verbose=0)
    assert out.shape == (2, p.n_deploy)
    assert np.abs(out).max() <= M.LOGIT_LIMIT + 1e-4 and np.abs(raw).max() > 50
    inside = np.abs(raw) < M.LOGIT_LIMIT          # the order and the values of every other logit are untouched
    assert np.allclose(out[inside], raw[inside], atol=1e-4)
