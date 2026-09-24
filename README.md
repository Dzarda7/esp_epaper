# esp_epaper

[![Component Registry](https://components.espressif.com/components/dzarda7/esp_epaper/badge.svg)](https://components.espressif.com/components/dzarda7/esp_epaper)
[![Build](https://github.com/Dzarda7/esp_epaper/actions/workflows/build.yml/badge.svg)](https://github.com/Dzarda7/esp_epaper/actions/workflows/build.yml)

A generic e-paper panel driver for ESP-IDF, exposed through the standard
`esp_lcd` panel interface.

One driver, many panels. Support for a new display is a descriptor; support for
a new controller family is one source file implementing a small vtable.

## Limitations

Monochrome panels only: controller RAM `0x26` is used as the previous image for
differential updates, which is the red plane on three-colour panels. No
four-level greyscale, and no waveform LUT upload yet, so controllers that need
custom LUTs are not supported. A panel handle is not thread safe. A refresh
takes seconds; `epaper_panel_refresh()` blocks for all of it, and the
start/wait split below is the way round that.

Currently supported:

| Panel | Controller | Size | Partial update | Verified |
|---|---|---|---|---|
| GDEY029T94 / GDEM029T94 | SSD1680 | 128x296 mono | yes (fast, differential) | yes, on hardware |
| GDEY0213B74 / GDEM0213B74 | SSD1680 | 122x250 mono | yes (fast, differential) | no, datasheet only |
| GDEH0154D67 / GDEY0154D67 | SSD1681 | 200x200 mono | yes (fast, differential) | no, datasheet only |

Only the 2.9" panel has been run on real glass. The other two come from the
datasheets and are unconfirmed - a starting point, not a guarantee. Reports
welcome if you have one.

The component itself is board-agnostic: it knows panels and controllers, not
pin maps. The examples get their wiring from `examples/common/epd_board_laskakit_espink.h`
(LaskaKit ESPink-Shelf-2.9); in an application, keep the pin map with the
application or in a BSP component alongside the board's other peripherals.

## Installation

```sh
idf.py add-dependency "dzarda7/esp_epaper^0.1.0"
```

Requires ESP-IDF v6.0 or newer. Tested on the ESP32; nothing in the driver is
target specific beyond needing an SPI master.

## Why it looks like this

* **`esp_lcd` compatible.** `esp_lcd_new_panel_epaper()` returns a regular
  `esp_lcd_panel_handle_t`, so `esp_lcd_panel_draw_bitmap()`, `mirror()`,
  `swap_xy()`, `invert_color()` and LVGL work unmodified.
* **E-paper is not an LCD**, so waveform selection, deep sleep and the panel
  power rail live in `epaper_panel_*` extension calls instead of being forced
  into the LCD model.
* **Buffered.** Drawing composes into an internal 1bpp framebuffer; the glass is
  only touched by `epaper_panel_refresh()` (or automatically with
  `auto_refresh`). A second buffer holds the image currently on the glass, so a
  partial refresh only sends and drives the rectangle that actually changed.
* **Rotation** is a config field at init and changeable at runtime with
  `epaper_panel_set_rotation()`. Only the coordinate mapping changes; the
  framebuffer itself always stays in the panel's native orientation.

RAM cost for a 2.9" panel: 2 x 4736 B. Only one buffer is ever sent to the
panel, so only that one wants DMA-capable memory; the other is compared and
copied by the CPU. For larger panels set `flags.fb_in_psram`, or let the driver
fall back on its own. Either way, transfers stage through a 4 KB bounce buffer
when the framebuffer is not DMA-capable.

## Usage

```c
esp_lcd_epaper_config_t epd_cfg = {
    .panel = &epd_panel_gdey029t94,
    .busy_gpio_num = EPINK_SHELF_29_PIN_BUSY,
    .power_gpio_num = EPINK_SHELF_29_PIN_POWER,
    .rotation = EPD_ROTATION_90,          // landscape, 296x128
    .refresh_mode = EPD_REFRESH_AUTO,
    .full_refresh_interval = 20,          // clear ghosting every 20 partials
};
const esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = EPINK_SHELF_29_PIN_RST,
    .bits_per_pixel = 1,
    .vendor_config = &epd_cfg,
};
esp_lcd_panel_handle_t panel;
ESP_ERROR_CHECK(esp_lcd_new_panel_epaper(io, &panel_cfg, &panel));
ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

esp_lcd_panel_draw_bitmap(panel, 0, 0, w, h, bitmap);   // 1bpp, MSB left, set bit = white
epaper_panel_refresh(panel, EPD_REFRESH_AUTO);
epaper_panel_sleep(panel);                              // deep sleep + power rail off
```

The first refresh after power-up or deep sleep is always a full one: the
controller's previous-image RAM cannot be trusted before that.

`epaper_panel_refresh()` blocks for the whole update. To avoid stalling a UI
task for seconds, split it:

```c
epaper_panel_refresh_start(panel, EPD_REFRESH_AUTO);   // returns immediately
...
while (epaper_panel_refresh_busy(panel)) { /* do something else */ }
epaper_panel_refresh_wait(panel, 0);                   // 0 = the panel's worst case
```

Between `start()` and `wait()` the framebuffer must be left alone - drawing then
would be recorded as already being on the glass and never shown - so
`draw_bitmap()` and `epaper_panel_clear()` return `ESP_ERR_INVALID_STATE` until
the refresh has been waited on.

## Rotation

Rotation happens inside `draw_bitmap()`, as part of the same pass that unpacks
the source bitmap, so there is no second rotate pass and no third buffer. That is
worth knowing if you drive the panel through a UI framework that offers its own
rotation: doing it there as well would rotate twice. Pick one.

The controller cannot help. An e-paper controller's RAM is addressed in bytes of
eight horizontal pixels, so while 180 degrees and mirroring are just a different
address-counter direction, 90 and 270 degrees would mean transposing bits across
bytes - which no panel in this class does. Hardware pixel accelerators do not
apply either: they work on colour pixel formats, not on 1bpp.

`esp_lcd_panel_swap_xy()` and `esp_lcd_panel_mirror()` work as usual and are
expressed through the same rotation state, so a framework that drives rotation
that way gets the right result and keeps whichever way up the panel is mounted.

## Examples

* `examples/epaper_demo` - test pattern with a full refresh, then a block
  stepping across the screen with fast partial refreshes.
* `examples/epaper_lvgl` - LVGL 9 rendering into an I1 buffer.

Both build for the ESP32 with `idf.py build` inside the example directory. They
depend on the component through the component manager with an `override_path`,
so they build against this checkout, and equally against the published version.

## Adding hardware

**A panel on an already supported controller needs no fork.** `epd_panel_desc_t`
is public, so the descriptor can live in your own application:

```c
#include "esp_lcd_epaper.h"

static const epd_init_cmd_t my_init[] = {
    {0x3C, 1, {0x05}},          // border waveform
    {0x21, 2, {0x00, 0x80}},    // source range, for glass narrower than the controller
    {0x18, 1, {0x80}},          // internal temperature sensor
};

static const epd_panel_desc_t my_panel = {
    .name = "GDEY042T81",
    .controller = &epd_controller_ssd1680,
    .width = 400, .height = 300, .gate_lines = 300,
    .init_cmds = my_init, .init_cmd_count = 3,
    .busy_active_high = true,
    .reset_low_ms = 10, .reset_high_ms = 10, .power_on_delay_ms = 10,
    .full_refresh_ms = 4000, .partial_refresh_ms = 500,
    .supports_partial = true,
};
```

Then point `esp_lcd_epaper_config_t::panel` at it. A pull request adding the
descriptor to `src/panels/` is welcome once it works, so the next person gets it
for free.

The split is deliberate: the controller owns the *sequence* - reset, soft reset,
geometry, window setup and their ordering - and the descriptor owns the *bytes*
that differ between panels on that controller. So a panel that must not restrict
the source range just leaves `0x21` out of its table, instead of the driver
growing another special case.

Build with debug logging to bring a panel up: the driver prints how long BUSY
was really held against the worst case declared. That is the number the
`*_refresh_ms` fields must cover, and on a board without a BUSY pin it is slept
through blindly - so overstating it costs time on every update.

Widths that are not a multiple of 8 are fine: the framebuffer is padded to whole
bytes the way the controller RAM is, so a 122 px wide 2.13" panel uses a 16 byte
stride with the last 6 columns unused.

**A new controller family does need a change inside the component** - a driver in
`src/controllers/` implementing `epd_controller_t` from
`priv_include/epd_private.h`: `init`, `set_window`, `write_ram`, `refresh`,
`power_off`, `sleep`. That interface is intentionally internal so it can keep
evolving; the framebuffer, rotation, dirty rectangle tracking and refresh policy
are shared and need no changes. Open an issue and it can go in the component.
