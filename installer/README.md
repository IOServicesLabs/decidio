# decidio installer website

This folder is the public website of decidio, at <https://ioserviceslabs.github.io/decidio/>. On the website, a person
selects a type of board and an idea. The person installs the idea on an ESP32-S3 board over USB. Then the person sets
up the board over USB: Wi-Fi, page password and alerts.

The website needs Chrome or Edge on a computer. The person does not need Python, drivers or other software. Steps 1
and 2 also operate in other browsers and on phones, for reading only.

## Files

| File | Contents |
|---|---|
| `index.html` | The page: a short introduction and the steps **1. Board**, **2. Idea**, **3. Install** and **4. Set up** |
| `site.css` | The look of IO Services: stone, ink and one olive color, system fonts |
| `site.js` | Steps 1 to 3. It reads `builds/index.json`, shows the boards and the ideas, and gives the manifest of the idea to ESP Web Tools |
| `setup.js` | Step 4. It opens the port of the board with Web Serial and sends the console commands |
| `demo.js` | The simulated board of the demo mode |
| `builds/` | The builds. They are generated, and the repository does not contain them |

The files are plain HTML, CSS and JavaScript. There is no build step and no framework. The page loads one external
file: ESP Web Tools 10.1.1 from unpkg.com, with a fixed version. The page does not use analytics. The page keeps
only the last board and the last idea in localStorage (`decidio.board`, `decidio.idea`). It does not keep passwords
or tokens.

## How the builds are made

The builds are about 50 MB. This command writes them to `installer/builds/`:

```
python tools/make_builds.py
```

```
builds/index.json                 every idea: title, setup text, what it needs, the boards it runs on
builds/firmware.bin               bootloader + partition table + firmware, for offset 0 (shared by all ideas)
builds/<board>/<idea>.dcd         the base model with that idea's rules, for the model partition
builds/<board>/<idea>.json        the ESP Web Tools manifest joining the two
```

The firmware (`builds/firmware.bin`) is the same for each board. `<board>` is the type of board, and it selects only
the model: `xiao_sense` has a microphone, `s3cam` has no microphone. The types `mic_only` and `plain` are for a board
without a camera: their packages tell the firmware not to look for a camera.

An idea build is different from the base model only in its rules. Thus `decidio install <idea> --port COMx` gives the
same result. Another way is to install a build and select ideas on the page of the board.

The install does not erase the settings of the board. The Wi-Fi settings, the page password and the alert settings
stay. They are in their own flash partition (`cfg`, at 0x610000 in `firmware/partitions.csv`), and `firmware.bin`
stops before it. Rules that a person saved on the page of the board belong to the model package that was on the board
at that time. Thus a new idea starts with its own rules.

## Serve the site on your computer

1. Make the builds (see above).
2. Start a local server:

   ```
   python -m http.server -d installer 8000
   ```

3. Open `http://localhost:8000` in Chrome or Edge.

Browsers allow USB only from `https://` addresses or from `localhost`. For the demo mode, open
`http://localhost:8000/?demo`.

## How the site is published

A GitHub Actions workflow of the repository publishes the site to GitHub Pages. The workflow runs
`python tools/make_builds.py`. Then it publishes the `installer/` folder, with `installer/builds/`. A different static
host that uses `https://` also operates.

## How step 4 operates

Step 4 operates with each board that has decidio firmware. It does not matter if the firmware came from this site or
from the `decidio install` command.

The page uses the console of the firmware (`firmware/main/console.c`). The console reads text lines on the USB port of
the chip (Seeed XIAO ESP32S3 Sense, USB-Serial-JTAG) and on UART0 at 921600 baud (ESP32-S3-CAM, through the USB-UART
chip of the socket with the label UART).

**Open the port.** The page opens the port at 921600 baud. Then it sets RTS to off, and after that DTR to off. This is
the same state as `decidio/serialio.py` (`dtr = rts = False`), so the board does not stay in reset or in download mode.
On a usual reset circuit, RTS alone pulls EN low (reset) and DTR alone pulls IO0 low. IO0 has no effect while the
chip runs, thus RTS goes off first.

**Wait for the console.** The firmware reads commands only after its start (`rt_console_ready` in `main.c`). The page
sends `info` each 4 seconds, for a maximum of 30 seconds, until a `{"info":...}` line comes back.

**One command at a time.** Each command answers with a JSON line, usually `{"ok":"<command>"}` or `{"error":"..."}`. Log
lines, model answers and events also come from the board. The page reads line by line. It waits for the one key that
the command gives, with a time limit. The next command goes out after the answer.

