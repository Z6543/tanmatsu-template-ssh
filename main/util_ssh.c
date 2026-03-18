//
// Derived from badgeteam/terminal-emulator, libssh2 example code, nicolaielectronics/tanmatsu-launcher
//
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "common/display.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "icons.h"
#include "message_dialog.h"
#include "textedit.h"
#include "pax_gfx.h"
#include "pax_codecs.h"
#include "wifi_connection.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <libssh2.h>
#include "lwip/sockets.h"
#include "util_ssh.h"
#include "settings_ssh.h"
#include "vterm.h"
#include "fastopen.h"

extern bool wifi_stack_get_initialized(void);

static char const TAG[] = "util_ssh";


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


// Simple status line drawing for pre-connection messages
static pax_buf_t *status_buf = NULL;
static int status_y = 0;

static void status_init(pax_buf_t *buf) {
    status_buf = buf;
    status_y = 0;
    pax_background(buf, 0xFF000000);
}

static void status_print(const char *fmt, ...) {
    if (!status_buf) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    pax_draw_text(status_buf, 0xFF00FF00, pax_font_sky_mono,
                  18, 0, status_y, line);
    status_y += 20;
}

// libvterm terminal emulator state
static VTerm *vt = NULL;
static VTermScreen *vt_screen = NULL;
static pax_buf_t *vt_pax_buf = NULL;
static const pax_font_t *vt_font = NULL;
static float vt_font_size = 0;
static int vt_char_width = 0;
static int vt_char_height = 0;
static int vt_cols = 0;
static int vt_rows = 0;
static LIBSSH2_CHANNEL *vt_ssh_channel = NULL;

// Dirty tracking: accumulated damage rect
static bool vt_dirty = false;
static VTermRect vt_dirty_rect = {0, 0, 0, 0};
static VTermPos vt_cursor_pos = {0, 0};
static bool vt_cursor_visible = true;


pax_buf_t ssh_bg_pax_buf = {0};

static char unicode_to_ascii(uint32_t ch) {
    if (ch >= 0x20 && ch <= 0x7e) return (char)ch;
    switch (ch) {
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
    case 0x2588: return '#';
    case 0x2591: case 0x2592: case 0x2593: return '#';
    case 0x2190: return '<';
    case 0x2191: return '^';
    case 0x2192: return '>';
    case 0x2193: return 'v';
    case 0x2022: case 0x2023: case 0x25CF: return '*';
    case 0x25CB: case 0x25E6: return 'o';
    case 0x25C6: case 0x25C7: return '*';
    case 0x2713: case 0x2714: return 'x';
    case 0x2717: case 0x2718: return 'X';
    default: return '?';
    }
}

static uint32_t vterm_color_to_pax(VTermColor col) {
    if (VTERM_COLOR_IS_INDEXED(&col)) {
        vterm_screen_convert_color_to_rgb(vt_screen, &col);
    }
    // Default and RGB colors both store values in .rgb fields
    return 0xFF000000 | ((uint32_t)col.rgb.red << 16) |
           ((uint32_t)col.rgb.green << 8) | col.rgb.blue;
}


static void vt_mark_dirty(VTermRect rect) {
    if (!vt_dirty) {
        vt_dirty_rect = rect;
        vt_dirty = true;
    } else {
        if (rect.start_row < vt_dirty_rect.start_row)
            vt_dirty_rect.start_row = rect.start_row;
        if (rect.end_row > vt_dirty_rect.end_row)
            vt_dirty_rect.end_row = rect.end_row;
        if (rect.start_col < vt_dirty_rect.start_col)
            vt_dirty_rect.start_col = rect.start_col;
        if (rect.end_col > vt_dirty_rect.end_col)
            vt_dirty_rect.end_col = rect.end_col;
    }
}

