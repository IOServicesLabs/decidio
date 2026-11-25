# Install a decidio idea with an LLM agent

This file is for an LLM agent that can run commands on the computer of the user. The user has an ESP32-S3 camera
board on a USB cable and wants one ready-made idea on it. Obey the rules, then do the steps in sequence.

If you cannot run commands, tell the user each command and ask for its output.

## Rules

1. Do not ask for the Wi-Fi password, the Telegram bot token or other secrets. Do not put them in a command. The user
   types them into the page of the board, into the prompt of `decidio wifi`, or into step 4 of the installer website.
2. To write to the board, use only the `decidio` commands in this file. Do not run `esptool erase-flash`. Do not
   write to flash offsets yourself.
3. Write to one board only. If `decidio ports` shows more than one port, ask the user which port to use.
4. Before step 5, tell the user that the install replaces the firmware and the model on the board. Continue only
   after the user agrees.
5. Do not tell the user that an idea is a safety device or an alarm. The model makes errors.
6. Do not install the full requirements (`pip install -e .` installs TensorFlow, approximately 2 GB). The small
   install in step 2 is sufficient.
7. If a step fails two times, stop and tell the user. Give the last lines of the output.

## Steps

### 1. Make sure that Python is available

```
python --version
```

The version must be 3.10, 3.11 or 3.12. On Linux and macOS, the command can be `python3`. On Windows, `py -3.12` is
an alternative. If no such version is installed, tell the user to install Python 3.12. As an alternative, the user
can use the installer website at https://ioserviceslabs.github.io/decidio/ in Chrome or Edge on a computer. Then stop.

### 2. Install the tool

Run these commands in the folder that contains this file.

```
python -m venv .venv
.venv\Scripts\python -m pip install --no-deps -e .
.venv\Scripts\python -m pip install -r requirements-install.txt
```

On Linux and macOS, use `.venv/bin/python` in place of `.venv\Scripts\python`. In the steps below, `PY` is that
Python program. If the folder `.venv` exists already, do not make it again.

Make sure that the install is correct:

```
PY -m decidio --version
```

The output must be `decidio 0.4.0` or a later version.

### 3. Find the board

```
PY -m decidio ports --json
```

The output is a JSON list. Each item has `port`, `usb`, `board`, `what` and `description`. You do not have to know
which board it is. The firmware is the same for each supported board. When the firmware starts, it finds its camera
and its microphone. The install command asks the board what it found.

The supported boards are the Seeed XIAO ESP32S3 Sense (camera and microphone) and the ESP32-S3-CAM boards with the
ESP32-S3-EYE pin layout (camera only).

If the list is empty:

- Tell the user to use a USB cable that carries data.
- XIAO: tell the user to hold the **B** (BOOT) button, connect the cable, then release the button.
- ESP32-S3-CAM: tell the user to use the socket with the label UART.
- Run the command again.

If the board is not one of the boards above, do not install. A different ESP32-S3 camera board must have a board
profile first (`skills/add-board/SKILL.md`). The firmware does not operate on boards that are not ESP32-S3.

### 4. Select the idea

```
PY -m decidio builds
```

Each line has the idea name, its number in USE_CASES.md, the boards that it operates on, and a title. Select the
idea that agrees best with what the user wants. Tell the user your selection and ask the user to confirm it.

- An idea that shows only `xiao_sense` uses the microphone. Do not select it for a board that has no microphone. If
  you select it, the install command stops and tells you.
- Ideas 106 to 111 use the fly eye (movement in the camera picture). They operate only with a camera.
- An idea with `(teach it first)` has no ready build. Tell the user that the idea must be taught first (GUIDE.md,
  section 2). Offer the nearest ready idea.
- If no idea agrees, tell the user. The user can add rules later on the Ideas tab of the page of the board.

### 5. Install

```
PY -m decidio install <idea> --port <port>
```

