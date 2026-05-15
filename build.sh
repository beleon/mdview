#!/usr/bin/env bash
# ============================================================================
# build.sh — build mdview inside a Podman container and package as a tarball
#
# Usage:
#   ./build.sh              # builds and produces mdview-linux-x86_64.tar.gz
#   ./build.sh --no-cache   # rebuild from scratch
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="mdview-builder"
CONTAINER_NAME="mdview-extract-$$"
OUTPUT_TAR="mdview-linux-x86_64.tar.gz"

EXTRA_BUILD_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --no-cache) EXTRA_BUILD_ARGS+=("--no-cache") ;;
        --help|-h)
            echo "Usage: $0 [--no-cache]"
            echo ""
            echo "Builds mdview inside a Podman container and produces ${OUTPUT_TAR}"
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

echo "==> Extracting build artifacts..."

podman create --name "${CONTAINER_NAME}" "${IMAGE_NAME}" /bin/true

STAGING=$(mktemp -d)
trap 'rm -rf "${STAGING}"; podman rm -f "${CONTAINER_NAME}" &>/dev/null || true' EXIT

podman cp "${CONTAINER_NAME}:/opt/mdview" "${STAGING}/mdview"
podman rm -f "${CONTAINER_NAME}" >/dev/null

# ── Package ─────────────────────────────────────────────────────────────────

echo "==> Packaging ${OUTPUT_TAR}..."

if command -v strip &>/dev/null; then
    strip "${STAGING}/mdview/mdview" 2>/dev/null || true
fi

chmod +x "${STAGING}/mdview/mdview"

tar -czf "${SCRIPT_DIR}/${OUTPUT_TAR}" \
    -C "${STAGING}" \
    mdview/

SIZE=$(du -h "${SCRIPT_DIR}/${OUTPUT_TAR}" | cut -f1)
echo ""
echo "==> Done! ${OUTPUT_TAR} (${SIZE})"
echo ""
echo "    Extract and run:"
echo "      tar xzf ${OUTPUT_TAR}"
echo "      ./mdview/mdview path/to/file.md"
