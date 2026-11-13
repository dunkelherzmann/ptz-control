# PTZ Control

PTZ Control reads and sets absolute pan, tilt and zoom on PTZ cameras: ONVIF cameras (Bosch, Milesight and others) and
Axis cameras through VAPIX. It reaches the camera directly or through an SSH host. Connecting never moves the camera;
only Move, Return to Start and Stop do.

One shared C++20 core does all camera, HTTP and SSH work. Each platform has its own native window around it:

| Version | Host | Folder | Its plan |
| --- | --- | --- | --- |
| macOS (the reference) | Swift and AppKit | `macos/` | [`macos/pending.md`](macos/pending.md) |
| Windows 10 and 11, x64 | Win32 and GDI+, C++ | `windows/` | [`windows/pending.md`](windows/pending.md) |
| Linux, as a Flatpak | Qt 6 Widgets, C++ | `linux/` | [`linux/pending.md`](linux/pending.md) |

Sections 1–13 and 17 describe the Mac app. Windows and Linux follow it unless section 14 says otherwise. Every feature has an
id (for example **M3**), which the `pending.md` files use.

**Shortcuts.** The Mac shortcuts are given here. Windows and Linux use Ctrl in place of ⌘ and Alt in place of ⌥. On
every platform the menus sit under a ☰ button in the window, never only in a menu bar at the top of the screen.

**Hover text.** Only the saved-login controls (§17) have hover text so far, on every platform. Each `pending.md` lists
the rest as a gap.

## 1. Core

- **C1 One C interface.** `core/include/ptzcore.h` is the only thing the hosts call: connect, camera info, read
  position, move, stop, take a picture, move toward a point of the picture, find the web interface, check the link,
  close. All calls block. Each host runs them on one
  worker thread or serial queue per session, never on the UI thread, so camera requests never overlap.
- **C2 Callbacks.** While connecting, the core can ask the host to log a line, to ask the operator for text (secret or
  not) and to ask a yes/no question. The host shows these as dialogs and the core thread waits for the answer.
- **C3 Readings are measured or unavailable.** A reading carries the camera's own digits as text, plus its numeric
  value. A value the camera didn't report is marked unavailable, never shown as zero.
- **C4 Locale-free numbers.** Numbers are parsed and printed with a `.` decimal point whatever the system locale.
  `1,5`, `nan` and `inf` are refused.
- **C5 HTTP.** libcurl on macOS and Linux, WinHTTP on Windows. User agent `PTZControl/1.0`; only `http` and `https`.
- **C6 Errors are redacted.** Any error or log line that passes through the C interface has the camera and SSH
  passwords replaced by `[redacted]`, and unprintable characters removed.
- **C7 Version.** `ptz_core_version()` returns the app's version (see B5). The event buffer's first line names it.

## 2. Window and layout

- **K1 One window**, titled "PTZ Control", with no subtitle. Default size
  1180 × 780, smallest 980 × 680. It can't be tabbed.
- **K2 Remembered frame.** The window's size and position are restored at the next launch; the first launch centers it.
- **K3 Layout.** On the left, the **Secure link configuration** panel (300 pt wide). On the right, from the top: the
  header with the status chip, the position plane next to the **Current coordinates** panel, the axis travel meters,
  the target row, the event buffer and a footer line.
- **K4 Closing.** Closing the window quits the app. If a camera is connected or connecting, the app first disconnects
  (L5), then quits. ⌘Q does the same.

## 3. Link configuration

- **F1 Route** (segmented): **Local** (the camera is reachable from this computer) or **SSH Host**. The SSH fields show
  only for SSH Host.
- **F2 Vendor** (segmented): **Bosch/Milesight** (ONVIF) or **Axis** (VAPIX). A note under it reads "ONVIF absolute
  coordinates with camera ONVIF credentials." or "Native VAPIX with HTTP Digest. Pan/tilt in degrees, integer zoom."
- **F3 SSH host fields:** Host (placeholder `jump.example.com`), Username, Password ("blank for keys / agent"), Port
  (22), Key file ("optional, e.g. ~/.ssh/id_ed25519"; a leading `~` means the home folder).
- **F4 Camera fields:** Address (placeholder `192.168.1.100`), HTTP port (80), Username (`admin`, or `PTZ_USER` when
  set), Password ("required", or "from PTZ_PASSWORD" when that variable fills it).