// Render a row cell-by-cell
static void vt_render_row(int row, int start_col, int end_col) {
    int py = vt_char_height * row;
    int px_start = vt_char_width * start_col;
    int width = vt_char_width * (end_col - start_col);

    // Get default bg via API (direct struct access has ABI issues)
    VTermColor api_fg, api_bg;
    vterm_state_get_default_colors(vterm_obtain_state(vt),
                                   &api_fg, &api_bg);
    uint32_t default_bg = vterm_color_to_pax(api_bg);

    // Clear the row background in one call
    pax_draw_rect(vt_pax_buf, default_bg, px_start, py,
                  width, vt_char_height);

    // Draw each cell using ABI-safe color extraction
    char ch_buf[2] = {0, 0};
    for (int col = start_col; col < end_col; col++) {
        VTermPos pos = {.row = row, .col = col};
        VTermColor cell_fg, cell_bg;
        uint32_t ch;
        int reverse;
        if (!vterm_screen_get_cell_colors(vt_screen, pos,
                &cell_fg, &cell_bg, &ch, &reverse))
            continue;

        uint32_t bg = vterm_color_to_pax(cell_bg);
        uint32_t fg = vterm_color_to_pax(cell_fg);
        if (reverse) {
            uint32_t tmp = bg; bg = fg; fg = tmp;
        }

        // Draw cell background if different from default
        if (bg != default_bg) {
            pax_draw_rect(vt_pax_buf, bg,
                vt_char_width * col, py,
                vt_char_width, vt_char_height);
        }

        // Only draw visible characters (skip spaces and empty)
        if (ch != 0 && ch != ' ') {
            ch_buf[0] = unicode_to_ascii(ch);
            if (ch_buf[0] != ' ') {
                pax_draw_text(vt_pax_buf, fg, vt_font,
                    vt_font_size, vt_char_width * col, py,
                    ch_buf);
            }
        }
    }
}

// Render all dirty cells and draw cursor
static void vt_render_dirty(void) {
    if (vt_dirty) {
        for (int row = vt_dirty_rect.start_row;
             row < vt_dirty_rect.end_row; row++) {
            vt_render_row(row, vt_dirty_rect.start_col,
                          vt_dirty_rect.end_col);
        }
        vt_dirty = false;
    }

    if (vt_cursor_visible &&
        vt_cursor_pos.row < vt_rows &&
        vt_cursor_pos.col < vt_cols) {
        int px = vt_char_width * vt_cursor_pos.col;
        int py = vt_char_height * vt_cursor_pos.row;
        pax_draw_line(vt_pax_buf, 0xffefefef, px, py,
                      px, py + vt_char_height - 1);
    }
}

static int vt_on_damage(VTermRect rect, void *user) {
    (void)user;
    vt_mark_dirty(rect);
    return 1;
}

static int vt_on_moverect(VTermRect dest, VTermRect src, void *user) {
    (void)user;
    vt_mark_dirty(dest);
    vt_mark_dirty(src);
    return 1;
}

static int vt_on_movecursor(VTermPos pos, VTermPos oldpos,
                            int visible, void *user) {
    (void)user;
    // Mark old cursor position dirty so it gets redrawn
    VTermRect old_r = {oldpos.row, oldpos.row + 1,
                       oldpos.col, oldpos.col + 1};
    vt_mark_dirty(old_r);
    vt_cursor_pos = pos;
    vt_cursor_visible = visible;
    return 1;
}

static int vt_on_settermprop(VTermProp prop, VTermValue *val,
                             void *user) {
    (void)user;
    (void)val;
    if (prop == VTERM_PROP_ALTSCREEN) {
        VTermRect full = {0, vt_rows, 0, vt_cols};
        vt_mark_dirty(full);
    }
    return 1;
}

static int vt_on_bell(void *user) { (void)user; return 1; }
static int vt_on_resize(int rows, int cols, void *user) {
    (void)user; (void)rows; (void)cols; return 1;
}

static VTermScreenCallbacks vt_screen_cbs = {
    .damage = vt_on_damage,
    .moverect = vt_on_moverect,
    .movecursor = vt_on_movecursor,
    .settermprop = vt_on_settermprop,
    .bell = vt_on_bell,
    .resize = vt_on_resize,
};

static void vt_output_cb(const char *s, size_t len, void *user) {
    (void)user;
    if (vt_ssh_channel) {
        libssh2_channel_write(vt_ssh_channel, s, len);
    }
}

