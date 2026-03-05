#include <stdio.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "common/display.h"
#include "common/theme.h"
#include "custom_certificates.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "wifi_connection.h"
#include "wifi_remote.h"
#include "menu_ssh.h"

static char const TAG[] = "main";

bool wifi_initialized = false;

bool wifi_stack_get_initialized(void) {
    return wifi_initialized;
}

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage partition
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %d", res);
            return;
        }
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    // Initialize display abstraction layer
    res = display_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %d", res);
        return;
    }

    // Initialize theme
    theme_initialize();

    pax_buf_t* fb = display_get_buffer();

    // Get input event queue from BSP
    QueueHandle_t input_event_queue = NULL;
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Start WiFi stack
    pax_background(fb, 0xFFFFFFFF);
    pax_draw_text(fb, 0xFF000000, pax_font_sky_mono, 16, 0, 0, "Connecting to radio...");
    display_blit_buffer(fb);

    if (wifi_remote_initialize() == ESP_OK) {
        pax_background(fb, 0xFFFFFFFF);
        pax_draw_text(fb, 0xFF000000, pax_font_sky_mono, 16, 0, 0, "Starting WiFi stack...");
        display_blit_buffer(fb);
        wifi_connection_init_stack();
        wifi_initialized = true;

        pax_background(fb, 0xFFFFFFFF);
        pax_draw_text(fb, 0xFF000000, pax_font_sky_mono, 16, 0, 0, "WiFi stack ready.");
        display_blit_buffer(fb);
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "WiFi radio not responding, WiFi not available");
        pax_background(fb, 0xFFFF0000);
        pax_draw_text(fb, 0xFFFFFFFF, pax_font_sky_mono, 16, 0, 0, "WiFi unavailable");
        display_blit_buffer(fb);
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    // Launch SSH connection menu
    gui_theme_t* theme = get_theme();
    menu_ssh(fb, theme);
}
