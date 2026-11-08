# PTZ Control

A native app to read and set absolute pan/tilt/zoom on ONVIF (Bosch, Milesight, …) and Axis VAPIX cameras,
either directly or through an SSH host.

Built like [TMOG](https://tmog.org/): one shared C++20 core, one native UI per
platform, no web view, no Python runtime.

```
core/              shared C++20 core (C ABI in core/include/ptzcore.h)
  src/onvif.cpp      ONVIF SOAP 1.2 + WS-Security digest, clock sync with the camera
  src/axis.cpp       Axis VAPIX ptz.cgi with HTTP Digest
  src/tunnel_posix   SSH tunnel via system OpenSSH, prompts relayed to the UI (askpass)
  src/tunnel_win     same on Windows: inbox ssh.exe, askpass over a per-user named pipe
  src/http_*.cpp     libcurl (macOS/Linux) or WinHTTP (Windows)
  src/web.cpp        camera web-interface probe (HTTP, then HTTPS), no credentials sent
macos/             Swift + AppKit host
windows/           Win32 + GDI+ host (C++), resources and manifest
linux/             Qt 6 Widgets host (C++), AppImage packaging and its build container
legacy/            the original ptz-control.sh
tools/             mock camera (ONVIF + Axis) for development without hardware
tests/, scripts/   smoke tests and the build scripts
```

## Installing

The apps aren't signed by Apple or Microsoft, so there are two ways to run them: build them yourself, or
use the prebuilt files from [Releases](https://github.com/dunkelherzmann/ptz-control/releases) and allow
them once.

### Build from source

```bash
git clone https://github.com/dunkelherzmann/ptz-control.git
cd ptz-control
scripts/build-macos.sh      # macOS: needs only the free Command Line Tools (xcode-select --install)
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

**Linux.** Make the AppImage executable (`chmod +x PTZ-Control-…AppImage`) and run it.

Only allow copies you downloaded from this repository's Releases page.

## Build (macOS 14+)

Needs only the Xcode Command Line Tools.

```bash
scripts/build-macos.sh          # → dist/PTZ Control.app (universal arm64 + x86_64)
scripts/build-macos.sh --zip    # also dist/PTZ-Control-1.0.0-macOS.zip
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
| Keep an axis | leave its field blank (or `x`) |
| Return to starting coordinates | ⌘B |
| Stop motion | ⌘. |
| Refresh / pause the 2 s read-only refresh | ⌘R / ⌘P |
| Open the camera's own web page | ⌘L |
| Phosphor (System, Green, Amber, Blue, Mono) | View ▸ Phosphor, ⌥⌘1–5 |

Readings are shown in the camera's own digits; a value the camera did not
report is shown as `—` or a hatched meter, never as zero.

Passwords stay in memory only. Non-secret connection settings are remembered.
`PTZ_USER` and `PTZ_PASSWORD` are honoured as defaults when set.

### SSH route

Uses the system `ssh` (no `sshpass`). The Host Password answers the first SSH
password prompt; key passphrases, MFA codes and further prompts appear as
dialogs. Your `~/.ssh/config`, agent and keys work as usual, and an optional
key file can be set. New host keys are accepted on first use. If a host key
*changes*, the app shows the new fingerprint and asks before replacing it
(`ssh-keygen` keeps a `.old` backup).

## Test

```bash
scripts/test-core.sh   # core C API vs. the mock camera, ONVIF and Axis
scripts/test-ssh.sh    # SSH route via a throwaway user-level sshd on 127.0.0.1:2222
python3 tools/mock-camera.py --port 8080 --user admin --password secret
```

`test-ssh.sh` backs up `~/.ssh/known_hosts` and restores it afterwards.

## Build for Windows (10/11, x64)

Built on Windows with the Visual Studio 2022 C++ Build Tools (the free
"Desktop development with C++" workload) and git, from PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1        # → dist\PTZControl.exe
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Zip   # also dist\PTZ-Control-1.0.0-Windows-x64.zip
```

The result is a single statically linked `PTZControl.exe` (about 0.9 MB) that
imports only Windows system DLLs. Copy it anywhere and run it: there is no
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

## Build for Linux (x86_64 AppImage)

Built on Linux, in a rootless Podman container from Ubuntu 22.04 (`linux/Containerfile`), so the
AppImage runs on older distributions than the one that built it. One-time setup on Ubuntu or Debian:
`sudo apt install podman` (Docker works too).

```bash
scripts/build-linux.sh            # → dist/PTZ-Control-1.0.0-x86_64.AppImage
```

The script builds for the machine's own CPU: run on an ARM64 Linux machine, it makes
`PTZ-Control-…-aarch64.AppImage`.

The AppImage (about 31 MB) is one self-contained file: Qt 6, libcurl and
OpenSSL, the JetBrains Mono font, icons, the desktop entry and the licence
notices of everything bundled. It runs on any distribution with glibc 2.35 or
newer (Ubuntu 22.04+, Debian 12+, Fedora 36+, current Arch and openSUSE
Tumbleweed), needs no root and no `libfuse2`:

```bash
chmod +x PTZ-Control-1.0.0-x86_64.AppImage
./PTZ-Control-1.0.0-x86_64.AppImage
```

- What stays with the system: glibc and libstdc++, the X11/xcb client
  libraries, fontconfig/freetype and the OpenGL drivers. The OpenGL loaders
  and HarfBuzz are bundled as fallbacks that `AppRun` uses only when the
  system has none.
- Windows open through X11 (XWayland on Wayland desktops), falling back to
  native Wayland when no X server exists; `QT_QPA_PLATFORM=wayland` forces it.
- Settings live in `~/.config/PTZControl/PTZControl.conf`. Shortcuts use Ctrl
  in place of ⌘; the ☰ button holds the menu.
- **SSH route:** uses the system OpenSSH client (`/usr/bin/ssh`), as on macOS.
- No FUSE at all (e.g. in a container): `./PTZ-Control-…AppImage --appimage-extract-and-run`.

The AppImage launchers come from
[AppImage/type2-runtime](https://github.com/AppImage/type2-runtime), pinned by
checksum in the build script. `linux/test-appimage.sh` runs an AppImage against
the mock camera under Xvfb inside a stock distribution container.

## Versioning

Versions are `major.minor.patch`. [`VERSION`](VERSION) holds the major and minor number, and the patch number is the count of commits since `VERSION` last changed, so it rises with every push to `main`. Editing `VERSION` (to `1.1`, say) starts the next series at `1.1.0`. `scripts/version.sh` prints the current version, and the builds use it.

## Open source

PTZ Control is free and open source under the [MIT License](LICENSE): use it, change it and share it, for personal or commercial work. Just keep the copyright and license notice with any copy. Third-party components keep their own licenses; see [NOTICES](NOTICES).

## Support

PTZ Control is a testing app, shared publicly so others can benefit from it. It is provided as is: no support is promised or given.

What has been reported and what's done is tracked in the open: [bugs.md](bugs.md) and [enhancements.md](enhancements.md).

## Contributing

Bug reports, ideas and pull requests are welcome: open an [issue](https://github.com/dunkelherzmann/ptz-control/issues) or a pull request here. Contributions are accepted under the same MIT License.

## Contact

Contact me through my GitHub profile: [dunkelherzmann](https://github.com/dunkelherzmann).

Copyright © 2026 Herbert Cisneros, developed with AI coding assistance.
