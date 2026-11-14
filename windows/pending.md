# Windows: what isn't production-ready yet

What the Windows version lacks or gets wrong compared with the macOS reference in
[`SPECIFICATION.md`](../SPECIFICATION.md), in build order, each with its spec section. Section 14 of the spec describes
how Windows differs today. Fix broken items first, one pull request each, and tick an item with its PR number once
it is built and its test passes on Windows. [`bugs.md`](../bugs.md) and [`enhancements.md`](../enhancements.md) stay the
record of requests and releases.

## Next (top priority)

- [x] **☰ menu matches the Mac's in-window menus** (#23). The ☰ holds the same menus, submenus, order and shortcuts, and no command is reachable only somewhere else. (H1, enhancements.md) *Asked 2026-11-09.*

## Bugs to fix

No open bugs in `bugs.md`.

- [ ] **The web interface fails through SSH when the camera redirects to another HTTPS port.** Windows OpenSSH has no
  connection sharing, so only the HTTPS port set under Advanced is forwarded, when the tunnel opens; the Mac forwards
  the port the camera redirects to, when it's needed. Forward on demand with a second `ssh` process, or say in the
  log which port to set. (§5 S6, §10 W1)
- [ ] **SSH dialogs need Win32-OpenSSH 8.6 or later.** The 8.1 client in Windows 10 can't relay password, passphrase
  or MFA prompts. Not checked what the app shows then; it should say so plainly in the status message and the log.
  (§5 S2)

## To build

- [x] **Camera picture in the plane** (I3, I4, V6; #5). The picture as large as fits with the view grid, the ⟳
  button and Camera ▸ Refresh Picture (Ctrl+Shift+R), the window growing to its shape; click to aim, double-click to
  move there (`ptz_picture`, `ptz_move_in_view`); a new picture once the camera stops. Tested like the Mac's run 3.
- [x] **Saved logins.** The Login menus, Save login, the first-save choice, unlocking and Manage Logins, over the
  core's login vault, with the vault file in the app's own data folder. (§17 P1–P8; #11)
- [x] **Copy each build to the desktop.** (#7) `windows\scripts\build.ps1` copies the new `PTZControl.exe` to the desktop as
  `PTZ Control.exe`, replacing the older one. (enhancements.md)
- [x] **Title bar reads "PTZ Control"** (#8), without "— PTZ Coordinates Controller by Herbert Cisneros". (§2 K1, §14)
- [x] **A test copy that can be driven.** The snapshot hook (`PTZ_SNAPSHOT_PATH`) the Mac and Linux apps have, so a
  test can connect, move and save a picture of the window against the mock camera. (§16 T4, T7; #11)
- [ ] **Core tests on Windows.** `test-core.sh` and `test-ssh.sh` run only where clang and libcurl are; the WinHTTP
  client and the Windows SSH tunnel have no automatic test. A PowerShell runner for `tests/core-smoke.c` and
  `tests/ssh-smoke.c` with MSVC, against the mock camera and Windows' own `sshd`. (§1 C5, §5, §16 T2–T3)
- [ ] **Remember the window's size and position.** (§2 K2)
- [ ] **Advanced behind a disclosure, closed at first.** (§3 F5)
- [ ] **Return in a connection field moves to the next field**, and connects only from the camera password or the
  Connect button. (§3 F8)
- [ ] **Event buffer keeps its place** when you've scrolled up; it jumps to the newest line today. (§11 E2)
- [ ] **Key file with `~`.** The field takes a full path; check whether Windows' `ssh` expands `~\.ssh\…`, and
  expand it in the app if not. (§3 F3)
- [ ] **Quit in the ☰ menu** (Ctrl+Q), as on Linux; today only closing the window quits. (§2 K4, §13 H1)
- [ ] **Hover text on every control**, as listed in `macos/pending.md`. (§3, §7, §8, §10)
- [ ] **Narrator.** Field labels, captions, the header, the status chip, the status message, the readouts and the
  footer are painted, not controls, so Narrator reads the fields without labels and can't read the position; the
  plane and meters have no accessible name or value. (§3, §7)

## Not checked automatically

- [ ] Saved-login dialogs and Manage Logins actions (§17 P3, P6, P7), and how the Login menus look in light and dark
  on a real desktop. `windows\scripts\test.ps1` covers saving, filling the fields from a saved login and the Manage Logins
  list (P4, P5).

Nothing tests the Windows app or its core build. Before a release, try by hand everything in spec sections 2–13, on
Windows 11 and on Windows 10 (for the OpenSSH 8.1 limits in §14).
