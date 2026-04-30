# esp32-idf-new — ESP-IDF Project Generator

**idf-new** is a command-line tool that generates clean, minimal ESP-IDF projects for
specific ESP32 boards. Pick a board, pick optional peripherals and modules, and get a
ready-to-build project — without copy-pasting old repos or re-discovering pinouts.

```bash
idf-new my_project --board waveshare/wvshr185_round_touch --feature tf_card --sim
```

---

## Supported Boards

| Board                                | Chip      | Display                         | Interface | Touch              | Board ID                                     |
| ------------------------------------ | --------- | ------------------------------- | --------- | ------------------ | -------------------------------------------- |
| ESP32‑P4 Function EV Board           | ESP32‑P4  | EK79007 4.3" IPS 1024×600       | MIPI-DSI  | GT911              | `espressif/esp32p4_function_ev`              |
| M5Stack Tab5                         | ESP32‑P4  | ST7123 5.0" IPS 720×1280        | MIPI-DSI  | ST7123 (combined)  | `m5stack/tab5`                               |
| Waveshare P4 WiFi6 Touch LCD 4B      | ESP32‑P4  | ST7703 4.0" IPS 720×720         | MIPI-DSI  | GT911              | `waveshare/wvshr_p4_720_touch`               |
| LilyGO T4 S3 AMOLED touch            | ESP32‑S3  | RM690B0 2.41" AMOLED 450×600    | QSPI      | CST226             | `lilygo/t4s3_amoled_touch`                   |
| LilyGO T-Display S3 AMOLED touch     | ESP32‑S3  | RM67162 1.91" AMOLED 536×240    | QSPI      | CST816S            | `lilygo/tdisp191_amoled_touch`               |
| Waveshare Round Touch LCD 1.85"      | ESP32‑S3  | ST77916 1.85" IPS 360×360 round | QSPI      | CST816             | `waveshare/wvshr185_round_touch`             |
| Waveshare Round LCD 1.85"            | ESP32‑S3  | ST77916 1.85" IPS 360×360 round | QSPI      | —                  | `waveshare/wvshr185_round`                   |
| Waveshare Touch LCD 2.0"             | ESP32‑S3  | ST7789 2.0" IPS 240×320         | SPI       | CST816S            | `waveshare/wvshr200_touch`                   |
| Waveshare LCD 2.0"                   | ESP32‑S3  | ST7789 2.0" IPS 240×320         | SPI       | —                  | `waveshare/wvshr200`                         |
| ESP32‑S3-DevKitC + Newhaven 2.4" IPS | ESP32‑S3  | ST7789 2.4" IPS 240×320         | i80 8-bit | —                  | `espressif/esp32s3_devkitc_nhd240320_st7789` |
| HackerBox 107 round                  | ESP32‑S3  | GC9A01 1.28" IPS 240×240 round  | SPI       | —                  | `hackerbox/hb107_round128`                   |
| CYD 2.8" ILI9341 resistive touch     | ESP32     | ILI9341 2.8" TN 240×320         | SPI       | XPT2046 (resistive)| `cyd/cyd28_ili9341_touch`                    |
| CYD 3.5" ST7796 resistive touch      | ESP32     | ST7796 3.5" IPS 320×480         | SPI       | XPT2046 (resistive)| `cyd/cyd35_st7796_touch`                     |
| Generic (any ESP32, no display)      | any       | —                               | —         | —                  | `generic`                                    |

Run `idf-new --list-boards` to see what's available in your checkout.

---

## Quick Start

```bash
git clone https://github.com/dmatking/esp32-idf-new.git
cd esp32-idf-new
python3 -m venv .venv
source .venv/bin/activate
pip install -e idf_new_tool
```

> The `-e` (editable) flag is required — idf-new reads `boards/`, `modules/`, and
> `idf-templates/` directly from the repo checkout at runtime.

After the initial setup, activate the venv at the start of each session alongside ESP-IDF:

```bash
source ~/esp/esp-idf/export.sh
source ~/esp32-idf-new/.venv/bin/activate
```

Generate a project and flash it:

```bash
idf-new my_project --board generic
cd my_project
idf.py set-target <esp32 | esp32s3 | esp32c3 | etc.>
idf.py build flash monitor
```

The `generic` board builds a minimal FreeRTOS heartbeat project and runs on nearly any
ESP32. It's a good smoke test to verify a board powers up, flashes, and runs correctly.

