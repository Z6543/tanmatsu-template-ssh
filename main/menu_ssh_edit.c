//
// Derived from nicolaielectronics/wifi-manager
//
#include <string.h>
#include "bsp/input.h"
#include "common/display.h"
#include "esp_eap_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi_types_generic.h"
#include "freertos/idf_additions.h"
#include "gui_element_footer.h"
#include "gui_menu.h"
#include "gui_style.h"
#include "icons.h"
#include "textedit.h"
#include "message_dialog.h"
#include "pax_gfx.h"
#include "pax_matrix.h"
#include "pax_types.h"
#include "wifi_connection.h"
#include "util_ssh.h"
#include "menu_ssh.h"
#include "menu_ssh_edit.h"
#include "settings_ssh.h"
// #include "shapes/pax_misc.h"

static char const TAG[] = "menu_ssh_edit";

typedef enum {
    ACTION_NONE,
    ACTION_CONNECTION_NAME,
    ACTION_DEST_HOST,
    ACTION_DEST_PORT,
    ACTION_USERNAME,
    ACTION_AUTH_MODE,
    ACTION_PASSWORD,
    ACTION_PRIVATE_KEY,
    ACTION_LAST,
} menu_ssh_edit_action_t;

static void render(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, pax_vec2_t position, bool partial, bool icons) {
    if (!partial || icons) {
        render_base_screen_statusbar(buffer, theme, !partial, !partial || icons, !partial,
                                     ((gui_element_icontext_t[]){{get_icon(ICON_TERMINAL), "Edit SSH connection"}}), 1,
                                     ((gui_element_icontext_t[]){{get_icon(ICON_ESC), "/"},
                                                                 {get_icon(ICON_F1), "Exit without saving"},
                                                                 {get_icon(ICON_F4), "Save and exit"}}),
                                     3, ((gui_element_icontext_t[]){{NULL, "↑ / ↓ | ⏎ Edit setting"}}), 1);
    }

    menu_render(buffer, menu, position, theme, partial);
    display_blit_buffer(buffer);
}

const char* ssh_auth_mode_to_string(ssh_auth_mode_t auth_mode) {
    switch (auth_mode) {
	case SSH_AUTH_PASSWORD:
            return "Password";
	case SSH_AUTH_PUBKEY:
            return "Public Key";
	case SSH_AUTH_INTERACTIVE:
            return "Interactive";
        default:
            return "Unknown";
    }
}

//const char* ssh_string_to_auth_mode(char *auth_mode_str) {
//    if (strncmp(auth_mode_str, "Password", 8)) {
//	return SSH_AUTH_PASSWORD;
//    } else if (strncmp(auth_mode_str, "Public Key", 10)) {
//	return SSH_AUTH_PUBKEY;
//    } else if (strncmp(auth_mode_str, "Interactive", 11)) {
//	return SSH_AUTH_INTERACTIVE;
//    }
//}

