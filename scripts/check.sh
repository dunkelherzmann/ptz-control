#!/bin/sh
# Builds and tests the current commit on the three machines before merging, as well as GitHub Actions (which only
# builds Linux and Windows, and runs no app tests there).
#
#   scripts/check.sh            every machine
#   scripts/check.sh mac linux  only those
#   scripts/check.sh -w ...     send the working tree as it is, uncommitted changes included
#
# - Mac (here, on this checkout): macos/scripts/build.sh, scripts/test-core.sh, macos/scripts/test.sh.
# - Linux, over ssh: linux/scripts/build.sh (the Flatpak), scripts/test-core.sh, then linux/scripts/test.sh on that
#   build through `flatpak build` (PTZ_TEST_BUILD), so the installed PTZ Control stays as it is. The files go to
#   ~/.cache/ptz-check/<commit>, not /tmp, which flatpak-builder's sandbox can't see.
# - Windows, over ssh and then in the signed-in desktop session (mctl, from the master-control skill), which the app
#   test's windows need: windows\scripts\build.ps1 -NoDesktopCopy (the desktop keeps its copy), windows\scripts\test.ps1.
# Without -w, Linux and Windows test the last commit, sent as files (no clone needed there), so their builds say
# version x.y.0. Prints one line per machine and exits non-zero if any failed. Logs go to .build/check/<machine>.log.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
# The ssh names of the Linux and Windows machines: from the environment, or from an untracked scripts/hosts.env
# (see scripts/hosts.env.example) in this checkout or in the main one, so worktrees find it too.
for hosts in "$ROOT/scripts/hosts.env" "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)/../scripts/hosts.env"; do
    if [ -f "$hosts" ]; then . "$hosts"; break; fi
done
LINUX_HOST="${PTZ_LINUX_HOST:-linux}"
WINDOWS_HOST="${PTZ_WINDOWS_HOST:-windows}"
# The Windows machine's name in Master Control (`mctl status` lists them).
WINDOWS_MCTL="${PTZ_WINDOWS_MCTL:-windows}"
LOGS="$ROOT/.build/check"
mkdir -p "$LOGS"
rm -f "$LOGS"/*.status
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

for machine in $MACHINES; do
    case "$machine" in
    mac)
        (macos/scripts/build.sh 2>&1 && scripts/test-core.sh 2>&1 && macos/scripts/test.sh 2>&1
         echo $? >"$LOGS/mac.status") >"$LOGS/mac.log" 2>&1 &
        ;;
    linux)
        (send | ssh -o ConnectTimeout=15 "$LINUX_HOST" "D=\$HOME/.cache/ptz-check/$DIR; rm -rf \$D && mkdir -p \$D \
            && tar -x -C \$D && cd \$D && timeout 1800 sh -c 'linux/scripts/build.sh 2>&1 && scripts/test-core.sh 2>&1 \
            && PTZ_TEST_BUILD=\$PWD/.build/flatpak/build linux/scripts/test.sh 2>&1'; code=\$?; cd && rm -rf \$D; exit \$code" \
            >"$LOGS/linux.log" 2>&1
         echo $? >"$LOGS/linux.status") &
        ;;
    windows)
        # The folder's full path comes back from `cd`, for mctl's --cwd. Folders an interrupted check left more than a
        # day ago are cleared first.
        (send | ssh -o ConnectTimeout=15 "$WINDOWS_HOST" "(forfiles /p %TEMP% /m ptz-check-* /d -1 /c \"cmd /c if @isdir==TRUE rmdir /s /q @path\" >nul 2>&1) \
            & (if exist %TEMP%\\$DIR rmdir /s /q %TEMP%\\$DIR) & mkdir %TEMP%\\$DIR \
            && tar -x -C %TEMP%\\$DIR && cd /d %TEMP%\\$DIR && cd" >"$LOGS/windows.where" 2>&1 \
         && mctl exec "$WINDOWS_MCTL" --timeout 1800 --cwd "$(tail -1 "$LOGS/windows.where" | tr -d '\r')" -- \
            cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File windows\\scripts\\build.ps1 -NoDesktopCopy 2>&1 \
                && powershell -NoProfile -ExecutionPolicy Bypass -File windows\\scripts\\test.ps1 2>&1 && echo PTZ_CHECK_PASSED" \
            >"$LOGS/windows.log" 2>&1
         grep -q PTZ_CHECK_PASSED "$LOGS/windows.log"; echo $? >"$LOGS/windows.status"
         ssh -o ConnectTimeout=15 "$WINDOWS_HOST" "cd /d %TEMP% & rmdir /s /q %TEMP%\\$DIR" >/dev/null 2>&1) &
        ;;
    *)
        echo "check.sh: unknown machine $machine (mac, linux, windows)" >&2
        exit 2
        ;;
    esac
done
wait

failed=0
for machine in $MACHINES; do
    code="$(cat "$LOGS/$machine.status" 2>/dev/null || echo none)"
    oks="$(grep -c '^ok ' "$LOGS/$machine.log" 2>/dev/null)"
    if [ "$code" = 0 ]; then
        echo "$machine: passed (${oks:-0} checks) at $COMMIT"
    else
        failed=1
        echo "$machine: FAILED (exit $code) at $COMMIT; see .build/check/$machine.log"
        grep -E '^FAIL|error|Error|failed' "$LOGS/$machine.log" | head -12 | sed 's/^/    /'
    fi
done
exit $failed
