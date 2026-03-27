#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Initialize W5500 Ethernet over SPI on the J4 PMOD connector.
// Requires esp_netif and default event loop to be initialized first.
// Returns ESP_OK on success. Non-fatal if W5500 is not connected.
esp_err_t ethernet_init(void);

// Return true if Ethernet link is up and has an IP address.
bool ethernet_connected(void);
