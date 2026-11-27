# P1: Session and whole-seat lock

Status: Phase 1 (`0.1.0`), Phase 1b (`0.1.1`), Phase 2a (`0.2.0`/`0.2.1`), and Phase 2b (`0.2.2`, disposable KVM VM end-to-end verification VM1–VM8) complete on `dev`. Live workstation seat/login integration requires Fred's explicit approval and a recorded rollback point.

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
2. **Phase 1b (`0.1.1`, implemented in W7): Display blanking and wake**
   - Bound `zwlr_output_power_manager_v1` and `ext_idle_notifier_v1` (preferring
     v2 `get_input_idle_notification` with v1 fallback) inside `src/wl_lock.c`
     with a unit-tested `src/power_policy.{h,c}` state machine.
   - Blanks all outputs after `--blank-after SECONDS` of idle (default 30s,
     `0` disables), clears any half-typed password on blank, wakes and redraws
     on input (`resumed` / keyboard fallback), degrades gracefully if protocols
     are absent or an output reports `failed`, and restores power before unlock.
   - Verified with `make test` (`test_power_policy`) and 2-output headless
     `tam-pane` (`make test-headless`).
3. **Phase 2a (`0.2.0`, implemented in W8): Lock confirmation, Argon2id store, and multi-account picker**
   - Privileged `libargon2` Argon2id passcode store (`src/passcode_store.{h,c}`,
     default `m=65536, t=2, p=1`, hard floors `m >= 19456, t >= 2, p = 1`,
     `0600` atomic save, `$argon2id$`-only load, constant-sweep matching across
     all accounts) and `tam-hidectl passcode-{set,del,list}` (reads passcodes
     from stdin/TTY without echo, never argv).
   - `tam-hided` session registry and compositor lock-confirmation gate
     (refuses `LOCK`/`HANDOFF` away from an active graphical session until
     `CONFIRM_LOCK`), plus privilege-separated `--auth-socket` with
     active-session `SO_PEERCRED` gating, server-side exponential lockout, and
     single-use 128-bit `PICK` tokens (`AUTH`/`SELECT`/`CANCEL`).
   - `tam-hide` `--ready-fd`, `--auth-socket`, `--session-vt`, controller auth
     client (`src/auth_controller.{h,c}`), and post-auth `LOCK_UI_PICKER` UI.
   - Verified with `make clean && make all test test-headless`.
4. **Phase 2b (`0.2.2`, verified in W11): Disposable KVM/QEMU VM end-to-end verification**
   - Fixed cross-VT handoff semantics (`AUTH_HANDOFF` keeps the active VT's
     Wayland session locked on cross-VT switch; `perform_handoff_to_vt`
     preserves `lock_confirmed` across cross-VT switches).
   - Verified VM1–VM8 end-to-end in a disposable Arch KVM guest with real
     kernel `VT_LOCKSWITCH`, `tam-hided.service`, Argon2id `passcodes.db`,
     `sway` 1.12, multi-account `LOCK_UI_PICKER`, and `SIGKILL` crash
     recovery (`upstream/2026-11-27-phase2b-vm-verification.md`).

## Rules and boundaries

- Phase 1 (`0.1.0`) protects the active `tam-pane` Wayland session only; it
  does not engage kernel `VT_LOCKSWITCH` or protect other virtual terminals.
- Any live login or privileged seat change requires its own approval and
  recorded rollback point.
