#!/bin/sh
# Builds and tests the current commit on the three machines before merging, as well as GitHub Actions (which only
# builds Linux and Windows, and runs no app tests there).
#
#   scripts/check.sh            every machine
#   scripts/check.sh mac linux  only those
#   scripts/check.sh -w ...     send the working tree as it is, uncommitted changes included
#
# - Mac (here, on this checkout): macos/scripts/build.sh, scripts/test-core.sh, macos/scripts/test.sh.
# - Linux, through Master Control (mc-exec): linux/scripts/build.sh (the Flatpak), scripts/test-core.sh, then linux/scripts/test.sh on that
#   build through `flatpak build` (PTZ_TEST_BUILD), so the installed PTZ Control stays as it is. The files go to
#   ~/.cache/ptz-check/<commit>, not /tmp, which flatpak-builder's sandbox can't see.
# - Windows, through Master Control in the signed-in desktop session, which the app test's windows need: windows\scripts\build.ps1 -NoDesktopCopy (the desktop keeps its copy), windows\scripts\test.ps1.
# - Security, on the Mac only, before the machines and whenever the Mac is checked: `gitleaks detect` scans the
#   whole git history for secrets and fails on any finding (a false positive goes in .gitleaksignore, with a comment).
#   The core is C++, so there is no Cargo.lock for cargo audit. gitleaks is installed on the Mac from
#   ~/.agents/config/tools.json (`python3 ~/.agents/sync/agents.py tools --install`).
# Without -w, Linux and Windows test the last commit, sent as files (no clone needed there), so their builds say
# version x.y.0. Prints one line for security and one per machine and exits non-zero if any failed. Logs go to
# .build/check/<machine>.log and security.log.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
# The Master Control names of the Linux and Windows machines (`mctl machines`): linux and windows, or others from
# the environment or an untracked scripts/hosts.env
# (see scripts/hosts.env.example) in this checkout or in the main one, so worktrees find it too.
# A name set in the environment wins over the file's.
ENV_LINUX="${PTZ_LINUX_HOST:-}" ENV_WINDOWS="${PTZ_WINDOWS_HOST:-}"
for hosts in "$ROOT/scripts/hosts.env" "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)/../scripts/hosts.env"; do
    if [ -f "$hosts" ]; then . "$hosts"; break; fi
