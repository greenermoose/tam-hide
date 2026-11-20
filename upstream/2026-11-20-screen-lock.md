# Screen lock and seat survey

Date: 2026-11-20. Question: which projects can inform passcode-only,
whole-seat hiding and account/session selection? Searched project
documentation, Wayland lockers and forks, Linux virtual console locking,
login/session managers, and idle/wake. Search terms included swaylock,
hyprlock, multi-user lock, vlock, greetd and session switching.

The project-by-project findings and links are in `../ideas/`; no source
was copied. Swaylock, hyprlock and gtklock address graphical sessions;
swayidle addresses idle events; logind addresses sessions; vlock addresses
console locking; greetd addresses login. swaylock-effects adds visual
effects. None was shown to meet all tam-hide requirements.

Recommend studying swaylock plus logind and vlock, with swayidle for power
behavior. Proposed use is design inspiration, not code adoption. Check
exact revisions and licenses before copying. CPU, memory and power costs
were not measured; compare in an isolated running test. Documentation
review is not a source audit or a security assessment. Fork coverage is
limited to the swaylock-effects family; no exhaustive fork audit was done.
