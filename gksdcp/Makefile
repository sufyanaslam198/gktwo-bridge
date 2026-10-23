# gksdcp build. Run from anywhere: make -C gksdcp host | test | arm
HOSTCC ?= cc
ARMCC  ?= arm-linux-gnueabihf-gcc
ARMSTRIP ?= arm-linux-gnueabihf-strip
WEBUI ?= 1
BUILD ?= build
MACHINE_NAME ?= GKtwo
BRAND_NAME ?= Uniformation
FW_VERSION ?= V1.3.4
# the project version: one line in the VERSION file at the repository root
VERSION_FILE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../VERSION)
BRIDGE_VERSION ?= $(strip $(shell cat $(VERSION_FILE) 2>/dev/null))
$(if $(BRIDGE_VERSION),,$(error no version in $(VERSION_FILE)))
XYZ_SIZE ?= 228.1x128.3x245
DEFS    = -DWEBUI=$(WEBUI) -DMACHINE_NAME='"$(MACHINE_NAME)"' -DBRAND_NAME='"$(BRAND_NAME)"' -DFW_VERSION='"$(FW_VERSION)"' -DBRIDGE_VERSION='"$(BRIDGE_VERSION)"' -DXYZ_SIZE='"$(XYZ_SIZE)"'
WARN    = -I$(BUILD) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread $(DEFS)
ARMOPT  = -march=armv7-a -mfpu=neon -mfloat-abi=hard -O2
SRC     = $(wildcard src/*.c)
LIBSRC  = $(filter-out src/main.c,$(SRC))
HDR     = $(wildcard src/*.h) $(if $(filter 1,$(WEBUI)),$(BUILD)/index_html.h) $(VERSION_FILE)
TESTS   = $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: host arm test clean
host: $(BUILD)/gksdcp-host $(TESTS)
arm: $(BUILD)/gksdcp

$(BUILD):
	mkdir -p $(BUILD)

# the web page embedded in the binary (WEBUI=1): www_index_html / www_index_html_len
$(BUILD)/index_html.h: www/index.html | $(BUILD)
	xxd -i www/index.html > $@

$(BUILD)/gksdcp-host: $(SRC) $(HDR) | $(BUILD)
	$(HOSTCC) $(WARN) -O2 -o $@ $(SRC) -lm

# every test links all of src/ except main.c
$(BUILD)/test_%: tests/test_%.c $(LIBSRC) $(HDR) | $(BUILD)
	$(HOSTCC) $(WARN) -O2 -Isrc -o $@ $< $(LIBSRC) -lm

$(BUILD)/gksdcp: $(SRC) $(HDR) | $(BUILD)
	$(ARMCC) $(WARN) -DNDEBUG $(ARMOPT) -o $@ $(SRC) -lm
	$(ARMSTRIP) $@

# run every test binary; stop at the first failure
test: host
	@set -e; for t in $(TESTS); do ./$$t; echo "OK $$t"; done

clean:
	rm -rf build build-noweb
