#include "ha_client.h"
#include "config.h"
#include "dbglog.h"

#include <curl/curl.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Generated at build time from data/cacert.bin (Mozilla's CA bundle, as
// distributed by curl.se) via the Makefile's DATA -> bin2s pipeline. Needed
// because HA_BASE_URL is reached over the public internet with a real
// CA-signed cert - the 3DS has no system trust store for libcurl to use.
#include "cacert_bin.h"

// Domains homeassistant.toggle can meaningfully act on. Filters the (often
// huge) /api/states response down to things worth showing on a small screen.
static const char *SUPPORTED_DOMAINS[] = {
    "light", "switch", "fan", "lock", "cover",
    "input_boolean", "climate", "media_player",
};
#define NUM_SUPPORTED_DOMAINS (sizeof(SUPPORTED_DOMAINS) / sizeof(SUPPORTED_DOMAINS[0]))

// Reused across every request instead of curl_easy_init()/cleanup() per
// call. That let libcurl tear down and renegotiate a fresh TCP+TLS
// connection - including a full asymmetric handshake - on every single
// request. The 3DS's ARM11 core does that handshake crypto in software and
// it's slow; keeping one handle alive lets libcurl reuse the underlying
// connection across calls to the same host, which is the single biggest
// latency win available here. Only ever touched from the network worker
// thread (see main.c) - never concurrently, so no locking needed.
static CURL *g_curl = NULL;

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
}

// (Re)configures g_curl for one request. Safe to call repeatedly on the
// same handle - each call just overwrites the relevant options rather than
// resetting the handle, so libcurl's connection cache survives across
// calls and gets reused for requests to the same host.
static void prepare_request(const char *url, struct curl_slist **headers_out) {
    char auth_header[512]; // HA long-lived access tokens are JWTs, typically 180-300+ chars.
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", HA_TOKEN);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);

    *headers_out = headers;
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
    // Requests round-trip over the internet (no LAN shortcut), so give them
    // more room than a same-network call would need.
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 8L);

    // Verify the server's cert against the bundled CA root store. Explicit
    // even though these are curl's defaults, since a bad HA_BASE_URL cert
    // failing silently would be worse than it failing loudly here.
    struct curl_blob ca_blob; // CURL_BLOB_COPY below makes curl copy the data immediately, so this struct doesn't need to outlive the call
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

// Shared GET + write-buffer plumbing for ha_fetch_states/ha_fetch_single_state.
static int ha_get(const char *url, struct membuf *mem) {
    if (!g_curl) {
        return -1;
    }

    struct curl_slist *headers = NULL;
    prepare_request(url, &headers);
    curl_easy_setopt(g_curl, CURLOPT_HTTPGET, 1L); // undo any prior POST left set on the reused handle
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, mem);

    CURLcode res = curl_easy_perform(g_curl);
    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    dbg_log("GET %s: res=%d (%s) http_code=%ld", url, res, curl_easy_strerror(res), http_code);

    curl_slist_free_all(headers);

    return (res == CURLE_OK && http_code == 200) ? 0 : -1;
}

int ha_fetch_states(ha_entity_t *out, int max_count) {
    char url[256];
    snprintf(url, sizeof(url), "%s/api/states", HA_BASE_URL);

    struct membuf mem = {0};
    if (ha_get(url, &mem) != 0) {
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
    char url[256];
    snprintf(url, sizeof(url), "%s/api/states/%s", HA_BASE_URL, entity_id);

    struct membuf mem = {0};
    if (ha_get(url, &mem) != 0) {
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
    if (!g_curl) {
        return -1;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/api/services/%s/%s", HA_BASE_URL, domain, service);

    struct curl_slist *headers = NULL;
    prepare_request(url, &headers);

    struct membuf mem = {0};
    curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &mem);

    CURLcode res = curl_easy_perform(g_curl);
    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    dbg_log("ha_post_service %s/%s: res=%d http_code=%ld", domain, service, res, http_code);

    curl_slist_free_all(headers);
    free(mem.data);

    return (res == CURLE_OK && http_code == 200) ? 0 : -1;
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