- **F5 Advanced**, behind a disclosure triangle that starts closed: HTTPS port (443, used for the web interface),
  Profile (ONVIF media profile index, 0), Timeout (seconds per request, 15).
- **F6 Checks before connecting**, shown in red under the Connect button: camera address and username required; ports
  1–65535; profile a nonnegative integer; timeout more than 0 and at most 300 seconds; for SSH, host and username
  required. The camera password is required, and focus moves to it when it's empty. The core also refuses an address
  that is a URL or has a username or port in it, an SSH username starting with `-`, and an SSH password with a line
  break.
- **F7 Remembered settings.** Everything except the two passwords is saved when you connect and filled in at the next
  launch. Passwords stay in memory only, unless a login is saved (§17 P5).
- **F8 Keyboard.** Return in the camera password connects; Return in another field moves to the next one. Connect is the
  default button while nothing is connected. All fields are locked while connecting or connected.
- **F9 Status message** under the Connect button: progress ("Connecting…", "Opening SSH tunnel…"), "Connected. No
  movement has been sent.", "Disconnected." or the error, in red.

## 4. Connecting

- **L1 Connect** (⌘K, the Connect button). The button and menu item become **Cancel** / **Cancel Connection** while
  connecting, and the button becomes **Disconnect** once connected.
- **L2 Cancel** (⌘K again while connecting) stops an SSH connection in progress.
- **L3 Connecting never moves the camera.** Once connected, the event buffer shows the camera's name, its units and
  the advertised pan, tilt and zoom ranges ("not advertised" when missing), and the first position is read at once.
- **L4 Disconnect** (⌘⇧K, or the Disconnect button) waits for the request in flight, then closes the session and the
  SSH tunnel. It never sends a PTZ stop.
- **L5 Link lost.** When the SSH tunnel goes away, the next request reports it in red, and the app disconnects.

## 5. SSH route

- **S1 System OpenSSH.** The camera port is forwarded through the system `ssh` (`/usr/bin/ssh` on the Mac), not a
  bundled one and not `sshpass`. `~/.ssh/config`, the agent and keys work as usual; the optional key file is passed
  with `-i` and must be readable.
- **S2 Prompts.** `ssh` runs the app itself as `SSH_ASKPASS`, which relays each prompt to the running app over a
  private socket. The Host Password answers the first password prompt. Key passphrases, MFA codes and further prompts
  appear as an "SSH needs your input" dialog (secret prompts in a password field); yes/no prompts appear as a
  confirmation.
- **S3 Host keys.** A new host key is accepted on first use. When a known host key has changed, the app shows the new
  fingerprint and asks "SSH host key changed" before removing the old key with `ssh-keygen -R` (which keeps a `.old`
  backup) and retrying once.
- **S4 Cipher fallback.** If the connection fails an integrity check, the app retries once with AES-CTR ciphers and
  HMAC-SHA2.
- **S5 Keep-alive and timeouts.** `ConnectTimeout` is the request timeout (at least 5 s); the server is probed every
  15 s and dropped after 3 missed replies. The forward listens on 127.0.0.1 only.
- **S6 HTTPS forward on demand.** The camera's HTTPS port is forwarded through the same connection, using SSH
  connection sharing, the first time the web interface needs it.

## 6. Cameras

- **V1 ONVIF.** SOAP 1.2 with a WS-Security digest. The app finds the media and PTZ services, uses the chosen profile
  and its PTZ configuration, and reads the absolute coordinate spaces and their ranges.
- **V2 Camera clock.** Before the first signed request, the app reads the camera's clock and signs with the camera's
  time. A difference of more than 30 s is logged.
- **V3 ONVIF moves.** AbsoluteMove needs pan and tilt together, so an axis left unchanged is filled in from a fresh
  reading. Targets outside the advertised range are refused before anything is sent. Through SSH, the camera's ONVIF
  services must be on plain HTTP.
- **V4 Axis.** VAPIX `ptz.cgi` with HTTP Digest, camera channel 1. Pan and tilt are in degrees; zoom is an integer in
  the camera's own range. Axes left unchanged are not sent. Fractional zoom and out-of-range values are refused. The
  Axis API is used over HTTP only.
- **V5 Units in the header.** ONVIF: "ONVIF absolute coordinates (commonly pan/tilt −1…1, zoom 0…1; not degrees)".
  Axis: "Axis native coordinates: pan/tilt in degrees; zoom is an integer, not optical magnification".

