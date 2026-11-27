# Phase P2b: Disposable KVM/QEMU `tam-pane` DRM/KMS Verification (`0.3.1`)

Date: 2026-11-27. Empirical end-to-end verification of `tam-hide` `0.3.0` → `0.3.1`
integrated with **`tam-pane` on DRM/KMS**, **`systemd/tam-hided.service`**,
**`libsystemd` `sd-login` automatic `seat0` session discovery**, the
**interactive `lock_vt` console prompt**, **`tam-hide` controller
auto-discovery**, and **live `SIGHUP` `passcodes.db` hot-reload** inside an
isolated disposable Arch Linux KVM/QEMU guest. Zero host packages were
installed and zero live host login, seat, or `systemd` changes were made.

## Environment

- Hypervisor: QEMU (`qemu-system-x86_64 -enable-kvm`, 4 vCPU, 4 GiB RAM)
- Guest image: Arch Linux 2026.11.01 live ISO (`archlinux-2026.11.01-x86_64.iso`,
  2 GiB tmpfs cowspace)
- Guest kernel: Linux `7.2.11-arch1-1`
- Virtual display and input: `bochs-display` (`1280x800`),
  `virtio-keyboard-pci`, `virtio-mouse-pci`
- Guest packages installed in ephemeral RAM: `wlroots0.20`, `lua54`, `argon2`,
  `pango`, `cairo`, `swaybg`, `ttf-dejavu`
- Binaries under test (staged via read-only 9p share into `/usr/local/bin` and
  `/etc/systemd/system`): `tam-hide`, `tam-hided`, `tam-hidectl`,
  `systemd/tam-hided.service`, and `tam-pane` (`0.2.14`)
- Real `systemd-logind` sessions on `seat0` (zero manual `tam-hidectl
  register-session` calls):
  - `tty1`: `root` (`uid=0`, `Type=tty`, Arch live ISO autologin)
  - `tty2`: `carol` (`uid=1002`, `Type=tty`, unique passcode)
  - `tty3`: `bob` (`uid=1001`, `Type=tty`, shared passcode with `alice`)
  - `tty4`: `alice` (`uid=1000`, `Type=wayland`, `XDG_VTNR=4`, `tam-pane` on
    DRM/KMS with `WLR_RENDERER=pixman`, `swaybg -c '#0055aa'` solid blue
    desktop, and `Meta+l` bound to `tp.lock{"/usr/local/bin/tam-hide",
    "--blank-after", "0"}` with zero controller flags)
  - `tty7`: dedicated `lock_vt` managed by `tam-hided.service` (`logind=on`,
    `vt_prompt=on`)
- Passcode store: `/var/lib/tam-hide/passcodes.db` (`0600 root:root`, Argon2id
  `m=65536 KiB, t=2, p=1`)

## Defects Found and Fixed During Phase P2b

1. **`systemd-logind` `autovt`/`ReserveVT=6` race onto `tty6` (security defect):**
   On default `systemd-logind` configurations (`NAutoVTs=6`, `ReserveVT=6`),
   activating VT 6 caused `systemd-logind` to start `autovt@tty6.service`
   (`getty@tty6.service`) even though `tam-hided.service` declared
   `Conflicts=getty@tty6.service autovt@tty6.service`. `agetty` ran `vhangup()`
   on `/dev/tty6`, stole keyboard input from `vt_prompt`, and echoed the typed
   passcode onto `archiso login:`.
   - **Fix:** Default `lock_vt` to **`7`** (outside `NAutoVTs=6` /
     `ReserveVT=6`) in `src/tam-hided.c` and `systemd/tam-hided.service`, while
     retaining `Conflicts=getty@tty7.service autovt@tty7.service`.
2. **100% CPU spin on `prompt.fd` / `watcher` `POLLHUP` in `tam-hided`:**
   When `/dev/tty6` was hung up (`vhangup()`), `poll()` in `src/tam-hided.c`
   returned `POLLHUP` on `prompt.fd`, which was ignored because only `POLLIN`
   was handled, causing a 100% CPU busy loop (`42.187s` CPU in `42s` wall time).
   - **Fix:** Handle `POLLHUP | POLLERR | POLLNVAL` on both `prompt.fd` (destroy
     and attempt clean reopen on `lock_vt`, disabling cleanly on failure) and
     `watcher` fd (destroy cleanly) in `src/tam-hided.c`. Post-fix CPU usage
     was `3.75s` across ~12 Argon2id 64 MiB verifications and `9.17 ms` after
     respawn.

## Verification Matrix (P2B-1–P2B-10)

