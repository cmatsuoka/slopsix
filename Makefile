# libslopsix - a DX7-family FM synthesis engine.
#
# Layout:
#   all            the archive and the test binary for the default build
#   check          regenerate-and-compare the tables, then run the harness
#   check-dx7      the default configuration (feature macros off)
#   check-dx7s     the DX7S personality build
#   check-dx7s-extras the DX7S personality with the extras conveniences
#   check-extras   the extras build (conveniences beyond the original hardware)
#   example        build the OpenAL terminal keyboard player
#   validate-render build the slopsix side of the validation loop
#   validate       build it and run tools/validate.py with BANKS="..."
#   tables         regenerate the generated tables under build/tables/
#   tables-install regenerate the tables and write src/slopsix_tables.c
#   clean          remove the build products
#
# The optimisation split is explicit: everything is compiled size-first
# (-Os) except the translation units on the per-sample render path, which
# are compiled speed-first (-O3).  The algorithm schedule and block
# renderer are the exception inside that path: compiling them at -O3
# duplicates the operator core across the specialized runs for almost no
# speed gain, so they stay at -O2.

CC     ?= cc
AR     ?= ar
PYTHON ?= python3

CONFIG ?= dx7

# The configurations the feature macros select.  The DX7 build is the
# default and the macro-off one; it must pass on every change.
ifeq ($(CONFIG),dx7)
  FEATURE_DEFS :=
  ARCHIVE := libslopsix.a
  TESTBIN := slopsix_test
  TESTRUN := ./slopsix_test
endif
# The DX7S configuration.  SLOPSIX_DX7S is the personality's controlling
# macro: it selects the configuration, enables the shared DX7S paths and
# brings the personality's own translation units under src/dx7s/ into the
# build.
ifeq ($(CONFIG),dx7s)
  FEATURE_DEFS := -DSLOPSIX_DX7S
  ARCHIVE := build/$(CONFIG)/libslopsix.a
  TESTBIN := build/$(CONFIG)/slopsix_test
  TESTRUN := $(TESTBIN)
endif
# The DX7S configuration with the extras conveniences: both feature macros
# on, so the paths that only exist when they are combined (the DX7S's pan
# split) are built and tested.
ifeq ($(CONFIG),dx7s-extras)
  FEATURE_DEFS := -DSLOPSIX_DX7S -DSLOPSIX_EXTRAS
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

# The example streams the engine through OpenAL; it is a host program, not
# part of the library.  pkg-config locates the headers and libraries when it
# can, otherwise the usual -lopenal link is tried.
OPENAL_CFLAGS := $(shell pkg-config --cflags openal 2>/dev/null)
OPENAL_LIBS   := $(shell pkg-config --libs openal 2>/dev/null || echo -lopenal)
EXAMPLE       := $(BUILD)/examples/play_keys

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
OPT_FAST := -O3
OPT_ALGORITHM := -O2

INCLUDES := -Iinclude -Isrc

# The core: context, API glue, the generated tables and the control-rate
# DX7 sources (size-first).
CORE_SRCS := src/slopsix.c src/slopsix_tables.c src/slopsix_bend.c \
             src/slopsix_pitch_eg.c src/slopsix_lfo.c
# The per-sample render path (speed-first).  The operator core is inline in
# src/slopsix_internal.h so it compiles into these translation units.
PERF_SRCS := src/slopsix_eg.c \
             src/slopsix_pitch.c \
             src/slopsix_output.c \
             src/slopsix_filter.c \
             src/slopsix_voice.c

# The optional personality's translation units (only the DX7S
# configurations set these).  The per-voice LFO is control-rate
# (size-first); the DX7S process loop is on the render path (speed-first).
ifneq ($(filter dx7s dx7s-extras,$(CONFIG)),)
  DX7S_CORE_SRCS := src/dx7s/slopsix_lfo.c \
                    src/dx7s/slopsix_filter.c \
                    src/dx7s/slopsix_pitch_eg.c
  DX7S_PERF_SRCS := src/dx7s/slopsix_voice.c \
                    src/dx7s/slopsix_output.c