- **V6 Pictures.** ONVIF: the media service's `GetSnapshotUri` for the chosen profile, fetched once per connection and
  sent through the SSH tunnel when there is one; aiming in the picture is a `RelativeMove` in the field-of-view
  translation space (`http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationSpaceFov`, origin at the picture's
  centre, (1, 1) at its upper right corner), offered only when the camera advertises that space. Axis:
  `/axis-cgi/jpg/image.cgi?camera=1`, and aiming is `ptz.cgi` `center=x,y` in the picture's pixels with its
  `imagewidth` and `imageheight`. Pictures are JPEG or PNG, fetched with HTTP Digest like every other request; a
  camera that asks for Basic sign-in is refused with "turn on Digest sign-in on the camera", since Basic would send
  the password unprotected.

## 7. Instruments

- **I1 Header**: "No camera link", "Connecting to <address>…", or "<address> · <camera name>" with the units below it.
- **I2 Status chip**: OFFLINE, LINKING, CLOSING, REQUEST (a request is running), READS PAUSED, POSITION OK or
  NO POSITION, with a green, orange or muted lamp.
- **I3 Position plane** (pan across, tilt up), in camera units, in **Range** mode: a 10 × 10 grid, the zero axes when zero is in range,
  range labels, the last 120 measured positions as a trail (gaps stay gaps), the starting position as a square marked
  **S**, the target as a dashed crosshair and ring, and the current position as a solid reticle. Without ranges it
  reads "NO CAMERA LINK" or "PAN/TILT RANGE UNAVAILABLE"; with no measured position, "POSITION UNAVAILABLE".
- **I4 Aim on the plane.** Pointing at it shows guide lines and the pan, tilt value under the pointer. Click or drag to
  aim (fills the pan and tilt targets); double-click to aim and move.
- **I9 Range or Picture.** A switch at the right of the plane's caption, **Range | Picture** (also View ▸ Whole Range
  ⌘1 and View ▸ Camera Picture ⌘2), picks what the plane shows; the choice is remembered. Hover text: "Range: the
  camera's whole pan/tilt range. Click to aim, double-click to move (⌘1)" and "Picture: a still picture of what the
  camera sees. Click a point to aim at it, double-click to move the camera there (⌘2)".
- **I10 Camera picture.** In Picture mode the plane, captioned "Camera picture // view — click to aim, double-click to
  move there", shows a still picture of the camera's view, as large as fits and in the picture's shape, with a view
  grid on top: tenths of the picture, stronger centre lines, and a solid reticle at the centre, where the camera
  points. The window grows or shrinks in height to the picture's shape, keeping its top edge and staying on the screen,
  and gets its Range height back when switching to Range. A plate in the corner reads "PICTURE <time taken>". A picture
  is taken on connecting, on switching to Picture, with **Refresh Picture** (⇧⌘R, or the ⟳ button beside the switch,
  hover text "Refresh Picture: take a new picture of the camera's view now (⇧⌘R)"), and after every move or stop
  once the camera stops (a reading taken after the move that isn't MOVING, or 3 s when the camera doesn't report its
  motion). Until then the picture is dimmed and the plate reads "MOVING · NEW PICTURE WHEN IT STOPS". Without a
  picture the plane reads "NO CAMERA LINK", "TAKING A PICTURE…" or "NO PICTURE" with the reason; a failed picture is
  logged in red once.
- **I11 Aim in the picture.** Pointing at the picture shows guide lines and the point in view units (◀/▶ and ▲/▼,
  0 at the centre, 1 at the edges). Click to aim: a dashed crosshair and ring with a dotted line from the centre.
  Double-click (or Move, ⌘↩) turns the camera so that point comes to the centre, logged as "Move toward the picture
  point accepted: 0.45 right, 0.30 up of the picture (movement may still be in progress)". Typing in a target field
  drops the point, and an absolute target takes over. A camera that can't aim from its picture (see V6) shows "THIS
  CAMERA CAN'T AIM FROM ITS PICTURE" in the corner and ignores clicks.
- **I5 Axis travel meters** for pan, tilt and zoom: the bar's fill is the position as a percentage of the advertised
  limits. A missing reading is hatched and reads "N/A", never an empty bar. Ticks mark the start and the target, a
  small handle marks the target, and the limits are printed at both ends.
