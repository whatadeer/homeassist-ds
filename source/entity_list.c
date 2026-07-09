#include "entity_list.h"

#include <stdlib.h>
#include <string.h>

ha_entity_t entities[MAX_ENTITIES];
int entity_count = 0;
int scroll_offset = 0;
int selected_index = 0;

char filter_text[FILTER_MAX_LEN] = "";
int visible_indices[MAX_VISIBLE_ROWS];
int visible_count = 0;
int visible_entity_count = 0;

group_mode_t group_mode = GROUP_NONE;

int entity_is_active(const ha_entity_t *e) {
    return strcmp(e->state, "off") != 0;
}

void clamp_selection(void) {
    if (selected_index >= visible_count) {
        selected_index = visible_count - 1;
    }
    if (selected_index < 0) {
        selected_index = 0;
    }

    int max_offset = visible_count - VISIBLE_ROWS;
    if (max_offset < 0) {
        max_offset = 0;
    }
    if (scroll_offset > max_offset) {
        scroll_offset = max_offset;
    }
    if (scroll_offset < 0) {
        scroll_offset = 0;
    }

    // Keep the selected row inside the visible window.
    if (selected_index < scroll_offset) {
        scroll_offset = selected_index;
    }
    if (selected_index >= scroll_offset + VISIBLE_ROWS) {
        scroll_offset = selected_index - VISIBLE_ROWS + 1;
    }
}

// Walks from pos in the given direction until landing on a non-header slot
// (or running off the list). Shared by move_selection() and
// rebuild_visible_list() so "never select a header row" has one
// implementation instead of two that can drift apart.
static int first_selectable_from(int pos, int dir) {
    while (pos >= 0 && pos < visible_count && visible_indices[pos] == ROW_IS_HEADER) {
        pos += dir;
    }
    return pos;
}

void move_selection(int dir) {
    int pos = first_selectable_from(selected_index + dir, dir);
    if (pos >= 0 && pos < visible_count) {
        selected_index = pos;
    }
    clamp_selection();
}

int sticky_group_bar_active(void) {
    return group_mode != GROUP_NONE && scroll_offset >= 0 && scroll_offset < visible_count &&
        visible_indices[scroll_offset] != ROW_IS_HEADER;
}

float list_top_y(void) {
    return FILTER_BOX_HEIGHT + (sticky_group_bar_active() ? STICKY_GROUP_BAR_HEIGHT : 0.0f);
}

// Case-insensitive string comparison - not relying on newlib providing
// strcasecmp, same reasoning as str_contains_ci below.
static int ci_strcmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 32;
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 32;
        }
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        if (ca == '\0') {
            return 0;
        }
        a++;
        b++;
    }
}

static int compare_entity_name_ci(const void *a, const void *b) {
    return ci_strcmp(((const ha_entity_t *)a)->friendly_name, ((const ha_entity_t *)b)->friendly_name);
}

// Groups by area/room first (entities with no assigned area sort last,
// after every named area), then by name within each area.
static int compare_entity_area_then_name_ci(const void *a, const void *b) {
    const ha_entity_t *ea = (const ha_entity_t *)a;
    const ha_entity_t *eb = (const ha_entity_t *)b;

    int a_empty = (ea->area_name[0] == '\0');
    int b_empty = (eb->area_name[0] == '\0');
    if (a_empty != b_empty) {
        return a_empty ? 1 : -1;
    }

    int area_cmp = ci_strcmp(ea->area_name, eb->area_name);
    if (area_cmp != 0) {
        return area_cmp;
    }
    return ci_strcmp(ea->friendly_name, eb->friendly_name);
}

// Active entities first (more actionable than what's already off), then by
// name within each of the two buckets.
static int compare_entity_status_then_name_ci(const void *a, const void *b) {
    const ha_entity_t *ea = (const ha_entity_t *)a;
    const ha_entity_t *eb = (const ha_entity_t *)b;

    int a_active = entity_is_active(ea);
    int b_active = entity_is_active(eb);
    if (a_active != b_active) {
        return a_active ? -1 : 1;
    }
    return ci_strcmp(ea->friendly_name, eb->friendly_name);
}

const char *entity_area_label(const ha_entity_t *e) {
    return e->area_name[0] ? e->area_name : AREA_UNASSIGNED_LABEL;
}

