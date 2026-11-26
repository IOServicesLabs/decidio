# Security

## Supported versions

| Version | Security fixes |
|---|---|
| 0.4.x (Python tool and firmware decidio-rt 0.4) | Yes |
| Earlier than 0.4 | No |

## Report a vulnerability

Do not open a public issue for a security problem.

1. Open the **Security** tab of https://github.com/IOServicesLabs/decidio.
2. Click **Report a vulnerability**.
3. Write the report. Give this data:

   | Item | Example |
   |---|---|
   | The version of the tool | The output of `decidio --version` |
   | The version of the firmware | The **Status** tab of the board's page, or `info` on the USB console |
   | The board | Seeed XIAO ESP32S3 Sense |
   | The steps that show the problem | One step on each line |
   | The result | What an attacker can read, change or do |

4. Do not put real secrets in the report. Remove Wi-Fi passwords, bot tokens, local keys and group secrets.

We reply in the advisory. We tell you when a fix is available.

## Known limits

The items in this table are limits of the design. They are not vulnerabilities. A report that shows a way past
one of these limits is a vulnerability.

| Limit | What you must do |
|---|---|
| The page of the board uses HTTP. HTTP has no encryption. | Do not make the board available from the internet. |
| Until you set a page password, each person on your network can open the page and change the settings. | Set a page password immediately after the install. |
| The page password goes through your network without encryption. | Use a page password that you use for nothing else. |
| The board keeps the Wi-Fi password and the bot token in its flash memory without encryption. A person who takes the board can read them. | Keep the board in a place that you control. |
| The board keeps the local keys of your devices and the group secret in its flash memory without encryption. | Keep the board in a place that you control. |
| The group secret goes through your network without encryption when you add a board to a group. | Add boards only on a network that you trust. |
| A group password is also the password of the page of each board in the group. The browser sends it through your network without encryption. | Use a group password that you use for nothing else. |
| A group without a group password does not protect the pages of its boards. Each person on your network can open a board that has no page password, and through it change each other board of the group. | Set a group password on the Boards tab. |
| A board in a group sends the names and the states of its rules as ESP-NOW radio messages. They have a signature but no encryption. A radio near your home can read them. | Do not put private data in the name of a rule or of a room. |
| A board in the group can replace the firmware of each other board in the group. | Give the group secret only to boards that you own. |
| A board in the group can change the settings of each other board in the group. The page password does not stop it. | Give the group secret only to boards that you own. |
| A board refuses a copy of an old group message only until the board that sent it restarts. | Give the group secret only to boards that you own. |
| The setup network decidio-xxxx has no password. It stays open until the board connects to your Wi-Fi, and for two minutes after that. During that time, a person near you can set up the board. | Connect the board to your Wi-Fi immediately after the install. |
| The bot token gives full control of the Telegram bot. | Use a Telegram bot that you made for decidio only. |
| Each board sends its name, its room and its firmware version to the local network each 2 seconds. A board in a group also sends the names and states of its rules. | Use a network that you trust. |
| The Live tab and the Eye tab show the camera picture to each person who has the page open. | Set a page password if other persons use your network. |

**WARNING: Do not use decidio as a safety device or as an alarm. The model makes errors. A rule operates on an
error as it does on a correct answer.**

## What leaves the board

| Data | Destination |
|---|---|
| Alert text | Only the destinations that you entered (Telegram, webhook, MQTT, ntfy) |
| A camera picture | Only Telegram and ntfy, and only for a rule where you select **with a photo** |
| Commands to a Smart Life device | That device on your Wi-Fi. They do not go to the internet. |
| Sound | Only ntfy, and only for a rule where you select **with a photo**: the last clip of the microphone (1 second for most models). The **Send test** button of ntfy also sends a picture and a clip. |
| Data on the decidio community server for ntfy | Only if you select it on the **Alerts** tab. The server keeps the alerts, the pictures and the sounds for 24 hours, and 25 MB of files for each internet address (oldest files deleted first). Each person who knows the topic name can read the topic. |

README.md tells you more in the sections "What leaves the board" and "Warnings".
