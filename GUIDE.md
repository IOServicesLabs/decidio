# decidio guide

This guide is the reference for the models, for how to teach a new question, for how to train a model, and for the
firmware. To put a ready-made idea on a board, start with [README.md](README.md) (quick install) or
[INSTALL_WITH_LLM.md](INSTALL_WITH_LLM.md) (an LLM agent does the install).

decidio makes small typed-decision models for the ESP32-S3. You write the questions. An int8 model on the chip
answers them from the **camera, the microphone or both**, many times each second. The board then acts on the answers.
It can set GPIO pins, write event lines to USB, or send an alert over Wi-Fi to Telegram, a webhook or MQTT. The model
does not generate text: each question has a fixed set of options, and the model gives a probability for each option.

```
                         ┌──────────── base model (shipped, trained on 80 object classes + 50 sounds) ───────────┐
your question + a few    │  camera ─▶ vision body ─┐                                                              │
dozen examples ─────────▶│                          ├─▶ feature vector ─▶ one small head per question ─▶ answers │─▶ rules ─▶ GPIO / events
   decidio teach (~1 min)│  mic ────▶ sound body ──┘        (frozen)          (this is all that is trained)       │
                         └────────────────────────────────────────────────────────────────────────────────────────┘
```

| Question type | Answer | Example |
|---|---|---|
| `noul` | true or false | `person_present: {p_true: 0.94}` |
| `choice` | one of the named options | `subject: {choice: animal, confidence: 0.81}` |
| `score` | ordered options; the answer is the expected value | `noise_level: {score: 1.2}` |

There are four ways to use decidio. The first way is the quickest, and the last way needs the most work:

0. **Install a ready-made idea.** There are 63 ideas, and each idea is a base model plus rules. You set up an idea
   from the web page of the board. 57 ideas use the answers of the base model or the Wi-Fi signal. Examples are a
   knock at the door, a person in the room, rain, a bird at the feeder and movement from the Wi-Fi signal. The
   other 6 ideas use the fly eye.
1. **Flash a base model.** Use the questions that it already answers, with your own rules. These questions are about
   people, animals, vehicles, indoors or outdoors, the type of place, the weather, speech and sounds in the home.
2. **Teach your own question** on a base model. `decidio teach` trains only a small head. On a laptop, this takes
   approximately one minute.
3. **Train a new model**, or fine-tune all of a base model. Do this when your camera view or your sounds are not
   similar to everyday photos and sounds.

[USE_CASES.md](USE_CASES.md) lists 111 ideas to build: for the home, for devices that you carry, and for ESP32
projects in general. For each idea, it tells what the idea needs and if a ready-made build exists. The questions of
the base models come from that list.

## Install

You need Python 3.10, 3.11 or 3.12 on Windows, Linux or macOS. Type these commands:

```
python -m venv .venv
.venv\Scripts\activate          (Linux / macOS: source .venv/bin/activate)
pip install -e .
decidio --help
```

decidio uses esptool to flash the board, and esptool installs with decidio. You need ESP-IDF only if you change the
C code of the firmware.

## 0. Install a ready-made idea

**WARNING:** Do not use decidio as a safety device or as an alarm. Each answer has an error rate (refer to the tables
in "The shipped base models"). A rule also operates on the errors of the model. The glass-break, thud and movement
ideas are conveniences, not alarms.

**CAUTION:** Do not forward the port of the board to the internet. The page uses plain HTTP on your own network. Until
you set a page password (Wi-Fi tab), each person on that network can open the page, see the camera frame and change
the rules. When you set a password, the password still goes through your network without encryption.

**CAUTION:** Use a Telegram bot that you made for decidio only. The board keeps the Wi-Fi password, the bot token and
the other secrets in its flash memory without encryption. A person who takes the board can read them.

**CAUTION:** Give the group secret only to boards that you own. The board keeps the local keys of devices and the
group secret in its flash memory without encryption. A board in the group can change each other board in the group,
because its token replaces the page password. When you add a board from the page of a different board, the secret
goes through the network without encryption.

**CAUTION:** Set up the board immediately after it starts its setup network. The setup network has no password. It
stays open until the board joins your Wi-Fi, and for two minutes after that. During that time, a person near the
board can set it up instead of you.

A board accepts rule states only from broadcasts that carry the signature of the group secret. It rejects a copy of
an old broadcast during the same run of the board that sent it (until that board restarts).

To install an idea, type the commands below in this sequence. Replace `COM5` with the port of your board. Replace
`MyNetwork` with the name of your Wi-Fi network. The comment on each line tells what the command does.

```
decidio ports                               # the USB serial ports, and the board each probably is
decidio builds                              # the ideas, and the boards each runs on
decidio install knock --port COM5           # firmware + the base model + that idea's rules
decidio wifi MyNetwork --port COM5          # asks for the password; prints the board's address
```

If you do not have Python, use `installer/index.html`. It flashes the same builds from Chrome or Edge over USB.
[installer/README.md](installer/README.md) tells how to publish it. The published installer is at
<https://ioserviceslabs.github.io/decidio/>.

To open the page of the board, do one of these steps:

- If you used `decidio wifi`, open the address that the board gave (`http://decidio-xxxx.local`).
- If you did not use `decidio wifi` and the board has no network, the board opens its own network,
  **decidio-xxxx**. This network has no password. Connect a phone to it. The setup page then opens.

| Tab | What it does |
|---|---|
| Status | Shows the board, the model, the free memory, the Wi-Fi signal, and the alerts that the board sent and that failed |
| Live | Shows each answer when the model gives it, the other inputs and the states of the rules. Shows the camera frame to aim the board, only while you select **show what it sees** |
| Eye | Shows what moves in the camera frame, a close-up, the sharp close-up and the events of the fly eye. Sets the eye on or off |
| Ideas | Shows the rules that operate now, with their thresholds, times, pin, device and alert. Adds any of the 63 ideas, or a rule of your own |
| Devices | Shows the Smart Life (Tuya) devices that this board switches. Adds a device (id and local key), tests it, and makes a light follow people |
| Boards | Sets the room and the group of this board. Shows the other boards on the network and their rules, and adds them to the group |
| Alerts | Sets a Telegram bot, a webhook and MQTT. The webhook sends JSON or plain text, with one optional header, for example to ntfy, Discord, Slack or Home Assistant. Each one has a test button |
| Radio | Shows the live movement score from the signal of your router, to help you select a threshold |
| Wi-Fi | Changes the network. Sets a password for the page |
| System | Sets the time of the daily summary. Uploads a model package or new firmware. Shows the sensors that the board found (**Detect again**). Sets input pins and does an I2C scan. Restarts the board. Erases the settings (**Erase all settings**) |