const char *entity_group_label(const ha_entity_t *e) {
    if (group_mode == GROUP_BY_STATUS) {
        return entity_is_active(e) ? "Active" : "Off";
    }
    return entity_area_label(e);
}

// Grouping/boundary key for rebuild_visible_list()'s header insertion -
// distinct from entity_group_label() since a boundary decision needs the
// raw area_name (see rebuild_visible_list's doc comment on
// AREA_UNASSIGNED_LABEL), not the resolved display text.
static const char *entity_group_key(const ha_entity_t *e) {
    if (group_mode == GROUP_BY_STATUS) {
        return entity_is_active(e) ? "1" : "0";
    }
    return e->area_name;
}

void sort_entities(void) {
    int (*cmp)(const void *, const void *) = compare_entity_name_ci;
    if (group_mode == GROUP_BY_ROOM) {
        cmp = compare_entity_area_then_name_ci;
    } else if (group_mode == GROUP_BY_STATUS) {
        cmp = compare_entity_status_then_name_ci;
    }
    qsort(entities, (size_t)entity_count, sizeof(ha_entity_t), cmp);
}

// Case-insensitive substring search, since newlib's strcasestr availability
// isn't guaranteed here and this is cheap to just write directly.
static int str_contains_ci(const char *haystack, const char *needle) {
    if (needle[0] == '\0') {
        return 1;
    }

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) {
        return 0;
    }

    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a += 32;
            }
            if (b >= 'A' && b <= 'Z') {
                b += 32;
            }
            if (a != b) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

void capture_selected_id(char *buf, size_t bufsize) {
    buf[0] = '\0';
    if (selected_index >= 0 && selected_index < visible_count &&
        visible_indices[selected_index] != ROW_IS_HEADER) {
        strncpy(buf, entities[visible_indices[selected_index]].entity_id, bufsize - 1);
        buf[bufsize - 1] = '\0';
    }
}

const ha_entity_t *current_selected_entity(void) {
    if (selected_index < 0 || selected_index >= visible_count) {
        return NULL;
    }
    int idx = visible_indices[selected_index];
    if (idx == ROW_IS_HEADER) {
        return NULL;
    }
    return &entities[idx];
}

// When group_mode isn't GROUP_NONE, a ROW_IS_HEADER sentinel is inserted
// right before the first entity of each group (entities[] is assumed
// already sorted to match - see sort_entities()), so a run of same-group
// entities in the filtered list stays contiguous and gets exactly one
// header.
void rebuild_visible_list(const char *preserved_id) {
    visible_count = 0;
    visible_entity_count = 0;
    // Grouping key of the last entity added - compared case-insensitively,
    // matching sort_entities()'s comparators, so a header boundary is never
    // split or merged by casing alone. For GROUP_BY_ROOM this is the raw
    // area_name (may be "" for unassigned) rather than entity_group_label()'s
    // resolved text, so a real area actually named AREA_UNASSIGNED_LABEL
    // can't be confused with the no-area bucket.
    char last_key[HA_MAX_NAME];
    int have_last_key = 0;
    for (int i = 0; i < entity_count; i++) {
        if (!(str_contains_ci(entities[i].friendly_name, filter_text) ||
              str_contains_ci(entities[i].entity_id, filter_text))) {
            continue;
        }
        if (group_mode != GROUP_NONE) {
            const char *key = entity_group_key(&entities[i]);
            if (!have_last_key || ci_strcmp(key, last_key) != 0) {
                visible_indices[visible_count++] = ROW_IS_HEADER;
                strncpy(last_key, key, sizeof(last_key) - 1);
                last_key[sizeof(last_key) - 1] = '\0';
                have_last_key = 1;
            }
        }
        visible_indices[visible_count++] = i;
        visible_entity_count++;
    }

    selected_index = 0;
    if (preserved_id[0]) {
        for (int i = 0; i < visible_count; i++) {
            if (visible_indices[i] != ROW_IS_HEADER &&
                strcmp(entities[visible_indices[i]].entity_id, preserved_id) == 0) {
                selected_index = i;
                break;
            }
        }
    }
    selected_index = first_selectable_from(selected_index, 1);

    clamp_selection();
}
