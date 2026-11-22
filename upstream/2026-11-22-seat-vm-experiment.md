# Whole-seat feasibility: disposable VM experiment

Date: 2026-11-22. Empirical test in an isolated disposable KVM/QEMU guest;
no host package installs, no live login or seat changes.

## Environment

- Hypervisor: QEMU 11.1.2 (`qemu-system-x86_64 -enable-kvm`, 4 vCPU, 4 GiB RAM)
- Guest image: official Arch Linux 2026.11.01 live ISO (2 GiB tmpfs overlay)
- Guest kernel: Linux 7.2.11-arch1-1
- Virtual display and input: `bochs-display` (DRM/KMS `1280x800`),
  `virtio-keyboard-pci`, `virtio-mouse-pci`
- Instrumentation: QMP `send-key` (hardware key injection including
  `Ctrl+Alt+F*`), QMP `screendump` (framebuffer pixel verification), serial
  console (`ttyS0`), and root SSH over loopback port forwarding
- Guest packages installed in ephemeral RAM: `sway` 1.12, `swaylock` 1.8.6,
  `swaybg` 1.2.2, `foot` 1.28.0, `kbd` (`vlock` 2.10.0)
- Sessions on `seat0`:
  - `tty2`: console user `alice` (`PAMName=login`, banner `ALICE-TTY2-SECRET`)
  - `tty3`: console user `bob` (`PAMName=login`, banner `BOB-TTY3-SECRET`)
  - `tty4`: graphical Wayland session for `alice` (`sway` with
    `WLR_RENDERER=pixman`, solid `#0055aa` blue background)
  - `tty6`: dedicated lock/controller VT

## Test matrix and observed results

### T1: `logind` `LockSession` vs. `ext-session-lock-v1`

- Baseline `tty4` unlocked `sway`: `screendump` 100.0% `#0055aa` blue
  (hash `51650968916fc57b`).
- After `loginctl lock-session <alice-tty4>`: `LockedHint=no`, `screendump`
  byte-identical (`51650968916fc57b`, 100.0% `#0055aa`).
- After launching `swaylock -c 228822` with `WAYLAND_DEBUG=1`: protocol trace
  recorded `ext_session_lock_v1#3.locked()`, and `screendump` changed to
  100.0% `#228822` green (`17fd234145c0fa79`). `logind` `LockedHint` remained
  `no` (swaylock does not call `SetLockedHint`).
- Finding: `LockSession` alone does not lock a session, and `LockedHint` is
  not proof of lock state. Only compositor protocol confirmation (`locked`)
  proves graphical lock acquisition.

### T2: Hardware VT switching while Wayland-locked

- While `swaylock` actively locked `tty4` (`100% #228822`), QMP `send-key`
  `Ctrl+Alt+F2` switched `active_vt` immediately to `tty2` and exposed
  `=== ALICE-TTY2-SECRET ===` on the framebuffer.
- QMP `send-key` `Ctrl+Alt+F3` switched `active_vt` to `tty3` and exposed
  `=== BOB-TTY3-SECRET ===`.
- Returning via `Ctrl+Alt+F4` showed `tty4` still locked (`100% #228822`).
- Finding: `ext-session-lock-v1` protects only its own compositor VT; it does
  not prevent hardware VT switching to other consoles.

### T3: Console VT-switch locking (`VT_LOCKSWITCH` vs. `vlock --all`)

- On `tty6`, issuing `VT_LOCKSWITCH` (`0x560B`) blocked hardware `Ctrl+Alt+F2`,
  `Ctrl+Alt+F3`, and `chvt 2` (`chvt 2` blocked in `VT_WAITACTIVE` and hit a
  2 s timeout, `RC=124`; `active_vt` stayed `tty6`).
- Running `vlock --all` on `tty6` also blocked `Ctrl+Alt+F2` and `chvt 2`
  (`RC=124`) while `vlock` was alive.
- Sending `SIGKILL` to `vlock --all` immediately allowed `Ctrl+Alt+F2` to
  switch to `tty2`, exposing `alice`'s console (fail-open).
- Finding: `vlock --all` (kbd 2.10.0) relies on `VT_SETMODE` (`VT_PROCESS`)
  rather than `VT_LOCKSWITCH`, so its sudden death releases VT switching.

### T4: Crash and failure semantics under `SIGKILL`