- **I6 Aim on a meter.** The pointer becomes a left-right resize cursor over it. Pointing shows the value under the
  pointer; click or drag to aim that axis alone; double-click to aim and move. Axis zoom snaps to whole numbers.
- **I7 Current coordinates panel**: PAN, TILT, ZOOM and MOTION in the camera's own digits (or —), each with its
  advertised range ("range not advertised", "· integer" for Axis zoom); MOTION says "not reported by camera" when the
  camera sends no status. Below them, the starting coordinates.
- **I8 Footer**: the route (`LOCAL` or `SSH user@host:port`), the camera address and port, the refresh state and
  busy/ready; while disconnected, "credentials stay in memory only · Return connects".

## 8. Target and moves

- **M1 Target fields** for pan, tilt and zoom (placeholder "Pan: unchanged" and so on). They are absolute positions,
  not increments. A blank field or `x` keeps that axis. Typing updates the target markers on the plane and meters.
- **M2 Move** (⌘↩, the Move button, Return in a target field, or a double-click on the plane or a meter). An invalid
  number beeps and is logged in red; all axes blank logs "All axes unchanged; no move sent." Success is logged as
  "Move accepted: pan=…, tilt=… (movement may still be in progress)" and the position is read again at once.
- **M3 Return to Starting Coordinates** (⌘B, the Return to Start button) moves back to the first reading after
  connecting. It fills the target fields with those values; axes the camera didn't report stay unchanged and are
  named in the log. Disabled until a starting position exists.
- **M4 Stop Motion** (⌘., the red Stop button) sends a PTZ stop.
- **M5 Queueing.** A move sent while another request runs waits behind it, and the log says so.

## 9. Position refresh

- **R1 Read-only refresh** of the current position every 2 seconds while connected. Changes are logged as "Current
  coordinates: …".
- **R2 Starting coordinates** are the first reading after connecting and don't change until the next connection.
- **R3 Refresh Position** (⌘R) reads now and resumes a paused refresh.
- **R4 Pause Auto-Refresh / Resume Auto-Refresh** (⌘P).
- **R5 Pause after failures.** After 3 failed reads in a row the refresh pauses, and the log says to press ⌘R.

## 10. Camera web interface

- **W1 Open Camera Web Interface** (⌘L, the Web Interface button) opens the camera's own page in the default browser.
  It tries HTTP first, then HTTPS on the HTTPS port, follows redirects only within the camera's host, and sends no
  credentials. A self-signed certificate is accepted for finding the page, with the notice "Verify the camera
  certificate in your browser." Through SSH the browser gets the forwarded local address.
- **W2 Fallback.** If no browser opens, a dialog shows the address to open by hand.

## 11. Event buffer

- **E1 Session log**: timestamped (HH:mm:ss), monospaced, selectable and copyable; warnings and failures in red. It
  keeps the last 500 lines.
- **E2 Scrolling.** New lines scroll it to the end only while you're already at the end.

## 12. Appearance

- **A1 Phosphor** (View ▸ Phosphor): System (⌥⌘1), Green Phosphor (⌥⌘2), Amber Phosphor (⌥⌘3), Blue Phosphor (⌥⌘4),
  Mono (⌥⌘5). The chosen one is ticked and remembered.
- **A2 System** uses the system's colors and follows its light or dark setting, also while the app is running. The
  phosphor themes are dark, with a CRT-style monochrome color for every instrument and text.
- **A3 Fonts.** The system monospaced font for values, captions and the log.

## 13. Menus, help and About

- **H1 Main Menu** (☰), a framed button at the right end of the main window's header (after the status chip) and of
  Manage Logins' top row, on every platform, holds every menu, each as a submenu, and opens right-aligned under the
  button: PTZ Control (About, Hide, Hide Others, Show All, Quit), Edit (Undo, Redo, Cut, Copy, Paste, Select All),
  Camera (Connect, Disconnect, Move to Target, Return to Starting Coordinates, Stop Motion, Refresh Position, Pause
  Auto-Refresh, Refresh Picture, Open Camera Web Interface, Manage Logins), View (Whole Range, Camera Picture,
  Phosphor), Window (Minimize, Zoom, Close), Help
  (Coordinate Help). Camera commands are enabled only when they can act. Hover text: "Main Menu: every command of PTZ
  Control, with its shortcut"; VoiceOver reads "Main Menu". On the Mac the menu bar keeps only the app menu macOS
  always shows (About, Services, Hide, Hide Others, Show All, Quit), so on a wide screen no command needs a trip to the
  top of the screen. Every shortcut works from any window, Cut, Copy and Paste in text fields included.
