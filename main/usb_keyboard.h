#pragma once

#include "esp_err.h"

// Start USB host and HID keyboard driver in a background task.
// Keyboard events are injected into the BSP input queue.
// Non-fatal: returns ESP_OK on success, logs and returns error otherwise.
esp_err_t usb_keyboard_init(void);
