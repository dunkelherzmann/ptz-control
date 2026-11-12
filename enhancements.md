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
- [x] **Ship the Linux app as a Flatpak instead of an AppImage.** `org.ptzcontrol.app` on KDE's Qt 6 runtime, built on the Linux machine with flatpak-builder (`scripts/build-linux.sh`) and tested against the mock camera (`scripts/test-linux.sh`). *Requested 2026-11-08.* Linux. Done in #3.
- [x] **Login vaults for SSH hosts and for cameras,** like Herb's Terminal's login vault. Two separate stores: one for SSH logins, one for camera logins. Both are unlocked by the same Administrator Password, but they stay independent of each other: one SSH host can be used with several different camera logins, and a local camera (the Local route) needs no SSH login at all. *Requested 2026-11-08.* Mac, Linux and Windows. Released in [1.0.49](https://github.com/dunkelherzmann/ptz-control/releases/tag/v1.0.49).
  - [x] Mac (#9)
  - [x] Linux (#10)
  - [x] Windows (#11)
- [x] **Keep a specification and a plan per version:** `SPECIFICATION.md` describes every feature with its id, platforms, shortcut and hover text, and the builds per platform; `pending.md` (macOS), `windows/pending.md` and `linux/pending.md` list what each version still lacks or gets wrong, in build order. *Requested 2026-11-08.* Done in #5.
- [x] **The title bar reads "PTZ Control",** on every platform: no "PTZ Coordinates Controller by Herbert Cisneros" subtitle or suffix. *Requested 2026-11-08.* Mac, Linux and Windows. Done in #8. Released in [1.0.49](https://github.com/dunkelherzmann/ptz-control/releases/tag/v1.0.49).
  - [x] Mac
  - [x] Linux
  - [x] Windows
- [x] **Windows: a portable `.exe` on the desktop.** PTZ Control for Windows stays a single static `PTZControl.exe` with no installer, runtime or anything added to the system, and every new Windows build is copied to the Windows PC's desktop as `PTZ Control.exe` so it's easy to find. *Requested 2026-11-08 · Windows.*
  - [x] Single static portable `.exe` (already the case)
  - [x] Copied to the desktop by `scripts\build-windows.ps1` on every build (PR #7)
- [x] **Saved logins keep only usernames and passwords,** for both the SSH logins and the camera logins: no host, port or key file. *Requested 2026-11-08.* Mac, Linux and Windows. Released in [1.0.49](https://github.com/dunkelherzmann/ptz-control/releases/tag/v1.0.49).
  - [x] Mac (#10)
  - [x] Linux (#10)
  - [x] Windows (#11)
- [x] **Saved logins have a Name,** so one username can be saved with different passwords (for example `cam1` and `cam2`, both `admin`). Lists show Name, Username and Password; the Name tells logins apart. *Requested 2026-11-08.* Mac, Linux and Windows. Done in #15; released in [1.0.60](https://github.com/dunkelherzmann/ptz-control/releases/tag/v1.0.60).
  - [x] Mac (#15)
  - [x] Linux (#15)
  - [x] Windows (#15)

- [x] **Keep each platform's files in its own folder.** The root holds only what applies to the whole project (shared core, docs, trackers, version and cross-platform scripts); `macos/`, `linux/` and `windows/` each hold their own build and test scripts and `pending.md`. Also line endings fixed by `.gitattributes`, a build check on every pull request (GitHub Actions on macOS, Linux and Windows), and the duplicate root `ptz-control.sh` removed (it lives in `legacy/`). *Requested 2026-11-09.* All platforms. Done in #19.

- [x] **All menus and submenus in the app's window, on macOS too.** Nothing in the macOS menu bar beyond the app menu macOS always shows, so on wide screens no command needs a trip to the top of the screen. Top priority: the next item in every `pending.md`. *Requested 2026-11-09.* All platforms.
  - [x] Mac (#22)
  - [x] Linux (#23)
  - [x] Windows (#23)
- [x] **Tests watch what the app does in the background** (asked 2026-11-10, Linux and Windows). Each test run of the app goes through the master-control skill: a crash, a window that stops responding, a write outside the test folders (Linux) or a change to the real app's settings and saved logins fails the run. `PTZ_NO_OBSERVE=1` turns it off.
  - [x] Linux (`linux/scripts/test.sh`)
  - [x] Windows (`windows/scripts/test.ps1`)
- [x] **Plain wording in the explanatory docs** (asked 2026-11-12, all platforms). A pass with the humanizer skill over `README.md` and `SPECIFICATION.md`. In #27: the README's opening says what the app leaves out in a full sentence. The rest already read plainly.