- **H2 Coordinate Help** (⌘?) explains absolute targets, the units per vendor, blank or `x`, aiming on the plane and
  meters, starting coordinates, the 2 s refresh, Stop and the web interface.
- **H3 About** shows the name, the version, "PTZ Coordinates Controller", "ONVIF · Axis VAPIX · Local or SSH",
  "By dunkelherzmann" and a link to the project on GitHub.

## 14. Windows and Linux

What differs from the Mac version today. The `pending.md` in each folder says which of these are gaps to close.

**Both**

- Saved logins (§17) are kept in `%APPDATA%\PTZControl\logins.vault` on Windows and in
  `~/.var/app/org.ptzcontrol.app/data/PTZControl/logins.vault` on Linux. Manage Logins is in the ☰ menu's Camera menu
  (Ctrl+Shift+L). On Windows the saved-login dialogs and Manage Logins use Windows' standard dialog look.
- The ☰ Main Menu holds the Mac's menus in the Mac's order, with Ctrl for ⌘ and Alt for ⌥: PTZ Control (About,
  Quit PTZ Control Ctrl+Q; Hide, Hide Others and Show All are macOS-only), Edit (Undo, Redo, Cut, Copy, Paste, Select
  All, acting on the field that has the focus), Camera, View ▸ Phosphor, Window (Minimize Ctrl+M, Maximize, Close
  Ctrl+W) and Help (Coordinate Help, F1 here for ⌘?).
- Default size 1180 × 820. The window's size and position aren't remembered.
- No camera picture yet (I9–I11): the plane always shows the whole range.
- The Advanced fields are always shown, without a disclosure triangle.
- Enter in any connection field connects while nothing is connected.
- The event buffer always scrolls to the newest line.
- SSH confirmations and the changed host key question default to **No**.
- About opens as a dialog with the version and a clickable project link.

**Windows**

- Smallest size 1020 × 780; per-monitor DPI aware.
- Fonts: Cascadia Mono, or Consolas where it's missing; Segoe UI for labels. Buttons and panels are drawn by the app.
- System follows the Windows app light/dark setting, also while running, and darkens the title bar.
- Settings live under `HKCU\Software\PTZControl`.
- SSH uses Windows' own OpenSSH Client (`C:\Windows\System32\OpenSSH\ssh.exe`, then any `ssh` on PATH). Prompts are
  relayed over a named pipe only the signed-in user can open. Password and passphrase dialogs need Win32-OpenSSH 8.6
  or later; the 8.1 build in Windows 10 can't relay them, so use a key without a passphrase or ssh-agent there.
- Windows OpenSSH has no connection sharing, so the HTTPS port set under Advanced is forwarded when the tunnel opens,
  and a camera that redirects to another HTTPS port can't be reached through SSH.
- The key file field takes a full path (placeholder `C:\Users\me\.ssh\id_ed25519`).
- Manage Logins is modal, so it has no ☰ of its own: the main window waits while it's open. Redo is greyed in
  Windows' plain text fields, which keep a single undo step.

**Linux**

- Smallest size 1000 × 740. Qt's Fusion style.
- Fonts: the bundled JetBrains Mono, or the system's `monospace`.
- System takes the desktop's light or dark setting once, at launch (from the desktop portal, `GTK_THEME`, GNOME's
  settings or KDE's colors).
- Windows open through X11 (XWayland on Wayland desktops), falling back to native Wayland when there is no X server;
  `QT_QPA_PLATFORM=wayland` forces it.
- Settings live in `~/.var/app/org.ptzcontrol.app/config/PTZControl/PTZControl.conf`.
- SSH uses the Flatpak runtime's OpenSSH client with the user's `~/.ssh` and ssh-agent.
- Every shortcut works in Manage Logins too.

## 15. Build, packaging and versions

Each version is built on its own kind of machine. Nothing is cross-compiled.

- **B1 macOS** (built on a Mac with the Xcode Command Line Tools): `macos/scripts/build.sh` builds a universal
  (arm64 + x86_64) `dist/PTZ Control.app` for macOS 14 or later with Swift Package Manager, draws its icon with
  `macos/scripts/make-icon.swift`, and signs it ad hoc with the hardened runtime. Bundle ID `com.herbertcisneros.PTZControl`.
  It declares a Local Network usage description. `--zip` also writes `dist/PTZ-Control-<version>-macOS.zip`. The app
  isn't notarized, so a downloaded copy needs its quarantine flag removed once (README, Installing).