**To update:** just `git pull` in the repo — no reinstall needed.

---

## Prerequisites

idf-new assumes a working ESP-IDF development environment.

You'll need:

- **Python 3.11+**
- **ESP-IDF installed** and `idf.py` available in your shell
  (ESP-IDF **v5.5.3+** recommended; v6.x also works)

Quick sanity check:

```bash
python --version
idf.py --version
```

idf-new **does not install ESP-IDF for you** — it generates projects that ESP-IDF
builds and flashes.

---

## Core Concepts

idf-new is built around three tiers:

**1. Boards** — Hardware-specific wiring, pin definitions, LCD/touch initialization,
and display drawing primitives. Every board implements a small C interface
(`board_interface.h`).

**2. Board Features** — Optional peripherals that are physically on the board (TF card
slot, speaker, buttons, etc.). These are board-specific — a feature only exists if the
board has that hardware.

**3. Modules** — Generic off-the-shelf breakout boards you wire up yourself (GPS,
sensors, etc.). Modules use `menuconfig` for pin selection and work across any board.

Plus the base **Template** — a minimal ESP-IDF project with no hardware assumptions
that everything is built on top of.

---

## Novel features

### 1. Dual entry point: standalone CLI + native `idf.py` extension

`idf-new` installs as both a standalone command and as a native `idf.py` extension via
the `idf_extension` entry-point API (ESP-IDF v6.0+). After installation the same
operations are available in two forms:

```bash
idf-new --list-boards                          # standalone
idf.py idfnew-boards                          # inside any IDF v6.0+ environment
idf.py idfnew-create --name blink --board m5stack/tab5
```

The extension is registered via `pyproject.toml` and requires no IDF patches.

### 2. Board catalog with tag search

Every board is described by a `board.json` alongside its C implementation. The tool
can list and filter boards by hardware traits:

```bash
idf-new --list-boards
idf-new --list-tags
idf-new --find touch
idf-new --find amoled
idf-new --find round
```

Board metadata includes display technology, resolution, shape, touch capability, and
a list of optional on-board peripherals.

### 3. Board feature system

Boards declare optional peripherals as *features*. A feature is a sub-directory under
`boards/<vendor>/<board>/features/<name>/` and may contain:

