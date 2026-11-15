# Changelog

## Unreleased

No changes.

## 0.4.0

This is the first public release. It has the Python tool 0.4.0 and the firmware decidio-rt 0.4.0.

### Python tool (`decidio`)

| Function | Commands |
|---|---|
| Find the board, show the ideas, install an idea | `decidio ports`, `decidio builds`, `decidio install <idea> --port <port>` |
| Connect the board to Wi-Fi through USB | `decidio wifi <network> --port <port>` |
| Write the firmware or a model to the board | `decidio flash firmware`, `decidio flash model` |
| Make a project, get data, label it | `decidio new`, `decidio fetch`, `decidio import`, `decidio collect`, `decidio label` |
| Teach a new question on a base model (about one minute on a laptop) | `decidio teach` |
| Train a model from a base or from the start | `decidio run`, `decidio train` |
| Examine a model on the computer | `decidio try`, `decidio test`, `decidio eval` |
| Export a model to an Arduino sketch | `decidio export --arduino <folder>` |
| Build the firmware with ESP-IDF 5.5 | `decidio build-firmware` |

- The install of an idea needs no TensorFlow: `pip install --no-deps -e .` and `requirements-install.txt`.
- `decidio install` asks the board what it has. It sends `general` to a board with a microphone and `general-cam`
  to a board without a microphone.
- The starter set: 1000 COCO val2017 photos and fold 1 of ESC-50, downloaded from their own hosts.
  `decidio fetch starter` gets it.

### Base models

| Base model | Inputs | Flashed size | Flashed questions |
|---|---|---|---|
| `general` | Camera (RGB and motion, 96 x 96) and microphone (1 second) | 493 KB | 9 about the picture, 5 about the sound |
| `general-cam` | Camera (RGB and motion, 96 x 96) | 403 KB | The same 9 about the picture |

GUIDE.md gives the accuracy of each question, measured on held-out data on a computer.

### Ideas

- 63 ideas install as they are: a base model and a set of rules. 32 more ideas need a question that you teach.
- USE_CASES.md lists 111 ideas and what each idea needs.
- The rules of each idea are in `recipes/*.yaml`.

### Firmware (decidio-rt 0.4.0)

- One firmware for each supported board. At start, it finds the camera and the microphone.

  | Board | Camera | Microphone |
  |---|---|---|
  | Seeed XIAO ESP32S3 Sense | Yes | Yes |
  | ESP32-S3-CAM, ESP32-S3-EYE pinout | Yes | No |

- The firmware reads the model and its rules from the `model` partition. A new model needs no firmware rebuild.
- Rules set pins, write events to USB and send alerts to Telegram (optionally with a photo), a webhook or MQTT.
  A board operates a maximum of 16 rules.
- The page of the board has the tabs Status, Live, Eye, Ideas, Devices, Boards, Alerts, Radio, Wi-Fi and System.
  You can change rules, add ideas, and load a new model or new firmware from the page.
- Smart Life (Tuya) devices: the board switches them on your Wi-Fi with the local protocol (versions 3.1, 3.3, 3.4
  and 3.5).
- Groups of boards: boards with the same group secret share the states of their rules in signed messages. A rule
  can depend on a rule of a different board.
- The eye: a small model of the motion circuits of the fruit fly eye. It finds and follows objects that move, and
  shows a close-up. With an OV3660 camera, it also makes a sharp close-up.
- Radio sensing: a movement score from the Wi-Fi signal of your router.
- Inputs: input pins, an I2C scan, the chip temperature, and `firmware/main/sensors/` for add-on sensors.
- Wi-Fi setup from a phone (setup network decidio-xxxx), through USB, or from the installer website. The page is at
  `http://decidio-xxxx.local`.

### Browser installer

- `installer/index.html` installs an idea through USB from Chrome or Edge, with ESP Web Tools. It is published at
  https://ioserviceslabs.github.io/decidio/.
- `python tools/make_builds.py` makes its builds. The workflow `pages.yml` makes them again for each push to `main`.

### Tests

- The rule engine, the model loader, the log-mel frontend and the camera-shift estimate of the firmware, compiled
  for the computer and compared with the Python side.
- Checks of the recipes, the catalogs, the builds and the partition table.

### Known limits

- README.md lists what was tested and what was not tested, in the section "What was tested".
- An install through the installer website was not tested on a board.
- Linux and macOS were not tested with a board.
- SECURITY.md lists the security limits.
- The `general` model and every build for a board with a microphone contain a sound part trained on ESC-50, which
  permits only non-commercial use. NOTICE lists the data licences.
