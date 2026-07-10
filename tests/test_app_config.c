// Host-native tests for source/app_config.c's load/save logic, run against a
// temp file via the path-parameterized app_config_load_path()/
// app_config_save_path() (declared here, not in app_config.h - test-only,
// not part of the app's real API) instead of the real sdmc: card path, which
// only resolves under libctru.
#include <stdio.h>
#include <string.h>

#include "../source/app_config.h"
#include "minitest.h"

int app_config_load_path(const char *path, app_config_t *cfg);
int app_config_save_path(const char *path, const app_config_t *cfg);

#define TEST_PATH "/tmp/ha3ds_test_config.tmp"

static void write_raw(const char *content) {
    FILE *f = fopen(TEST_PATH, "w");
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void test_round_trip(void) {
    remove(TEST_PATH);
    app_config_t cfg = {0};
    strcpy(cfg.base_url, "https://ha.example.com");
    strcpy(cfg.refresh_token, "sometoken");
    cfg.enabled_domains = 0x2A;
    CHECK(app_config_save_path(TEST_PATH, &cfg) == 0);

    app_config_t loaded = {0};
    CHECK(app_config_load_path(TEST_PATH, &loaded) == 0);
    CHECK_STREQ(loaded.base_url, "https://ha.example.com");
    CHECK_STREQ(loaded.refresh_token, "sometoken");
    CHECK(loaded.enabled_domains == 0x2Au);
}

static void test_missing_file_fails(void) {
    remove(TEST_PATH);
    app_config_t cfg;
    CHECK(app_config_load_path(TEST_PATH, &cfg) == -1);
}

static void test_legacy_two_line_config_defaults_all_domains_on(void) {
    write_raw("https://ha.example.com\nsometoken\n");
    app_config_t cfg = {0};
    CHECK(app_config_load_path(TEST_PATH, &cfg) == 0);
    CHECK(cfg.enabled_domains == HA_ALL_DOMAINS_MASK);
}

static void test_corrupt_mask_falls_back_to_all_domains(void) {
    write_raw("https://ha.example.com\nsometoken\nNOTANUMBER\n");
    app_config_t cfg = {0};
    CHECK(app_config_load_path(TEST_PATH, &cfg) == 0);
    CHECK(cfg.enabled_domains == HA_ALL_DOMAINS_MASK);
}

static void test_zero_mask_falls_back_to_all_domains(void) {
    write_raw("https://ha.example.com\nsometoken\n0\n");
    app_config_t cfg = {0};
    CHECK(app_config_load_path(TEST_PATH, &cfg) == 0);
    CHECK(cfg.enabled_domains == HA_ALL_DOMAINS_MASK);
}

static void test_incomplete_config_fails(void) {
    write_raw("\nsometoken\n5\n"); // blank base_url
    app_config_t cfg = {0};
    CHECK(app_config_load_path(TEST_PATH, &cfg) == -1);
}

static void test_crlf_line_endings_trimmed(void) {
    write_raw("https://ha.example.com\r\nsometoken\r\n5\r\n");
    app_config_t cfg = {0};
    CHECK(app_config_load_path(TEST_PATH, &cfg) == 0);
    CHECK_STREQ(cfg.base_url, "https://ha.example.com");
    CHECK_STREQ(cfg.refresh_token, "sometoken");
    CHECK(cfg.enabled_domains == 5u);
}

int main(void) {
    printf("test_app_config:\n");
    RUN(test_round_trip);
    RUN(test_missing_file_fails);
    RUN(test_legacy_two_line_config_defaults_all_domains_on);
    RUN(test_corrupt_mask_falls_back_to_all_domains);
    RUN(test_zero_mask_falls_back_to_all_domains);
    RUN(test_incomplete_config_fails);
    RUN(test_crlf_line_endings_trimmed);
    remove(TEST_PATH);
    printf("%d checks, %d failures\n", mt_checks, mt_failures);
    return mt_failures ? 1 : 0;
}
