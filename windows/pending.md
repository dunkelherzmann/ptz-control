# Windows: what isn't production-ready yet

What the Windows version lacks or gets wrong compared with the macOS reference in
[`SPECIFICATION.md`](../SPECIFICATION.md), in build order, each with its spec section. Section 14 of the spec describes
how Windows differs today. Fix broken items first, one pull request each, and tick an item with its PR number once
it is built and its test passes on Windows. [`bugs.md`](../bugs.md) and [`enhancements.md`](../enhancements.md) stay the
record of requests and releases.

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

- [ ] **Title bar reads "PTZ Control" only**, without "— PTZ Coordinates Controller by Herbert Cisneros". (§2 K1, §14)
- [ ] **A test copy that can be driven.** The snapshot hook (`PTZ_SNAPSHOT_PATH`) the Mac and Linux apps have, so a
  test can connect, move and save a picture of the window against the mock camera. (§16 T4)
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
- [ ] **Hover text on every control**, as listed in the Mac `pending.md`. (§3, §7, §8, §10)
- [ ] **Narrator.** Field labels, captions, the header, the status chip, the status message, the readouts and the
  footer are painted, not controls, so Narrator reads the fields without labels and can't read the position; the
  plane and meters have no accessible name or value. (§3, §7)

## Not checked automatically

Nothing tests the Windows app or its core build. Before a release, try by hand everything in spec sections 2–13, on
Windows 11 and on Windows 10 (for the OpenSSH 8.1 limits in §14).
