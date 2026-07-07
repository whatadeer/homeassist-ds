#ifndef HA_CLIENT_H
#define HA_CLIENT_H

#define HA_MAX_ENTITY_ID 128
#define HA_MAX_NAME 96
#define HA_MAX_STATE 32

typedef struct {
    char entity_id[HA_MAX_ENTITY_ID];
    char friendly_name[HA_MAX_NAME];
    char state[HA_MAX_STATE];
    int supports_brightness;  // 1 if this is a dimmable light
    int brightness_pct;       // 0-100, last known brightness (only meaningful if supports_brightness)
} ha_entity_t;

// Must be called once after socInit(), before any other ha_* call.
void ha_client_init(void);

// Call once at shutdown.
void ha_client_exit(void);

// Fetches entities from GET /api/states, keeping only domains this remote
// knows how to toggle (light, switch, fan, lock, cover, input_boolean,
// climate, media_player). Fills at most max_count entries into out and
// returns how many were written, or -1 on network/parse failure.
int ha_fetch_states(ha_entity_t *out, int max_count);

// Fetches just one entity from GET /api/states/<entity_id> - much cheaper
// than ha_fetch_states() when only one entity changed (e.g. after a toggle
// or brightness change). Returns 0 on success, -1 on failure.
int ha_fetch_single_state(const char *entity_id, ha_entity_t *out);

// Calls POST /api/services/homeassistant/toggle for entity_id.
// Returns 0 on success (HTTP 200), -1 otherwise.
int ha_toggle_entity(const char *entity_id);

// Calls POST /api/services/light/turn_on with brightness_pct (0-100) for
// entity_id. Also turns the light on if it was off. Returns 0 on success
// (HTTP 200), -1 otherwise.
int ha_set_brightness(const char *entity_id, int brightness_pct);

#endif