- **B2 Windows** (built on Windows with the Visual Studio 2022 C++ Build Tools): `windows/scripts/build.ps1` compiles
  the core and the host with MSVC into one statically linked `dist\PTZControl.exe` (x64, about 0.9 MB) that imports
  only Windows system DLLs. No installer; it runs as the signed-in user (`asInvoker`). `-Zip` also writes
  `dist\PTZ-Control-<version>-Windows-x64.zip`. Every build is also copied to the desktop as `PTZ Control.exe`,
  replacing the older copy (skipped with a warning while that copy is open). The exe is unsigned, so SmartScreen
  warns on first run.
- **B3 Linux** (built on Linux with flatpak-builder): `linux/scripts/build.sh` builds `org.ptzcontrol.app` from
  `linux/org.ptzcontrol.app.yml` on KDE's Qt 6 runtime (`org.kde.Platform` 6.11) for the machine's own CPU and writes
  `dist/PTZ-Control-<version>-<arch>.flatpak` (about 0.6 MB). The bundle holds the app, JetBrains Mono, the icons,
  the desktop entry, the AppStream metadata and the licence notices; Qt, libcurl, OpenSSL and OpenSSH come from the
  runtime. The sandbox gets the network, X11 and Wayland, the GPU, `~/.ssh` and the ssh-agent socket. `--install`
  also installs it for the current user.
- **B4 Releases** carry the macOS zip, the Windows zip and the Linux Flatpak.
- **B5 Versions** are `major.minor.patch`: `VERSION` holds major.minor, and the patch is the number of commits since
  `VERSION` last changed. `scripts/version.sh` prints it and, with `--header`, writes `core/include/ptz_version.h`
  (not tracked); `windows\scripts\build.ps1` does the same in PowerShell.

## 16. Tests

- **T1 Mock camera.** `tools/mock-camera.py` answers ONVIF and Axis requests, for work without hardware.
- **T2 Core test.** `scripts/test-core.sh` builds `tests/core-smoke.c` against the core and runs it against the mock
  camera, for ONVIF and Axis: connect, read, move, a tilt-only move, out-of-range and fractional-zoom refusals, stop,
  the web address, number parsing and address checks. It needs clang and libcurl (macOS or Linux).
- **T3 SSH test.** `scripts/test-ssh.sh` starts a throwaway user-level `sshd` on 127.0.0.1:2222 with a
  passphrase-protected key and runs `tests/ssh-smoke.c` through it: tunnel, askpass relay, read, move, web address.
  It backs up `~/.ssh/known_hosts` and restores it afterwards.
- **T4 Window snapshot.** With `PTZ_SNAPSHOT_PATH` (and `PTZ_PASSWORD`, optionally `PTZ_THEME`) the Mac and Linux
  apps connect with the saved settings, move, aim along the zoom meter, save a picture of the window and quit. The
  Linux app returns to the start first when `PTZ_SNAPSHOT_RETURN` is set.
- **T6 Mac app test.** `macos/scripts/test.sh` runs a private test copy of the Mac app (its own bundle id, settings
  and vault file, off screen and out of the Dock) against the mock camera twice: first it ticks Save login and checks
  the camera login reached the vault, then it starts from a wrong username, fills the fields from the saved login and
  checks that the camera moved. It saves pictures of the window, of Manage Logins and of the Edit form.
  `linux/scripts/test.sh` does the same with the installed Flatpak (T5).
- **T7 Windows test.** `windows\scripts\test.ps1` builds and runs `tests/vault-smoke.c` with MSVC, then runs
  `dist\PTZControl.exe` twice as the Mac test does, with its own registry key (`PTZ_SETTINGS_KEY`) and vault file,
  without taking focus. Pictures are taken with `WM_PRINT`, so the test also works over SSH, where no compositor runs;
  it fails on a blank picture. Combo boxes leave their text out of such pictures, so the snapshot logs what the Login
  menus show. `test-core.sh` also runs
  `tests/vault-smoke.c`: plain and encrypted files, both stores, wrong and changed passwords, tampering.
