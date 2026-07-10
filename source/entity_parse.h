#ifndef ENTITY_PARSE_H
#define ENTITY_PARSE_H

#include <jansson.h>

#include "ha_client.h"

// Fills out->* from a single /api/states-style JSON object (works whether it
// came from the states array or the single-entity endpoint). Split out of
// ha_client.c into its own jansson-only translation unit (no curl, no
// cacert_bin.h) specifically so this - the most fiddly logic in the app:
// color-mode classification, brightness percent rounding, climate setpoint
// fallback - can be exercised directly by tests/test_entity_parse.c instead
// of only against a live Home Assistant instance.
void ha_parse_entity_fields(json_t *item, const char *eid, ha_entity_t *out);

#endif
