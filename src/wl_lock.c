/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wl_lock.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "lock_state.h"
#include "power_policy.h"
#include "render.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

struct wl_lock_ctx;

struct wl_lock_output {
	struct wl_lock_output *next;
	struct wl_lock_ctx *ctx;
	uint32_t registry_name;
	struct wl_output *wl_output;
	int32_t scale;
	struct wl_surface *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct zwlr_output_power_v1 *power;
	bool power_known;
	uint32_t power_mode;
	uint32_t width;
	uint32_t height;
	bool configured;
};

struct wl_lock_ctx {
	struct wl_lock_config cfg;
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct ext_session_lock_manager_v1 *lock_mgr;
	struct ext_session_lock_v1 *lock;
	struct ext_idle_notifier_v1 *idle_notifier;
	uint32_t idle_notifier_ver;
	struct ext_idle_notification_v1 *idle_notif;
	struct zwlr_output_power_manager_v1 *output_power_mgr;

	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	struct wl_lock_output *outputs;
	struct lock_state state;
	struct power_policy power_policy;
	struct auth_job auth;

	bool locked;
	bool unlocked;
	bool done;
	int exit_code;
};

static void emit_event(struct wl_lock_ctx *ctx, const char *line) {
	if (!ctx || ctx->cfg.event_fd < 0 || !line)
		return;
	(void)write(ctx->cfg.event_fd, line, strlen(line));
}

static void output_power_mode(void *data, struct zwlr_output_power_v1 *power,
							  uint32_t mode) {
	(void)power;
	struct wl_lock_output *out = data;
	out->power_known = true;
	out->power_mode = mode;
	emit_event(out->ctx, (mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF)
							 ? "POWER_OFF\n"
							 : "POWER_ON\n");
}

static void output_power_failed(void *data, struct zwlr_output_power_v1 *power) {
	struct wl_lock_output *out = data;
	zwlr_output_power_v1_destroy(power);
	if (out->power == power)
		out->power = NULL;
	out->power_known = false;
}

static const struct zwlr_output_power_v1_listener output_power_listener = {
	.mode = output_power_mode,
	.failed = output_power_failed,
};

static void attach_output_power(struct wl_lock_ctx *ctx,
								struct wl_lock_output *out) {
	if (!ctx->output_power_mgr || !out->wl_output || out->power)
		return;
	if (!ctx->power_policy.enabled)
		return;
	out->power = zwlr_output_power_manager_v1_get_output_power(
		ctx->output_power_mgr, out->wl_output);
	if (!out->power)
		return;
	zwlr_output_power_v1_add_listener(out->power, &output_power_listener, out);
}

