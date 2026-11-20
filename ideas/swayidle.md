# swayidle

Starter notes from documentation, 2026-11-20; verify source and license
at the selected revision before adopting code. No code copied.

## What it is

C; MIT; Wayland idle manager.

## Ideas worth taking

consider: idle timeout and activity-triggered display wake; coordinate lock acquisition before blanking or sleep.

## Things to avoid

Idle notifications and display power are not authentication or a lock.

## Where to look

https://github.com/swaywm/swayidle/blob/master/swayidle.1.scd