endif
CORE_SRCS += $(DX7S_CORE_SRCS)
PERF_SRCS += $(DX7S_PERF_SRCS)

# The algorithm schedule and block renderer.  This is on the render path but
# is compiled at -O2: -O3 expands the schedule decode and the specialized
# block runs (the operator core is inlined once per run) without a
# measurable speed gain over -O2.
ALG_SRCS := src/slopsix_algorithm.c

CORE_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRCS))
PERF_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(PERF_SRCS))
ALG_OBJS  := $(patsubst src/%.c,$(BUILD)/%.o,$(ALG_SRCS))
OBJS      := $(CORE_OBJS) $(PERF_OBJS) $(ALG_OBJS)

HEADERS := include/slopsix.h src/slopsix_internal.h

# The generated tables: each generator emits one C fragment, and
# src/slopsix_tables.c is their concatenation in a fixed order.
TABLES := build/tables/logsin_table.c \
          build/tables/exp_table.c \
          build/tables/note_table.c \
          build/tables/com_table.c \
          build/tables/freq_table.c \
          build/tables/filter_table.c

.PHONY: all check check-dx7 check-dx7s check-dx7s-extras \
        check-extras example tables tables-install check-tables \
        validate-render validate clean

all: $(ARCHIVE) $(TESTBIN)

$(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(OPT_SIZE) $(INCLUDES) $(FEATURE_DEFS) -c $< -o $@

# The per-sample path is compiled speed-first.
$(PERF_OBJS): $(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(OPT_FAST) $(INCLUDES) $(FEATURE_DEFS) -c $< -o $@

# The algorithm schedule and block renderer stay at -O2 (see above).
$(ALG_OBJS): $(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(OPT_ALGORITHM) $(INCLUDES) $(FEATURE_DEFS) -c $< -o $@

$(ARCHIVE): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TESTBIN): test/slopsix_test.c $(ARCHIVE)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(OPT_SIZE) $(INCLUDES) $(FEATURE_DEFS) $< $(ARCHIVE) -o $@

# --- example ----------------------------------------------------------------

example: $(EXAMPLE)

$(EXAMPLE): examples/play_keys.c $(ARCHIVE)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(OPT_SIZE) $(INCLUDES) $(FEATURE_DEFS) \
	    $(OPENAL_CFLAGS) $< $(ARCHIVE) $(OPENAL_LIBS) -o $@

# --- validation loop -------------------------------------------------------

# The validation loop compares rendered audio, not engine state.  The
# slopsix renderer is the only part built here; the reference renderers are
# external artefacts and are passed to the script by path.
VALIDATE_RENDER := $(BUILD)/validation/render_slopsix

validate-render: $(VALIDATE_RENDER)

$(VALIDATE_RENDER): tools/validate_render.c $(ARCHIVE)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(OPT_FAST) $(INCLUDES) $(FEATURE_DEFS) $< \
	    $(ARCHIVE) -o $@

# Run the comparison when BANKS names the input sysex banks (they are
# out-of-repo inputs, so they are supplied on the command line):
#   make validate BANKS="bank1.syx bank2.syx"
validate: $(VALIDATE_RENDER)
	@if [ -z "$(BANKS)" ]; then \
	    echo "validate: built $(VALIDATE_RENDER)"; \
	    echo "run: python3 tools/validate.py --bank BANK.syx ..."; \
	else \
	    $(PYTHON) tools/validate.py \
	        $(foreach b,$(BANKS),--bank $(b)) $(VALIDATE_ARGS); \
	fi

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

check-dx7s:
	$(MAKE) CONFIG=dx7s check

check-dx7s-extras:
	$(MAKE) CONFIG=dx7s-extras check

check-extras:
	$(MAKE) CONFIG=extras check

clean:
	rm -rf build libslopsix.a slopsix_test