static void set_outputs_power(struct wl_lock_ctx *ctx, bool on) {
	uint32_t mode = on ? ZWLR_OUTPUT_POWER_V1_MODE_ON
					   : ZWLR_OUTPUT_POWER_V1_MODE_OFF;
	for (struct wl_lock_output *o = ctx->outputs; o; o = o->next) {
		if (o->power)
			zwlr_output_power_v1_set_mode(o->power, mode);
	}
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
	(void)data;
	wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static struct wl_buffer *render_shm_buffer(struct wl_lock_ctx *ctx,
										   uint32_t logical_w,
										   uint32_t logical_h,
										   int32_t scale) {
	if (!ctx->shm || logical_w == 0 || logical_h == 0)
		return NULL;
	if (scale < 1)
		scale = 1;

	uint64_t phys_w = (uint64_t)logical_w * (uint64_t)scale;
	uint64_t phys_h = (uint64_t)logical_h * (uint64_t)scale;
	if (phys_w > 16384 || phys_h > 16384)
		return NULL;

	size_t stride = (size_t)phys_w * 4;
	size_t size = stride * (size_t)phys_h;

	int fd = memfd_create("tam-hide-shm", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return NULL;
	}

	uint32_t *pixels =
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	lock_render_frame(pixels, logical_w, logical_h, scale, &ctx->state,
					  lock_monotonic_ms(), ctx->power_policy.blanked);
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, (int32_t)size);
	close(fd);
	if (!pool)
		return NULL;

	struct wl_buffer *buf = wl_shm_pool_create_buffer(
		pool, 0, (int32_t)phys_w, (int32_t)phys_h, (int32_t)stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	if (!buf)
		return NULL;

	wl_buffer_add_listener(buf, &buffer_listener, NULL);
	return buf;
}

static void render_output(struct wl_lock_output *out) {
	if (!out || !out->configured || !out->surface || out->width == 0 ||
		out->height == 0)
		return;
	struct wl_lock_ctx *ctx = out->ctx;
	struct wl_buffer *buf =
		render_shm_buffer(ctx, out->width, out->height, out->scale);
	if (!buf)
		return;

	wl_surface_set_buffer_scale(out->surface, out->scale);
	wl_surface_attach(out->surface, buf, 0, 0);
	wl_surface_damage_buffer(out->surface, 0, 0,
							 (int32_t)(out->width * (uint32_t)out->scale),
							 (int32_t)(out->height * (uint32_t)out->scale));
	wl_surface_commit(out->surface);
}

static void render_all_outputs(struct wl_lock_ctx *ctx) {
	for (struct wl_lock_output *o = ctx->outputs; o; o = o->next)
		render_output(o);
}

static void lock_surface_configure(void *data,
								   struct ext_session_lock_surface_v1 *ls,
								   uint32_t serial, uint32_t width,
								   uint32_t height) {
	struct wl_lock_output *out = data;
	ext_session_lock_surface_v1_ack_configure(ls, serial);
	out->width = width;
	out->height = height;
	out->configured = true;
	render_output(out);
}

static const struct ext_session_lock_surface_v1_listener
	lock_surface_listener = {
		.configure = lock_surface_configure,
};

static void create_lock_surface_for_output(struct wl_lock_ctx *ctx,
										   struct wl_lock_output *out) {
	if (!ctx->lock || !ctx->compositor || !out->wl_output || out->lock_surface)
		return;
	out->surface = wl_compositor_create_surface(ctx->compositor);
	if (!out->surface)
		return;
	out->lock_surface = ext_session_lock_v1_get_lock_surface(
		ctx->lock, out->surface, out->wl_output);
	if (!out->lock_surface) {
		wl_surface_destroy(out->surface);
		out->surface = NULL;
		return;
	}
	ext_session_lock_surface_v1_add_listener(out->lock_surface,
											 &lock_surface_listener, out);
}

static void destroy_output(struct wl_lock_output *out) {
	if (!out)
		return;
	if (out->power)
		zwlr_output_power_v1_destroy(out->power);
	if (out->lock_surface)
		ext_session_lock_surface_v1_destroy(out->lock_surface);
	if (out->surface)
		wl_surface_destroy(out->surface);
	if (out->wl_output)
		wl_output_destroy(out->wl_output);
	free(out);
}

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
							int32_t pw, int32_t ph, int32_t sub, const char *mk,
							const char *md, int32_t tr) {
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph;
	(void)sub; (void)mk; (void)md; (void)tr;
}
static void output_mode(void *d, struct wl_output *o, uint32_t fl, int32_t w,
						int32_t h, int32_t r) {
	(void)d; (void)o; (void)fl; (void)w; (void)h; (void)r;
}
static void output_done(void *data, struct wl_output *o) {
	(void)o;
	struct wl_lock_output *out = data;
	if (out->configured)
		render_output(out);
}
static void output_scale(void *data, struct wl_output *o, int32_t factor) {
	(void)o;
	struct wl_lock_output *out = data;
	if (factor < 1)
		factor = 1;
	out->scale = factor;
}
static void output_name(void *d, struct wl_output *o, const char *n) {
	(void)d; (void)o; (void)n;
}
static void output_desc(void *d, struct wl_output *o, const char *desc) {
	(void)d; (void)o; (void)desc;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_desc,
};

