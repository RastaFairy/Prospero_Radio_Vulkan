#!/usr/bin/env bash
set -euo pipefail

# This launcher is safe to invoke directly from /mnt/<drive> under WSL.
# The long-running build is re-executed from WSL's Linux filesystem so bash,
# Git, chmod, and native tooling never depend on DrvFs/NTFS semantics.
MODE="${1:-packages}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPSTREAM_URL="https://github.com/blackbearreloaded/ProsperoRadio.git"
UPSTREAM_SHA="33898dd35375c1ae8370da137cfb6941d91c7684"
LINUX_WORKSPACE="${PROSPERO_BUILD_ROOT:-$HOME/.cache/prospero-radio-modernized}"
HOST_ROOT="${PROSPERO_HOST_ROOT:-$SCRIPT_DIR}"
STAGE_READY="${PROSPERO_STAGE_READY:-0}"

# If this wrapper lives on a Windows-mounted drive, copy only the launcher and
# overlay to a Linux filesystem and re-exec from there before doing any long work.
if [[ "$STAGE_READY" != "1" && "$SCRIPT_DIR" == /mnt/* ]]; then
    STAGE_DIR="$LINUX_WORKSPACE/launcher"
    rm -rf "$STAGE_DIR"
    mkdir -p "$STAGE_DIR"
    cp "$SCRIPT_DIR/build.sh" "$STAGE_DIR/build.sh"
    cp -R "$SCRIPT_DIR/overlay" "$STAGE_DIR/overlay"
    chmod +x "$STAGE_DIR/build.sh"
    exec env \
        PROSPERO_BUILD_ROOT="$LINUX_WORKSPACE" \
        PROSPERO_HOST_ROOT="$SCRIPT_DIR" \
        PROSPERO_STAGE_READY=1 \
        bash "$STAGE_DIR/build.sh" "$MODE"
fi

ROOT_DIR="$HOST_ROOT"
WORK_DIR="$LINUX_WORKSPACE/ProsperoRadio-$UPSTREAM_SHA"

mkdir -p "$ROOT_DIR/out" "$LINUX_WORKSPACE"

echo "==> Materializing pinned ProsperoRadio source"
if [[ ! -d "$WORK_DIR/.git" ]]; then
    git clone "$UPSTREAM_URL" "$WORK_DIR"
fi

cd "$WORK_DIR"
git fetch --depth 1 origin "$UPSTREAM_SHA"
git checkout --detach "$UPSTREAM_SHA"
git reset --hard "$UPSTREAM_SHA"
git clean -fd

python3 "$SCRIPT_DIR/overlay/check-runtime-texture-budget.py"
python3 "$SCRIPT_DIR/overlay/apply-vulkan.py" "$WORK_DIR"

# Make does not reliably notice overlay replacement when its timestamps come
# from Windows/DrvFs. Tie generated objects to the exact overlay and launcher
# contents, and force a clean rebuild whenever that input fingerprint changes.
OVERLAY_STAMP="$LINUX_WORKSPACE/overlay-$UPSTREAM_SHA.sha256"
OVERLAY_FINGERPRINT="$({
    printf '%s\n' "$UPSTREAM_SHA"
    sha256sum "$SCRIPT_DIR/build.sh"
    find "$SCRIPT_DIR/overlay" -type f ! -path '*/__pycache__/*' -print0 |
        LC_ALL=C sort -z | xargs -0 sha256sum
} | sha256sum | awk '{print $1}')"

if [[ "$MODE" != "clean" ]]; then
    PREVIOUS_OVERLAY_FINGERPRINT=""
    if [[ -f "$OVERLAY_STAMP" ]]; then
        PREVIOUS_OVERLAY_FINGERPRINT="$(cat "$OVERLAY_STAMP")"
    fi
    if [[ "$PREVIOUS_OVERLAY_FINGERPRINT" != "$OVERLAY_FINGERPRINT" ]]; then
        echo "==> Overlay/build inputs changed; removing stale generated objects"
        make clean
    fi
fi

if [[ "$MODE" == "clean" ]]; then
    make clean
    exit 0
fi

# Build/stage the real PS5 Vulkan implementation. The driver is statically
# linked into the title; no runtime dlopen() path is used. The cache is kept
# outside the Git worktree because git clean -fd resets the worktree each run.
PS5_VULKAN_CACHE="$LINUX_WORKSPACE/deps"
bash "$SCRIPT_DIR/overlay/setup-ps5-vulkan.sh" "$WORK_DIR" "$PS5_VULKAN_CACHE"

mapfile -t VULKAN_LIBS < <(cd "$WORK_DIR" && find .local/vulkan/lib -maxdepth 1 -type f -name '*.a' -print | sort)
mapfile -t VULKAN_STUBS < <(cd "$WORK_DIR" && find .local/vulkan/imports -maxdepth 1 -type f -name 'libSce*.so' -print | sort)
((${#VULKAN_LIBS[@]} > 0)) || { echo "No staged PS5 Vulkan archives" >&2; exit 1; }
((${#VULKAN_STUBS[@]} > 0)) || { echo "No staged PS5 Vulkan SCE import stubs" >&2; exit 1; }

VULKAN_LIB_ARGS="${VULKAN_LIBS[*]}"
VULKAN_STUB_ARGS="${VULKAN_STUBS[*]}"
mapfile -t VULKAN_EXTRA_OBJECTS < <(cd "$WORK_DIR" && find build/obj/mesa-util -maxdepth 1 -type f -name '*.o' -print | sort)
VULKAN_EXTRA_ARGS="${VULKAN_EXTRA_OBJECTS[*]:-}"

# These are ordinary Make variables consumed by the original ProsperoRadio
# toolchain. The original app sources and static service libraries remain in
# the upstream Makefile; we only append the Vulkan SDK/driver pieces here.
export APP_INCLUDE_PATHS=".local/vulkan/include ${APP_INCLUDE_PATHS:-}"
export APP_VULKAN_ARCHIVES="${VULKAN_LIB_ARGS}"
export APP_EXTRA_OBJECTS="${VULKAN_EXTRA_ARGS} ${APP_EXTRA_OBJECTS:-}"
export APP_STATIC_ARCHIVES="${APP_STATIC_ARCHIVES:-}"
export APP_IMPORT_STUBS="${VULKAN_STUB_ARGS} ${APP_IMPORT_STUBS:-}"

case "$MODE" in
    packages)
        make packages
        ;;
    app)
        make app
        ;;
    check)
        make check
        ;;
    clean)
        make clean
        ;;
    *)
        echo "Unknown build mode: $MODE" >&2
        echo "Use: packages | app | check | clean" >&2
        exit 2
        ;;
esac

if [[ "$MODE" == "packages" || "$MODE" == "app" || "$MODE" == "check" ]]; then
    OUT_DIR="$ROOT_DIR/out"
    rm -rf "$OUT_DIR/PPSA99001"
    cp -r "$WORK_DIR/dist/PPSA99001" "$OUT_DIR/"
    for artifact in "$WORK_DIR"/dist/PPSA99001.*; do
        [[ -e "$artifact" ]] || continue
        cp -f "$artifact" "$OUT_DIR/"
    done

    stamp_tmp="$OVERLAY_STAMP.tmp.$$"
    printf '%s\n' "$OVERLAY_FINGERPRINT" > "$stamp_tmp"
    mv -f "$stamp_tmp" "$OVERLAY_STAMP"

    echo
    echo "============================================================"
    echo "ProsperoRadio Modernized build complete"
    echo "============================================================"
    echo "Source:  $WORK_DIR"
    echo "Output:  $OUT_DIR"
    find "$OUT_DIR" -maxdepth 2 -type f -printf '  %p\n' | sort
fi
