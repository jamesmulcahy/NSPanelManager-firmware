#pragma once
// The custom PCB's (Waveshare ESP32-P4-86-Panel-ETH-2RO) 720x720 MIPI-DSI panel: which controller the
// unit has, and the panel for it. C, as the JD9365 init table is (C compound literals).
#if defined(BOARD_CUSTOM)

#include <esp_err.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  DSI_PANEL_UNKNOWN = 0,
  DSI_PANEL_ST7703,
  DSI_PANEL_JD9365,
} dsi_panel_kind_t;

/**
 * Reset the panel (`reset_gpio`, active low), read its controller's ID over DBI on `io`, and create the
 * DPI panel for it with `num_fbs` frame buffers (RGB565, 720x720), at that controller's timings. Not yet
 * reset or initialised as an esp_lcd panel: esp_lcd_panel_reset() and esp_lcd_panel_init() follow.
 * ESP_ERR_NOT_SUPPORTED: neither controller answered with its ID.
 */
esp_err_t dsi_panel_create(esp_lcd_dsi_bus_handle_t bus, esp_lcd_panel_io_handle_t io, int reset_gpio, uint8_t num_fbs, esp_lcd_panel_handle_t *panel,
                           dsi_panel_kind_t *kind);

const char *dsi_panel_name(dsi_panel_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif
