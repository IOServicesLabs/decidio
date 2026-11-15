---
name: install-idea
description: Put a ready-made decidio idea on the ESP32-S3 camera board of the user through USB. Find the board, select the idea, write the firmware and the model, and give the user the Wi-Fi and alert steps. Use this skill when the user wants to flash a board with decidio or set up a board with decidio.
---

# Install a decidio idea

The complete procedure is in [INSTALL_WITH_LLM.md](../../INSTALL_WITH_LLM.md), in the top folder of the project.
Read that file now and obey it step by step. This file gives only the rules that you must obey at all times.

1. Do not ask for the Wi-Fi password, the Telegram bot token or other secrets. Do not put them in a command.
   The user types them into the page of the board, into the prompt of `decidio wifi`, or into step 4 of the
   installer website.
2. To write to the board, use only the `decidio` commands. Do not run `esptool erase-flash`.
3. Write to one board only. If there is more than one port, ask the user which port to use.
4. Before the install, tell the user that it replaces the firmware and the model on the board. Continue only after
   the user agrees.
5. Do not tell the user that an idea is a safety device or an alarm.
6. You do not have to know which board it is. `decidio install <idea> --port <port>` asks the board for its camera
   and microphone, and selects the model. Tell the user what the board reported.
7. If a step fails two times, stop and tell the user.

Related tasks:

| Task | Skill |
|---|---|
| A sensor that the firmware does not read | `../add-sensor/SKILL.md` |
| A board that the firmware does not know | `../add-board/SKILL.md` |
| A new idea | `../add-idea/SKILL.md` |
