-- Nested demo configuration for tam-hide 0.1.1 under tam-pane.
-- Run with: sandbox/demo-tam-pane, which prepends GUIDE and TAM_HIDE.

tp.config{ gaps = 8 }

tp.bind("Meta+l", function()
	tp.lock{TAM_HIDE, "--blank-after", "10"}
end)

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

print("demo-tam-pane.lua loaded: Meta+L launches tam-hide --blank-after 10 via tp.lock")
