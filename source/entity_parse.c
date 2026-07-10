#include "entity_parse.h"

#include <string.h>

void ha_parse_entity_fields(json_t *item, const char *eid, ha_entity_t *out) {
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

    // HA's built-in Group platform (light/switch/fan/cover groups, however
    // created - YAML, the Group helper, etc.) stamps attributes.entity_id
    // with the member entity_id list; individual entities never have this
    // attribute. A group's own state is an aggregate over those members and
    // can lag behind a just-issued command longer than a single entity's
    // does - see ha_toggle_entity()'s use of this.
    json_t *jmembers = jattrs ? json_object_get(jattrs, "entity_id") : NULL;
    out->is_group = json_is_array(jmembers) ? 1 : 0;

    // Not available from /api/states - filled in separately by
    // ha_fetch_area_map() and merged in by the caller, if at all.
    out->area_name[0] = '\0';

    // A light is dimmable if supported_color_modes has anything besides
    // "onoff" (every non-onoff color mode implies brightness support in
    // HA's model), or the legacy SUPPORT_BRIGHTNESS bit (0x1) is set in
    // supported_features for integrations that predate color modes.
    // supports_color/supports_color_temp come from the same list: "hs",
    // "xy", "rgb", "rgbw", and "rgbww" all mean the light takes an
    // rgb_color; "color_temp" means it takes a color_temp_kelvin. A light
    // can have both (most CCT+RGB bulbs report two modes and switch between
    // them depending on which parameter was last set).
    out->supports_brightness = 0;
    out->supports_color = 0;
    out->supports_color_temp = 0;
    out->brightness_pct = 100;
    if (jattrs) {
        json_t *jmodes = json_object_get(jattrs, "supported_color_modes");
        if (json_is_array(jmodes)) {
            size_t n_modes = json_array_size(jmodes);
            for (size_t m = 0; m < n_modes; m++) {
                json_t *jmode = json_array_get(jmodes, m);
                const char *mode_str = json_is_string(jmode) ? json_string_value(jmode) : NULL;
                if (!mode_str) {
                    continue;
                }
                if (strcmp(mode_str, "onoff") != 0) {
                    out->supports_brightness = 1;
                }
                if (strcmp(mode_str, "hs") == 0 || strcmp(mode_str, "xy") == 0 ||
                    strcmp(mode_str, "rgb") == 0 || strcmp(mode_str, "rgbw") == 0 ||
                    strcmp(mode_str, "rgbww") == 0) {
                    out->supports_color = 1;
                } else if (strcmp(mode_str, "color_temp") == 0) {
                    out->supports_color_temp = 1;
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
