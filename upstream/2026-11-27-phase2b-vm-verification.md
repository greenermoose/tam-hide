# Phase 2b: Disposable KVM/QEMU End-to-End Verification (`0.2.2`)

Date: 2026-11-27. Empirical end-to-end verification of `tam-hided`, `tam-hide`,
`tam-hidectl`, `systemd/tam-hided.service`, the privileged Argon2id passcode
store, the compositor lock-confirmation gate, the multi-account/session picker,
and real kernel `VT_LOCKSWITCH` in an isolated disposable KVM/QEMU guest. No
host packages were installed and no live login or seat changes were made.

## Environment

- Hypervisor: QEMU (`qemu-system-x86_64 -enable-kvm`, 4 vCPU, 4 GiB RAM)
- Guest image: Arch Linux 2026.11.01 live ISO (`archlinux-2026.11.01-x86_64.iso`,
  2 GiB tmpfs cowspace)
- Guest kernel: Linux `7.2.11-arch1-1`
- Virtual display and input: `bochs-display` (`1280x800`),
  `virtio-keyboard-pci`, `virtio-mouse-pci`
- Guest packages installed in ephemeral RAM: `argon2`, `sway` 1.12
  (`WLR_RENDERER=pixman`), `swaybg`, `ttf-dejavu`
- Binaries under test: `tam-hide`, `tam-hided`, `tam-hidectl`, and
  `systemd/tam-hided.service` staged via read-only 9p share into `/usr/local/bin`
  and `/etc/systemd/system`
- Sessions on `seat0`:
  - `tty2`: console user `carol` (`uid=1002`, unique passcode)
  - `tty3`: console user `bob` (`uid=1001`, shared passcode with `alice`)
  - `tty4`: graphical Wayland session for `alice` (`uid=1000`, `exec sway`,
    solid `#0055aa` blue background, shared passcode with `bob`)
  - `tty6`: dedicated lock/controller VT managed by `tam-hided.service`
- Passcode store: `/var/lib/tam-hide/passcodes.db` (`0600 root:root`,
  Argon2id `m=65536 KiB, t=2, p=1`) populated via `tam-hidectl passcode-set`
  over stdin

## Defects Found and Fixed During Phase 2b

1. **Cross-VT authentication unlocked the active VT's Wayland session:**
   `auth_controller_verify()` and `auth_controller_select()` previously mapped
   any `OK ` response from `tam-hided` to `AUTH_OK`, causing `tam-hide` on
   `tty4` to call `ext_session_lock_v1_unlock_and_destroy()` when another user
   authenticated to switch to `tty2`/`tty3`. Fixed by adding `AUTH_HANDOFF` in
   `src/auth.h`, `src/auth.c`, `src/auth_controller.c`, and `src/wl_lock.c` so
   `tam-hide` keeps the current Wayland session locked and resets to
   `LOCK_UI_IDLE` whenever the target VT differs from `--session-vt`.
2. **Cross-VT handoff cleared `lock_confirmed` on still-locked graphical VTs:**
   `perform_handoff_to_vt()` in `src/controller.c` previously cleared
   `dst->lock_confirmed = false` on every handoff to a graphical VT, which
   caused `auth_peer_allowed()` to deny `alice`'s `tam-hide` upon returning to
   a still-locked `tty4`. Fixed to preserve `dst->lock_confirmed` across
   cross-VT switches and clear it only on same-VT unlock.
3. **Banner version string:** Updated `src/vt.c` from `"prototype 0.0.0"` to
   use `TAM_HIDE_VERSION`.

## Verification Matrix (VM1–VM8)

