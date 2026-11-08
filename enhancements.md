# Enhancements

Every enhancement requested for PTZ Control, oldest first. New requests are added at the end and nothing is removed.
A box is ticked once the change has shipped. A request that covers several platforms has a box for each, and its main box is ticked when they're all done.

- [x] **Get the repository ready to be public.** Personal data was removed from the whole history, the repo was recreated with that clean history, and it was made public. *Requested 2026-11-07.* Done 2026-11-07.
- [x] **Say that the app is provided as is, without support,** in the README. *Requested 2026-11-07.* Done 2026-11-07 (the README's Support section).
- [x] **Protect the `main` branch** from force pushes and deletion. *Requested 2026-11-07.* Done 2026-11-07 (a "Protect main" ruleset).
- [x] **Keep these trackers:** `enhancements.md` and `bugs.md`, updated as requests come in and as they're done. *Requested 2026-11-07.* Set up in #1.
- [x] **Build each platform's app on its own machine.** The Windows app is built on Windows with the Visual Studio C++ Build Tools (`scripts/build-windows.ps1`, still one static portable `.exe`), and the Linux AppImage on Linux in a rootless Podman container (`scripts/build-linux.sh`), instead of cross-compiling with mingw-w64 and Apple `container` VMs on the Mac. *Requested 2026-11-08.* Linux and Windows. Done in #2.
  - [x] Windows
  - [x] Linux
