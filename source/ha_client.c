#include "ha_client.h"
#include "dbglog.h"

#include <curl/curl.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Generated at build time from data/cacert.bin (Mozilla's CA bundle, as
// distributed by curl.se) via the Makefile's DATA -> bin2s pipeline. Needed
// because the HA instance may be reached over the public internet with a
// real CA-signed cert - the 3DS has no system trust store for libcurl.
#include "cacert_bin.h"

// Domains homeassistant.toggle can meaningfully act on. Filters the (often
// huge) /api/states response down to things worth showing on a small screen.
static const char *SUPPORTED_DOMAINS[] = {
    "light", "switch", "fan", "lock", "cover",
    "input_boolean", "climate", "media_player",
};
#define NUM_SUPPORTED_DOMAINS (sizeof(SUPPORTED_DOMAINS) / sizeof(SUPPORTED_DOMAINS[0]))

// Reused across every request instead of curl_easy_init()/cleanup() per
// call - keeping one handle alive lets libcurl reuse the TCP+TLS connection
// across calls to the same host, skipping the (software-crypto, slow on the
// 3DS) handshake. Only ever touched from one thread at a time: the main
// thread during the sign-in wizard, the network worker thread afterwards -
// never concurrently (the wizard only runs while no worker is in flight).
static CURL *g_curl = NULL;

// Runtime connection config - set via ha_client_set_* instead of the old
// compile-time config.h.
static char g_base_url[HA_MAX_URL] = "";
static char g_access_token[HA_MAX_TOKEN] = "";

struct membuf {
    char *data;
    size_t size;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    struct membuf *mem = (struct membuf *)userdata;

    char *newdata = realloc(mem->data, mem->size + realsize + 1);
    if (!newdata) {
        return 0;
    }

