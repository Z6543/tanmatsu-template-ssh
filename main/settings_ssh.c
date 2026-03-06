//
// Derived from nicolaielectronics/wifi-manager
//
#include "settings_ssh.h"
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include "esp_eap_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi_types_generic.h"
#include "nvs.h"

#define member_size(type, member) (sizeof(((type*)0)->member))

static const char* NVS_NAMESPACE = "ssh";
static const char TAG[] = "settings_ssh";

static void ssh_settings_combine_key(uint8_t index, const char* parameter, char* out_nvs_key) {
    assert(snprintf(out_nvs_key, 16, "s%02x.%s", index, parameter) <= 16);
}

static esp_err_t ssh_settings_get_parameter_str(nvs_handle_t nvs_handle, uint8_t index, const char* parameter,
                                                 char* out_string, size_t max_length) {
    char nvs_key[16];
    ssh_settings_combine_key(index, parameter, nvs_key);
    size_t    size = 0;
    esp_err_t res  = nvs_get_str(nvs_handle, nvs_key, NULL, &size);
    if (res != ESP_OK) {
        return res;
    }
    if (size > max_length) {
        return ESP_ERR_NO_MEM;
    }
    return nvs_get_str(nvs_handle, nvs_key, out_string, &size);
}

static esp_err_t ssh_settings_set_parameter_str(nvs_handle_t nvs_handle, uint8_t index, const char* parameter,
                                                 char* string, size_t max_length) {
    char nvs_key[16];
    ssh_settings_combine_key(index, parameter, nvs_key);
    return nvs_set_str(nvs_handle, nvs_key, string);
}

static esp_err_t ssh_settings_get_parameter_u32(nvs_handle_t nvs_handle, uint8_t index, const char* parameter,
                                                 uint32_t* out_value) {
    char nvs_key[16];
    ssh_settings_combine_key(index, parameter, nvs_key);
    return nvs_get_u32(nvs_handle, nvs_key, out_value);
}

static esp_err_t ssh_settings_set_parameter_u32(nvs_handle_t nvs_handle, uint8_t index, const char* parameter,
                                                 uint32_t value) {
    char nvs_key[16];
    ssh_settings_combine_key(index, parameter, nvs_key);
    return nvs_set_u32(nvs_handle, nvs_key, value);
}

static esp_err_t ssh_settings_get_parameter_blob(nvs_handle_t nvs_handle, uint8_t index, const char* parameter,
                                                  void* out_blob, size_t expected_length) {
    char nvs_key[16];
    ssh_settings_combine_key(index, parameter, nvs_key);
    size_t size = expected_length;
    return nvs_get_blob(nvs_handle, nvs_key, out_blob, &size);
}

static esp_err_t ssh_settings_set_parameter_blob(nvs_handle_t nvs_handle, uint8_t index, const char* parameter,
                                                  const void* blob, size_t length) {
    char nvs_key[16];
    ssh_settings_combine_key(index, parameter, nvs_key);
    return nvs_set_blob(nvs_handle, nvs_key, blob, length);
}

