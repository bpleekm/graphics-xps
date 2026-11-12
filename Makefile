.DEFAULT_GOAL := all

CC = cc
HOSTCC = cc
BUILD_DIR ?= build
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude -I$(BUILD_DIR)
PATH_MAP_FLAGS ?= "-ffile-prefix-map=$(CURDIR)=."
CPPFLAGS += $(PATH_MAP_FLAGS)
CFLAGS ?= -O3
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS += -lm
BACKEND ?= opencl
UNAME_S := $(shell uname -s)

ifeq ($(BACKEND),opencl)
ifeq ($(UNAME_S),Darwin)
OPENCL_LIBS := -framework OpenCL
else
OPENCL_CFLAGS := $(shell pkg-config --cflags OpenCL 2>/dev/null)
OPENCL_LIBS := $(shell pkg-config --libs OpenCL 2>/dev/null || printf '%s' '-lOpenCL')
endif
BACKEND_SOURCES := src/backend_opencl.c
else
$(error Backend '$(BACKEND)' is not implemented; available: opencl)
endif

CORE_SOURCES := src/config.c src/model.c
SOURCES := src/main.c src/backend.c src/runner.c src/report.c src/os.c $(CORE_SOURCES) $(BACKEND_SOURCES)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
BINARY := $(BUILD_DIR)/graphics-xps

MEMORY ?= 256M
SECONDS ?= 10
ARGS ?=

.PHONY: all check smoke probe bench report-check privacy-check privacy-history clean help
all: $(BINARY)

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/embed: tools/embed.c | $(BUILD_DIR)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Wpedantic $< -o $@

$(BUILD_DIR)/gemm_source.h: kernels/gemm.cl $(BUILD_DIR)/embed
	$(BUILD_DIR)/embed $< $@

$(BUILD_DIR)/backend_opencl.o: $(BUILD_DIR)/gemm_source.h

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(OPENCL_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BINARY): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(OPENCL_LIBS) $(LDLIBS) -o $@

$(BUILD_DIR)/test-core: tests/test_core.c $(CORE_SOURCES) src/report.c src/os.c include/gxps.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_core.c $(CORE_SOURCES) src/report.c src/os.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(BUILD_DIR)/test-core
	$(BUILD_DIR)/test-core

probe: $(BINARY)
	$(BINARY) probe -pauto -m$(MEMORY) $(ARGS)

smoke: $(BINARY)
	$(BINARY) bench -pauto -n37 -m16M -t0.05 -w0 $(ARGS)

bench: $(BINARY)
	$(BINARY) bench -pauto -m$(MEMORY) -t$(SECONDS) $(ARGS)

report-check:
	python3 tools/check_report.py $(REPORT)

privacy-check:
	python3 -m unittest discover -s tests -p test_privacy.py
	python3 tools/check_privacy.py

privacy-history:
	python3 tools/check_privacy.py --history

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(BINARY) $(BUILD_DIR)/embed $(BUILD_DIR)/gemm_source.h $(BUILD_DIR)/test-core

help:
	@printf '%s\n' 'make [all]          Build only; no GPU test' \
	  'make check          CPU-only parser/budget/numeric tests' \
	  'make probe          Capability + small-matrix GPU checks' \
	  'make smoke          Serial N=37 tail/correctness GPU tests' \
	  'make privacy-check  Privacy regression tests + publishable working files' \
	  'make privacy-history  Also inspect reachable Git history and identities' \
	  'make bench MEMORY=1G SECONDS=10 ARGS="-o results/run.json"' \
	  'make clean          Remove this build directory artifacts' \
	  'CC=clang BUILD_DIR=build/clang are supported overrides'

-include $(OBJECTS:.o=.d)
