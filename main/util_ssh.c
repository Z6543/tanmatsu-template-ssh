//
// Derived from badgeteam/terminal-emulator, libssh2 example code, nicolaielectronics/tanmatsu-launcher
//
#include <string.h>
#include <sys/_intsup.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "common/display.h"
#include "console.h"
#include "driver/uart.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "gui_element_footer.h"
#include "gui_style.h"
#include "icons.h"
#include "message_dialog.h"
#include "textedit.h"
#include "pax_types.h"
#include "pax_codecs.h"
#include "tanmatsu_coprocessor.h"
#include "wifi_connection.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <libssh2.h>
#include "libssh2_setup.h"
#include "lwip/sockets.h"
#include "util_ssh.h"
#include "settings_ssh.h"

extern bool wifi_stack_get_initialized(void);

static char const TAG[] = "util_ssh";

// Normal cursor mode sequences (CSI)
static char const CSI_LEFT[] = "\e[D";
static char const CSI_RIGHT[] = "\e[C";
static char const CSI_UP[] = "\e[A";
static char const CSI_DOWN[] = "\e[B";
// Application cursor mode sequences (SS3) - used by mc, vim, etc.
static char const SS3_LEFT[] = "\eOD";
static char const SS3_RIGHT[] = "\eOC";
static char const SS3_UP[] = "\eOA";
static char const SS3_DOWN[] = "\eOB";
static char const CHR_TAB[] = "\t";
//static char const CHR_BS[] = "\b";
static char const CHR_BS[] = "\x7f";
static char const CHR_NL[] = "\n";

// DECCKM (application cursor keys mode) - set by \e[?1h, reset by \e[?1l
static bool decckm_mode = false;

// F-key escape sequences (xterm style)
// Fn+1..Fn+0 → F1..F10, Fn+hyphen → F11, Fn+equals → F12
static const char* const fkey_sequences[] = {
    [1] = "\eOP",       // F1
    [2] = "\eOQ",       // F2
    [3] = "\eOR",       // F3
    [4] = "\eOS",       // F4
    [5] = "\e[15~",     // F5
    [6] = "\e[17~",     // F6
    [7] = "\e[18~",     // F7
    [8] = "\e[19~",     // F8
    [9] = "\e[20~",     // F9
    [0] = "\e[21~",     // F10
    [10] = "\e[23~",    // F11
    [11] = "\e[24~",    // F12
};

#if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL) || \
    defined(CONFIG_BSP_TARGET_HACKERHOTEL_2026)
#define FOOTER_LEFT  ((gui_element_icontext_t[]){{get_icon(ICON_F5), "Settings"}, {get_icon(ICON_F6), "USB mode"}}), 2
#define FOOTER_RIGHT ((gui_element_icontext_t[]){{NULL, "↑ / ↓ / ← / → | ⏎ Select"}}), 1
#elif defined(CONFIG_BSP_TARGET_MCH2022)
#define FOOTER_LEFT  NULL, 0
#define FOOTER_RIGHT ((gui_element_icontext_t[]){{NULL, "🅰 Select"}}), 1
#else
#define FOOTER_LEFT  NULL, 0
#define FOOTER_RIGHT NULL, 0
#endif

#define BUFFER_SIZE 4096

//static uint8_t       read_buffer[BUFFER_SIZE] = {0};

struct cons_insts_s console_instance;

static char *screen_buf = NULL;
static size_t screen_cols = 0;
static size_t screen_rows = 0;

static void screen_buf_init(size_t cols, size_t rows) {
    free(screen_buf);
    screen_cols = cols;
    screen_rows = rows;
    screen_buf = malloc(cols * rows);
    if (screen_buf) {
        memset(screen_buf, ' ', cols * rows);
    }
}

static void screen_buf_put(size_t x, size_t y, char c) {
    if (screen_buf && x < screen_cols && y < screen_rows) {
        screen_buf[y * screen_cols + x] = c;
    }
}

static void screen_buf_clear(void) {
    if (screen_buf) {
        memset(screen_buf, ' ', screen_cols * screen_rows);
    }
}

static void screen_buf_scroll_up(void) {
    if (!screen_buf || screen_rows < 2) return;
    memmove(screen_buf, screen_buf + screen_cols,
            screen_cols * (screen_rows - 1));
    memset(screen_buf + screen_cols * (screen_rows - 1), ' ',
           screen_cols);
}

static void screen_buf_track_put(struct cons_insts_s *inst, char c) {
    if (c == '\n') {
        if (inst->cursor_y + 1 >= inst->chars_y) {
            screen_buf_scroll_up();
        }
        return;
    }
    if (c == '\r') return;
    if (c == '\t') return;
    if (c < 0x20) return;
    screen_buf_put(inst->cursor_x, inst->cursor_y, c);
    if (inst->cursor_x + 1 >= inst->chars_x) {
        if (inst->cursor_y + 1 >= inst->chars_y) {
            screen_buf_scroll_up();
        }
    }
}