done
LINUX_HOST="${ENV_LINUX:-${PTZ_LINUX_HOST:-linux}}"
WINDOWS_HOST="${ENV_WINDOWS:-${PTZ_WINDOWS_HOST:-windows}}"
LOGS="$ROOT/.build/check"
mkdir -p "$LOGS"
# Old logs go first: a machine that never got to run would otherwise be judged by the last run's log.
rm -f "$LOGS"/*.status "$LOGS"/*.log
WORKTREE=0
if [ "${1:-}" = "-w" ]; then
    WORKTREE=1
    shift
fi
MACHINES="${*:-mac linux windows}"
# The files to test, as a tar stream: the last commit, or with -w every file git would track now.
send() {
    if [ "$WORKTREE" = 1 ]; then
        git ls-files -co --exclude-standard -z | COPYFILE_DISABLE=1 tar --no-xattrs -cf - --null -T -
    else
        git archive HEAD
    fi
}
if [ "$WORKTREE" = 0 ] && [ -n "$(git status --porcelain)" ]; then
    echo "check.sh: there are uncommitted changes; Linux and Windows test the last commit" >&2
fi
COMMIT="$(git rev-parse --short HEAD)"
# One folder per commit on the other machines, so two checks at once don't delete each other's files.
DIR="ptz-check-$COMMIT"

# The tar both machines get, written once; mc-exec copies it there and the machine unpacks it.
TAR="$LOGS/$DIR.tar"
case " $MACHINES " in *" linux "* | *" windows "*) send >"$TAR" ;; esac

# Security first, here on the Mac; its line is printed before the machines'.
case " $MACHINES " in *" mac "*)
    (PATH="$HOME/.local/bin:$PATH" gitleaks detect --no-banner --no-color --redact 2>&1
     echo $? >"$LOGS/security.status") >"$LOGS/security.log" 2>&1 ;;
esac

for machine in $MACHINES; do
    case "$machine" in
    mac)
        (macos/scripts/build.sh 2>&1 && scripts/test-core.sh 2>&1 && macos/scripts/test.sh 2>&1
         echo $? >"$LOGS/mac.status") >"$LOGS/mac.log" 2>&1 &
        ;;
    linux)
        (mc-exec "$LINUX_HOST" --put "$TAR" "/tmp/$DIR.tar" \
         && mc-exec "$LINUX_HOST" --timeout 1900 "D=\$HOME/.cache/ptz-check/$DIR; rm -rf \$D && mkdir -p \$D \
            && tar -x -C \$D -f /tmp/$DIR.tar && rm -f /tmp/$DIR.tar && cd \$D && timeout 1800 sh -c 'linux/scripts/build.sh 2>&1 && scripts/test-core.sh 2>&1 \
            && PTZ_TEST_BUILD=\$PWD/.build/flatpak/build linux/scripts/test.sh 2>&1'; code=\$?; cd && rm -rf \$D /tmp/$DIR.tar; exit \$code"
         echo $? >"$LOGS/linux.status") >"$LOGS/linux.log" 2>&1 &
        ;;
    windows)
        # Folders an interrupted check left more than a day ago are cleared first.
        (WTEMP="$(mc-exec "$WINDOWS_HOST" "echo %TEMP%" | tr -d '\r\n')" && [ -n "$WTEMP" ] \
         && mc-exec "$WINDOWS_HOST" --put "$TAR" "$WTEMP\\$DIR.tar" \
         && mc-exec "$WINDOWS_HOST" --timeout 3600 "(forfiles /p %TEMP% /m ptz-check-* /d -1 /c \"cmd /c if @isdir==TRUE rmdir /s /q @path\" >nul 2>&1) \
            & (if exist %TEMP%\\$DIR rmdir /s /q %TEMP%\\$DIR) & mkdir %TEMP%\\$DIR \
            && tar -x -C %TEMP%\\$DIR -f %TEMP%\\$DIR.tar && del /q %TEMP%\\$DIR.tar && cd /d %TEMP%\\$DIR \
            && powershell -NoProfile -ExecutionPolicy Bypass -File windows\\scripts\\build.ps1 -NoDesktopCopy 2>&1 \
            && powershell -NoProfile -ExecutionPolicy Bypass -File windows\\scripts\\test.ps1 2>&1"
         echo $? >"$LOGS/windows.status"
         mc-exec "$WINDOWS_HOST" "cd /d %TEMP% & rmdir /s /q %TEMP%\\$DIR & del /q %TEMP%\\$DIR.tar" >/dev/null 2>&1) >"$LOGS/windows.log" 2>&1 &
        ;;
    *)
        echo "check.sh: unknown machine $machine (mac, linux, windows)" >&2
        exit 2
        ;;
    esac
done
wait
rm -f "$TAR"

failed=0
if [ -f "$LOGS/security.status" ]; then
    code="$(cat "$LOGS/security.status")"
    if [ "$code" = 0 ]; then
        echo "security: passed (gitleaks) at $COMMIT"
    else
        failed=1
        echo "security: FAILED (exit $code) at $COMMIT; see .build/check/security.log"
        grep -E 'leaks found|^(RuleID|File|Commit):|not found' "$LOGS/security.log" | head -12 | sed 's/^/    /'
    fi
fi
for machine in $MACHINES; do
    code="$(cat "$LOGS/$machine.status" 2>/dev/null || echo none)"
    oks="$(grep -c '^ok ' "$LOGS/$machine.log" 2>/dev/null)"
    if [ "$code" = 0 ]; then
        echo "$machine: passed (${oks:-0} checks) at $COMMIT"
    else
        failed=1
        echo "$machine: FAILED (exit $code) at $COMMIT; see .build/check/$machine.log"
        grep -E '^FAIL|error|Error|failed|^mc-exec:|Could not' "$LOGS/$machine.log" 2>/dev/null | head -12 | sed 's/^/    /'
    fi
done
exit $failed
