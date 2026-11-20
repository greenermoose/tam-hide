# P1: Whole-seat lock

First establish a defensible, testable design for protecting existing
console and graphical sessions, VT switching, crashes and recovery.
Prototype in an isolated environment before changing a live login setup.
Then implement enrollment, passcode matching, duplicate account and session
selection, blanking and wake, with compositor integration behind an
interface. Test failures and hotplug as well as successful paths.

Test Hyprland first. Identify the protocols tam-pane needs, and verify its
backend only when those capabilities exist. A login or privileged live
change requires its own approval and recorded rollback point.
