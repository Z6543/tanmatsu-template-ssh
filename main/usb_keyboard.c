#include "usb_keyboard.h"

#include <string.h>
#include "bsp/input.h"
#include "bsp/power.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"

static const char TAG[] = "usb_kbd";

// HID key code to ASCII lookup (unshifted / shifted)
// Index = HID key code, value = {unshifted, shifted}
static const char hid_to_ascii[][2] = {
    [HID_KEY_A] = {'a', 'A'}, [HID_KEY_B] = {'b', 'B'},
    [HID_KEY_C] = {'c', 'C'}, [HID_KEY_D] = {'d', 'D'},
    [HID_KEY_E] = {'e', 'E'}, [HID_KEY_F] = {'f', 'F'},
    [HID_KEY_G] = {'g', 'G'}, [HID_KEY_H] = {'h', 'H'},
    [HID_KEY_I] = {'i', 'I'}, [HID_KEY_J] = {'j', 'J'},
    [HID_KEY_K] = {'k', 'K'}, [HID_KEY_L] = {'l', 'L'},
    [HID_KEY_M] = {'m', 'M'}, [HID_KEY_N] = {'n', 'N'},
    [HID_KEY_O] = {'o', 'O'}, [HID_KEY_P] = {'p', 'P'},
    [HID_KEY_Q] = {'q', 'Q'}, [HID_KEY_R] = {'r', 'R'},
    [HID_KEY_S] = {'s', 'S'}, [HID_KEY_T] = {'t', 'T'},
    [HID_KEY_U] = {'u', 'U'}, [HID_KEY_V] = {'v', 'V'},
    [HID_KEY_W] = {'w', 'W'}, [HID_KEY_X] = {'x', 'X'},
    [HID_KEY_Y] = {'y', 'Y'}, [HID_KEY_Z] = {'z', 'Z'},
    [HID_KEY_1] = {'1', '!'}, [HID_KEY_2] = {'2', '@'},
    [HID_KEY_3] = {'3', '#'}, [HID_KEY_4] = {'4', '$'},
    [HID_KEY_5] = {'5', '%'}, [HID_KEY_6] = {'6', '^'},
    [HID_KEY_7] = {'7', '&'}, [HID_KEY_8] = {'8', '*'},
    [HID_KEY_9] = {'9', '('}, [HID_KEY_0] = {'0', ')'},
    [HID_KEY_SPACE]         = {' ', ' '},
    [HID_KEY_MINUS]         = {'-', '_'},
    [HID_KEY_EQUAL]         = {'=', '+'},
    [HID_KEY_OPEN_BRACKET]  = {'[', '{'},
    [HID_KEY_CLOSE_BRACKET] = {']', '}'},
    [HID_KEY_BACK_SLASH]    = {'\\', '|'},
    [HID_KEY_COLON]         = {';', ':'},
    [HID_KEY_QUOTE]         = {'\'', '"'},
    [HID_KEY_TILDE]         = {'`', '~'},
    [HID_KEY_LESS]          = {',', '<'},
    [HID_KEY_GREATER]       = {'.', '>'},
    [HID_KEY_SLASH]         = {'/', '?'},
};

#define HID_TO_ASCII_SIZE (sizeof(hid_to_ascii) / sizeof(hid_to_ascii[0]))

