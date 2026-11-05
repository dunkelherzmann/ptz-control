#!/bin/sh
# ONVIF / Axis VAPIX PTZ client. Dependencies live in a private environment.
set -eu

VENV="${HOME}/.local/share/pzt/venv"
SYSTEM="$(uname -s)"
INSTALL_ONLY=0
if [ "${1:-}" = "--install" ]; then
    if [ "$#" -ne 1 ]; then
        printf '%s\n' 'Usage: ptz-control.sh --install' >&2
        exit 2
    fi
    INSTALL_ONLY=1
fi


SHOW_HELP=0
for argument in "$@"; do
    case "$argument" in
        -h|--help) SHOW_HELP=1 ;;
    esac
done

approve() {
    printf '%s\n' "$1" >&2
    if [ ! -t 0 ]; then
        printf '%s\n' 'No interactive input: nothing installed. Run again in a terminal.' >&2
        return 1
    fi
    printf '%s' 'Install these dependencies? [y/N]: ' >&2
    IFS= read -r answer || return 1
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) printf '%s\n' 'Installation declined; nothing installed.' >&2; return 1 ;;
    esac
}

system_packages() {
    component="$1"
    case "$SYSTEM" in
        Darwin)
            if ! command -v brew >/dev/null 2>&1; then
                printf '%s\n' 'macOS: install Python 3/Homebrew manually (https://brew.sh), then retry. Homebrew is not installed automatically.' >&2
                return 1
            fi
            set -- brew install python
            ;;
        Linux)
            if command -v apt-get >/dev/null 2>&1; then
                set -- apt-get install python3 python3-venv python3-pip
            elif command -v dnf >/dev/null 2>&1; then
                set -- dnf install python3 python3-pip
            elif command -v pacman >/dev/null 2>&1; then
                set -- pacman -S --needed python python-pip
            elif command -v zypper >/dev/null 2>&1; then
                set -- zypper install python3 python3-pip
            else
                printf '%s\n' 'Linux: install Python 3, venv and pip using your package manager, then retry.' >&2
                return 1
            fi
            if [ "$(id -u)" -ne 0 ]; then
                if ! command -v sudo >/dev/null 2>&1; then
                    printf '%s\n' 'System packages require administrator access; install them manually.' >&2
                    return 1
                fi
                set -- sudo "$@"
            fi
            ;;
        *)
            printf 'Unsupported OS: %s. Use macOS, Linux, or Linux inside WSL.\n' "$SYSTEM" >&2
            return 1
            ;;
    esac
    approve "Detected $SYSTEM; missing $component. Proposed command: $*" || return 1
    "$@"
}

select_python() {
    if [ -x "$VENV/bin/python" ]; then
        PYTHON="$VENV/bin/python"
    elif command -v python3 >/dev/null 2>&1; then
        PYTHON=python3
    else
        return 1
    fi
}

if ! select_python; then
    if [ "$SHOW_HELP" -eq 1 ]; then
        printf '%s\n' 'Usage: ptz-control.sh [--local|--ssh] [--plain] [--vendor onvif|axis]' \
            '       ptz-control.sh host pan tilt [--zoom VALUE]' \
            '       ptz-control.sh --install' \
            'Python 3 is required for full option help. No installation was performed.'
        exit 0
    fi
    system_packages 'Python 3' || exit 1
    hash -r
    select_python || { printf '%s\n' 'Python 3 still unavailable; check PATH and retry.' >&2; exit 1; }
fi

check_dependencies() {
    "$1" -c 'from onvif import ONVIFCamera; from requests import Session; from zeep.transports import Transport' >/dev/null 2>&1
}

if [ "$SHOW_HELP" -eq 0 ]; then
    if ! check_dependencies "$PYTHON"; then
        if ! "$PYTHON" -c 'import venv, ensurepip' >/dev/null 2>&1; then
            system_packages 'Python venv/pip support' || exit 1
        fi
        approve "Detected $SYSTEM; missing Python camera packages. Create/reuse $VENV and run: python -m pip install onvif-zeep requests zeep" || exit 1
        printf '%s\n' "Setting up PTZ dependencies in $VENV ..." >&2
        if [ ! -x "$VENV/bin/python" ]; then
            if ! "$PYTHON" -m venv "$VENV"; then
                printf '%s\n' 'Unable to create the private Python environment. Check Python venv support and directory permissions.' >&2
                exit 1
            fi
        fi
        PYTHON="$VENV/bin/python"
        if ! "$PYTHON" -m pip --version >/dev/null 2>&1; then
            "$PYTHON" -m ensurepip --upgrade
        fi
        if ! "$PYTHON" -m pip install onvif-zeep requests zeep; then
            printf '%s\n' 'Dependency installation failed. Check your internet connection and retry.' >&2
            exit 1
        fi
        if ! check_dependencies "$PYTHON"; then
            printf '%s\n' 'Dependencies could not be imported after installation. Check the Python environment.' >&2
            exit 1
        fi
    fi
    if [ "$INSTALL_ONLY" -eq 1 ]; then
        printf '%s\n' 'PTZ dependencies are ready.'
        exit 0
    fi
fi

exec "$PYTHON" - "$@" <<'PYTHON'
import argparse
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager, nullcontext, redirect_stderr, redirect_stdout
import getpass
import io
import math
import os
import platform
import re
from pathlib import Path
import shutil
import shlex
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
import webbrowser
from urllib.parse import urljoin, urlsplit, urlunsplit


def finite_number(value):
    try:
        result = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a number")
    if not math.isfinite(result):
        raise argparse.ArgumentTypeError("must be a finite number")
    return result


parser = argparse.ArgumentParser(
    prog="ptz-control.sh",
    description="PTZ Coordinates Controller by Herbert Cisneros",
    epilog=(
        "Missing dependencies are checked on startup and installed only after approval. "
        "Manual setup: ~/ptz-control.sh --install. Password: PTZ_PASSWORD environment variable "
        "or a hidden terminal prompt. ONVIF uses advertised absolute spaces "
        "(usually pan/tilt -1..1, zoom 0..1). Axis uses pan/tilt degrees and native "
        "integer zoom. Both interactive modes accept pan tilt [zoom] with x for unchanged axes. "
        "No arguments opens the full-screen controller with a local/remote selector. "
        "Use --plain for line-based prompts. "
        "Direct example: ~/ptz-control.sh 192.168.1.100 0.55 0.22 --zoom 0.40"
    ),
)
parser.add_argument("host", nargs="?", help="camera IP address or hostname (direct mode)")
parser.add_argument("pan", nargs="?", type=finite_number, help="absolute pan coordinate")
parser.add_argument("tilt", nargs="?", type=finite_number, help="absolute tilt coordinate")
connection_flags = parser.add_mutually_exclusive_group()
connection_flags.add_argument("--ssh", action="store_true", help="use an interactive remote-host SSH connection")
connection_flags.add_argument("--local", action="store_true", help="use an interactive camera connection from this computer")
parser.add_argument("--plain", action="store_true", help="use line-based prompts instead of the full-screen TUI")
parser.add_argument("--vendor", choices=("onvif", "axis"), help="camera backend (direct default: onvif; interactive default: ask)")
parser.add_argument("--zoom", type=finite_number, help="absolute zoom; omitted = unchanged")
parser.add_argument("--user", default=os.environ.get("PTZ_USER", "admin"), help="camera user (default: PTZ_USER or admin)")
parser.add_argument("--port", type=int, default=80, help="camera HTTP port (default: 80)")
parser.add_argument("--https-port", type=int, default=443, help="camera web-interface HTTPS fallback port (default: 443)")
parser.add_argument("--profile", type=int, default=0, help="ONVIF media profile index (default: 0; Axis uses channel 1)")
parser.add_argument("--timeout", type=finite_number, default=15, help="request timeout in seconds (default: 15)")
args = parser.parse_args()
interactive = args.ssh or args.local or args.host is None
if interactive:
    if any(value is not None for value in (args.host, args.pan, args.tilt, args.zoom)):
        parser.error("interactive mode accepts coordinates after connecting, not on the command line")
elif args.pan is None or args.tilt is None:
    parser.error("direct mode requires host, pan, and tilt")
if not 1 <= args.port <= 65535:
    parser.error("--port must be between 1 and 65535")
if not 1 <= args.https_port <= 65535:
    parser.error("--https-port must be between 1 and 65535")
if args.profile < 0:
    parser.error("--profile must be nonnegative")
if args.timeout <= 0:
    parser.error("--timeout must be positive")

try:
    from onvif import ONVIFCamera
    from requests import Session
    from requests.auth import HTTPDigestAuth
    from requests.exceptions import RequestException, SSLError
    from zeep.transports import Transport
