# Instructions for LLM agents

- To install an idea on an ESP32-S3 board for the user, read [INSTALL_WITH_LLM.md](INSTALL_WITH_LLM.md) and obey it.
- Do not ask for the Wi-Fi password or a bot token. The user types them into the page of the board, into
  `decidio wifi`, or into step 4 of the installer website.
- Use these files for other data:

  | File | Contents |
  |---|---|
  | [README.md](README.md) | The quick install for persons |
  | [GUIDE.md](GUIDE.md) | The reference for the models, for how to train a model, and for the firmware |
  | [USE_CASES.md](USE_CASES.md) | The list of ideas and the fields of a rule |

## Skills

Each folder in [skills/](skills/) has a `SKILL.md` file with the procedure for one task. Before you do the task, read
the file. For Claude Code, copy the folders into `.claude/skills/`. Claude Code then loads each skill by its name.

| Task | Skill |
|---|---|
| Put a ready-made idea on a board | [skills/install-idea](skills/install-idea/SKILL.md) |
| Add a sensor that the firmware does not have (I2C part, switch, PIR) | [skills/add-sensor](skills/add-sensor/SKILL.md) |
| Add an ESP32-S3 camera board that the firmware does not know | [skills/add-board](skills/add-board/SKILL.md) |
| Write a new idea from questions and inputs that exist | [skills/add-idea](skills/add-idea/SKILL.md) |

decidio is a product of [IO Services](https://ioservices.io/). Built in the USA.