    mem->data = newdata;
    memcpy(mem->data + mem->size, ptr, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

static int is_supported_domain(const char *entity_id) {
    const char *dot = strchr(entity_id, '.');
    if (!dot) {
        return 0;
    }
    size_t domain_len = (size_t)(dot - entity_id);

    for (size_t i = 0; i < NUM_SUPPORTED_DOMAINS; i++) {
        if (strlen(SUPPORTED_DOMAINS[i]) == domain_len &&
            strncmp(SUPPORTED_DOMAINS[i], entity_id, domain_len) == 0) {
            return 1;
        }
    }
    return 0;
}

// Fills out->* from a single /api/states-style JSON object (works whether
// it came from the states array or the single-entity endpoint).
static void parse_entity_fields(json_t *item, const char *eid, ha_entity_t *out) {
    json_t *jstate = json_object_get(item, "state");
    json_t *jattrs = json_object_get(item, "attributes");
    json_t *jname = jattrs ? json_object_get(jattrs, "friendly_name") : NULL;

    strncpy(out->entity_id, eid, HA_MAX_ENTITY_ID - 1);
    out->entity_id[HA_MAX_ENTITY_ID - 1] = '\0';

    const char *state_str = jstate ? json_string_value(jstate) : "unknown";
    strncpy(out->state, state_str ? state_str : "unknown", HA_MAX_STATE - 1);
    out->state[HA_MAX_STATE - 1] = '\0';

    const char *name_str = jname ? json_string_value(jname) : eid;
    strncpy(out->friendly_name, name_str ? name_str : eid, HA_MAX_NAME - 1);
    out->friendly_name[HA_MAX_NAME - 1] = '\0';

    // Not available from /api/states - filled in separately by
    // ha_fetch_area_map() and merged in by the caller, if at all.
    out->area_name[0] = '\0';

    // A light is dimmable if supported_color_modes has anything besides
    // "onoff" (every non-onoff color mode implies brightness support in
    // HA's model), or the legacy SUPPORT_BRIGHTNESS bit (0x1) is set in
    // supported_features for integrations that predate color modes.
    out->supports_brightness = 0;
    out->brightness_pct = 100;
    if (jattrs) {
        json_t *jmodes = json_object_get(jattrs, "supported_color_modes");
        if (json_is_array(jmodes)) {
            size_t n_modes = json_array_size(jmodes);
            for (size_t m = 0; m < n_modes; m++) {
                json_t *jmode = json_array_get(jmodes, m);
                const char *mode_str = json_is_string(jmode) ? json_string_value(jmode) : NULL;
                if (mode_str && strcmp(mode_str, "onoff") != 0) {
                    out->supports_brightness = 1;
                    break;
                }
            }
        }
        json_t *jfeatures = json_object_get(jattrs, "supported_features");
        if (json_is_integer(jfeatures) && (json_integer_value(jfeatures) & 0x1)) {
            out->supports_brightness = 1;
        }

        json_t *jbrightness = json_object_get(jattrs, "brightness");
        if (json_is_integer(jbrightness)) {
            long raw = json_integer_value(jbrightness); // 0-255
            out->brightness_pct = (int)((raw * 100 + 127) / 255);
        }
    }

    out->is_climate = 0;
    out->current_temp = 0.0f;
    out->target_temp = 0.0f;
    {
        const char *dot = strchr(eid, '.');
        if (dot && (size_t)(dot - eid) == 7 && strncmp(eid, "climate", 7) == 0) {
            out->is_climate = 1;
        }
    }
    if (out->is_climate && jattrs) {
        json_t *jcur = json_object_get(jattrs, "current_temperature");
        if (json_is_number(jcur)) {
            out->current_temp = (float)json_number_value(jcur);
        }

        // Single-setpoint thermostats use "temperature"; range-mode ones
        // (heat_cool) use target_temp_low/high instead - fall back to the
        // low end so there's still something sensible to display/adjust.
        json_t *jtarget = json_object_get(jattrs, "temperature");
        if (json_is_number(jtarget)) {
            out->target_temp = (float)json_number_value(jtarget);
        } else {
            json_t *jlow = json_object_get(jattrs, "target_temp_low");
            if (json_is_number(jlow)) {
                out->target_temp = (float)json_number_value(jlow);
            }
        }
    }
}

void ha_client_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    g_curl = curl_easy_init();
    if (!g_curl) {
        return;
    }

    // Options that never change between requests - set once here instead
    // of on every call.
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_NODELAY, 1L);
    // Requests may round-trip over the internet (no LAN shortcut), so give
    // them more room than a same-network call would need.
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 8L);

    // Verify the server's cert against the bundled CA root store. Explicit
    // even though these are curl's defaults, since a bad cert failing
    // silently would be worse than it failing loudly here.
    struct curl_blob ca_blob; // CURL_BLOB_COPY makes curl copy immediately, so stack lifetime is fine
    ca_blob.data = (void *)cacert_bin;
    ca_blob.len = cacert_bin_size;
    ca_blob.flags = CURL_BLOB_COPY;
    curl_easy_setopt(g_curl, CURLOPT_CAINFO_BLOB, &ca_blob);
    curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

void ha_client_exit(void) {
    if (g_curl) {
        curl_easy_cleanup(g_curl);
        g_curl = NULL;
    }
    curl_global_cleanup();
}

void ha_client_set_base_url(const char *base_url) {
    strncpy(g_base_url, base_url, sizeof(g_base_url) - 1);
    g_base_url[sizeof(g_base_url) - 1] = '\0';

    // Normalize: no trailing slash (all URL builders below add their own).
    size_t len = strlen(g_base_url);
    while (len > 0 && g_base_url[len - 1] == '/') {
        g_base_url[--len] = '\0';
    }
}

void ha_client_set_access_token(const char *token) {
    strncpy(g_access_token, token, sizeof(g_access_token) - 1);
    g_access_token[sizeof(g_access_token) - 1] = '\0';
}

