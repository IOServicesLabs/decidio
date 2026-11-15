---
name: add-sensor
description: Add a sensor to the decidio firmware for an ESP32-S3 board (an I2C part, a switch, a PIR, an analog part). Its value then shows on the page of the board, and rules can test it. Use this skill when the user wants the board to read hardware that decidio does not read yet.
---

# Add a sensor to decidio

The firmware reads a camera, a microphone and the radio by itself. Each other sensor is one C file in
`firmware/main/sensors/`. The file gives the firmware one or more named numbers ("inputs"). An input shows on the
Live tab of the page of the board, and a rule can test it. You do not change other files.

In this file, `PY` is the Python program of the decidio environment (refer to INSTALL_WITH_LLM.md, step 2).

## Rules

1. Do not use a pin that the board uses. Ask the board (step 2). Do not use the pins of the camera, the microphone,
   the memory card, the console or USB.
2. **CAUTION: Do not connect a part with 5 V signals. The pins use 3.3 V. A signal of 5 V causes damage to the
   board.** Tell this to the user. If the part operates only at 5 V, stop.
3. Do not ask for the Wi-Fi password or other secrets.
4. Tell the user clearly what you tested and what you did not test. A sensor file that compiles is not a tested sensor.
5. To add a sensor, do not change `rules.c`, `inputs.c` or the page. If you think that you must change them, stop and
   tell the user why.
6. A switch, a button, a reed contact or a PIR output operates without new code. Use an input pin (step 3).

## Steps

### 1. Find what the part is

Ask the user for the part name (for example SHT31, BME280, VL53L0X, HC-SR501). Ask the user how the part is
connected. In the datasheet, find the supply voltage, the bus (I2C, one pin, analog) and the I2C address.

### 2. Ask the board which pins are free

Send the console command `info` on the serial port (115200 baud on a XIAO, 921600 baud on a board with a USB-serial
chip). The answer has `"pins":[...]`: a sensor can use these pins on this board. The `sensors` field gives the board
type. The same list is in the "Pin" selection of a rule on the Ideas tab.

| Board | Pins |
|---|---|
| XIAO ESP32S3 Sense | D0 to D5 are GPIO 1 to 6. D8, D9 and D10 are GPIO 7, 8 and 9 (the memory card also uses them). GPIO 21 is the LED |
| ESP32-S3-CAM | Use the pins from the `pins` list that the board has on its header |

### 3. A part with one on/off output: use an input pin

On the page of the board:

1. Open the **System** tab.
2. Below **Pins and add-on sensors**, type the GPIO number in **Use a pin as an input (GPIO)**.
3. Select the pull in **Pull**.
4. Click **Add input pin**.
5. Restart the board.

On the console, send `pin in <gpio> up` (a switch to ground) or `pin in <gpio> down` (a 3.3 V signal, for example
a PIR). Then restart the board.

The input has the name `pin<gpio>` and the value 0 or 1. Go to step 7.

### 4. An I2C part: make sure that it answers

Tell the user to connect SDA, SCL, 3.3 V and ground. Then send this command:

```
i2c scan <sda gpio> <scl gpio>
```

The answer lists the addresses, for example `{"sda":5,"scl":6,"found":["0x44"]}`. The System tab has the same
function. If the list is empty, do these steps:

1. Examine the wires and the supply.
2. Interchange the SDA wire and the SCL wire.
3. Make sure that the lines have pull-up resistors.

Do not continue until the address from the datasheet is in the list.

### 5. Write the sensor file

Copy `firmware/main/sensors/_template.c` to `firmware/main/sensors/<part>.c`. The name must not start with `_`.
Read `chip_temp.c` in the same folder: it is a complete sensor that operated on a board.

- Set the pins and the address.
- Change `measure()` to the commands and the conversion from the datasheet. `rt_i2c_write`, `rt_i2c_read` and
  `rt_i2c_read_reg` are in `rt.h`. The I2C bus is slow: a 6-byte read takes approximately 2 ms. Read the part a
  maximum of a few times each second, in the task of the file. Keep the last value.
- Register one input for each number: `rt_input_register("name", "unit", low, high, getter)`. The name must have
  only letters, digits and `_`. It must not be the name of a model question. Low and high are the usual range. The
  page uses them for the bar.
- A getter must return immediately. If there is no value, it returns `NAN`.
- Keep `RT_SENSOR(<start function>)` on the last line.

### 6. Build and write the firmware

For the build, ESP-IDF 5.5 is necessary. Type these commands in an ESP-IDF shell:

```
PY -m decidio build-firmware
PY -m decidio flash firmware --port <port>
```

If you have Docker and no ESP-IDF, run `idf.py -B build -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3" build`
in `firmware/` in the image `espressif/idf:release-v5.5`. Then upload `build/decidio_rt.bin` on the System tab of the
page of the board, below **Firmware**.

`flash firmware` keeps the model, the rules and all settings.

### 7. Do a check

Send `inputs` on the console, or open the Live tab (**Other inputs**). The new input must be in the list. Its value
must change when the user changes what the sensor measures (for example, the user makes the part warm or pushes the
switch). If the input is not in the list, the start function did not find the part. Do step 4 again.

### 8. Use the input in a rule

On the page of the board:

1. Open the **Ideas** tab.
2. Below **Add your own rule**, select the input.
3. Select "at least" or "below".
4. Type a threshold.

In a recipe file, write `when: in:<name> > 25`. To make an idea with the input, obey `skills/add-idea/SKILL.md`.

## What to tell the user at the end

- The name of the input and where it shows.
- What you measured in step 7, and what you did not test (for example the accuracy of the numbers).
- That a rule on a sensor is not a safety device.