| Case | Scenario | Result | Observed Evidence |
| --- | --- | --- | --- |
| **VM1** | Service startup & `VT_LOCKSWITCH` barrier | **PASS** | `tam-hided.service` moved `seat0` to `tty6` (`c4a1cfe3616c84ac`, `state=LOCKED active_vt=6 vt_locked=1`). QMP `Ctrl+Alt+F2`/`F3`/`F4` stayed on `tty6`; `timeout 1 chvt 2` blocked (`rc=124`). |
| **VM2** | Socket privilege separation & `SO_PEERCRED` | **PASS** | `/run/tam-hide` `0755`, `control.sock` `0600 root:root` (rejected `alice`), `auth.sock` `0666 root:root` (rejected `HANDOFF 2` with `ERR unknown_command`; rejected `bob` on `tty6` with `ERR permission_denied`), `passcodes.db` `0600 root:root`. |
| **VM3** | Compositor lock-confirmation gate | **PASS** | `HANDOFF 4` (`0.08 ms`) showed `100.0%` `#0055aa` blue. `LOCK` and `HANDOFF 3` both failed closed with `ERR lock_unconfirmed`. Launching `tam-hide --auth-socket /run/tam-hide/auth.sock --session-vt 4 --ready-fd ...` emitted `LOCKED`, set `locked=1`, and covered `tty4` (`0.0%` blue, `bfa3cd3b8ba31195`). |
| **VM4** | Server-side lockout & wrong passcode | **PASS** | Three wrong passcodes via QMP `send-key` triggered server-side 1s lockout; immediate valid passcode during lockout kept `seat0` on `tty4` (`0.0%` blue). |
| **VM5** | Cross-VT handoff via unique passcode (`carol`) | **PASS** | Typing `carol`'s passcode into `tam-hide` on `tty4` atomically switched `seat0` to `tty2` (`1e961d71e21fc3c3`), kept `VT_LOCKSWITCH` active (`Ctrl+Alt+F3`/`F4` blocked), and left `alice`'s `tam-hide` process alive and locked on `tty4`. |
| **VM6** | Multi-account picker & single-use `PICK` token | **PASS** | Returning to `tty4` (`0.07 ms`) confirmed `alice`'s session remained locked (`0.0%` blue, `locked=1`). Entering the shared passcode rendered `LOCK_UI_PICKER` (`4d333f9a79cb4e3e`); choosing `2` + `Enter` switched to `bob` on `tty3` (`f77be1e306382d8a`) while keeping `tty4` locked. Returning to `tty4` and choosing `1` + `Enter` unlocked `tty4` cleanly (`100.0%` blue, `locked=0`). |
| **VM7** | Crash resilience (`SIGKILL` on `tam-hide` and `tam-hided`) | **PASS** | `SIGKILL` on `tam-hide` left `sway` fail-closed (`100.0%` `#ff0000` red, `0.0%` blue). `SIGKILL` on `tam-hided` (`pid 1734`) left kernel `VT_LOCKSWITCH` engaged (`Ctrl+Alt+F2` stayed on `tty4` while daemon dead); `systemd` `Restart=always` spawned `pid 2105` and returned `seat0` to locked `tty6`. |
| **VM8** | Clean teardown | **PASS** | QEMU terminated cleanly and ephemeral `/tmp/tam-hide-phase2b-vm` removed; zero live host changes. |

## Compositor Note Observed in VM Testing

Under `sway` 1.12 (`wlroots`), after a VT switch back to `tty4`, `wl_seat` does
not re-advertise `WL_SEAT_CAPABILITY_KEYBOARD` until the first key press arrives,
delivering that key inside `wl_keyboard.enter(..., keys)` rather than as a
`wl_keyboard.key(..., PRESSED)` event. `tam-hide` intentionally ignores keys
already held at `wl_keyboard.enter` so lock/switch shortcut keys are never
appended to the passcode buffer. Verify `tam-pane`'s `wl_seat` resume behavior
when integrating `tam-hide` with `tam-pane` on DRM/KMS.

## Next Boundary

Phase 2b is complete. Installing `tam-hided.service` or enabling whole-seat
locking on a live Tamlinux workstation remains a separate step requiring Fred's
explicit approval and a recorded rollback point.
