# systemd-logind

Starter notes from documentation, 2026-11-20; verify source and license
at the selected revision before adopting code. No code copied.

## What it is

C; LGPL-2.1-or-later; login and seat manager.

## Ideas worth taking

consider: enumerate and activate sessions using logind APIs.

## Things to avoid

LockSession sends a request; do not treat its return as proof of secure lock.

## Where to look

https://github.com/systemd/systemd/blob/main/man/org.freedesktop.login1.xml