The command writes the firmware and asks the board for its sensors. Then it writes the model that agrees with them.
This takes one to three minutes. The install is successful when the output contains `installed '`.

The output has a line `the board reports: ...` with the board type, the camera, the microphone and the memory card.
Tell the user what the board found.

- `camera none`: tell the user to examine the flat cable of the camera, then to restart the board.
- `the board did not answer`: the install continues with a model that operates on each board. Do step 6 to make
  sure that the board operates.

If the output contains `Failed to connect` or stops at `Connecting`:

- XIAO: tell the user to disconnect the cable, hold the **B** (BOOT) button, connect the cable, then release the
  button. Run `PY -m decidio ports --json` again, because the port name can change. Then run the install again.
- ESP32-S3-CAM: add `--baud 115200` and run the install again.

If the port is busy or access is denied, tell the user to close each program that uses the port. On Linux, the user must be in the `dialout` group.

If the output tells you that the chip is not an ESP32-S3, stop. The board is not supported.

### 6. Do a check

Wait 10 seconds, then run:

```
PY -m decidio wifi --port <port>
```

An answer from the board shows that the firmware operates. The answer tells you if the board is connected to Wi-Fi
and gives its address. If there is no answer, tell the user to disconnect the cable and connect it again. Then run
the command one more time.

Do not use `decidio monitor` for the check. It does not stop.

### 7. Give the user the next steps

Tell the user these items, in your own words:

1. The setup text that the install command showed (where to point the board).
2. How to connect the board to Wi-Fi. There are three methods:
   - From a phone: connect to the network **decidio-xxxx** (no password). On the setup page, select the home network
     and type its password.
   - From this computer: the user runs `decidio wifi <network name> --port <port>` in a terminal and types the
     password at the prompt. The user runs it, not you: the user must type the password on a keyboard, and you must not see it.
   - From the installer website: the user opens https://ioserviceslabs.github.io/decidio/ in Chrome or Edge and
     clicks **Connect to the board** at step **4. Set up**. Then the user types the network name and the password in
     the **Wi-Fi** form and clicks **Save Wi-Fi**. Before this, stop each command of yours that uses the port.
3. The address of the page of the board (`http://decidio-xxxx.local`, or the address from step 6).
4. Set a password for the page immediately, on the Wi-Fi tab or in step 4 of the installer website. Until then, each
   person on the network can open the page and see the camera picture.
5. For alerts: the Alerts tab has Telegram, webhook, MQTT and ntfy fields, each with a test button. README.md has the
   Telegram procedure.
6. The idea is not a safety device. The user must test it in the room where it operates and adjust the threshold on
   the Ideas tab.

## Other tasks

| The user wants | Command |
|---|---|
| A different idea, with the same firmware | `PY -m decidio install <idea> --port <port> --keep-firmware` |
| To know what sensors the board found | `PY -m decidio wifi --port <port>`, then the Status tab of the page of the board |
| A sensor that the firmware does not have | Obey `skills/add-sensor/SKILL.md` |
| A camera board that the firmware does not know | Obey `skills/add-board/SKILL.md` |
| A new idea from questions and inputs that exist | Obey `skills/add-idea/SKILL.md` |
| The address of the board | `PY -m decidio wifi --port <port>` |
| The board to forget its Wi-Fi network | `PY -m decidio wifi --port <port> --forget` |
| The builds for the browser installer | `PY tools/make_builds.py`, then `PY -m http.server -d installer 8000` |
| To install or set up a board without Python | No command. Tell the user to use the installer website, https://ioserviceslabs.github.io/decidio/, in Chrome or Edge on a computer |

An install keeps the Wi-Fi settings and the alert settings of the board. Rules that the user changed on the page
belong to the model package of that idea. A new idea starts with its own rules.

For rules and questions, and to train a model, read USE_CASES.md and GUIDE.md.

---

decidio is a product of [IO Services](https://ioservices.io/). Built in the USA.
