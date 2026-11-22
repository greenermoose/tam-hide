#ifndef TAM_HIDE_CONTROLLER_H
#define TAM_HIDE_CONTROLLER_H

#include "vt.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum seat_state {
    SEAT_STATE_LOCKED = 0,
    SEAT_STATE_ACTIVE_USER = 1,
};

struct seat_controller {
    struct vt_ops vt;
    int lock_vt;
    int active_vt;
    enum seat_state state;
    bool vt_locked;
    uid_t allowed_uid;
    int listen_fd;
    char socket_path[108];
};

/*
 * Initialize controller, immediately engage VT_LOCKSWITCH, atomically
 * switch to lock_vt (recovering from any prior crash/user VT), render the
 * non-disclosing banner, and bind the Unix domain control socket.
 */
int controller_init(struct seat_controller *c,
                    const struct vt_ops *vt,
                    int lock_vt,
                    const char *socket_path,
                    uid_t allowed_uid);

/* Handle one pending client connection on listen_fd (accept + single request). */
int controller_handle_one(struct seat_controller *c);

/* Process a single command line directly (useful for unit tests and IPC). */
int controller_dispatch(struct seat_controller *c,
                        const char *line,
                        char *resp,
                        size_t resp_size);

/* Close socket and unlink socket_path. Optionally release VT_LOCKSWITCH. */
void controller_cleanup(struct seat_controller *c, bool release_vt_lock);

const char *seat_state_name(enum seat_state st);

#endif
