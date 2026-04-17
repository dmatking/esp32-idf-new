# idf-new — Claude Code Project Context

`idf-new` is a CLI scaffolding tool that generates ESP32 IDF projects pre-wired to a
specific board. It is not a firmware project itself — it generates firmware projects.

## How it works

```bash
idf-new /path/to/output --board <vendor>/<board_name>
```

This creates a ready-to-build IDF project with the board's `board_impl.c` wired in,
`sdkconfig.defaults` merged, and component dependencies declared.

## Board discovery

A board is registered by the **presence of `board_impl.c`** in its directory.
The tool scans `boards/` recursively for this file. Path structure:

```
boards/<vendor>/<board_name>/
├── board_impl.c       ← presence of this file = board is registered
├── board.json         ← metadata (display_name, panel, traits, screen)
├── idf_component.yml  ← IDF + component dependencies
├── sdkconfig.defaults ← build config (target, PSRAM, console, WiFi)
└── components/        ← vendor drivers not in IDF registry (optional)
```

The board ID used in `--board` is `<vendor>/<board_name>` — the two path components
under `boards/`.

## Board interface contract

Every `board_impl.c` must implement all functions declared in:

```
idf-templates/base_project/main/board_interface.h
```

These include: `board_init`, `board_get_name`, `board_has_lcd`, `board_lcd_width/height`,
`board_lcd_flush`, `board_lcd_clear`, `board_lcd_fill`, pixel read/write, pack/unpack RGB,
and `board_lcd_sanity_test`.

Weak no-op defaults are provided for `board_lcd_sanity_test` and all drawing functions
for headless (no-display) boards via `board_defaults.c` in the base template.

## Building projects

Always use the `idf` wrapper, not `idf.py` directly:

```bash
idf build
idf set-target esp32p4
idf flash
idf monitor
```

The wrapper sources the correct IDF version automatically. To pin a project to a specific
IDF version, create `.idf-version` in the project root (e.g. `5.4.2`). Default is 5.5.3.

## Vendor files

Vendor files live at `~/hardware/vendors/<BoardName>/`. Always check for a `CLAUDE.md`
in the vendor directory — the user puts key notes there (display specs, IDF version, gotchas).

## Adding a new board

Use the `/newboard` skill. It drives the full workflow: research → branch → generate files
→ build validation loop → commit.

```
/newboard
```

The skill will ask for the board ID and vendor directory if not already provided.

## Existing boards (reference implementations)

| Board | Target | Display | Interface | Notes |
|-------|--------|---------|-----------|-------|
| `waveshare/wvshr_p4_720_touch` | esp32p4 | ST7703 720×720 | MIPI-DSI 2-lane | Best template for P4 MIPI-DSI boards |

## Key implementation notes

- **PSRAM**: All P4 boards require `CONFIG_SPIRAM=y` and `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`
  in `sdkconfig.defaults`. After `idf set-target`, verify PSRAM config is not reset.
- **Console**: P4 boards must use `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` +
  `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`. Do not use USB CDC as primary.
- **Byte order**: Always verify from vendor driver source — do not assume from controller family.
- **SDKCONFIG_DEFAULTS**: The build system prepends the board's `sdkconfig.defaults` before
  WiFi credentials. Never put credential keys in board sdkconfig files.
