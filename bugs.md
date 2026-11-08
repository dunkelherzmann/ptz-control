# Bugs

Every bug reported for PTZ Control, oldest first. New reports are added at the end and nothing is removed.
A box is ticked once the fix has shipped. A bug that affects several platforms has a box for each, and its main box is ticked when they're all done.
Items marked *found in testing* were found while testing other work, not reported by a user.

- [x] **Mac: clicking a login in Manage Logins crashes the app.** Selecting a row reloaded the list, which selected the row again, until the stack ran out. *Reported 2026-11-08 (1.0.49).* Mac only: the Linux and Windows lists update only their buttons on selection. Fixed in #13; released in [1.0.54](https://github.com/dunkelherzmann/ptz-control/releases/tag/v1.0.54).
- [x] **Manage Logins opens on the wrong list.** Choosing Manage Logins… from the SSH host's Login menu should show SSH Logins, and from the camera's Login menu Camera Logins. *Reported 2026-11-08 (1.0.60).* Mac, Linux and Windows. Fixed in #17.
  - [x] Mac
  - [x] Linux
  - [x] Windows