- **T5 Linux app test.** `linux/scripts/test.sh` runs the installed Flatpak off screen against the mock camera with
  its own settings folder, and passes when the window picture is saved and the camera saw the app's requests.

## 17. Saved logins

- **P1 Two stores of named logins.** Saved SSH logins and saved camera logins are each a **name**, a username and its
  password, so one username can be saved with different passwords (for example `cam1` and `cam2`, both `admin`).
  Hosts, addresses, ports and key files aren't saved (they stay in the remembered settings, F7). The two lists are kept
  apart: any SSH login can be used with any camera login, and the Local route needs no SSH login. Names are unique
  within a list, ignoring case; a login with the same name replaces the older one. Lists are sorted by name. A file
  saved before names existed opens with each login named after its username.
- **P2 One file, one Administrator Password.** The core keeps both lists in one file, `logins.vault` (on the Mac in
  `~/Library/Application Support/PTZControl/`, mode 0600), in the same format on every platform; it's replaced
  atomically on each change. With an Administrator Password the whole file is encrypted with XChaCha20-Poly1305
  (Monocypher) under a key derived from the password with Argon2id (64 MiB, 3 passes, random 16-byte salt), so the
  names and usernames are hidden too. The Administrator Password itself is never stored and can't be recovered. Without one,
  logins are plain text, only by explicit choice. The macOS Keychain isn't used.
- **P3 Locked at launch.** An encrypted vault is unlocked once per session, when first needed: Unlock Saved Logins…
  in a Login menu, or opening Manage Logins. "Unlock Saved Logins" asks for the Administrator Password, with Unlock and
  Cancel; a wrong one says so and asks again.
- **P4 Login menus.** A **Login** pop-up heads the SSH host fields and the Camera fields: "Type a login", the saved
  logins by name, and Manage Logins… (while locked, Unlock Saved Logins… in place of the list). Choosing a login fills that
  section's Username and Password; typing in either returns the menu to "Type a login". Hover: "SSH login — Fill the
  SSH username and password from a saved SSH login, or open Manage Logins (⇧⌘L)." and "Camera login — Fill the camera
  username and password from a saved camera login, or open Manage Logins (⇧⌘L)."
- **P5 Save login.** A checkbox under each section's password, off at first. Once the connection succeeds, "Save Camera
  Login" (or SSH) asks for the login's name, suggesting the name of the login chosen in the Login menu or else the
  username, with Save and Don't Save; saving under an existing name updates that login. The event buffer says "Saved
  camera login cam1." (or SSH), and the boxes clear. A failed connection saves nothing. Hover: "Save login — Once connected, save this camera username
  and password to the saved camera logins." (and the same for SSH).
- **P6 First save.** While nothing has ever been saved, saving asks "Protect Saved Logins": it explains that an
  Administrator Password encrypts saved logins, is asked once per launch and can't be recovered, and that plain text
  can be read by anyone who can read your files. It offers **Create Administrator Password…** (the default), **Not
  Now** and **Store as Plain Text…**, which asks again with Cancel as the default. Nothing is saved until one is
  chosen.
- **P7 Manage Logins** (Camera ▸ Manage Logins…, ⇧⌘L, or the Login menus) is a window with **SSH Logins** and
  **Camera Logins**. Opened from a Login menu it shows that menu's list: SSH Logins from the SSH host's menu, Camera
  Logins from the camera's. From Camera ▸ Manage Logins… it shows the list last open (SSH Logins at first). Each list
  is a list with the columns Name, Username and Password (hidden).
  - **New…**, **Edit…** (or double-click) open a form with Name, Username, Password and **Show password**; a name and
    a username are required.
  - **Delete** (or ⌫) removes the selected login after a confirmation whose default is Cancel.
  - **Lock** locks the saved logins again; **Unlock…** asks for the Administrator Password.
  - **Create Administrator Password…** (or **Change Administrator Password…** once one is set) asks for the new one
    twice and encrypts every login again. Leaving both blank removes it after a confirmation (Cancel by default), and
    logins are then plain text.
  - A status line reads "Encrypted with the Administrator Password.", "Stored as plain text.", "Locked." or "No logins
    saved yet."; **Done** (Esc) closes the window. Every button has hover text.
- **P8 Unreadable file.** If the vault file can't be read (damaged, or from a newer version), it's left untouched,
  the event buffer says why in red, and the Login menus show "Saved logins unavailable".
