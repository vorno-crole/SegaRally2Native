#!/bin/bash
# UPDATE NOW on macOS (src/ui/frontend.cpp, mac_start_update): the game opens
# this in Terminal and closes. It waits for the game to be gone, merges the
# original project's main branch (snicolast/SegaRally2Native) into the branch
# checked out here, recompiles the game's code from data/crom.bin, builds the
# game again and starts it. Run it by hand any time to do the same.
#
# It stops, changing nothing, when there are uncommitted changes to tracked
# files, and undoes the merge when it has conflicts (macOS code to adapt by
# hand). SR2_UPDATE_NO_LAUNCH=1: do not start the game at the end (tests).

cd "$(dirname "$0")/.." || exit 1
ROOT="$PWD"

fail() {
    echo
    echo "UPDATE STOPPED: $*"
    echo "Press Return to close this window."
    read -r
    exit 1
}

echo "Sega Rally 2 Native: update (macOS)"
echo "In $ROOT"
echo

# The game writes its process id here before it opens this window
if [ -f out/update.pid ]; then
    pid=$(cat out/update.pid)
    rm -f out/update.pid
    for _ in $(seq 1 200); do   # up to 20 s
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    kill -0 "$pid" 2>/dev/null && fail "the game (process $pid) is still running; close it and run tools/update_mac.command again."
fi

command -v git >/dev/null || fail "git not found (xcode-select --install)."
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "$ROOT is not a git checkout."
if ! git diff --quiet || ! git diff --cached --quiet; then
    git status --short --untracked-files=no
    fail "there are uncommitted changes (above). Commit or stash them first."
fi

# The original project's remote: the one already set up, else 'upstream'
remote=$(git remote -v | awk '$2 ~ /github\.com[:\/]snicolast\/SegaRally2Native(\.git)?$/ && $3 == "(fetch)" {print $1; exit}')
if [ -z "$remote" ]; then
    remote=upstream
    echo "Adding the remote 'upstream' (github.com/snicolast/SegaRally2Native)"
    git remote add upstream https://github.com/snicolast/SegaRally2Native.git || fail "could not add the remote."
fi

echo "Downloading the new version ($remote/main)..."
git fetch "$remote" || fail "the download failed (git fetch $remote)."
if git merge-base --is-ancestor "$remote/main" HEAD; then
    echo "Already up to date with $remote/main."
else
    echo "Merging $remote/main into $(git rev-parse --abbrev-ref HEAD)..."
    if ! git merge --no-edit "$remote/main"; then
        git diff --name-only --diff-filter=U
        git merge --abort
        fail "the new version conflicts with the macOS changes in the files above. Nothing was changed; merge $remote/main by hand."
    fi
fi

BREW=$(command -v brew || echo /opt/homebrew/bin/brew)
PREFIX=$("$BREW" --prefix 2>/dev/null || echo /opt/homebrew)
PY="$PREFIX/bin/python3"
[ -x "$PY" ] || fail "Homebrew's python3 not found ($PY): brew install python"
[ -f data/crom.bin ] || fail "data/crom.bin not found: read your ROM first (README, Building on a Mac, step 3)."

echo
echo "Recompiling the game's code from data/crom.bin..."
"$PY" tools/recomp.py data/crom.bin build/gen || fail "tools/recomp.py failed. The source is updated, the game is still the old build."

echo
echo "Building..."
if [ ! -f build/mac/build.ninja ]; then
    LLVM="$PREFIX/opt/llvm"
    cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_IN_SOURCE_DIR=ON -DPython3_EXECUTABLE="$PY" \
        -DCMAKE_C_COMPILER="$LLVM/bin/clang" -DCMAKE_CXX_COMPILER="$LLVM/bin/clang++" \
        -DCMAKE_OBJCXX_COMPILER="$LLVM/bin/clang++" || fail "cmake could not configure build/mac."
fi
cmake --build build/mac || fail "the build failed (above). The source is updated, the game is still the old build: after a fix, cmake --build build/mac."

echo
echo "Updated: $(git log -1 --format='%h %s')"
[ -n "$SR2_UPDATE_NO_LAUNCH" ] && exit 0
echo "Starting the game..."
exec ./SegaRally2