// Map HID key codes to BSP navigation keys (0 = not a nav key)
static bsp_input_navigation_key_t hid_to_nav(uint8_t keycode) {
    switch (keycode) {
        case HID_KEY_ESC:       return BSP_INPUT_NAVIGATION_KEY_ESC;
        case HID_KEY_ENTER:     return BSP_INPUT_NAVIGATION_KEY_RETURN;
        case HID_KEY_TAB:       return BSP_INPUT_NAVIGATION_KEY_TAB;
        case HID_KEY_DEL:       return BSP_INPUT_NAVIGATION_KEY_BACKSPACE;
        case HID_KEY_DELETE:    return BSP_INPUT_NAVIGATION_KEY_BACKSPACE;
        case HID_KEY_UP:        return BSP_INPUT_NAVIGATION_KEY_UP;
        case HID_KEY_DOWN:      return BSP_INPUT_NAVIGATION_KEY_DOWN;
        case HID_KEY_LEFT:      return BSP_INPUT_NAVIGATION_KEY_LEFT;
        case HID_KEY_RIGHT:     return BSP_INPUT_NAVIGATION_KEY_RIGHT;
        case HID_KEY_HOME:      return BSP_INPUT_NAVIGATION_KEY_HOME;
        case HID_KEY_END:       return BSP_INPUT_NAVIGATION_KEY_END;
        case HID_KEY_PAGEUP:    return BSP_INPUT_NAVIGATION_KEY_PGUP;
        case HID_KEY_PAGEDOWN:  return BSP_INPUT_NAVIGATION_KEY_PGDN;
        case HID_KEY_F1:        return BSP_INPUT_NAVIGATION_KEY_F1;
        case HID_KEY_F2:        return BSP_INPUT_NAVIGATION_KEY_F2;
        case HID_KEY_F3:        return BSP_INPUT_NAVIGATION_KEY_F3;
        case HID_KEY_F4:        return BSP_INPUT_NAVIGATION_KEY_F4;
        case HID_KEY_F5:        return BSP_INPUT_NAVIGATION_KEY_F5;
        case HID_KEY_F6:        return BSP_INPUT_NAVIGATION_KEY_F6;
        case HID_KEY_F7:        return BSP_INPUT_NAVIGATION_KEY_F7;
        case HID_KEY_F8:        return BSP_INPUT_NAVIGATION_KEY_F8;
        case HID_KEY_F9:        return BSP_INPUT_NAVIGATION_KEY_F9;
        case HID_KEY_F10:       return BSP_INPUT_NAVIGATION_KEY_F10;
        case HID_KEY_F11:       return BSP_INPUT_NAVIGATION_KEY_F11;
        case HID_KEY_F12:       return BSP_INPUT_NAVIGATION_KEY_F12;
        default:                return BSP_INPUT_NAVIGATION_KEY_NONE;
    }
}

static uint32_t hid_modifiers_to_bsp(uint8_t mod) {
    uint32_t bsp_mod = 0;
    if (mod & HID_LEFT_CONTROL)  bsp_mod |= BSP_INPUT_MODIFIER_CTRL_L;
    if (mod & HID_RIGHT_CONTROL) bsp_mod |= BSP_INPUT_MODIFIER_CTRL_R;
    if (mod & HID_LEFT_SHIFT)    bsp_mod |= BSP_INPUT_MODIFIER_SHIFT_L;
    if (mod & HID_RIGHT_SHIFT)   bsp_mod |= BSP_INPUT_MODIFIER_SHIFT_R;
    if (mod & HID_LEFT_ALT)      bsp_mod |= BSP_INPUT_MODIFIER_ALT_L;
    if (mod & HID_RIGHT_ALT)     bsp_mod |= BSP_INPUT_MODIFIER_ALT_R;
    if (mod & HID_LEFT_GUI)      bsp_mod |= BSP_INPUT_MODIFIER_SUPER_L;
    if (mod & HID_RIGHT_GUI)     bsp_mod |= BSP_INPUT_MODIFIER_SUPER_R;
    return bsp_mod;
}

// Track previously pressed keys to detect new presses
static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX];