static void menu_populate(menu_t* menu, ssh_settings_t* settings, uint8_t index) {
    ESP_LOGI(TAG, "starting menu_populate()");

    size_t previous_position = menu_get_position(menu);
    ESP_LOGI(TAG, "  previous menu position: %d", (int)previous_position);
    while (menu_get_length(menu) > 0) {
        menu_remove_item(menu, 0);
    }

    char temp[129] = {0};

    memset(temp, 0, sizeof(temp)); // don't display the password
    memcpy(temp, settings->connection_name, sizeof(settings->connection_name));
    ESP_LOGI(TAG, "connection_name: %s", temp);
    menu_insert_item_value(menu, "Name", temp, NULL, (void*)ACTION_CONNECTION_NAME, -1);

    memcpy(temp, settings->dest_host, sizeof(settings->dest_host));
    ESP_LOGI(TAG, "dest_host: %s", temp);
    menu_insert_item_value(menu, "Host", temp, NULL, (void*)ACTION_DEST_HOST, -1);

    memset(temp, 0, sizeof(temp)); // don't display the password
    memcpy(temp, settings->dest_port, sizeof(settings->dest_port));
    ESP_LOGI(TAG, "dest_port: %s", temp);
    menu_insert_item_value(menu, "Port", temp, NULL, (void*)ACTION_DEST_PORT, -1);

    memset(temp, 0, sizeof(temp)); // don't display the password
    memcpy(temp, settings->username, sizeof(settings->username));
    ESP_LOGI(TAG, "username: %s", temp);
    menu_insert_item_value(menu, "Username", temp, NULL, (void*)ACTION_USERNAME, -1);

    //memcpy(temp, ssh_auth_mode_to_string(settings->auth_mode), sizeof(settings->auth_mode));
    //ESP_LOGI(TAG, "auth_mode: %s", temp);
    //menu_insert_item_value(menu, "Auth Mode", temp, NULL, (void*)ACTION_AUTH_MODE, -1);

    // Placeholder so the user can tell whether a password is stored without revealing it.
    ESP_LOGI(TAG, "password: <redacted>");
    const char* password_display = (settings->password[0] != '\0') ? "********" : "";
    menu_insert_item_value(menu, "Password", password_display, NULL, (void*)ACTION_PASSWORD, -1);

    // Show private key status (read-only, set via badgelink)
    bool has_key = ssh_settings_has_private_key(index);
    menu_insert_item_value(menu, "Private Key", has_key ? "Set" : "Not set", NULL, (void*)ACTION_PRIVATE_KEY, -1);

    if (previous_position >= menu_get_length(menu)) {
        previous_position = menu_get_length(menu) - 1;
        ESP_LOGI(TAG, "  updated previous menu position: %d", (int)previous_position);
    }
    menu_set_position(menu, previous_position);
}

static void edit_connection_name(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, ssh_settings_t* settings) {
    ESP_LOGI(TAG, "edit_connection_name()");
    char temp[129] = {0};
    bool accepted  = false;

    memset(temp, 0, sizeof(temp));
    memcpy(temp, settings->connection_name, sizeof(settings->connection_name));
    ESP_LOGI(TAG, "fetched connection_name: %s", settings->connection_name);

    menu_textedit(buffer, theme, "Connection Name", temp, sizeof(settings->connection_name) + 1, true, &accepted);
    if (accepted) {
        memcpy(settings->connection_name, temp, sizeof(settings->connection_name));
        ESP_LOGI(TAG, "updated connection_name: %s", settings->connection_name);
        menu_set_value(menu, 0, temp);
    }
}

static void edit_dest_host(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, ssh_settings_t* settings) {
    char temp[129] = {0};
    bool accepted  = false;

    memset(temp, 0, sizeof(temp));
    memcpy(temp, settings->dest_host, sizeof(settings->dest_host));
    ESP_LOGI(TAG, "fetched dest_host: %s", settings->dest_host);

    menu_textedit(buffer, theme, "Host", temp, sizeof(settings->dest_host) + 1, true, &accepted);
    if (accepted) {
        memcpy(settings->dest_host, temp, sizeof(settings->dest_host));
        ESP_LOGI(TAG, "updated dest_host: %s", settings->dest_host);
        menu_set_value(menu, 1, temp);
    }
}

static void edit_dest_port(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, ssh_settings_t* settings) {
    char temp[129] = {0};
    bool accepted  = false;

    memset(temp, 0, sizeof(temp));
    memcpy(temp, settings->dest_port, sizeof(settings->dest_port));
    ESP_LOGI(TAG, "fetched dest_port: %s", settings->dest_port);

    menu_textedit(buffer, theme, "Port", temp, sizeof(settings->dest_port) + 1, true, &accepted);
    if (accepted) {
        memcpy(settings->dest_port, temp, sizeof(settings->dest_port));
        ESP_LOGI(TAG, "updated dest_port: %s", settings->dest_port);
        menu_set_value(menu, 2, temp);
    }
}

