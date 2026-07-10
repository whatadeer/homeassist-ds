// Host-native tests for source/entity_list.c: sorting, filtering, grouping
// header insertion, and selection/scroll bookkeeping. This module has no
// citro2d/curl/libctru dependency at all, so it's compiled and run as-is -
// no stubs, no extraction needed.
#include <stdio.h>
#include <string.h>

#include "../source/entity_list.h"
#include "minitest.h"

static void reset(void) {
    entity_count = 0;
    memset(entities, 0, sizeof(entities));
    scroll_offset = 0;
    selected_index = 0;
    filter_text[0] = '\0';
    visible_count = 0;
    visible_entity_count = 0;
    group_mode = GROUP_NONE;
}

static void add_entity(const char *id, const char *name, const char *state, const char *area) {
    ha_entity_t *e = &entities[entity_count++];
    strncpy(e->entity_id, id, sizeof(e->entity_id) - 1);
    strncpy(e->friendly_name, name, sizeof(e->friendly_name) - 1);
    strncpy(e->state, state, sizeof(e->state) - 1);
    strncpy(e->area_name, area, sizeof(e->area_name) - 1);
}

static void test_entity_is_active(void) {
    ha_entity_t e = {0};
    strcpy(e.state, "off");
    CHECK(!entity_is_active(&e));
    strcpy(e.state, "on");
    CHECK(entity_is_active(&e));
    // lock/cover never report "off" at all - always active under this rule.
    strcpy(e.state, "locked");
    CHECK(entity_is_active(&e));
}

static void test_sort_by_name_ci(void) {
    reset();
    add_entity("light.b", "banana", "on", "");
    add_entity("light.a", "Apple", "on", "");
    add_entity("light.c", "cherry", "on", "");
    group_mode = GROUP_NONE;
    sort_entities();
    CHECK_STREQ(entities[0].friendly_name, "Apple");
    CHECK_STREQ(entities[1].friendly_name, "banana");
    CHECK_STREQ(entities[2].friendly_name, "cherry");
}

static void test_sort_by_room_unassigned_last(void) {
    reset();
    add_entity("light.a", "Alpha", "on", "");         // unassigned
    add_entity("light.b", "Beta", "on", "Kitchen");
    add_entity("light.c", "Gamma", "on", "Attic");
    group_mode = GROUP_BY_ROOM;
    sort_entities();
    CHECK_STREQ(entities[0].area_name, "Attic");
    CHECK_STREQ(entities[1].area_name, "Kitchen");
    CHECK_STREQ(entities[2].friendly_name, "Alpha"); // unassigned sorts last regardless of name
}

static void test_sort_by_status_active_first(void) {
    reset();
    add_entity("light.a", "Zed", "off", "");
    add_entity("light.b", "Anna", "on", "");
    add_entity("light.c", "Milo", "off", "");
    group_mode = GROUP_BY_STATUS;
    sort_entities();
    CHECK_STREQ(entities[0].friendly_name, "Anna"); // the only active one
    CHECK_STREQ(entities[1].friendly_name, "Milo"); // off, alpha order after
    CHECK_STREQ(entities[2].friendly_name, "Zed");
}

static void test_rebuild_visible_list_filter_matches_name(void) {
    reset();
    add_entity("light.kitchen", "Kitchen Light", "on", "");
    add_entity("switch.fan", "Fan Switch", "off", "");
    strcpy(filter_text, "kitchen");
    rebuild_visible_list("");
    CHECK(visible_count == 1);
    CHECK(visible_entity_count == 1);
    CHECK(visible_indices[0] == 0);
}

static void test_rebuild_visible_list_filter_matches_entity_id(void) {
    reset();
    add_entity("light.kitchen", "Main Light", "on", "");
    add_entity("switch.other", "Other", "off", "");
    strcpy(filter_text, "switch.");
    rebuild_visible_list("");
    CHECK(visible_count == 1);
    CHECK(visible_indices[0] == 1);
}

static void test_rebuild_visible_list_grouping_headers(void) {
    reset();
    add_entity("light.a", "A", "on", "Kitchen");
    add_entity("light.b", "B", "on", "Kitchen");
    add_entity("light.c", "C", "on", "Attic");
    group_mode = GROUP_BY_ROOM;
    sort_entities();
    rebuild_visible_list("");
    // Sorted: C(Attic), A(Kitchen), B(Kitchen) -> header, C, header, A, B
    CHECK(visible_count == 5);
    CHECK(visible_indices[0] == ROW_IS_HEADER);
    CHECK(visible_indices[2] == ROW_IS_HEADER);
    CHECK(visible_entity_count == 3);
}