static void submit_password(struct wl_lock_ctx *ctx, int64_t now_ms) {
	if (!lock_state_can_submit(&ctx->state, now_ms))
		return;
	char pass[TAM_HIDE_MAX_PASS_BYTES + 1];
	size_t plen = 0;
	if (!lock_state_begin_verify(&ctx->state, pass, sizeof(pass), &plen))
		return;

	int rc = auth_start(&ctx->auth, ctx->cfg.verify_fn, ctx->cfg.verify_ctx,
						pass, plen);
	explicit_bzero(pass, sizeof(pass));
	if (rc < 0) {
		lock_state_on_auth_result(&ctx->state, false, now_ms);
	}
	render_all_outputs(ctx);
}

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
					  int32_t fd, uint32_t size) {
	(void)kb;
	struct wl_lock_ctx *ctx = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
		close(fd);
		return;
	}

	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;

	struct xkb_keymap *km = xkb_keymap_new_from_string(
		ctx->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!km)
		return;

	struct xkb_state *st = xkb_state_new(km);
	if (!st) {
		xkb_keymap_unref(km);
		return;
	}

	if (ctx->xkb_state)
		xkb_state_unref(ctx->xkb_state);
	if (ctx->xkb_keymap)
		xkb_keymap_unref(ctx->xkb_keymap);
	ctx->xkb_keymap = km;
	ctx->xkb_state = st;
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s,
					 struct wl_surface *surf, struct wl_array *keys) {
	(void)d; (void)k; (void)s; (void)surf; (void)keys;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s,
					 struct wl_surface *surf) {
	(void)d; (void)k; (void)s; (void)surf;
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
				   uint32_t time, uint32_t key, uint32_t state) {
	(void)kb; (void)serial; (void)time;
	struct wl_lock_ctx *ctx = data;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !ctx->xkb_state)
		return;

	if (power_policy_on_activity(&ctx->power_policy)) {
		set_outputs_power(ctx, true);
		render_all_outputs(ctx);
		emit_event(ctx, "WOKEN\n");
	}

	int64_t now_ms = lock_monotonic_ms();
	uint32_t keycode = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(ctx->xkb_state, keycode);

	bool ctrl = xkb_state_mod_name_is_active(
		ctx->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
	bool alt = xkb_state_mod_name_is_active(
		ctx->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
	bool logo = xkb_state_mod_name_is_active(
		ctx->xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;

	if (sym == XKB_KEY_Escape || (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U))) {
		if (lock_state_clear(&ctx->state, now_ms))
			render_all_outputs(ctx);
		return;
	}

	if (sym == XKB_KEY_BackSpace) {
		if (lock_state_backspace(&ctx->state, now_ms))
			render_all_outputs(ctx);
		return;
	}

	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		submit_password(ctx, now_ms);
		return;
	}

	if (ctrl || alt || logo)
		return;

	char utf8[32];
	int n = xkb_state_key_get_utf8(ctx->xkb_state, keycode, utf8, sizeof(utf8));
	if (n > 0) {
		if (lock_state_append_utf8(&ctx->state, utf8, (size_t)n, now_ms))
			render_all_outputs(ctx);
		explicit_bzero(utf8, sizeof(utf8));
	}
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
						 uint32_t depressed, uint32_t latched, uint32_t locked,
						 uint32_t group) {
	(void)kb; (void)serial;
	struct wl_lock_ctx *ctx = data;
	if (ctx->xkb_state) {
		xkb_state_update_mask(ctx->xkb_state, depressed, latched, locked, 0, 0,
							  group);
	}
}

static void kb_repeat_info(void *d, struct wl_keyboard *k, int32_t r,
						   int32_t del) {
	(void)d; (void)k; (void)r; (void)del;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
	struct wl_lock_ctx *ctx = data;
	bool has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
	if (has_kb && !ctx->keyboard) {
		ctx->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(ctx->keyboard, &keyboard_listener, ctx);
	} else if (!has_kb && ctx->keyboard) {
		wl_keyboard_destroy(ctx->keyboard);
		ctx->keyboard = NULL;
	}
}

