# Graphics Optimization: board_lcd_flush_region()

Track progress adding partial-region flush to the idf-new HAL. Primary motivation
is LVGL dirty-region callbacks — LVGL only re-renders and flushes changed rectangles,
so a full-screen push on every frame wastes bus bandwidth proportional to unchanged area.

---

## HAL Changes (do first)

### `idf-templates/base_project/main/board_interface.h`

Add after `board_lcd_flush()`:

```c
// Push a rectangular sub-region of the framebuffer to the display.
// Coordinates are end-exclusive (x2, y2 are one past the last pixel).
// Falls back to a full flush on boards where region flushing is not meaningful
// (MIPI-DSI auto-refresh, stripe framebuffer).
void board_lcd_flush_region(int x1, int y1, int x2, int y2);
```

- [ ] Add declaration to `board_interface.h`

### `idf-templates/base_project/main/board_defaults.c`

Add weak no-op so headless/generic boards build without changes:

```c
__attribute__((weak))
void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    (void)x1; (void)y1; (void)x2; (void)y2;
}
```

- [ ] Add weak default to `board_defaults.c`

---

## Memory Layout Note (applies to all Tier-1 boards)

All simple boards store pixels as a contiguous row-major `uint16_t` array:

```
s_fb[y * LCD_H_RES + x]
```

Full-width horizontal bands (`x1 == 0 && x2 == LCD_H_RES`) are contiguous in
memory — pass a single pointer offset:

```c
&s_fb[y1 * LCD_H_RES]   // base of first row
```

Partial-width regions are **not** contiguous; each row must be sent separately:

```c
for (int y = y1; y < y2; y++)
    esp_lcd_panel_draw_bitmap(s_panel, x1, y, x2, y + 1,
                              &s_fb[y * LCD_H_RES + x1]);
```

For LVGL the typical flush region is a full-width dirty band, so the fast path
covers the common case. The row-loop fallback handles any arbitrary rectangle.

---

## Tier 1 — Simple `esp_lcd_panel_draw_bitmap` boards (do first)

These boards already call `esp_lcd_panel_draw_bitmap()` with hardcoded full-screen
coords in `board_lcd_flush()`. Adding `board_lcd_flush_region()` is mechanical:
detect full-width band → single call with offset pointer; else → row loop.

### Implementation template

```c
void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    if (!s_lcd_ready || !s_fb) return;
    if (x1 == 0 && x2 == LCD_H_RES) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y1, LCD_H_RES, y2,
                                  &s_fb[y1 * LCD_H_RES]);
    } else {
        for (int y = y1; y < y2; y++) {
            esp_lcd_panel_draw_bitmap(s_panel, x1, y, x2, y + 1,
                                      &s_fb[y * LCD_H_RES + x1]);
        }
    }
}
```

Replace `s_lcd_ready` / `LCD_H_RES` / `s_fb` / `s_panel` with the actual names
used in each board's impl (they vary slightly).

---

### waveshare/wvshr200

Panel: GC9A01A 240×240 round, SPI.
Framebuffer: `uint16_t *s_fb` (heap, `LCD_H_RES × LCD_V_RES`).
Flush: `esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb)`.

- [ ] Add `board_lcd_flush_region()` using template above
- Readiness guard: `s_panel != NULL`

---

### waveshare/wvshr200_touch

Same display + panel driver as wvshr200 with CST816S touch added.
Board guard: `s_panel != NULL`.

- [ ] Add `board_lcd_flush_region()` using template above (identical to wvshr200)

---

### waveshare/wvshr185_round

Panel: GC9A01A 360×360 round, SPI.
Same pattern as wvshr200.

- [ ] Add `board_lcd_flush_region()` using template above

---

### waveshare/wvshr185_round_touch

Same display + FT6336U touch.
Flush: `esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb)`.

- [ ] Add `board_lcd_flush_region()` using template above

---

### hackerbox/hb107_round128

Panel: GC9A01A 128×128, SPI.
Flush: `esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb)`.

- [ ] Add `board_lcd_flush_region()` using template above

---

### espressif/esp32s3_devkitc_nhd240320_st7789

Panel: ST7789 240×320, SPI.
Flush: `esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb)`.

- [ ] Add `board_lcd_flush_region()` using template above

---

### cyd/cyd28_ili9341_touch

Panel: ILI9341 320×240, SPI shared with XPT2046 touch.
Framebuffer: full `320×240` RGB565.
Flush: `esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb)`.

- [ ] Add `board_lcd_flush_region()` using template above
- Note: SPI bus is shared with touch; touch reads happen between flushes, so no
  additional locking needed for flush_region.

---

## Tier 2 — AMOLED custom-protocol boards

These boards use hand-rolled QSPI or SPI drivers (`amoled_set_window` /
`display_push_colors`) instead of `esp_lcd_panel_draw_bitmap`. Region flush maps
naturally to window + push, but the push helper must be verified to support
arbitrary rectangles (not just full rows).

---

### lilygo/tdisp191_amoled_touch

Panel: RM67162 536×240 AMOLED, QSPI.
Flush: `amoled_set_window(0, 0, LCD_H_RES-1, LCD_V_RES-1)` then
       `amoled_push_buffer(s_fb, LCD_H_RES * LCD_V_RES)`.

`amoled_set_window` accepts any (xs, ys, xe, ye). `amoled_push_buffer` takes a
flat pointer + pixel count; it DMA's chunks in a loop.

Region plan:
- Full-width band: set window to `(0, y1, LCD_H_RES-1, y2-1)`, push
  `&s_fb[y1 * LCD_H_RES]` for `(y2-y1) * LCD_H_RES` pixels.
