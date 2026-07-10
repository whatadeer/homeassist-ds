// Host-native tests for source/entity_parse.c: JSON -> ha_entity_t field
// mapping, extracted out of ha_client.c specifically so it can be exercised
// here without a live Home Assistant instance or curl. See entity_parse.h
// for why this function exists as its own translation unit.
#include <stdio.h>

#include <jansson.h>

#include "../source/entity_parse.h"
#include "minitest.h"

static json_t *load_json(const char *text) {
    json_error_t err;
    json_t *j = json_loads(text, 0, &err);
    if (!j) {
        fprintf(stderr, "    json parse error: %s\n", err.text);
    }
    return j;
}

static void test_basic_fields(void) {
    json_t *item = load_json(
        "{\"state\":\"on\",\"attributes\":{\"friendly_name\":\"Kitchen Light\"}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.kitchen", &e);
    CHECK_STREQ(e.entity_id, "light.kitchen");
    CHECK_STREQ(e.state, "on");
    CHECK_STREQ(e.friendly_name, "Kitchen Light");
    CHECK(!e.is_group);
    json_decref(item);
}

static void test_missing_friendly_name_falls_back_to_entity_id(void) {
    json_t *item = load_json("{\"state\":\"on\"}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "switch.unnamed", &e);
    CHECK_STREQ(e.friendly_name, "switch.unnamed");
    json_decref(item);
}

static void test_missing_state_defaults_to_unknown(void) {
    json_t *item = load_json("{}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.x", &e);
    CHECK_STREQ(e.state, "unknown");
    json_decref(item);
}

static void test_group_detected_via_member_entity_id_list(void) {
    json_t *item = load_json(
        "{\"state\":\"on\",\"attributes\":{\"entity_id\":[\"light.a\",\"light.b\"]}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.den_group", &e);
    CHECK(e.is_group);
    json_decref(item);
}

static void test_onoff_only_light_has_no_brightness_or_color_support(void) {
    json_t *item = load_json(
        "{\"state\":\"on\",\"attributes\":{\"supported_color_modes\":[\"onoff\"]}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.simple", &e);
    CHECK(!e.supports_brightness);
    CHECK(!e.supports_color);
    CHECK(!e.supports_color_temp);
    json_decref(item);
}

static void test_rgb_mode_implies_brightness_and_color(void) {
    json_t *item = load_json(
        "{\"state\":\"on\",\"attributes\":{\"supported_color_modes\":[\"rgb\"],\"brightness\":128}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.rgb", &e);
    CHECK(e.supports_brightness);
    CHECK(e.supports_color);
    CHECK(!e.supports_color_temp);
    CHECK(e.brightness_pct == 50); // (128*100+127)/255 = 50
    json_decref(item);
}

static void test_color_temp_mode(void) {
    json_t *item = load_json(
        "{\"state\":\"on\",\"attributes\":{\"supported_color_modes\":[\"color_temp\"]}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.cct", &e);
    CHECK(e.supports_brightness);
    CHECK(!e.supports_color);
    CHECK(e.supports_color_temp);
    json_decref(item);
}

static void test_legacy_brightness_feature_bit(void) {
    json_t *item = load_json("{\"state\":\"on\",\"attributes\":{\"supported_features\":1}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.legacy", &e);
    CHECK(e.supports_brightness);
    json_decref(item);
}

static void test_brightness_default_100_when_absent(void) {
    json_t *item = load_json("{\"state\":\"on\"}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.x", &e);
    CHECK(e.brightness_pct == 100);
    json_decref(item);
}

static void test_brightness_rounding_full_scale(void) {
    json_t *item = load_json("{\"state\":\"on\",\"attributes\":{\"brightness\":255}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "light.full", &e);
    CHECK(e.brightness_pct == 100);
    json_decref(item);
}

static void test_climate_entity_reads_current_and_target_temp(void) {
    json_t *item = load_json(
        "{\"state\":\"heat\",\"attributes\":{\"current_temperature\":21.5,\"temperature\":22.0}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "climate.living_room", &e);
    CHECK(e.is_climate);
    CHECK(e.current_temp > 21.49f && e.current_temp < 21.51f);
    CHECK(e.target_temp > 21.99f && e.target_temp < 22.01f);
    json_decref(item);
}

static void test_climate_range_mode_falls_back_to_low_setpoint(void) {
    json_t *item = load_json(
        "{\"state\":\"heat_cool\",\"attributes\":{\"target_temp_low\":18.0,\"target_temp_high\":24.0}}");
    ha_entity_t e;
    ha_parse_entity_fields(item, "climate.range", &e);
    CHECK(e.is_climate);
    CHECK(e.target_temp > 17.99f && e.target_temp < 18.01f);
    json_decref(item);
}

static void test_non_climate_entity_is_not_flagged_climate(void) {
    json_t *item = load_json("{\"state\":\"on\"}");
    ha_entity_t e;
    // Domain prefix is "light", not "climate" - the substring elsewhere in
    // the id must not trip the domain check.
    ha_parse_entity_fields(item, "light.climate_control_lamp", &e);
    CHECK(!e.is_climate);
    json_decref(item);
}

int main(void) {
    printf("test_entity_parse:\n");
    RUN(test_basic_fields);
    RUN(test_missing_friendly_name_falls_back_to_entity_id);
    RUN(test_missing_state_defaults_to_unknown);
    RUN(test_group_detected_via_member_entity_id_list);
    RUN(test_onoff_only_light_has_no_brightness_or_color_support);
    RUN(test_rgb_mode_implies_brightness_and_color);
    RUN(test_color_temp_mode);
    RUN(test_legacy_brightness_feature_bit);
    RUN(test_brightness_default_100_when_absent);
    RUN(test_brightness_rounding_full_scale);
    RUN(test_climate_entity_reads_current_and_target_temp);
    RUN(test_climate_range_mode_falls_back_to_low_setpoint);
    RUN(test_non_climate_entity_is_not_flagged_climate);
    printf("%d checks, %d failures\n", mt_checks, mt_failures);
    return mt_failures ? 1 : 0;
}
