/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * esp_epaper demo: full refresh of a test pattern, then a sequence of fast
 * partial updates, on a LaskaKit ESPink-Shelf-2.9 (GDEY029T94).
 */
#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "epd_board_laskakit_espink.h"
#include "esp_lcd_epaper.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo";

/* In this driver a set bit is white, a cleared bit is black. */
#define PX_WHITE 0xFF
#define PX_BLACK 0x00

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
        .rotation = EPD_ROTATION_90,             /* landscape, 296x128 */
        .refresh_mode = EPD_REFRESH_AUTO,
        .full_refresh_interval = 20, /* clear ghosting every 20 partial updates */
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

/** @brief Draw a filled rectangle by handing esp_lcd a 1bpp bitmap of one colour. */
static void fill_rect(esp_lcd_panel_handle_t panel, int x, int y, int w, int h, uint8_t colour)
{
    const int stride = (w + 7) / 8;
    uint8_t *buf = malloc((size_t)stride * h);

    if (!buf) {
        return;
    }
    memset(buf, colour, (size_t)stride * h);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, buf));
    free(buf);
}

void app_main(void)
{
    esp_lcd_panel_handle_t panel = display_init();
    uint16_t width, height;

    ESP_ERROR_CHECK(epaper_panel_get_size(panel, &width, &height));
    ESP_LOGI(TAG, "visible size %ux%u", width, height);

    /* Test pattern: white page, black frame, black bar down the left edge. */
    ESP_ERROR_CHECK(epaper_panel_clear(panel, true));
    fill_rect(panel, 0, 0, width, 4, PX_BLACK);
    fill_rect(panel, 0, height - 4, width, 4, PX_BLACK);
    fill_rect(panel, 0, 0, 8, height, PX_BLACK);
    fill_rect(panel, width - 8, 0, 8, height, PX_BLACK);
    ESP_LOGI(TAG, "full refresh");
    ESP_ERROR_CHECK(epaper_panel_refresh(panel, EPD_REFRESH_FULL));

    /* A block stepping across the screen, each step a fast partial update. */
    const int block = 24;
    for (int step = 0; step < 8; step++) {
        const int x = 16 + step * 32;
        if (x + block > width - 16) {
            break;
        }
        if (step > 0) {
            fill_rect(panel, 16 + (step - 1) * 32, 48, block, block, PX_WHITE);
        }
        fill_rect(panel, x, 48, block, block, PX_BLACK);
        ESP_LOGI(TAG, "partial refresh %d", step);
        ESP_ERROR_CHECK(epaper_panel_refresh(panel, EPD_REFRESH_PARTIAL));
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "done, putting the panel to sleep");
    ESP_ERROR_CHECK(epaper_panel_sleep(panel));
}
