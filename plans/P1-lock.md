# P1: Session and whole-seat lock

Status: Phase 1 (`0.1.0`) implemented and verified for `tam-pane` (`dev`), including Fred's nested `sandbox/demo-tam-pane` check.

## Completed groundwork

- Field survey recorded in `upstream/2026-11-20-screen-lock.md` and `ideas/`.
- Disposable KVM/QEMU whole-seat experiment (T1-T7) completed in
  `upstream/2026-11-22-seat-vm-experiment.md`, establishing the fail-closed
  `VT_LOCKSWITCH` + atomic handoff + `ext-session-lock-v1` + `exec`
  session-leader design in `arch/seat-control.md`.
- Minimal privileged seat-controller prototype (`src/tam-hided.c`,
  `src/tam-hidectl.c`, `src/controller.c`, `src/vt.c`,
  `systemd/tam-hided.service`) built and verified in a disposable KVM guest (W5).

## Implementation phases (`tam-pane` first, then VTs)

1. **Phase 1 (`0.1.0`, implemented in W6): `tam-hide` Wayland locker for `tam-pane`**
   - `ext-session-lock-v1` client (`src/wl_lock.c`) launched via `tam-pane`'s
     `tp.lock{"tam-hide"}` over `WAYLAND_SOCKET`.
   - Multi-output coverage and live `wl_output` hotplug handling.
   - Minimal Cairo/Pango non-disclosing cover (`src/render.c`) with passcode
     dots and status/error/lockout feedback.
   - Async forked PAM worker (`src/auth.c`, `src/auth_pam.c`) for the current
     user, `PR_SET_DUMPABLE=0`, `explicit_bzero` password wiping, and client-side
     backoff (`src/lock_state.c`).
   - Fail-closed on signal/crash (`tam-pane` keeps black cover rects and binding
     gate active until a newly launched locker unlocks).
   - Verified with unit tests (`make test`), 2-output headless `tam-pane`
     integration test (`make test-headless`), and nested `sandbox/demo-tam-pane`.
2. **Phase 1b (`0.1.1`, next): Display blanking and wake**
   - Bind `zwlr_output_power_manager_v1` (already exposed by `tam-pane` 0.2.0)
     and `ext_idle_notifier_v1` to blank outputs while locked and wake on input.
3. **Phase 2 (`0.2.0`): Broaden to whole-seat VT locking**
   - Wire `tam-hide` lock confirmation (`--ready-fd` / adapter IPC) into
     `tam-hided` before atomic VT handoff.
   - Implement the privileged Argon2id salted slow-hash passcode store,
     passcode-only matching (no passcodes in argv/logs), and duplicate-passcode
     account/session picker.
   - Verify end-to-end in a disposable KVM VM before proposing any live login
     or seat change.

## Rules and boundaries

- Phase 1 (`0.1.0`) protects the active `tam-pane` Wayland session only; it
  does not engage kernel `VT_LOCKSWITCH` or protect other virtual terminals.
- Any live login or privileged seat change requires its own approval and
  recorded rollback point.
