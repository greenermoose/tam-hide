CC ?= gcc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX ?= $(HOME)/.local
VERSION := $(shell tr -d '[:space:]' < VERSION)

CFLAGS ?= -std=c17 -D_GNU_SOURCE -Wall -Wextra -Werror -O2 -g
CFLAGS += -DTAM_HIDE_VERSION=\"$(VERSION)\"
LDFLAGS ?=

WL_PKGS := wayland-client xkbcommon cairo pangocairo
WL_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(WL_PKGS))
WL_LIBS := $(shell $(PKG_CONFIG) --libs $(WL_PKGS)) -lm
ARGON2_CFLAGS := $(shell $(PKG_CONFIG) --cflags libargon2)
ARGON2_LIBS := $(shell $(PKG_CONFIG) --libs libargon2)
PAM_LIBS := -lpam

# libtam (sockets, atomic writes, clock), linked statically. 0.14.0 or later:
# its socket line reader wipes request bytes, which may be passcodes.
export PKG_CONFIG_PATH := $(HOME)/.local/lib/pkgconfig$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))
LIBTAM_OK := $(shell $(PKG_CONFIG) --atleast-version=0.14.0 libtam 2>/dev/null && echo yes)
ifeq ($(LIBTAM_OK),)
$(error tam-hide needs libtam 0.14.0 or later; install it or add its pkg-config directory to PKG_CONFIG_PATH)
endif
LIBTAM_CFLAGS := $(shell $(PKG_CONFIG) --cflags libtam)
LIBTAM_LIBS := $(shell $(PKG_CONFIG) --libs libtam)
LIBTAM_ARCHIVE := $(wildcard $(shell $(PKG_CONFIG) --variable=libdir libtam)/libtam.a)
CFLAGS += $(LIBTAM_CFLAGS)
LDFLAGS += $(LIBTAM_LIBS)

WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
SESSION_LOCK_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/ext-session-lock/ext-session-lock-v1.xml
IDLE_NOTIFY_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/ext-idle-notify/ext-idle-notify-v1.xml
OUTPUT_POWER_XML := protocols/wlr-output-power-management-unstable-v1.xml

BUILD := build
PROTO_DIR := $(BUILD)/protocols
SESSION_LOCK_H := $(PROTO_DIR)/ext-session-lock-v1-client-protocol.h
SESSION_LOCK_C := $(PROTO_DIR)/ext-session-lock-v1-protocol.c
IDLE_NOTIFY_H := $(PROTO_DIR)/ext-idle-notify-v1-client-protocol.h
IDLE_NOTIFY_C := $(PROTO_DIR)/ext-idle-notify-v1-protocol.c
OUTPUT_POWER_H := $(PROTO_DIR)/wlr-output-power-management-unstable-v1-client-protocol.h
OUTPUT_POWER_C := $(PROTO_DIR)/wlr-output-power-management-unstable-v1-protocol.c

PROTO_HDRS := $(SESSION_LOCK_H) $(IDLE_NOTIFY_H) $(OUTPUT_POWER_H)
PROTO_SRCS := $(SESSION_LOCK_C) $(IDLE_NOTIFY_C) $(OUTPUT_POWER_C)

LOCK_CORE_SRCS := src/wl_lock.c src/render.c src/lock_state.c src/power_policy.c src/auth.c src/auth_controller.c $(PROTO_SRCS)
LOCK_CORE_HDRS := src/wl_lock.h src/render.h src/lock_state.h src/power_policy.h src/auth.h src/auth_controller.h $(PROTO_HDRS)

.PHONY: all test test-headless install clean

all: $(BUILD)/tam-hide $(BUILD)/tam-hided $(BUILD)/tam-hidectl

$(BUILD) $(PROTO_DIR):
	mkdir -p $@

$(SESSION_LOCK_H): $(SESSION_LOCK_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(SESSION_LOCK_C): $(SESSION_LOCK_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(IDLE_NOTIFY_H): $(IDLE_NOTIFY_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(IDLE_NOTIFY_C): $(IDLE_NOTIFY_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(OUTPUT_POWER_H): $(OUTPUT_POWER_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(OUTPUT_POWER_C): $(OUTPUT_POWER_XML) | $(PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD)/tam-hide: $(LIBTAM_ARCHIVE) src/tam-hide.c src/auth_pam.c $(LOCK_CORE_SRCS) $(LOCK_CORE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(PROTO_DIR) $(WL_CFLAGS) -o $@ \
		src/tam-hide.c src/auth_pam.c $(LOCK_CORE_SRCS) $(LDFLAGS) $(WL_LIBS) $(PAM_LIBS)

$(BUILD)/tam-hided: $(LIBTAM_ARCHIVE) src/tam-hided.c src/controller.c src/vt.c src/passcode_store.c src/controller.h src/vt.h src/passcode_store.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(ARGON2_CFLAGS) -o $@ \
		src/tam-hided.c src/controller.c src/vt.c src/passcode_store.c $(LDFLAGS) $(ARGON2_LIBS)

$(BUILD)/tam-hidectl: $(LIBTAM_ARCHIVE) src/tam-hidectl.c src/passcode_store.c src/passcode_store.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(ARGON2_CFLAGS) -o $@ \
		src/tam-hidectl.c src/passcode_store.c $(LDFLAGS) $(ARGON2_LIBS)

$(BUILD)/test_passcode_store: tests/test_passcode_store.c src/passcode_store.c src/passcode_store.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(ARGON2_CFLAGS) -o $@ \
		tests/test_passcode_store.c src/passcode_store.c $(LDFLAGS) $(ARGON2_LIBS)

$(BUILD)/test_controller: tests/test_controller.c src/controller.c src/vt.c src/passcode_store.c src/auth_controller.c src/controller.h src/vt.h src/passcode_store.h src/auth_controller.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(ARGON2_CFLAGS) -o $@ \
		tests/test_controller.c src/controller.c src/vt.c src/passcode_store.c src/auth_controller.c $(LDFLAGS) $(ARGON2_LIBS)

$(BUILD)/test_lock_state: tests/test_lock_state.c src/lock_state.c src/lock_state.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_lock_state.c src/lock_state.c $(LDFLAGS)

$(BUILD)/test_power_policy: tests/test_power_policy.c src/power_policy.c src/power_policy.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_power_policy.c src/power_policy.c $(LDFLAGS)

$(BUILD)/test_auth: tests/test_auth.c src/auth.c src/auth.h src/lock_state.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_auth.c src/auth.c $(LDFLAGS)

$(BUILD)/test_wayland_lock: tests/test_wayland_lock.c $(LOCK_CORE_SRCS) $(LOCK_CORE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(PROTO_DIR) $(WL_CFLAGS) -o $@ \
		tests/test_wayland_lock.c $(LOCK_CORE_SRCS) $(LDFLAGS) $(WL_LIBS)

test: $(BUILD)/test_passcode_store $(BUILD)/test_controller $(BUILD)/test_lock_state $(BUILD)/test_power_policy $(BUILD)/test_auth $(BUILD)/test_wayland_lock
	./$(BUILD)/test_passcode_store
	./$(BUILD)/test_controller
	./$(BUILD)/test_lock_state
	./$(BUILD)/test_power_policy
	./$(BUILD)/test_auth

test-headless: $(BUILD)/test_wayland_lock $(BUILD)/tam-hide
	./tests/test_tam_pane_headless.sh

install: $(BUILD)/tam-hide
	install -Dm755 $(BUILD)/tam-hide $(DESTDIR)$(PREFIX)/bin/tam-hide

clean:
	rm -rf $(BUILD)
