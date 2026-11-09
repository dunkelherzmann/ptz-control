# Linux: what isn't production-ready yet

What the Linux version lacks or gets wrong compared with the macOS reference in
[`SPECIFICATION.md`](../SPECIFICATION.md), in build order, each with its spec section. Section 14 of the spec describes
how Linux differs today. Fix broken items first, one pull request each, and tick an item with its PR number once it
is built and its test passes on Linux. [`bugs.md`](../bugs.md) and [`enhancements.md`](../enhancements.md) stay the record
of requests and releases.

## Next (top priority)

- [x] **☰ menu matches the Mac's in-window menus** (#23). The ☰ holds the same menus, submenus, order and shortcuts, and no command is reachable only somewhere else. (H1, enhancements.md) *Asked 2026-11-09.*

## Bugs to fix

No open bugs in `bugs.md`.

- [ ] **System doesn't follow a light/dark switch while the app runs.** It reads the desktop's setting once, at
  launch; the Mac and Windows versions follow the switch. Listen for the portal's color-scheme change and reapply.
  (§12 A2)

## To build

- [x] **Saved logins.** The Login menus, Save login, the first-save choice, unlocking and Manage Logins, over the
  core's login vault, with the vault file in the app's own data folder. (§17 P1–P8; #10)
- [x] **Title bar reads "PTZ Control"** (#8), without "— PTZ Coordinates Controller by Herbert Cisneros". (§2 K1, §14)
- [ ] **Remember the window's size and position.** (§2 K2)
- [ ] **Advanced behind a disclosure, closed at first.** (§3 F5)
- [ ] **Return in a connection field moves to the next field**, and connects only from the camera password or the
  Connect button. (§3 F8)
- [ ] **Event buffer keeps its place** when you've scrolled up; it jumps to the newest line today. (§11 E2)
- [ ] **Hover text on every control**, as listed in `macos/pending.md`. (§3, §7, §8, §10)
- [ ] **Screen readers.** The status chip, the readouts, the plane and the meters are painted widgets without an
  accessible name or value, so Orca can't read the position or aim with them. (§7 I2–I7)
- [ ] **Feature tests beyond the snapshot.** `linux/scripts/test.sh` checks that the Flatpak connects to the mock camera over
  the Local route with ONVIF, moves and returns to the start, by counting requests and saving a picture. Extend it to
  Axis, the SSH route through a throwaway `sshd`, and a check of the values the app shows. (§16 T4–T5)

## Not checked automatically

- [ ] Saved-login dialogs and Manage Logins actions (§17 P3, P6, P7): unlocking, the first-save choice, the
  Administrator Password, New, Edit, Delete and Lock. `linux/scripts/test.sh` covers saving from the window, filling the
  fields from a saved login and the Manage Logins list (P4, P5).

- [ ] Whether `test-core.sh` and `test-ssh.sh` (§16 T2–T3) run on the Linux machine: they need clang and libcurl's
  headers, and were written on the Mac.
- [ ] Everything in spec sections 2–13 beyond connect, move and return with ONVIF on the Local route, in particular
  the SSH route inside the Flatpak sandbox (§5), on X11 and on native Wayland (§14).
