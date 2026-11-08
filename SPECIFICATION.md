# PTZ Control

PTZ Control reads and sets absolute pan, tilt and zoom on PTZ cameras: ONVIF cameras (Bosch, Milesight and others) and
Axis cameras through VAPIX. It reaches the camera directly or through an SSH host. Connecting never moves the camera;
only Move, Return to Start and Stop do.

One shared C++20 core does all camera, HTTP and SSH work. Each platform has its own native window around it:

| Version | Host | Folder | Its plan |
| --- | --- | --- | --- |
| macOS (the reference) | Swift and AppKit | `macos/` | [`pending.md`](pending.md) |
| Windows 10 and 11, x64 | Win32 and GDI+, C++ | `windows/` | [`windows/pending.md`](windows/pending.md) |
| Linux, as a Flatpak | Qt 6 Widgets, C++ | `linux/` | [`linux/pending.md`](linux/pending.md) |

Sections 1–13 describe the Mac app. Windows and Linux follow it unless section 14 says otherwise. Every feature has an
id (for example **M3**), which the `pending.md` files use.

**Shortcuts.** The Mac shortcuts are given here. Windows and Linux use Ctrl in place of ⌘ and Alt in place of ⌥, and
hold the menu under a ☰ button in the window instead of a menu bar.

**Hover text.** No control has hover text on any platform yet. Each `pending.md` lists that as a gap.

## 1. Core

- **C1 One C interface.** `core/include/ptzcore.h` is the only thing the hosts call: connect, camera info, read
  position, move, stop, find the web interface, check the link, close. All calls block. Each host runs them on one
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

- **K1 One window**, "PTZ Control", subtitle "PTZ Coordinates Controller by Herbert Cisneros". Default size
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
  launch. Passwords stay in memory only.
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

## 7. Instruments

- **I1 Header**: "No camera link", "Connecting to <address>…", or "<address> · <camera name>" with the units below it.
- **I2 Status chip**: OFFLINE, LINKING, CLOSING, REQUEST (a request is running), READS PAUSED, POSITION OK or
  NO POSITION, with a green, orange or muted lamp.
- **I3 Position plane** (pan across, tilt up), in camera units: a 10 × 10 grid, the zero axes when zero is in range,
  range labels, the last 120 measured positions as a trail (gaps stay gaps), the starting position as a square marked
  **S**, the target as a dashed crosshair and ring, and the current position as a solid reticle. Without ranges it
  reads "NO CAMERA LINK" or "PAN/TILT RANGE UNAVAILABLE"; with no measured position, "POSITION UNAVAILABLE".
- **I4 Aim on the plane.** Pointing at it shows guide lines and the pan, tilt value under the pointer. Click or drag to
  aim (fills the pan and tilt targets); double-click to aim and move.
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

- **H1 Menu bar**: PTZ Control (About, Services, Hide, Hide Others, Show All, Quit), Edit (Undo, Redo, Cut, Copy,
  Paste, Select All), Camera (Connect, Disconnect, Move to Target, Return to Starting Coordinates, Stop Motion, Refresh
  Position, Pause Auto-Refresh, Open Camera Web Interface), View (Phosphor), Window (Minimize, Zoom, Close), Help
  (Coordinate Help). Camera commands are enabled only when they can act.
- **H2 Coordinate Help** (⌘?) explains absolute targets, the units per vendor, blank or `x`, aiming on the plane and
  meters, starting coordinates, the 2 s refresh, Stop and the web interface.
- **H3 About** shows the name, the version, "PTZ Coordinates Controller", "ONVIF · Axis VAPIX · Local or SSH",
  "By dunkelherzmann" and a link to the project on GitHub.

## 14. Windows and Linux

What differs from the Mac version today. The `pending.md` in each folder says which of these are gaps to close.

**Both**

- The menu is a ☰ button: Camera, Phosphor, Coordinate Help (F1) and About. There is no Edit, View or Window menu.
- Default size 1180 × 820. The window's size and position aren't remembered.
- The Advanced fields are always shown, without a disclosure triangle.
- Enter in any connection field connects while nothing is connected.
- The event buffer always scrolls to the newest line.
- SSH confirmations and the changed host key question default to **No**.
- About opens as a dialog with the version and a clickable project link.

**Windows**

- Smallest size 1020 × 780; per-monitor DPI aware. Title "PTZ Control — PTZ Coordinates Controller by Herbert
  Cisneros".
