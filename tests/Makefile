# Host-native unit tests for the pure-logic modules that don't touch
# libctru/citro2d/citro3d/curl: entity_list.c (sorting/filtering/grouping/
# selection), entity_parse.c (JSON -> ha_entity_t mapping), and app_config.c
# (config file load/save). Compiled with the system gcc - NOT devkitARM -
# and run directly on the host. See ../Makefile for the actual 3DS build,
# which this does not touch or depend on.
#
# Requires: gcc, and jansson dev headers/lib (`libjansson-dev` on
# Debian/Ubuntu, `jansson` via pacman/brew elsewhere) for test_entity_parse.
#
# Usage: make -C tests            (build + run all tests)
#        make -C tests coverage   (build + run with gcov line coverage)
#        make -C tests clean
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -I../source -g
JANSSON_CFLAGS := $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS   := $(shell pkg-config --libs jansson 2>/dev/null || echo -ljansson)

BUILD := build
TESTS := test_entity_list test_app_config test_entity_parse
BINS  := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all run clean coverage coverage-clean

all: run

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_entity_list: test_entity_list.c ../source/entity_list.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/test_app_config: test_app_config.c ../source/app_config.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/test_entity_parse: test_entity_parse.c ../source/entity_parse.c | $(BUILD)
	$(CC) $(CFLAGS) $(JANSSON_CFLAGS) -o $@ $^ $(JANSSON_LIBS)

run: $(BINS)
	@status=0; \
	for t in $(BINS); do \
		./$$t || status=1; \
	done; \
	exit $$status

clean:
	rm -rf $(BUILD)

# --- Coverage -----------------------------------------------------------
# Separate object dir + -O0 --coverage build (gcov's line counts get
# unreliable under optimization, since the compiler merges/reorders lines),
# instrumenting only the modules the tests exercise directly - not
# test_*.c themselves, which is why gcov is pointed at the *_impl objects
# below rather than run bare over the whole build-cov dir.
COV_BUILD := build-cov
COVFLAGS  := -std=c11 -Wall -Wextra -I../source -g -O0 --coverage

$(COV_BUILD):
	@mkdir -p $(COV_BUILD)

$(COV_BUILD)/test_entity_list.o: test_entity_list.c | $(COV_BUILD)
	$(CC) $(COVFLAGS) -c -o $@ $<
$(COV_BUILD)/entity_list_impl.o: ../source/entity_list.c | $(COV_BUILD)
	$(CC) $(COVFLAGS) -c -o $@ $<
$(COV_BUILD)/test_entity_list: $(COV_BUILD)/test_entity_list.o $(COV_BUILD)/entity_list_impl.o
	$(CC) $(COVFLAGS) -o $@ $^

$(COV_BUILD)/test_app_config.o: test_app_config.c | $(COV_BUILD)
	$(CC) $(COVFLAGS) -c -o $@ $<
$(COV_BUILD)/app_config_impl.o: ../source/app_config.c | $(COV_BUILD)
	$(CC) $(COVFLAGS) -c -o $@ $<
$(COV_BUILD)/test_app_config: $(COV_BUILD)/test_app_config.o $(COV_BUILD)/app_config_impl.o
	$(CC) $(COVFLAGS) -o $@ $^

$(COV_BUILD)/test_entity_parse.o: test_entity_parse.c | $(COV_BUILD)
	$(CC) $(COVFLAGS) $(JANSSON_CFLAGS) -c -o $@ $<
$(COV_BUILD)/entity_parse_impl.o: ../source/entity_parse.c | $(COV_BUILD)
	$(CC) $(COVFLAGS) $(JANSSON_CFLAGS) -c -o $@ $<
$(COV_BUILD)/test_entity_parse: $(COV_BUILD)/test_entity_parse.o $(COV_BUILD)/entity_parse_impl.o
	$(CC) $(COVFLAGS) -o $@ $^ $(JANSSON_LIBS)

COV_BINS := $(addprefix $(COV_BUILD)/,test_entity_list test_app_config test_entity_parse)
COV_IMPL_OBJS := $(addprefix $(COV_BUILD)/,entity_list_impl.o app_config_impl.o entity_parse_impl.o)

coverage: $(COV_BINS)
	@for t in $(COV_BINS); do ./$$t >/dev/null; done
	@gcov $(COV_IMPL_OBJS) 2>&1 | grep -E "^File|^Lines"
	@mv -f *.gcov $(COV_BUILD)/ 2>/dev/null || true

coverage-clean:
	rm -rf $(COV_BUILD)