static esp_err_t _ssh_settings_get(nvs_handle_t nvs_handle, uint8_t index, ssh_settings_t* out_settings) {
    //ESP_LOGI(TAG, "_ssh_settings_get()");
    char buffer[128 + sizeof('\0')] = {0};

    // Check parameters
    if (out_settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    //ESP_LOGI(TAG, "  zeroing data structure");
    memset(out_settings, 0, sizeof(ssh_settings_t));

    // Read destination host / IP address (32 bytes)
    //ESP_LOGI(TAG, "  getting dest_host");
    memset(buffer, 0, sizeof(buffer));
    esp_err_t res = ssh_settings_get_parameter_str(nvs_handle, index, "dest_host", buffer, member_size(ssh_settings_t, dest_host) + 1);
    if (res != ESP_OK) {
        return res;
    }
    memcpy(out_settings->dest_host, buffer, member_size(ssh_settings_t, dest_host));

    // Read destination port number (32 bytes)
    //ESP_LOGI(TAG, "  getting dest_port");
    memset(buffer, 0, sizeof(buffer));
    res = ssh_settings_get_parameter_str(nvs_handle, index, "dest_port", buffer, member_size(ssh_settings_t, dest_port) + 1);
    if (res != ESP_OK) {
        return res;
    }
    memcpy(out_settings->dest_port, buffer, member_size(ssh_settings_t, dest_port));

    // Read username (128 bytes)
    //ESP_LOGI(TAG, "  getting username");
    memset(buffer, 0, sizeof(buffer));
    res = ssh_settings_get_parameter_str(nvs_handle, index, "username", buffer, member_size(ssh_settings_t, username) + 1);
    if (res != ESP_OK) {
        return res;
    }
    memcpy(out_settings->username, buffer, member_size(ssh_settings_t, username));

    // Read password (64 bytes)
    //ESP_LOGI(TAG, "  getting password");
    memset(buffer, 0, sizeof(buffer));
    res = ssh_settings_get_parameter_str(nvs_handle, index, "password", buffer, member_size(ssh_settings_t, password) + 1);
    if (res != ESP_OK) {
        return res;
    }
    memcpy(out_settings->password, buffer, member_size(ssh_settings_t, password));

    // Read auth mode (enum, stored as u32)
    //ESP_LOGI(TAG, "  getting auth_mode");
    uint32_t auth_mode = 0;
    res = ssh_settings_get_parameter_u32(nvs_handle, index, "auth_mode", &auth_mode);
    if (res != ESP_OK) {
        return res;
    }
    out_settings->auth_mode = (ssh_auth_mode_t)auth_mode;

    // Read connection name - XXX moved to the end because the function was crashing when this was first
    //ESP_LOGI(TAG, "  getting connection_name");
    memset(buffer, 0, sizeof(buffer));
    res = ssh_settings_get_parameter_str(nvs_handle, index, "conn_name", buffer, member_size(ssh_settings_t, connection_name) + 1);
    if (res != ESP_OK) {
        return res;
    }
    //ESP_LOGI(TAG, "  ...done!");
    memcpy(out_settings->connection_name, buffer, member_size(ssh_settings_t, connection_name));

    return res;
}

static esp_err_t _ssh_settings_set(nvs_handle_t nvs_handle, uint8_t index, ssh_settings_t* settings) {
    //ESP_LOGI(TAG, "_ssh_settings_set()");
    char buffer[128 + 1] = {0};

    // Check parameters
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Write destination host name / IP address
    memcpy(buffer, settings->dest_host, member_size(ssh_settings_t, dest_host));
    buffer[member_size(ssh_settings_t, dest_host)] = '\0';
    //ESP_LOGI(TAG, "  setting dest_host");
    esp_err_t res = ssh_settings_set_parameter_str(nvs_handle, index, "dest_host", buffer, member_size(ssh_settings_t, dest_host) + 1);
    if (res != ESP_OK) {
        return res;
    }

    // Write destination port number
    // We'll store this as text even though it's a number, since most of the other settings are stored as text
    memcpy(buffer, settings->dest_port, member_size(ssh_settings_t, dest_port));
    buffer[member_size(ssh_settings_t, dest_port)] = '\0';
    //ESP_LOGI(TAG, "  setting dest_port");
    res = ssh_settings_set_parameter_str(nvs_handle, index, "dest_port", buffer, member_size(ssh_settings_t, dest_port) + 1);
    if (res != ESP_OK) {
        return res;
    }

    // Write username
    memcpy(buffer, settings->username, member_size(ssh_settings_t, username));
    buffer[member_size(ssh_settings_t, username)] = '\0';
    //ESP_LOGI(TAG, "  setting username");
    res = ssh_settings_set_parameter_str(nvs_handle, index, "username", buffer, member_size(ssh_settings_t, username) + 1);
    if (res != ESP_OK) {
        return res;
    }

    // Write password
    memcpy(buffer, settings->password, member_size(ssh_settings_t, password));
    buffer[member_size(ssh_settings_t, password)] = '\0';
    //ESP_LOGI(TAG, "  setting password");
    res = ssh_settings_set_parameter_str(nvs_handle, index, "password", buffer, member_size(ssh_settings_t, password) + 1);
    if (res != ESP_OK) {
        return res;
    }

    // Write auth mode (enum, stored as u32)
    uint32_t auth_mode = (uint32_t)settings->auth_mode;
    //ESP_LOGI(TAG, "  setting auth_mode");
    res = ssh_settings_set_parameter_u32(nvs_handle, index, "auth_mode", auth_mode);
    if (res != ESP_OK) {
        return res;
    }

    // Write connection name
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, settings->connection_name, member_size(ssh_settings_t, connection_name));
    //buffer[member_size(ssh_settings_t, connection_name)] = '\0';
    //ESP_LOGI(TAG, "  setting connection_name");
    //ESP_LOGI(TAG, "  connection_name value: %s", settings->connection_name);
    res = ssh_settings_set_parameter_str(nvs_handle, index, "conn_name", buffer, member_size(ssh_settings_t, connection_name) + 1);
    if (res != ESP_OK) {
        return res;
    }

    return res;
}

