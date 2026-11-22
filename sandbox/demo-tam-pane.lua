-- Nested demo configuration for tam-hide 0.1.0 under tam-pane.
-- Run with: sandbox/demo-tam-pane, which prepends GUIDE and TAM_HIDE.

tp.config{ gaps = 8 }

tp.bind("Meta+l", function()
	tp.lock{TAM_HIDE}
end)

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

print("demo-tam-pane.lua loaded: Meta+L launches tam-hide via tp.lock")