static void seat_name(void *d, struct wl_seat *s, const char *n) {
	(void)d; (void)s; (void)n;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

static void idle_idled(void *data, struct ext_idle_notification_v1 *notif) {
	(void)notif;
	struct wl_lock_ctx *ctx = data;
	if (ctx->unlocked || ctx->done)
		return;
	bool verifying = (ctx->state.ui_state == LOCK_UI_VERIFYING);
	if (!power_policy_on_idle(&ctx->power_policy, verifying))
		return;
	int64_t now_ms = lock_monotonic_ms();
	(void)lock_state_clear(&ctx->state, now_ms);
	render_all_outputs(ctx);
	set_outputs_power(ctx, false);
	emit_event(ctx, "BLANKED\n");
}

static void idle_resumed(void *data, struct ext_idle_notification_v1 *notif) {
	(void)notif;
	struct wl_lock_ctx *ctx = data;
	if (!power_policy_on_activity(&ctx->power_policy))
		return;
	set_outputs_power(ctx, true);
	render_all_outputs(ctx);
	emit_event(ctx, "WOKEN\n");
}

static const struct ext_idle_notification_v1_listener idle_notif_listener = {
	.idled = idle_idled,
	.resumed = idle_resumed,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {
	(void)lock;
	struct wl_lock_ctx *ctx = data;
	ctx->locked = true;
	if (power_policy_on_locked(&ctx->power_policy,
							   ctx->idle_notifier != NULL && ctx->seat != NULL,
							   ctx->output_power_mgr != NULL)) {
		for (struct wl_lock_output *o = ctx->outputs; o; o = o->next)
			attach_output_power(ctx, o);
		if (!ctx->idle_notif) {
			if (ctx->idle_notifier_ver >= 2) {
				ctx->idle_notif =
					ext_idle_notifier_v1_get_input_idle_notification(
						ctx->idle_notifier, ctx->cfg.blank_timeout_ms,
						ctx->seat);
			} else {
				ctx->idle_notif = ext_idle_notifier_v1_get_idle_notification(
					ctx->idle_notifier, ctx->cfg.blank_timeout_ms, ctx->seat);
			}
			if (ctx->idle_notif) {
				ext_idle_notification_v1_add_listener(
					ctx->idle_notif, &idle_notif_listener, ctx);
			}
		}
	}
	if (ctx->cfg.ready_fd >= 0) {
		const char msg[] = "LOCKED\n";
		(void)write(ctx->cfg.ready_fd, msg, sizeof(msg) - 1);
	}
}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock) {
	struct wl_lock_ctx *ctx = data;
	ext_session_lock_v1_destroy(lock);
	ctx->lock = NULL;
	ctx->done = true;
	ctx->exit_code = 1;
}

static const struct ext_session_lock_v1_listener session_lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
					   const char *iface, uint32_t ver) {
	struct wl_lock_ctx *ctx = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		uint32_t v = (ver < 4) ? ver : 4;
		ctx->compositor =
			wl_registry_bind(reg, name, &wl_compositor_interface, v);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		ctx->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && !ctx->seat) {
		uint32_t v = (ver < 5) ? ver : 5;
		ctx->seat = wl_registry_bind(reg, name, &wl_seat_interface, v);
		wl_seat_add_listener(ctx->seat, &seat_listener, ctx);
	} else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
		ctx->lock_mgr = wl_registry_bind(
			reg, name, &ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(iface, ext_idle_notifier_v1_interface.name) == 0 &&
			   !ctx->idle_notifier) {
		uint32_t v = (ver < 2) ? ver : 2;
		ctx->idle_notifier_ver = v;
		ctx->idle_notifier = wl_registry_bind(
			reg, name, &ext_idle_notifier_v1_interface, v);
	} else if (strcmp(iface, zwlr_output_power_manager_v1_interface.name) == 0 &&
			   !ctx->output_power_mgr) {
		ctx->output_power_mgr = wl_registry_bind(
			reg, name, &zwlr_output_power_manager_v1_interface, 1);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		struct wl_lock_output *out = calloc(1, sizeof(*out));
		if (!out)
			return;
		uint32_t v = (ver < 4) ? ver : 4;
		out->ctx = ctx;
		out->registry_name = name;
		out->scale = 1;
		out->wl_output = wl_registry_bind(reg, name, &wl_output_interface, v);
		wl_output_add_listener(out->wl_output, &output_listener, out);
		out->next = ctx->outputs;
		ctx->outputs = out;

		/* Hotplug while already locked: cover the new output immediately. */
		if (ctx->lock)
			create_lock_surface_for_output(ctx, out);
		if (ctx->locked && ctx->power_policy.enabled) {
			attach_output_power(ctx, out);
			if (out->power && ctx->power_policy.blanked)
				zwlr_output_power_v1_set_mode(out->power,
											  ZWLR_OUTPUT_POWER_V1_MODE_OFF);
		}
	}
}