- Fonts: Cascadia Mono, or Consolas where it's missing; Segoe UI for labels. Buttons and panels are drawn by the app.
- System follows the Windows app light/dark setting, also while running, and darkens the title bar.
- Settings live under `HKCU\Software\PTZControl`.
- SSH uses Windows' own OpenSSH Client (`C:\Windows\System32\OpenSSH\ssh.exe`, then any `ssh` on PATH). Prompts are
  relayed over a named pipe only the signed-in user can open. Password and passphrase dialogs need Win32-OpenSSH 8.6
  or later; the 8.1 build in Windows 10 can't relay them, so use a key without a passphrase or ssh-agent there.
- Windows OpenSSH has no connection sharing, so the HTTPS port set under Advanced is forwarded when the tunnel opens,
  and a camera that redirects to another HTTPS port can't be reached through SSH.
- The key file field takes a full path (placeholder `C:\Users\me\.ssh\id_ed25519`).
- There is no Quit command; close the window (Alt+F4).

**Linux**

- Smallest size 1000 × 740. Title "PTZ Control — PTZ Coordinates Controller by Herbert Cisneros". Qt's Fusion style.
- Fonts: the bundled JetBrains Mono, or the system's `monospace`.
- System takes the desktop's light or dark setting once, at launch (from the desktop portal, `GTK_THEME`, GNOME's
  settings or KDE's colors).
- Windows open through X11 (XWayland on Wayland desktops), falling back to native Wayland when there is no X server;
  `QT_QPA_PLATFORM=wayland` forces it.
- Settings live in `~/.var/app/org.ptzcontrol.app/config/PTZControl/PTZControl.conf`.
- SSH uses the Flatpak runtime's OpenSSH client with the user's `~/.ssh` and ssh-agent.
- Quit is in the ☰ menu (Ctrl+Q).

## 15. Build, packaging and versions

Each version is built on its own kind of machine. Nothing is cross-compiled.

- **B1 macOS** (built on a Mac with the Xcode Command Line Tools): `scripts/build-macos.sh` builds a universal
  (arm64 + x86_64) `dist/PTZ Control.app` for macOS 14 or later with Swift Package Manager, draws its icon with
  `scripts/make-icon.swift`, and signs it ad hoc with the hardened runtime. Bundle ID `com.herbertcisneros.PTZControl`.
  It declares a Local Network usage description. `--zip` also writes `dist/PTZ-Control-<version>-macOS.zip`. The app
  isn't notarized, so a downloaded copy needs its quarantine flag removed once (README, Installing).
- **B2 Windows** (built on Windows with the Visual Studio 2022 C++ Build Tools): `scripts/build-windows.ps1` compiles
  the core and the host with MSVC into one statically linked `dist\PTZControl.exe` (x64, about 0.9 MB) that imports
  only Windows system DLLs. No installer; it runs as the signed-in user (`asInvoker`). `-Zip` also writes
  `dist\PTZ-Control-<version>-Windows-x64.zip`. The exe is unsigned, so SmartScreen warns on first run.
- **B3 Linux** (built on Linux with flatpak-builder): `scripts/build-linux.sh` builds `org.ptzcontrol.app` from
  `linux/org.ptzcontrol.app.yml` on KDE's Qt 6 runtime (`org.kde.Platform` 6.11) for the machine's own CPU and writes
  `dist/PTZ-Control-<version>-<arch>.flatpak` (about 0.6 MB). The bundle holds the app, JetBrains Mono, the icons,
  the desktop entry, the AppStream metadata and the licence notices; Qt, libcurl, OpenSSL and OpenSSH come from the
  runtime. The sandbox gets the network, X11 and Wayland, the GPU, `~/.ssh` and the ssh-agent socket. `--install`
  also installs it for the current user.
- **B4 Releases** carry the macOS zip, the Windows zip and the Linux Flatpak.
- **B5 Versions** are `major.minor.patch`: `VERSION` holds major.minor, and the patch is the number of commits since
  `VERSION` last changed. `scripts/version.sh` prints it and, with `--header`, writes `core/include/ptz_version.h`
  (not tracked); `build-windows.ps1` does the same in PowerShell.

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
- **T5 Linux app test.** `scripts/test-linux.sh` runs the installed Flatpak off screen against the mock camera with
  its own settings folder, and passes when the window picture is saved and the camera saw the app's requests.
