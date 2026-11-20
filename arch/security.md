# Security boundary

The intended boundary is the whole local seat, not a single Wayland
surface. A lock must protect every existing graphical and console session
and prevent switching to an unprotected virtual terminal. A failed lock,
crashed controller, changing display or unavailable compositor must not
expose a session. Establish and test these properties before presenting the
command as a lock. Document physical-access and recovery limitations.

Keep authentication and seat control independent of compositor adapters.
An adapter must report secure lock acquisition and loss, cover all outputs,
and provide input and display-power handling. Investigate standard Wayland
session-lock protocols and their failure semantics before selecting an
implementation. tam-pane currently lacks the needed desktop protocols;
do not claim support until verified against a running compositor.

Passcodes select accounts, including all accounts sharing a passcode.
Store only salted slow hashes in a privileged store. Passcodes must not
appear in arguments or logs. A normal console login still requires normal
Linux authentication. Investigate safe authentication, rate limiting,
session selection and recovery before implementing privileged operations.
