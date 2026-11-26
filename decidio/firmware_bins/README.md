# Prebuilt decidio runtime firmware

The folder `esp32s3/` contains one firmware. It operates on each supported board. When the firmware starts, it finds
the camera and the microphone of the board (`firmware/main/board.c`). Thus you do not select a board:

| Board | Console |
|---|---|
| Seeed XIAO ESP32S3 Sense (8 MB flash, camera and PDM microphone) | Native USB |
| ESP32-S3-CAM / ESP32-S3-EYE pinout (16 MB flash, camera, no microphone) | The socket with the label UART, 921600 baud |

The image is for 8 MB of flash. It also operates on boards with more flash.

To write the firmware, type `decidio flash firmware --port COMx`. This command reads the offsets from `flash.json`.
You can also write the firmware with esptool:

```
python -m esptool --chip esp32s3 --port COM5 write-flash 0x0 decidio_rt_merged.bin
```

| File | Offset | Contents |
|---|---|---|
| `bootloader.bin` | 0x0 | The bootloader |
| `partition-table.bin` | 0x8000 | The partition table from `firmware/partitions.csv` |
| `ota_data_initial.bin` | 0xd000 | "Start from the first firmware slot" |
| `decidio_rt.bin` | 0x10000 | The firmware. The page of the board also accepts this file at System > Firmware |
| `decidio_rt_merged.bin` | 0x0 | The four files above in one file |

The firmware operates with the model package that is in the `model` partition. To write a model package, type
`decidio flash model --port COMx` or `decidio install <idea> --port COMx`. The offset is `model_offset` in
`flash.json` (0x510000).

Version: decidio-rt 0.5.0. It was built from the current `firmware/` folder in the `espressif/idf:release-v5.5`
Docker image, with these components:

| Component | Version |
|---|---|
| ESP-IDF | 5.5.6 |
| esp-tflite-micro | 1.4.3 |
| esp-nn | 1.5.0 |
| esp32-camera | 2.1.9 |
| mdns | 1.15.1 |

This firmware operated on a XIAO ESP32S3 Sense (Wi-Fi, the page, the eye). It also operated on an ESP32-S3-CAM
through USB (the detection of the board, the model, the eye).
