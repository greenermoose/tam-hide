# Security boundary

The intended boundary is the whole local seat, not a single Wayland
surface. A lock must protect every existing graphical and console session
and prevent switching to an unprotected virtual terminal. A failed lock,
crashed controller, changing display or unavailable compositor must not
expose a session. Establish and test these properties before presenting the
command as a lock. Document physical-access and recovery limitations.

Keep authentication and seat control independent of compositor adapters.
An adapter must report secure lock acquisition and loss, cover all outputs,
and provide input and display-power handling. `tam-pane` 0.2.0 provides
`ext-session-lock-v1` (granted exclusively over `WAYLAND_SOCKET` via `tp.lock`),
`ext-idle-notify-v1`, and `zwlr-output-power-management-v1`.

Phase 1 (`0.1.0`) verifies single-session Wayland locking under `tam-pane`
with PAM authentication for the current user; it does **not** yet engage
`VT_LOCKSWITCH` or protect other virtual terminals. Whole-seat VT protection
and multi-account passcode selection land in Phase 2 (`0.2.0`).

Passcodes select accounts, including all accounts sharing a passcode.
Store only salted slow hashes in a privileged store. Passcodes must not
appear in arguments or logs. A normal console login still requires normal
Linux authentication. Investigate safe authentication, rate limiting,
session selection and recovery before implementing privileged operations.
