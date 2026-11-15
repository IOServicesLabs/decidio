"""decidio.yaml: the project definition (inputs, questions, model, training, board, actions, sources)."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import yaml

CAM = 96                 # every stored frame is 96x96 (what the board's camera task produces)
CONFIG_NAME = "decidio.yaml"
BASES = Path(__file__).parent / "base"      # trained base models shipped with the package (base: <name>)
# what the runtime firmware can hold (firmware/main/rt.h); auxiliary questions are not flashed and do not count
MAX_QUESTIONS, MAX_OPTIONS, MAX_LOGITS, MAX_ACTIONS, MAX_ID = 16, 16, 128, 16, 31
NOUL_ALIASES = {"yes": "true", "no": "false", "1": "true", "0": "false", "y": "true", "n": "false"}

PRESETS = {
    "tiny":  {"width": 0.5, "emb": 32, "hidden": 16},
    "small": {"width": 1.0, "emb": 64, "hidden": 32},
    "base":  {"width": 1.5, "emb": 96, "hidden": 48},
}

BOARDS = {
    "xiao_sense": {"mic": True,  "flash_mb": 8,  "console": "usb"},
    "s3cam":      {"mic": False, "flash_mb": 16, "console": "uart"},
}


class ConfigError(ValueError):
    pass


@dataclass
class Question:
    qid: str
    qtype: str               # noul | choice | score
    options: list[str]       # option ids in logit order
    offset: int              # first index in the concatenated logits
    instructions: str
    criteria: object
    modality: str | None     # image | audio | None (both / unknown)
    aux: bool = False        # trained (it shapes what the model's features hold) but left out of the flashed model

    @property
    def n(self) -> int:
        return len(self.options)

    def option_text(self) -> list[str]:
        """Human description per option (zero-shot teachers, label UI)."""
        if self.qtype == "noul":
            c = self.criteria or {}
            return [str(c.get("false", "")), str(c.get("true", ""))]
        if self.qtype == "choice":
            return [str(self.criteria[o]) for o in self.options]
        return [str(c) for c in self.criteria]


@dataclass
class ImageCfg:
    size: int = 96
    color: str = "gray"      # gray | rgb
    motion: bool = True
    ego: bool = False        # motion: ego -> the camera's own shift is taken out of the motion channel (decidio/ego.py)
    platform: str = "still"  # still | moving (drone, car, hand-held): train and validate with simulated camera motion
    motion_ms: int = 200
    frame_ms: int = 50
    flip: bool = True        # horizontal-flip augmentation
    crop: bool = True        # random crop / zoom augmentation (off when where-in-the-picture is the answer)

    @property
    def colors(self) -> int:
        return 3 if self.color == "rgb" else 1

    @property
    def channels(self) -> int:
        return self.colors + (1 if self.motion else 0)

    @property
    def motion_lag(self) -> int:
        return self.motion_ms // self.frame_ms


@dataclass
class AudioCfg:
    sample_rate: int = 16000
    clip_seconds: float = 1.0
    win: int = 480
    hop: int = 320
    n_fft: int = 512
    n_mels: int = 40
    fmin: float = 20.0
    fmax: float = 7600.0
    gain_db: float = 2.0     # augmentation range; keep small if any question depends on loudness

    @property
    def clip(self) -> int:
        return int(round(self.clip_seconds * self.sample_rate))

    @property
    def n_frames(self) -> int:
        return 1 + (self.clip - self.win) // self.hop


@dataclass
class TrainCfg:
    epochs: int = 60
    batch: int = 64
    lr: float = 3e-3
    aug: bool = True
    val: float = 0.15
    balance: bool = True     # weight rare options up
    patience: int = 0        # >0: stop early when val loss stalls this many epochs (off: cosine schedule, final weights)
    seed: int = 0
    max_per_epoch: int = 0   # cap samples per epoch (0 = all); big imports train faster per epoch
    workers: int = -1        # batches are built in this many parallel workers (-1 = pick by machine, 0 = none)


@dataclass
class Project:
    root: Path
    name: str
    raw: dict
    image: ImageCfg | None
    audio: AudioCfg | None
    questions: list[Question]
    model: dict
    train: TrainCfg
    board: str
    teacher: dict
    actions: list[dict]
    sources: dict
    n_logits: int = 0
    paths: dict = field(default_factory=dict)
    base: Path | None = None                       # base: the trained project this one builds on (decidio teach)
    keep: list = field(default_factory=list)       # questions taken over from the base, answers and all
    inputs_raw: dict = field(default_factory=dict)
    model_raw: dict = field(default_factory=dict)
    questions_raw: dict = field(default_factory=dict)

    # ---------------------------------------------------------------- what gets flashed
    @property
    def deployed(self) -> list[Question]:
        """The questions in the flashed model (auxiliary ones come last in `questions` and are cut off)."""
        return [q for q in self.questions if not q.aux]

    @property
    def n_deploy(self) -> int:
        return sum(q.n for q in self.deployed)

    # ---------------------------------------------------------------- paths
    @property
    def data(self) -> Path:
        return self.root / "data"

    @property
    def samples(self) -> Path:
        return self.data / "samples"

    @property
    def out(self) -> Path:
        return self.root / "out"

    @property
    def builds(self) -> Path:
        return self.root / "builds"

    def qmap(self) -> dict[str, Question]:
        return {q.qid: q for q in self.questions}

    def option_index(self, q: Question, value) -> int:
        v = str(value).strip().lower()
        if q.qtype == "noul":
            v = NOUL_ALIASES.get(v, v)
        if v in q.options:
            return q.options.index(v)
        if q.qtype == "score" and q.criteria:
            names = [str(c).strip().lower() for c in q.criteria]
            if v in names:
                return names.index(v)
        raise ConfigError(f"{q.qid}: {value!r} is not an option. Use one of {q.options}")

    def teacher_questions(self, modality: str | None = None) -> dict:
        out = {}
        for q in self.questions:
            if modality and q.modality not in (None, modality):
                continue
            d = {"type": q.qtype}
            if q.instructions:
                d["instructions"] = q.instructions
            if q.criteria is not None:
                d["criteria"] = q.criteria
            out[q.qid] = d
        return out


def _image_cfg(d) -> ImageCfg | None:
    if d in (None, False):
        return None
    d = {} if d is True else dict(d)
    if str(d.get("motion")).lower() == "ego":
        d["motion"], d["ego"] = True, True
    elif "motion" in d and not isinstance(d["motion"], bool):
        raise ConfigError("inputs.image.motion must be true, false or ego")
    c = ImageCfg(**{k: d[k] for k in d if k in ImageCfg.__dataclass_fields__ and k != "ego"})
    c.ego = bool(d.get("ego", False))
    if c.platform not in ("still", "moving"):
        raise ConfigError("inputs.image.platform must be still or moving")
    if not 16 <= c.size <= CAM:
        raise ConfigError(f"inputs.image.size must be from 16 to {CAM}")
    if c.color not in ("gray", "rgb"):
        raise ConfigError("inputs.image.color must be gray or rgb")
    if c.motion and (c.motion_ms % c.frame_ms or c.motion_ms <= 0 or c.motion_lag > 20):
        raise ConfigError("inputs.image.motion_ms must be a multiple of frame_ms, more than 0 and not more than 20 frames")
    return c


def _audio_cfg(d) -> AudioCfg | None:
    if d in (None, False):
        return None
    d = {} if d is True else dict(d)
    c = AudioCfg(**{k: d[k] for k in d if k in AudioCfg.__dataclass_fields__})
    if c.n_fft & (c.n_fft - 1) or c.n_fft < c.win or c.hop > c.win:
        raise ConfigError("inputs.audio: n_fft must be a power of two and n_fft >= win. Also, hop must be <= win")
    if c.fmax > c.sample_rate / 2:
        raise ConfigError("inputs.audio.fmax must be <= sample_rate / 2")
    if c.n_frames < 4:
        raise ConfigError("inputs.audio.clip_seconds is too small. Use a larger value")
    return c


def resolve_base(root: Path, ref: str) -> Path:
    """base: a project folder (relative to this project, or absolute) or the name of a shipped base model."""
    for cand in (root / ref, Path(ref), BASES / ref):
        if (cand / CONFIG_NAME).is_file():
            return cand.resolve()
    have = ", ".join(sorted(d.name for d in BASES.iterdir() if (d / CONFIG_NAME).is_file())) if BASES.is_dir() else ""
    raise ConfigError(f"base {ref!r} is not a project folder and not a shipped base model. "
                      f"Shipped base models: {have or 'none'}")


def load(root: Path | str = ".") -> Project:
    root = Path(root).resolve()
    path = root / CONFIG_NAME if root.is_dir() else root
    if path.is_file() and path.name != CONFIG_NAME and path.suffix in (".yaml", ".yml"):
        root = path.parent
    if not path.is_file():
        raise ConfigError(f"there is no {CONFIG_NAME} in {root}. To make a project, run  decidio new <name>")
    raw = yaml.safe_load(path.read_text(encoding="utf-8")) or {}

    # base: the camera / microphone setup and the model's body come from the base; this project adds questions
    base, inputs, m, q_raw, keep = None, raw.get("inputs") or {}, dict(raw.get("model") or {}), {}, []
    if raw.get("base"):
        base = load(resolve_base(root, str(raw["base"])))
        for key, own, theirs in (("inputs", inputs, base.inputs_raw), ("model", m, base.model_raw)):
            if own and own != theirs:
                raise ConfigError(f"{key}: comes from the base ({base.name}). Remove {key}: from this decidio.yaml")
        inputs, m = base.inputs_raw, dict(base.model_raw)
        keep = raw.get("keep") or []
        if keep == "all":
            keep = [q.qid for q in base.deployed]
        for qid in keep:
            if qid not in base.questions_raw or base.qmap()[qid].aux:
                raise ConfigError(f"keep: the base ({base.name}) has no question {qid!r}. "
                                  f"Its questions are: {', '.join(q.qid for q in base.deployed)}")
            q_raw[qid] = base.questions_raw[qid]
    elif raw.get("keep"):
        raise ConfigError("keep: needs base: in decidio.yaml. Add base: or remove keep:")
    for qid, q in (raw.get("questions") or {}).items():
        if qid in q_raw:
            raise ConfigError(f"{qid}: is in keep: and in questions: (remove it from one of the two)")
        q_raw[qid] = q

    image, audio = _image_cfg(inputs.get("image")), _audio_cfg(inputs.get("audio"))
    if not image and not audio:
        raise ConfigError("inputs: no input is set. Set image, audio or both")

    questions = []
    for qid, q in q_raw.items():
        if not str(qid).isidentifier():
            raise ConfigError(f"question id {qid!r}: use only letters, digits and _")
        t = q.get("type")
        if t == "noul":
            opts = ["false", "true"]
        elif t == "choice":
            opts = [str(k).lower() for k in (q.get("criteria") or {})]
        elif t == "score":
            opts = [str(i) for i in range(len(q.get("criteria") or []))]
        else:
            raise ConfigError(f"{qid}: type must be noul, choice or score")
        if len(opts) < 2 or len(set(opts)) != len(opts):
            raise ConfigError(f"{qid}: needs 2 or more different options")
        crit = q.get("criteria")
        if t == "choice":
            crit = {str(k).lower(): v for k, v in crit.items()}
        mod = q.get("modality")
        if mod not in (None, "image", "audio"):
            raise ConfigError(f"{qid}: modality must be image or audio")
        if (mod == "image" and not image) or (mod == "audio" and not audio):
            raise ConfigError(f"{qid}: modality {mod} is not an input of this project. Add inputs.{mod} or change the modality")
        aux = bool(q.get("aux", False))
        if not aux:
            if len(str(qid)) > MAX_ID or any(len(o) > MAX_ID for o in opts):
                raise ConfigError(f"{qid}: question and option ids can be {MAX_ID} characters at most")
            if len(opts) > MAX_OPTIONS:
                raise ConfigError(f"{qid}: has {len(opts)} options, but the board holds a maximum of {MAX_OPTIONS} for each "
                                  f"question. To use the question for training only, set aux: true")
        questions.append(Question(qid, t, opts, 0, q.get("instructions", ""), crit, mod, aux))
    if not questions:
        raise ConfigError("questions: there are no questions. Add one or more questions")
    questions.sort(key=lambda q: q.aux)                    # stable: the flashed questions first, auxiliary ones after
    off = 0
    for q in questions:
        q.offset = off
        off += q.n
    live = [q for q in questions if not q.aux]
    if not live:
        raise ConfigError("questions: all questions have aux: true. Remove aux: true from one or more questions")
    if len(live) > MAX_QUESTIONS or sum(q.n for q in live) > MAX_LOGITS:
        raise ConfigError(f"the board holds a maximum of {MAX_QUESTIONS} questions and {MAX_LOGITS} options in total. "
                          f"This project has {len(live)} questions and {sum(q.n for q in live)} options. "
                          f"To use a question for training only, set aux: true")

    preset = dict(PRESETS.get(m.get("preset", "small"), PRESETS["small"]))
    preset.update({k: m[k] for k in ("width", "emb", "hidden") if k in m})
    preset["backbone"] = m.get("backbone", "tiny")
    if preset["backbone"] not in ("tiny", "mobilenet_v1_0.25", "mobilenet_v1_0.5"):
        raise ConfigError("model.backbone must be tiny, mobilenet_v1_0.25 or mobilenet_v1_0.5")
    if preset["backbone"] != "tiny" and image and image.size < 32:
        raise ConfigError("a mobilenet backbone needs inputs.image.size >= 32")
    tr = raw.get("train") or {}
    board = raw.get("board", base.board if base else "xiao_sense")
    if board not in BOARDS:
        raise ConfigError(f"board must be one of {list(BOARDS)}")
    if audio and not BOARDS[board]["mic"]:
        raise ConfigError(f"board {board} has no microphone. " + (
            f"The base {base.name} also uses sound. Use a base that uses only the camera" if base
            else "Remove inputs.audio and the audio questions"))
    if len(raw.get("actions") or []) > MAX_ACTIONS:
        raise ConfigError(f"actions: the board holds a maximum of {MAX_ACTIONS} rules")

    p = Project(
        root=root, name=raw.get("name", root.name), raw=raw, image=image, audio=audio, questions=questions,
        model=preset, train=TrainCfg(**{k: tr[k] for k in tr if k in TrainCfg.__dataclass_fields__}),
        board=board, teacher=raw.get("teacher") or {}, actions=raw.get("actions") or [],
        sources=raw.get("sources") or {}, n_logits=off,
        base=base.root if base else None, keep=list(keep), inputs_raw=inputs, model_raw=m, questions_raw=q_raw,
    )
    _check_actions(p)
    return p


def board_questions(p: Project) -> list[dict]:
    """The flashed questions the way the model package lists them."""
    return [{"id": q.qid, "type": q.qtype, "options": q.options} for q in p.deployed]


def _check_actions(p: Project) -> None:
    from . import boardrules
    aux = {q.qid for q in p.questions if q.aux}
    qs = board_questions(p)
    for i, a in enumerate(p.actions):
        try:
            boardrules.wire(a, qs)
        except boardrules.RuleError as e:
            hit = [q for q in aux if f"{q!r}" in str(e)]
            raise ConfigError(f"actions[{i}]: {hit[0]} has aux: true, so it is not on the board" if hit else f"actions[{i}]: {e}") from None
