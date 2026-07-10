#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Namespaced under /3ds/ha3ds/ rather than the SD card root - this app is
// open source and installed on other people's cards, and every other
// well-behaved homebrew app keeps its files under its own /3ds/<name>/
// folder instead of scattering them next to boot9strap.firm and everyone
// else's stuff.
#define DATA_DIR "sdmc:/3ds/ha3ds"
#define CONFIG_PATH DATA_DIR "/ha3ds.cfg"

// Format: three lines - base URL, refresh token, enabled-domains bitmask.
// Plain text on the SD card; anyone with physical card access could read
// it, same trust model as every other homebrew app storing credentials
// (and the token is revocable from the HA profile page at any time).
//
// The third line was added after the first two shipped, so configs written
// by older builds only have two lines - read_line() below just leaves the
// mask blank in that case, and it's treated as "not set" -> all domains on,
// matching the pre-Settings-screen behavior exactly.

// mkdir() fails (harmlessly) if a path segment already exists - fopen()
// itself never creates missing parent directories, so this has to run
// before the first write a fresh SD card could ever see.
static void ensure_data_dir(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
}

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

// Path-parameterized core of app_config_load()/app_config_save() - split out
// so tests/test_app_config.c can exercise the parsing/serialization logic
// against an arbitrary file instead of the real sdmc: card path, which only
// resolves under libctru and doesn't exist on a host test build.
int app_config_load_path(const char *path, app_config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    read_line(f, cfg->base_url, sizeof(cfg->base_url));
    read_line(f, cfg->refresh_token, sizeof(cfg->refresh_token));

    // A blank line (no third line at all, i.e. an older two-line config) or
    // a corrupt/non-numeric one (torn write, manual edit) both parse to 0
    // here - strtoul() returns 0 on failure same as on a literal "0", and
    // settings_toggle_domain() never persists 0 itself (it refuses to turn
    // off the last enabled domain), so 0 is never a legitimately-saved value
    // and always means "fall back to everything on" rather than "nothing on".
    char mask_line[16];
    read_line(f, mask_line, sizeof(mask_line));
    unsigned int mask = mask_line[0] ? (unsigned int)strtoul(mask_line, NULL, 10) : 0;
    cfg->enabled_domains = mask ? mask : HA_ALL_DOMAINS_MASK;
    fclose(f);

    return (cfg->base_url[0] && cfg->refresh_token[0]) ? 0 : -1;
}

int app_config_save_path(const char *path, const app_config_t *cfg) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }

    int ok = fprintf(f, "%s\n%s\n%u\n", cfg->base_url, cfg->refresh_token, cfg->enabled_domains) > 0;
    fclose(f);
    return ok ? 0 : -1;
}

int app_config_load(app_config_t *cfg) {
    return app_config_load_path(CONFIG_PATH, cfg);
}

int app_config_save(const app_config_t *cfg) {
    ensure_data_dir();
    return app_config_save_path(CONFIG_PATH, cfg);
}

void app_config_delete(void) {
    remove(CONFIG_PATH);
}