static esp_err_t _ssh_settings_erase(nvs_handle_t nvs_handle, uint8_t index) {
    ESP_LOGI(TAG, "ssh_settings_erase()");
    char nvs_key[16];
    ssh_settings_combine_key(index, "conn_name", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    ssh_settings_combine_key(index, "dest_host", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    ssh_settings_combine_key(index, "dest_port", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    ssh_settings_combine_key(index, "username", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    ssh_settings_combine_key(index, "password", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    ssh_settings_combine_key(index, "auth_mode", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    ssh_settings_combine_key(index, "hk_sha256", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    ssh_settings_combine_key(index, "privkey", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    return ESP_OK;
}

esp_err_t ssh_settings_get(uint8_t index, ssh_settings_t* out_settings) {
    //ESP_LOGI(TAG, "ssh_settings_get()");
    if (out_settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t    res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) {
        return res;
    }

    res = _ssh_settings_get(nvs_handle, index, out_settings);
    nvs_close(nvs_handle);
    return res;
}

esp_err_t ssh_settings_set(uint8_t index, ssh_settings_t* settings) {
    //ESP_LOGI(TAG, "ssh_settings_set()");
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) {
        return res;
    }

    res = _ssh_settings_set(nvs_handle, index, settings);
    if (res != ESP_OK) {
        nvs_close(nvs_handle);
        return res;
    }

    res = nvs_commit(nvs_handle);
    if (res != ESP_OK) {
        nvs_close(nvs_handle);
        return res;
    }

    nvs_close(nvs_handle);
    return res;
}

esp_err_t ssh_settings_erase(uint8_t index) {
    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) {
        return res;
    }

    res = _ssh_settings_erase(nvs_handle, index);
    if (res != ESP_OK) {
        nvs_close(nvs_handle);
        return res;
    }

    uint8_t move_index = index;
    while (1) {
        ssh_settings_t temp = {0};
        res = _ssh_settings_get(nvs_handle, move_index + 1, &temp);
        if (res != ESP_OK) {
            break;
        }
        res = _ssh_settings_set(nvs_handle, move_index, &temp);
        move_index++;
    }

    if (move_index != index) {
        res = _ssh_settings_erase(nvs_handle, move_index);
        if (res != ESP_OK) {
            nvs_close(nvs_handle);
            return res;
        }
    }

    res = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return res;
}

esp_err_t ssh_settings_get_host_key(uint8_t index, uint8_t out_sha256[32]) {
    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (res != ESP_OK) return res;
    res = ssh_settings_get_parameter_blob(nvs_handle, index, "hk_sha256", out_sha256, 32);
    nvs_close(nvs_handle);
    return res;
}

esp_err_t ssh_settings_set_host_key(uint8_t index, const uint8_t sha256[32]) {
    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) return res;
    res = ssh_settings_set_parameter_blob(nvs_handle, index, "hk_sha256", sha256, 32);
    if (res == ESP_OK) res = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return res;
}

esp_err_t ssh_settings_clear_host_key(uint8_t index) {
    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) return res;
    char nvs_key[16];
    ssh_settings_combine_key(index, "hk_sha256", nvs_key);
    nvs_erase_key(nvs_handle, nvs_key);
    res = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return res;
}

int ssh_settings_find_empty_slot(void) {
    ESP_LOGI(TAG, "find_empty_slot()");
    int slot = -1;
    for (uint32_t index = 0; index < SSH_SETTINGS_MAX; index++) {
        ESP_LOGI(TAG, "  slot %d", index);
        ssh_settings_t settings;
        esp_err_t       res = ssh_settings_get(index, &settings);
        if (res != ESP_OK) {
            slot = index;
            break;
        }
    }
    return slot;
}

bool ssh_settings_has_private_key(uint8_t index) {
    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (res != ESP_OK) {
        return false;
    }
    char nvs_key[16];
    ssh_settings_combine_key(index, "privkey", nvs_key);
    size_t size = 0;
    res = nvs_get_blob(nvs_handle, nvs_key, NULL, &size);
    nvs_close(nvs_handle);
    return (res == ESP_OK && size > 0);
}

esp_err_t ssh_settings_get_private_key(uint8_t index, uint8_t** out_buf, size_t* out_len) {
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (res != ESP_OK) {
        return res;
    }

    char nvs_key[16];
    ssh_settings_combine_key(index, "privkey", nvs_key);

    size_t size = 0;
    res = nvs_get_blob(nvs_handle, nvs_key, NULL, &size);
    if (res != ESP_OK) {
        nvs_close(nvs_handle);
        return res;
    }

    uint8_t* buf = malloc(size);
    if (buf == NULL) {
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }

    res = nvs_get_blob(nvs_handle, nvs_key, buf, &size);
    nvs_close(nvs_handle);
    if (res != ESP_OK) {
        free(buf);
        return res;
    }

    *out_buf = buf;
    *out_len = size;
    return ESP_OK;
}

esp_err_t ssh_settings_set_private_key(uint8_t index, const uint8_t* data, size_t len) {
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) {
        return res;
    }

    char nvs_key[16];
    ssh_settings_combine_key(index, "privkey", nvs_key);
    res = nvs_set_blob(nvs_handle, nvs_key, data, len);
    if (res != ESP_OK) {
        nvs_close(nvs_handle);
        return res;
    }

    res = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return res;
}

esp_err_t ssh_settings_clear_private_key(uint8_t index) {
    nvs_handle_t nvs_handle;
    esp_err_t res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) {
        return res;
    }

    char nvs_key[16];
    ssh_settings_combine_key(index, "privkey", nvs_key);
    res = nvs_erase_key(nvs_handle, nvs_key);
    if (res != ESP_OK && res != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return res;
    }

    res = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return res;
}
