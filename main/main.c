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
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "wifi_connection.h"
#include "wifi_remote.h"
#include "menu_ssh.h"
#include "sdcard.h"

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

    // Mount SD card (optional, for screenshots and background images)
    esp_err_t sd_res = sdcard_init();
    if (sd_res != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available: %s", esp_err_to_name(sd_res));
    }

    pax_buf_t* fb = display_get_buffer();

    // Get input event queue from BSP
    QueueHandle_t input_event_queue = NULL;
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    bool network_ok = false;

    // Try WiFi first
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
        network_ok = true;
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGW(TAG, "WiFi radio not responding");
    }

    // Init netif/event loop if WiFi stack didn't do it
    if (!wifi_initialized) {
        esp_netif_init();
        esp_event_loop_create_default();
    }

    // Try Ethernet (W5500 on J4 PMOD) if available
    if (!network_ok) {
        pax_background(fb, 0xFFFFFFFF);
        pax_draw_text(fb, 0xFF000000, pax_font_sky_mono, 16, 0, 0, "Trying Ethernet...");
        display_blit_buffer(fb);
    }
    esp_err_t eth_res = ethernet_init();
    if (eth_res == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet initialized, waiting for IP...");
        if (!network_ok) {
            for (int i = 0; i < 50 && !ethernet_connected(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (ethernet_connected()) {
                ESP_LOGI(TAG, "Ethernet connected");
                pax_background(fb, 0xFFFFFFFF);
                pax_draw_text(fb, 0xFF000000, pax_font_sky_mono, 16, 0, 0, "Ethernet connected.");
                display_blit_buffer(fb);
                network_ok = true;
            }
        }
    } else if (eth_res != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(eth_res));
    }

    if (!network_ok) {
        pax_background(fb, 0xFFFF0000);
        pax_draw_text(fb, 0xFFFFFFFF, pax_font_sky_mono, 16, 0, 0, "No network available");
        display_blit_buffer(fb);
        ESP_LOGW(TAG, "No network connectivity");
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Launch SSH connection menu
    gui_theme_t* theme = get_theme();
    menu_ssh(fb, theme);
}