// One HTTP round-trip on the shared handle. Exactly one of json_body /
// form_body may be non-NULL (NULL for both = GET). with_auth attaches the
// bearer token header. Response body lands in mem (caller frees mem->data);
// returns -1 on transport failure, else the HTTP status code.
static long do_request(const char *url, const char *json_body, const char *form_body,
                       int with_auth, struct membuf *mem) {
    if (!g_curl) {
        return -1;
    }

    struct curl_slist *headers = NULL;
    if (with_auth) {
        char auth_header[HA_MAX_TOKEN + 32];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", g_access_token);
        headers = curl_slist_append(headers, auth_header);
    }
    if (json_body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    } else if (form_body) {
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    } else if (with_auth) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, mem);

    const char *body = json_body ? json_body : form_body;
    if (body) {
        curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
    } else {
        // Undo any POST state left on the reused handle by a prior call.
        curl_easy_setopt(g_curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode res = curl_easy_perform(g_curl);
    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    dbg_log("%s %s: res=%d http=%ld", body ? "POST" : "GET", url, res, http_code);

    curl_slist_free_all(headers);

    return (res == CURLE_OK) ? http_code : -1;
}

// --- Sign-in flow ----------------------------------------------------------
// Drives the same /auth/login_flow + /auth/token endpoints the HA web
// frontend uses. client_id/redirect_uri follow the frontend's convention
// (base URL as client_id) so the resulting refresh token shows up in HA
// under a sensible name.

static void build_client_id(char *out, size_t out_size) {
    snprintf(out, out_size, "%s/", g_base_url);
}

int ha_auth_login_begin(char *flow_id, size_t flow_id_size) {
    char url[192];
    snprintf(url, sizeof(url), "%s/auth/login_flow", g_base_url);

    char client_id[HA_MAX_URL + 2];
    build_client_id(client_id, sizeof(client_id));
    char redirect_uri[HA_MAX_URL + 24];
    snprintf(redirect_uri, sizeof(redirect_uri), "%s/?auth_callback=1", g_base_url);

    json_t *req = json_pack("{s:s, s:[s,n], s:s}",
        "client_id", client_id,
        "handler", "homeassistant",
        "redirect_uri", redirect_uri);
    char *body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (!body) {
        return -1;
    }

    struct membuf mem = {0};
    long http = do_request(url, body, NULL, 0, &mem);
    free(body);

    int rc = -1;
    if (http == 200 && mem.data) {
        json_t *root = json_loads(mem.data, 0, NULL);
        json_t *jflow = root ? json_object_get(root, "flow_id") : NULL;
        const char *fid = jflow ? json_string_value(jflow) : NULL;
        if (fid) {
            strncpy(flow_id, fid, flow_id_size - 1);
            flow_id[flow_id_size - 1] = '\0';
            rc = 0;
        }
        if (root) {
            json_decref(root);
        }
    }
    free(mem.data);
    return rc;
}

// Posts one step of the login flow and classifies the response.
// Returns 0 (done, auth_code filled), 1 (another form step - MFA / retry),
// or -1 (failure).
static int login_flow_step(const char *flow_id, json_t *fields,
                           char *auth_code, size_t auth_code_size) {
    char url[256];
    snprintf(url, sizeof(url), "%s/auth/login_flow/%s", g_base_url, flow_id);

    char client_id[HA_MAX_URL + 2];
    build_client_id(client_id, sizeof(client_id));
    json_object_set_new(fields, "client_id", json_string(client_id));

    char *body = json_dumps(fields, JSON_COMPACT);
    json_decref(fields);
    if (!body) {
        return -1;
    }

    struct membuf mem = {0};
    long http = do_request(url, body, NULL, 0, &mem);
    free(body);

    int rc = -1;
    if (http == 200 && mem.data) {
        json_t *root = json_loads(mem.data, 0, NULL);
        if (root) {
            const char *type = json_string_value(json_object_get(root, "type"));
            if (type && strcmp(type, "create_entry") == 0) {
                const char *code = json_string_value(json_object_get(root, "result"));
                if (code) {
                    strncpy(auth_code, code, auth_code_size - 1);
                    auth_code[auth_code_size - 1] = '\0';
                    rc = 0;
                }
            } else if (type && strcmp(type, "form") == 0) {
                // Either an MFA step or the same form again with errors
                // (bad credentials). Distinguish: errors object non-empty
                // means rejected input; a new step means more input needed.
                json_t *errors = json_object_get(root, "errors");
                if (errors && json_object_size(errors) > 0) {
                    rc = -1;
                } else {
                    rc = 1;
                }
            }
            json_decref(root);
        }
    }
    free(mem.data);
    return rc;
}

int ha_auth_login_submit(const char *flow_id, const char *username, const char *password,
                         char *auth_code, size_t auth_code_size) {
    json_t *fields = json_pack("{s:s, s:s}", "username", username, "password", password);
    if (!fields) {
        return -1;
    }
    return login_flow_step(flow_id, fields, auth_code, auth_code_size);
}

int ha_auth_login_mfa(const char *flow_id, const char *mfa_code,
                      char *auth_code, size_t auth_code_size) {
    json_t *fields = json_pack("{s:s}", "code", mfa_code);
    if (!fields) {
        return -1;
    }
    return login_flow_step(flow_id, fields, auth_code, auth_code_size);
}

// Shared /auth/token POST for both the code-exchange and refresh grants.
static int token_request(const char *form_body,
                         char *refresh_token, size_t refresh_size,
                         char *access_token, size_t access_size,
                         int *expires_in) {
    char url[192];
    snprintf(url, sizeof(url), "%s/auth/token", g_base_url);

    struct membuf mem = {0};
    long http = do_request(url, NULL, form_body, 0, &mem);

    int rc = -1;
    if (http == 200 && mem.data) {
        json_t *root = json_loads(mem.data, 0, NULL);
        if (root) {
            const char *access = json_string_value(json_object_get(root, "access_token"));
            if (access) {
                strncpy(access_token, access, access_size - 1);
                access_token[access_size - 1] = '\0';

                json_t *jexp = json_object_get(root, "expires_in");
                *expires_in = json_is_integer(jexp) ? (int)json_integer_value(jexp) : 1800;

                if (refresh_token) {
                    const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
                    if (refresh) {
                        strncpy(refresh_token, refresh, refresh_size - 1);
                        refresh_token[refresh_size - 1] = '\0';
                        rc = 0;
                    }
                } else {
                    rc = 0;
                }
            }
            json_decref(root);
        }
    }
    free(mem.data);
    return rc;
}

// URL-encodes value into out via curl. Returns 0 on success.
static int url_escape(const char *value, char *out, size_t out_size) {
    char *escaped = curl_easy_escape(g_curl, value, 0);
    if (!escaped) {
        return -1;
    }
    int fit = strlen(escaped) < out_size;
    if (fit) {
        strcpy(out, escaped);
    }
    curl_free(escaped);
    return fit ? 0 : -1;
}

int ha_auth_exchange_code(const char *auth_code,
                          char *refresh_token, size_t refresh_size,
                          char *access_token, size_t access_size,
                          int *expires_in) {
    char code_esc[HA_MAX_AUTH_CODE * 3], client_esc[HA_MAX_URL * 3];
    char client_id[HA_MAX_URL + 2];
    build_client_id(client_id, sizeof(client_id));
    if (url_escape(auth_code, code_esc, sizeof(code_esc)) != 0 ||
        url_escape(client_id, client_esc, sizeof(client_esc)) != 0) {
        return -1;
    }

    char body[sizeof(code_esc) + sizeof(client_esc) + 64];
    snprintf(body, sizeof(body), "grant_type=authorization_code&code=%s&client_id=%s",
        code_esc, client_esc);

    return token_request(body, refresh_token, refresh_size, access_token, access_size, expires_in);
}

int ha_auth_refresh(const char *refresh_token,
                    char *access_token, size_t access_size,
                    int *expires_in) {
    char token_esc[HA_MAX_TOKEN * 3], client_esc[HA_MAX_URL * 3];
    char client_id[HA_MAX_URL + 2];
    build_client_id(client_id, sizeof(client_id));
    if (url_escape(refresh_token, token_esc, sizeof(token_esc)) != 0 ||
        url_escape(client_id, client_esc, sizeof(client_esc)) != 0) {
        return -1;
    }

    char body[sizeof(token_esc) + sizeof(client_esc) + 64];
    snprintf(body, sizeof(body), "grant_type=refresh_token&refresh_token=%s&client_id=%s",
        token_esc, client_esc);

    return token_request(body, NULL, 0, access_token, access_size, expires_in);
}

// --- Entity API -------------------------------------------------------------

int ha_fetch_states(ha_entity_t *out, int max_count) {
    char url[192];
    snprintf(url, sizeof(url), "%s/api/states", g_base_url);

    struct membuf mem = {0};
    long http = do_request(url, NULL, NULL, 1, &mem);
    if (http != 200) {
        free(mem.data);
        return -1;
    }

    json_error_t error;
    json_t *root = json_loads(mem.data, 0, &error);
    free(mem.data);

    if (!root || !json_is_array(root)) {
        if (root) {
            json_decref(root);
        }
        return -1;
    }

    int count = 0;
    size_t n = json_array_size(root);
    for (size_t i = 0; i < n && count < max_count; i++) {
        json_t *item = json_array_get(root, i);
        json_t *jid = json_object_get(item, "entity_id");
        const char *eid = jid ? json_string_value(jid) : NULL;
        if (!eid || !is_supported_domain(eid)) {
            continue;
        }

        parse_entity_fields(item, eid, &out[count]);
        count++;
    }

    json_decref(root);
    dbg_log("ha_fetch_states done: count=%d", count);
    return count;
}

int ha_fetch_single_state(const char *entity_id, ha_entity_t *out) {
    char url[320];
    snprintf(url, sizeof(url), "%s/api/states/%s", g_base_url, entity_id);

    struct membuf mem = {0};
    long http = do_request(url, NULL, NULL, 1, &mem);
    if (http != 200) {
        free(mem.data);
        return -1;
    }

    json_error_t error;
    json_t *root = json_loads(mem.data, 0, &error);
    free(mem.data);

    if (!root || !json_is_object(root)) {
        if (root) {
            json_decref(root);
        }
        return -1;
    }

    parse_entity_fields(root, entity_id, out);
    json_decref(root);
    return 0;
}

static int ha_post_service(const char *domain, const char *service, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s/api/services/%s/%s", g_base_url, domain, service);

    struct membuf mem = {0};
    long http = do_request(url, body, NULL, 1, &mem);
    free(mem.data);

    return (http == 200) ? 0 : -1;
}

int ha_toggle_entity(const char *entity_id) {
    char body[HA_MAX_ENTITY_ID + 32];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity_id);
    return ha_post_service("homeassistant", "toggle", body);
}

