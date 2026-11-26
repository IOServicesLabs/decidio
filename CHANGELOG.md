# Changelog

## Unreleased

## 0.5.2

It has the Python tool 0.5.2 and the firmware decidio-rt 0.5.2.

### Added

- Firmware: **Check for updates** on the System tab. The board reads the newest version from the installer website
  and, when you click **Install**, downloads the firmware over Wi-Fi and restarts with it. Then it gives the firmware
  to the other boards of the group. The board does not look for updates by itself. The routes are `GET /api/update`
  and `POST /api/update`.
- Installer builds: `builds/app.bin`, the app image for **Check for updates**.

## 0.5.1

It has the Python tool 0.5.1 and the firmware decidio-rt 0.5.1.

### Changed

- Alerts: **Use the community server** saves the new topic immediately. The box **Subscribe in the ntfy app** shows the
  server and the topic as two values, with **Copy** buttons and a link that opens the ntfy app on Android. The box stays
  on the page. **Make a new topic** replaces the topic.

## 0.5.0

It has the Python tool 0.5.0 and the firmware decidio-rt 0.5.0.

### Added

- Alerts: a new destination, ntfy. It sends push messages to the ntfy app on a phone, through `ntfy.sh` or a server of
  your own, with an optional access token. For a rule with a photo, the alert has the camera picture, and a second,
  quiet message has the last clip of the microphone as a WAV file. The Alerts tab, step 4 of the installer website and
  the console (`alert test ntfy`) have it.
- Alerts: the button **Use the community server** on the Alerts tab and in step 4 of the installer website. It makes a
  new topic with a long random name on the decidio community server for ntfy, `https://nftyio.duckdns.org`. The server
  is optional.
- Alerts: if the ntfy server has no space for the picture, the board sends the text of the alert alone.
- Firmware: the route `GET /api/sound.wav`, the last clip of the microphone.

- Devices tab: a new card **Devices heard on the network**. It shows a name for each Smart Life device that the board
  hears. The board asks the router (DNS), then the DNS server from DHCP, then mDNS.
- Devices tab: **Try** switches a heard device before you save it. Type the local key and click **Blink**, **On**,
  **Off** or **Read**. **Blink** sets the other state for 2 seconds, then the first state again. **Use** writes the
  data in **Add a device**.
- Firmware: the route `POST /api/devices/try`.
- Groups: one board sends the alerts of all the boards in the group. The other boards give it their alerts in signed
  messages. If it does not answer in 3 seconds, the board sends the alert itself.
- Alerts: after an alert, the board keeps the alerts of the next 60 seconds and sends them together as one message.
  The setting `alert_window` changes the time (`0`: send each alert immediately).
- Rules: the field `urgent`. The alert of an urgent rule always goes immediately.
- Alerts tab: **Use these alert settings on all boards in the group** (`POST /api/alerts/share`).
- Low power for a board on a battery (`power.c`). It is off until you switch it on. The board sleeps, wakes on a
  timer (1 to 1440 times each day) and optionally on a motion sensor pin, looks for some seconds, and connects to
  Wi-Fi only to send and at check-ins. The rules keep their states through the sleep.
- Page: the tab **Power**. It has a plan with an estimate of the days on one charge, the list of what changes when
  low power is on, the switch, and the measured counts.
- Firmware: the routes `GET /api/power` and `POST /api/power`, and the console commands `power`, `power on|off`,
  `power stay|go` and `power wakes <n>`.
- The sensor file `sensors/battery.c`: the battery voltage through two resistors, as the inputs `battery_v` and
  `battery_pct`. The Power tab can add a low-battery alert.

- Groups: the group password. It is optional. **Add to the group** makes a group with a random secret and no
  password. If you save a group password, the board gives it to the other boards of the group, and it is then also
  the password of the page of each board in the group (user name `decidio`). A page password is not necessary.
- Page: the card **Alerts of the group** on the Group tab. It shows the alert destinations and the rules of each
  board. You can set **send an alert**, **with a photo** and **urgent** for each rule of each board there.
- Page: the tab **Group**. It shows each board of the group on one page: the camera picture, the strongest answers
  and the rules. **Open** shows all the tabs of that board. Each board of the group can show this page.
- Groups: the firmware update of the group (`push.c`). When a board gets new firmware from its page, it restarts and
  then sends that firmware to each other board of the group that has a different build. The button **Give the
  firmware of this board to the other boards** and the route `POST /api/home/push` do the same by hand.
- Groups: the radio link. A board in a group also sends its broadcasts with ESP-NOW, directly to the boards in radio
  range. A board reads such a message immediately. The broadcasts through the router continue.
- `/api/home`: the fields `fw`, `id` (the build), `radio` and `push`, and `id`, `direct`, `rssi` and `lead` for each
  board. `/api/info`: the field `group_pass`. `/api/home`: the field `open` (a group with no password), and
  `POST /api/home` with `{"start":true}`.
- Camera: **Turn the picture** on the Live tab, for a board that is not upright (90, 180 or 270 degrees). The
  route `POST /api/camera` with `{"rotate":90}` does the same, and `/api/info` has the field `cam_rot`. A turn of 180
  degrees also turns the picture of the eye. A quarter turn turns only the frame of the model, the camera picture
  on the page and the photo of an alert.
- Boards without a camera: the types `mic_only` (a XIAO ESP32S3 Sense that does not use its camera) and `plain` (an
  ESP32-S3 board with no camera and no microphone), on the installer website and in `decidio install --board`. The
  package tells the firmware that there is no camera (`"hw"` in the package header). The firmware then does not look
  for one. With a microphone, the sound questions answer and the picture input gets a black frame. With no
  microphone, the model does not run, and the rules use the Wi-Fi movement score, pins, sensors and other boards.
- Firmware: the board profile `plain`.
- README: pictures of the Live, Ideas and Power tabs.

### Changed

- The Wi-Fi movement score starts by itself when a rule uses it and you did not set it to off on the Radio tab.
- The firmware stops the camera interface of the chip before a restart. Before, the interface continued to run
  after a restart by software. A start that did not use the camera then could not use Wi-Fi.
- Wi-Fi starts 2.5 seconds after the start of the board or later.
- Two sound buffers moved from the internal memory to the PSRAM. A board with a microphone has approximately 16 KB
  more free internal memory. The decision rate did not change (9.2 decisions each second on the XIAO).
- The model loader no longer takes a block of internal memory to see if it fits. It reads the free size first.

- README: the install from a browser is now procedure A. The install with commands is procedure B.
- The label **Group secret** on the Boards tab is now **Group password**.
- `/api/devices`: each heard device has the fields `name` and `named`.
- Alerts: the text shows the room of the board (or its name if it has no room). The webhook JSON has the field
  `room`. `device` and the MQTT topic name the board that saw the event.
- The alert text of a rule on an input shows the value of the input, not a percentage (before: 62 °C showed as
  "6200%").
- `/api/info` alerts: the fields `window`, `grouped` and `handed`. `/api/home`: the field `sender`, and `alerts` for
  each board.
- The prebuilt firmware in `decidio/firmware_bins/esp32s3/` was built again with these changes (ESP-IDF 5.5).

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
