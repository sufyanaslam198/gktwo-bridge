# gkhook build. Run from anywhere: make -C gkhook host | test | arm
HOSTCC ?= cc
ARMCC  ?= arm-linux-gnueabihf-gcc
ARMSTRIP ?= arm-linux-gnueabihf-strip
WARN    = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror
SOFLAGS = -fPIC -shared -fvisibility=hidden -pthread
ARMOPT  = -march=armv7-a -mfpu=neon -mfloat-abi=hard -O2
SRC     = $(sort $(wildcard src/*.c))
HDR     = $(sort $(wildcard src/*.h))

.PHONY: host arm test clean
host: build/libgkhook_host.so build/test_load build/test_fingerprint build/test_proto build/test_queue build/test_server build/test_validate build/test_tick build/test_zjog
arm: build/libgkhook.so

build:
	mkdir -p build

build/libgkhook_host.so: $(SRC) $(HDR) | build
	$(HOSTCC) $(WARN) -O2 $(SOFLAGS) -o $@ $(SRC)

build/test_load: tests/test_load.c | build
	$(HOSTCC) $(WARN) -O2 -o $@ $< -ldl

build/test_fingerprint: tests/test_fingerprint.c src/fingerprint.c src/fingerprint.h | build
	$(HOSTCC) $(WARN) -O2 -o $@ tests/test_fingerprint.c src/fingerprint.c

build/test_proto: tests/test_proto.c src/proto.c $(HDR) | build
	$(HOSTCC) $(WARN) -O2 -o $@ tests/test_proto.c src/proto.c

build/test_queue: tests/test_queue.c src/queue.c $(HDR) | build
	$(HOSTCC) $(WARN) -O2 -pthread -o $@ tests/test_queue.c src/queue.c

build/test_server: tests/test_server.c src/proto.c src/queue.c src/server.c ../gksdcp/src/hook_io.c $(HDR) | build
	$(HOSTCC) $(WARN) -O2 -pthread -o $@ tests/test_server.c src/proto.c src/queue.c src/server.c ../gksdcp/src/hook_io.c

# Mock-chitu tests: tick.c/validate.c built with GK_ABI_INDIRECT (the single test seam in chitu_abi.h).
INDIRECT = -DGK_ABI_INDIRECT

build/test_validate: tests/test_validate.c tests/mock_chitu.c tests/mock_chitu.h src/validate.c $(HDR) | build
	$(HOSTCC) $(WARN) $(INDIRECT) -O2 -o $@ tests/test_validate.c tests/mock_chitu.c src/validate.c

build/test_tick: tests/test_tick.c tests/mock_chitu.c tests/mock_chitu.h src/tick.c src/validate.c src/queue.c src/zjog.c $(HDR) | build
	$(HOSTCC) $(WARN) $(INDIRECT) -O2 -pthread -Wl,--wrap=gkq_end_start -Wl,--wrap=gkq_publish_status -o $@ tests/test_tick.c tests/mock_chitu.c src/tick.c src/validate.c src/queue.c src/zjog.c -lm

build/test_zjog: tests/test_zjog.c tests/mock_chitu.c tests/mock_chitu.h src/zjog.c src/proto.c $(HDR) | build
	$(HOSTCC) $(WARN) $(INDIRECT) -O2 -o $@ tests/test_zjog.c tests/mock_chitu.c src/zjog.c src/proto.c -lm

# -DNDEBUG: no assert (abort) may live inside chitu.
build/libgkhook.so: $(SRC) $(HDR) | build
	$(ARMCC) $(WARN) -DNDEBUG $(ARMOPT) $(SOFLAGS) -o $@ $(SRC)
	$(ARMSTRIP) $@

test: host
	build/test_load build/libgkhook_host.so
	build/test_fingerprint
	build/test_proto
	build/test_queue
	build/test_server
	build/test_validate
	build/test_tick
	build/test_zjog

clean:
	rm -rf build