static void reg_global_remove(void *data, struct wl_registry *reg,
							  uint32_t name) {
	(void)reg;
	struct wl_lock_ctx *ctx = data;
	struct wl_lock_output **pp = &ctx->outputs;
	while (*pp) {
		if ((*pp)->registry_name == name) {
			struct wl_lock_output *gone = *pp;
			*pp = gone->next;
			destroy_output(gone);
			return;
		}
		pp = &(*pp)->next;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = reg_global,
	.global_remove = reg_global_remove,
};

static void cleanup_ctx(struct wl_lock_ctx *ctx) {
	auth_cancel(&ctx->auth);
	lock_state_destroy(&ctx->state);

	while (ctx->outputs) {
		struct wl_lock_output *next = ctx->outputs->next;
		destroy_output(ctx->outputs);
		ctx->outputs = next;
	}

	if (ctx->idle_notif) {
		ext_idle_notification_v1_destroy(ctx->idle_notif);
		ctx->idle_notif = NULL;
	}
	if (ctx->idle_notifier)
		ext_idle_notifier_v1_destroy(ctx->idle_notifier);
	if (ctx->output_power_mgr)
		zwlr_output_power_manager_v1_destroy(ctx->output_power_mgr);
	if (ctx->lock) {
		/* Never unlock here: unlock only happens explicitly on AUTH_OK. */
		ext_session_lock_v1_destroy(ctx->lock);
		ctx->lock = NULL;
	}
	if (ctx->lock_mgr)
		ext_session_lock_manager_v1_destroy(ctx->lock_mgr);
	if (ctx->keyboard)
		wl_keyboard_destroy(ctx->keyboard);
	if (ctx->seat)
		wl_seat_destroy(ctx->seat);
	if (ctx->shm)
		wl_shm_destroy(ctx->shm);
	if (ctx->compositor)
		wl_compositor_destroy(ctx->compositor);
	if (ctx->registry)
		wl_registry_destroy(ctx->registry);
	if (ctx->xkb_state)
		xkb_state_unref(ctx->xkb_state);
	if (ctx->xkb_keymap)
		xkb_keymap_unref(ctx->xkb_keymap);
	if (ctx->xkb_ctx)
		xkb_context_unref(ctx->xkb_ctx);
	if (ctx->display)
		wl_display_disconnect(ctx->display);
}

int wl_lock_run(const struct wl_lock_config *cfg) {
	if (!cfg || !cfg->verify_fn)
		return 1;

	struct wl_lock_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg = *cfg;
	ctx.exit_code = 1;
	lock_state_init(&ctx.state);
	power_policy_init(&ctx.power_policy, cfg->blank_timeout_ms);
	auth_job_init(&ctx.auth);

	ctx.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ctx.xkb_ctx) {
		fprintf(stderr, "tam-hide: xkb_context_new failed\n");
		cleanup_ctx(&ctx);
		return 1;
	}

	ctx.display = wl_display_connect(NULL);
	if (!ctx.display) {
		fprintf(stderr, "tam-hide: cannot connect to Wayland display\n");
		cleanup_ctx(&ctx);
		return 1;
	}

	ctx.registry = wl_display_get_registry(ctx.display);
	wl_registry_add_listener(ctx.registry, &registry_listener, &ctx);
	wl_display_roundtrip(ctx.display);
	wl_display_roundtrip(ctx.display);

	if (!ctx.compositor || !ctx.shm || !ctx.lock_mgr) {
		fprintf(stderr,
				"tam-hide: compositor missing required globals "
				"(compositor=%d shm=%d ext_session_lock_manager_v1=%d)\n",
				ctx.compositor != NULL, ctx.shm != NULL, ctx.lock_mgr != NULL);
		cleanup_ctx(&ctx);
		return 1;
	}

	ctx.lock = ext_session_lock_manager_v1_lock(ctx.lock_mgr);
	if (!ctx.lock) {
		fprintf(stderr, "tam-hide: ext_session_lock_manager_v1_lock failed\n");
		cleanup_ctx(&ctx);
		return 1;
	}
	ext_session_lock_v1_add_listener(ctx.lock, &session_lock_listener, &ctx);

	for (struct wl_lock_output *o = ctx.outputs; o; o = o->next)
		create_lock_surface_for_output(&ctx, o);

	wl_display_flush(ctx.display);

	int wl_fd = wl_display_get_fd(ctx.display);

	while (!ctx.done) {
		if (ctx.cfg.stop_flag && *ctx.cfg.stop_flag) {
			/* Signal received: exit WITHOUT unlocking so compositor stays fail-locked. */
			ctx.exit_code = 1;
			break;
		}

		while (wl_display_prepare_read(ctx.display) != 0) {
			if (wl_display_dispatch_pending(ctx.display) < 0) {
				ctx.done = true;
				break;
			}
		}
		if (ctx.done)
			break;

		if (wl_display_flush(ctx.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(ctx.display);
			break;
		}

		struct pollfd fds[2];
		nfds_t nfds = 1;
		fds[0].fd = wl_fd;
		fds[0].events = POLLIN;
		fds[0].revents = 0;

		if (ctx.auth.result_fd >= 0) {
			fds[1].fd = ctx.auth.result_fd;
			fds[1].events = POLLIN;
			fds[1].revents = 0;
			nfds = 2;
		}

		int timeout_ms =
			(ctx.state.ui_state == LOCK_UI_FAILED ||
			 ctx.state.ui_state == LOCK_UI_LOCKOUT)
				? 200
				: 1000;

		int prc = poll(fds, nfds, timeout_ms);
		if (prc < 0) {
			wl_display_cancel_read(ctx.display);
			if (errno == EINTR)
				continue;
			break;
		}

		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(ctx.display) < 0)
				break;
			if (wl_display_dispatch_pending(ctx.display) < 0)
				break;
		} else {
			wl_display_cancel_read(ctx.display);
		}

		if (fds[0].revents & (POLLERR | POLLHUP))
			break;

		if (nfds == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
			enum auth_outcome res = auth_finish(&ctx.auth);
			int64_t now_ms = lock_monotonic_ms();
			if (res == AUTH_OK) {
				lock_state_on_auth_result(&ctx.state, true, now_ms);
				if (ctx.idle_notif) {
					ext_idle_notification_v1_destroy(ctx.idle_notif);
					ctx.idle_notif = NULL;
				}
				if (power_policy_on_unlock(&ctx.power_policy)) {
					set_outputs_power(&ctx, true);
					emit_event(&ctx, "WOKEN\n");
				}
				if (ctx.lock && ctx.locked) {
					ext_session_lock_v1_unlock_and_destroy(ctx.lock);
					ctx.lock = NULL;
					wl_display_roundtrip(ctx.display);
				}
				ctx.unlocked = true;
				ctx.exit_code = 0;
				ctx.done = true;
				break;
			}
			lock_state_on_auth_result(&ctx.state, false, now_ms);
			render_all_outputs(&ctx);
		}

		if (lock_state_tick(&ctx.state, lock_monotonic_ms()))
			render_all_outputs(&ctx);
	}

	int code = ctx.exit_code;
	cleanup_ctx(&ctx);
	return code;
}
