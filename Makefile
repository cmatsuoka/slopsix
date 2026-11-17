# libslopsix - a DX7-family FM synthesis engine.
#
# Layout:
#   all            the archive and the test binary for the default build
#   check          regenerate-and-compare the tables, then run the harness
#   check-dx7      the default configuration (feature macros off)
#   check-dx100    the DX100 personality build
#   check-extras   the extras build (conveniences beyond the original hardware)
#   tables         regenerate the generated tables under build/tables/
#   tables-install regenerate the tables and write src/slopsix_tables.c
#   clean          remove the build products
#
# The optimisation split is explicit: everything is compiled size-first
# (-Os) except the translation units on the per-sample render path, which
# are compiled speed-first (-O2).

CC     ?= cc
AR     ?= ar
PYTHON ?= python3

CONFIG ?= dx7

# The three configurations the feature macros select.  The DX7 build is
# the default and the macro-off one; it must pass on every change.
ifeq ($(CONFIG),dx7)
  FEATURE_DEFS :=
  ARCHIVE := libslopsix.a
  TESTBIN := slopsix_test
  TESTRUN := ./slopsix_test
endif
ifeq ($(CONFIG),dx100)
  FEATURE_DEFS := -DSLOPSIX_DX100
  ARCHIVE := build/$(CONFIG)/libslopsix.a
  TESTBIN := build/$(CONFIG)/slopsix_test
  TESTRUN := $(TESTBIN)
endif
ifeq ($(CONFIG),extras)
  FEATURE_DEFS := -DSLOPSIX_EXTRAS
  ARCHIVE := build/$(CONFIG)/libslopsix.a
  TESTBIN := build/$(CONFIG)/slopsix_test
  TESTRUN := $(TESTBIN)
endif

BUILD := build/$(CONFIG)

# -Wfloat-conversion and -Wdouble-promotion are where the compiler has
# them; a floating-point operation anywhere in the library is an error.
FLOAT_WARNINGS := -Wfloat-conversion -Wdouble-promotion
HAVE_FLOAT_WARNINGS := $(shell $(CC) $(FLOAT_WARNINGS) -E -x c /dev/null >/dev/null 2>&1 && echo yes)

WARNINGS := -std=c99 -Wall -Wextra -Werror
ifeq ($(HAVE_FLOAT_WARNINGS),yes)
  WARNINGS += $(FLOAT_WARNINGS)
endif

# C never needs exception unwinding, and the compiler-identification
# string is dead weight in the archive.  Drop both when the compiler
# understands the options.
NO_UNWIND_FLAGS := -fno-asynchronous-unwind-tables -fno-unwind-tables \
                   -fno-ident
HAVE_NO_UNWIND := $(shell $(CC) $(NO_UNWIND_FLAGS) -E -x c /dev/null \
                    >/dev/null 2>&1 && echo yes)
ifeq ($(HAVE_NO_UNWIND),yes)
  WARNINGS += $(NO_UNWIND_FLAGS)
endif

OPT_SIZE := -Os
OPT_FAST := -O2

INCLUDES := -Iinclude -Isrc

# The core: context, API glue, the generated tables and the control-rate
# DX7 sources (size-first).
CORE_SRCS := src/slopsix.c src/slopsix_tables.c src/slopsix_bend.c
# The per-sample render path (speed-first).  The operator core is inline in
# src/slopsix_internal.h so it compiles into these translation units.
PERF_SRCS := src/slopsix_eg.c \
             src/slopsix_pitch.c \
             src/slopsix_algorithm.c \
             src/slopsix_output.c

CORE_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRCS))
PERF_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(PERF_SRCS))
OBJS      := $(CORE_OBJS) $(PERF_OBJS)

HEADERS := include/slopsix.h src/slopsix_internal.h

# The generated tables: each generator emits one C fragment, and
# src/slopsix_tables.c is their concatenation in a fixed order.
TABLES := build/tables/logsin_table.c \
          build/tables/exp_table.c \
          build/tables/note_table.c \
          build/tables/com_table.c \
          build/tables/freq_table.c

.PHONY: all check check-dx7 check-dx100 check-extras tables tables-install \
        check-tables clean

all: $(ARCHIVE) $(TESTBIN)

$(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(WARNINGS) $(OPT_SIZE) $(INCLUDES) $(FEATURE_DEFS) -c $< -o $@

# The per-sample path is compiled speed-first.
$(PERF_OBJS): $(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(WARNINGS) $(OPT_FAST) $(INCLUDES) $(FEATURE_DEFS) -c $< -o $@

$(ARCHIVE): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TESTBIN): test/slopsix_test.c $(ARCHIVE)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(OPT_SIZE) $(INCLUDES) $(FEATURE_DEFS) $< $(ARCHIVE) -o $@

# --- generated tables -------------------------------------------------------

tables: $(TABLES)

build/tables/%_table.c: tools/gen_%_table.py
	@mkdir -p build/tables
	$(PYTHON) $< > $@

build/tables/all_tables.c: $(TABLES)
	cat $(TABLES) > $@

# The checked-in tables must be reproducible from the generators.
check-tables: build/tables/all_tables.c
	@cmp src/slopsix_tables.c build/tables/all_tables.c \
	  && echo "tables: reproducible from tools/"

tables-install: build/tables/all_tables.c
	cp build/tables/all_tables.c src/slopsix_tables.c

# --- checks -----------------------------------------------------------------

check: check-tables $(TESTBIN)
	$(TESTRUN)

check-dx7:
	$(MAKE) CONFIG=dx7 check

check-dx100:
	$(MAKE) CONFIG=dx100 check

check-extras:
	$(MAKE) CONFIG=extras check

clean:
	rm -rf build libslopsix.a slopsix_test
