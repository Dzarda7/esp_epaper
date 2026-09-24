/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Good Display GDEH0154D67 (also sold as GDEY0154D67) - 1.54", 200x200, mono.
 * Controller: SSD1681.
 *
 * The glass is as wide as the controller has sources, so unlike the narrower
 * panels it must not restrict the source range with command 0x21 - it simply
 * leaves that command out of the table.
 *
 * NOT VERIFIED ON HARDWARE - see the note in README.md.
 */
#include "epd_private.h"

static const epd_init_cmd_t gdeh0154d67_init[] = {
    { 0x3C, 1, { 0x05 } }, /* border waveform: follow the LUT1 white level */
    { 0x18, 1, { 0x80 } }, /* waveform compensation from the internal temperature sensor */
};

const epd_panel_desc_t epd_panel_gdeh0154d67 = {
    .name = "GDEH0154D67",
    .controller = &epd_controller_ssd1680,
    .width = 200,
    .height = 200,
    .gate_lines = 200,
    .init_cmds = gdeh0154d67_init,
    .init_cmd_count = sizeof(gdeh0154d67_init) / sizeof(gdeh0154d67_init[0]),
    .busy_active_high = true,
    .reset_low_ms = 10,
    .reset_high_ms = 10,
    .power_on_delay_ms = 10,
    .full_refresh_ms = 2600,
    .partial_refresh_ms = 500,
    .supports_partial = true,
};
