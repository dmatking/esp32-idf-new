# /newboard — Add a New Board to idf-new

Onboard a new ESP32 board into the `idf-new` scaffolding tool. Follow the steps below
in order. Report progress at each step and stop to ask if anything is ambiguous.

---

## Step 1 — Gather inputs

Ask the user for these if not already provided:

- **Board ID** — used as the path under `boards/`. Format: `<vendor>/<board_name>`
  (e.g. `m5stack/tab5`, `waveshare/wvshr_p4_1024`). Lowercase, underscores, no spaces.
- **Vendor directory** — path to the vendor files (e.g. `~/hardware/vendors/M5Stack-Tab5/`).

Once you have the vendor directory, read `<vendor-dir>/CLAUDE.md` if it exists — the user
often puts important notes there (IDF version, display specs, gotchas).

---

## Step 2 — Research the board

From the vendor directory and any vendor drivers, identify:

| Property | Where to look |
|----------|--------------|
| ESP32 target | CLAUDE.md, sdkconfig.defaults, CMakeLists.txt |
| Display controller | CLAUDE.md, header files, `esp_lcd_*.c` driver names |
| Display interface | MIPI DSI / SPI / i80 — from driver init code |
| Resolution (W × H) | Driver `#define`s or config structs |
| Color format + byte order | Driver source, NOT assumed from controller family |
| Touch controller + interface | Schematic, CLAUDE.md, touch driver filenames |
| Key GPIOs | Driver `#define`s (backlight, reset, touch INT/RST) |
| PSRAM config | sdkconfig.defaults — speed, mode (quad/hex/octal) |
| IDF version requirement | CLAUDE.md, `.idf-version`, `idf_component.yml` |

Then find the closest existing board in `boards/` as a template. For ESP32-P4 MIPI-DSI
boards, use `boards/waveshare/wvshr_p4_720_touch/` as the template. Read its
`board_impl.c`, `board.json`, and `sdkconfig.defaults` before writing anything.

---

## Step 3 — Create branch

```bash
git checkout -b board/<vendor>-<board_name>
# e.g. board/m5stack-tab5
```

---

## Step 4 — Generate board files

Create `boards/<vendor>/<board_name>/` with these files:

### board.json

```json
{
  "display_name": "<Human-readable name>",
  "panel": "<CONTROLLER>",
  "has_touch": true,
  "screen": {
    "size_inches": <N.N>,
    "resolution": "<W>x<H>",
    "technology": "IPS",
    "shape": "rect"
  },
  "traits": [
    "mipi-dsi",
    "rgb888",
    "esp32p4"
  ],
  "board_features": []
}
```

Traits to include as applicable: `mipi-dsi`, `spi`, `i80`, `rgb888`, `rgb565`,
`esp32p4`, `esp32s3`, `wifi6`, `touch`.

### board_impl.c

Implement all functions from `board_interface.h`:

- `board_init()` — init display, touch, backlight; allocate framebuffers
- `board_get_name()` — return display name string
- `board_has_lcd()` — return `s_panel != NULL`
- `board_lcd_width()` / `board_lcd_height()` — return W / H
- `board_lcd_flush()` — push framebuffer to display
- `board_lcd_clear()` — memset framebuffer to 0
- `board_lcd_fill(color)` — fill with RGB565 color
- `board_lcd_set_pixel_raw(x, y, color)` — write native-format pixel
- `board_lcd_set_pixel_rgb(x, y, r, g, b)` — write RGB888 pixel
- `board_lcd_pack_rgb(r, g, b)` — convert RGB888 → native format
- `board_lcd_get_pixel_raw(x, y)` — read pixel from framebuffer
- `board_lcd_unpack_rgb(color, r, g, b)` — unpack native → RGB888
- `board_lcd_sanity_test()` — cycle colors to verify display (optional but useful)

**Critical details:**
- **Byte order**: verify from vendor driver, not assumed from controller family
- **Console**: use `UART primary + USB_SERIAL_JTAG secondary` for all P4 boards
  (do NOT use USB CDC as primary — it breaks early boot logs)
- **PSRAM**: allocate framebuffers from PSRAM (`MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`)
- Use double-buffering with PPA SRM for P4/MIPI-DSI boards (see waveshare template)

### idf_component.yml

```yaml
dependencies:
  idf: ">=5.3"
  # Add component deps for this board's driver:
  # espressif/esp_lcd_touch_gt911: "^1.0.0"
```

If the vendor driver is not in the IDF component registry, copy it into `components/`
inside the board directory. The scaffolding tool copies `components/` into generated projects.

### sdkconfig.defaults

Base it on `boards/waveshare/wvshr_p4_720_touch/sdkconfig.defaults`. Adjust:

- `CONFIG_IDF_TARGET` — target chip
- PSRAM speed/mode — match vendor spec
- CPU frequency — default 360MHz for P4
- WiFi/hosted config — only if board has WiFi; Tab5 uses esp_hosted via SDIO like waveshare
- Remove sections that don't apply

**Always include:**
```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y
CONFIG_SPIRAM=y
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
```

**Note on SDKCONFIG_DEFAULTS**: the build system prepends this file automatically
before WiFi credentials. Do not add credential keys here.

---

## Step 5 — Build validation loop

```bash
# Scaffold a temporary test project
idf-new /tmp/board_test --board <vendor>/<board_name>

# Build
cd /tmp/board_test
idf set-target <target>
idf build
```

If the build fails: read the error, fix the board files, rebuild. Repeat until clean.
Stop after 5 iterations and report if still failing — don't guess endlessly.

Clean up when done:
```bash
rm -rf /tmp/board_test
```

---

## Step 6 — IDF version check

If the vendor code requires a specific IDF version (e.g. v5.4.2 for M5Stack Tab5):

- Note it in `board.json` `traits`: `"idf-5.4.2"`
- Tell the user: *"This board requires IDF v5.4.2. To use it, create a `.idf-version`
  file containing `5.4.2` in projects that use this board."*
- Do NOT create `.idf-version` yourself — let the user decide per project.

---

## Step 7 — Hardware validation (optional)

Only if the user says the hardware is physically connected.

```bash
# Flash the test project (requires idf-new /tmp/board_test to still exist)
cd /tmp/board_test
idf flash && idf monitor

# Check display output with webcam
esp32-capture
```

Check `/dev/video0` for webcam, `/dev/ttyACM0` or `/dev/ttyUSB0` for serial.
Never ask "what do you see on screen" — use `esp32-capture` to see it directly.

---

## Step 8 — Commit

```bash
git add boards/<vendor>/<board_name>/
git commit -m "Add <board_name> board support"
```

Do NOT push. Let the user review the commit before deciding whether to push.

Report a summary: branch name, files created, build result, and any IDF version notes.
