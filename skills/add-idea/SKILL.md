---
name: add-idea
description: Write a new decidio idea (a recipe) from questions that the base models answer and from inputs such as the fly eye, the radio score and add-on sensors. The idea then installs with `decidio install` and shows on the Ideas tab of the board. Use this skill when the user tells you what the board must find and no ready-made idea agrees.
---

# Add an idea to decidio

An idea is a small set of rules with a name, a title and a setup text. If the base models already answer its
questions, it is not necessary to train a model for the idea. The ideas are in `recipes/*.yaml`.

In this file, `PY` is the Python program of the decidio environment (refer to INSTALL_WITH_LLM.md, step 2).

## Rules

1. Use only questions, options and inputs that exist (step 1). If the idea must have a new question, it is a "teach"
   idea. Tell the user, and refer the user to GUIDE.md, section 2.
2. Do not set a pin in a recipe (`gpio` stays -1). The user selects a pin on the page of the board.
3. If the user did not ask for photos, keep photos off. `photo: true` sends a camera picture to Telegram.
4. Do not write that an idea is an alarm or a safety device. In the setup text, write what can start the rule by
   mistake.
5. Thresholds are start values. Tell this to the user.

## Steps

### 1. Find what you can test

| Input | Where to find it |
|---|---|
| Questions and options | `PY -m decidio bases` |
| The fly eye (movement in the camera picture) | `eye_moving`, `eye_figures`, `eye_approach`, `eye_new`, `eye_changed`, `eye_self_motion`. The file `recipes/eye.yaml` tells you what each one is |
| The radio | `radio_motion`: how much the Wi-Fi signal changes when a person moves |
| Other inputs of the board | The console command `inputs`, or the Live tab |
| Ideas that exist | `PY -m decidio builds`. Find an idea that is near to the new idea, and copy its form |

### 2. Write the recipe

Add an entry to the file in `recipes/` that agrees with the subject (`home.yaml`, `carried.yaml`, `projects.yaml`,
`eye.yaml`).

```yaml
- id: cat-on-the-counter          # letters, digits and dashes, 31 characters at most
  idea: 112                       # the next free number in USE_CASES.md
  title: A cat on the kitchen counter
  setup: Point the camera at the counter. A person at the counter does not fire it. A dog or a toy animal can.
  rules:
    - {name: cat_on_counter, when: [animal_type = cat @ 0.6, person_present != true @ 0.7], for_ms: 1000,
       hold_ms: 20000, notify: {text: "The cat is on the counter", cooldown_s: 300}}
```

The fields of a rule are in the comment at the top of `decidio/boardrules.py`. A condition is one of these:

- `question = option` or `question != option`. Add `@ 0.6` to give the condition its own threshold.
- `input > number` or `input < number`, for the eye, the radio and sensors. For a sensor that you added, write
  `in:<name>`.

One rule has a maximum of three conditions. One idea has a maximum of eight rules. A recipe with a `radio_motion`
condition must also have the line `status: radio` (refer to the radio recipes in `recipes/projects.yaml`).

### 3. Add the row to USE_CASES.md

Add a row to the table with the same number, the idea, what it uses (`base: ...`, `eye: ...` or `radio: ...`) and
the id in backticks. A test compares the table with the recipes.

### 4. Do a check of the recipes and build the catalog

```
PY -m decidio.recipes
```

This command uses `pyyaml`. If a question, an option or a field is incorrect, it stops and shows the cause. It
writes `decidio/recipes.json` and `firmware/main/recipes.json`.

### 5. Install the idea

```
PY -m decidio install <id> --port <port> --keep-firmware
```

1. Open the Ideas tab. The rules must be there. The dot of a rule comes on when its condition is true.
2. Cause the event that the idea must find. Make sure that the rule comes on.
3. Adjust the threshold on the page.
4. Put the value that operates correctly into the recipe.

The Ideas tab of a board lists only the ideas that were in the firmware when you built it. To show the new idea
there, build the firmware again (`skills/add-sensor/SKILL.md`, step 6).

## What to tell the user at the end

Tell the user the id, what starts the rule, and what can start it by mistake. Also tell the user if you saw the rule
operate on the board, or if you only made sure that the idea packages correctly.