// XXX auth_mode is hard coded to SSH_AUTH_PASSWORD right now and this function isn't used yet
static void edit_auth_mode(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, menu_t* menu_auth_mode,
                          pax_vec2_t position, ssh_settings_t* settings, QueueHandle_t input_event_queue) {
    menu_set_position(menu_auth_mode, 0);
    for (size_t i = 0; i < menu_get_length(menu_auth_mode); i++) {
        if ((ssh_auth_mode_t)menu_get_callback_args(menu_auth_mode, i) == settings->auth_mode) {
            menu_set_position(menu_auth_mode, i);
            break;
        }
    }

    bool partial = false;
    while (1) {
        render_base_screen_statusbar(
            buffer, theme, !partial, true, !partial, ((gui_element_icontext_t[]){{get_icon(ICON_TERMINAL), "Auth Mode"}}), 1,
            ((gui_element_icontext_t[]){{get_icon(ICON_ESC), "/"}, {get_icon(ICON_F1), "Go back"}}), 2,
            ((gui_element_icontext_t[]){{NULL, "↑ / ↓ | ⏎ Select"}}), 1);
        menu_render(buffer, menu_auth_mode, position, theme, partial);
        display_blit_buffer(buffer);
        partial = true;
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION: {
                    if (event.args_navigation.state) {
                        switch (event.args_navigation.key) {
                            case BSP_INPUT_NAVIGATION_KEY_ESC:
                            case BSP_INPUT_NAVIGATION_KEY_F1:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_B:
                                return;
                            case BSP_INPUT_NAVIGATION_KEY_UP:
                                menu_navigate_previous(menu_auth_mode);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_DOWN:
                                menu_navigate_next(menu_auth_mode);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A:
                            case BSP_INPUT_NAVIGATION_KEY_JOYSTICK_PRESS: {
                                ssh_auth_mode_t auth_mode = (ssh_auth_mode_t)menu_get_callback_args(
                                    menu_auth_mode, menu_get_position(menu_auth_mode));
                                settings->auth_mode = auth_mode;
                                menu_set_value(menu, 3, ssh_auth_mode_to_string(auth_mode));
                                return;
                            }
                            default:
                                break;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

static void edit_username(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, ssh_settings_t* settings) {
    char temp[129] = {0};
    bool accepted  = false;
    memset(temp, 0, sizeof(temp));
    memcpy(temp, settings->username, sizeof(settings->username));
    ESP_LOGI(TAG, "fetched username: %s", settings->username);

    menu_textedit(buffer, theme, "Username", temp, sizeof(settings->username) + 1, true, &accepted);
    if (accepted) {
        memcpy(settings->username, temp, sizeof(settings->username));
        ESP_LOGI(TAG, "updated username: %s", settings->username);
        menu_set_value(menu, 3, temp);
    }
}

static void edit_password(pax_buf_t* buffer, gui_theme_t* theme, menu_t* menu, ssh_settings_t* settings) {
    char temp[129] = {0};
    bool accepted  = false;
    // Pre-fill with the existing password so the user can verify and edit it,
    // matching the behaviour of the other edit_* functions.
    memcpy(temp, settings->password, sizeof(settings->password));

    menu_textedit(buffer, theme, "Password", temp, sizeof(settings->password) + 1, true, &accepted);
    if (accepted) {
        memset(settings->password, 0, sizeof(settings->password));
        memcpy(settings->password, temp, sizeof(settings->password));
        ESP_LOGI(TAG, "updated password: <redacted>");
        menu_set_value(menu, 4, (settings->password[0] != '\0') ? "********" : "");
    }
    // Wipe the local copy of the password from the stack.
    memset(temp, 0, sizeof(temp));
}

bool menu_ssh_edit(pax_buf_t* buffer, gui_theme_t* theme, uint8_t index, bool new_entry) {
    QueueHandle_t input_event_queue = NULL;
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));
    ESP_LOGI(TAG, "menu_ssh_edit()");

    ssh_settings_t settings = {0};
    if (new_entry) {
	ESP_LOGI(TAG, "making new ssh connection");
	//memcpy(settings.connection_name, "Conn", 4);
	//memcpy(settings.dest_host, "Host", 4);
	//memcpy(settings.dest_port, "Port", 4);
	//memcpy(settings.username, "User", 4);
	//memcpy(settings.password, "Pass", 4);
    } else {
	ESP_LOGI(TAG, "fetching ssh connection info");
        esp_err_t res = ssh_settings_get(index, &settings);
        if (res != ESP_OK) {
            char message[128];
            snprintf(message, sizeof(message), "%s, failed to read SSH settings at index %u", esp_err_to_name(res), index);
            printf("%s\r\n", message);
            message_dialog(get_icon(ICON_ERROR), "An error occurred", message, "Go back");
            return false;
        }
    }

    // XXX for now we hard code password auth
    settings.auth_mode = SSH_AUTH_PASSWORD;

    // Menus for enum selection
    //menu_t menu_auth_mode = {0};
    //menu_insert_item(&menu_auth_mode, "Password", NULL, (void*)SSH_AUTH_PASSWORD, -1);
    //menu_insert_item(&menu_auth_mode, "Public Key", NULL, (void*)SSH_AUTH_PUBKEY, -1);
    //menu_insert_item(&menu_auth_mode, "Interactive", NULL, (void*)SSH_AUTH_INTERACTIVE, -1);

    // Menu for parameters
    menu_t menu = {0};
    menu_initialize(&menu);
    menu_populate(&menu, &settings, index);

    int header_height = theme->header.height + (theme->header.vertical_margin * 2);
    int footer_height = theme->footer.height + (theme->footer.vertical_margin * 2);

    pax_vec2_t position = {
        .x0 = theme->menu.horizontal_margin + theme->menu.horizontal_padding,
        .y0 = header_height + theme->menu.vertical_margin + theme->menu.vertical_padding,
        .x1 = pax_buf_get_width(buffer) - theme->menu.horizontal_margin - theme->menu.horizontal_padding,
        .y1 = pax_buf_get_height(buffer) - footer_height - theme->menu.vertical_margin - theme->menu.vertical_padding,
    };

    render(buffer, theme, &menu, position, false, true);
    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION: {
                    if (event.args_navigation.state) {
                        switch (event.args_navigation.key) {
                            case BSP_INPUT_NAVIGATION_KEY_ESC:
                            case BSP_INPUT_NAVIGATION_KEY_F1:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_B:
                                menu_free(&menu);
                                //menu_free(&menu_auth_mode);
                                return false;
                            case BSP_INPUT_NAVIGATION_KEY_F4:
                            case BSP_INPUT_NAVIGATION_KEY_START: {
                                esp_err_t res = ssh_settings_set(index, &settings);
                                if (res == ESP_OK) {
                                    return true;
                                } else {
                                    message_dialog(get_icon(ICON_ERROR), "Error", "Failed to save SSH settings",
                                                   "Go back");
                                }
                                break;
                            }
                            case BSP_INPUT_NAVIGATION_KEY_UP:
                                menu_navigate_previous(&menu);
                                render(buffer, theme, &menu, position, true, false);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_DOWN:
                                menu_navigate_next(&menu);
                                render(buffer, theme, &menu, position, true, false);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A:
                            case BSP_INPUT_NAVIGATION_KEY_JOYSTICK_PRESS: {
                                menu_ssh_edit_action_t action =
                                    (menu_ssh_edit_action_t)menu_get_callback_args(&menu, menu_get_position(&menu));
                                switch (action) {
                                    case ACTION_CONNECTION_NAME:
                                        edit_connection_name(buffer, theme, &menu, &settings);
                                        break;
                                    case ACTION_DEST_HOST:
                                        edit_dest_host(buffer, theme, &menu, &settings);
                                        break;
                                    case ACTION_DEST_PORT:
                                        edit_dest_port(buffer, theme, &menu, &settings);
                                        break;
                                    case ACTION_USERNAME:
                                        edit_username(buffer, theme, &menu, &settings);
                                        break;
                                    case ACTION_AUTH_MODE:
				        // XXX for now we're hard coding password auth
					settings.auth_mode = SSH_AUTH_PASSWORD;
                                    //    edit_auth_mode(buffer, theme, &menu, &menu_auth_mode, position, &settings,
                                    //                  input_event_queue);
                                    //    menu_populate( &menu, &settings);
                                        break;
                                    case ACTION_PASSWORD:
                                        edit_password(buffer, theme, &menu, &settings);
                                        break;
                                    case ACTION_PRIVATE_KEY:
                                        message_dialog(get_icon(ICON_TERMINAL), "Private Key",
                                                       "Use badgelink to write a PEM private key to NVS.", "OK");
                                        break;
                                    default:
                                        break;
                                }
                                render(buffer, theme, &menu, position, false, false);
                                break;
                            }
                            default:
                                break;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        } else {
            render(buffer, theme, &menu, position, true, true);
        }
    }
}
