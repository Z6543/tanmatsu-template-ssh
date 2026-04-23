// 
// Derived from nicolaielectronics/wifi-manager
//
#include <string.h>
#include "bsp/input.h"
#include "bsp/device.h"
#include "common/display.h"
#include "freertos/idf_additions.h"
#include "gui_menu.h"
#include "gui_style.h"
#include "icons.h"
#include "message_dialog.h"
#include "pax_gfx.h"
#include "pax_matrix.h"
#include "pax_types.h"
#include "util_ssh.h"
#include "menu_ssh.h"
#include "menu_ssh_edit.h"
#include "settings_ssh.h"
#include "esp_log.h"

static char const TAG[] = "menu_ssh";

static bool populate_menu_from_ssh_entries(menu_t* menu) {
    // Repopulate from NVS: drop any existing rows first so callers can invoke
    // this after add/edit/delete without duplicating entries.
    size_t previous_position = menu_get_position(menu);
    while (menu_get_length(menu) > 0) {
        menu_remove_item(menu, 0);
    }
    bool empty = true;
    for (uint32_t index = 0; index < SSH_SETTINGS_MAX; index++) {
        ssh_settings_t settings;
        esp_err_t       res = ssh_settings_get(index, &settings);
        if (res == ESP_OK) {
            menu_insert_item(menu, (char*)settings.connection_name, NULL, (void*)index, -1);
            empty = false;
        }
    }
    if (menu_get_length(menu) > 0) {
        if (previous_position >= menu_get_length(menu)) {
            previous_position = menu_get_length(menu) - 1;
        }
        menu_set_position(menu, previous_position);
    }
    return !empty;
}

static void render(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, pax_vec2_t position, bool partial, bool icons,
                   bool loading, bool connected) {
    if (!partial || icons) {
        render_base_screen_statusbar(
            buffer, theme, !partial, !partial || icons, !partial,
            ((gui_element_icontext_t[]){{get_icon(ICON_TERMINAL), "SSH connections"}}), 1,
            ((gui_element_icontext_t[]){{get_icon(ICON_ESC), "/"},
                                        {get_icon(ICON_F1), "Back"},
                                        {get_icon(ICON_F2), "KB Backlight"},
                                        {get_icon(ICON_F3), "Add new"},
                                        {get_icon(ICON_F4), "Edit"},
                                        {get_icon(ICON_F5), "Remove"}}),
            6, ((gui_element_icontext_t[]){{NULL, "↑ / ↓ | ⏎ Connect"}}), 1);
    }
    menu_render(buffer, menu, position, theme, partial);
    if (menu_find_item(menu, 0) == NULL) {
        if (loading) {
            pax_draw_text(buffer, theme->palette.color_foreground, theme->footer.text_font, 16, position.x0,
                          position.y0 + 18 * 0, "Loading list of stored SSH connections...");
        } else {
            pax_draw_text(buffer, theme->palette.color_foreground, theme->footer.text_font, 16, position.x0,
                          position.y0 + 18 * 0, "No SSH connections stored");
        }
    }
    display_blit_buffer(buffer);
}

static void add_connection(pax_buf_t* buffer, gui_theme_t* theme) {
    int index = ssh_settings_find_empty_slot();
    if (index == -1) {
        message_dialog(get_icon(ICON_ERROR), "Error", "No empty slot, can not add another SSH connection", "Go back");
    }
    menu_ssh_edit(buffer, theme, index, true);
}

static void keyboard_backlight(void) {
    uint8_t brightness;
    bsp_input_get_backlight_brightness(&brightness);
    if (brightness != 100) {
        brightness = 100;
    } else {
        brightness = 0;
    }
    ESP_LOGI(TAG, "Keyboard brightness: %u%%\r\n", brightness);
    bsp_input_set_backlight_brightness(brightness);
}