static void vt_init(int rows, int cols) {
    if (vt) vterm_free(vt);
    vt = vterm_new(rows, cols);
    vterm_set_utf8(vt, 1);
    vt_screen = vterm_obtain_screen(vt);
    vterm_screen_set_callbacks(vt_screen, &vt_screen_cbs, NULL);
    vterm_screen_set_damage_merge(vt_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(vt_screen, 1);
    vterm_screen_reset(vt_screen, 1);

    VTermColor fg, bg;
    vterm_color_rgb(&fg, 0x00, 0xff, 0x00);
    vterm_color_rgb(&bg, 0x00, 0x00, 0x00);
    vterm_screen_set_default_colors(vt_screen, &fg, &bg);

    vterm_output_set_callback(vt, vt_output_cb, NULL);
    vt_cols = cols;
    vt_rows = rows;
}

static void vt_render_full(void) {
    pax_draw_rect(vt_pax_buf, 0xff000000, 0, 0,
                  pax_buf_get_width(vt_pax_buf),
                  pax_buf_get_height(vt_pax_buf));
    if (ssh_bg_pax_buf.width > 0) {
        pax_draw_image(vt_pax_buf, &ssh_bg_pax_buf, 0, 0);
    }
    for (int row = 0; row < vt_rows; row++) {
        vt_render_row(row, 0, vt_cols);
    }
}

static void vt_compute_size(const pax_font_t *font, float mult) {
    pax_buf_t *buf = display_get_buffer();
    vt_font = font;
    vt_font_size = font->default_size * mult;
    vt_char_width = font->ranges->bitmap_mono.width * mult;
    vt_char_height = font->ranges->bitmap_mono.height * mult;
    vt_cols = pax_buf_get_width(buf) / vt_char_width;
    vt_rows = pax_buf_get_height(buf) / vt_char_height;
}

static void screenshot_to_uart(void) {
    ESP_LOGI(TAG, "=== SCREENSHOT START ===");
    if (!vt_screen) {
        ESP_LOGE(TAG, "screenshot: no vterm screen");
        ESP_LOGI(TAG, "=== SCREENSHOT END ===");
        return;
    }
    char *line = malloc(vt_cols + 1);
    if (!line) {
        ESP_LOGE(TAG, "screenshot: alloc failed");
        return;
    }
    for (int y = 0; y < vt_rows; y++) {
        VTermRect rect = {y, y + 1, 0, vt_cols};
        size_t len = vterm_screen_get_text(vt_screen, line,
                                           vt_cols, rect);
        while (len > 0 && line[len - 1] == ' ') len--;
        line[len] = '\0';
        ESP_LOGI(TAG, "|%s", line);
    }
    free(line);
    ESP_LOGI(TAG, "=== SCREENSHOT END ===");
}

static void save_screenshot(void) {
    if (!vt_pax_buf) return;

    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char filename[64];
    snprintf(filename, sizeof(filename),
        "/sd/ssh-%04d%02d%02d%02d%02d%02d.ppm",
        tm_info->tm_year + 1900, tm_info->tm_mon + 1,
        tm_info->tm_mday, tm_info->tm_hour,
        tm_info->tm_min, tm_info->tm_sec);

    FILE *f = fastopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "Cannot save screenshot (SD not mounted?)");
        return;
    }

    size_t w = pax_buf_get_width(vt_pax_buf);
    size_t h = pax_buf_get_height(vt_pax_buf);
    fprintf(f, "P6\n%zu %zu\n255\n", w, h);

    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            pax_col_t col = pax_get_pixel(vt_pax_buf, x, y);
            fputc((col >> 16) & 0xFF, f);
            fputc((col >> 8) & 0xFF, f);
            fputc(col & 0xFF, f);
        }
    }

    fastclose(f);
    ESP_LOGI(TAG, "Screenshot saved: %s", filename);
}

