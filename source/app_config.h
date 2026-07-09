#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "ha_client.h"

// Persisted sign-in state, stored at sdmc:/3ds/ha3ds/ha3ds.cfg. The refresh token is
// long-lived (HA keeps it valid until revoked from the user's profile), so
// sign-in survives reboots; short-lived access tokens are minted from it at
// runtime and never stored.
typedef struct {
    char base_url[HA_MAX_URL];
    char refresh_token[HA_MAX_TOKEN];
    // Bitmask of HA_DOMAINS entries to pull - see ha_client_set_enabled_domains().
    unsigned int enabled_domains;
} app_config_t;

// Returns 0 and fills cfg if a complete config was loaded, -1 otherwise.
int app_config_load(app_config_t *cfg);

// Returns 0 on success.
int app_config_save(const app_config_t *cfg);

// Deletes the stored config (sign out).
void app_config_delete(void);

#endif
