# Whole-seat feasibility: initial study

Date: 2026-11-20. Documentation study only; no live or isolated execution.

A Wayland session lock is per compositor. The compositor must implement
ext-session-lock-v1 and confirm the lock; it does not itself protect other
VTs. See https://wayland.app/protocols/ext-session-lock-v1 . tam-pane's
current source has no session-lock implementation, and its handoff says
desktop protocols and multiple monitors are not yet implemented.

logind can enumerate and activate sessions, but its LockSession operation
sends a Lock signal: https://github.com/systemd/systemd/blob/main/man/org.freedesktop.login1.xml .
A successful request is not evidence that a client secured its session.
vlock --all disables console switching while active, but its documented
behavior does not prove safe handoff to an arbitrary existing session.
See https://man7.org/linux/man-pages/man1/vlock.1.html .

Open issue: after handoff, prevent access to other unlocked VTs without
blocking legitimate future switching or exposing the previous session.
Also determine how to recover from controller and compositor failure, and
how to authenticate a selected user without leaking other accounts' state.
Do not assert feasibility until an isolated multi-session test demonstrates
these properties, including crashes, VT switching, hotplug, and login.

Next experiment: use a disposable VM with two console users and a nested
Wayland session; instrument lock acknowledgement and VT transitions, test
crash and restart at every handoff, and record observable leaks and recovery.
No installation, account changes or live-seat experiments are authorized
by this note.

## VM preparation

The official 2026.11.01 Arch ISO matched its published SHA-256 and reached
the boot menu in QEMU 11.1.2 using software emulation. No guest sessions
were started. KVM was unavailable, and QEMU exposed no local graphical
display backend. A loopback VNC listener started outside the sandbox, but
no VNC viewer was found. A display and input path remains to be set up;
this is a preparation gap, not evidence that the experiment is impossible.
No handoff, crash, VT, hotplug or authentication property was tested.

Follow-up: the disposable VM experiment (T1-T7) was executed on 2026-11-22
using KVM, `bochs-display`, QMP key injection, and QMP `screendump`; see
[2026-11-22-seat-vm-experiment.md](2026-11-22-seat-vm-experiment.md).
