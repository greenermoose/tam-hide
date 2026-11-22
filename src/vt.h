#ifndef TAM_HIDE_VT_H
#define TAM_HIDE_VT_H

#include <stdbool.h>

#define TAM_VT_MIN 1
#define TAM_VT_MAX 63

struct vt_ops {
    void *ctx;
    int (*get_active)(void *ctx, int *out_vt);
    int (*lock_switch)(void *ctx);
    int (*unlock_switch)(void *ctx);
    int (*activate_and_wait)(void *ctx, int vt_nr);
    int (*render_banner)(void *ctx, int lock_vt, const char *status);
};

struct linux_vt_ctx {
    const char *tty_dev;
};

/* Populate ops with the real Linux /dev/tty0 ioctl backend. */
void vt_linux_init(struct vt_ops *ops, struct linux_vt_ctx *ctx, const char *tty_dev);

/*
 * Atomically switch to target_vt while keeping VT_LOCKSWITCH fail-closed.
 * Always attempts to re-engage VT_LOCKSWITCH before returning, even on error.
 * Returns 0 on success, negative errno on failure.
 */
int vt_atomic_handoff(const struct vt_ops *ops, int target_vt, double *elapsed_ms);

#endif
