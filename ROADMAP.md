# tam-hide roadmap

`tam-hide` is Tamlinux's Wayland session locker and whole-seat VT lock
controller (`tam-hide`, `tam-hided`, `tam-hidectl`), written in C17 on
top of `libtam`.

- **Current version:** `0.2.2` (branch `dev`)
- **Progress:** Phase P1 complete (`0.0.0`–`0.2.2` verified end-to-end in disposable KVM VM); live workstation rollout awaits Fred's explicit approval and rollback point
- **Versioning:** Semantic versioning; minor marks a locking capability
  tier, patch fixes defects or hardens protocols and IPC.

## Phase summary

| Phase | Versions | Goal | Plan | Status |
| --- | --- | --- | --- | --- |
| **P1** | 0.0.0–0.2.2 | Wayland `ext-session-lock-v1` locker, output blanking/wake, Argon2id whole-seat controller, and KVM VM verification | [P1-lock.md](plans/P1-lock.md) | Done (VM-verified) |

## Phases and milestones

### Phase P1: Session and whole-seat lock (0.0.0–0.2.2) — [P1-lock.md](plans/P1-lock.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.0.0** | Repository setup, security design, and VM-verified VT seat-controller prototype (`tam-hided`) | Done (2026-11-22) |
| **0.1.0** | Wayland `ext-session-lock-v1` locker verified with `tam-pane` (PAM auth, Cairo/Pango cover, multi-output/hotplug, fail-closed) | Done (2026-11-22) |
| **0.1.1** | Output blanking and wake (`zwlr_output_power_manager_v1` + `ext_idle_notifier_v1`) under `tam-pane` | Done (2026-11-23) |
| **0.2.0** | Whole-seat VT integration: `tam-hided` session registry, lock-confirmation gate, privileged Argon2id passcode store, multi-account picker | Done (2026-11-23) |
| **0.2.1** | `libtam` sockets/passcode writes/clock, 2s per-request deadline fixing slow-client stalls, directory sync, `tam-hidectl` echo restore | Done (2026-11-26) |
| **0.2.2** | Phase 2b: cross-VT handoff fixes (`AUTH_HANDOFF`, preserved `lock_confirmed`) and disposable KVM/QEMU VM end-to-end verification (VM1–VM8) | Done (2026-11-27) |

## How a milestone is done

A milestone is done when its acceptance checks in the phase plan pass and
Fred has tried the build. When a milestone lands, bump `VERSION`, update
the phase plan's status, and update this roadmap in the same change.
