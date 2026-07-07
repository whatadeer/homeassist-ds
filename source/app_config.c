#include "app_config.h"

#include <stdio.h>
#include <string.h>

#define CONFIG_PATH "sdmc:/ha3ds.cfg"

// Format: two lines - base URL, then refresh token. Plain text on the SD
// card; anyone with physical card access could read it, same trust model
// as every other homebrew app storing credentials (and the token is
// revocable from the HA profile page at any time).

static void read_line(FILE *f, char *out, size_t out_size) {
    out[0] = '\0';
    if (!fgets(out, out_size, f)) {
        return;
    }
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
}

int app_config_load(app_config_t *cfg) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        return -1;
    }

    read_line(f, cfg->base_url, sizeof(cfg->base_url));
    read_line(f, cfg->refresh_token, sizeof(cfg->refresh_token));
    fclose(f);

    return (cfg->base_url[0] && cfg->refresh_token[0]) ? 0 : -1;
}

int app_config_save(const app_config_t *cfg) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return -1;
    }

    int ok = fprintf(f, "%s\n%s\n", cfg->base_url, cfg->refresh_token) > 0;
    fclose(f);
    return ok ? 0 : -1;
}

void app_config_delete(void) {
    remove(CONFIG_PATH);
}