static char utf8_to_ascii(uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7e) return (char)cp;
    switch (cp) {
    // Box-drawing: horizontals
    case 0x2500: case 0x2501: case 0x2504: case 0x2505:
    case 0x2508: case 0x2509: case 0x254C: case 0x254D:
    case 0x2550: case 0x2574: case 0x2576: case 0x2578:
    case 0x257A: case 0x257C: case 0x257E:
        return '-';
    // Box-drawing: verticals
    case 0x2502: case 0x2503: case 0x2506: case 0x2507:
    case 0x250A: case 0x250B: case 0x254E: case 0x254F:
    case 0x2551: case 0x2575: case 0x2577: case 0x2579:
    case 0x257B: case 0x257D: case 0x257F:
        return '|';
    // Box-drawing: all corners, tees, and crosses
    case 0x250C: case 0x250D: case 0x250E: case 0x250F:
    case 0x2510: case 0x2511: case 0x2512: case 0x2513:
    case 0x2514: case 0x2515: case 0x2516: case 0x2517:
    case 0x2518: case 0x2519: case 0x251A: case 0x251B:
    case 0x251C: case 0x251D: case 0x251E: case 0x251F:
    case 0x2520: case 0x2521: case 0x2522: case 0x2523:
    case 0x2524: case 0x2525: case 0x2526: case 0x2527:
    case 0x2528: case 0x2529: case 0x252A: case 0x252B:
    case 0x252C: case 0x252D: case 0x252E: case 0x252F:
    case 0x2530: case 0x2531: case 0x2532: case 0x2533:
    case 0x2534: case 0x2535: case 0x2536: case 0x2537:
    case 0x2538: case 0x2539: case 0x253A: case 0x253B:
    case 0x253C: case 0x253D: case 0x253E: case 0x253F:
    case 0x2540: case 0x2541: case 0x2542: case 0x2543:
    case 0x2544: case 0x2545: case 0x2546: case 0x2547:
    case 0x2548: case 0x2549: case 0x254A: case 0x254B:
    case 0x2552: case 0x2553: case 0x2554: case 0x2555:
    case 0x2556: case 0x2557: case 0x2558: case 0x2559:
    case 0x255A: case 0x255B: case 0x255C: case 0x255D:
    case 0x255E: case 0x255F: case 0x2560: case 0x2561:
    case 0x2562: case 0x2563: case 0x2564: case 0x2565:
    case 0x2566: case 0x2567: case 0x2568: case 0x2569:
    case 0x256A: case 0x256B: case 0x256C:
        return '+';
    // Block elements
    case 0x2588: return '#';
    case 0x2591: case 0x2592: case 0x2593: return '#';
    // Arrows
    case 0x2190: return '<';
    case 0x2191: return '^';
    case 0x2192: return '>';
    case 0x2193: return 'v';
    // Bullets / dots
    case 0x2022: case 0x2023: case 0x25CF: return '*';
    case 0x25CB: case 0x25E6: return 'o';
    // Diamond
    case 0x25C6: case 0x25C7: return '*';
    // Checkmark / ballot
    case 0x2713: case 0x2714: return 'x';
    case 0x2717: case 0x2718: return 'X';
    default: return '?';
    }
}

void ssh_console_write_cb(char* str, size_t len) {
    // NOOP
}

pax_buf_t ssh_bg_pax_buf = {0};



static bool load_ssh_bg(void) {
    int backgrounds = 0;
    int randbgno = 0;
    DIR *d;
    struct dirent *dir;
    char bgfilename[PATH_MAX];
    
    //ESP_LOGI(TAG, "trying to opendir(/sd/bg)`");
    d = opendir("/sd/bg");
    if (!d) {
	ESP_LOGI(TAG, "no background images directory found");
	return false;
    }

    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type==DT_REG) {
            //ESP_LOGI(TAG, "found file %s\n", dir->d_name);
            backgrounds++;
        }
    }
    closedir(d);
    
    if (backgrounds == 0) {
	ESP_LOGI(TAG, "background images directory was empty - nothing loaded");
	return false;
    }

    //ESP_LOGI(TAG, "choosing a random background from %d", backgrounds + 1);
    randbgno = rand() % (backgrounds + 1);
    //ESP_LOGI(TAG, "picked number %d", randbgno);
    sprintf(bgfilename, "/sd/bg/%02d.png", randbgno);
    //ESP_LOGI(TAG, "which is filename %s", bgfilename);

    FILE* fd = fopen(bgfilename, "rb");
    if (fd == NULL) {
        ESP_LOGE(TAG, "Failed to open background image file");
        return false;
    }
    if (!pax_decode_png_fd(&ssh_bg_pax_buf, fd, PAX_BUF_32_8888ARGB, 0)) {  // CODEC_FLAG_EXISTING)) {
        ESP_LOGE(TAG, "Failed to decode png file");
        return false;
    }
    fclose(fd);
    return true;
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

static void screenshot_to_uart(struct cons_insts_s *inst) {
    ESP_LOGI(TAG, "=== SCREENSHOT START ===");
    if (!screen_buf) {
        ESP_LOGE(TAG, "screenshot: screen buffer not allocated");
        ESP_LOGI(TAG, "=== SCREENSHOT END ===");
        return;
    }
    char *line = malloc(screen_cols + 1);
    if (!line) {
        ESP_LOGE(TAG, "screenshot: failed to allocate line buffer");
        return;
    }
    for (size_t y = 0; y < screen_rows; y++) {
        memcpy(line, screen_buf + y * screen_cols, screen_cols);
        size_t len = screen_cols;
        while (len > 0 && line[len - 1] == ' ') {
            len--;
        }
        line[len] = '\0';
        ESP_LOGI(TAG, "|%s", line);
    }
    free(line);
    ESP_LOGI(TAG, "=== SCREENSHOT END ===");
}

