#ifndef HA_CLIENT_H
#define HA_CLIENT_H

#include <stddef.h>

#define HA_MAX_ENTITY_ID 128
#define HA_MAX_NAME 96
#define HA_MAX_STATE 32
#define HA_MAX_URL 128
#define HA_MAX_TOKEN 512
#define HA_MAX_FLOW_ID 64
#define HA_MAX_AUTH_CODE 256

typedef struct {
    char entity_id[HA_MAX_ENTITY_ID];
    char friendly_name[HA_MAX_NAME];
    char state[HA_MAX_STATE];
    int supports_brightness;  // 1 if this is a dimmable light
    int brightness_pct;       // 0-100, last known brightness (only meaningful if supports_brightness)
    int supports_color;       // 1 if this light takes an rgb_color (hs/xy/rgb/rgbw/rgbww mode)
    int supports_color_temp;  // 1 if this light takes a color_temp_kelvin (color_temp mode)
    int is_climate;           // 1 if this is a climate/thermostat entity
    float current_temp;       // attributes.current_temperature, 0 if not reported
    float target_temp;        // attributes.temperature (falls back to target_temp_low for range-mode thermostats)
    char area_name[HA_MAX_NAME]; // room/area name, "" if unassigned or not fetched - see ha_fetch_area_map
    int is_group;              // 1 if attributes.entity_id (member list) is present - see parse_entity_fields
} ha_entity_t;

typedef struct {
    char entity_id[HA_MAX_ENTITY_ID];
    char area_name[HA_MAX_NAME];
} ha_area_entry_t;

// Domains ha_fetch_states() knows how to toggle, and which of them are
// currently pulled - see ha_client_set_enabled_domains(). Bit i of a mask
// corresponds to HA_DOMAINS[i]/HA_DOMAIN_LABELS[i]. Fixed order, used by the
// Settings screen to list them and by app_config to persist the choice.
#define HA_NUM_DOMAINS 8
#define HA_ALL_DOMAINS_MASK ((1u << HA_NUM_DOMAINS) - 1)
extern const char *const HA_DOMAINS[HA_NUM_DOMAINS];
extern const char *const HA_DOMAIN_LABELS[HA_NUM_DOMAINS];

// Must be called once after socInit(), before any other ha_* call.
void ha_client_init(void);

// Call once at shutdown.
void ha_client_exit(void);

// Runtime connection config (replaces the old compile-time config.h).
// Base URL has no trailing slash, e.g. "https://ha.example.com". Both are
// copied internally. Set the base URL before any auth/API call; the access
// token before any API call.
void ha_client_set_base_url(const char *base_url);
void ha_client_set_access_token(const char *token);

// Restricts ha_fetch_states()/ha_fetch_area_map() to the domains whose bit
// is set (see HA_DOMAINS/HA_ALL_DOMAINS_MASK above). Defaults to
// HA_ALL_DOMAINS_MASK until called. Takes effect on the next fetch - it
// doesn't touch entities already in memory.
void ha_client_set_enabled_domains(unsigned int mask);

// --- Sign-in (Home Assistant's frontend login flow over REST) ------------
// All return 0 on success, -1 on failure unless noted.

// Starts a login flow. On success fills flow_id.
int ha_auth_login_begin(char *flow_id, size_t flow_id_size);

// Submits username/password to the flow. Returns:
//   0  - done; auth_code filled
//   1  - MFA required; call ha_auth_login_mfa next with the same flow_id
//  -1  - failed (bad credentials, network, ...)
int ha_auth_login_submit(const char *flow_id, const char *username, const char *password,
                         char *auth_code, size_t auth_code_size);

// Submits an MFA code to the flow. Same return convention as submit,
// except 1 means the code was rejected and can be retried.
int ha_auth_login_mfa(const char *flow_id, const char *mfa_code,
                      char *auth_code, size_t auth_code_size);

// Exchanges the auth code for tokens. expires_in is in seconds.
int ha_auth_exchange_code(const char *auth_code,
                          char *refresh_token, size_t refresh_size,
                          char *access_token, size_t access_size,
                          int *expires_in);

// Gets a fresh access token from a stored refresh token.
int ha_auth_refresh(const char *refresh_token,
                    char *access_token, size_t access_size,
                    int *expires_in);

// --- Entity API -----------------------------------------------------------

// Fetches entities from GET /api/states, keeping only entities whose domain
// is both one this remote knows how to toggle and currently enabled (see
// ha_client_set_enabled_domains()). Fills at most max_count entries into out
// and returns how many were written, or -1 on network/parse failure.
int ha_fetch_states(ha_entity_t *out, int max_count);

// Fetches just one entity from GET /api/states/<entity_id> - much cheaper
// than ha_fetch_states() when only one entity changed (e.g. after a toggle
// or brightness change). Returns 0 on success, -1 on failure.
int ha_fetch_single_state(const char *entity_id, ha_entity_t *out);

// Toggles entity_id. current_state is the caller's last-known state ("on"/
// "off", from ha_entity_t.state) - for domains with their own turn_on/
// turn_off services (light, switch, fan, input_boolean), this decides the
// direction itself and calls the explicit one directly, which is both
// faster and more reliable than asking HA to decide via a toggle service:
// no state-lookup-then-redispatch hop, and no dependence on the entity's own
// (occasionally laggy, for polling/cloud-backed integrations) is_on. Pass ""
// if the current state isn't known. is_group (ha_entity_t.is_group) opts out
// of that direct dispatch even for those domains: a group's own state is an
// aggregate over its members ("on" if any member is on) that can lag behind
// a command longer than a single entity's does, making current_state less
// trustworthy to decide direction from. Falls back to
// POST /api/services/homeassistant/toggle for groups, unknown current_state,
// and domains without a direct on/off pair (lock, climate, media_player).
// Returns 0 on success (HTTP 200), -1 otherwise.
int ha_toggle_entity(const char *entity_id, const char *current_state, int is_group);

// Calls POST /api/services/light/turn_on with brightness_pct (0-100) for
// entity_id. Also turns the light on if it was off. Returns 0 on success
// (HTTP 200), -1 otherwise.
int ha_set_brightness(const char *entity_id, int brightness_pct);

// Calls POST /api/services/climate/set_temperature with the given target
// temperature (in whatever unit the HA instance is configured for) for
// entity_id. Returns 0 on success (HTTP 200), -1 otherwise.
int ha_set_temperature(const char *entity_id, float target_temp);

// Calls POST /api/services/light/turn_on with rgb_color:[r,g,b] (0-255 each)
// for entity_id. Only meaningful if supports_color. Returns 0 on success
// (HTTP 200), -1 otherwise.
int ha_set_color(const char *entity_id, int r, int g, int b);

// Calls POST /api/services/light/turn_on with color_temp_kelvin for
// entity_id. Only meaningful if supports_color_temp. Returns 0 on success
// (HTTP 200), -1 otherwise.
int ha_set_color_temp(const char *entity_id, int kelvin);

// Fetches an entity_id -> area/room name mapping via a Jinja template call
// (HA's REST API has no direct area-registry endpoint; this uses the
// built-in area_name() template function so no WebSocket connection is
// needed). Only covers the same domains ha_fetch_states() does. Returns the
// number of entries written (can legitimately be 0 if nothing has an area
// assigned), or -1 on failure - callers should treat that as "no area data
// available" rather than fatal, since grouping is a nice-to-have.
int ha_fetch_area_map(ha_area_entry_t *out, int max_count);

#endif
