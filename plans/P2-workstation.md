# P2: Workstation readiness and `tam-pane` DRM/KMS integration

- **Status:** in progress (`0.3.1` Phase P2b disposable KVM `tam-pane` DRM/KMS verification done on `dev`; `0.3.2` Phase P2c live workstation rollout awaits Fred)
- **Versions:** `0.3.0` to `0.3.x`

## Goal

Close the operational gaps between the Phase P1 VM prototype (`0.2.2`) and an
unattended Tamlinux workstation running `tam-pane` on DRM/KMS, then prove the
integrated stack in a disposable KVM guest before any Fred-authorized live host
installation.

## Versions and milestones

1. **Phase P2a (`0.3.0`, done in W12): Workstation readiness (repo-only)**
   - **Live `passcodes.db` hot-reload:** `controller_reload_store()` loads into
     a temporary store and swaps atomically (preserving the existing store on
     corrupt/unreadable files and wiping any pending `PICK`). Exposed via
     `RELOAD_STORE` on `control.sock`, `tam-hidectl reload-store`, `SIGHUP` in
     `tam-hided`, and `ExecReload=/bin/kill -HUP $MAINPID` in
     `systemd/tam-hided.service`.
   - **Automatic `seat0` session discovery (`src/session_sync.{h,c}`):**
     `controller_reconcile_sessions()` synchronizes `seat0` graphical and
     console sessions, preserves `lock_confirmed` for unchanged `(vt, uid, kind)`
     entries, and fails closed back to `lock_vt` if the active user session
     exits or crashes (T4b). Backed by `libsystemd` `sd_login_monitor` when
     built with `USE_SYSTEMD=1` (default), with `--no-logind` at runtime and
     `USE_SYSTEMD=0` at build time for Void/runit portability.
   - **Interactive non-disclosing console prompt on `lock_vt` (`src/vt_prompt.{h,c}`):**
     Non-blocking raw TTY prompt in `tam-hided`'s `poll()` loop driving
     `controller_dispatch_auth` directly (masked dots, server-side lockout,
     post-auth multi-account/session picker with `1-9`/arrows/`j`/`k`/`Esc`,
     30s idle buffer clear, `explicit_bzero`, `--no-vt-prompt` opt-out).
   - **`tam-hide` controller auto-discovery:** Automatically connects to
     `/run/tam-hide/auth.sock` and reads `XDG_VTNR` when `tam-hided` is
     running, falling back seamlessly to PAM when `tam-hided` is absent (with
     `--pam-only` opt-out).
   - **Packaging hygiene:** `make install` remains `tam-hide`-only for Tamlinux
     `0.7.x`; opt-in `make install-seat` installs `tam-hided` and `tam-hidectl`.
2. **Phase P2b (`0.3.1`, done in W13): Disposable KVM `tam-pane` DRM/KMS verification**
   - Booted a disposable Arch KVM guest with `tam-pane` `0.2.14` on DRM/KMS,
     `tam-hided.service` (`sd-login` + `lock_vt` prompt), and `tam-hide`
     auto-discovery (no manual `tam-hidectl register-session` or `handoff`).
   - Caught and fixed two real-world defects: moved default `lock_vt` from `6`
     to `7` (avoiding `systemd-logind`'s `NAutoVTs=6` / `ReserveVT=6` race
     where `agetty` stole `/dev/tty6` and echoed passcodes) and handled
     `POLLHUP | POLLERR | POLLNVAL` on `prompt.fd` and `watcher` fd in
     `src/tam-hided.c` (eliminating a 100% CPU busy loop after `vhangup()`).
   - Verified P2B-1–P2B-10 (`upstream/2026-11-27-phase-p2b-tam-pane-vm-verification.md`),
     including `tam-pane`'s `wl_seat` VT-resume behavior, `tty7` unlock and
     picker handoff, `SIGHUP` passcode reload + corrupt-DB preservation,
     compositor exit T4b return to `tty7`, and `SIGKILL` resilience.
3. **Phase P2c (`0.3.2`, awaits Fred): Live workstation rollout**
   - Install and verify on a Fred-selected host with an explicit approval and
     recorded Snapper rollback point.

## Acceptance

- `0.3.0`: `make clean && make all test test-headless` and
  `make clean && make USE_SYSTEMD=0 all test` pass with zero warnings; PTY
  tests cover `vt_prompt` and unit tests cover `RELOAD_STORE` and
  `controller_reconcile_sessions`.
- `0.3.1`: Disposable KVM `tam-pane` DRM/KMS verification matrix passes and is
  recorded under `upstream/`.
- `0.3.2`: Live host verification accepted by Fred with rollback point recorded.

## Dependencies

- `libsystemd` (`sd-login`, optional via `USE_SYSTEMD?=1`, default `1` on
  systemd hosts; `USE_SYSTEMD=0` keeps clean non-systemd builds).

## Risks

- Live `tam-hided.service` activation can lock out local console access if
  misconfigured; mitigated by keeping `make install` `tam-hide`-only and gating
  live installation on Phase P2b VM verification, Fred's explicit approval, and
  a Snapper rollback point.

## Open points

- Target host selection and schedule for Phase P2c live workstation rollout.
