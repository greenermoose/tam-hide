# Roadmap

- 0.0.0: repository, security design, and VM-verified VT seat-controller prototype (`tam-hided`).
- 0.1.0: `tam-hide` Wayland `ext-session-lock-v1` locker verified with `tam-pane` (PAM for current user, minimal Cairo/Pango non-disclosing UI, multi-output/hotplug coverage, fail-closed recovery).
- 0.1.1: output blanking and wake (`zwlr_output_power_manager_v1` + `ext_idle_notifier_v1`) under `tam-pane`.
- 0.2.1: libtam for the daemon's sockets and the passcode store; a slow
  client can no longer hold the daemon (one 2 s deadline per request);
  saves sync the directory; tam-hidectl restores echo on Ctrl+C.
- 0.2.0: whole-seat VT integration (`tam-hided` + compositor adapter confirmation + privileged Argon2id passcode store + multi-account/VT handoff).