int ha_set_brightness(const char *entity_id, int brightness_pct) {
    if (brightness_pct < 0) {
        brightness_pct = 0;
    }
    if (brightness_pct > 100) {
        brightness_pct = 100;
    }

    char body[HA_MAX_ENTITY_ID + 64];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"brightness_pct\":%d}", entity_id, brightness_pct);
    return ha_post_service("light", "turn_on", body);
}

int ha_set_temperature(const char *entity_id, float target_temp) {
    char body[HA_MAX_ENTITY_ID + 64];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"temperature\":%.1f}", entity_id, (double)target_temp);
    return ha_post_service("climate", "set_temperature", body);
}

int ha_fetch_area_map(ha_area_entry_t *out, int max_count) {
    if (!g_curl) {
        return -1;
    }

    // Filters to our supported domains server-side, so the response is a
    // short "entity_id|area_name" list instead of every entity on the
    // instance (sensors, automations, scripts, ...). area_name() is a
    // long-standing built-in HA template function; empty string if the
    // entity has no area assigned.
    static const char *tmpl =
        "{% set domains = ['light','switch','fan','lock','cover','input_boolean','climate','media_player'] %}"
        "{% for s in states %}{% if s.domain in domains %}{{ s.entity_id }}|{{ area_name(s.entity_id) or '' }}\n"
        "{% endif %}{% endfor %}";

    json_t *req = json_pack("{s:s}", "template", tmpl);
    if (!req) {
        return -1;
    }
    char *body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (!body) {
        return -1;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/api/template", g_base_url);

    struct membuf mem = {0};
    long http = do_request(url, body, NULL, 1, &mem);
    free(body);

    if (http != 200 || !mem.data) {
        free(mem.data);
        return -1;
    }

    // Manual line/field splitting rather than strtok_r - keeps this
    // independent of newlib providing the reentrant variant.
    int count = 0;
    char *p = mem.data;
    while (*p && count < max_count) {
        char *line_end = strchr(p, '\n');
        if (line_end) {
            *line_end = '\0';
        }

        char *sep = strchr(p, '|');
        if (sep) {
            *sep = '\0';
            strncpy(out[count].entity_id, p, HA_MAX_ENTITY_ID - 1);
            out[count].entity_id[HA_MAX_ENTITY_ID - 1] = '\0';
            strncpy(out[count].area_name, sep + 1, HA_MAX_NAME - 1);
            out[count].area_name[HA_MAX_NAME - 1] = '\0';
            count++;
        }

        if (!line_end) {
            break;
        }
        p = line_end + 1;
    }

    free(mem.data);
    dbg_log("ha_fetch_area_map done: count=%d", count);
    return count;
}