static void process_keyboard_report(
    const hid_keyboard_input_report_boot_t *report
) {
    uint8_t mod = report->modifier.val;
    bool shifted = (mod & (HID_LEFT_SHIFT | HID_RIGHT_SHIFT)) != 0;
    uint32_t bsp_mod = hid_modifiers_to_bsp(mod);

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        uint8_t keycode = report->key[i];
        if (keycode == HID_KEY_NO_PRESS || keycode == HID_KEY_ROLLOVER) {
            continue;
        }

        // Only process newly pressed keys
        bool already_pressed = false;
        for (int j = 0; j < HID_KEYBOARD_KEY_MAX; j++) {
            if (prev_keys[j] == keycode) {
                already_pressed = true;
                break;
            }
        }
        if (already_pressed) continue;

        // Try navigation key first
        bsp_input_navigation_key_t nav = hid_to_nav(keycode);
        if (nav != BSP_INPUT_NAVIGATION_KEY_NONE) {
            bsp_input_event_t event = {
                .type = INPUT_EVENT_TYPE_NAVIGATION,
                .args_navigation = {
                    .key = nav,
                    .modifiers = bsp_mod,
                    .state = true,
                },
            };
            bsp_input_inject_event(&event);
            continue;
        }

        // Try ASCII character
        if (keycode < HID_TO_ASCII_SIZE) {
            char ch = hid_to_ascii[keycode][shifted ? 1 : 0];
            if (ch != 0) {
                // Apply Ctrl modifier
                if (mod & (HID_LEFT_CONTROL | HID_RIGHT_CONTROL)) {
                    ch &= 0x1f;
                }
                char utf8[2] = {ch, '\0'};
                bsp_input_event_t event = {
                    .type = INPUT_EVENT_TYPE_KEYBOARD,
                    .args_keyboard = {
                        .ascii = ch,
                        .utf8 = utf8,
                        .modifiers = bsp_mod,
                    },
                };
                bsp_input_inject_event(&event);
            }
        }
    }

    memcpy(prev_keys, report->key, sizeof(prev_keys));
}

static void hid_iface_event_cb(
    hid_host_device_handle_t hid_dev,
    const hid_host_interface_event_t event,
    void *arg
) {
    (void)arg;
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            hid_keyboard_input_report_boot_t report;
            size_t len = 0;
            esp_err_t ret = hid_host_device_get_raw_input_report_data(
                hid_dev, (uint8_t *)&report, sizeof(report), &len);
            if (ret == ESP_OK
                && len >= sizeof(hid_keyboard_input_report_boot_t)) {
                process_keyboard_report(&report);
            }
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "USB keyboard disconnected");
            hid_host_device_close(hid_dev);
            break;
        default:
            break;
    }
}

static void hid_driver_event_cb(
    hid_host_device_handle_t hid_dev,
    const hid_host_driver_event_t event,
    void *arg
) {
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t params;
    if (hid_host_device_get_params(hid_dev, &params) != ESP_OK) return;

    // Only handle keyboard devices (protocol 1 = keyboard)
    if (params.proto != 1) {
        ESP_LOGI(TAG, "Ignoring non-keyboard HID device (proto=%d)",
                 params.proto);
        return;
    }

    ESP_LOGI(TAG, "USB keyboard connected");

    const hid_host_device_config_t dev_config = {
        .callback = hid_iface_event_cb,
        .callback_arg = NULL,
    };

    esp_err_t ret = hid_host_device_open(hid_dev, &dev_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HID device: %s",
                 esp_err_to_name(ret));
        return;
    }

    // Use boot protocol for simpler report parsing
    hid_class_request_set_protocol(hid_dev, HID_REPORT_PROTOCOL_BOOT);
    hid_class_request_set_idle(hid_dev, 0, 0);

    memset(prev_keys, 0, sizeof(prev_keys));

    ret = hid_host_device_start(hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HID device: %s",
                 esp_err_to_name(ret));
    }
}

static void usb_host_task(void *arg) {
    (void)arg;
    while (true) {
        uint32_t events;
        usb_host_lib_handle_events(portMAX_DELAY, &events);
        if (events & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

esp_err_t usb_keyboard_init(void) {
    // Enable USB host power boost
    bsp_power_set_usb_host_boost_enabled(true);

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "USB host install failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    BaseType_t xret = xTaskCreate(
        usb_host_task, "usb_host", 4096, NULL, 2, NULL);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB host task");
        usb_host_uninstall();
        return ESP_FAIL;
    }

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = hid_driver_event_cb,
        .callback_arg = NULL,
    };
    ret = hid_host_install(&hid_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HID host install failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "USB keyboard support initialized");
    return ESP_OK;
}
