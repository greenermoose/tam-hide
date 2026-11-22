#!/bin/sh
# Headless integration test: verify tam-hide's ext-session-lock-v1 engine
# against tam-pane (2 headless outputs, WAYLAND_SOCKET via tp.lock,
# Cairo/Pango lock-surface rendering, and fail-closed on SIGTERM).
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

CFG="$TMPDIR_TEST/init.lua"
READY="$TMPDIR_TEST/ready"
PIDFILE="$TMPDIR_TEST/locker.pid"
LOG="$TMPDIR_TEST/tam-pane.log"
XDG_RUNTIME_DIR="$TMPDIR_TEST/run"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

cat >"$CFG" <<EOF
tp.on("startup", function()
	tp.lock{"$LOCKER", "--ready-file", "$READY", "--pid-file", "$PIDFILE"}
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

# 2. Terminate the locker with SIGTERM and verify fail-closed behavior:
#    tam-hide must NOT unlock on signal, and tam-pane must stay locked.
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

echo "PASS: test_tam_pane_headless (2 outputs, WAYLAND_SOCKET lock, fail-closed SIGTERM)"
