---
name: add-board
description: Add a board profile for an ESP32-S3 camera board that the decidio firmware does not know (a new camera pin layout, microphone pins or memory card pins). Use this skill when the board reports no camera although a camera is connected, or when the user names a board that is not the XIAO ESP32S3 Sense or the ESP32-S3-CAM.
---

# Add a board to decidio

One firmware operates on all boards. When it starts, it tries each camera pin layout that it knows. Then it looks for
a microphone, and it keeps the result. In the source, a board has two parts:

| File | Part |
|---|---|
| `firmware/main/camera.c` | A row in `PINS`: the 14 camera pins |
| `firmware/main/board.c` | A row in `PROFILES`: the camera row, the microphone pins, the memory card pins, the console, and the pins that rules and sensors can use |

In this file, `PY` is the Python program of the decidio environment (refer to INSTALL_WITH_LLM.md, step 2).

## Rules

1. The chip must be an ESP32-S3 with PSRAM (2 MB or more) and 8 MB of flash or more. For other chips, stop: the
   firmware does not operate on them.
2. Use only the pin numbers from the schematic or from the pin table of the manufacturer. Do not use a pin number
   that you are not sure of. An incorrect layout sends signals to pins that belong to other parts of the board.
3. If the user did not make a backup of the board, make a backup before you write to the board:
   `python -m esptool --chip esp32s3 --port <port> read-flash 0 ALL backup.bin`.
4. Tell the user clearly what you ran on the board and what you did not run.

## Steps

### 1. Find what the firmware sees now

Write the firmware (`PY -m decidio flash firmware --port <port>`). Then send `board` on the console. An example of
the answer:

```
{"sensors":{"profile":"s3cam","saved":false,"camera":null,"mic":false,...}}
```

If a camera is connected and the answer has `"camera":null`, no known layout agrees with this board. If the answer
has a camera name, the firmware already supports the board. Stop.

### 2. Find the pin layout

Ask the user for the exact name of the board. Find its schematic or its camera pin table. Many manufacturers give an
Arduino `camera_pins.h` file. Find these items:

| Item | What to find |
|---|---|
| Camera pins | XCLK, SIOD (SDA), SIOC (SCL), D7 to D0 (also named Y9 to Y2), VSYNC, HREF, PCLK |
| Microphone | The type and the pins. The firmware supports only PDM microphones with a clock pin and a data pin |
| Memory card | The pins |
| USB socket | If it connects to the USB of the chip or to a USB-serial chip |

### 3. Add the rows

In `camera.c`, add a row at the end of `PINS`. The `pins_t` line above `PINS` gives the sequence of the numbers:

```c
    { "my-board", <xclk>, <siod>, <sioc>, <d7>, <d6>, <d5>, <d4>, <d3>, <d2>, <d1>, <d0>, <vsync>, <href>, <pclk> },
```

In `board.c`, add a row at the end of `PROFILES`:

```c
    { .name = "my-board", .cam_map = <its row number in PINS, from 0>, .orient = -1, .mic_clk = -1, .mic_din = -1,
      .uart_console = <true if the USB socket goes to a USB-serial chip> },
```

| Field | Value |
|---|---|
| `.mic_clk`, `.mic_din` | The PDM microphone pins, or -1 |
| `.card_bus`, `.card_pin` | Refer to the two rows that have these fields (`xiao_sense` and `s3cam`) |
| `.pins` | One bit for each pin that rules and sensors can use. If you do not set this field, rules and sensors can use each pin that the board does not use |
| `.orient` | -1 lets the firmware select. If the picture on the Live tab is upside down, try 0 to 3 |

When the firmware starts, it finds if the board has a USB-serial chip: such a chip holds the receive pin high. Then
the firmware tries only the profiles whose `.uart_console` agrees. Thus it does not try a layout that belongs to the
other family of boards.

### 4. Build the firmware, write it, and examine the board again

1. Build and write the firmware (`skills/add-sensor/SKILL.md`, step 6).
2. Send this command on the console:

   ```
   board auto
   ```

3. Restart the board.
4. Send `board` again. The answer must show the new profile and a camera name (for example `OV2640`).
5. If the answer does not show them, send `board my-board` to force the profile. Then read the start messages to
   find the camera error.

### 5. Do a check

- The Live tab shows the camera picture with the correct side up.
- `info` shows `"mic":true` only if the board has a microphone. If the board has a microphone, speak near it. The
  sound questions on the Live tab must change.
- Install an idea (`skills/install-idea/SKILL.md`). Make sure that the model answers.

## What to tell the user at the end

Tell the user the profile name and what the board reported. Also tell the user what you saw on the Live tab and
what you did not test (for example the memory card). Ask the user to send the two rows to the project. Then other users can also use them.
