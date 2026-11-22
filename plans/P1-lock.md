# P1: Whole-seat lock

Status: in progress (`0.0.0`, no locker implemented yet).

## Completed feasibility groundwork

- Field survey recorded in `upstream/2026-11-20-screen-lock.md` and `ideas/`.
- Disposable KVM/QEMU whole-seat experiment (T1-T7) completed in
  `upstream/2026-11-22-seat-vm-experiment.md`, establishing the fail-closed
  `VT_LOCKSWITCH` + atomic handoff + `ext-session-lock-v1` + `exec`
  session-leader design in `arch/seat-control.md`.

## Remaining phases

1. **Seat controller prototype (isolated VM):** Implement the minimal
   privileged controller (dedicated lock VT, `VT_LOCKSWITCH` management,
   atomic VT handoff, `systemd` crash-restart recovery) and verify all crash
   and handoff paths in the disposable VM.
2. **Compositor adapter interface + Hyprland verification:** Define the
   compositor adapter interface (`ext-session-lock-v1` lock confirmation,
   hotplug coverage, blanking/wake) and verify it against Hyprland in an
   isolated VM (after `sway` baseline).
3. **Passcode enrollment, authentication and selection:** Implement the
   privileged salted slow-hash store, passcode-only matching (no passcodes in
   argv/logs), rate limiting, duplicate-passcode account and session picker,
   and safe PAM/session authentication.
4. **Hardening and live-seat readiness:** Verify failures, hotplug, blanking,
   and recovery end-to-end in the VM before proposing any live login change.

## Rules and boundaries

- Identify the protocols `tam-pane` needs, and verify its backend only when
  those capabilities exist.
- A login or privileged live change requires its own approval and recorded
  rollback point.