The board saves the rules that you change on the page, and a change applies immediately. You do not flash the board
again. More than one idea can run at the same time, with a maximum of 16 rules.
[USE_CASES.md](USE_CASES.md#rules-for-base-answers) tells what a rule can contain. The rules of each idea are in
`recipes/*.yaml`.

**Devices and more than one board** (firmware 0.3). [README.md](README.md#switch-a-smart-life-device) gives the
procedure. This is a summary:

| Part | What it does |
|---|---|
| `devices.c` | Sends commands to the device on its own port 6668, with the Tuya local protocol (versions 3.1, 3.3, 3.4 and 3.5). It uses one connection for each command, and the local key of the device. It gets the addresses and the protocol versions from the broadcasts of the devices |
| `home.c` | Broadcasts the name and the room of each board every 2 s, and immediately after a change. Inside a group, it also broadcasts the states of the rules, signed with the group secret |
| `tools/fake_tuya_device.py` | A device to test against, without a key |

Thus a rule can test `board:<name>` conditions, and one page can set up each board through `/api/via/<board>/...`.
A rule switches a device with `"device":"name","device_do":"follow|inverse|on|off"`. You set these two fields and the
`board:` conditions on the page. `decidio.yaml` and the recipes do not contain them, because devices and boards are
specific to one home.

**Alerts.** The alert of a rule is its text and one line of detail (`The washer finished` /
`washer on, 93% - decidio-f154`). To set up Telegram, do these steps:

1. Make a bot with @BotFather.
2. Send a message to the bot.
3. On the Alerts tab, type the token of the bot and your chat id.

| Alert item | Value |
|---|---|
| MQTT topic for events | `decidio/<name>/event/<rule>` |
| MQTT topic for the status | `decidio/<name>/status`, kept at `online` or `offline` |
| Alerts that leave the board | A maximum of 20 each minute |
| Tries for each alert | 3 |
| Shortest time between two alerts of one rule | `cooldown_s`, set for each rule |
| Rules with `count: daily` | Send one summary each day instead of each alert |

**What leaves the board.** By default, only the alert text leaves the board, and it goes only to the destinations
that you entered. The other data that leaves the board is in this table:

| Data | Who or what receives it |
|---|---|
| The picture of the eye (40x30) and a 64x64 colour close-up of what moves | Each person who has the Eye tab open. The board sends these pictures only to the page |
| With an OV3660 camera: a sharp 160x120 close-up. The sensor zooms onto the figure, sufficiently to recognise a person at some distance | Each person who has the Eye tab open. The board sends it only to the page |
| A camera frame (192x192) | Telegram only, and only for a rule where you selected **with a photo**. One build, `trail-camera`, has **with a photo** selected from the start |
| The camera frame on the Live tab | Each person who has the page open |
| Sound | Nobody. The board never sends sound |

**Tests on a XIAO ESP32S3 Sense.** These items were tested:

| Test | Result |
|---|---|
| Setup from a phone | Done |
| The page and its tabs | Done |
| Rules that you change on the page, kept after a restart | Done |
| Webhook and MQTT alerts from a real rule to a listener on a PC | Done |
| A model update, uploaded from the page | Done, 6 s |
| A firmware update, uploaded from the page | Done, 25 s |
| `decidio install` and `decidio wifi` | Done |
| Time for one decision, with Wi-Fi, the page open and the radio score on | 102-115 ms (100 ms without Wi-Fi) |
| Free internal memory | 31-41 KB, with a lowest point of 8 KB. This margin is small. A test of several days was not done |
| Free internal memory, firmware 0.3 with devices and the board group | 28 KB, with the same lowest point |

**Not tested yet:**

- A real Telegram message and a photo alert. Only the encrypted connection to the server of Telegram was tested.
- An install through the browser installer.
- Wi-Fi and the page on an ESP32-S3-CAM. Firmware 0.4 runs on this board over USB: the detection, the model and the
  eye.
- The power that Wi-Fi uses.

Of the 63 ideas, only a few were started by a real event. A test makes sure that all ideas load, and that each idea
names only questions that its model answers.

## 1. Flash a base model

Flash the runtime firmware one time. On the XIAO, connect the USB-C socket. On the S3-CAM, connect the socket marked
**UART**. If the board has firmware 0.1, flash the firmware and then flash the model again. The flash layout is
different from 0.1: it has two firmware slots, and the model is at `0x510000`.

```
decidio flash firmware --port COM5        # one firmware for every board: it finds its camera and microphone at start
```

Then flash a model. This takes seconds, you can do it at any time, and you do not rebuild the firmware:

```
decidio bases                                                # what is shipped and what each answers
decidio flash model -C decidio/base/general --port COM5      # camera + mic (XIAO ESP32S3 Sense)
decidio flash model -C decidio/base/general-cam --port COM5  # camera only (any board)
decidio monitor --port COM5
```

If you do not have a board, run the same int8 model on the PC. It uses the same preprocessing as the board:

```
decidio try -C decidio/base/general-cam photo.jpg some_folder/
```

### The shipped base models

| Base | Board | Inputs | Flashed size | Multiply-adds | Flashed questions |
|---|---|---|---|---|---|
| `general` | XIAO ESP32S3 Sense | RGB + motion 96x96, 1 s of microphone sound | 493 KB | 8.9 M | 9 about the picture, 5 about the sound |
| `general-cam` | any | RGB + motion 96x96 | 403 KB | 7.8 M | the same 9 about the picture |

Both base models have a MobileNetV1-0.25 vision body and a feature vector of 128 values. The vision body starts from
ImageNet-pretrained weights. Its training data was 42.8k COCO train2017 photos (85k crops) and 16k weather photos.
`general` also has a small sound body, with no pretrained weights. Its training data was ESC-50 folds 2-5 (1,600
clips), 5,250 spoken words (Speech Commands) and 1,500 read sentences (LibriSpeech dev-clean).

The table below gives the results of the **int8 model on held-out data**. The training did not use any of this data.
The data is all 5,000 COCO val2017 photos, 1,781 weather photos kept apart, and ESC-50 fold 1. It also includes the
Speech Commands test list, LibriSpeech test-clean (other speakers) and new generated quiet clips.

| Term | Description |
|---|---|
| acc | The accuracy at p > 0.5 |
| AUC | How well the probabilities put the true samples above the false samples: 1.0 = perfect, 0.5 = chance |
| majority | The accuracy of an answer that is always the most common answer. It is the number in parentheses after acc |

| Question | Options | `general` | `general-cam` |
|---|---|---|---|
| `person_present` | true / false | acc 0.84 (majority 0.53), AUC 0.92 | acc 0.84, AUC 0.92 |
| `animal_present` | true / false | acc 0.89 (0.82), AUC 0.94 | acc 0.86, AUC 0.93 |
| `vehicle_present` | true / false | acc 0.86 (0.81), AUC 0.94 | acc 0.90, AUC 0.94 |
| `indoors` | true / false | acc 0.96 (0.77), AUC 0.99 | acc 0.94, AUC 0.99 |
| `subject` | 13 types of thing (the largest object) | acc 0.64 (0.32) | acc 0.63 (0.32) |
| `place` | other / kitchen / bathroom / bedroom / living_room / eating / office / street / green / nature / water | acc 0.68 (0.48) | acc 0.67 (0.48) |
| `weather` | none / clear / cloudy / rain / snow / fog | acc 0.80 (0.36) | acc 0.80 (0.36) |
| `animal_type` | none / cat / dog / bird / horse / farm / wild | acc 0.87 (0.82) | acc 0.85 (0.82) |
| `vehicle_type` | none + 8 vehicles | acc 0.83 (0.81) | acc 0.87 (0.81) |
| `speech_present` | true / false | acc 0.99 (0.69), AUC 1.00 | - |
| `sound_group` | quiet / animal / nature / human / speech / household / outside | refer to the notes below | - |
| `alert_sound` | true / false | AUC 0.97; finds 69% of alert sounds at 52% precision | - |
| `home_sound` | none / dog / baby / cough / knock / glass / alarm / water / appliance / footsteps / snoring / clap | refer to the notes below | - |
| `weather_sound` | none / rain / wind / thunder | refer to the notes below | - |

The next table gives, for `general`, how often the model finds each option (the recall):

| Question | How often the model finds each option |
|---|---|
| `place` | `street` 83%, `green` (park or garden) 82%, `water` 75%, `bathroom` 70%, `kitchen` 67%, `bedroom` 45%, `eating` (places to eat) 18% (too few examples) |
| `weather` | `fog` 85%, `snow` 81%, `rain` 81%, `clear` 73%, `cloudy` 62% |
| `animal_type` | `wild` (wild animals) 81%, `cat` 60%, `dog` 27% |
| `vehicle_type` | `airplane` 71%, `bicycle` 21% |
| `sound_group` | `quiet` 100%, `speech` 99%, `nature` 75%, `animal` 69%, `outside` 60%, `human` 58%, `household` 55% |
| `home_sound` | `knock` 88%, `dog` 82%, `baby` (a baby that cries) 81%, `water` 72%, `glass` (glass that breaks) 69%, `alarm` (alarm clock) 66%, `clap` 65%, `snoring` 57%, `appliance` 41%, `cough` 38%, `footsteps` 12% |
| `weather_sound` | `rain` 85%, `thunder` 64%, `wind` 25% |

How to read these results:

- **The yes/no questions about the picture are the strong part** (AUC 0.92-0.99). Select your own threshold. The
  reports give the best threshold for each question.
- **`indoors` and `place` learned from the COCO photo captions**, not from labels that a person made. A photo is
  "kitchen" when two of its five captions say so. A photo is "other" when no caption names a place. The test labels
  come from the captions in the same way. Thus these two numbers tell how well the model agrees with the captions.
- **`weather`** learned from a set of weather photos in 5 classes. All of these photos are outdoor views. When the
  model says "rain", it is correct 44% of the time. Thus, set a few seconds of agreement (`for_ms`) before a rule
  acts. `none` is for indoor views. The model has never seen your window or your camera.
- **`animal_type` and `vehicle_type` are weak.** They are only a little better than an answer of "none" every time.
  At 96x96, everyday photos leave few pixels for each object. Use the `*_present` questions. Or teach or train a
  question on views that are similar to the views of your camera.
- **The result of `speech_present` is almost perfect on this test, but the test makes it look better than it is.**
  The speech in the test is clean single words and read sentences. The non-speech is ESC-50 sounds and quiet. These
  were not tested: talk from across a room, a television, and two people who talk at the same time.
- **`sound_group`:** on the starter set, which has no speech, the accuracy is 67% over the six other options.
- **`home_sound`:** when the model names one of the first three sounds in the table (`knock`, `dog`, `baby`), it is
  correct 55-60% of the time. Each sound had only 32-96 clips in the training data.
- **`weather_sound`:** when the model says "rain", it is correct 44% of the time.
- The model also learned to identify 35 spoken words, as a hidden training task. It is correct 80% of the time on the
  data that was set aside during the training. Thus a taught wake word can probably work. How well `decidio teach`
  learns a new word was not measured.
- int8 decreases the accuracy a little. The flashed model gives the same answer as the float model on 93-100% of
  samples (85-92% for `subject`, `place` and `weather`).
- The new questions did not decrease the accuracy of the old questions. A model with only the earlier eight
  questions, with training on the same photos, has the same scores on them (person AUC 0.93 against 0.92).
- These accuracy figures are PC results with the preprocessing of the board. The accuracy with a real camera and
  microphone was not measured. The checks on a board are below.

On a XIAO ESP32S3 Sense (OV3660 camera, PDM microphone), with `general` as flashed:

| Check | Result |
|---|---|
| Time for one decision | 100-103 ms of model time plus 5 ms of preprocessing: 8.7-9.1 decisions each second, all 14 questions at the same time |
| Parts of the model time | Convolutions 61%, depthwise convolutions 24%, the question heads 10% |
| Tensor arena | Approximately 150 KB, in PSRAM |
| The board and the PC agree | A second of sound and a frame that the board recorded (`collect`) give the same answers in the int8 model on the PC |
| TensorFlow Lite Micro against TensorFlow Lite | The model gives the same answers on 2799 of 2800 samples (`tests/test_tflm_parity.py`) |
| Not done yet | The accuracy on labelled camera frames and sounds that the board itself recorded |

Three problems occurred only on a board. All three are now repaired:

| Problem | Repair |
|---|---|
| TensorFlow Lite Micro does not join more than 10 tensors. Thus a model with 14 questions loaded but did not run | The heads are now joined in groups |
| One very sure question stretched the int8 output scale. Then `p_true` had only a small number of possible values | The flashed logits now stay within +-12. This gives 0.09 between steps |
| The microphone has a constant offset. The model heard it as a knock in a silent room | The firmware now removes the offset. A silent room gives `quiet`, 100% |

Full reports:

| File | Contents |
|---|---|
| `decidio/base/<name>/out/test_heldout.md` | The accuracy table above, with each option |
| `decidio/base/<name>/out/test_starter.md` | The results on the starter set |
| `decidio/base/<name>/out/report.md` | The results on the part of the training data that was set aside during the training |

## 2. Teach your own question (approximately one minute)

Make a project from the `teach` template:

```
decidio new doorbell -t teach
cd doorbell
```

`decidio.yaml` contains the whole project:

```yaml
name: doorbell
base: general-cam            # the model body and the camera setup come from the base
board: s3cam
keep: [person_present]       # answers of the base you want to keep (free: no data needed)

questions:
  parcel_at_door:
    type: noul
    modality: image
    criteria: {true: "a parcel on the doorstep", false: "an empty doorstep"}

actions:                     # what the board does with the answers
  - {name: parcel, when: parcel_at_door = true, above: 0.8, for_ms: 500, gpio: 21, level: 0, hold_ms: 3000}

sources:
  mine:    {type: folders, path: photos, question: parcel_at_door}     # photos/true/*.jpg  (photos/false/*.jpg)
  starter: {type: starter, labels: {parcel_at_door: {false: "*"}}}     # 1000 everyday photos as "everything else"
```

Then type these commands in the project folder:

```
decidio fetch starter            # once per PC (see "The starter set")
decidio import                   # your photos + the starter set
decidio teach                    # features once, then the heads -> out/model.dcd, out/report.md
decidio try my_test_photos/      # the exact int8 model, on the PC
decidio flash model --port COM5
```

`decidio teach` sends each sample through the frozen body of the base model one time, and keeps the feature vectors.
Then it trains only the heads on these vectors (a few thousand weights for each question). Then it converts the
whole model to int8 again. A kept question keeps its trained head without changes. If you give labels for a kept
question, `decidio teach` trains that head more.

More examples make the model better. Examples **from your own camera** have the most effect.
`decidio collect --port COM5` records frames (and sound) from the board. `decidio label ui` lets you label them in
the browser.

Teach is not sufficient for unusual viewpoints, very small objects, or a look that the base never saw. For these,
train the whole model from the base with `decidio run` in the same project. This takes a few minutes instead of one.
It also needs labels for each kept question (refer to the notes below).

### Measured results of teach

All results are from a PC, with int8 models. The test data is data that neither the base nor the taught head saw.
The times are for the whole `decidio teach` command on 6 CPU threads at low priority, with no GPU.

**Questions about everyday things.** These questions were taught from the starter set only, with no photos of your
own. The test used 4,000 other COCO val photos (7,500 crops).

| New question | Examples of "true" in the starter set | `general` | `general-cam` | Time |
|---|---|---|---|---|
| `drink_in_view` (cup, bottle or glass) | 162 crops | AUC 0.87 | AUC 0.85 | 57-73 s |
| `screen_in_view` (tv, laptop or phone) | 121 crops | AUC 0.89 | AUC 0.90 | the same run |

**A type of thing that the base never saw, and a difficult one.** The question is ant / bee / neither (small insects
at 96x96). The training data was a number of insect examples, plus the starter set as "neither". The test used 153
other ant and bee photos. An answer that is always the more common insect gives 0.54. These results are from the
first version of the bases, before the place, weather and speech questions. Nobody measured them again after that.

| Examples for each insect | `decidio teach` (heads only, approximately 72 s) | `decidio run` (the whole model from the base, approximately 3.5 min) |
|---|---|---|
| 10 | 0.18 | not run |
| 25 | 0.33 | not run |
| 50 | 0.43 | not run |
| 120 | 0.53 | **0.76** |

Conclusions:

- **Teach works when the question is a new combination of things that the base can already see.** These things are
  objects, people, animals, vehicles, household and street things, and the 50 sounds.
- **Teach is not sufficient for a thing that looks different from all of that.** The frozen features do not hold
  it, and the taught head usually answers "neither". Train the whole model from the base instead (`decidio run` in
  the same project). This takes minutes instead of one minute, and the result is much better. A plain probe of the
  features shows the limit for heads only on this task: approximately 0.65, with any head.
- The report from `decidio teach` uses a part of the data of *your* project, and most of that data is "everything
  else". Thus the report can look good (0.9+) while the model misses new examples. **Test on separate examples of
  your thing.** Put them in a different project folder and run `decidio test --with <that folder>`.
- `decidio teach` does not change the head of a kept question. But it converts the model to int8 again, thus the
  answers change a little (`person_present` AUC 0.924 in the base, 0.923 after a teach).
- `decidio run` with `keep:` needs labels for the kept questions (the starter set has them). Without these labels, it
  does not start. When the whole body trains and nothing holds a kept answer, that answer drifts far. The measured
  drift: person AUC 0.93 → 0.83, and the sound questions fell to chance.

## The starter set

The starter set is a small mixed set of public data that all projects can share. With it, you must bring only
examples of *your* thing.

| Part | Contents | Size |
|---|---|---|
| photos | 1000 COCO val2017 photos with their boxes. Each of the 80 COCO object classes is clearly visible in them (12 or more photos for almost all classes) | 161 MB |
| sounds | Fold 1 of ESC-50: 8 clips of five seconds for each of the 50 sound classes | 176 MB |
| quiet | 60 generated clips of a quiet room (class `quiet`), so that a sound question can also say "nothing" | generated |

The starter set has labels for objects and for the 50 sounds. It has no weather photos, no place labels and no
speech. Thus a question about these needs examples of your own, or the `weather` and `speech-commands` downloads.

`decidio fetch starter` downloads the files from their own hosts (cocodataset.org and the ESC-50 GitHub repository)
into `~/.decidio/starter`. You do this one time on each PC. `DECIDIO_HOME` changes the location. This repository
contains only the two lists (`decidio/starter_data/`). After the import, the starter set gives approximately 2000
photo crops and 860 one-second clips.

Use the starter set as a source in any project. `labels:` tells what the starter set means for your questions:

```yaml
sources:
  starter:
    type: starter
    labels:
      my_cat: {false: "*"}                           # none of it shows my cat: all "false"
      drink_in_view: {true: [cup, bottle, wine glass]}   # COCO classes: true where one is clearly visible
      sound: {dog: [dog], knock: [door_wood_knock], other: "*"}   # ESC-50 classes; "*" = all the rest
```

Image questions read the photos, and audio questions read the sounds. The base models used different data for their
training (COCO train2017, ESC-50 folds 2-5). Thus the starter set is also a fair test set for them.

## 3. Train a new model

```
decidio new mymodel -t blank          # decidio templates lists the starting points
cd mymodel                            # edit decidio.yaml: inputs, questions, sources
decidio import                        # or: decidio collect / import-files / import-video
decidio run                           # prep + train + export + eval  -> out/model.dcd, out/report.md
decidio flash model --port COM5
```

To get data, use any mix of these methods:

| Method | Command |
|---|---|
| Record with the board (frames and audio, in pairs). Press Ctrl+C to stop | `decidio collect --port COM5 --every 1000` |
| Folders of photos or sounds (`<folder>/<option>/<files>`) | `decidio import-files photos --question what` |
| A video file of any type (frames and soundtrack, in pairs) | `decidio import-video clips/*.mp4 --every 0.5` |
| The starter set | `decidio fetch starter`, then a `type: starter` source |
| Public datasets | `decidio fetch voc2007 voc2012 coco128 coco-val2017 coco-train2017 esc50 urbansound8k`, then a `sources:` entry |
| Your own labelled detection sets or sound sets | `sources:` with `type: voc / coco / yolo / esc50 / folders / video` |

To label the data that has no labels yet, use any mix of these methods:

| Method | Command |
|---|---|
| In the browser, with the keyboard (1-9, Enter, A = accept teacher) | `decidio label ui` |
| A large teacher model on a GPU: Clef / Clef-Flash | `pip install -e .[teacher]`, then `decidio label clef` |
| Zero-shot from the descriptions of your options: CLIP (images) and CLAP (audio) | `decidio label zeroshot` |
| By hand, or in a spreadsheet | Edit `data/labels.csv` (id, one column for each question) |

A hand label always has priority over a teacher label. Other commands:

| Command | What it does |
|---|---|
| `decidio test --with ../other` | Runs the exact int8 model on a different labelled project |
| `decidio try files...` | Runs the int8 model on photos or sounds on this PC |
| `decidio info` | Shows the samples and the label coverage |

The training uses a cosine learning-rate schedule and keeps the final weights. Then it calculates the BatchNorm
statistics again over 150 clean batches. Rare options get a higher weight (`train.balance`). With
`train.patience: N`, the training stops early when the validation loss does not improve for N epochs. Parallel
workers make the batches (`train.workers`).

### Your own base model

A base is a usual trained project. `base:` accepts a folder, and also the name of a shipped base. A good base has
these properties:

- Many questions over varied data, so that the feature vector must hold much information.
- `aux: true` questions. These questions train like the others, but decidio **removes them before it flashes the
  model**. They can be as large as you want. Because of them, a model with few flashed answers can still tell
  hundreds of things apart inside.
- A `model.emb` that is wider than the preset (the shipped bases use 128).

The shipped bases have an 81-option `aux` question "what is the largest object". `general` also has a 52-option
`aux` question "what is the sound".

`decidio new mybase -t general-cam` gives the definition of the shipped base. To make your own base, do these steps:

1. Run `decidio new mybase -t general-cam`.
2. Change `decidio.yaml` in the new folder.
3. Run `decidio run`.
4. In each other project, set `base: ../mybase`.

## decidio.yaml

In paths in the file, use forward slashes (`D:/photos`) or single quotes (`'D:\photos'`).

```yaml
name: doorbell
board: xiao_sense            # xiao_sense (camera + mic) | s3cam (camera only)
base: general                # optional: build on a trained model (then no inputs: / model: here)
keep: [person_present]       # with base: its questions to keep (or: all)

inputs:                      # either or both
  image:
    size: 96                 # 16..96 model input edge (64 ~ 2x faster vision)
    color: gray              # gray | rgb
    motion: true             # extra channel |frame - frame 200 ms earlier|  (motion_ms: 200, frame_ms: 50)
                             # true | false | ego (camera's own shift removed first: drones, cars)
    platform: still          # still | moving: train + validate with simulated camera motion
  audio:
    sample_rate: 16000
    clip_seconds: 1.0        # n_mels: 40, win: 480, hop: 320, n_fft: 512, fmin: 20, fmax: 7600, gain_db: 2

model:
  preset: small              # tiny | small | base   (or width / emb / hidden)
  backbone: tiny             # tiny (from scratch) | mobilenet_v1_0.25 | mobilenet_v1_0.5 (ImageNet-pretrained vision)

train:
  epochs: 60                 # batch 64, lr 0.003 (0.001 with a mobilenet backbone), val 0.15, balance true,
                             # patience 0 (off), max_per_epoch 0 (cap samples per epoch), workers -1 (auto)

questions:
  someone_at_door:
    type: noul
    modality: image          # image | audio: which input answers it (optional, helps training + zero-shot)
    instructions: "Is someone standing at the door?"
    criteria: {true: "a person standing at a front door", false: "an empty doorstep"}
  sound:
    type: choice
    modality: audio
    criteria: {none: "quiet or other", knock: "knocking", bell: "a doorbell", dog: "a dog barking"}
    # aux: true              # train it, do not flash it

actions:                     # rules the board runs after every decision (up to 16); all fields: USE_CASES.md
  - {name: visitor, when: someone_at_door = true, above: 0.8, for_ms: 500, gpio: 21, level: 0, hold_ms: 3000,
     notify: {text: "Someone is at the door", cooldown_s: 120}}

sources: {}                  # see below
```

The board holds a maximum of 16 questions, 16 options for each question and 128 options in total. Aux questions do
not count.

### Sources (`decidio import`)

```yaml
sources:
  starter: {type: starter, labels: {...}}                                  # the shared starter set
  voc:                                  # Pascal VOC (zip or folder)
    type: voc
    paths: [downloads/VOCtrainval_06-Nov-2007.zip]
    crops: 2                            # square crops per photo; "objects" = one crop per object
    min_size: 0.12                      # smaller objects are "unknown", never "absent"
    labels:
      person_present: {true: [person]}                                     # noul: true if visible, else false
      vehicle_type: {car: [car], bus: [bus], none: []}                     # choice: largest visible object; [] = none of them
  coco:   {type: coco, annotations: downloads/annotations/instances_val2017.json, images: downloads/val2017.zip,
           max_images: 5000, labels: {...}}                                # fetch: true downloads missing images one by one
  yolo:   {type: yolo, path: D:/roboflow_export.zip, labels: {...}}        # Roboflow / Ultralytics; names from data.yaml or names: coco80
  sounds: {type: esc50, path: downloads/ESC-50, windows: 2, labels: {sound: {siren: [siren], other: "*"}}}   # "*" = every other class
  street: {type: urbansound8k, path: downloads/UrbanSound8K, labels: {sound: {siren: [siren], horn: [car_horn], other: "*"}}}
  mine:   {type: folders, path: D:/data, question: what}                   # D:/data/<option>/<images or audio>
  clips:  {type: video, path: D:/videos, every: 1.0, labels: {what: {cat: [cat], nothing: [empty]}}}   # labels from parent folder
  hush:   {type: quiet, count: 200, labels: {sound: {none: [quiet]}}}      # generated quiet-room clips
```

| Label rule | Description |
|---|---|
| `{false: "*"}` | Nothing in this source shows it |
| `same` | Each option is the class with the same name |

The base models use more source options. `decidio new x -t general` shows them in place:

| Source option | What it does |
|---|---|
| `caption_labels:` (in `coco`, next to `labels:`) | It has the same shape as `labels:`, but the lists are words or phrases to find in the captions of the photo (`indoors: {true: [kitchen, bedroom], false: [street, beach]}`). Two captions must agree |
| `class: speech` (in `folders`) | Each file is that class, whatever its folder |
| `per_class: N` (in `folders`) | A maximum of N files for each class |
| `windows: K` (in `folders`) | K one-second windows for each sound file |
| `speech_commands` (source type) | Reads Speech Commands v0.02 (`decidio fetch speech-commands`). It labels by word, and each clip is also class `speech` |
| `split: test` (in `speech_commands`) | Gives the clips in its test list (`testing_list.txt`) |
| `per_class: N` (in `speech_commands`) | N clips for each word |

`decidio fetch weather` unpacks a set of weather photos into `downloads/weather/train` and `.../test`, for the
`folders` importer. The test folder gets every 10th photo, and duplicates are removed. This command needs
`pip install pyarrow`.

During the training, decidio pairs each sample that has only one modality (a photo or a sound clip) with a sample of
the other modality. Thus photo datasets and sound datasets train one camera and microphone model together.

## Cameras that move (drone, car, bicycle, hand)

```yaml
inputs:
  image:
    motion: ego          # the board finds the camera's own shift between the two frames (block matching, +-13 px)
    motion_ms: 100       #   and removes it before the motion channel
    platform: moving     # training and validation see simulated camera motion: pan / tilt, zoom, roll, shake, blur
```

To start, run `decidio new <folder> -t moving-camera`. `decidio test --with <set> --moving` tests any model with
the data as a camera that moves sees it. The shift estimate finds only translation, and the simulation is not real footage.
Thus, record some footage from your platform and fine-tune the model with it.

## Boards and the runtime firmware

| Board | Profile | Camera | Microphone | Console and flash port |
|---|---|---|---|---|
| Seeed XIAO ESP32S3 Sense | `xiao_sense` | OV2640/OV3660 | PDM | native USB |
| ESP32-S3-CAM / -EYE pinout (Freenove and similar) | `s3cam` | OV3660/OV2640 | - | socket marked UART (CH340, 921600 baud) |
| ESP32-S3-CAM with a display connector | `s3cam-lcd` | - | - | The same as above. The pin layout is in the source code. It was not tested on a board |

In a project, `board:` in `decidio.yaml` accepts only `xiao_sense` and `s3cam`. A project for an `s3cam-lcd` board
can use `s3cam`, because both profiles have no microphone.

**One firmware, found at the start (0.4).** The same image runs on each board above. At the start, `board.c` tries
the camera pin layouts of the board family. A board with a USB-serial chip holds the receive pin of UART0 high. The
XIAO has no such chip. Then `board.c` listens for 0.2 s to find a PDM microphone. The samples of a real microphone
change, but a pin with no microphone gives one value.

The board saves the result. These console commands use it:

| Console command | What it does |
|---|---|
| `board` | Shows the result |
| `board auto` (System tab: **Detect again**) | Looks again |
| `board <profile>` | Forces one profile |

The console answers on the USB port of the chip. If the board has a USB-serial chip on UART0, the console also
answers there, at 921600 baud. `info` and the Status tab list the `sensors`.

A model that uses sound also runs on a board without a microphone. Its sound input gets silence, its sound questions
are marked off, and the rules on them never come on. This was measured on 800 held-out samples of `general`. With
silence instead of the real sound, the camera questions gave the same answer on 98.5 to 100% of samples. Their
accuracy changed by 0.6 points or less.

Thus `decidio install` first asks the board if it has a microphone. It sends `general-cam` to a board without a
microphone. That model has no sound branch to pay for: approximately 10.8 decisions each second against 8.4.

**Named inputs.** Apart from the answers of the model, a rule can test any named number:

| Named input | Source |
|---|---|
| `radio_motion` | The movement score from the Wi-Fi signal |
| `eye_moving`, `eye_figures`, `eye_approach`, `eye_new`, `eye_changed`, `eye_self_motion` | The six numbers of the fly eye |
| `chip_temp` | The temperature of the chip |
| `pin<N>` | Input pins |
| Other names | Each file in `firmware/main/sensors/` that registers an input (`rt_input_register`) |

On the board, a condition is `{"q":"in:<name>","op":">=" or "<","p":x}`. In a recipe, a condition is
`eye_moving > 0.5` or `in:<name> < 20`. `inputs` on the console and `/api/inputs` list the inputs with their values.
One function, `rt_board_pin_free`, decides which pins the rules, the input pins and the sensors can use. It never
gives them the pins of the camera, the microphone, the console, USB, the flash or the boot.

**The fly eye** (`eye.cc`, `fly/`) runs beside the model on the full 160x120 frame, as 40x30 points. These results
were measured with the model, Wi-Fi and the page in operation:

| Board | Eye frames each second | Time for each eye frame | Other effects |
|---|---|---|---|
| XIAO | 12 to 16 | 30 to 43 ms | The eye skips approximately a third of the camera frames. The model is 9% slower (8.4 against 9.2 decisions each second). Free internal memory is 23 KB, with a lowest point of 8.7 KB |
| ESP32-S3-CAM | 17 to 20 | 23 to 25 ms | - |

The arrays of the eye and the stack of its task are in PSRAM. The Eye tab requests one binary picture approximately
four times each second (`/api/eye.bin`). The picture contains the retina, the flow, what stands out, and the tracks.
It also contains a 64x64 colour close-up of the figure that the eye follows, cut from the full frame. The eye was
tuned on a different board at 18 to 23 frames each second. Its thresholds were not tuned again for these boards.

**A second look by the model at the close-up: measured, not built.** The plan is that the model looks a second time
at the thing that the eye follows. For the test, 1,000 COCO val photos became frames of 160x120, as on the board. The
boxes of the photos took the place of the eye. Then the int8 `general-cam` model answered a question about a square
around one object:

| Look (only objects that are the only one of their type in the photo) | Person, 24-48 px | Person, more than 48 px | False "person" on other things |
|---|---|---|---|
| Whole frame (as now) | 75% | 78% | 17% |
| Close-up cut from the 160x120 frame, made larger | 58% | 80% | 5% |
| Close-up with real detail (cut from the full photo) | 83% | 90% | 14% |

The two size groups have 12 and 40 objects, thus the differences are only indications. Over all 1,279 objects, the
enlarged close-up found small persons much less often than the whole frame: 25% against 65%. An enlarged cut of the
current frame makes the model worse. The reason is that the model trained on whole scenes, and the cut is not sharp.
A close-up helps only when it has detail that the frame does not have.

**The sharp close-up: the sensor zooms. Built and measured on both boards.** The camera driver cannot change the
frame size while it runs. Thus the frame stays 160x120, and the firmware tells the OV3660 to read a smaller window of
its 2048x1536 pixels into it (`camera.c`, `set_res_raw`).

The window can be the whole sensor (the normal view). It can also be as small as 160x120 sensor pixels, which is
12.8 times nearer. While a person has the Eye tab open and
the eye follows a figure, the eye requests one such frame every 4 seconds. The page shows it below the picture.

| Check on the boards | Result |
|---|---|
| The close-up shows the requested part (correlation with that part of the wide frame, 15 zooms, all four picture orientations) | 0.92 to 0.99. The best fit is at the requested place, or 1 pixel from it |
| Detail (mean edge strength, compared with the same part cut from the wide frame) | 1.5 to 8 times more |
| Colour | Correct when the window has an even number of rows. With odd heights, red came out blue. Thus the widths are multiples of 16 |
| Time with no wide frame for each zoom | Approximately 0.6 s. The window needs 3 frames to become stable, then the board takes 1 frame. The exposure of the wide view then needs 6 frames to return. It was stable after 3 frames on the XIAO, and after 5 on the S3-CAM |
| Wide view after the zoom | The mean colour is within 3/255 of the value before. The model and the eye never get a zoomed frame or a frame that is not stable |

The sensor controls its own exposure. It adapts to the window within the three frames that the window needs to
become stable. Thus a dark figure in a bright scene gets the correct exposure for the figure. This test used zooms
that were requested on the console (`eye zoom <x> <y> <w>`), on a scene that did not move.

The test with a figure that moves was **not** done, because nobody walked past during the tests. Thus it is an open
question if the window still holds the figure when the board takes the frame (approximately 0.2 s after the
request). Other sensors (OV2640 and others) keep only the 64x64 cut. The model does not look at the sharp close-up
yet. That is the next step.

**Not in 0.4:**

- Records to a memory card. Each board reports the memory card as "none".
- Eye events in the list of the board group.
- The sound side of the fly work (the ear).

The firmware is generic. At boot, it reads all model-specific items (inputs, audio frontend, questions, rules) from
the model package (`out/model.dcd`) in the `model` flash partition. Thus a new model never needs a rebuild. The flash
layout (`firmware/partitions.csv`) fits in 8 MB:

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `ota_0`, `ota_1` | `0x10000`, `0x290000` | 2.5 MB each | Two firmware slots, so that the page can replace the firmware. The app is 1.6 MB |
| `model` | `0x510000` | 1 MB | The model package |
| `cfg` | `0x610000` | 64 KB | The settings store (Wi-Fi, alerts, changed rules) |

| Part of the runtime | What it does |
|---|---|
| Camera | 160x120 RGB565 → centre square → 96x96, every 50 ms, into a ring buffer |
| Microphone | PDM at the sample rate of the model. One log-mel frame for each 20 ms hop, thus a full second of features is always ready |
| Decision loop, on core 1 | Runs on each new audio hop, or on each camera frame for models that use only images |
| Tensor arena | In internal SRAM when it fits and leaves space for Wi-Fi. In PSRAM if not |
| Wi-Fi, the web page (`web.c`), alerts (`alert.c`) and the rules (`rules.c`, `actions.c`) | Run beside the decision loop. A slow network never delays a decision, because the alerts wait in their own queue |

Console commands (one command on each line):

| Console command | What it does |
|---|---|
| `info` | Shows the board, the model and the `sensors` |
| `run` | Starts the decisions |
| `stop` | Stops the decisions |
| `print <ms>` | Sets the time between two output lines (0 = each decision) |
| `collect <ms>`, `collect off` | Sends a sample to the PC every `<ms>` (200 ms minimum), or stops it. `decidio collect` uses this command |
| `orient 0-3\|auto` | Sets the orientation of the picture. The setting stays after a restart |
| `profile on\|off` | Prints the time of each layer |
| `reboot` | Restarts the board |
| `mem` | Shows the free memory |
| `wifi` | Shows the Wi-Fi state |
| `wifi "<network>" "<password>"` | Joins a network |
| `wifi forget` | Forgets the network |
| `wifi off` | Forgets the network and keeps Wi-Fi off after the next start |
| `alert test <telegram\|webhook\|mqtt>` | Sends a test alert |
| `alert set <key> <value>` | Sets one alert setting |
| `radio scan`, `radio <channel> [sender]`, `radio motion on\|off`, `radio off` | Radio sensing (refer to "Radio sensing" below) |
| `board`, `board auto`, `board <profile>` | Refer to "One firmware, found at the start" above |
| `inputs` | Lists the named inputs with their values |
| `eye on\|off` | Sets the fly eye on or off. The setting stays after a restart |
| `eye zoom <x> <y> <w>` | Requests a sharp close-up of that part of the frame |
| `pin in <gpio> [up\|down\|none]` | Uses a pin as an input. The change applies at the next start |
| `i2c scan <sda pin> <scl pin>` | Lists the addresses of the I2C parts on these two pins |
| `mic probe <clk pin> <data pin>` | Looks for a PDM microphone on these two pins (only while the microphone does not run) |

The output lines are JSON:

```json
{"answers":{"person_present":{"p_true":0.931},"subject":{"choice":"person","confidence":0.81,"probabilities":{"none":0.01,"person":0.81}}},"prep_us":310,"invoke_us":12400,"hz":49.0}
{"event":"person","state":"on","p":0.912,"ms":123456}
```

The numbers in this example are only examples. `profile on` prints the time of each layer.

### Radio sensing

For each Wi-Fi frame that the board receives, it can report how each subcarrier arrived (channel state information,
CSI). A body that moves between the sender and the board changes that pattern. The beacons of your router are a
steady source, approximately 10 each second.

**On the board:** after the board joins your Wi-Fi, `radio motion on` (or the Radio tab) keeps a movement score. The
score comes from the beacons of the router over the last 2 seconds. Rules read it as `radio_motion` (builds
`radio-room-movement`, `radio-movement-alert`, `radio-movement-night`). When a person sits without movement next to
the board, the score is approximately 0.045. This is the same as the score on the PC (below).

**Recordings, for more than movement:** the board can also listen without a joined network, and it then sends
nothing.

```
python tools/radio_record.py --port COM5 scan                          # senders heard: address, channel, signal
python tools/radio_record.py --port COM5 listen --channel 6 --sender aa:bb:cc:dd:ee:ff \
        --seconds 120 --label empty --out empty.npz                    # a movement score each second + a recording
```

Measured on a XIAO ESP32S3 Sense:

| Measurement | Result |
|---|---|
| Access points that the board heard | Two, each at a steady 10 frames each second |
| Usable subcarriers in each frame | 51-52 |
| Decisions while Wi-Fi is on | The model continues to decide at 100 ms |
| Movement score in one two-minute test, in one room | 0.04 with a person who sits without movement. 0.06-0.17 while the person walks (each 2-second window is above each still one). 0.04 with the room empty |

Thus the score sees movement, but it cannot tell a person who does not move from an empty room.

**What is not there yet:** a model input for radio. Only the one movement score above exists. Recordings with the
labels `empty`, `sitting` and `walking` are the training data for such an input.

Radio patterns are specific to one room and one layout. Thus anything finer than movement will be a question that you teach in place, not a part of a
base model. While the board is joined to a network, it stays on the channel of that network. The board refuses
`radio <channel>` for a different channel.

### Rebuild the firmware

Rebuild the firmware only after you change `firmware/`:

1. Open an ESP-IDF 5.5 shell.
2. Run `decidio build-firmware`. It writes `decidio/firmware_bins/esp32s3/`.

To compile a model into your own Arduino sketch instead, run `decidio export --arduino <sketch folder>`.

## What is in this folder

| Path | Contents |
|---|---|
| `decidio/` | The Python tool (the `decidio` command) |
| `decidio/base/` | The trained base models (definition, weights, int8 model, reports) |
| `decidio/starter_data/` | The two lists of the starter set (file names and labels; decidio downloads the files) |
| `decidio/templates/` | The templates for `decidio new` |
| `decidio/firmware_bins/` | Prebuilt runtime firmware (`esp32s3/`: one image for all supported boards) |
| `firmware/main/sensors/` | One file for each add-on sensor. `_template.c` is the file to start from |
| `firmware/main/fly/` | The fly eye (refer to its [README](firmware/main/fly/README.md)) |
| `skills/` | Procedures for LLM agents: install an idea, add a sensor, add a board, add an idea |
| `firmware/` | The source code of the runtime firmware (ESP-IDF). `firmware/main/www/index.html` is the page of the board |
| `recipes/` | The ready-made ideas. Rules for the base models (`home`, `carried`, `projects`), rules for the fly eye (`eye`), and what to record for the other ideas (`teach`) |
| `installer/` | The browser installer. `python tools/make_builds.py` writes its builds (generated files, approximately 50 MB) |
| `tools/make_builds.py` | Checks the recipes again and writes the build of each idea |
| `tools/make_starter.py` | Makes the starter lists again from the public datasets |
| `tools/radio_record.py` | Records the Wi-Fi channel state from a board (radio sensing) |
| `tools/fake_tuya_device.py` | A simulated Smart Life device on the PC, to test the device control of a board (needs `pip install tinytuya`) |
| `USE_CASES.md` | 111 things to build, and what each one needs |
| `README.md` | Quick install and board setup |
| `INSTALL_WITH_LLM.md`, `AGENTS.md` | The steps that an LLM agent follows to install an idea for you |
| `requirements-install.txt` | The few packages that you need to install an idea (no TensorFlow) |
| `tests/` | Parity tests between the firmware and Python, the rule engine of the board run on the PC, and checks of the recipes and builds |

## Data and licences

decidio downloads public datasets from their own hosts. This repository does not contain any of their files.

| Data | Use | Licence |
|---|---|---|
| COCO 2017 (cocodataset.org) | Photos of the base models (train2017), photos of the starter set (val2017) | Annotations: CC BY 4.0. Photos: their Flickr licences |
| ESC-50 (K. J. Piczak) | Sounds of the base model (folds 2-5), sounds of the starter set (fold 1) | **CC BY-NC** (non-commercial) |
| WeatherNet-05-18039 (Hugging Face, prithivMLmods) | Weather photos of the base models | Apache 2.0, as its uploader states. The source of the photos is not stated |
| Speech Commands v0.02 (Google) | Spoken words of the base model | CC BY 4.0 |
| LibriSpeech dev-clean / test-clean (openslr.org) | Continuous speech of the base model | CC BY 4.0 |
| ImageNet-pretrained MobileNetV1 weights (Keras) | The start point of the vision body | Apache 2.0 |
| male-cns v1.0 connectome (Janelia FlyEM, University of Cambridge) | The connection tables of the fly eye (`firmware/main/fly/circuit_data.h`, `optic_lobe_data.h`). These tables are in this repository | CC BY 4.0 |

The training data of the sound branch of the `general` base model includes ESC-50, which is non-commercial. If you
need a model for commercial use, use `general-cam` (it has no sound data). Or train the sound branch again on data
whose licence permits commercial use. The same applies to the idea builds. A build for a board with a microphone
contains `general`, and a build for a board without a microphone contains `general-cam`.

Both bases use the weather photo set. Its origin is not documented beyond its Hugging Face page. If that is important
to you, remove the `weather` source and train the model again.

---

decidio is a product of [IO Services](https://ioservices.io/). Built in the USA.