except ImportError:
    parser.exit(1, "Missing ONVIF dependencies. Run: ~/ptz-control.sh --install\n")
password = None
host_password = None


def redact(message):
    message = str(message)
    for secret in sorted((value for value in (password, host_password) if value),
                         key=len, reverse=True):
        message = message.replace(secret, "[redacted]")
    return message


def report_error(exc):
    print(f"PTZ error: {redact(exc)}", file=sys.stderr)


def prompt(terminal, label, default=None):
    suffix = f" [{default}]" if default is not None else ""
    while True:
        sys.stderr.write(f"{label}{suffix}: ")
        sys.stderr.flush()
        line = terminal.readline()
        if not line:
            raise EOFError
        value = line.strip()
        if value:
            return value
        if default is not None:
            return str(default)


def prompt_port(terminal, label, default):
    while True:
        try:
            value = int(prompt(terminal, label, default))
            if 1 <= value <= 65535:
                return value
        except ValueError:
            pass
        print("Port must be an integer between 1 and 65535.", file=sys.stderr)


def component_available(component):
    if component == "curses":
        try:
            __import__("curses")
            return True
        except ImportError:
            return False
    return bool(shutil.which(component))


def install_command(component):
    system = platform.system()
    if system == "Darwin":
        if not shutil.which("brew"):
            raise RuntimeError("macOS requires an existing Homebrew installation (https://brew.sh); "
                               "install it or the missing component manually")
        package = {"ssh": "openssh", "sshpass": "sshpass", "curses": "python"}[component]
        return ["brew", "install", package]
    if system != "Linux":
        raise RuntimeError(f"Unsupported OS {system}; use macOS, Linux, or WSL")
    managers = (
        ("apt-get", ["install"], {"ssh": ["openssh-client"], "sshpass": ["sshpass"],
                                 "curses": ["python3", "libpython3-stdlib"]}),
        ("dnf", ["install"], {"ssh": ["openssh-clients"], "sshpass": ["sshpass"],
                             "curses": ["python3-libs"]}),
        ("pacman", ["-S", "--needed"], {"ssh": ["openssh"], "sshpass": ["sshpass"],
                                     "curses": ["python"]}),
        ("zypper", ["install"], {"ssh": ["openssh-clients"], "sshpass": ["sshpass"],
                                "curses": ["python3-curses"]}),
    )
    for manager, flags, packages in managers:
        if shutil.which(manager):
            command = [manager, *flags, *packages[component]]
            if os.geteuid() != 0:
                if not shutil.which("sudo"):
                    raise RuntimeError("Install system dependencies manually with administrator access")
                command.insert(0, "sudo")
            return command
    raise RuntimeError("No supported Linux package manager found; install the missing component manually")


def ensure_component(component, terminal):
    if component_available(component):
        return
    command = install_command(component)
    print(f"Detected {platform.system()}; missing {component}. Proposed command: {shlex.join(command)}",
          file=sys.stderr, flush=True)
    if terminal is None or not terminal.isatty():
        raise RuntimeError("No interactive terminal; nothing installed")
    if prompt(terminal, "Install this dependency? y/N", "n").lower() not in ("y", "yes"):
        raise RuntimeError(f"Installation declined; {component} is still missing")
    result = subprocess.run(command, stdin=terminal, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{component} installation failed; check the package-manager output")
    if not component_available(component):
        raise RuntimeError(f"{component} is still unavailable; check PATH/Python and restart the script")


@contextmanager
def ssh_password_pipe(command, secret):
    if not secret:
        yield command, {}
        return
    encoded = secret.encode("utf-8")
    if len(encoded) > 1024 or any(character in secret for character in ("\x00", "\r", "\n")):
        raise ValueError("Host Password must be one line, at most 1024 UTF-8 bytes")
    read_fd, write_fd = os.pipe()
    try:
        os.write(write_fd, encoded + b"\n")
        os.close(write_fd)
        write_fd = None
        yield ["sshpass", "-d", str(read_fd), *command], {"pass_fds": (read_fd,)}
    finally:
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)


