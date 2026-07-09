#ifndef ENTITY_LIST_H
#define ENTITY_LIST_H

#include <stddef.h>

#include "ha_client.h"

#define MAX_ENTITIES 64
// Grouped view interleaves a non-selectable header row before each area's
// first entity, so the visible list can hold up to 2x the raw entity count
// (worst case: every entity in its own area).
#define MAX_VISIBLE_ROWS (MAX_ENTITIES * 2)
// Sentinel stored in visible_indices[] for a header row - never a valid
// entities[] index.
#define ROW_IS_HEADER (-1)
// Fallback header/group label for entities with no assigned area. Deliberately
// not "Other" - a real Home Assistant area can legitimately be named "Other",
// which would otherwise show two disconnected header rows with the same text.
#define AREA_UNASSIGNED_LABEL "Ungrouped"
#define VISIBLE_ROWS 8
#define ROW_HEIGHT 24
#define FILTER_BOX_HEIGHT 22
// Height of the persistent "current group" bar shown between the filter box
// and the list - see sticky_group_bar_active(). Doesn't consume one of the
// VISIBLE_ROWS row slots, just pushes the list's start Y down, so scrolling
// math (list positions, not pixels) is unaffected by whether it's shown.
#define STICKY_GROUP_BAR_HEIGHT 16.0f
#define FILTER_MAX_LEN 32

extern ha_entity_t entities[MAX_ENTITIES];
extern int entity_count;
extern int scroll_offset;
extern int selected_index;

// Entities matching filter_text (case-insensitive substring of name or
// entity_id), in original order, interleaved with ROW_IS_HEADER sentinels
// when group_mode isn't GROUP_NONE (one before each group's first entity).
// Header rows are never selectable. selected_index/scroll_offset are
// positions into THIS list, not directly into `entities` - always go
// through visible_indices[] to get the real entities[] index.
extern char filter_text[FILTER_MAX_LEN];
extern int visible_indices[MAX_VISIBLE_ROWS];
extern int visible_count;
// Count of real entity rows in visible_indices[] (visible_count minus header
// rows) - what the filter box's "(shown/total)" text should report, since
// visible_count itself includes non-entity header rows when grouped.
extern int visible_entity_count;

// Cycled by X. GROUP_BY_ROOM sorts entities[] by (area, name) and inserts a
// header before each area's entities; room data comes from a separate
// template-based fetch (ha_fetch_area_map) merged in after every full
// refresh - see the worker's OP_REFRESH branch. GROUP_BY_STATUS sorts by
// (active, name) and inserts one header for the active entities and one for
// the rest, using the same on/off rule as entity_is_active().
typedef enum {
    GROUP_NONE = 0,
    GROUP_BY_ROOM,
    GROUP_BY_STATUS,
} group_mode_t;
extern group_mode_t group_mode;

// True unless the entity's state is exactly "off" - the one state string
// every domain here agrees means "inactive". Domains that never report
// "off" at all (lock: locked/unlocked, cover: open/closed) are always
// active under this rule, which is deliberate: dimming reads as "not
// running", and neither a locked nor an open state is a not-running
// condition worth muting.
int entity_is_active(const ha_entity_t *e);

// Display label for an entity's area/room: its own area_name, or
// AREA_UNASSIGNED_LABEL if it has none. Only for display - grouping
// decisions in rebuild_visible_list compare the raw area_name instead, so a
// real area actually named AREA_UNASSIGNED_LABEL can't merge with the
// no-area bucket.
const char *entity_area_label(const ha_entity_t *e);

// Label for an entity's current group under group_mode: entity_area_label()
// for GROUP_BY_ROOM, "Active"/"Off" for GROUP_BY_STATUS. Used for header-row
// and sticky-group-bar text, which always reflect the active grouping -
// unlike entity_area_label(), which the top-screen hero uses unconditionally
// to show an entity's actual room regardless of how the list is grouped.
const char *entity_group_label(const ha_entity_t *e);

// Sorts entities[] in place using whichever order is currently selected.
// Callers must capture_selected_id() before calling this (sorting changes
// which index the previously-selected entity sits at) and
// rebuild_visible_list() after.
void sort_entities(void);

// Copies the currently-selected entity's id into buf (empty string if no
// valid selection). Must be called while visible_indices[] still matches
// the entities[] array it was built from - i.e. BEFORE overwriting
// entities[] with a fresh fetch.
void capture_selected_id(char *buf, size_t bufsize);

// The entity the currently-selected row points at, or NULL if there isn't
// one (empty/filtered-to-nothing list) - same bounds check as
// capture_selected_id(), just returning a pointer instead of copying an id.
const ha_entity_t *current_selected_entity(void);

// Recomputes visible_indices[] from entities[]/filter_text, re-selecting
// preserved_id (captured by the caller via capture_selected_id) if it's
// still in the filtered list, else resetting to the first real row. See
// entity_list.c for the full doc comment on header-row insertion.
void rebuild_visible_list(const char *preserved_id);

// Clamps selected_index/scroll_offset to the current visible list bounds
// and keeps the selected row inside the visible scroll window.
void clamp_selection(void);

// Moves selected_index by dir (+-1), stepping over any header row so
// Up/Down always lands on a real entity. If dir runs off the end of the
// list without finding one, the selection is left where it was.
void move_selection(int dir);

// True when the top of the current scroll window is mid-group - the
// group's own header has scrolled above the visible list.
int sticky_group_bar_active(void);

// Y where the entity list starts, pushed down when sticky_group_bar_active().
float list_top_y(void);

#endif