- Partial-width: set window to `(x1, y1, x2-1, y2-1)` and push row by row (the
  row data is non-contiguous in the framebuffer). Alternatively, copy to a temp
  buffer and push in one shot; for small regions this may be faster than multiple
  transactions.

```c
void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    if (!s_lcd_ready || !s_fb) return;
    if (x1 == 0 && x2 == LCD_H_RES) {
        amoled_set_window(0, y1, LCD_H_RES - 1, y2 - 1);
        amoled_push_buffer(&s_fb[y1 * LCD_H_RES], (y2 - y1) * LCD_H_RES);
    } else {
        for (int y = y1; y < y2; y++) {
            amoled_set_window(x1, y, x2 - 1, y);
            amoled_push_buffer(&s_fb[y * LCD_H_RES + x1], x2 - x1);
        }
    }
}
```

- [ ] Add `board_lcd_flush_region()` as above
- [ ] Verify RM67162 tolerates repeated `set_window` + single-row pushes (timing)

---

### lilygo/t4s3_amoled_touch

Panel: RM690B0 (QSPI AMOLED), resolution TBD from board_impl.
Flush: `display_push_colors(0, 0, w, h, s_fb)`.

`display_push_colors(x, y, width, height, data)` already accepts x/y/w/h; it calls
`amoled_set_window` internally. Region flush:

```c
void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    if (!s_fb) return;
    int w = board_lcd_width();
    if (x1 == 0 && x2 == w) {
        display_push_colors(0, y1, w, y2 - y1, &s_fb[y1 * w]);
    } else {
        for (int y = y1; y < y2; y++) {
            display_push_colors(x1, y, x2 - x1, 1, &s_fb[y * w + x1]);
        }
    }
}
```

- [ ] Confirm `display_push_colors` signature supports arbitrary x/y/w/h (read impl)
- [ ] Add `board_lcd_flush_region()` as above

---

## Tier 3 — MIPI-DSI auto-refresh boards (just delegate to full flush)

These boards write into a DMA framebuffer that the display controller scans
continuously. There is no "flush" in the traditional sense — the hardware picks up
changes on the next scan. Partial region semantics don't improve bus utilisation
here (DSI bandwidth is consumed by the panel's auto-refresh regardless).

`board_lcd_flush_region()` should just call `board_lcd_flush()`.

---

### m5stack/tab5

Panel: ST7123 MIPI-DSI 1280×800. Flush: `memcpy(s_fb, s_backbuf, FB_SIZE)` +
`esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M)`.

```c
void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    (void)x1; (void)y1; (void)x2; (void)y2;
    board_lcd_flush();
}
```

- [ ] Add `board_lcd_flush_region()` delegating to `board_lcd_flush()`

---

### waveshare/wvshr_p4_720_touch

Panel: ST7703 MIPI-DSI 720×720. Flush: `flush_async()` + `flush_wait()` via PPA.

```c
void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    (void)x1; (void)y1; (void)x2; (void)y2;
    board_lcd_flush();
}
```

- [ ] Add `board_lcd_flush_region()` delegating to `board_lcd_flush()`

---

## Special Case — cyd/cyd35_st7796_touch (stripe framebuffer)

Panel: ST7796 480×320, SPI. This board does **not** hold the full framebuffer in
RAM. It uses 4 rotating 80-scanline stripes. The caller must drive the stripe loop:
set `s_stripe_y`, draw the stripe, call `board_lcd_flush()`, repeat.

`board_lcd_flush_region()` cannot satisfy the same semantics as Tier 1 boards —
it doesn't have full-screen pixel data to work from.

Reasonable behaviour: if the requested region falls within the current active stripe
(`s_stripe_y .. s_stripe_y + STRIPE_H - 1`), flush that stripe. Otherwise fall back
to a full `board_lcd_flush()` (which flushes whatever stripe is current).

```c
void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    // Stripe FB: full-screen region data is not available.
    // Flush current stripe; caller is responsible for stripe iteration.
    (void)x1; (void)x2;
    (void)y1; (void)y2;
    board_lcd_flush();
}
```

For LVGL integration this board would need a custom flush callback that understands
the stripe layout — not a drop-in region flush. Defer until LVGL port is planned.

- [ ] Add `board_lcd_flush_region()` as full-flush delegate with comment
- [ ] Document LVGL limitation in board README or board_impl.c comment block

---

## generic board

`boards/generic/board_impl.c` — headless stub, no LCD. The weak default from
`board_defaults.c` covers it; no changes needed.

- [x] Covered by weak default

---

## Future: `board_lcd_draw_bitmap()`

For a proper LVGL flush callback the interface also needs:

```c
// Write an external pixel buffer directly to a display rectangle.
// data is caller-owned RGB565, width × height pixels, row-major.
// Does NOT touch the board's internal framebuffer.
void board_lcd_draw_bitmap(int x1, int y1, int x2, int y2,
                           const uint16_t *data);
```

This is distinct from `flush_region` (which reads from `s_fb`). LVGL renders into
its own buffer and calls the flush callback with a pointer to that buffer. Without
`draw_bitmap`, LVGL output would have to be copied into `s_fb` first.

Keep this as a separate work item — don't conflate with `flush_region`.

- [ ] Add `board_lcd_draw_bitmap()` declaration to `board_interface.h`
- [ ] Add weak no-op to `board_defaults.c`
- [ ] Implement for each board (same tier ordering as above)

---

## Implementation Order

1. HAL headers + defaults (unblocks everything else)
2. Tier 3 MIPI-DSI boards (trivial — just call flush; low risk)
3. Tier 1 simple SPI boards (mechanical template — do all in one pass)
4. cyd35 special case (delegate + add comment)
5. Tier 2 AMOLED boards (verify `display_push_colors` / `amoled_push_buffer` first)
6. `board_lcd_draw_bitmap()` (separate PR — needed for LVGL flush callback)