static bool load_ssh_bg(void) {
    int backgrounds = 0;
    int randbgno = 0;
    DIR *d;
    struct dirent *dir;
    char bgfilename[PATH_MAX];

    d = opendir("/sd/bg");
    if (!d) {
	ESP_LOGI(TAG, "no background images directory found");
	return false;
    }

    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type==DT_REG) {
            backgrounds++;
        }
    }
    closedir(d);

    if (backgrounds == 0) {
	ESP_LOGI(TAG, "background images directory was empty - nothing loaded");
	return false;
    }

    randbgno = rand() % (backgrounds + 1);
    sprintf(bgfilename, "/sd/bg/%02d.png", randbgno);

    FILE* fd = fastopen(bgfilename, "rb");
    if (fd == NULL) {
        ESP_LOGE(TAG, "Failed to open background image file");
        return false;
    }
    if (!pax_decode_png_fd(&ssh_bg_pax_buf, fd, PAX_BUF_32_8888ARGB, 0)) {
        ESP_LOGE(TAG, "Failed to decode png file");
        fastclose(fd);
        return false;
    }
    fastclose(fd);
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

    ssize_t nbytes; // bytes read from ssh server
    int rc; // return code from libssh2 library calls
    struct sockaddr_in ssh_addr;
    char *ssh_buffer = NULL;
    char dialog_buffer[256];
    char ssh_password[128];
    LIBSSH2_SESSION *ssh_session;
    LIBSSH2_CHANNEL *ssh_channel = NULL;
    libssh2_socket_t ssh_sock;
    char ssh_out = '\0';
    const char *ssh_hostkey = '\0';
    char ssh_printable_fingerprint[128];
    size_t ssh_hostkey_len;
    char *ssh_userauthlist = '\0';
    static float font_size_mult = 1.5;

    status_init(buffer);
    keyboard_backlight();

    status_print("Connecting to WiFi...");
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

    status_print("Initialising libssh2...\n");
    display_blit_buffer(buffer);
    rc = libssh2_init(0);
    if (rc) {
        ESP_LOGE(TAG, "libssh2 initialization failed (%d)", rc);
        return;
    }

    ESP_LOGI(TAG, "setting up destination host IP address and port");
    inet_pton(AF_INET, settings->dest_host, &ssh_addr.sin_addr);
    ssh_addr.sin_port = htons(atoi(settings->dest_port));
    ssh_addr.sin_family = AF_INET;

    ESP_LOGI(TAG, "creating socket to use for ssh session");
    ssh_sock = socket(AF_INET, SOCK_STREAM, 0);

    ESP_LOGI(TAG, "connecting...");
    status_print("Connecting...\n");
    display_blit_buffer(buffer);
    if (connect(ssh_sock, (struct sockaddr*)&ssh_addr, sizeof(ssh_addr))) {
        ESP_LOGE(TAG, "failed to connect.");
        return;
    }

    status_print("Starting SSH session...\n");
    display_blit_buffer(buffer);
    ESP_LOGI(TAG, "initialising session");
    ssh_session = libssh2_session_init();
    if (!ssh_session) {
        ESP_LOGE(TAG, "could not initialize SSH session");
        return;
    }

    ESP_LOGI(TAG, "session handshake");
    status_print("Session handshake...\n");
    rc = libssh2_session_handshake(ssh_session, ssh_sock);
    if (rc) {
        ESP_LOGE(TAG, "failure establishing SSH session: %d", rc);
        return;
    }

    // Fetch and verify host key
    ESP_LOGI(TAG, "fetching destination host key");
    status_print("Fetching host key...\n");
    display_blit_buffer(buffer);
    ssh_hostkey = libssh2_session_hostkey(ssh_session, &ssh_hostkey_len, NULL);
    if (!ssh_hostkey) {
        ESP_LOGE(TAG, "failed to get host key");
        message_dialog(get_icon(ICON_ERROR), "SSH error", "Failed to get server host key", "Quit");
        libssh2_session_free(ssh_session);
        shutdown(ssh_sock, 2);
        LIBSSH2_SOCKET_CLOSE(ssh_sock);
        libssh2_exit();
        return;
    }

    ESP_LOGI(TAG, "remote host key len: %d", (int)ssh_hostkey_len);

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

    bzero(ssh_printable_fingerprint, sizeof(ssh_printable_fingerprint));
    char* j = ssh_printable_fingerprint;
    for (int i = 0; i < 32; i++) {
        sprintf(j, "%02X", (unsigned char)sha256_hash[i]);
        j += 2;
        if (i < 31) { *j++ = ':'; }
    }
    ESP_LOGI(TAG, "Host key SHA256 fingerprint: %s", ssh_printable_fingerprint);
    status_print("SHA256: %s\n", ssh_printable_fingerprint);
    display_blit_buffer(buffer);

    ESP_LOGI(TAG, "checking host key against saved fingerprint");
    status_print("Checking saved host key...\n");
    display_blit_buffer(buffer);

    uint8_t saved_sha256[32] = {0};
    esp_err_t hk_res = ssh_settings_get_host_key(connection_index, saved_sha256);

    if (hk_res == ESP_OK) {
        if (memcmp(saved_sha256, sha256_hash, 32) == 0) {
            ESP_LOGI(TAG, "host key matches saved fingerprint");
            status_print("Host key verified.\n");
            display_blit_buffer(buffer);
        } else {
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
            ESP_LOGI(TAG, "user accepted new host key, saving");
            ssh_settings_set_host_key(connection_index, (const uint8_t*)sha256_hash);
        }
    } else {
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
        ESP_LOGI(TAG, "user accepted host key, saving to NVS");
        ssh_settings_set_host_key(connection_index, (const uint8_t*)sha256_hash);
    }

    pax_draw_rect(buffer, 0xff000000, 0, 0, 800, 480);
    display_blit_buffer(buffer);

    ESP_LOGI(TAG, "user auth methods check");
    ssh_userauthlist = libssh2_userauth_list(ssh_session, settings->username, (unsigned int)strlen(settings->username));
    ESP_LOGI(TAG, "user auth methods list: %s", ssh_userauthlist);
    status_print("Host supports auth methods... %s\n", ssh_userauthlist);

    bool authenticated = false;

    if (ssh_settings_has_private_key(connection_index)) {
        ESP_LOGI(TAG, "private key found, attempting public key authentication");
        status_print("Trying public key authentication...\n");
        display_blit_buffer(buffer);

        uint8_t* privkey_data = NULL;
        size_t privkey_len = 0;
        esp_err_t key_res = ssh_settings_get_private_key(connection_index, &privkey_data, &privkey_len);
        if (key_res == ESP_OK) {
            const char* passphrase = (strlen(settings->password) > 0) ? settings->password : NULL;
            rc = libssh2_userauth_publickey_frommemory(
                ssh_session, settings->username, strlen(settings->username),
                NULL, 0, (const char*)privkey_data, privkey_len, passphrase);
            free(privkey_data);
            if (rc == 0) {
                ESP_LOGI(TAG, "public key authentication succeeded");
                status_print("Public key authentication succeeded.\n");
                display_blit_buffer(buffer);
                authenticated = true;
            } else {
                ESP_LOGW(TAG, "public key authentication failed (%d), falling back to password", rc);
                status_print("Public key auth failed, trying password...\n");
                display_blit_buffer(buffer);
            }
        } else {
            ESP_LOGW(TAG, "failed to read private key from NVS (%d)", key_res);
        }
    }

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
            status_print("Authenticating to %s:%s as %s...\n",
                           settings->dest_host, settings->dest_port, settings->username);
            display_blit_buffer(buffer);

            if (libssh2_userauth_password(ssh_session, settings->username, ssh_password) == 0) {
                ESP_LOGI(TAG, "authentication by password succeeded");
                authenticated = true;
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

    if (!authenticated) {
        message_dialog(get_icon(ICON_ERROR), "SSH error", "Authentication failed", "Quit");
        goto shutdown;
    }

    ESP_LOGI(TAG, "requesting session");
    status_print("Requesting ssh session...\n");
    ssh_channel = libssh2_channel_open_session(ssh_session);
    if (!ssh_channel) {
        ESP_LOGE(TAG, "unable to open a session");
        return;
    }

    libssh2_channel_setenv(ssh_channel, "LANG", "en_US.UTF-8");

    // Compute terminal dimensions and init libvterm
    vt_pax_buf = buffer;
    vt_compute_size(pax_font_sky_mono, font_size_mult);

    ESP_LOGI(TAG, "requesting pty");
    status_print("Requesting pseudoterminal for interactive login session...\n");
    int pty_cols = vt_cols;
    int pty_rows = vt_rows;
    int pty_width_px = pax_buf_get_width(buffer);
    int pty_height_px = pax_buf_get_height(buffer);
    ESP_LOGI(TAG, "PTY size: %d cols x %d rows (%d x %d px)", pty_cols, pty_rows, pty_width_px, pty_height_px);
    if (libssh2_channel_request_pty_ex(ssh_channel, "xterm-256color", strlen("xterm-256color"),
                                        NULL, 0, pty_cols, pty_rows, pty_width_px, pty_height_px)) {
        ESP_LOGE(TAG, "failed requesting pty");
        return;
    }

    if (libssh2_channel_shell(ssh_channel)) {
        ESP_LOGE(TAG, "failed requesting shell");
        return;
    }

    ESP_LOGI(TAG, "making the channel non-blocking");
    libssh2_channel_set_blocking(ssh_channel, 0);

    // Initialize libvterm
    vt_init(pty_rows, pty_cols);
    vt_ssh_channel = ssh_channel;

    ESP_LOGI(TAG, "trying to load background image");
    load_ssh_bg();

    // Clear screen and render initial state
    vt_render_full();
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
            switch (event.type) {
                case INPUT_EVENT_TYPE_KEYBOARD:
		    ssh_out = event.args_keyboard.ascii;
		    if (event.args_keyboard.modifiers & BSP_INPUT_MODIFIER_FUNCTION) {
		        const char* fseq = NULL;
		        if (ssh_out >= '0' && ssh_out <= '9') {
		            int idx = ssh_out - '0';
		            fseq = fkey_sequences[idx];
		        } else if (ssh_out == '-') {
		            fseq = fkey_sequences[10];
		        } else if (ssh_out == '=') {
		            fseq = fkey_sequences[11];
		        }
		        if (fseq) {
		            libssh2_channel_write(ssh_channel, fseq, strlen(fseq));
		            break;
		        }
		    }
		    if (event.args_keyboard.modifiers & BSP_INPUT_MODIFIER_CTRL) {
			if (ssh_out == 'r' || ssh_out == 'R') {
			    screenshot_to_uart();
			    break;
			}
			if (ssh_out == 's' || ssh_out == 'S') {
			    save_screenshot();
			    break;
			}
                        ssh_out &= 0x1f;
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
		    break;
                case INPUT_EVENT_TYPE_NAVIGATION:
                    if (event.args_navigation.state) {
                        switch (event.args_navigation.key) {
                            case BSP_INPUT_NAVIGATION_KEY_ESC:
				vterm_keyboard_key(vt, VTERM_KEY_ESCAPE, VTERM_MOD_NONE);
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F1:
				ESP_LOGI(TAG, "close key pressed - returning to app launcher");
				goto shutdown;
                            case BSP_INPUT_NAVIGATION_KEY_F2:
				keyboard_backlight();
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F3:
				display_backlight();
				break;
                            case BSP_INPUT_NAVIGATION_KEY_F6:
				vterm_keyboard_key(vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
				break;
			    case BSP_INPUT_NAVIGATION_KEY_LEFT:
				vterm_keyboard_key(vt, VTERM_KEY_LEFT, VTERM_MOD_NONE);
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
				vterm_keyboard_key(vt, VTERM_KEY_RIGHT, VTERM_MOD_NONE);
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_UP:
				vterm_keyboard_key(vt, VTERM_KEY_UP, VTERM_MOD_NONE);
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_DOWN:
				vterm_keyboard_key(vt, VTERM_KEY_DOWN, VTERM_MOD_NONE);
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_TAB:
				vterm_keyboard_key(vt, VTERM_KEY_TAB, VTERM_MOD_NONE);
				break;
			    case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
                                font_size_mult += 0.3;
                                vt_compute_size(pax_font_sky_mono, font_size_mult);
                                vt_init(vt_rows, vt_cols);
                                vt_render_full();
                                vterm_keyboard_key(vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
                                display_blit_buffer(buffer);
				break;
			    case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
                                font_size_mult -= 0.3;
                                vt_compute_size(pax_font_sky_mono, font_size_mult);
                                vt_init(vt_rows, vt_cols);
                                vt_render_full();
                                vterm_keyboard_key(vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
                                display_blit_buffer(buffer);
				break;
            		    case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
				vterm_keyboard_key(vt, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RETURN:
				vterm_keyboard_key(vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
                                break;
                            default:
                                break;
                        }
		    }
		default:
		    break;
		case INPUT_EVENT_TYPE_LAST:
		    break;
	    }
        }

        if (libssh2_channel_eof(ssh_channel)) {
            ESP_LOGI(TAG, "server sent EOF");
            goto shutdown;
        }

        // Feed SSH data directly to libvterm
        bool got_data = false;
        int64_t read_start = esp_timer_get_time();
        do {
            nbytes = libssh2_channel_read(ssh_channel, ssh_buffer, 8192);
            if (nbytes <= 0) break;
            got_data = true;
            vterm_input_write(vt, ssh_buffer, nbytes);
        } while (nbytes > 0 && (esp_timer_get_time() - read_start) < 50000);

        if (got_data) {
            vterm_screen_flush_damage(vt_screen);
            vt_render_dirty();
            display_blit_buffer(buffer);
        }
    }

    // closing the ssh connection and freeing resources
 shutdown:
    ESP_LOGI(TAG, "in shutdown, clearing the screen...");
    pax_draw_rect(buffer, 0xffefefef, 0, 0, 800, 480);
    display_blit_buffer(buffer);
    ESP_LOGI(TAG, "freeing memory...");
    vt_ssh_channel = NULL;
    if (ssh_channel) {
        libssh2_channel_send_eof(ssh_channel);
        libssh2_channel_close(ssh_channel);
        libssh2_channel_free(ssh_channel);
    }
    libssh2_session_disconnect(ssh_session, "User closed session");
    if (ssh_sock != LIBSSH2_INVALID_SOCKET) {
        shutdown(ssh_sock, 2);
        LIBSSH2_SOCKET_CLOSE(ssh_sock);
    }
    libssh2_exit();
    free(ssh_buffer);
    if (vt) {
        vterm_free(vt);
        vt = NULL;
        vt_screen = NULL;
    }
}