static void test_unassigned_label_not_confused_with_real_area_of_same_name(void) {
    reset();
    add_entity("light.a", "A", "on", "");                    // unassigned
    add_entity("light.b", "B", "on", AREA_UNASSIGNED_LABEL); // a *real* area named "Ungrouped"
    group_mode = GROUP_BY_ROOM;
    sort_entities();
    rebuild_visible_list("");
    // Grouping keys off the raw area_name, not the display label, so these
    // stay two distinct headers even though both display the same text.
    int header_count = 0;
    for (int i = 0; i < visible_count; i++) {
        if (visible_indices[i] == ROW_IS_HEADER) {
            header_count++;
        }
    }
    CHECK(header_count == 2);
}

static void test_move_selection_skips_headers(void) {
    reset();
    add_entity("light.a", "A", "on", "Attic");
    add_entity("light.b", "B", "on", "Kitchen");
    group_mode = GROUP_BY_ROOM;
    sort_entities();
    rebuild_visible_list("");
    // visible: [header Attic, A, header Kitchen, B] - lands on A first.
    CHECK(selected_index == 1);
    move_selection(1);
    CHECK(selected_index == 3); // stepped clean over the Kitchen header to B
}

static void test_capture_and_current_selected_entity(void) {
    reset();
    add_entity("light.a", "A", "on", "");
    add_entity("light.b", "B", "on", "");
    rebuild_visible_list("");
    selected_index = 1;
    char buf[HA_MAX_ENTITY_ID];
    capture_selected_id(buf, sizeof(buf));
    CHECK_STREQ(buf, "light.b");
    CHECK(current_selected_entity() == &entities[1]);
}

static void test_rebuild_visible_list_preserves_selection_by_id(void) {
    reset();
    add_entity("light.a", "A", "on", "");
    add_entity("light.b", "B", "on", "");
    rebuild_visible_list("");
    rebuild_visible_list("light.b");
    CHECK(selected_index == 1);
}

static void test_clamp_selection_scroll_window(void) {
    reset();
    for (int i = 0; i < 20; i++) {
        char id[16], name[16];
        snprintf(id, sizeof(id), "light.%d", i);
        snprintf(name, sizeof(name), "L%02d", i);
        add_entity(id, name, "on", "");
    }
    rebuild_visible_list("");

    selected_index = 15;
    clamp_selection();
    CHECK(scroll_offset == 15 - VISIBLE_ROWS + 1);

    selected_index = 0;
    clamp_selection();
    CHECK(scroll_offset == 0);
}

static void test_entity_area_label_and_group_label(void) {
    ha_entity_t e = {0};
    strcpy(e.state, "on");
    CHECK_STREQ(entity_area_label(&e), AREA_UNASSIGNED_LABEL);
    strcpy(e.area_name, "Den");
    CHECK_STREQ(entity_area_label(&e), "Den");

    group_mode = GROUP_BY_STATUS;
    CHECK_STREQ(entity_group_label(&e), "Active");
    strcpy(e.state, "off");
    CHECK_STREQ(entity_group_label(&e), "Off");

    group_mode = GROUP_BY_ROOM;
    CHECK_STREQ(entity_group_label(&e), "Den");
}

static void test_sticky_group_bar_and_list_top_y(void) {
    reset();
    add_entity("light.a", "A", "on", "Attic");
    add_entity("light.b", "B", "on", "Kitchen");
    group_mode = GROUP_BY_ROOM;
    sort_entities();
    rebuild_visible_list("");

    // scroll_offset sits on the Attic header row itself - bar not needed yet.
    CHECK(!sticky_group_bar_active());
    CHECK(list_top_y() == FILTER_BOX_HEIGHT);

    scroll_offset = 1; // now mid-group: header has scrolled above the window
    CHECK(sticky_group_bar_active());
    CHECK(list_top_y() == FILTER_BOX_HEIGHT + STICKY_GROUP_BAR_HEIGHT);
}

int main(void) {
    printf("test_entity_list:\n");
    RUN(test_entity_is_active);
    RUN(test_sort_by_name_ci);
    RUN(test_sort_by_room_unassigned_last);
    RUN(test_sort_by_status_active_first);
    RUN(test_rebuild_visible_list_filter_matches_name);
    RUN(test_rebuild_visible_list_filter_matches_entity_id);
    RUN(test_rebuild_visible_list_grouping_headers);
    RUN(test_unassigned_label_not_confused_with_real_area_of_same_name);
    RUN(test_move_selection_skips_headers);
    RUN(test_capture_and_current_selected_entity);
    RUN(test_rebuild_visible_list_preserves_selection_by_id);
    RUN(test_clamp_selection_scroll_window);
    RUN(test_entity_area_label_and_group_label);
    RUN(test_sticky_group_bar_and_list_top_y);
    printf("%d checks, %d failures\n", mt_checks, mt_failures);
    return mt_failures ? 1 : 0;
}
