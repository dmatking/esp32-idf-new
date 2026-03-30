# idf-new — ESP-IDF Project Generator

**idf-new** is a command-line tool that generates clean, minimal ESP-IDF projects for specific ESP32 boards. Pick a board, pick optional peripherals and modules, and get a ready-to-build project — without copy-pasting old repos or re-discovering pinouts.

```bash
idf-new my_project --board waveshare/wvshr200_touch --feature tf_card --module gps_neo6m
```

---

## Quick Start

```bash
git clone https://github.com/dmatking/idf-new.git
cd idf-new
python3 -m venv .venv
source .venv/bin/activate
pip install -e idf_new_tool
```

> The `-e` (editable) flag is required — idf-new reads `boards/`, `modules/`, and `idf-templates/` directly from the repo checkout at runtime.

After the initial setup, activate the venv at the start of each session alongside ESP-IDF:

```bash
source ~/esp/esp-idf/export.sh
source ~/idf-new/.venv/bin/activate
```

> On Debian/Raspberry Pi OS, system Python is externally managed and blocks global pip installs. A venv sidesteps this cleanly. If you'd prefer a one-liner without a venv, see [Alternative: system-wide install](#alternative-system-wide-install).

Generate a project:

```bash
idf-new my_project --board generic
cd my_project
idf.py set-target <esp32 | esp32s3 | esp32c3 | etc.>
idf.py build flash monitor
```

If you see serial output from your board, you're up and running.

The `generic` board target builds a minimal FreeRTOS heartbeat project and should run on nearly any ESP32. It's a good smoke test to verify a board powers up, flashes, and runs correctly.

---

## Prerequisites

idf-new assumes a working ESP-IDF development environment.

You'll need:

- **Python 3.11+**
- **ESP-IDF installed** and `idf.py` available in your shell
  (tested with ESP-IDF **v5.5.1**)

Quick sanity check:

```bash
python --version
idf.py --version
```

> If `idf.py` is not found, install ESP-IDF using Espressif's official installer or setup guide for your OS.

idf-new **does not install ESP-IDF for you** — it generates projects that ESP-IDF builds and flashes.

---

## Core Concepts

idf-new is built around three tiers:

**1. Boards** — Hardware-specific wiring, pin definitions, LCD/touch initialization, and display drawing primitives. Every board implements a small C interface (`board_interface.h`).

**2. Board Features** — Optional peripherals that are physically on the board (TF card slot, speaker, buttons, etc.). These are board-specific — a feature only exists if the board has that hardware.

**3. Modules** — Generic off-the-shelf breakout boards you wire up yourself (GPS, sensors, IMUs, etc.). Modules use `menuconfig` for pin selection and work across any board.

Plus the base **Template** — a minimal ESP-IDF project with no hardware assumptions that everything is built on top of.

---

## High-Level Architecture

```
idf-new/
├── boards/           # Board wiring & pin definitions (per-board C files)
│   └── <vendor>/
│       └── <board>/
│           ├── board_impl.c       # Required: implements board_interface.h
│           ├── board.json         # Metadata for CLI listing
│           ├── idf_component.yml  # Component dependencies
│           ├── main.cmake.extra   # Optional CMake additions
│           └── features/          # Optional on-board peripherals
│               └── <name>/        # e.g. tf_card/, speaker/, buttons/
├── modules/          # Generic breakout board drivers
│   └── gps/
│       ├── _common/  # Shared NMEA/UART core (gps.c, gps.h, Kconfig)
│       ├── neo6m/    # u-blox NEO-6M / GY-NEO6MV2
│       └── atgm336h/ # ATGM336H
├── idf-templates/    # Minimal ESP-IDF base project template
└── idf_new_tool/     # Python CLI and generator
```

---

## How Project Generation Works

```bash
idf-new myapp --board waveshare/wvshr200_touch --feature tf_card --module gps_neo6m
```

This command:

1. Copies the base ESP-IDF template
2. Installs the selected board's wiring (`board_impl.c`, helpers, component deps)
3. Applies selected board features (copies `boards/.../features/tf_card/` into the project)
4. Applies selected modules (copies `modules/gps/_common/gps.c`, `gps.h`, merges `Kconfig`)
5. Merges all `idf_component.yml` dependency declarations
6. Updates project name and CMake config

The result is a minimal, fully wired ESP-IDF project ready to build.

---

## Discovering What's Available