- **T4a (locker client crash):** `SIGKILL` on `swaylock` while locked on
  `tty4` changed the framebuffer from `100% #228822` green to `100% #ff0000`
  solid red (`322d5399ad099919`). `alice`'s `#0055aa` desktop was never
  exposed (fail-closed, per `ext-session-lock-v1`).
- **T4b (compositor crash while locked):**
  - When `sway` was the session leader (`exec sway`), `SIGKILL` terminated
    `alice`'s `logind` session cleanly; `tty4` showed only the console login
    banner and no user shell (fail-closed).
  - When `sway` was started from a shell without `exec`, `SIGKILL` dropped
    directly back to `alice`'s unlocked interactive shell on `tty4`
    (`=== ALICE-TTY4-FALLBACK-SHELL === ... === SWAY EXITED - SHELL STILL
    EXPOSED ===`; fail-open).
- **T4c (`VT_LOCKSWITCH` holder crash):** `SIGKILL` on the process that called
  `VT_LOCKSWITCH` left `vt_dont_switch` enabled in the kernel; `Ctrl+Alt+F2`
  stayed on `tty6` and `chvt 2` timed out (`RC=124`) until a privileged
  process explicitly issued `VT_UNLOCKSWITCH` (`0x560C`) (fail-closed).
- **T4d (`VT_SETMODE` `VT_PROCESS` holder crash):** Blocked `Ctrl+Alt+F2`
  while alive, then failed open to `tty2` immediately on `SIGKILL`.

### T5: Whole-seat handoff and post-handoff VT isolation

- **Model A (unlock `VT_LOCKSWITCH` on handoff):** Handing off to `bob` on
  `tty3` and clearing `VT_LOCKSWITCH` allowed `bob` to press `Ctrl+Alt+F2`
  (exposing `alice`'s `tty2`) and `Ctrl+Alt+F4` (exposing `alice`'s unlocked
  `#0055aa` `sway` desktop). Unacceptable.
- **Model B (persistent `VT_LOCKSWITCH` + confirmed per-session lock + atomic
  controller handoff):**
  1. Locked `alice`'s `sway` on `tty4` and confirmed
     `ext_session_lock_v1#3.locked()`.
  2. Performed an atomic privileged handoff (`VT_UNLOCKSWITCH` ->
     `VT_ACTIVATE(3)` -> `VT_WAITACTIVE(3)` -> `VT_LOCKSWITCH`) in 48.06 ms,
     landing on `tty3`.
  3. While `bob` used `tty3`, QMP `Ctrl+Alt+F2` and `Ctrl+Alt+F4` were both
     blocked (`active_vt` stayed `tty3`, framebuffer showed only `bob`'s
     console).
  4. A subsequent controller-mediated handoff from `tty3` to `tty4` landed on
     `alice`'s still-locked `sway` (`100% #228822` green), confirming
     defense-in-depth.

### T6: Display / output hotplug while locked

- While `swaylock` held the lock on `tty4`, `swaymsg create_output` added
  `HEADLESS-1` (`1920x1080`).
- `WAYLAND_DEBUG` recorded `swaylock` immediately binding a new lock surface
  (`ext_session_lock_v1#3.get_lock_surface(..., wl_output#30)`) and
  acknowledging `configure(46, 1920, 1080)`.
- Unplugging `HEADLESS-1` kept `swaylock` alive and the framebuffer 100.0%
  `#228822` green with no exposure.

### T7: Account and session metadata visibility

- Unprivileged `bob` could list all sessions (`loginctl list-sessions`) and
  read `alice`'s session attributes (`Name=alice`, `TTY=tty4`, `Type=wayland`,
  `State=active`, `LockedHint=no`).
- `bob` was denied `loginctl lock-session` and `loginctl activate` on
  `alice`'s session (`RC=1`, polkit access denied).

## Conclusions and remaining gaps

1. Whole-seat protection is feasible without relying on `vlock --all` or
   `VT_PROCESS`: combine kernel `VT_LOCKSWITCH` (fail-closed across crashes),
   a privileged seat controller performing brief atomic VT handoffs,
   confirmed `ext-session-lock-v1` locking on graphical sessions, and `exec`
   session-leader launch for compositors.
2. Because `VT_LOCKSWITCH` stays set if the controller crashes, the controller
   must run under `systemd` supervision (`Restart=always`) with a documented
   recovery path if the lock UI itself cannot start.
3. Still untested (future phases): Hyprland's `ext-session-lock-v1` behavior
   and DRM hotplug, PAM/passcode verification and rate limiting, display power
   blanking/wake, and `tam-pane` (lacks required desktop protocols).
