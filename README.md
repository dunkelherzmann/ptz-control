# PTZ Control

A native app to read and set absolute pan/tilt/zoom on ONVIF (Bosch, Milesight, …) and Axis VAPIX cameras,
either directly or through an SSH host.

Built like [TMOG](https://tmog.org/): one shared C++20 core and one native UI per
platform, without a web view or a Python runtime.

```
core/              shared C++20 core (C ABI in core/include/ptzcore.h)
  src/onvif.cpp      ONVIF SOAP 1.2 + WS-Security digest, clock sync with the camera
  src/axis.cpp       Axis VAPIX ptz.cgi with HTTP Digest
  src/tunnel_posix   SSH tunnel via system OpenSSH, prompts relayed to the UI (askpass)
  src/tunnel_win     same on Windows: inbox ssh.exe, askpass over a per-user named pipe
  src/http_*.cpp     libcurl (macOS/Linux) or WinHTTP (Windows)
  src/web.cpp        camera web-interface probe (HTTP, then HTTPS), no credentials sent
macos/             Swift + AppKit host, its build and test scripts
windows/           Win32 + GDI+ host (C++), resources, manifest, build and test scripts
linux/             Qt 6 Widgets host (C++), its Flatpak manifest, build and test scripts
legacy/            the original ptz-control.sh
tools/             mock camera (ONVIF + Axis) for development without hardware
tests/, scripts/   core smoke tests and the scripts for every platform (version, core and SSH tests)
```

## Installing

The apps aren't signed by Apple or Microsoft, so there are two ways to run them: build them yourself, or
use the prebuilt files from [Releases](https://github.com/dunkelherzmann/ptz-control/releases) and allow
them once.

### Build from source

```bash
git clone https://github.com/dunkelherzmann/ptz-control.git
cd ptz-control
macos/scripts/build.sh      # macOS: needs only the free Command Line Tools (xcode-select --install)
```

Windows and Linux builds are described below. An app built on your own Mac isn't quarantined, so
Gatekeeper opens it without any warning.

### Prebuilt apps

**macOS.** Unzip `PTZ-Control-…-macOS.zip` and drag **PTZ Control.app** to your Applications folder.
Downloaded apps carry macOS's quarantine flag, and because this one isn't notarized, macOS refuses to open
it at first. Remove the flag once in Terminal:

```bash
xattr -dr com.apple.quarantine "/Applications/PTZ Control.app"
```

- `-d com.apple.quarantine` removes only the quarantine flag, and `-r` does it for everything inside the app.
- `xattr -cr "/Applications/PTZ Control.app"` also works: `-c` clears all extended attributes, the
  quarantine flag among them.
- `-c` and `-d` can't be combined (`xattr -cdr` stops with "Cannot specify -d with -c"); use one or the other.

Without Terminal: try to open the app once, then click **Open Anyway** in **System Settings ▸ Privacy &
Security** (this needs an administrator password). Since macOS 15, Control-click ▸ Open no longer bypasses
the warning.

**Windows.** SmartScreen warns on first run: choose **More info ▸ Run anyway**.

**Linux.** Install the bundle with `flatpak install --user PTZ-Control-…flatpak`; it appears in your
applications menu.

Only allow copies you downloaded from this repository's Releases page.

## Build (macOS 14+)

Needs only the Xcode Command Line Tools.

```bash
macos/scripts/build.sh          # → dist/PTZ Control.app (universal arm64 + x86_64)
macos/scripts/build.sh --zip    # also dist/PTZ-Control-1.0.0-macOS.zip
```

The app is ad-hoc signed. It runs on the Mac that built it; a copy given to
someone else needs its quarantine flag removed once (see [Installing](#installing)).

## Use

1. Choose the route: **Local** (camera reachable from this Mac) or **SSH Host**.
2. Choose the vendor: **Bosch/Milesight** (ONVIF) or **Axis** (VAPIX).
3. Enter the camera details and **Connect** (⌘K). Connecting never moves the camera.

| Action | How |
| --- | --- |
| Aim | click the position plane, drag a travel meter, or type pan/tilt/zoom targets |
| Move | Return in a target field, **Move**, ⌘↩, or double-click the plane or a meter |
| Aim in the camera's picture | View ▸ Camera Picture (⌘2) or **Picture** above the plane, then click a point; double-click to move there (macOS) |
| New picture / whole range again | ⇧⌘R / ⌘1 |
| Keep an axis | leave its field blank (or `x`) |
| Return to starting coordinates | ⌘B |
| Stop motion | ⌘. |
| Refresh / pause the 2 s read-only refresh | ⌘R / ⌘P |
| Open the camera's own web page | ⌘L |
| Phosphor (System, Green, Amber, Blue, Mono) | View ▸ Phosphor, ⌥⌘1–5 |

Readings are shown in the camera's own digits; a value the camera did not
report is shown as `—` or a hatched meter, never as zero.

Non-secret connection settings are remembered. Passwords stay in memory only,
unless you tick **Save login**: the username and password are then saved, in
two separate lists for SSH and cameras, encrypted with an Administrator
Password you choose (or, by explicit choice, as plain text). Pick a saved login
from the **Login** menu above the SSH or camera fields, and edit them in
☰ ▸ Camera ▸ Manage Logins (⇧⌘L; Ctrl+Shift+L on Linux).
A forgotten Administrator Password can't be recovered. The macOS Keychain
isn't used. `PTZ_USER` and `PTZ_PASSWORD` are honoured as defaults when set.

### SSH route

Uses the system `ssh` (no `sshpass`). The Host Password answers the first SSH
password prompt; key passphrases, MFA codes and further prompts appear as
dialogs. Your `~/.ssh/config`, agent and keys work as usual, and an optional
key file can be set. New host keys are accepted on first use. If a host key
*changes*, the app shows the new fingerprint and asks before replacing it
(`ssh-keygen` keeps a `.old` backup).

## Test

```bash
scripts/test-core.sh   # core C API vs. the mock camera, ONVIF and Axis; the login vault
macos/scripts/test.sh  # a private test copy of the Mac app vs. the mock camera, off screen; saved logins
scripts/test-ssh.sh    # SSH route via a throwaway user-level sshd on 127.0.0.1:2222
python3 tools/mock-camera.py --port 8080 --user admin --password secret
```

`test-ssh.sh` backs up `~/.ssh/known_hosts` and restores it afterwards.

## Build for Windows (10/11, x64)

Built on Windows with the Visual Studio 2022 C++ Build Tools (the free
"Desktop development with C++" workload) and git, from PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File windows\scripts\build.ps1        # → dist\PTZControl.exe
powershell -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Zip   # also dist\PTZ-Control-1.0.0-Windows-x64.zip
powershell -ExecutionPolicy Bypass -File windows\scripts\test.ps1         # vault test, then a private test copy vs. the mock camera
```

The result is a single statically linked `PTZControl.exe` (about 0.9 MB) that
imports only Windows system DLLs. Each build is also copied to the desktop as
`PTZ Control.exe`, replacing the older copy. Copy it anywhere and run it: there is no
installer, it runs as the signed-in user (`asInvoker`), and settings live
under `HKCU\Software\PTZControl`. Shortcuts use Ctrl in place of ⌘; the ☰
button holds the menu.

- **Local route:** no extra requirements.
- **SSH route:** uses Windows' own OpenSSH Client
  (`C:\Windows\System32\OpenSSH\ssh.exe`, installed by default on Windows 11,
  optional on Windows 10). Password and passphrase dialogs rely on
  `SSH_ASKPASS` support, which arrived in Win32-OpenSSH 8.6. The 8.1 build that
  ships with Windows 10 lacks it, so use a key without a passphrase or
  ssh-agent there.
- Windows OpenSSH has no connection multiplexing, so the camera HTTPS port
  (Advanced ▸ HTTPS port) is forwarded when the tunnel opens.
- The exe is unsigned, so SmartScreen warns on first run ("More info ▸ Run
  anyway"). Sign it with a code-signing certificate to avoid that.

## Build for Linux (Flatpak)

Built on Linux with flatpak-builder (`linux/org.ptzcontrol.app.yml`) on KDE's Qt 6 runtime from Flathub.
One-time setup on Ubuntu or Debian:

```bash
sudo apt install flatpak
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.flatpak.Builder
```

Then:

```bash
linux/scripts/build.sh             # → dist/PTZ-Control-1.0.0-x86_64.flatpak
linux/scripts/build.sh --install   # also installs it for this user
linux/scripts/test.sh              # drives the installed app against the mock camera, off screen; saved logins
```

The script builds for the machine's own CPU: on an ARM64 Linux machine it makes
`PTZ-Control-…-aarch64.flatpak`.

The bundle (about 0.6 MB) holds the app, the JetBrains Mono font, icons, the desktop entry and the licence
notices. Qt 6, libcurl, OpenSSL and the OpenSSH client come from the `org.kde.Platform` runtime, which
Flatpak installs from Flathub with the app and keeps updated. It runs on any distribution with Flatpak and
needs no root:

```bash
flatpak install --user PTZ-Control-1.0.0-x86_64.flatpak
flatpak run org.ptzcontrol.app
```

- The sandbox allows the network, the display (X11 and Wayland), the GPU, `~/.ssh` and the ssh-agent
  socket, and nothing else of the system.
- Windows open through X11 (XWayland on Wayland desktops), falling back to
  native Wayland when no X server exists; `QT_QPA_PLATFORM=wayland` forces it.
- Settings live in `~/.var/app/org.ptzcontrol.app/config/PTZControl/PTZControl.conf`. Shortcuts use Ctrl
  in place of ⌘; the ☰ button holds the menu.
- **SSH route:** uses the runtime's OpenSSH client with your `~/.ssh` keys, config and `known_hosts`, and
  your ssh-agent.

## Versioning

Versions are `major.minor.patch`. [`VERSION`](VERSION) holds the major and minor number, and the patch number is the count of commits since `VERSION` last changed, so it rises with every push to `main`. Editing `VERSION` (to `1.1`, say) starts the next series at `1.1.0`. `scripts/version.sh` prints the current version, and the builds use it.

## Open source

PTZ Control is free and open source under the [MIT License](LICENSE): use it, change it and share it, for personal or commercial work. Keep the copyright and license notice with any copy. Third-party components keep their own licenses; see [NOTICES](NOTICES).

## Support

PTZ Control is a testing app, shared publicly so others can benefit from it. It is provided as is: no support is promised or given.

What has been reported and what's done is tracked in the open: [bugs.md](bugs.md) and [enhancements.md](enhancements.md).

## Contributing

Bug reports, ideas and pull requests are welcome: open an [issue](https://github.com/dunkelherzmann/ptz-control/issues) or a pull request here. Contributions are accepted under the same MIT License.

## Contact

Contact me through my GitHub profile: [dunkelherzmann](https://github.com/dunkelherzmann).

Copyright © 2026 Herbert Cisneros, developed with AI coding assistance.
