# Whole-seat control and handoff architecture

Status: design validated by disposable VM experiment
([2026-11-22-seat-vm-experiment.md](../upstream/2026-11-22-seat-vm-experiment.md)).
No locker or controller is implemented yet (`0.0.0`).

## Core invariant

`tam-hide` protects the entire local seat (`seat0`), not a single Wayland
surface. No hardware VT switch, locker crash, controller crash, or output
hotplug may expose an unauthenticated user's console or graphical session.

## Verified mechanisms

### 1. Kernel VT-switch barrier (`VT_LOCKSWITCH`)

- The privileged seat controller keeps `VT_LOCKSWITCH` (`0x560B`) enabled on
  `seat0` whenever multiple user sessions exist or the seat is hidden.
- Unlike `VT_SETMODE` (`VT_PROCESS`) and `vlock --all` (which both failed open
  under `SIGKILL` in T3/T4d), `VT_LOCKSWITCH` persists in the kernel if the
  controller dies (T4c), making controller crashes fail-closed.
- Hardware `Ctrl+Alt+F*` and unprivileged `chvt` are blocked while
  `VT_LOCKSWITCH` is active.

### 2. Privileged atomic handoff

When a user authenticates on the dedicated lock VT (or requests a legitimate
switch via the controller):

1. Before leaving any graphical session, the controller locks it through its
   compositor adapter and waits for protocol confirmation
   (`ext_session_lock_v1.locked`), not `logind`'s `LockSession` return or
   `LockedHint` (T1).
2. The controller transitions to the target VT in a brief privileged critical
   section: `VT_UNLOCKSWITCH` -> `VT_ACTIVATE(target)` ->
   `VT_WAITACTIVE(target)` -> `VT_LOCKSWITCH` (measured at ~48 ms in T5).
3. If the target is a graphical session that is currently locked, the
   controller unlocks it only after the user's passcode/account selection
   succeeds.
4. `VT_LOCKSWITCH` remains active after handoff so the active user cannot
   switch directly to another user's console or graphical VT via `Ctrl+Alt+F*`
   (T5 Model B).

### 3. Compositor adapter contract

Compositor integration sits behind a narrow adapter interface so Hyprland
(first) and future compositors (`tam-pane` once it implements the required
protocols) share the same seat controller:

- **Acquire lock:** Bind `ext-session-lock-v1`, cover every current and
  hotplugged `wl_output` with a lock surface, and report lock acquisition only
  after the compositor sends `locked` (T1, T6).
- **Fail-closed on locker crash:** Require compositors whose
  `ext-session-lock-v1` implementation keeps the session locked (solid blank
  or red screen, no client surfaces revealed) if the lock client dies (T4a).
- **Session-leader requirement:** Graphical sessions managed by `tam-hide`
  must run the compositor as the `logind` session leader (`exec`), never as a
  child of an interactive console shell, so a compositor crash terminates the
  session instead of exposing a shell on the VT (T4b).
- **Display power / blanking:** Adapter exposes blanking and wake hooks behind
  the same interface (not yet tested in VM).

### 4. Failure recovery

- **Locker client crash:** Compositor holds the VT fail-closed (T4a); the
  controller detects client exit and either restarts the lock client or
  switches the seat atomically to the dedicated lock VT.
- **Compositor crash:** Because the compositor is the session leader (`exec`),
  its death ends that session without exposing a shell (T4b); the controller
  switches the seat to the dedicated lock VT.
- **Controller crash:** `VT_LOCKSWITCH` remains set in the kernel (T4c),
  preventing unauthorized VT switches. `systemd` restarts the controller
  (`Restart=always`); on startup, the controller re-attaches to the lock VT
  and re-verifies all active sessions before accepting input.
- **Last-resort local recovery:** Documented physical-access boundary —
  if both the controller and its lock UI fail repeatedly, recovery requires
  root over SSH, serial console, or reboot/recovery boot.

## Known boundaries and open work

- **Local `logind` visibility (T7):** Unprivileged local users can still read
  session lists via `loginctl list-sessions` once logged in; `tam-hide`'s lock
  UI itself must never disclose account names or active sessions prior to
  authentication.
- **Physical access:** Cold boot, firmware access, DMA, and Magic SysRq are
  outside a software locker's boundary and must be documented clearly.
- **Unverified items for P1 implementation:** Hyprland adapter verification in
  an isolated VM, privileged salted-hash passcode store and PAM integration,
  rate limiting, duplicate-passcode account/session picker, and display
  blanking/wake.
