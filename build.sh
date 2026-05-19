#!/usr/bin/env bash
# ============================================================================
# build.sh — build mdview inside a Podman container and drop the binary
# under dist/. No tarball — the binary is self-contained (icons baked in,
# `mdview --install` lays it out under ~/.local).
#
# Usage:
#   ./build.sh              # build and produce dist/mdview
#   ./build.sh --no-cache   # rebuild from scratch (no podman layer cache)
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="mdview-builder"
CONTAINER_NAME="mdview-extract-$$"
OUT_DIR="${SCRIPT_DIR}/dist"

EXTRA_BUILD_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --no-cache) EXTRA_BUILD_ARGS+=("--no-cache") ;;
        --help|-h)
            echo "Usage: $0 [--no-cache]"
            echo ""
            echo "Builds mdview inside a Podman container, leaving the binary at dist/mdview."
            exit 0
            ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

if ! command -v podman &>/dev/null; then
    echo "Error: podman is not installed." >&2
    exit 1
fi

cd "$SCRIPT_DIR"

if [[ ! -f Containerfile ]]; then
    echo "Error: Containerfile not found in $SCRIPT_DIR" >&2
    exit 1
fi

# ── Build ───────────────────────────────────────────────────────────────────

echo "==> Building container image '${IMAGE_NAME}'..."
podman build \
    "${EXTRA_BUILD_ARGS[@]+"${EXTRA_BUILD_ARGS[@]}"}" \
    -t "${IMAGE_NAME}" \
    -f Containerfile \
    .

# ── Extract ─────────────────────────────────────────────────────────────────

echo "==> Extracting binary..."
mkdir -p "${OUT_DIR}"
podman create --name "${CONTAINER_NAME}" "${IMAGE_NAME}" /bin/true >/dev/null
trap 'podman rm -f "${CONTAINER_NAME}" &>/dev/null || true' EXIT

podman cp "${CONTAINER_NAME}:/build/mdview/build/mdview" "${OUT_DIR}/mdview"
podman rm -f "${CONTAINER_NAME}" >/dev/null
trap - EXIT

if command -v strip &>/dev/null; then
    strip "${OUT_DIR}/mdview" 2>/dev/null || true
fi
chmod +x "${OUT_DIR}/mdview"

SIZE=$(du -h "${OUT_DIR}/mdview" | cut -f1)
BIN="${OUT_DIR}/mdview"

# ── Done ────────────────────────────────────────────────────────────────────

echo
echo "==> Built ${BIN} (${SIZE})"
echo
echo "Install into ~/.local (binary + icons + desktop entry):"
echo "    ${BIN} --install"
echo
echo "Or run directly:"
echo "    ${BIN} path/to/file.md"