static void display_backlight(void) {
    uint8_t brightness;
    bsp_display_get_backlight_brightness(&brightness);
    brightness += 15;
    if (brightness > 100) {
        brightness = 10;
    }
    ESP_LOGI(TAG, "Display brightness: %u%%\r\n", brightness);
    bsp_display_set_backlight_brightness(brightness);
}

// XXX this is the main function but it's getting a bit unwieldy - consider breaking up into functional parts
void util_ssh(pax_buf_t* buffer, gui_theme_t* theme, ssh_settings_t* settings, uint8_t connection_index) {
    QueueHandle_t input_event_queue = NULL;
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    struct cons_config_s con_conf = {
        .font = pax_font_sky_mono, 
	.font_size_mult = 1.5, 
	.paxbuf = display_get_buffer(), 
	.output_cb = ssh_console_write_cb
    };

    ssize_t nbytes; // bytes read from ssh server
    int rc; // return code from libssh2 library calls
    int i;
    struct sockaddr_in ssh_addr;
    char *ssh_buffer = NULL;
    char dialog_buffer[256];
    char ssh_password[128];
    LIBSSH2_SESSION *ssh_session;
    LIBSSH2_CHANNEL *ssh_channel;
    libssh2_socket_t ssh_sock;
    char ssh_out = '\0';
    const char *ssh_hostkey = '\0';
    char ssh_printable_fingerprint[128];
    size_t ssh_hostkey_len;
    int ssh_hostkey_type;
    char *ssh_userauthlist = '\0';
    int cx = 0; // old cursor x position
    int cy = 0; // old cursor y position
    int ocx = 0; // old cursor x position
    int ocy = 0; // old cursor y position

    console_init(&console_instance, &con_conf);
    screen_buf_init(console_instance.chars_x, console_instance.chars_y);
    //console_set_colors(&console_instance, CONS_COL_VGA_GREEN, CONS_COL_VGA_BLACK);
    console_instance.fg = 0xff00ff00;
    console_instance.bg = 0xff000000;
    decckm_mode = false;
    keyboard_backlight();

    //busy_dialog(get_icon(ICON_TERMINAL), "SSH", "Connecting to WiFi...");
    console_printf(&console_instance, "\nConnecting to WiFi...\n");
    display_blit_buffer(buffer);

    if (!wifi_stack_get_initialized()) {
        ESP_LOGE(TAG, "WiFi stack not initialized");
        message_dialog(get_icon(ICON_TERMINAL), "SSH: fatal error", "WiFi stack not initialized", "Quit");
        return;
    }

    if (!wifi_connection_is_connected()) {
        if (wifi_connect_try_all() != ESP_OK) {
            ESP_LOGE(TAG, "Not connected to WiFi");
            message_dialog(get_icon(ICON_TERMINAL), "SSH: fatal error", "Failed to connect to WiFi network", "Quit");
            return;
        }
    }

    //ESP_LOGI(TAG, "initialising libssh2");
    console_printf(&console_instance, "Initialising libssh2...\n");
    display_blit_buffer(buffer);
    rc = libssh2_init(0);
    if (rc) {
        ESP_LOGE(TAG, "libssh2 initialization failed (%d)", rc);
        return;
    }

    ESP_LOGI(TAG, "setting up destination host IP address and port");
    // TODO: check if any changes needed for IPv6 support
    // TODO: check if any changes needed for DNS lookup of hostnames
    inet_pton(AF_INET, settings->dest_host, &ssh_addr.sin_addr);
    ssh_addr.sin_port = htons(atoi(settings->dest_port));
    ssh_addr.sin_family = AF_INET;

    ESP_LOGI(TAG, "creating socket to use for ssh session");
    ssh_sock = socket(AF_INET, SOCK_STREAM, 0);
    /*if (ssh_sock == LIBSSH2_INVALID_SOCKET) {
        ESP_LOGE(TAG, "failed to create socket");
        return;
    }*/

    ESP_LOGI(TAG, "connecting...");
    console_printf(&console_instance, "Connecting...\n");
    display_blit_buffer(buffer);
    if (connect(ssh_sock, (struct sockaddr*)&ssh_addr, sizeof(ssh_addr))) {
        ESP_LOGE(TAG, "failed to connect.");
        return;
    }

    console_printf(&console_instance, "Starting SSH session...\n");
    display_blit_buffer(buffer);
    ESP_LOGI(TAG, "initialising session");
    ssh_session = libssh2_session_init();
    if (!ssh_session) {
        ESP_LOGE(TAG, "could not initialize SSH session");
        return;
    }

    // XXX we can do verbose ssh debugging if needed... 
    //libssh2_trace(ssh_session, ~0);
    // TODO: display server banner?
    // TODO: display the info we're currently logging about connection setup
    // TODO: condense multiple dialogs at connection time down to one

    ESP_LOGI(TAG, "session handshake");
    console_printf(&console_instance, "Session handshake...\n");
    rc = libssh2_session_handshake(ssh_session, ssh_sock);
    if (rc) {
        ESP_LOGE(TAG, "failure establishing SSH session: %d", rc);
        return;
    }

    // Fetch and verify host key
    ESP_LOGI(TAG, "fetching destination host key");
    console_printf(&console_instance, "Fetching host key...\n");
    display_blit_buffer(buffer);
    ssh_hostkey = libssh2_session_hostkey(ssh_session, &ssh_hostkey_len, &ssh_hostkey_type);
    if (!ssh_hostkey) {
        ESP_LOGE(TAG, "failed to get host key");
        message_dialog(get_icon(ICON_ERROR), "SSH error", "Failed to get server host key", "Quit");
        libssh2_session_free(ssh_session);
        shutdown(ssh_sock, 2);
        LIBSSH2_SOCKET_CLOSE(ssh_sock);
        libssh2_exit();
        return;
    }

    ESP_LOGI(TAG, "remote host key len: %d, type: %d", (int)ssh_hostkey_len, ssh_hostkey_type);

    // Get SHA256 fingerprint for display and storage
    const char *sha256_hash = libssh2_hostkey_hash(ssh_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!sha256_hash) {
        ESP_LOGE(TAG, "failed to compute host key SHA256 hash");
        message_dialog(get_icon(ICON_ERROR), "SSH error", "Failed to compute host key fingerprint", "Quit");
        libssh2_session_free(ssh_session);
        shutdown(ssh_sock, 2);
        LIBSSH2_SOCKET_CLOSE(ssh_sock);
        libssh2_exit();
        return;
    }

    // Format printable fingerprint (SHA256)
    bzero(ssh_printable_fingerprint, sizeof(ssh_printable_fingerprint));
    char* j = ssh_printable_fingerprint;
    for (i = 0; i < 32; i++) {
        sprintf(j, "%02X", (unsigned char)sha256_hash[i]);
        j += 2;
        if (i < 31) { *j++ = ':'; }
    }
    ESP_LOGI(TAG, "Host key SHA256 fingerprint: %s", ssh_printable_fingerprint);
    console_printf(&console_instance, "SHA256: %s\n", ssh_printable_fingerprint);
    display_blit_buffer(buffer);

    // Check host key against saved fingerprint in NVS
    ESP_LOGI(TAG, "checking host key against saved fingerprint");
    console_printf(&console_instance, "Checking saved host key...\n");
    display_blit_buffer(buffer);

    uint8_t saved_sha256[32] = {0};
    esp_err_t hk_res = ssh_settings_get_host_key(connection_index, saved_sha256);

    if (hk_res == ESP_OK) {
        // We have a saved fingerprint - compare
        if (memcmp(saved_sha256, sha256_hash, 32) == 0) {
            ESP_LOGI(TAG, "host key matches saved fingerprint");
            console_printf(&console_instance, "Host key verified.\n");
            display_blit_buffer(buffer);
        } else {
            // MISMATCH - this is serious
            ESP_LOGW(TAG, "HOST KEY MISMATCH - possible MITM attack");
            snprintf(dialog_buffer, sizeof(dialog_buffer),
                "WARNING: Host key has CHANGED!\n\n"
                "This could indicate a man-in-the-middle attack.\n\n"
                "SHA256: %.65s...\n\n"
                "Accept new key?", ssh_printable_fingerprint);
            int dialog_rc = adv_dialog_yes_no(get_icon(ICON_ERROR), "Host key mismatch!", dialog_buffer);
            if (dialog_rc == MSG_DIALOG_RETURN_NO) {
                ESP_LOGI(TAG, "user rejected changed host key");
                libssh2_session_free(ssh_session);
                shutdown(ssh_sock, 2);
                LIBSSH2_SOCKET_CLOSE(ssh_sock);
                libssh2_exit();
                return;
            }
            // User accepted the new key - save it
            ESP_LOGI(TAG, "user accepted new host key, saving");
            ssh_settings_set_host_key(connection_index, (const uint8_t*)sha256_hash);
        }
    } else {
        // No saved fingerprint - first connection to this host
        ESP_LOGI(TAG, "no saved host key, first connection");
        snprintf(dialog_buffer, sizeof(dialog_buffer),
            "New host - no saved key.\n\n"
            "SHA256: %.65s...\n\n"
            "Trust this host?", ssh_printable_fingerprint);
        int dialog_rc = adv_dialog_yes_no(get_icon(ICON_HELP), "New SSH host key", dialog_buffer);
        if (dialog_rc == MSG_DIALOG_RETURN_NO) {
            ESP_LOGI(TAG, "user rejected new host key");
            libssh2_session_free(ssh_session);
            shutdown(ssh_sock, 2);
            LIBSSH2_SOCKET_CLOSE(ssh_sock);
            libssh2_exit();
            return;
        }
        // Save the fingerprint for future connections
        ESP_LOGI(TAG, "user accepted host key, saving to NVS");
        ssh_settings_set_host_key(connection_index, (const uint8_t*)sha256_hash);
    }

    pax_draw_rect(buffer, 0xff000000, 0, 0, 800, 480);
    display_blit_buffer(buffer);

    ESP_LOGI(TAG, "user auth methods check");
    ssh_userauthlist = libssh2_userauth_list(ssh_session, settings->username, (unsigned int)strlen(settings->username));
    ESP_LOGI(TAG, "user auth methods list: %s", ssh_userauthlist);
    console_printf(&console_instance, "Host supports auth methods... %s\n", ssh_userauthlist);

    bool authenticated = false;

    // Try public key authentication first if a private key is stored
    if (ssh_settings_has_private_key(connection_index)) {
        ESP_LOGI(TAG, "private key found, attempting public key authentication");
        console_printf(&console_instance, "Trying public key authentication...\n");
        display_blit_buffer(buffer);

        uint8_t* privkey_data = NULL;
        size_t privkey_len = 0;
        esp_err_t key_res = ssh_settings_get_private_key(connection_index, &privkey_data, &privkey_len);
        if (key_res == ESP_OK) {
            // Use password as passphrase for the key if one is configured
            const char* passphrase = (strlen(settings->password) > 0) ? settings->password : NULL;

            rc = libssh2_userauth_publickey_frommemory(
                ssh_session, settings->username, strlen(settings->username),
                NULL, 0,
                (const char*)privkey_data, privkey_len,
                passphrase);

            free(privkey_data);

            if (rc == 0) {
                ESP_LOGI(TAG, "public key authentication succeeded");
                console_printf(&console_instance, "Public key authentication succeeded.\n");
                display_blit_buffer(buffer);
                authenticated = true;
            } else {
                ESP_LOGW(TAG, "public key authentication failed (%d), falling back to password", rc);
                console_printf(&console_instance, "Public key auth failed, trying password...\n");
                display_blit_buffer(buffer);
            }
        } else {
            ESP_LOGW(TAG, "failed to read private key from NVS (%d)", key_res);
        }
    }

    // Fall back to password authentication if pubkey auth didn't succeed
    if (!authenticated) {
        if (strlen(settings->password) > 0) {
            ESP_LOGI(TAG, "using saved password");
            strncpy(ssh_password, settings->password, sizeof(ssh_password) - 1);
            ssh_password[sizeof(ssh_password) - 1] = '\0';
        } else {
            memset(ssh_password, 0, sizeof(ssh_password));
        }

        int max_auth_attempts = 3;
        for (int attempt = 0; attempt < max_auth_attempts; attempt++) {
            if (strlen(ssh_password) == 0) {
                bool accepted = false;
                menu_textedit(buffer, theme, "Password", ssh_password, sizeof(ssh_password), true, &accepted);
                if (!accepted || strlen(ssh_password) == 0) {
                    ESP_LOGI(TAG, "user cancelled password entry");
                    libssh2_session_disconnect(ssh_session, "User cancelled authentication");
                    libssh2_session_free(ssh_session);
                    shutdown(ssh_sock, 2);
                    LIBSSH2_SOCKET_CLOSE(ssh_sock);
                    libssh2_exit();
                    return;
                }
            }

            ESP_LOGI(TAG, "authenticating to %s:%s as user %s (attempt %d/%d)",
                     settings->dest_host, settings->dest_port, settings->username, attempt + 1, max_auth_attempts);
            console_printf(&console_instance, "Authenticating to %s:%s as %s...\n",
                           settings->dest_host, settings->dest_port, settings->username);
            display_blit_buffer(buffer);

            if (libssh2_userauth_password(ssh_session, settings->username, ssh_password) == 0) {
                ESP_LOGI(TAG, "authentication by password succeeded");
                break;
            }

            ESP_LOGE(TAG, "authentication by password failed (attempt %d/%d)", attempt + 1, max_auth_attempts);
            memset(ssh_password, 0, sizeof(ssh_password));

            if (attempt + 1 < max_auth_attempts) {
                message_dialog(get_icon(ICON_ERROR), "Authentication failed",
                              "Incorrect password. Please try again.", "Retry");
            } else {
                message_dialog(get_icon(ICON_ERROR), "Authentication failed",
                              "Too many failed attempts.", "Go back");
                libssh2_session_disconnect(ssh_session, "Authentication failed");
                libssh2_session_free(ssh_session);
                shutdown(ssh_sock, 2);
                LIBSSH2_SOCKET_CLOSE(ssh_sock);
                libssh2_exit();
                return;
            }
        }
    }

    // TODO: Support keyboard_interactive auth
    // TODO: Support agent auth

    ESP_LOGI(TAG, "requesting session");
    console_printf(&console_instance, "Requesting ssh session...\n");
    ssh_channel = libssh2_channel_open_session(ssh_session);
    if (!ssh_channel) {
        ESP_LOGE(TAG, "unable to open a session");
        return;
    }

    //ESP_LOGI(TAG, "sending env variables");
    libssh2_channel_setenv(ssh_channel, "LANG", "en_US.UTF-8");

    ESP_LOGI(TAG, "requesting pty");
    console_printf(&console_instance, "Requesting pseudoterminal for interactive login session...\n");
    // Use actual terminal dimensions from the console instance
    int pty_cols = console_instance.chars_x;
    int pty_rows = console_instance.chars_y;
    int pty_width_px = pax_buf_get_width(buffer);
    int pty_height_px = pax_buf_get_height(buffer);
    ESP_LOGI(TAG, "PTY size: %d cols x %d rows (%d x %d px)", pty_cols, pty_rows, pty_width_px, pty_height_px);
    if (libssh2_channel_request_pty_ex(ssh_channel, "xterm-256color", strlen("xterm-256color"),
                                        NULL, 0, pty_cols, pty_rows, pty_width_px, pty_height_px)) {
        ESP_LOGE(TAG, "failed requesting pty");
        return;
    }

    // TODO: check whether libssh2_channel_shell is required
    if (libssh2_channel_shell(ssh_channel)) {
        ESP_LOGE(TAG, "failed requesting shell");
        return;
    }

    ESP_LOGI(TAG, "making the channel non-blocking");
    libssh2_channel_set_blocking(ssh_channel, 0);

    // TODO: make background image loading into a task, so it doesn't hold everything else up
    // TODO: see if we can find a way to stop background image from scrolling
    // TODO: function key switches between background images?
    ESP_LOGI(TAG, "trying to load background image");
    console_printf(&console_instance, "Looking for background images...\n");
    load_ssh_bg();
    console_clear(&console_instance);
    console_set_cursor(&console_instance, 0, 0);
    pax_draw_image(buffer, &ssh_bg_pax_buf, 0, 0);
    display_blit_buffer(buffer);

    ESP_LOGI(TAG, "ssh setup completed, entering main loop");

    ssh_buffer = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!ssh_buffer) {
        ESP_LOGE(TAG, "failed to allocate SSH read buffer");
        goto shutdown;
    }

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
            //ESP_LOGI(TAG, "input received");
            switch (event.type) {
                case INPUT_EVENT_TYPE_KEYBOARD:
		    ssh_out = event.args_keyboard.ascii;
		    // Fn + number keys → send F-key escape sequences
		    if (event.args_keyboard.modifiers & BSP_INPUT_MODIFIER_FUNCTION) {
		        const char* fseq = NULL;
		        if (ssh_out >= '0' && ssh_out <= '9') {
		            int idx = ssh_out - '0';
		            fseq = fkey_sequences[idx];
		        } else if (ssh_out == '-') {
		            fseq = fkey_sequences[10]; // F11
		        } else if (ssh_out == '=') {
		            fseq = fkey_sequences[11]; // F12
		        }
		        if (fseq) {
		            libssh2_channel_write(ssh_channel, fseq, strlen(fseq));
		            break;
		        }
		    }
		    if (event.args_keyboard.modifiers & BSP_INPUT_MODIFIER_CTRL) {
			if (ssh_out == 'r' || ssh_out == 'R') {
			    screenshot_to_uart(&console_instance);
			    break;
			}
                        ssh_out &= 0x1f;
		    }
		    // Skip characters that are also handled as navigation events to avoid double-send
		    if (ssh_out == '\t' || ssh_out == '\n' || ssh_out == '\r' || ssh_out == '\x1b' || ssh_out == '\x7f' || ssh_out == '\b') {
		        break;
		    }
                    libssh2_channel_write(ssh_channel, &ssh_out, sizeof(ssh_out));
                    break;
		case INPUT_EVENT_TYPE_NONE:
		    ESP_LOGI(TAG, "input is a non-event");
		    break;
		case INPUT_EVENT_TYPE_ACTION:
		    ESP_LOGI(TAG, "input is an action event");
		    break;
		case INPUT_EVENT_TYPE_SCANCODE:
		    //ESP_LOGI(TAG, "input is a scancode event");
		    break;
                case INPUT_EVENT_TYPE_NAVIGATION:
		    //ESP_LOGI(TAG, "input is a navigation event");
                    if (event.args_navigation.state) {
		        //ESP_LOGI(TAG, "checking to see which navigation key/button has been pressed");
                        switch (event.args_navigation.key) {
                            case BSP_INPUT_NAVIGATION_KEY_ESC:
				ESP_LOGI(TAG, "esc key pressed");
				ssh_out = '\e';
                                libssh2_channel_write(ssh_channel, &ssh_out, 1);
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F1:
				ESP_LOGI(TAG, "close key pressed - returning to app launcher");
				// TODO: ask if they really want to close the connection when they hit F1
				goto shutdown;
                                return;
                            case BSP_INPUT_NAVIGATION_KEY_F2:
				ESP_LOGI(TAG, "keyboard backlight toggle");
				keyboard_backlight();
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F3:
				ESP_LOGI(TAG, "display backlight toggle");
				display_backlight();
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F6:
				ESP_LOGI(TAG, "colour randomiser");
	        		//printf("clearing cursor visual at old cursor position... %d, %d\n", ocx, ocy);
  	        		pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
    				console_init(&console_instance, &con_conf);
    				screen_buf_init(console_instance.chars_x, console_instance.chars_y);
    				console_clear(&console_instance);
                                int randfg = (rand() % 0xffffff) & 0xff000000;
                                int randbg = (rand() % 0xffffff) & 0xff000000;
	                        console_instance.fg = randfg;
	                        console_instance.bg = randbg;
				fprintf(stderr, "fg: %08x, bg: %08x\n", randfg, randbg);
				console_set_cursor(&console_instance, 0, 0);
				cx = cy = ocx = ocy = 0;
  	        		pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
                                libssh2_channel_write(ssh_channel, CHR_NL, 1);
                                display_blit_buffer(buffer);
				break;
			    case BSP_INPUT_NAVIGATION_KEY_LEFT:
				ESP_LOGI(TAG, "left key pressed");
                                if (decckm_mode) {
                                    libssh2_channel_write(ssh_channel, SS3_LEFT, strlen(SS3_LEFT));
                                } else {
                                    libssh2_channel_write(ssh_channel, CSI_LEFT, strlen(CSI_LEFT));
                                }
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
				ESP_LOGI(TAG, "right key pressed");
                                if (decckm_mode) {
                                    libssh2_channel_write(ssh_channel, SS3_RIGHT, strlen(SS3_RIGHT));
                                } else {
                                    libssh2_channel_write(ssh_channel, CSI_RIGHT, strlen(CSI_RIGHT));
                                }
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_UP:
				ESP_LOGI(TAG, "up key pressed");
                                if (decckm_mode) {
                                    libssh2_channel_write(ssh_channel, SS3_UP, strlen(SS3_UP));
                                } else {
                                    libssh2_channel_write(ssh_channel, CSI_UP, strlen(CSI_UP));
                                }
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_DOWN:
				ESP_LOGI(TAG, "down key pressed");
                                if (decckm_mode) {
                                    libssh2_channel_write(ssh_channel, SS3_DOWN, strlen(SS3_DOWN));
                                } else {
                                    libssh2_channel_write(ssh_channel, CSI_DOWN, strlen(CSI_DOWN));
                                }
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_TAB:
				ESP_LOGI(TAG, "tab key pressed");
                                libssh2_channel_write(ssh_channel, CHR_TAB, 1);
				break;
			    case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
				ESP_LOGI(TAG, "volume up key pressed");
	        		printf("clearing cursor visual at old cursor position... %d, %d\n", ocx, ocy);
  	        		pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
                                con_conf.font_size_mult += 0.3;
    				console_init(&console_instance, &con_conf);
    				screen_buf_init(console_instance.chars_x, console_instance.chars_y);
    				console_clear(&console_instance);
				console_set_cursor(&console_instance, 0, 0);
				cx = cy = ocx = ocy = 0;
  	        		pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
	                        console_instance.fg = 0xff00ff00;
	                        console_instance.bg = 0x00000000;
                                libssh2_channel_write(ssh_channel, CHR_NL, 1);
                                display_blit_buffer(buffer);
				break;
			    case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
				ESP_LOGI(TAG, "volume down key pressed");
	        		//printf("clearing cursor visual at old cursor position... %d, %d\n", ocx, ocy);
  	        		pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
                                con_conf.font_size_mult -= 0.3;
    				console_init(&console_instance, &con_conf);
    				screen_buf_init(console_instance.chars_x, console_instance.chars_y);
    				console_clear(&console_instance);
				console_set_cursor(&console_instance, 0, 0);
				cx = cy = ocx = ocy = 0;
  	        		pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
	                        console_instance.fg = 0xff00ff00;
	                        console_instance.bg = 0x00000000;
                                libssh2_channel_write(ssh_channel, CHR_NL, 1);
                                display_blit_buffer(buffer);
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
				ESP_LOGI(TAG, "backspace key pressed");
                                rc = libssh2_channel_write(ssh_channel, CHR_BS, 1);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RETURN:
				ESP_LOGI(TAG, "return key pressed");
	        		//printf("clearing cursor visual at old cursor position... %d, %d\n", ocx, ocy);
  	        		pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
    				//ESP_LOGI(TAG, "redrawing background image");
				// XXX this is too slow to do every time return is pressed
				//pax_draw_image(buffer, &ssh_bg_pax_buf, 0, 0);
                                libssh2_channel_write(ssh_channel, CHR_NL, 1);
                                break;
			    // TODO: handle control key combinations
			    // TODO: improve escape character processing so we can use vi, emacs etc
			    // TODO: light/dark mode - maybe use a function key to toggle through several presets?
                            // TODO: font size +/-
                            // TODO: stretch goal: themes - fg/bg colours, fonts, text size
                            // TODO: connect/disconnect cleanly - code can be cribbed from libssh2 ssh2_exec demo
                            // TODO: change wifi network? or maybe tie wifi network to ssh connection details
                            default:
				ESP_LOGI(TAG, "some other navigation key has been pressed");
                                break;
                        }
			default:
			    break;
		    }
		case INPUT_EVENT_TYPE_LAST:
		    break;
	    }
        }

        if (libssh2_channel_eof(ssh_channel)) {
            ESP_LOGI(TAG, "server sent EOF");
            goto shutdown;
            break;
        }

        // Read available data from SSH channel, batching into a single display
        // update. Cap at ~50ms to keep the screen responsive during large output.
        bool got_data = false;
        int64_t read_start = esp_timer_get_time();
        do {
            nbytes = libssh2_channel_read(ssh_channel, ssh_buffer, 8192);
            if (nbytes <= 0) break;
            got_data = true;

            // Parse ANSI escape sequences
            char* p = ssh_buffer;
            char* end = ssh_buffer + nbytes;
            while (p < end) {
                if (*p == '\x1b' && p + 1 < end && *(p+1) == ']') {
                    // OSC sequence: ESC] ... BEL/ST - skip entirely
                    p += 2;
                    while (p < end) {
                        if (*p == '\x07') { p++; break; }
                        if (*p == '\x1b' && p + 1 < end
                            && *(p+1) == '\\') {
                            p += 2; break;
                        }
                        p++;
                    }
                } else if (*p == '\x1b' && p + 1 < end
                           && *(p+1) == '[') {
                    // CSI sequence: ESC[
                    p += 2;
                    char seq[32] = {0};
                    int si = 0;
                    while (p < end && si < 31) {
                        if ((*p >= '0' && *p <= '9') || *p == ';' || *p == '?') {
                            seq[si++] = *p++;
                        } else {
                            break;
                        }
                    }
                    if (p < end) {
                        char cmd = *p++;
                        if (cmd == 'J' && strcmp(seq, "2") == 0) {
                            screen_buf_clear();
                            console_clear(&console_instance);
                            pax_draw_rect(buffer, 0xff000000, 0, 0,
                                          pax_buf_get_width(buffer), pax_buf_get_height(buffer));
                            if (ssh_bg_pax_buf.width > 0) {
                                pax_draw_image(buffer, &ssh_bg_pax_buf, 0, 0);
                            }
                            console_set_cursor(&console_instance, 0, 0);
                            cx = cy = ocx = ocy = 0;
                        } else if (cmd == 'H' || cmd == 'f') {
                            int row = 1, col = 1;
                            if (seq[0]) sscanf(seq, "%d;%d", &row, &col);
                            console_set_cursor(&console_instance, col - 1, row - 1);
                        } else if (cmd == 'h' && strcmp(seq, "?1") == 0) {
                            decckm_mode = true;
                        } else if (cmd == 'l' && strcmp(seq, "?1") == 0) {
                            decckm_mode = false;
                        } else {
                            char esc_seq[64];
                            snprintf(esc_seq, sizeof(esc_seq), "\x1b[%s%c", seq, cmd);
                            console_puts(&console_instance, esc_seq);
                        }
                    }
                } else if (*p == 0x08) {
                    if (console_instance.cursor_x > 0) {
                        console_instance.cursor_x--;
                        screen_buf_put(console_instance.cursor_x,
                                       console_instance.cursor_y, ' ');
                        int erase_x = console_instance.char_width * console_instance.cursor_x;
                        int erase_y = console_instance.char_height * console_instance.cursor_y;
                        pax_draw_rect(buffer, console_instance.bg,
                                     erase_x, erase_y,
                                     console_instance.char_width, console_instance.char_height);
                    }
                    p++;
                } else if ((unsigned char)*p >= 0xC0) {
                    // UTF-8 multi-byte sequence
                    unsigned char lead = (unsigned char)*p;
                    int seq_len;
                    uint32_t cp;
                    if (lead < 0xE0)      { seq_len = 2; cp = lead & 0x1F; }
                    else if (lead < 0xF0) { seq_len = 3; cp = lead & 0x0F; }
                    else                  { seq_len = 4; cp = lead & 0x07; }
                    p++;
                    bool valid = true;
                    for (int ub = 1; ub < seq_len && p < end; ub++) {
                        if (((unsigned char)*p & 0xC0) != 0x80) {
                            valid = false;
                            break;
                        }
                        cp = (cp << 6) | ((unsigned char)*p & 0x3F);
                        p++;
                    }
                    if (valid) {
                        char ascii = utf8_to_ascii(cp);
                        screen_buf_track_put(&console_instance, ascii);
                        console_put(&console_instance, ascii);
                    }
                } else if ((unsigned char)*p >= 0x80) {
                    // Stray UTF-8 continuation byte, skip
                    p++;
                } else {
                    screen_buf_track_put(&console_instance, *p);
                    console_put(&console_instance, *p++);
                }
            }
        } while (nbytes > 0 && (esp_timer_get_time() - read_start) < 50000);

        if (got_data) {
            // Draw cursor
            cx = console_instance.char_width * console_instance.cursor_x;
            cy = console_instance.char_height * console_instance.cursor_y;
            if (ocx != 0 || ocy != 0) {
                pax_draw_line(buffer, 0xff000000, ocx, ocy, ocx, ocy + (console_instance.char_height - 1));
            }
            ocx = cx;
            ocy = cy;
            pax_draw_line(buffer, 0xffefefef, cx, cy, cx, cy + (console_instance.char_height - 1));

            display_blit_buffer(buffer);
        }
    }
 
    // closing the ssh connection and freeing resources
    // could be due to user action, or an error
 shutdown:
    ESP_LOGI(TAG, "in shutdown, clearing the screen...");
    pax_draw_rect(buffer, 0xffefefef, 0, 0, 800, 480);
    display_blit_buffer(buffer);
    ESP_LOGI(TAG, "freeing memory...");
    libssh2_channel_send_eof(ssh_channel);
    libssh2_channel_close(ssh_channel);
    libssh2_session_disconnect(ssh_session, "User closed session");
    libssh2_channel_free(ssh_channel);
    if (ssh_sock != LIBSSH2_INVALID_SOCKET) {
        shutdown(ssh_sock, 2);
        LIBSSH2_SOCKET_CLOSE(ssh_sock);
    }
    libssh2_exit();
    free(ssh_buffer);
    free(screen_buf);
    screen_buf = NULL;
}
