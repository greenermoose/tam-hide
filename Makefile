CC ?= gcc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX ?= /usr/local
VERSION := $(shell tr -d '[:space:]' < VERSION)

CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -O2 -g
CFLAGS += -DTAM_HIDE_VERSION=\"$(VERSION)\"
LDFLAGS ?=

WL_PKGS := wayland-client xkbcommon cairo pangocairo
WL_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(WL_PKGS))
WL_LIBS := $(shell $(PKG_CONFIG) --libs $(WL_PKGS)) -lm
PAM_LIBS := -lpam

WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
SESSION_LOCK_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/ext-session-lock/ext-session-lock-v1.xml

BUILD := build
PROTO_DIR := $(BUILD)/protocols
PROTO_H := $(PROTO_DIR)/ext-session-lock-v1-client-protocol.h
PROTO_C := $(PROTO_DIR)/ext-session-lock-v1-protocol.c

LOCK_CORE_SRCS := src/wl_lock.c src/render.c src/lock_state.c src/auth.c $(PROTO_C)
LOCK_CORE_HDRS := src/wl_lock.h src/render.h src/lock_state.h src/auth.h $(PROTO_H)

.PHONY: all test test-headless install clean

all: $(BUILD)/tam-hide $(BUILD)/tam-hided $(BUILD)/tam-hidectl

$(BUILD) $(PROTO_DIR):
	mkdir -p $@

$(PROTO_H): $(SESSION_LOCK_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_C): $(SESSION_LOCK_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD)/tam-hide: src/tam-hide.c src/auth_pam.c $(LOCK_CORE_SRCS) $(LOCK_CORE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(PROTO_DIR) $(WL_CFLAGS) -o $@ \
		src/tam-hide.c src/auth_pam.c $(LOCK_CORE_SRCS) $(LDFLAGS) $(WL_LIBS) $(PAM_LIBS)

$(BUILD)/tam-hided: src/tam-hided.c src/controller.c src/vt.c src/controller.h src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/tam-hided.c src/controller.c src/vt.c $(LDFLAGS)

$(BUILD)/tam-hidectl: src/tam-hidectl.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/tam-hidectl.c $(LDFLAGS)

$(BUILD)/test_controller: tests/test_controller.c src/controller.c src/vt.c src/controller.h src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_controller.c src/controller.c src/vt.c $(LDFLAGS)

$(BUILD)/test_lock_state: tests/test_lock_state.c src/lock_state.c src/lock_state.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_lock_state.c src/lock_state.c $(LDFLAGS)

$(BUILD)/test_auth: tests/test_auth.c src/auth.c src/auth.h src/lock_state.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_auth.c src/auth.c $(LDFLAGS)

$(BUILD)/test_wayland_lock: tests/test_wayland_lock.c $(LOCK_CORE_SRCS) $(LOCK_CORE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(PROTO_DIR) $(WL_CFLAGS) -o $@ \
		tests/test_wayland_lock.c $(LOCK_CORE_SRCS) $(LDFLAGS) $(WL_LIBS)

test: $(BUILD)/test_controller $(BUILD)/test_lock_state $(BUILD)/test_auth $(BUILD)/test_wayland_lock
	./$(BUILD)/test_controller
	./$(BUILD)/test_lock_state
	./$(BUILD)/test_auth

test-headless: $(BUILD)/test_wayland_lock
	./tests/test_tam_pane_headless.sh

install: $(BUILD)/tam-hide
	install -Dm755 $(BUILD)/tam-hide $(DESTDIR)$(PREFIX)/bin/tam-hide

clean:
	rm -rf $(BUILD)
