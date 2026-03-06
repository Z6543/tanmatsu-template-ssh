//
// Derived from nicolaielectronics/wifi-manager
//
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define SSH_SETTINGS_MAX 0xFF

typedef enum {
    SSH_AUTH_PASSWORD,
    SSH_AUTH_PUBKEY,
    SSH_AUTH_INTERACTIVE,
} ssh_auth_mode_t;

typedef struct {
    // Basic connection information
    char                      connection_name[128];
    char                      dest_host[128];
    char		      dest_port[6];
    char                      username[128];
    // Password, if not using key based authentication
    char                      password[64];
    ssh_auth_mode_t           auth_mode;
    // Host key fingerprint (SHA256, 32 bytes)
    uint8_t                   host_key_sha256[32];
    bool                      host_key_saved;
} ssh_settings_t;

esp_err_t ssh_settings_get(uint8_t index, ssh_settings_t* out_settings);
esp_err_t ssh_settings_set(uint8_t index, ssh_settings_t* settings);
esp_err_t ssh_settings_erase(uint8_t index);
int       ssh_settings_find_empty_slot(void);

// Host key fingerprint persistence
esp_err_t ssh_settings_get_host_key(uint8_t index, uint8_t out_sha256[32]);
esp_err_t ssh_settings_set_host_key(uint8_t index, const uint8_t sha256[32]);
esp_err_t ssh_settings_clear_host_key(uint8_t index);

// Private key blob storage (for public key authentication)
esp_err_t ssh_settings_get_private_key(uint8_t index, uint8_t** out_buf, size_t* out_len);
esp_err_t ssh_settings_set_private_key(uint8_t index, const uint8_t* data, size_t len);
esp_err_t ssh_settings_clear_private_key(uint8_t index);
bool      ssh_settings_has_private_key(uint8_t index);

