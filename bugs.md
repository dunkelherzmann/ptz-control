# Bugs

Every bug reported for PTZ Control, oldest first. New reports are added at the end and nothing is removed.
A box is ticked once the fix has shipped. A bug that affects several platforms has a box for each, and its main box is ticked when they're all done.
Items marked *found in testing* were found while testing other work, not reported by a user.

- [x] **Mac: clicking a login in Manage Logins crashes the app.** Selecting a row reloaded the list, which selected the row again, until the stack ran out. *Reported 2026-11-08 (1.0.49).* Mac only: the Linux and Windows lists update only their buttons on selection. Fixed in #13; released in [1.0.54](https://github.com/dunkelherzmann/ptz-control/releases/tag/v1.0.54).
- [x] **Manage Logins opens on the wrong list.** Choosing Manage Logins… from the SSH host's Login menu should show SSH Logins, and from the camera's Login menu Camera Logins. *Reported 2026-11-08 (1.0.60).* Mac, Linux and Windows. Fixed in #17; released in [1.0.65](https://github.com/dunkelherzmann/ptz-control/releases/tag/v1.0.65).
  - [x] Mac
  - [x] Linux
  - [x] Windows
- [x] **The core test sometimes ran before its mock camera was listening.** `scripts/test-core.sh` paused one second after starting the mock, and on a busy GitHub macOS runner both connect checks failed with "Camera unreachable". It now waits until the port answers, up to 10 seconds. *Found in testing, 2026-11-12 · Mac (GitHub's build).*
- [x] **Windows: Digest sign-in never answered** (*found in testing* 2026-11-14, Windows). WinHTTP accepted the camera's username and password but resent the request without an answer to the Digest challenge, so camera pictures (and every Axis request) got HTTP 401. The core now answers the challenge itself (MD5 or SHA-256 through Windows' CNG), as curl does on the Mac and Linux. Fixed in #5.
