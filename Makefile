CC          ?= gcc
PKG_CONFIG  ?= pkg-config

# Plugin name (output .so)
TARGET      ?= libgstgray16norm.so
SRC         := gstgray16norm.c
OBJ         := $(SRC:.c=.o)

# Dependencies
GST_DEPS    ?= gstreamer-1.0 gstreamer-video-1.0

# Flags
CSTD        ?= -std=gnu11
WARN        ?= -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith -Wcast-qual -Wno-unused-parameter
OPT         ?= -O2
SANITIZERS  ?=

CFLAGS      ?= $(CSTD) $(WARN) $(OPT) -fPIC $(shell $(PKG_CONFIG) --cflags $(GST_DEPS))
CFLAGS      += $(SANITIZERS)
LDFLAGS     ?=
LDLIBS      ?= $(shell $(PKG_CONFIG) --libs $(GST_DEPS)) $(SANITIZERS)

# Install path (typical user-local)
INSTALL_DIR ?= $(HOME)/.local/lib/gstreamer-1.0

.PHONY: all clean install uninstall debug test-smoke

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	@echo "Installing to $(INSTALL_DIR)..."
	install -d "$(INSTALL_DIR)"
	install -m 0644 "$(TARGET)" "$(INSTALL_DIR)/"
	@echo "Done. You may need: export GST_PLUGIN_PATH=\"$(INSTALL_DIR):$$GST_PLUGIN_PATH\""

uninstall:
	$(RM) -f "$(INSTALL_DIR)/$(TARGET)"

clean:
	$(RM) -f $(OBJ) $(TARGET)

# Quick build with sanitizers
debug:
	$(MAKE) clean
	$(MAKE) OPT=-O0 SANITIZERS="-fsanitize=address,undefined" CFLAGS+=" -g"

# Simple smoke test (requires gst-inspect-1.0)
test-smoke: all
	@export GST_PLUGIN_PATH="$(PWD):$$GST_PLUGIN_PATH"; \
	if command -v gst-inspect-1.0 >/dev/null; then \
	  gst-inspect-1.0 gray16norm; \
	else \
	  echo "SKIP: gst-inspect-1.0 missing"; \
	fi
