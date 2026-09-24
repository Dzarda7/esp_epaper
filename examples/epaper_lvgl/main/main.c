/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * esp_epaper + LVGL 9 on a LaskaKit ESPink-Shelf-2.9 (GDEY029T94).
 *
 * LVGL renders into a 1bpp (I1) full-screen buffer; the flush callback hands
 * that buffer to esp_lcd and asks the panel for a refresh. Because e-paper
 * updates take hundreds of milliseconds, the screen is refreshed from the flush
 * callback and LVGL is told the flush is ready only once the glass is updated.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "epd_board_laskakit_espink.h"
#include "esp_lcd_epaper.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "lvgl_demo";

#define LVGL_TICK_PERIOD_MS 5
#define DISP_WIDTH          296 /* landscape */
#define DISP_HEIGHT         128

static esp_lcd_panel_handle_t s_panel;
static lv_obj_t *s_counter_label;

static esp_lcd_panel_handle_t display_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = EPINK_SHELF_29_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = EPINK_SHELF_29_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPINK_SHELF_29_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = EPINK_SHELF_29_PIN_CS,
        .dc_gpio_num = EPINK_SHELF_29_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = EPINK_SHELF_29_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EPINK_SHELF_29_SPI_HOST, &io_cfg, &io));

    esp_lcd_epaper_config_t epd_cfg = {
        .panel = &epd_panel_gdey029t94,
        .busy_gpio_num = EPINK_SHELF_29_PIN_BUSY,
        .power_ctrl = epink_shelf_29_power_ctrl, /* board switches the panel rail */
        .rotation = EPD_ROTATION_90,
        .refresh_mode = EPD_REFRESH_AUTO,
        .full_refresh_interval = 20,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = EPINK_SHELF_29_PIN_RST,
        .bits_per_pixel = 1,
        .vendor_config = &epd_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_epaper(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* An I1 draw buffer starts with a two-colour palette; the pixel bits follow.
     * LVGL's palette maps index 1 to white, which is what the driver expects. */
    px_map += 8;

    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    epaper_panel_refresh(s_panel, EPD_REFRESH_AUTO);
    lv_display_flush_ready(disp);
}

static uint32_t lvgl_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "esp_epaper + LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_counter_label = lv_label_create(scr);
    lv_label_set_text(s_counter_label, "0");
    lv_obj_set_style_text_font(s_counter_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_counter_label, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, DISP_WIDTH - 40, 3);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 42);
}

void app_main(void)
{
    s_panel = display_init();

    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);

    /* Full render mode: LVGL always hands over the whole screen, so the flush
     * buffer stride is always DISP_WIDTH / 8. */
    /* Must be aligned for the colour format, otherwise LVGL refuses the buffer
     * and renders nothing. */
    static uint8_t draw_buf[8 + (DISP_WIDTH / 8) * DISP_HEIGHT] __attribute__((aligned(64)));
    lv_display_t *disp = lv_display_create(DISP_WIDTH, DISP_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    build_ui();

    ESP_LOGI(TAG, "UI created, entering LVGL loop");
    uint32_t counter = 0;
    uint32_t next_tick = 0;
    while (1) {
        uint32_t now = lvgl_tick_cb();
        if (now >= next_tick) {
            next_tick = now + 10000; /* one update every 10 s */
            lv_label_set_text_fmt(s_counter_label, "%" LV_PRIu32, counter++);
        }
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms == LV_NO_TIMER_READY || wait_ms > LVGL_TICK_PERIOD_MS * 4) {
            wait_ms = LVGL_TICK_PERIOD_MS * 4;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms ? wait_ms : LVGL_TICK_PERIOD_MS));
    }
}