```bash
idf-new --list-boards     # All supported boards
idf-new --list-modules    # All generic modules (GPS, sensors, etc.)
```

Board features are board-specific — check `boards/<vendor>/<board>/features/` or the board's `board.json` for what a given board supports.

---

## Adding a New Board

1. Create `boards/<vendor>/<board_name>/`
2. Add `board_impl.c` implementing the functions from `board_interface.h`:
   - **Required:** `board_init()`, `board_get_name()`, `board_has_lcd()`
   - **LCD boards:** `board_lcd_sanity_test()`, `board_lcd_fill()`, plus the display drawing API (`board_lcd_width()`, `board_lcd_height()`, `board_lcd_flush()`, `board_lcd_clear()`, `board_lcd_set_pixel_raw()`, `board_lcd_set_pixel_rgb()`, `board_lcd_pack_rgb()`, `board_lcd_get_pixel_raw()`, `board_lcd_unpack_rgb()`)
   - Weak no-op defaults are provided in `board_defaults.c` for headless boards
3. Optionally add:
   - `board.json` — metadata for `--list-boards`
   - `idf_component.yml` — component dependencies
   - `main.cmake.extra` — CMake additions (extra sources, defines, requires)
   - Additional `.c`/`.h` helper files (copied into generated project automatically)
   - `features/<name>/` — optional on-board peripheral implementations
4. Test: `idf-new test_proj --board <vendor>/<board_name>`

---

## Adding a Board Feature

Board features live under `boards/<vendor>/<board>/features/<name>/` and represent optional peripherals physically on that board (TF card, speaker, buttons, etc.).

1. Create `boards/<vendor>/<board>/features/<name>/`
2. Add any combination of:
   - `.c` / `.h` source files (copied into generated project's `main/`)
   - `Kconfig` — appended to `Kconfig.projbuild`
   - `idf_component.yml` — merged into project dependencies
   - `main.cmake.extra` — appended to project's CMake extras
3. Add `"board_features": ["<name>"]` to the board's `board.json`

Enable via:

```bash
idf-new my_project --board <vendor>/<board> --feature <name>
```

The `--feature` flag is repeatable: `--feature tf_card --feature buttons`

---

## Adding a New Module

Modules are generic drivers for off-the-shelf breakout boards. Pins are configured via `menuconfig`.

1. Create `modules/<category>/<module_name>/` with a `module.json` descriptor
2. Add shared C code under `modules/<category>/_common/` (or directly in the module dir)
3. Add a `Kconfig` with pin and configuration options
4. Create `idf_new_tool/idf_new/modules/<category>.py`:
   - Implement the `Module` protocol (`name`, `flag`, `category`, `apply()`)
   - Call `register()` at module load
   - `apply()` copies source files and merges Kconfig into the project

Enable via:

```bash
idf-new my_project --board generic --module <flag>
```

Use `idf-new --list-modules` to see registered modules.

---

## Installation Notes

idf-new must be installed as an editable install from the repo checkout — it reads `boards/`, `modules/`, and `idf-templates/` from the repo at runtime.

```bash
git clone https://github.com/dmatking/idf-new.git
cd idf-new
python3 -m venv .venv
source .venv/bin/activate
pip install -e idf_new_tool
```

**To update:** just `git pull` in the repo — no reinstall needed.

---

## Alternative: system-wide install

If you don't want to manage a venv, you can install directly into your user Python environment. On Debian/Raspberry Pi OS you'll need to pass `--break-system-packages`:

```bash
pip install -e idf_new_tool --break-system-packages
```

This is safe for a dev tool with a single dependency (PyYAML), but the venv approach above is cleaner and won't interfere with system packages.

---

## Why idf-new Exists

idf-new grew out of repeatedly solving the same ESP-IDF setup problems across many boards and projects.

Rather than maintaining dozens of near-duplicate repos, this tool centralizes:

- board wiring knowledge
- optional peripheral implementations
- generic driver modules
- clean project structure

The goal is to make starting a new ESP32 project **boring** — in the best possible way.

---

## Licensing

All original code in this repository is licensed under the **Apache License 2.0**.

ESP-IDF and bundled third-party libraries are licensed separately under permissive licenses (Apache, MIT, BSD, etc.).

---

Built with:

- Python 3.13.5
- ESP-IDF 5.5.1
- [Claude Code](https://claude.ai/claude-code)

<center> This repository was automatically pushed to GitHub from my self-hosted <a href="https://forgejo.org/">Forgejo</a> server. </center>