//static bool _menu_ssh(pax_buf_t* buffer, gui_theme_t* theme) {
void menu_ssh(pax_buf_t* buffer, gui_theme_t* theme) {
    ESP_LOGI(TAG, "menu_ssh()");
    char message_buffer[256] = {0};
    QueueHandle_t input_event_queue   = NULL;
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));
    ssh_settings_t settings;

    int header_height = theme->header.height + (theme->header.vertical_margin * 2);
    int footer_height = theme->footer.height + (theme->footer.vertical_margin * 2);

    pax_vec2_t position = {
        .x0 = theme->menu.horizontal_margin + theme->menu.horizontal_padding,
        .y0 = header_height + theme->menu.vertical_margin + theme->menu.vertical_padding,
        .x1 = pax_buf_get_width(buffer) - theme->menu.horizontal_margin - theme->menu.horizontal_padding,
        .y1 = pax_buf_get_height(buffer) - footer_height - theme->menu.vertical_margin - theme->menu.vertical_padding,
    };

    ESP_LOGI(TAG, "  initialising menu");
    menu_t menu = {0};
    menu_initialize(&menu);
    ESP_LOGI(TAG, "  rendering menu");
    render(buffer, theme, &menu, position, false, true, true, false);
    ESP_LOGI(TAG, "  populating menu");
    populate_menu_from_ssh_entries(&menu);

    //bool prev_connected = false;
    //bool connected      = update_connected((uint32_t)menu_get_callback_args(&menu, menu_get_position(&menu)));
    render(buffer, theme, &menu, position, false, true, false, false);

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION:
                    if (event.args_navigation.state) {
                        switch (event.args_navigation.key) {
                            case BSP_INPUT_NAVIGATION_KEY_ESC:
                            case BSP_INPUT_NAVIGATION_KEY_F1:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_B:
                                bsp_device_restart_to_launcher();
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F2:
				keyboard_backlight();
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F3:
                                add_connection(buffer, theme);
                                populate_menu_from_ssh_entries(&menu);
                                render(buffer, theme, &menu, position, false, true, false, false);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_F4:
                                if (menu_find_item(&menu, 0) != NULL) {
                                    uint8_t index = (uint32_t)menu_get_callback_args(&menu, menu_get_position(&menu));
                                    menu_ssh_edit(buffer, theme, index, false);
                                    populate_menu_from_ssh_entries(&menu);
                                    render(buffer, theme, &menu, position, false, true, false, false);
                                }
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F5:
                            case BSP_INPUT_NAVIGATION_KEY_SELECT:
                                uint8_t index = (uint32_t)menu_get_callback_args(&menu, menu_get_position(&menu));
                                ssh_settings_t settings = {0};
                                if (ssh_settings_get(index, &settings) == ESP_OK) {
                                    snprintf(message_buffer, sizeof(message_buffer),
                                             "Do you really want to delete SSH connection %s?", settings.connection_name);
                                } else {
                                    snprintf(message_buffer, sizeof(message_buffer),
                                             "Do you really want to delete this connection?");
                                }
                                message_dialog_return_type_t msg_ret =
                                    adv_dialog_yes_no(get_icon(ICON_HELP), "Delete SSH connection", message_buffer);
                                if (msg_ret == MSG_DIALOG_RETURN_OK) {
                                    ssh_settings_erase(index);
                                    populate_menu_from_ssh_entries(&menu);
                                }
                                render(buffer, theme, &menu, position, false, true, false, false);
                            	break;
                            case BSP_INPUT_NAVIGATION_KEY_UP:
                                menu_navigate_previous(&menu);
                                render(buffer, theme, &menu, position, true, false, false, false);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_DOWN:
                                menu_navigate_next(&menu);
                                render(buffer, theme, &menu, position, true, false, false, false);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A:
                            case BSP_INPUT_NAVIGATION_KEY_JOYSTICK_PRESS:
				// launch an ssh session using the selected connection details
                                if (menu_find_item(&menu, 0) != NULL) {
                                    uint8_t index = (uint32_t)menu_get_callback_args(&menu, menu_get_position(&menu));
				    ssh_settings_get(index, &settings);
				    util_ssh(buffer, theme, &settings, index);
                                    render(buffer, theme, &menu, position, false, false, false, false);
                                }
                                break;
                            default:
                                break;
                        }
                    }
                    break;
                default:
                    break;
            }
        } else {
            //prev_connected = connected;
            //uint8_t index  = (uint32_t)menu_get_callback_args(&menu, menu_get_position(&menu));
            //connected      = update_connected(index);
            render(buffer, theme, &menu, position, true, true, false, false);
        }
    }
}