| Action on the page | Commands that the page sends | Answer that the page waits for |
|---|---|---|
| **Connect to the board** | `info`, then `wifi` | `{"info":{...}}`, then `{"wifi":{"state":...,"setup_open":...}}` |
| **Save Wi-Fi** | `wifi "<name>" "<password>"`, then `wifi` each 2 seconds, a maximum of 15 times (as `decidio wifi`) | `{"ok":"wifi"}` (`{"error":"wifi: ..."}` comes first if the board refuses), then the state `online` |
| **Save page password** | `alert set page_pass "<password>"` (`""` removes the password) | `{"ok":"alert"}` |
| **Save alerts** | `alert set <key> "<value>"` for each field that the person changed; `alert set <key> ""` for each key of a destination that the person removes | `{"ok":"alert"}` for each command |
| **Send test** | `alert test telegram`, `alert test webhook`, `alert test mqtt` or `alert test ntfy` | `{"alert_test":{"ok":...,"status":...,"ms":...,"detail":"..."}}`, a maximum of 40 seconds |
| **Restart the board** | `reboot`, then `info` | The `{"info":...}` line that the board prints at its start. If the USB port goes away, the page opens it again when it comes back |

The alert keys and their maximum lengths (from `firmware/main/alert.c` and `web.c`): `tg_token` 63, `tg_chat` 23,
`hook_url` 199, `hook_hdr` 159 (`Name: value`), `hook_fmt` (`json` or `text`), `mqtt_url` 127 (`mqtt://` or `mqtts://`),
`mqtt_user` 47, `mqtt_pass` 63, `ntfy_url` 199, `ntfy_tok` 79. An empty value removes the key from the board (`rt_store_set` in `store.c`).

The board does not tell the values of its settings. Thus an empty field on the page keeps the value that is on the
board.

The page checks each value before it sends it:

- No `"` and no `\`. The console cannot carry them in an argument. For a Wi-Fi name or password with these characters,
  use the setup network of the board.
- No control characters.
- Wi-Fi name: 1 to 32 bytes. Wi-Fi password: empty (open network), 8 to 63 characters, or a key of 64 hex digits.
- Page password: 60 characters or fewer.
- Alert values: the lengths above, and the form of the token, chat ID, addresses and header.
- A full command line: 255 bytes or fewer (the line buffer of `console.c`).

The page password applies after a restart. `load_auth()` in `web.c` reads `page_pass` only when the web server starts,
and when the page of the board itself changes the password. Thus the page tells the person to click **Restart the
board**. The alert settings apply at once: `alert set` calls `rt_alert_reload()`.

The page does not offer the room name or the group secret. Use the page of the board for them.

The console log shows each line from the board and each command from the page. The page replaces each value that it
sent for the Wi-Fi password, the page password, `tg_token`, `hook_url`, `hook_hdr`, `mqtt_url`, `mqtt_pass`, `ntfy_url` and `ntfy_tok` with
`[hidden]`. This applies to the commands and to the lines from the board (for values of 4 characters or more). The
model answers (about 4 lines each second) are hidden until the person selects them.

## Demo mode

Add `?demo` to the address, for example `https://ioserviceslabs.github.io/decidio/?demo`. Step 4 then uses a simulated
board (`demo.js`), not a USB device. A label at the top of the page shows the demo mode. The simulated board answers
the commands as the firmware does. The Wi-Fi password `wrongpassword` gives the result "wrong password" (reason 15).
Each other password connects after about 3 seconds. Step 3 is not simulated: **Connect and install** is real.

## What was tested

Tested with Chromium 131 (headless, Playwright) on `http://localhost`:

- The page loads with no console errors. ESP Web Tools loads from unpkg.com.
- The list of ideas comes from `builds/index.json`: 63 ideas for `xiao_sense`, 40 for `s3cam`, with the note that 23
  ideas need a microphone. The search and the selection operate. The arrow keys move the selection.
- The ESP Web Tools element gets the manifest of the selected board and idea, and the manifest loads.
- localStorage holds only the board and the idea.
- In demo mode: connect, board summary, Save Wi-Fi to the state online with the two addresses, Save page password, Save
  alerts, Send test (with and without settings), removal of a destination, and Restart the board. The checks refuse
  bad values. The console log contains no secret that the page sent.
- At a width of 360 px, the page has no horizontal scroll. On a phone (user agent of a Pixel 7), the page shows the
  message and the connect button is off. Reduced motion stops the smooth scroll.

Not tested:

- An install through this page on a real board.
- Step 4 with a real board: the XIAO over USB-Serial-JTAG, and the ESP32-S3-CAM through its USB-UART chip.
- The effect of the DTR and RTS sequence on a real reset circuit.
- The reconnection after a restart on a board whose USB port goes away and comes back.
- Edge, macOS, Linux, and the GitHub Pages workflow.

## Licence of what is in the builds

Each `xiao_sense` build contains the `general` base model. Its sound branch was trained on ESC-50 (CC BY-NC,
non-commercial). The `s3cam` builds contain `general-cam`, which has no sound data. Refer to "Data and licences" in
GUIDE.md, also for the open question about the weather photos.

---

decidio is a product of [IO Services](https://ioservices.io/). Built in the USA.