| Case | Scenario | Result | Observed Evidence |
| --- | --- | --- | --- |
| **P2B-1** | Startup, `VT_LOCKSWITCH`, & `sd-login` auto-discovery | **PASS** | `tam-hided.service` moved `seat0` to `tty7` (`OK state=LOCKED lock_vt=7 active_vt=7 vt_locked=1`, dump `e8502195f8309c0a`). `sd-login` automatically discovered `tty1` (`root`, console), `tty2` (`carol`, console), `tty3` (`bob`, console), and `tty4` (`alice`, graphical, `locked=0`) with zero manual `register-session` calls. QMP `Ctrl+Alt+F2`/`F3`/`F4` and `timeout 1 chvt 2` (`rc=124`) stayed on `tty7`. |
| **P2B-2** | `tty7` console prompt: lockout & unique passcode | **PASS** | Three wrong passcodes via QMP `send-key` triggered server-side 1s lockout; immediate valid `carol456` stayed on `tty7`. After lockout expiry, `carol456` atomically switched `seat0` from `tty7` to `tty2` (`OK state=ACTIVE_USER lock_vt=7 active_vt=2 vt_locked=1`, `d4e96a65fd4f8e97`). |
| **P2B-3** | `tty7` console prompt: multi-account picker | **PASS** | Locking back to `tty7` and entering shared `shared123` rendered the interactive picker on `tty7`; choosing `1` + `Enter` switched `seat0` to `tty4` (`tam-pane` on DRM/KMS, `99.99%` `#0055aa` blue, `69aaa6067537b112`). |
| **P2B-4** | `tam-pane` DRM/KMS lock + `tam-hide` auto-discovery | **PASS** | While unlocked on `tty4`, `tam-hidectl lock` failed closed (`ERR lock_unconfirmed`). QMP `Meta+l` launched `/usr/local/bin/tam-hide --blank-after 0` (no controller flags); `tam-hide` auto-discovered `/run/tam-hide/auth.sock` + `XDG_VTNR=4`, sent `CONFIRM_LOCK 4` (`locked=1`), and covered `tty4` (`0.0%` blue, `2ed6e0d072a055fa`). |
| **P2B-5** | Cross-VT handoff & `tam-pane` `wl_seat` VT-resume check | **PASS** | Typing `carol456` into `tam-hide` on `tty4` switched `seat0` to `tty2` while keeping `tty4` `locked=1`. Returning via `tty7` picker kept `tty4` covered (`0.0%` blue). Confirmed `tam-pane` `wl_seat` VT-resume behavior (see note below), then shared passcode + `1` + `Enter` unlocked `tty4` (`99.99%` blue, `locked=0`). |
| **P2B-6** | Live `SIGHUP` / `reload-store` & corrupt-file resilience | **PASS** | Updated `carol` to `carol789` and ran `systemctl reload tam-hided.service` (`SIGHUP`): same PID `3514`, old `carol456` rejected on locked `tty4`, new `carol789` accepted (switched to `tty2`). Corrupting `passcodes.db` and running `tam-hidectl reload-store` returned `ERR reload_failed errno=22` while keeping PID `3514` and the in-memory store intact (`carol789` still authenticated). |
| **P2B-7** | Compositor exit/crash fail-closed return to `tty7` (T4b) | **PASS** | Stopping `sess-alice` while `seat0` was on `tty4` triggered `sd-login` reconciliation: `vt=4` was removed from `tam-hidectl sessions` (`OK count=3`), `seat0` atomically returned to locked `tty7` (`OK state=LOCKED lock_vt=7 active_vt=7 vt_locked=1`, `1c987d76a410675c`), and the freshly reset `tty7` prompt accepted `carol789` cleanly. |
| **P2B-8** | `SIGKILL` resilience (`tam-hide` & `tam-hided`) | **PASS** | `SIGKILL` on `tam-hide` left `tam-pane` fail-closed pure black (`100.0%` black, `0.0%` blue, `44c6b1f0ddcf9849`). `SIGKILL` on `tam-hided` (`pid 3514`) left kernel `VT_LOCKSWITCH` engaged (`Ctrl+Alt+F3` & `timeout 0.5 chvt 3` blocked at `rc=124`); `systemd` `Restart=always` respawned `pid 3887` (`CPUUsageNSec=9.17 ms`) and returned `seat0` to locked `tty7` (`e8502195f8309c0a`). |
| **P2B-9** | Host regression suite | **PASS** | `make clean && make all test test-headless` (6 unit/PTY suites + 2-output headless `tam-pane` suite) and `make clean && make USE_SYSTEMD=0 all test` passed with zero `-Werror` warnings. |
| **P2B-10** | Clean teardown | **PASS** | QEMU terminated cleanly and `/tmp/tam-hide-p2b-vm` removed; zero live host changes. |

## Empirical `tam-pane` `wl_seat` VT-Resume Finding

Under `tam-pane` `0.2.14` (`wlroots 0.20`), just as observed under `sway` 1.12
in Phase P1, after a VT switch back to a still-locked `tty4`, `first_key_delivered`
was `False`: `wl_seat` does not re-advertise `WL_SEAT_CAPABILITY_KEYBOARD` until
the first key press arrives, delivering that first key inside
`wl_keyboard.enter(..., keys)` rather than as a `wl_keyboard.key(..., PRESSED)`
event. Because `tam-hide` intentionally ignores keys already held at
`wl_keyboard.enter` (so lock/switch shortcut keys are never appended to the
passcode buffer), the very first keystroke after returning to a locked `tam-pane`
VT wakes the keyboard seat and subsequent keystrokes enter normally. This is
recorded here as an upstream `tam-pane` / `wlroots` observation; no changes were
made to `tam-pane` in this slice.

## Next Boundary

Phase P2b (`0.3.1`) is complete. Phase P2c (`0.3.2`) — installing and enabling
`tam-hided.service` on a live Tamlinux workstation — remains gated on Fred's
target host selection, explicit approval, and a recorded Snapper rollback point.
