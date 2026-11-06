#!/usr/bin/env bash
# sync_and_build.sh -- run from WSL: copy the source tree (+ generated assets/
# and certs/) to a Windows-native folder and build drifto.exe there with MSVC.
# The WSL repo stays the source of truth; the Windows copy is disposable
# (only its build/windows and vcpkg caches are worth keeping).
#
#   tools/windows/sync_and_build.sh            sync + build (Release x64)
#   tools/windows/sync_and_build.sh --sync     sync only
#   tools/windows/sync_and_build.sh --clean    sync + clean rebuild
#   WINDIR_DRIFTO=/mnt/c/some/where tools/windows/sync_and_build.sh
#
# Default destination: C:\Users\<you>\source\repos\drifto (not \\wsl$: MSVC
# and vcpkg are slow and flaky on the 9P share).
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC="$PWD"

if [[ -z "${WINDIR_DRIFTO:-}" ]]; then
    WINUSER="$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')"
    WINDIR_DRIFTO="/mnt/c/Users/${WINUSER}/source/repos/drifto"
fi
DEST="$WINDIR_DRIFTO"
WINDEST="$(wslpath -w "$(dirname "$DEST")")\\$(basename "$DEST")"

[[ -d assets ]] || echo "[sync] warning: no assets/ (run 'make assets' first); the game will use fallback meshes"
[[ -f certs/ca.pem ]] || echo "[sync] warning: no certs/ (run 'make certs'); Windows players will need --insecure"

mkdir -p "$DEST"
echo "[sync] $SRC -> $DEST"
# --delete keeps the copy identical, but never touches its build/ (vcpkg + objects)
rsync -a --delete \
    --exclude='.git/' --exclude='/build/' --exclude='*.o' --exclude='*.d' \
    --exclude='/drifto' --exclude='/api_server' --exclude='/game_server' --exclude='/net_tests' \
    --exclude='/game_tests' --exclude='/gfx_demo' --exclude='/lan_probe' --exclude='/audio_demo' --exclude='/server' --exclude='/client' \
    --exclude='/client_cpp' --exclude='/server_cpp' --exclude='/players.auth' --exclude='/messages.[ch]' \
    --exclude='certs/*.key.pub' --exclude='__pycache__/' \
    ./ "$DEST/"

[[ "${1:-}" == "--sync" ]] && exit 0

ARG=""
[[ "${1:-}" == "--clean" ]] && ARG="clean"
echo "[build] cmd.exe: $WINDEST\\tools\\windows\\build.bat $ARG"
cd /mnt/c   # cmd.exe refuses a \\wsl$ working directory
cmd.exe /c "cd /d $WINDEST && tools\\windows\\build.bat $ARG"
echo "[build] run it:  $WINDEST\\build\\windows\\Release\\drifto.exe"
