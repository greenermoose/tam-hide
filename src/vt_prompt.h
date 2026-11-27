/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_VT_PROMPT_H
#define TAM_HIDE_VT_PROMPT_H

#include "controller.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#define TAM_HIDE_VT_PROMPT_MAX_PASS 256
#define TAM_HIDE_VT_PROMPT_IDLE_CLEAR_MS 30000

enum vt_prompt_mode {
	VT_PROMPT_IDLE = 0,
	VT_PROMPT_PICKER = 1,
};

struct vt_prompt {
	int fd;
	bool owns_fd;
	bool active;
	bool termios_saved;
	struct termios saved_termios;

	enum vt_prompt_mode mode;
	char pass[TAM_HIDE_VT_PROMPT_MAX_PASS + 1];
	size_t pass_len;
	int64_t last_input_ms;
	char status[96];

	char pick_token[TAM_HIDE_PICK_TOKEN_HEX_LEN + 1];
	struct seat_pick_candidate pick_items[TAM_HIDE_MAX_PICK_CANDIDATES];
	size_t pick_count;
	size_t pick_selected;
};

/* Attach non-blocking raw console prompt to an existing TTY/PTY fd. */
int vt_prompt_init_fd(struct vt_prompt *p, int fd, bool owns_fd);

/* Open /dev/tty<lock_vt> and attach the non-disclosing console prompt. */
int vt_prompt_init_vt(struct vt_prompt *p, int lock_vt);

/* Pollable fd for keyboard input, or -1 if inactive. */
int vt_prompt_fd(const struct vt_prompt *p);

/* Clear passcode/picker state, set optional status message, and render. */
void vt_prompt_reset(struct vt_prompt *p, const char *status);

/* Render current non-disclosing idle or post-auth picker screen to the TTY. */
void vt_prompt_render(struct vt_prompt *p);

/* Clear half-typed passcode after TAM_HIDE_VT_PROMPT_IDLE_CLEAR_MS of idle. */
void vt_prompt_tick(struct vt_prompt *p, int64_t now_ms);

/* Read available bytes from TTY and drive AUTH/SELECT/CANCEL on controller. */
int vt_prompt_handle_input(struct vt_prompt *p, struct seat_controller *c);

/* Wipe secrets, restore termios, and close owned fd. */
void vt_prompt_destroy(struct vt_prompt *p);

#endif