def replace_changed_host_keys(diagnostics, host, port):
    """Edit only verified host matches; preserve shared aliases and unique backups."""
    if "REMOTE HOST IDENTIFICATION HAS CHANGED" not in diagnostics:
        return []
    offending = re.findall(r"^Offending (?:\S+ )?key(?: for IP)? in (.+):(\d+)\s*$",
                           diagnostics, re.MULTILINE)
    if not offending:
        return []
    if not shutil.which("ssh-keygen"):
        raise RuntimeError("ssh-keygen is required to replace changed host keys")
    lookup = re.search(r"^Host key for (.+) has changed and you have requested strict checking\.\s*$",
                       diagnostics, re.MULTILINE)
    identifier = lookup.group(1) if lookup else host if port == 22 else f"[{host}]:{port}"
    if not identifier or any(c.isspace() or ord(c) < 32 for c in identifier):
        raise RuntimeError("Cannot identify the conflicting SSH host-key entry")
    files = {}
    for filename, number in offending:
        path = Path(filename).expanduser().resolve(strict=True)
        files.setdefault(path, set()).add(int(number))
    backups = []
    for path, offending_lines in files.items():
        original = path.read_bytes()
        found = subprocess.run(
            ["ssh-keygen", "-F", identifier, "-f", str(path)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, timeout=5,
        )
        matches = {int(number) for number in re.findall(r"found: line (\d+)", found.stdout)}
        if found.returncode != 0 or not offending_lines.issubset(matches):
            raise RuntimeError("Conflicting host-key entry could not be verified; known-hosts file unchanged")
        lines = original.splitlines(keepends=True)
        preserved = []
        for number in sorted(matches):
            if not 1 <= number <= len(lines):
                raise RuntimeError("Known-hosts file changed during verification; retry the connection")
            line = lines[number - 1]
            record = re.match(rb"^(\s*(?:@\S+\s+)?)(\S+)(.*)", line.rstrip(b"\r\n"))
            if record is None:
                raise RuntimeError("Cannot safely update the conflicting known-hosts record")
            names = record[2].decode("utf-8")
            if "," in names or "*" in names or "?" in names:
                remaining = [name for name in names.split(",") if name.lower() != identifier.lower()]
                if any("*" in name or "?" in name for name in remaining):
                    remaining.insert(0, "!" + identifier)
                if remaining:
                    preserved.append(record[1] + ",".join(remaining).encode() + record[3] + b"\n")
        with tempfile.TemporaryDirectory(prefix=".ptz-hostkeys-", dir=path.parent) as directory:
            candidate = Path(directory) / "known_hosts"
            shutil.copy2(path, candidate)
            removed = subprocess.run(
                ["ssh-keygen", "-R", identifier, "-f", str(candidate)],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=5,
            )
            if removed.returncode != 0:
                raise RuntimeError("Could not remove the conflicting host key; original file unchanged")
            if preserved:
                with candidate.open("ab") as output:
                    output.write(b"\n" + b"".join(preserved))
            if path.read_bytes() != original:
                raise RuntimeError("Known-hosts file changed during update; original file left untouched")
            descriptor, backup = tempfile.mkstemp(
                prefix=path.name + ".ptz-backup-" + time.strftime("%Y%m%d-%H%M%S") + "-",
                dir=path.parent,
            )
            with os.fdopen(descriptor, "wb") as output:
                output.write(original)
            os.replace(candidate, path)
            backups.append(backup)
    return backups


LOGO = (
    r" ____ _____ _____ ",
    r"|  _ \_   _|__  / ",
    r"| |_) || |   / /  ",
    r"|  __/ | |  / /_  ",
    r"|_|    |_| /____| ",
)


def welcome():
    title = "PTZ Coordinates Controller by Herbert Cisneros"
    border = "+" + "-" * (len(title) + 4) + "+"
    print(f"\n{border}\n|  {title}  |\n{border}")
    for index, line in enumerate(LOGO):
        caption = ("PRECISION CAMERA CONTROL", "LOCAL / SSH", "ONVIF / AXIS VAPIX", "PAN  |  TILT  |  ZOOM", "")[index]
        print(f"  {line}   {caption}")
    print()


def choose_connection(terminal):
    print("Choose Connection:\n1. Local machine\n2. Remote host (SSH)", flush=True)
    while True:
        choice = prompt(terminal, "Selection (1 or 2)")
        if choice in ("1", "2"):
            return "local" if choice == "1" else "remote"
        print("Please enter 1 or 2.", file=sys.stderr)


def choose_vendor(terminal):
    print("Choose Camera Vendor:\n1. Bosch/Milesight\n2. Axis", flush=True)
    while True:
        choice = prompt(terminal, "Selection (1 or 2)")
        if choice in ("1", "2"):
            return "onvif" if choice == "1" else "axis"
        print("Please enter 1 or 2.", file=sys.stderr)


def validate_host(value):
    # Addresses are used in SSH forwarding syntax, never as shell commands.
    value = value.strip("[]")
    if not value or value.startswith("-") or any(c.isspace() or c in "/@\\\x00" for c in value):
        raise ValueError("Enter an IP address or hostname, without a URL, username, or port")
    if ":" in value:
        socket.inet_pton(socket.AF_INET6, value)
    return value


@contextmanager
def ssh_tunnel(host, user, ssh_port, camera_host, camera_port, terminal, ssh_password=""):
    ensure_component("ssh", terminal)
    if ssh_password:
        ensure_component("sshpass", terminal)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        local_port = reservation.getsockname()[1]
    target = f"[{camera_host}]" if ":" in camera_host else camera_host
    # A short path avoids macOS's Unix-domain socket path-length limit.
    with tempfile.TemporaryDirectory(prefix="pzt-", dir="/tmp") as directory:
        control = str(Path(directory) / "ssh")
        diagnostics = Path(directory) / "ssh.log"
        diagnostics.touch(mode=0o600)
        command = [
            "ssh", "-E", str(diagnostics), "-N", "-T", "-M", "-S", control,
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "ControlPersist=no", "-o", "ForkAfterAuthentication=no",
            "-o", "ExitOnForwardFailure=yes", "-o", "PermitLocalCommand=no",
            "-o", "RemoteCommand=none",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
            "-L", f"127.0.0.1:{local_port}:{target}:{camera_port}",
            "-p", str(ssh_port), "-l", user, "--", host,
        ]
        if ssh_password:
            # Passwords answer only password authentication, never key/MFA prompts.
            command[1:1] = [
                "-o", "BatchMode=no",
                "-o", "PreferredAuthentications=password", "-o", "KbdInteractiveAuthentication=no",
                "-o", "PubkeyAuthentication=no", "-o", "NumberOfPasswordPrompts=1",
            ]
        print("Opening SSH tunnel. " + (
            "Using the pre-entered Host Password."
            if ssh_password else "SSH may prompt for a password or key passphrase."
        ), flush=True)
        process = None
        try:
            connected = False
            for attempt in range(2):
                diagnostics.write_text("")
                with ssh_password_pipe(command, ssh_password) as (launch, options):
                    process = subprocess.Popen(launch, stdin=terminal, **options)
                while True:
                    if process.poll() is not None:
                        process.wait()
                        if attempt == 0 and process.returncode in (7, 255):
                            backups = replace_changed_host_keys(
                                diagnostics.read_text(errors="replace"), host, ssh_port,
                            )
                            if backups:
                                print("Changed SSH host key replaced; retrying once. Backups: "
                                      + ", ".join(backups), flush=True)
                                if Path(control).exists():
                                    Path(control).unlink()
                                break
                        reason = {
                            5: "Host Password rejected; check credentials or use your site's MFA login workflow",
                            6: "Unknown host key could not be saved; check known-hosts file permissions",
                            7: "Changed host key could not be replaced automatically",
                            255: "SSH connection/authentication failed; verify host key, credentials and network access",
                        }.get(process.returncode, f"SSH exited with status {process.returncode}")
                        raise RuntimeError(reason)
                    if Path(control).exists():
                        check = subprocess.run(
                            ["ssh", "-S", control, "-O", "check", "-p", str(ssh_port),
                             "-l", user, "--", host],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, timeout=5,
                        )
                        if check.returncode == 0:
                            connected = True
                            break
                    time.sleep(0.1)
                if connected:
                    break
            def forward(camera_web_port):
                with socket.socket() as reservation:
                    reservation.bind(("127.0.0.1", 0))
                    web_port = reservation.getsockname()[1]
                result = subprocess.run(
                    ["ssh", "-S", control, "-O", "forward",
                     "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes",
                     "-L", f"127.0.0.1:{web_port}:{target}:{camera_web_port}",
                     "-p", str(ssh_port), "-l", user, "--", host],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, timeout=5,
                )
                if result.returncode:
                    raise RuntimeError("Could not forward the camera HTTPS port through the existing SSH connection")
                return web_port

            process.ptz_forward = forward
            yield local_port, process
        finally:
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


class TunnelTransport(Transport):
    """Route SOAP calls through one camera's forwarded HTTP endpoint."""

    def __init__(self, local_port, **kwargs):
        super().__init__(**kwargs)
        self.local_port = local_port

    def post(self, address, message, headers):
        url = urlsplit(address)
        if url.scheme != "http":
            raise ValueError("SSH mode currently requires camera ONVIF services over HTTP")
        address = urlunsplit((
            "http", f"127.0.0.1:{self.local_port}", url.path, url.query, ""
        ))
        return super().post(address, message, headers)


def choose_space(spaces, default_uri, label):
    if not spaces:
        raise ValueError(f"Camera does not advertise an absolute {label} space")
    if default_uri:
        for space in spaces:
            if space.URI == default_uri:
                return space
        raise ValueError(f"Camera's default absolute {label} space is not advertised")
    return spaces[0]


def check_range(value, limits, label):
    if not limits.Min <= value <= limits.Max:
        raise ValueError(
            f"{label} {value:g} is outside the camera's range "
            f"[{limits.Min:g}, {limits.Max:g}]"
        )


def connect_onvif(host, port, user, password, tunneled=False):

    session = Session()
    # Camera traffic must not be sent to a proxy from the local environment.
    session.trust_env = False
    transport_type = TunnelTransport if tunneled else Transport
    kwargs = {"local_port": port} if tunneled else {}
    transport = transport_type(
        **kwargs,
        session=session, timeout=args.timeout, operation_timeout=args.timeout
    )
    cam = ONVIFCamera(
        host, port, user, password, transport=transport
    )
    media = cam.create_media_service()
    ptz = cam.create_ptz_service()
    profiles = media.GetProfiles()
    if args.profile >= len(profiles):
        raise ValueError(
            f"Profile {args.profile} does not exist; camera returned {len(profiles)} profiles"
        )
    profile = profiles[args.profile]
    config = profile.PTZConfiguration
    if config is None:
        raise ValueError("Selected profile has no PTZ configuration; try another --profile")

    options = ptz.GetConfigurationOptions({"ConfigurationToken": config.token})
    pan_tilt = choose_space(
        options.Spaces.AbsolutePanTiltPositionSpace,
        config.DefaultAbsolutePantTiltPositionSpace,
        "pan/tilt",
    )
    return ptz, profile, config, options, pan_tilt, media


def print_move(host, values):
    print(
        f"Absolute move accepted by {host}: "
        + ", ".join(f"{label}={value}" for label, value in values.items() if value is not None)
        + " (movement may still be in progress)"
    )


def move_onvif(camera, host, pan, tilt, zoom=None):
    ptz, profile, config, options, pan_tilt, _ = camera
    if pan is None and tilt is None and zoom is None:
        print("All axes unchanged; no move sent.")
        return False
    position = {}
    if pan is not None or tilt is not None:
        if pan is None or tilt is None:
            try:
                status = ptz.GetStatus({"ProfileToken": profile.token})
                current = getattr(getattr(status, "Position", None), "PanTilt", None)
                if current is None:
                    raise ValueError("camera did not report its pan/tilt position")
                current_space = getattr(current, "space", None)
                if current_space and current_space != pan_tilt.URI:
                    raise ValueError("camera reported position in a different coordinate space")
                if pan is None:
                    pan = finite_number(current.x)
                if tilt is None:
                    tilt = finite_number(current.y)
            except Exception as exc:
                raise ValueError(f"Cannot preserve the unchanged pan/tilt axis: {exc}") from exc
        check_range(pan, pan_tilt.XRange, "Pan")
        check_range(tilt, pan_tilt.YRange, "Tilt")
        position["PanTilt"] = {"x": pan, "y": tilt, "space": pan_tilt.URI}
    if zoom is not None:
        zoom_space = choose_space(
            options.Spaces.AbsoluteZoomPositionSpace,
            config.DefaultAbsoluteZoomPositionSpace,
            "zoom",
        )
        check_range(zoom, zoom_space.XRange, "Zoom")
        position["Zoom"] = {"x": zoom, "space": zoom_space.URI}

    request = ptz.create_type("AbsoluteMove")
    request.ProfileToken = profile.token
    request.Position = position
    ptz.AbsoluteMove(request)
    print_move(host, {"pan": pan, "tilt": tilt, "zoom": zoom})
    return True


class OnvifController:
    name = "Bosch/Milesight - ONVIF"
    units = "ONVIF absolute coordinates (commonly pan/tilt -1..1, zoom 0..1; not degrees)."

    def __init__(self, host, port, user, password, tunneled=False):
        self.camera = connect_onvif(host, port, user, password, tunneled)
        _, _, config, options, pan_tilt, _ = self.camera
        self.ranges = {
            "pan": (pan_tilt.XRange.Min, pan_tilt.XRange.Max),
            "tilt": (pan_tilt.YRange.Min, pan_tilt.YRange.Max),
        }
        self.zoom_space = None
        if options.Spaces.AbsoluteZoomPositionSpace:
            self.zoom_space = choose_space(
                options.Spaces.AbsoluteZoomPositionSpace,
                config.DefaultAbsoluteZoomPositionSpace, "zoom",
            )
            self.ranges["zoom"] = (self.zoom_space.XRange.Min, self.zoom_space.XRange.Max)

    def position(self):
        ptz, profile, _, _, pan_tilt, _ = self.camera
        status = ptz.GetStatus({"ProfileToken": profile.token})
        position = getattr(status, "Position", None)
        values = {}
        current = getattr(position, "PanTilt", None)
        if current is not None:
            space = getattr(current, "space", None)
            if space and space != pan_tilt.URI:
                raise ValueError("Camera reported pan/tilt in a different coordinate space")
            for axis, attribute in (("pan", "x"), ("tilt", "y")):
                value = getattr(current, attribute, None)
                if value is not None:
                    finite_number(value)
                    values[axis] = str(value)
        zoom = getattr(position, "Zoom", None)
        if zoom is not None and self.zoom_space is not None:
            space = getattr(zoom, "space", None)
            if space and space != self.zoom_space.URI:
                raise ValueError("Camera reported zoom in a different coordinate space")
            value = getattr(zoom, "x", None)
            if value is not None:
                finite_number(value)
                values["zoom"] = str(value)
        if not values:
            raise ValueError("Camera did not report its current coordinates")
        return values

    def move(self, host, pan, tilt, zoom=None):
        return move_onvif(self.camera, host, pan, tilt, zoom)

    def close(self):
        self.camera[0].zeep_client.transport.session.close()


class AxisController:
    name = "Axis - VAPIX / HTTP Digest (camera channel 1)"
    units = "Axis native coordinates: pan/tilt in degrees; zoom is an integer, not optical magnification."

    def __init__(self, host, port, user, password, tunneled=False):
        self.session = Session()
        self.session.trust_env = False
        self.session.auth = HTTPDigestAuth(user, password)
        address = f"[{host}]" if ":" in host else host
        self.url = f"http://{address}:{port}/axis-cgi/com/ptz.cgi"
        try:
            limits = self.query("limits")
            self.ranges = {}
            for axis in ("pan", "tilt", "zoom"):
                low, high = limits.get("min" + axis), limits.get("max" + axis)
                if low is not None and high is not None:
                    low, high = finite_number(low), finite_number(high)
                    if low > high:
                        raise ValueError(f"Axis returned invalid {axis} limits")
                    self.ranges[axis] = (low, high)
            if not self.ranges:
                raise ValueError("Axis did not report usable PTZ limits for camera channel 1")
        except BaseException:
            self.close()
            raise

    def request(self, parameters):
        response = self.session.get(
            self.url, params={"camera": "1", **parameters},
            timeout=args.timeout, allow_redirects=False,
        )
        try:
            if 300 <= response.status_code < 400:
                raise ValueError("Axis redirected the request; use an HTTP camera endpoint (HTTPS is not supported)")
            response.raise_for_status()
            text = response.text.strip()
            if any(line.strip().lower().startswith("error") for line in text.splitlines()):
                raise ValueError(f"Axis rejected the request: {text}")
            if text and text.lstrip().startswith("<"):
                raise ValueError("Axis returned HTML/XML instead of a VAPIX PTZ response")
            return text
        finally:
            response.close()

    def query(self, kind):
        text = self.request({"query": kind})
        return {
            key.strip().lower(): value.strip()
            for line in text.splitlines()
            for key, separator, value in [line.partition("=")]
            if separator
        }

    def position(self):
        reported = self.query("position")
        values = {}
        for axis in ("pan", "tilt", "zoom"):
            if axis in reported:
                finite_number(reported[axis])
                # Preserve all digits returned by the camera, including trailing zeros.
                values[axis] = reported[axis]
        if not values:
            raise ValueError("Axis did not report its current coordinates")
        return values

    def move(self, host, pan, tilt, zoom=None):
        values = {"pan": pan, "tilt": tilt, "zoom": zoom}
        parameters = {}
        for axis, value in values.items():
            if value is None:
                continue
            value = finite_number(value)
            if axis not in self.ranges:
                raise ValueError(f"Axis did not advertise {axis} limits; move not sent")
            low, high = self.ranges[axis]
            if not low <= value <= high:
                raise ValueError(f"{axis.title()} {value} is outside the camera's range [{low}, {high}]")
            if axis == "zoom":
                if not value.is_integer():
                    raise ValueError("Axis zoom must be an integer in the advertised native range")
                parameters[axis] = str(int(value))
            else:
                parameters[axis] = str(value)
        if not parameters:
            print("All axes unchanged; no move sent.")
            return False
        # Omitted axes are not sent to VAPIX, so no status-derived coordinates are needed.
        self.request(parameters)
        print_move(host, values)
        return True

    def close(self):
        self.session.close()




class CameraWebView:
    """Open the camera's own UI; probe without sending credentials or proxying pages."""

    def __init__(self, host, http_port, local_http_port=None, forward=None, tunneled=False):
        self.host = validate_host(host)
        self.http_port = http_port
        self.local_http_port = local_http_port or http_port
        self.forward = forward
        self.tunneled = tunneled
        self.https_local_port = None
        self.https_camera_port = None
        self.url = ""
        self.notice = ""
        self.error = ""
        self.probe_target = ""

    def address(self, scheme, port):
        host = "127.0.0.1" if self.tunneled else self.host
        host = f"[{host}]" if ":" in host else host
        return f"{scheme}://{host}:{port}/"

    def probe(self, url):
        self.probe_target = url
        with Session() as session:
            session.trust_env = False
            try:
                with session.get(url, timeout=min(args.timeout, 4), stream=True,
                                 allow_redirects=False) as response:
                    if 300 <= response.status_code < 400:
                        target = urlsplit(urljoin(url, response.headers.get("Location", "")))
                        if (target.username is not None or target.password is not None
                                or (target.hostname or "").lower() not in (self.host.lower(), "127.0.0.1")):
                            raise RuntimeError("Camera redirected outside the configured host; check its web settings")
                        if target.scheme not in ("http", "https"):
                            raise RuntimeError("Camera web redirect uses an unsupported protocol")
                        if urlsplit(url).scheme == "http" and target.scheme == "https":
                            secure_port = target.port or 443
                            if (self.tunneled and target.hostname == "127.0.0.1"
                                    and secure_port == self.local_http_port):
                                secure_port = args.https_port
                            return False, secure_port
                        if self.tunneled and target.hostname.lower() == self.host.lower():
                            expected = self.http_port if target.scheme == "http" else self.https_camera_port
                            if (target.port or (443 if target.scheme == "https" else 80)) != expected:
                                raise RuntimeError("Camera redirected to an unforwarded port; check its web settings")
                            self.probe_target = urlunsplit((
                                target.scheme, urlsplit(url).netloc, target.path or "/", target.query, "",
                            ))
                    # Login challenges mean the web interface is reachable.
                    return response.status_code < 400 or response.status_code in (401, 403), None
            except SSLError as exc:
                if url.startswith("https:") and "CERTIFICATE_VERIFY_FAILED" in str(exc):
                    self.notice = " Verify the camera certificate in your browser."
                    return True, None
                return False, None
            except RequestException:
                return False, None

    def prepare(self):
        self.url, self.notice, self.error, self.probe_target = "", "", "", ""
        try:
            http_url = self.address("http", self.local_http_port)
            available, redirect_port = self.probe(http_url)
            if available:
                self.url = self.probe_target or http_url
            else:
                camera_port = redirect_port or args.https_port
                if self.tunneled:
                    if self.forward is None:
                        raise RuntimeError("The SSH connection cannot add an HTTPS forward; reconnect")
                    if self.https_local_port is None or self.https_camera_port != camera_port:
                        self.https_local_port = self.forward(camera_port)
                        self.https_camera_port = camera_port
                    local_port = self.https_local_port
                else:
                    local_port = camera_port
                https_url = self.address("https", local_port)
                self.probe_target = ""
                available, _ = self.probe(https_url)
                if not available:
                    raise RuntimeError("Camera web interface unavailable over HTTP and HTTPS; check its ports and network access")
                self.url = self.probe_target or https_url
            output = "Camera web interface: " + self.url + self.notice
        except Exception as exc:
            self.error = redact(exc)
            output = "Camera web interface: " + self.error
        return {"position": None, "error": "", "output": output}

    def open(self):
        try:
            return bool(self.url) and webbrowser.open(self.url, new=1)
        except Exception:
            return False



def connect_camera(host, port, user, password, tunneled=False, vendor="onvif"):
    controller = AxisController if vendor == "axis" else OnvifController
    camera = controller(host, port, user, password, tunneled)
    camera.web_view = CameraWebView(host, port)
    return camera


@contextmanager
def camera_session(mode, details, terminal):
    """Own only the resources needed for this local or remote connection."""
    remote = mode == "remote"
    tunnel = (
        ssh_tunnel(
            details["ssh_host"], details["ssh_user"], details["ssh_port"],
            details["camera_host"], details["camera_port"], terminal,
            ssh_password=details.get("ssh_password", ""),
        )
        if remote else nullcontext((details["camera_port"], None))
    )
    with tunnel as (port, process):
        host = "127.0.0.1" if remote else details["camera_host"]
        camera = connect_camera(
            host, port, details["camera_user"], details["camera_password"],
            tunneled=remote, vendor=details["vendor"],
        )
        camera.web_view = CameraWebView(
            details["camera_host"], details["camera_port"], local_http_port=port,
            forward=getattr(process, "ptz_forward", None), tunneled=remote,
        )
        try:
            yield camera, details["camera_host"], process
        finally:
            camera.close()


def parse_coordinates(line):
    parts = line.split()
    if len(parts) not in (2, 3):
        raise ValueError("Enter two or three values (numbers or x): pan tilt [zoom]")
    return [None if part.lower() == "x" else finite_number(part) for part in parts]


def show_position(camera, initial=False):
    try:
        values = camera.position()
        missing = [axis for axis in ("pan", "tilt", "zoom") if axis not in values]
        if missing:
            print("Position unavailable for: " + ", ".join(missing) + ".", file=sys.stderr)
    except Exception as exc:
        report_error(exc)
        values = {}
    coordinates = " ".join(
        f"{axis.title()}:{values.get(axis, 'unknown')}" for axis in ("pan", "tilt", "zoom")
    )
    if initial:
        # Both initial lines use the same query; this baseline is never printed again.
        print(f"Starting coordinates: {coordinates}", flush=True)
    print(f"Current coordinates: {coordinates}", flush=True)
    return values


def starting_move(starting):
    values = [finite_number(starting[axis]) if axis in (starting or {}) else None
              for axis in ("pan", "tilt", "zoom")]
    if all(value is None for value in values):
        raise ValueError("Starting coordinates unavailable; no return move sent")
    missing = [axis for axis, value in zip(("pan", "tilt", "zoom"), values) if value is None]
    message = "Returning to starting coordinates"
    if missing:
        message += "; unavailable axes left unchanged: " + ", ".join(missing)
    return values, message


def coordinate_help():
    print("Enter a b c, where a=pan, b=tilt, c=zoom; use actual numbers, not the letters.")
    print("  a b c : change all axes     x b c : leave pan unchanged")
    print("  a x c : leave tilt unchanged     a b x : leave zoom unchanged")
    print("Use x for any combination of unchanged axes; omitted zoom is also unchanged.")
    print("Values are absolute targets, not increments. Units depend on the selected vendor.")
    print("O then Enter: return to known starting coordinates (sends a move).")
    print("Type help for this explanation; quit or Ctrl+C disconnects (does not stop a move).")


def coordinate_loop(terminal, camera, host, process):
    print(f"\nConnected: {camera.name}\n{camera.units}")
    for axis, (low, high) in camera.ranges.items():
        print(f"  {axis.title()} range: {low}..{high}")
    coordinate_help()
    starting = show_position(camera, initial=True)
    while True:
        if process is not None and process.poll() is not None:
            raise RuntimeError("SSH connection closed; reconnect by running ptz-control.sh again")
        line = prompt(terminal, "pan tilt [zoom]")
        if process is not None and process.poll() is not None:
            raise RuntimeError("SSH connection closed; coordinates were not sent")
        if line.lower() in ("quit", "exit", "q"):
            return
        if line.lower() in ("help", "?"):
            coordinate_help()
            continue
        try:
            if line.lower() == "o":
                values, message = starting_move(starting)
                print(message)
            else:
                values = parse_coordinates(line)
            if camera.move(host, *values):
                # This is a fresh snapshot, not confirmation that motion has finished.
                show_position(camera)
        except Exception as exc:
            report_error(exc)


class PositionState:
    """Freeze the first snapshot while allowing subsequent live reads."""

    def __init__(self):
        self.starting = None
        self.current = {}
        self.failures = 0
        self.paused = False
        self.activity = deque(maxlen=30)

    def apply(self, result):
        values = result["position"]
        if values is not None:
            if self.starting is None:
                self.starting = dict(values)
            self.current = dict(values)
        if result["error"]:
            self.failures += 1
            self.activity.append("Read failed: " + redact(result["error"]))
            if self.failures >= 3:
                self.paused = True
                self.activity.append("Auto-refresh paused after 3 failures. Press R to retry.")
        elif values is not None:
            self.failures = 0
        for line in result["output"].splitlines():
            if line.strip():
                self.activity.append(redact(line))


def camera_job(camera, host, values=None):
    """One worker owns camera calls; never overlap session requests."""
    output = io.StringIO()
    position, error = None, ""
    with redirect_stdout(output), redirect_stderr(output):
        moved = camera.move(host, *values) if values is not None else True
        if moved:
            try:
                position = camera.position()
            except Exception as exc:
                position, error = {}, redact(exc)
    return {"position": position, "error": error, "output": redact(output.getvalue())}


def edit_text(value, cursor, key, keys):
    """Shared editing for visible and masked fields; no secrets are logged."""
    if key in (keys.KEY_BACKSPACE, "\x7f", "\b"):
        return value[:max(0, cursor - 1)] + value[cursor:], max(0, cursor - 1)
    if key == keys.KEY_DC:
        return value[:cursor] + value[cursor + 1:], cursor
    if key == keys.KEY_LEFT:
        return value, max(0, cursor - 1)
    if key == keys.KEY_RIGHT:
        return value, min(len(value), cursor + 1)
    if key in (keys.KEY_HOME, "\x01"):
        return value, 0
    if key in (keys.KEY_END, "\x05"):
        return value, len(value)
    if key == "\x15":
        return "", 0
    if isinstance(key, str) and key.isprintable() and len(value) < 1024:
        return value[:cursor] + key + value[cursor:], cursor + len(key)
    return value, cursor


def validate_details(mode, details):
    clean = dict(details)
    clean["camera_host"] = validate_host(clean["camera_host"])
    if not clean["camera_user"].strip():
        raise ValueError("Camera Username is required")
    if mode == "remote":
        clean["ssh_host"] = validate_host(clean["ssh_host"])
        if not clean["ssh_user"].strip():
            raise ValueError("Username is required")
        secret = clean.get("ssh_password", "")
        if len(secret.encode("utf-8")) > 1024 or any(c in secret for c in ("\x00", "\r", "\n")):
            raise ValueError("Host Password must be one line, at most 1024 UTF-8 bytes")
    for field in ("camera_port", "ssh_port") if mode == "remote" else ("camera_port",):
        try:
            value = int(clean[field])
        except (ValueError, TypeError):
            raise ValueError("Ports must be integers between 1 and 65535") from None
        if not 1 <= value <= 65535:
            raise ValueError("Ports must be integers between 1 and 65535")
        clean[field] = value
    return clean


class TerminalUI:
    """Built-in curses interface; the camera protocols remain independent."""

    def __init__(self, screen):
        import curses
        self.c = curses
        self.screen = screen
        self.color = False
        unicode_lines = (sys.stdout.encoding or "utf-8").lower().replace("-", "").startswith("utf")
        self.lines = ("┌", "─", "┐", "│", "└", "┘", "┼", "↑", "→") if unicode_lines else (
            "+", "-", "+", "|", "+", "+", "+", "^", ">",
        )
        screen.keypad(True)
        screen.timeout(100)
        try:
            curses.curs_set(0)
            if curses.has_colors():
                curses.start_color()
                rich = curses.COLORS >= 256
                background = 16 if rich else curses.COLOR_BLACK
                cyan, amber, white, red, green, muted = (
                    (51, 214, 252, 203, 84, 245) if rich else
                    (curses.COLOR_CYAN, curses.COLOR_YELLOW, curses.COLOR_WHITE,
                     curses.COLOR_RED, curses.COLOR_GREEN, curses.COLOR_BLUE)
                )
                palette = (
                    (cyan, background), (amber, background), (white, background),
                    (red, background), (green, background), (muted, background),
                    (cyan, background), (amber, background), (white, background),
                )
                for index, (foreground, fill) in enumerate(palette, 1):
                    curses.init_pair(index, foreground, fill)
                self.color = True
                if hasattr(screen, "bkgd"):
                    screen.bkgd(" ", curses.color_pair(3))
        except curses.error:
            pass

    def style(self, pair=1, bold=False):
        return (self.c.color_pair(pair) if self.color else 0) | (self.c.A_BOLD if bold else 0)

    def panel(self, y, x, height, width, title, pair=1):
        """Continuous panel outline, with an ASCII fallback for non-UTF terminals."""
        if height < 3 or width < 8:
            return
        upper_left, horizontal, upper_right, vertical, lower_left, lower_right = self.lines[:6]
        edge = self.style(3)
        self.put(y, x, upper_left + horizontal * (width - 2) + upper_right, edge, width)
        self.put(y, x + 2, " " + title[:width - 6] + " ", self.style(pair, True), width - 4)
        for row in range(y + 1, y + height - 1):
            self.put(row, x, vertical, edge)
            self.put(row, x + width - 1, vertical, edge)
        self.put(y + height - 1, x, lower_left + horizontal * (width - 2) + lower_right, edge, width)

    def put(self, y, x, text, style=0, width=None):
        height, columns = self.screen.getmaxyx()
        if y < 0 or y >= height or x >= columns - 1:
            return
        # Do not interpret escape/control sequences received in camera errors.
        text = "".join(character if character.isprintable() else " " for character in str(text))
        try:
            self.screen.addnstr(y, x, text, min(width or columns, columns - x - 1), style)
        except self.c.error:
            pass

    def key(self):
        try:
            key = self.screen.get_wch()
            if key == self.c.KEY_RESIZE:
                self.c.update_lines_cols()
            if key == "\x03":
                raise KeyboardInterrupt
            return key
        except self.c.error:
            return None

    def frame(self, title, subtitle="", compact=False):
        self.screen.erase()
        height, width = self.screen.getmaxyx()
        if width < 76 or height < 24:
            self.put(1, 2, "PTZ Controller needs at least 76 columns x 24 rows.", self.style(2, True))
            self.put(3, 2, f"Current size: {width} x {height}. Resize, or Esc to return.")
            self.screen.refresh()
            return None
        self.screen.border()
        self.put(0, 3, " PTZ // FIELD OPERATIONS ", self.style(1, True))
        self.put(1, 3, "PTZ Coordinates Controller by Herbert Cisneros", self.style(1, True))
        self.put(2, 3, "LINK / POSITION / OPTICS" + " " * max(1, width - 53)
                 + "ABSOLUTE CONTROL", self.style(6))
        if compact:
            self.put(3, 4, subtitle, self.style(5))
            self.put(4, 2, self.lines[1] * (width - 4), self.style(3))
            self.put(5, 4, title, self.style(2, True))
            self.put(5, max(30, width - 27), "[ POSITION CONSOLE ]", self.style(9, True))
            return 7
        large = height >= 30
        logo = LOGO if large else (LOGO[0], LOGO[1], LOGO[-1])
        for row, line in enumerate(logo, 3):
            self.put(row, 4, line, self.style(1, True))
        self.put(3, 29, " PRECISION CAMERA CONTROL ", self.style(8, True))
        self.put(4, 29, "LOCAL / SSH  |  ONVIF / AXIS VAPIX", self.style(3))
        self.put(5, 29, subtitle, self.style(5))
        top = 10 if large else 8
        self.put(top - 1, 2, self.lines[1] * (width - 4), self.style(3))
        self.put(top, 4, title, self.style(2, True))
        return top + 2

    def footer(self, text):
        height, _ = self.screen.getmaxyx()
        self.put(height - 2, 3, text, self.style(1))
        self.screen.refresh()

    def menu(self, title, options):
        self.screen.clear()
        selected = 0
        while True:
            top = self.frame(title, "Choose a connection, then a camera vendor")
            if top is not None:
                for index, (label, description, _) in enumerate(options):
                    marker = ">" if index == selected else " "
                    _, width = self.screen.getmaxyx()
                    row = top + index * 4
                    self.panel(row - 1, 4, 4, width - 8,
                               "ACTIVE ROUTE" if index == selected else "AVAILABLE ROUTE",
                               1 if index == selected else 6)
                    self.put(row, 6, f"{marker} {index + 1}. {label}",
                             self.style(7 if index == selected else 3, index == selected))
                    self.put(row + 1, 10, description, self.style(3), width - 16)
                self.footer("Up/Down or J/K: select   Enter: continue   1/2: quick select   Esc/Q: back")
            key = self.key()
            if key in ("\x1b", "q", "Q", self.c.KEY_F10):
                return None
            if top is None:
                continue
            if key in (self.c.KEY_UP, "k"):
                selected = (selected - 1) % len(options)
            elif key in (self.c.KEY_DOWN, "j", "\t"):
                selected = (selected + 1) % len(options)
            elif key in ("\n", "\r", self.c.KEY_ENTER):
                return options[selected][2]
            elif isinstance(key, str) and key in ("1", "2"):
                return options[int(key) - 1][2]

    def form(self, mode, vendor):
        self.screen.clear()
        fields = []
        if mode == "remote":
            fields += [("ssh_host", "Host IP/DNS Name", ""), ("ssh_user", "Username", ""),
                       ("ssh_password", "Host Password", ""),
                       ("ssh_port", "SSH Port", "22")]
        fields += [("camera_host", "Camera IP", ""), ("camera_port", "Camera HTTP Port", str(args.port)),
                   ("camera_user", "Camera Username", args.user),
                   ("camera_password", "Camera Password", os.environ.get("PTZ_PASSWORD", ""))]
        values = {name: default for name, _, default in fields}
        cursors = {name: len(default) for name, _, default in fields}
        focus, error = 0, ""
        while True:
            top = self.frame("Connection details", f"{mode.upper()}  |  {vendor.upper()}  |  credentials stay in memory")
            if top is not None:
                height, width = self.screen.getmaxyx()
                self.panel(top - 1, 2, len(fields) + 2, width - 4,
                           "SECURE LINK CONFIGURATION", 1)
                for index, (name, label, _) in enumerate(fields):
                    active = index == focus
                    value = "*" * len(values[name]) if name.endswith("_password") else values[name]
                    cursor = cursors[name]
                    available = width - 32
                    offset = max(0, cursor - available + 1) if active else 0
                    self.put(top + index, 4, ("> " if active else "  ") + label,
                             self.style(1 if active else 3, active), 25)
                    self.put(top + index, 29, "[" + value[offset:offset + available - 2] + "]",
                             self.style(7 if active else 3, active))
                button_y = top + len(fields) + 1
                self.put(button_y, 6, "[ CONNECT ]", self.style(8 if focus == len(fields) else 2, True))
                self.put(button_y, 24, "[ BACK ]", self.style(7 if focus == len(fields) + 1 else 6, True))
                if focus >= len(fields):
                    self.put(button_y, 4 if focus == len(fields) else 22, ">", self.style(1, True))
                self.put(button_y + 1, 4, error, self.style(4))
                self.footer("Tab/Shift-Tab: field   Enter: next/connect   Ctrl+U: clear field   Esc: back")
            key = self.key()
            if key == "\x1b":
                return None
            if top is None:
                continue
            if key in ("\t", self.c.KEY_DOWN):
                focus = (focus + 1) % (len(fields) + 2)
            elif key in (self.c.KEY_BTAB, self.c.KEY_UP):
                focus = (focus - 1) % (len(fields) + 2)
            elif key in ("\n", "\r", self.c.KEY_ENTER):
                if focus == len(fields) + 1:
                    return None
                if focus < len(fields):
                    focus += 1
                else:
                    try:
                        return validate_details(mode, {**values, "vendor": vendor})
                    except ValueError as exc:
                        error = str(exc)
            elif focus < len(fields):
                name = fields[focus][0]
                values[name], cursors[name] = edit_text(values[name], cursors[name], key, self.c)

    @contextmanager
    def suspended(self):
        self.c.def_prog_mode()
        self.c.endwin()
        try:
            yield
        finally:
            self.c.reset_prog_mode()
            self.screen.keypad(True)
            self.screen.timeout(100)
            self.screen.clear()
            self.screen.refresh()

    def dialog(self, title, lines):
        offset = 0
        while True:
            top = self.frame(title, "REFERENCE / OPERATOR INFORMATION", compact=True)
            if top is not None:
                height, width = self.screen.getmaxyx()
                wrapped = [part for line in lines for part in textwrap.wrap(redact(line), width - 10)]
                available = max(1, height - top - 4)
                offset = max(0, min(offset, len(wrapped) - available))
                self.panel(top - 1, 3, available + 2, width - 6, "REFERENCE BUFFER", 1)
                for row, line in enumerate(wrapped[offset:offset + available], top):
                    self.put(row, 5, line, self.style(3))
                self.footer("Up/Down: scroll   Enter/Esc: continue   Ctrl+C: exit")
            key = self.key()
            if key in ("\n", "\r", self.c.KEY_ENTER, "\x1b"):
                return
            if key == self.c.KEY_DOWN:
                offset += 1
            elif key == self.c.KEY_UP:
                offset -= 1

    @staticmethod
    def axis_fraction(camera, values, axis):
        """A meter is valid only when a current reading and usable limits exist."""
        try:
            low, high = camera.ranges[axis]
            value = float(values[axis])
            if not all(math.isfinite(number) for number in (low, high, value)) or high <= low:
                return None
            if not low <= value <= high:
                return None
            return (value - low) / (high - low)
        except (KeyError, TypeError, ValueError):
            return None

    def plane_position(self, camera, values, columns, rows):
        pan = self.axis_fraction(camera, values or {}, "pan")
        tilt = self.axis_fraction(camera, values or {}, "tilt")
        if pan is None or tilt is None:
            return None
        return round((1 - tilt) * (rows - 1)), round(pan * (columns - 1))

    def draw_plane(self, camera, state, y, x, width):
        self.panel(y, x, 16, width, "POSITION PLANE // XY")
        columns, rows = width - 8, 9
        left, top = x + 4, y + 3
        zero_pan = self.axis_fraction(camera, {"pan": "0"}, "pan")
        zero_tilt = self.axis_fraction(camera, {"tilt": "0"}, "tilt")
        axis_x = round(zero_pan * (columns - 1)) if zero_pan is not None else 0
        axis_y = round((1 - zero_tilt) * (rows - 1)) if zero_tilt is not None else rows - 1
        for row in range(rows):
            line = []
            for column in range(columns):
                character = "." if row % 2 == 0 and column % 4 == 0 else " "
                if row == axis_y:
                    character = self.lines[1]
                if column == axis_x:
                    character = self.lines[3]
                if row == axis_y and column == axis_x:
                    character = self.lines[6]
                line.append(character)
            self.put(top + row, left, "".join(line), self.style(6), columns)
        self.put(top, left + axis_x, self.lines[7], self.style(1, True), 1)
        self.put(top + axis_y, left + columns - 1, self.lines[8], self.style(1, True), 1)
        position = self.plane_position(camera, state.current, columns, rows)
        starting = self.plane_position(camera, state.starting, columns, rows)
        for point, marker, pair in ((starting, "S", 5), (position, "@", 2)):
            if point is not None:
                self.put(top + point[0], left + point[1], marker, self.style(pair, True), 1)

        def bounds(axis):
            try:
                low, high = map(float, camera.ranges[axis])
                if not math.isfinite(low) or not math.isfinite(high) or low >= high:
                    return "unknown"
                return f"{low:g}..{high:g}"
            except (KeyError, TypeError, ValueError):
                return "unknown"

        self.put(y + 1, x + 2, " CAMERA COORDINATES " if position is not None
                 else " POSITION UNAVAILABLE ", self.style(1 if position is not None else 4, True), width - 4)
        self.put(y + 2, x + 2, "Y / TILT  " + bounds("tilt"), self.style(1), width - 4)
        self.put(y + 12, x + 2, "X / PAN   " + bounds("pan"), self.style(1), width - 4)
        self.put(y + 13, x + 2, "@ current   S start", self.style(2), width - 4)
        self.put(y + 14, x + 2, "Range view / zero off-scale" if zero_pan is None or zero_tilt is None
                 else "Camera units / not video", self.style(6), width - 4)

    def telemetry(self, camera, state, y, x, width):
        self.draw_plane(camera, state, y, x, width)
        self.panel(y + 17, x, 6, width, "AXIS TRAVEL // RANGE")
        for index, axis in enumerate(("pan", "tilt", "zoom")):
            fraction = self.axis_fraction(camera, state.current, axis)
            length = max(4, width - 16)
            fill = round(fraction * length) if fraction is not None else 0
            meter = "#" * fill + "." * (length - fill) if fraction is not None else "?" * length
            percent = f"{fraction * 100:3.0f}%" if fraction is not None else " N/A"
            self.put(y + 18 + index, x + 2,
                     f"{axis.upper():4} [{meter}] {percent}", self.style(2 if axis == "zoom" else 1),
                     width - 4)
        self.put(y + 21, x + 2, "% of reported axis limits", self.style(6), width - 4)

    def draw_dashboard(self, camera, host, mode, state, command, cursor, busy):
        top = self.frame("Camera control", f"{mode.upper()}  |  {host}  |  {'BUSY' if busy else 'CONNECTED'}",
                         compact=True)
        if top is None:
            return
        height, width = self.screen.getmaxyx()
        wide = width >= 112 and height >= 38
        sidebar = min(38, width // 3) if wide else 0
        main_width = width - sidebar - 7 if wide else width - 8
        self.put(top, 4, camera.name, self.style(5, True), main_width)
        roomy = height >= 32 and width >= 90
        row = top + (2 if roomy else 1)
        for label, values, style in (
            ("Starting coordinates", state.starting, self.style(2)),
            ("Current coordinates", state.current, self.style(1, True)),
        ):
            text = "  ".join(f"{axis.title()}:{(values or {}).get(axis, 'pending' if values is None else 'unknown')}"
                             for axis in ("pan", "tilt", "zoom"))
            parts = textwrap.wrap(text, main_width - 4)
            if wide:
                self.panel(row, 3, len(parts) + 2, main_width, label + ":", 2 if label.startswith("Starting") else 1)
                for offset, part in enumerate(parts, 1):
                    self.put(row + offset, 5, part, style, main_width - 4)
                row += len(parts) + 3
            else:
                self.put(row, 4, label + ": " + self.lines[1] * max(0, width - len(label) - 12), style)
                for part in parts:
                    row += 1
                    self.put(row, 6, part, style)
                row += 2 if roomy else 1
        ranges = "  ".join(f"{axis.title()} {low}..{high}" for axis, (low, high) in camera.ranges.items())
        if wide:
            self.put(row, 4, " ADVERTISED LIMITS ", self.style(9, True))
            row += 1
        for line in textwrap.wrap(ranges, main_width - 2):
            self.put(row, 4, line, self.style(3), main_width - 2)
            row += 1
        units = "AXIS: degrees + integer zoom" if isinstance(camera, AxisController) else "ONVIF: advertised absolute coordinates"
        for line in textwrap.wrap(units + " | x leaves an axis unchanged", main_width - 2):
            self.put(row, 4, line, self.style(2), main_width - 2)
            row += 1
        row += 2 if roomy else 1
        if wide:
            self.telemetry(camera, state, top + 1, width - sidebar - 3, sidebar)
        boxed_activity = height - 7 - row >= 4
        if boxed_activity:
            self.panel(row, 3, height - 6 - row, main_width, "EVENT BUFFER // SESSION", 6)
            row += 1
        latest_y = height - (8 if boxed_activity else 7)
        remaining = max(0, latest_y - row)
        for line in list(state.activity)[:-1][-remaining:] if remaining else []:
            self.put(row, 5, line, self.style(3), main_width - 4)
            row += 1
        if state.activity:
            self.put(latest_y, 5, state.activity[-1], self.style(3), main_width - 4)
        elif remaining:
            self.put(row, 5, "Activity messages appear here.", self.style(6), main_width - 4)
        refresh = "PAUSED" if state.paused else "2s read-only refresh"
        self.put(height - 6, 4, "[ REQUEST ]" if busy else "[ READS PAUSED ]" if state.paused
                 else "[ POSITION OK ]" if state.current else "[ NO POSITION ]",
                 self.style(2 if busy or state.paused else 5 if state.current else 4, True))
        self.put(height - 6, 25, refresh
                 + (" | BUSY" if busy else " | ready"), self.style(5))
        available = width - 10
        offset = max(0, cursor - available + 1)
        self.put(height - 5, 4, "> " + command[offset:], self.style(7, True))
        self.put(height - 4, 4, " " * (2 + cursor - offset) + "^", self.style(2))
        self.put(height - 3, 3, "O: return to starting coordinates", self.style(4, True))
        self.footer("Enter:send  V:view  R:refresh  P:pause  ?:help  Esc:back  Q:quit")

    def coordinate_dialog(self, camera):
        self.dialog("Coordinate help", [
            camera.units,
            "a b c = all | x b c = keep pan | a x c = keep tilt | a b x = keep zoom",
            "Use numbers in place of a/b/c; x can appear in any field. Omitted zoom stays unchanged.",
            "Starting coordinates stay fixed. Current coordinates refresh every 2 seconds unless paused.",
            "O (empty input): return to known starting coordinates. This sends an absolute move.",
            "V (empty input): open the camera web interface. HTTP first, HTTPS fallback.",
            "Values are absolute targets, not increments. Esc/Q disconnect; neither issues PTZ Stop.",
        ])

    def dashboard(self, camera, host, process, mode):
        state = PositionState()
        command, cursor = "", 0
        history, history_index = [], 0
        view = camera.web_view
        web_pending = False
        web_requested = False
        executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="ptz")
        future = executor.submit(camera_job, camera, host)
        next_refresh = time.monotonic() + 2
        try:
            while True:
                if process is not None and process.poll() is not None:
                    raise RuntimeError("SSH connection closed; reconnect to continue")
                if future is not None and future.done():
                    try:
                        state.apply(future.result())
                    except Exception as exc:
                        if state.starting is None:
                            state.starting = {}
                        state.current = {}
                        state.activity.append("Command failed: " + redact(exc))
                    if web_pending:
                        web_pending = False
                        if view.url and not view.open():
                            self.dialog("Open camera web interface", [
                                "The browser could not be opened automatically. Open this address:",
                                view.url,
                            ])
                        elif not view.url:
                            self.dialog("Camera web interface unavailable", [view.error])
                    future = None
                    next_refresh = time.monotonic() + 2
                if web_requested and future is None:
                    web_requested = False
                    web_pending = True
                    future = executor.submit(view.prepare)
                if future is None and not state.paused and time.monotonic() >= next_refresh:
                    future = executor.submit(camera_job, camera, host)
                self.draw_dashboard(camera, host, mode, state, command, cursor, future is not None)
                key = self.key()
                if key in ("\x1b", self.c.KEY_F10):
                    return "back"
                if not command and key in ("q", "Q"):
                    return "quit"
                height, width = self.screen.getmaxyx()
                if height < 24 or width < 76:
                    continue
                if not command and key in ("r", "R"):
                    state.failures, state.paused = 0, False
                    next_refresh = 0
                elif not command and key in ("p", "P"):
                    state.paused = not state.paused
                elif not command and key in ("o", "O"):
                    try:
                        if future is not None:
                            raise ValueError("Request in progress; wait before returning to start")
                        values, message = starting_move(state.starting)
                        state.activity.append(message)
                        future = executor.submit(camera_job, camera, host, values)
                    except Exception as exc:
                        state.activity.append(redact(exc))
                elif not command and key in ("v", "V"):
                    if view.url:
                        if not view.open():
                            self.dialog("Open camera web interface", [view.url])
                    else:
                        web_requested = True
                        if future is not None:
                            state.activity.append("Camera view will open after the current request")
                elif not command and key == "?":
                    self.coordinate_dialog(camera)
                elif key in ("\n", "\r", self.c.KEY_ENTER):
                    if command.strip().lower() in ("q", "quit", "exit"):
                        return "quit"
                    if command.strip().lower() in ("help", "?"):
                        self.coordinate_dialog(camera)
                        command, cursor = "", 0
                    elif command.strip():
                        try:
                            values = parse_coordinates(command)
                            if future is not None:
                                raise ValueError("Request in progress; wait before sending another move")
                            future = executor.submit(camera_job, camera, host, values)
                            history.append(command)
                            history_index = len(history)
                            command, cursor = "", 0
                        except Exception as exc:
                            state.activity.append(redact(exc))
                elif key in (self.c.KEY_UP, self.c.KEY_DOWN) and history:
                    step = -1 if key == self.c.KEY_UP else 1
                    history_index = max(0, min(len(history), history_index + step))
                    command = history[history_index] if history_index < len(history) else ""
                    cursor = len(command)
                else:
                    command, cursor = edit_text(command, cursor, key, self.c)
        finally:
            self.frame("Disconnecting", "Waiting for any in-flight camera request; no new moves will be queued")
            self.screen.refresh()
            executor.shutdown(wait=True, cancel_futures=True)

    def run(self, terminal):
        global password, host_password
        while True:
            mode = "remote" if args.ssh else "local" if args.local else self.menu("Choose Connection", [
                ("Local machine", "Reach the camera directly from this computer; no SSH.", "local"),
                ("Remote host (SSH)", "Reach the camera through a remote site's SSH host.", "remote"),
            ])
            if mode is None:
                return
            vendor = args.vendor or self.menu("Choose Camera Vendor", [
                ("Bosch/Milesight", "ONVIF absolute coordinates and camera ONVIF credentials.", "onvif"),
                ("Axis", "Native VAPIX with HTTP Digest; degrees and integer zoom.", "axis"),
            ])
            if vendor is None:
                if args.ssh or args.local:
                    return
                continue
            details = self.form(mode, vendor)
            if details is None:
                if (args.ssh or args.local) and args.vendor:
                    return
                continue
            password = details["camera_password"]
            host_password = details.get("ssh_password", "")
            try:
                with ExitStack() as stack:
                    if mode == "remote":
                        # Dependency approval and key-based SSH prompts use the real terminal.
                        with self.suspended():
                            camera, host, process = stack.enter_context(camera_session(mode, details, terminal))
                    else:
                        self.frame("Connecting", f"{details['camera_host']} | {vendor.upper()} | no movement sent")
                        self.screen.refresh()
                        camera, host, process = stack.enter_context(camera_session(mode, details, terminal))
                    if self.dashboard(camera, host, process, mode) == "quit":
                        return
            except Exception as exc:
                self.dialog("Connection error", [redact(exc), "Press Enter to return to connection setup."])
            finally:
                details["camera_password"] = ""
                details["ssh_password"] = ""
                host_password = None
                password = os.environ.get("PTZ_PASSWORD")


def run_tui():
    if not os.isatty(sys.stdout.fileno()) or os.environ.get("TERM") in (None, "", "dumb"):
        raise RuntimeError("The TUI needs an interactive terminal; use --plain instead")
    try:
        import curses
    except ImportError:
        with open("/dev/tty", "r") as terminal:
            ensure_component("curses", terminal)
        import curses
    # Python source used stdin, so attach curses input to the controlling terminal.
    with open("/dev/tty", "r") as terminal:
        saved_stdin = os.dup(0)
        try:
            os.dup2(terminal.fileno(), 0)
            curses.wrapper(lambda screen: TerminalUI(screen).run(terminal))
        finally:
            os.dup2(saved_stdin, 0)
            os.close(saved_stdin)


def main():
    global password, host_password
    password = os.environ.get("PTZ_PASSWORD")
    if not interactive:
        if password is None:
            password = getpass.getpass("Camera Password: ", stream=sys.stderr)
        camera = connect_camera(args.host, args.port, args.user, password, vendor=args.vendor or "onvif")
        try:
            print(f"{camera.name}\n{camera.units}")
            show_position(camera, initial=True)
            if camera.move(args.host, args.pan, args.tilt, args.zoom):
                show_position(camera)
        finally:
            camera.close()
        return

    if not args.plain:
        run_tui()
        return

    # Source code occupies stdin; all interactive input uses the terminal.
    with open("/dev/tty", "r") as terminal:
        welcome()
        mode = "remote" if args.ssh else "local" if args.local else choose_connection(terminal)
        vendor = args.vendor or choose_vendor(terminal)
        details = {"vendor": vendor}
        if mode == "remote":
            details["ssh_host"] = validate_host(prompt(terminal, "Host IP/DNS Name"))
            details["ssh_user"] = prompt(terminal, "Username")
            host_password = getpass.getpass("Host Password (blank for keys/SSH prompt): ", stream=sys.stderr)
            details["ssh_password"] = host_password
            details["ssh_port"] = prompt_port(terminal, "SSH Port", 22)
        camera_host = validate_host(prompt(terminal, "Camera IP"))
        camera_port = prompt_port(terminal, "Camera HTTP Port", args.port)
        camera_user = prompt(terminal, "Camera Username")
        if password is None:
            password = getpass.getpass("Camera Password: ", stream=sys.stderr)
        details.update(camera_host=camera_host, camera_port=camera_port,
                       camera_user=camera_user, camera_password=password)
        try:
            with camera_session(mode, details, terminal) as (camera, host, process):
                coordinate_loop(terminal, camera, host, process)
        except Exception as exc:
            # The outer reporter runs after secret references have been cleared.
            raise RuntimeError(redact(exc)) from None
        finally:
            details["ssh_password"] = ""
            details["camera_password"] = ""
            host_password = None


try:
    main()
except KeyboardInterrupt:
    print("\nSession closed; SSH tunnel cleaned up.", file=sys.stderr)
    sys.exit(130)
except EOFError:
    print("\nSession closed.", file=sys.stderr)
except Exception as exc:
    report_error(exc)
    sys.exit(1)
PYTHON
