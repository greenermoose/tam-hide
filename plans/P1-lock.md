# P1: Whole-seat lock

Status: in progress (`0.0.0`, no locker implemented yet).

## Completed groundwork and prototype

- Field survey recorded in `upstream/2026-11-20-screen-lock.md` and `ideas/`.
- Disposable KVM/QEMU whole-seat experiment (T1-T7) completed in
  `upstream/2026-11-22-seat-vm-experiment.md`, establishing the fail-closed
  `VT_LOCKSWITCH` + atomic handoff + `ext-session-lock-v1` + `exec`
  session-leader design in `arch/seat-control.md`.
- Minimal privileged seat-controller prototype (`src/tam-hided.c`,
  `src/tam-hidectl.c`, `src/controller.c`, `src/vt.c`,
  `systemd/tam-hided.service`) built with zero external dependencies, unit
  tested (`make test`), and verified end-to-end in a disposable KVM guest
  (startup auto-lock on `tty6`, `Ctrl+Alt+F*` blocking, `0600`+`SO_PEERCRED`
  socket enforcement, atomic handoff, and `SIGKILL` + `Restart=always`
  fail-closed recovery).

## Remaining phases

1. **Compositor adapter interface + Hyprland verification:** Define the
   compositor adapter interface (`ext-session-lock-v1` lock confirmation,
   hotplug coverage, blanking/wake) and verify it against Hyprland in an
   isolated VM (after `sway` baseline).
2. **Passcode enrollment, authentication and selection:** Implement the
   privileged salted slow-hash store, passcode-only matching (no passcodes in
   argv/logs), rate limiting, duplicate-passcode account and session picker,
   and safe PAM/session authentication.
3. **Hardening and live-seat readiness:** Verify failures, hotplug, blanking,
   and recovery end-to-end in the VM before proposing any live login change.

## Rules and boundaries

- Identify the protocols `tam-pane` needs, and verify its backend only when
  those capabilities exist.
- A login or privileged live change requires its own approval and recorded
  rollback point.
