# PTZ Control

PTZ Coordinates Controller by Herbert Cisneros: a native app to read and set
absolute pan/tilt/zoom on ONVIF (Bosch, Milesight, …) and Axis VAPIX cameras,
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

## Build (macOS 14+)

Needs only the Xcode Command Line Tools.

```bash
scripts/build-macos.sh          # → dist/PTZ Control.app (universal arm64 + x86_64)
scripts/build-macos.sh --zip    # also dist/PTZ-Control-1.0.0-macOS.zip
```

The app is ad-hoc signed, so it runs on the Mac that built it. To hand it to
others without Gatekeeper warnings, sign it with a Developer ID certificate and
notarize it.

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

Cross-compiled from macOS or Linux with mingw-w64 (`brew install mingw-w64`):

```bash
scripts/build-windows.sh          # → dist/PTZControl.exe
scripts/build-windows.sh --zip    # also dist/PTZ-Control-1.0.0-Windows-x64.zip
```

The result is a single statically linked `PTZControl.exe` (about 1.8 MB) that
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

## Build for Linux (x86_64 and arm64 AppImages)

Built on macOS with Apple's `container` tool (`brew install container`), in an
Ubuntu 22.04 container per CPU (`linux/Containerfile`):

```bash
scripts/build-linux.sh            # → dist/PTZ-Control-1.0.0-x86_64.AppImage
                                  #   dist/PTZ-Control-1.0.0-aarch64.AppImage
scripts/build-linux.sh aarch64    # one architecture only
```

Each AppImage (about 31 MB) is one self-contained file: Qt 6, libcurl and
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

## License

Copyright © 2026 Herbert Cisneros. Creative direction and design by Herbert Cisneros; source code developed with AI coding assistance.

This project is dual-licensed:

| Use case | License |
|---|---|
| Personal, open-source, or non-commercial — **provided you publish your modifications under the same terms** | [AGPLv3](LICENSE) — free |
| Commercial use, or any use where you do not want to publish your source | [Commercial license](LICENSE-COMMERCIAL) — contact the author |

The short version: **use it freely and share your improvements, or pay to keep them private.**
