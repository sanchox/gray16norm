CC          ?= gcc
PKG_CONFIG  ?= pkg-config

# Plugin name (output .so)
TARGET      ?= libgstgray16norm.so
SRC         := gstgray16norm.c gstgray16window.c gstgray16color.c gstgray16plugin.c
OBJ         := $(SRC:.c=.o)

# Mandatory LUT headers (generated at build time)
LUT_HEADERS := \
  gray16_to_rgb_lut.h \
  gray16_to_rgb_lut_viridis.h \
  gray16_to_rgb_lut_magma.h \
  gray16_to_rgb_lut_jet.h \
  gray16_to_rgb_lut_prism.h

# Dependencies
GST_DEPS    ?= gstreamer-1.0 gstreamer-video-1.0

# Flags
CSTD        ?= -std=gnu11
WARN        ?= -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith -Wcast-qual -Wno-unused-parameter
OPT         ?= -O2
SANITIZERS  ?=

GST_CFLAGS  ?= $(shell $(PKG_CONFIG) --cflags $(GST_DEPS))

# Base CFLAGS can be overridden by environments, but we always append
# pkg-config derived includes to avoid losing required headers.
CFLAGS      ?= $(CSTD) $(WARN) $(OPT) -fPIC
CFLAGS      += $(GST_CFLAGS)
CFLAGS      += $(SANITIZERS)
LDFLAGS     ?=
LDLIBS      ?= $(shell $(PKG_CONFIG) --libs $(GST_DEPS)) $(SANITIZERS)

# Optional ARM tuning (safe defaults; can be overridden by environment)
ARCH        ?= $(shell uname -m)
ifeq ($(ARCH),aarch64)
# On aarch64 (ARMv8-A) NEON is baseline; enable reasonable tuning
CFLAGS      += -march=armv8-a+simd -mtune=cortex-a53
endif

# Install path (typical user-local)
INSTALL_DIR ?= $(HOME)/.local/lib/gstreamer-1.0

.PHONY: all clean install uninstall debug \
        test test-smoke test-functional test-all \
        format check-format lint style \
        generate-luts

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Ensure LUTs are present before compiling elements that use them
gstgray16norm.o: $(LUT_HEADERS)
gstgray16color.o: $(LUT_HEADERS)

# Generate all LUT headers when any is missing or older than lut_gen.py
$(LUT_HEADERS): lut_gen.py
	@echo "Generating LUT headers (turbo, viridis, magma, jet, prism)..."
	@python3 lut_gen.py --all

install: $(TARGET)
	@echo "Installing to $(INSTALL_DIR)..."
	install -d "$(INSTALL_DIR)"
	install -m 0644 "$(TARGET)" "$(INSTALL_DIR)/"
	@echo "Done. You may need: export GST_PLUGIN_PATH=\"$(INSTALL_DIR)\""

uninstall:
	$(RM) -f "$(INSTALL_DIR)/$(TARGET)"

clean:
	$(RM) -f $(OBJ) $(TARGET) $(LUT_HEADERS) \
		gstgray16rgb.o

# Quick build with sanitizers
debug:
	$(MAKE) clean
	$(MAKE) OPT=-O0 SANITIZERS="-fsanitize=address,undefined" CFLAGS+=" -g"

# Simple smoke test (requires gst-inspect-1.0)
TESTS_DIR := tests

# Test entry points
test: test-smoke

test-smoke:
	@bash "$(TESTS_DIR)/smoke.sh"

test-functional:
	@bash "$(TESTS_DIR)/functional.sh"

test-all: test-smoke test-functional

# Code style helpers
format:
	@if command -v clang-format >/dev/null; then \
	  clang-format -i $(SRC); \
	else \
	  echo "SKIP: clang-format not found"; \
	fi

check-format:
	@if command -v clang-format >/dev/null; then \
	  clang-format --dry-run -Werror $(SRC); \
	else \
	  echo "SKIP: clang-format not found"; \
	fi

lint:
	@if command -v clang-tidy >/dev/null; then \
	  clang-tidy $(SRC) -- $(CSTD) -fPIC $$($(PKG_CONFIG) --cflags $(GST_DEPS)); \
	else \
	  echo "SKIP: clang-tidy not found"; \
	fi

style: check-format lint

# Generate LUT headers (requires python3, numpy, matplotlib)
generate-luts: $(LUT_HEADERS)