- C/C++/header source files
- A `Kconfig` fragment (merged into `Kconfig.projbuild`)
- A `main.cmake.extra` snippet (appended to the project's CMake)
- An `idf_component.yml` manifest (merged with the project's manifest)

```bash
idf-new my_proj --board hackerbox/hb107_round128 --feature tf_card
```

The `--feature` flag is repeatable: `--feature tf_card --feature buttons`

Available features for each board are listed in `idf-new --list-boards`.

### 4. Board HAL abstraction (`board_interface.h`)

Every board implements a common C interface:

```c
void board_init(void);
bool board_has_lcd(void);
void board_lcd_fill(uint16_t color);
void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void board_lcd_flush(void);
int  board_lcd_width(void);
int  board_lcd_height(void);
```

Headless boards receive automatic no-op defaults from `board_defaults.c`. Your
`main.c` uses only `board_interface.h` and is board-portable.

### 5. Desktop simulator module (`--sim`)

Adds a `sim/` directory that builds a native SDL2 binary replaying the LCD framebuffer
on a desktop. The module reads the target board's exact pixel dimensions from its
`board.json` and generates a sized CMake configuration automatically. Your rendering
code runs unchanged — the same `board_interface.h` calls that drive the hardware are
backed by a native SDL2 framebuffer in the sim.

```bash
idf-new my_proj --board waveshare/wvshr185_round --sim
cd my_proj/sim && mkdir build && cd build && cmake .. && make
./my_proj_sim                                   # live SDL2 window
./my_proj_sim --screenshot out.png --frames 1  # headless PNG
```

Only meaningful for LCD boards — the module skips itself with a notice on headless
targets.

### 6. Pluggable module system

Modules are Python classes that self-register at import time. Adding a new module
means dropping a file into `idf_new_tool/idf_new/modules/` — no registration table
to update. The CLI auto-discovers all modules and adds their `--flag` to the argument
parser dynamically.

Built-in modules:

| Flag             | Description                                        |
| ---------------- | -------------------------------------------------- |
| `--sim`          | SDL2 desktop simulator with board-aware dimensions |
| `--gps-neo6m`    | u-blox NEO-6M GPS over UART                        |
| `--gps-atgm336h` | ATGM336H GPS over UART                             |

### 7. Component manifest merging

Board, feature, and module manifests are merged (not overwritten) into the project's
`idf_component.yml`. Dependencies accumulate correctly across multiple boards, features,
and modules applied to the same project.

---

## How Project Generation Works

```bash
idf-new myapp --board waveshare/wvshr200_touch --feature tf_card --gps-neo6m
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

## Usage

```bash
# List available boards
idf-new --list-boards

# Filter by tag
idf-new --list-tags
idf-new --find touch
idf-new --find amoled
idf-new --find round

# Create a project
idf-new my_project --board waveshare/wvshr185_round_touch

# Include a board feature
idf-new my_project --board hackerbox/hb107_round128 --feature tf_card

# Include multiple features + a GPS module
idf-new my_project --board waveshare/wvshr185_round --feature tf_card --gps-neo6m

# Add a desktop simulator
idf-new my_project --board waveshare/wvshr185_round --sim

# Pick a board interactively
idf-new my_project --interactive
```

### Via idf.py extension (v6.0+)

```bash
idf.py idfnew-boards
idf.py idfnew-boards --find touch
idf.py idfnew-find --tag amoled
idf.py idfnew-modules
idf.py idfnew-create --name blink --board m5stack/tab5
idf.py idfnew-create --name myapp --board waveshare/wvshr200_touch --feature tf_card
```

> **Note:** `idfnew-create` uses `--name` rather than a positional project name.
> idf.py's click `chain=True` mode would interpret a bare positional argument as the
> next chained command, so `--name` is required.

---

## Desktop Simulator

For boards with an LCD, `--sim` adds a native desktop build that renders your display
code in an SDL2 window — no hardware needed.

```
my_project/
├── main/             # Your ESP-IDF app code (runs on device)
└── sim/
    ├── CMakeLists.txt
    ├── main_sim.c    # Desktop entry point
    └── screencap/    # git submodule — esp32-screencap library
```

### Building and running

```bash
cd my_project/sim
mkdir build && cd build
cmake .. && make
./my_project_sim                                  # interactive SDL2 window
./my_project_sim --screenshot out.png --frames 1  # headless PNG capture
```

**Prerequisites:** `libsdl2-dev` (Debian/Ubuntu: `sudo apt install libsdl2-dev`)

### Interactive mode

- Live SDL2 window with integer-scaled preview
- Press **P** to save a screenshot (`screenshot_NNNN.png`)
- **Esc** or close the window to quit

### Headless mode

Works over SSH with no display server — useful for CI or headless Pis:

```bash
./my_project_sim --screenshot summary.png --frames 1
```

`--frames N` runs N iterations of your render loop before saving. This lets you
capture specific screens if your app cycles through multiple views.

### How it works

The sim reuses your project's rendering code through `board_interface.h` — the same
HAL that idf-new scaffolds for every LCD board. A generic `board_interface_sim.c`
(provided by esp32-screencap) implements the HAL against an in-memory framebuffer.
SDL2 displays it on screen; `stb_image_write` saves it as PNG.

Your drawing code doesn't know or care whether it's running on an ESP32 or a desktop —
it just calls `board_lcd_set_pixel_rgb()`, `board_lcd_flush()`, etc.

### X11 forwarding

To view the SDL2 window from a remote Pi on your local machine:

```bash
ssh -X user@pi
cd my_project/sim/build && ./my_project_sim
```

On Windows, WSL2 with WSLg handles X11 forwarding natively.

---

## Using as an idf.py Extension (ESP-IDF v6.0+)

ESP-IDF v6.0 supports Python package entry-point extensions that add subcommands
directly to `idf.py`. Install idf-new into the v6.0 Python venv:

```bash
/home/$USER/.espressif/tools/python/v6.0/venv/bin/pip install -e /path/to/esp32-idf-new
```

Find the venv path with:

```bash
eim run "python -c 'import sys; print(sys.prefix)'" v6.0
```

### Verify

```bash
eim run "idf.py --help" v6.0   # idfnew-* commands should appear in the list
```

Or after activating the v6.0 environment:

```bash
. ~/.espressif/tools/activate_idf_v6.0.sh
idf.py --help
```

---

## Adding a board

1. Create `boards/<vendor>/<board_id>/board.json` with display metadata.
2. Implement `board_impl.c` against `board_interface.h`.
   - **Required:** `board_init()`, `board_get_name()`, `board_has_lcd()`
   - **LCD boards:** also implement the display drawing API
   - Weak no-op defaults are provided in `board_defaults.c` for headless boards
3. Add an `idf_component.yml` listing any component registry dependencies.
4. Optionally add `sdkconfig.defaults`, `main.cmake.extra`, and a `components/`
   directory for vendored components.
5. Create `boards/<vendor>/<board_id>/features/<name>/` sub-directories for optional
   peripherals.

The board is auto-discovered — no registration step required.

---

## Adding a board feature

Features live under `boards/<vendor>/<board>/features/<name>/` and represent optional
peripherals physically on the board. Add any combination of:

- `.c` / `.h` source files (copied into generated project's `main/`)
- `Kconfig` — appended to `Kconfig.projbuild`
- `idf_component.yml` — merged into project dependencies
- `main.cmake.extra` — appended to project's CMake extras

Add `"board_features": ["<name>"]` to the board's `board.json`, then enable via:

```bash
idf-new my_project --board <vendor>/<board> --feature <name>
```

---

## Adding a module

1. Create `idf_new_tool/idf_new/modules/<name>.py`.
2. Define a class with `name`, `flag`, `category`, and `apply(ctx)`.
3. Call `register(MyModule())` at module scope.

The module is auto-registered and its `--flag` appears in `idf-new --help`
automatically. Pins and configuration should be exposed via a `Kconfig` fragment
so users can configure them with `menuconfig`.

---

## Project structure

```
esp32-idf-new/
├── boards/                   Board HAL implementations and metadata
│   ├── <vendor>/<board>/
│   │   ├── board.json        Display and hardware metadata
│   │   ├── board_impl.c      board_interface.h implementation
│   │   ├── idf_component.yml Component manager dependencies
│   │   ├── sdkconfig.defaults
│   │   ├── main.cmake.extra  (optional) extra CMake for the main component
│   │   ├── components/       (optional) vendored components
│   │   └── features/<name>/  (optional) board-local optional peripherals
├── idf-templates/base_project/   Base project template
│   └── main/
│       ├── board_interface.h     Common board API
│       ├── board_defaults.c      No-op defaults for headless boards
│       └── main.c                Entry point using only board_interface.h
├── idf_new_tool/idf_new/     Python package
│   ├── cli.py                Standalone idf-new CLI
│   ├── idf_new_ext.py        idf.py extension (idfnew-* commands)
│   ├── generator.py          Orchestrates project generation
│   ├── project.py            File copy and manifest merge operations
│   ├── boards.py             Board discovery and metadata loading
│   ├── manifest.py           idf_component.yml merge logic
│   └── modules/              Auto-registered module plugins
├── modules/                  Shared C source for GPS and sim modules
└── tests/                    Unit, integration, and IDF build tests
```

---

## Installation notes

idf-new must be installed as an editable install from the repo checkout — it reads
`boards/`, `modules/`, and `idf-templates/` from the repo at runtime.

```bash
git clone https://github.com/dmatking/esp32-idf-new.git
cd esp32-idf-new
python3 -m venv .venv
source .venv/bin/activate
pip install -e idf_new_tool
```

**To update:** just `git pull` — no reinstall needed.

### Alternative: system-wide install

If you don't want to manage a venv, install directly into your user Python environment.
On Debian/Raspberry Pi OS you'll need `--break-system-packages`:

```bash
pip install -e idf_new_tool --break-system-packages
```

---

## Tests

```bash
pytest                         # unit + integration
pytest -m build                # slow IDF build tests (requires IDF_PATH)
```

---

## Why idf-new exists

idf-new grew out of repeatedly solving the same ESP-IDF setup problems across many
boards and projects. Rather than maintaining dozens of near-duplicate repos, this tool
centralizes board wiring knowledge, optional peripheral implementations, and generic
driver modules in one place. The goal is to make starting a new ESP32 project
**boring** — in the best possible way.

---

## License

Apache-2.0. ESP-IDF and bundled third-party libraries are licensed separately under
permissive licenses (Apache, MIT, BSD, etc.).
