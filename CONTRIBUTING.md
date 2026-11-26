# Contributing

## Report a problem

Open an issue. Give this data:

| Item | Example |
|---|---|
| The command or the step | `decidio install knock --port COM5` |
| The output | Copy the full output of the command |
| The result that you expected | "The command shows `installed`" |
| The version of the tool | The output of `decidio --version` |
| The version of the firmware | The **Status** tab of the board's page |
| The board | Seeed XIAO ESP32S3 Sense |
| The computer | Windows 11, Python 3.12 |

**CAUTION: Do not put a Wi-Fi password, a bot token, a local key or a group secret in an issue. An issue is public.**

For a security problem, do not open an issue. Do the procedure in [SECURITY.md](SECURITY.md).

## Set up a development environment

You must have Python 3.10, 3.11 or 3.12 and Git.

1. Clone the repository:

   ```
   git clone https://github.com/IOServicesLabs/decidio.git
   cd decidio
   ```

2. Make a Python environment:

   ```
   python -m venv .venv
   .venv\Scripts\activate                 (Linux and macOS: source .venv/bin/activate)
   ```

3. Install the tool. Select the line for your work:

   | Work | Command |
   |---|---|
   | The install path, the recipes, the builds, the firmware | `pip install --no-deps -e .` then `pip install -r requirements-install.txt pytest` |
   | Teach, train or export a model (installs TensorFlow) | `pip install -e ".[test]"` |

To keep TensorFlow on the processor and off the graphics card, set `CUDA_VISIBLE_DEVICES=-1`.

## Run the tests

1. Type this command:

   ```
   python -m pytest -rs tests
   ```

2. Read the list of skipped tests. Each skipped test shows what it needs.

| Test | It needs |
|---|---|
| `tests/test_recipes.py`, `tests/test_builds.py` | Python only |
| `tests/test_base_projects.py` | Python only. One test needs TensorFlow |
| `tests/test_mel_parity.py`, `tests/test_ego_parity.py` | `gcc` |
| `tests/test_rules.py` | `gcc` and the cJSON source (`CJSON_DIR`) |
| `tests/test_model_loader.py` (a script) | `gcc` and the cJSON source |
| `tests/test_tflm_parity.py` (a script) | TensorFlow, `tflite-micro` and a project with its dataset |

The C tests compile files from `firmware/main/` for the computer. The CI runs them on Ubuntu. On Windows, run them
in WSL or in a Linux container.

To run the C tests:

1. Get the cJSON source. The firmware uses cJSON 1.7.19 from ESP-IDF 5.5:

   ```
   git clone --depth 1 --branch v1.7.19 https://github.com/DaveGamble/cJSON.git ../cJSON
   ```

2. Set the variable:

   ```
   export CJSON_DIR=$PWD/../cJSON
   ```

3. Run the tests:

   ```
   python -m pytest -rs tests
   python tests/test_model_loader.py decidio/base/general "$CJSON_DIR"
   python tests/test_model_loader.py decidio/base/general-cam "$CJSON_DIR"
   ```

`tests/test_ego_parity.py` uses random pictures. To use real photos, set `VOC2012_ZIP` to the path of
`VOCtrainval_11-May-2012.zip`.

## Change an idea

The ideas are in `recipes/*.yaml`. Each idea must also have a row in `USE_CASES.md`.

1. Change the file in `recipes/`.
2. Type this command. It checks each recipe against the base models and writes `decidio/recipes.json` and
   `firmware/main/recipes.json`:

   ```
   python tools/make_builds.py
   ```

3. Run the tests.
4. Add the two `recipes.json` files to your commit.

The firmware contains `firmware/main/recipes.json` for the **Ideas** tab of the board's page. After you change it,
rebuild the firmware.

## Rebuild the firmware

Rebuild the firmware only after you change `firmware/`. The prebuilt files are in `decidio/firmware_bins/esp32s3/`.
One firmware operates on each supported board.

### With ESP-IDF on your computer

1. Install ESP-IDF 5.5.
2. Open the ESP-IDF 5.5 shell.
3. Activate your Python environment.
4. Type this command:

   ```
   decidio build-firmware
   ```

The command writes the files to `decidio/firmware_bins/esp32s3/`.

### With Docker

Use this method on Linux, macOS or WSL. The prebuilt firmware of 0.5.2 was made with this method (ESP-IDF 5.5.6).

1. Type this command in the top folder of the repository:

   ```
   docker run --rm -v "$PWD/firmware:/project" -w /project -e HOME=/tmp -u "$(id -u):$(id -g)" \
     espressif/idf:release-v5.5 \
     idf.py -B build_esp32s3 -D SDKCONFIG=sdkconfig.esp32s3 \
     -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3" build
   ```

2. Copy the files to `decidio/firmware_bins/esp32s3/`:

   ```
   python -c "from pathlib import Path; from decidio.firmware import BINS, collect_bins; collect_bins(Path('firmware/build_esp32s3'), BINS / 'esp32s3')"
   ```

### After a rebuild

1. Run the tests. `tests/test_builds.py` checks the firmware files against `firmware/partitions.csv`.
2. Write the new versions in `decidio/firmware_bins/README.md` (ESP-IDF and the components).
3. Write the firmware to a board:

   ```
   decidio flash firmware --port COM5
   ```

4. Make sure that the board starts and that the **Status** tab shows the camera and the microphone.
5. Write in the pull request what you tested and on which board.

## Make the installer builds

The browser installer is `installer/index.html`. Its builds are generated files of approximately 50 MB. Do not add
them to the repository.

1. Type this command. It writes `installer/builds/`:

   ```
   python tools/make_builds.py
   ```

2. Serve the installer on your computer:

   ```
   python -m http.server -d installer 8000
   ```

3. Open `http://localhost:8000` in Chrome or Edge.

The workflow `.github/workflows/pages.yml` makes the builds again and publishes the installer at
https://ioserviceslabs.github.io/decidio/ after each push to `main`.

## Write documentation

Use these rules for each Markdown file and for each text that the tool or the page shows:

| Rule | Example |
|---|---|
| Write short sentences. Use no more than 20 words in an instruction and 25 words in a description. | "The board keeps the rule after a restart." |
| Write one action in each step. | "Click **Save**." |
| Start an instruction with a verb. | "Connect the board to the computer." |
| Use the active voice. | "The board sends the alert." Not: "The alert is sent." |
| Use simple, common words. Use one word for one thing. | Use "board" each time. Do not change to "device" or "module". |
| Do not use marketing words. | Do not write "powerful", "seamless" or "easy". |
| Put data in tables. | Values, options, results, symptoms and remedies |
| Put a warning or a caution before the step that it applies to. | **WARNING** for a risk to persons. **CAUTION** for a risk to equipment or data. |
| Give measured values. Tell what was not tested. | "15 to 20 pictures each second, measured on two boards." |
| Use example names, not real ones. | `COM5`, `decidio-xxxx`, `MyNetwork` |

Do not write a personal name, an e-mail address, a private network address or a path on your computer.

## Send a change

1. Fork the repository.
2. Make a branch.
3. Make one change with one purpose.
4. Add a test for a change of behavior.
5. Run the tests.
6. Add a line to `CHANGELOG.md` below "Unreleased".
7. Open a pull request. Tell what you changed, why, and what you tested.

## Data and licences

The code is under the Apache License 2.0. When you send a change, you agree that your change has the same licence.

Do not add dataset files (photos, sounds) to the repository. If you train a base model on new data, the licence of
that data must permit you to publish the model. Write the data and its licence in `NOTICE` and in the section
"Data and licences" of `GUIDE.md`.
