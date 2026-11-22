#!/bin/sh
# Headless integration test: verify tam-hide's ext-session-lock-v1 engine,
# idle output blanking (ext_idle_notifier_v1 + zwlr_output_power_manager_v1),
# disabled-blanking mode, and fail-closed SIGTERM against 2-output tam-pane.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TAM_PANE="$ROOT/../tam-pane/build/tam-pane"
LOCKER="$ROOT/build/test_wayland_lock"

if [ ! -x "$TAM_PANE" ]; then
	echo "SKIP: $TAM_PANE not found (build tam-pane first)"
	exit 0
fi
if [ ! -x "$LOCKER" ]; then
	echo "FAIL: $LOCKER not built (run make test)" >&2
	exit 1
fi

TMPDIR_TEST=$(mktemp -d "${TMPDIR:-/tmp}/tam-hide-headless.XXXXXX")
TP_PID=""
cleanup() {
	if [ -n "$TP_PID" ]; then
		kill "$TP_PID" 2>/dev/null || true
		wait "$TP_PID" 2>/dev/null || true
	fi
	rm -rf "$TMPDIR_TEST"
}
trap cleanup EXIT INT TERM

# --- Case 1: 2 outputs, lock, idle blanking (150ms) -> 2x POWER_OFF, fail-closed SIGTERM ---
CFG="$TMPDIR_TEST/init.lua"
READY="$TMPDIR_TEST/ready"
PIDFILE="$TMPDIR_TEST/locker.pid"
EVENTS="$TMPDIR_TEST/events"
LOG="$TMPDIR_TEST/tam-pane.log"
XDG_RUNTIME_DIR="$TMPDIR_TEST/run1"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

cat >"$CFG" <<EOF
tp.on("startup", function()
	tp.lock{"$LOCKER", "--ready-file", "$READY", "--pid-file", "$PIDFILE",
		"--event-file", "$EVENTS", "--blank-after-ms", "150"}
end)
EOF

XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
WLR_BACKENDS=headless \
WLR_RENDERER=pixman \
WLR_HEADLESS_OUTPUTS=2 \
"$TAM_PANE" -c "$CFG" >"$LOG" 2>&1 &
TP_PID=$!

# 1. Wait up to 5s for tam-hide to receive ext_session_lock_v1.locked.
i=0
while [ $i -lt 50 ]; do
	if [ -s "$READY" ] && grep -q '^LOCKED$' "$READY"; then
		break
	fi
	sleep 0.1
	i=$((i + 1))
done

if ! grep -q '^LOCKED$' "$READY" 2>/dev/null; then
	echo "FAIL: locker did not report LOCKED within 5s" >&2
	cat "$LOG" >&2
	exit 1
fi

if ! grep -q "session locked" "$LOG"; then
	echo "FAIL: tam-pane did not log 'session locked'" >&2
	cat "$LOG" >&2
	exit 1
fi

# 2. Wait up to 3s for idle blanking + compositor POWER_OFF confirmation on both outputs.
i=0
while [ $i -lt 30 ]; do
	off_count=$(grep -c '^POWER_OFF$' "$EVENTS" 2>/dev/null || true)
	if grep -q '^BLANKED$' "$EVENTS" 2>/dev/null && [ "$off_count" -ge 2 ]; then
		break
	fi
	sleep 0.1
	i=$((i + 1))
done

off_count=$(grep -c '^POWER_OFF$' "$EVENTS" 2>/dev/null || true)
if ! grep -q '^BLANKED$' "$EVENTS" 2>/dev/null || [ "$off_count" -lt 2 ]; then
	echo "FAIL: expected BLANKED and 2x POWER_OFF, got events:" >&2
	cat "$EVENTS" >&2 || true
	cat "$LOG" >&2
	exit 1
fi

# 3. Terminate the locker with SIGTERM while blanked and verify fail-closed behavior.
LOCKER_PID=$(cat "$PIDFILE" 2>/dev/null || true)
if [ -z "$LOCKER_PID" ]; then
	echo "FAIL: locker did not write pid file" >&2
	cat "$LOG" >&2
	exit 1
fi

kill -TERM "$LOCKER_PID"

i=0
while [ $i -lt 30 ]; do
	if grep -q "session locker exited without unlocking; session stays locked" "$LOG"; then
		break
	fi
	sleep 0.1
	i=$((i + 1))
done

if ! grep -q "session locker exited without unlocking; session stays locked" "$LOG"; then
	echo "FAIL: tam-pane did not confirm fail-closed lock after locker SIGTERM" >&2
	cat "$LOG" >&2
	exit 1
fi

kill "$TP_PID" 2>/dev/null || true
wait "$TP_PID" 2>/dev/null || true
TP_PID=""

# --- Case 2: --blank-after-ms 0 disables blanking cleanly ---
CFG2="$TMPDIR_TEST/init2.lua"
READY2="$TMPDIR_TEST/ready2"
EVENTS2="$TMPDIR_TEST/events2"
LOG2="$TMPDIR_TEST/tam-pane2.log"
XDG_RUNTIME_DIR2="$TMPDIR_TEST/run2"
mkdir -p "$XDG_RUNTIME_DIR2"
chmod 700 "$XDG_RUNTIME_DIR2"

cat >"$CFG2" <<EOF
tp.on("startup", function()
	tp.lock{"$LOCKER", "--ready-file", "$READY2",
		"--event-file", "$EVENTS2", "--blank-after-ms", "0"}
end)
EOF

XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR2" \
WLR_BACKENDS=headless \
WLR_RENDERER=pixman \
WLR_HEADLESS_OUTPUTS=2 \
"$TAM_PANE" -c "$CFG2" >"$LOG2" 2>&1 &
TP_PID=$!

i=0
while [ $i -lt 50 ]; do
	if [ -s "$READY2" ] && grep -q '^LOCKED$' "$READY2"; then
		break
	fi
	sleep 0.1
	i=$((i + 1))
done

if ! grep -q '^LOCKED$' "$READY2" 2>/dev/null; then
	echo "FAIL: locker (--blank-after-ms 0) did not report LOCKED" >&2
	cat "$LOG2" >&2
	exit 1
fi

sleep 0.3
if grep -qE '^(BLANKED|POWER_OFF)$' "$EVENTS2" 2>/dev/null; then
	echo "FAIL: --blank-after-ms 0 unexpectedly blanked outputs:" >&2
	cat "$EVENTS2" >&2
	exit 1
fi

echo "PASS: test_tam_pane_headless (2 outputs, WAYLAND_SOCKET lock, idle blanking 2x POWER_OFF, disabled-blank mode, fail-closed SIGTERM)"
