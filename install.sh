#!/bin/sh
# One-line installer for ds4-translator. Downloads a prebuilt release
# tarball from GitHub and runs the Makefile's install target on it, so no
# compiler is needed (the release ships the already-built binaries, with
# mtimes newer than the sources, so make has nothing to rebuild).
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/Billones142/ds4-translator/main/install.sh | sh
#   curl -fsSL .../install.sh | sh -s -- --version v1.2.3
#   curl -fsSL .../install.sh | sh -s -- --with-ui
#
# POSIX sh on purpose: this is piped straight into whatever /bin/sh is.
set -eu

REPO="Billones142/ds4-translator"
API="https://api.github.com/repos/${REPO}"
DOWNLOAD="https://github.com/${REPO}/releases/download"

# Overridable by --version; DS4_VERSION allows the same choice from the
# environment, which is the only way to pass it when the script is piped
# without `sh -s --`.
VERSION="${DS4_VERSION:-}"
# The release tarball only carries `make all` output (daemon, ds4-ctl,
# spoof libs, BPF object) and not the Qt UI, so the default target is
# install-cli. --with-ui switches to `install`, which builds the UI from
# the shipped sources and therefore does need Qt 6 + CMake.
MAKE_TARGET="install-cli"

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Install ds4-translator from a prebuilt GitHub release.

Options:
  -v, --version <tag>  Install this release tag (e.g. v1.2.3).
                       Defaults to the latest release.
                       Equivalent env var: DS4_VERSION.
      --with-ui        Also build and install the Qt 6 settings UI
                       (requires Qt 6 and CMake).
  -h, --help           Show this help.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -v|--version)
            [ $# -ge 2 ] || die "--version needs a release tag (e.g. v1.2.3)"
            VERSION="$2"
            shift 2
            ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --with-ui)   MAKE_TARGET="install"; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "unknown option: $1 (try --help)" ;;
    esac
done

for cmd in curl tar make; do
    command -v "$cmd" >/dev/null 2>&1 || die "'$cmd' is required but not installed"
done

# Everything the Makefile's install target does (writing under /usr/local
# and /etc, reloading udev and systemd) needs root, so the whole make step
# runs under sudo rather than re-prompting per file.
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    command -v sudo >/dev/null 2>&1 || die "not running as root and 'sudo' is not installed"
    SUDO="sudo"
fi

if [ -z "$VERSION" ]; then
    log "Resolving latest release"
    # tag_name rather than the asset URL: the tarball name is derived from
    # the tag by the release workflow, so this one field is enough and the
    # parse stays trivial.
    VERSION=$(curl -fsSL "${API}/releases/latest" \
        | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -n 1)
    [ -n "$VERSION" ] || die "could not determine the latest release tag (GitHub API rate limit? pass --version <tag>)"
fi

DIST_NAME="ds4-translator-${VERSION}"
ASSET_URL="${DOWNLOAD}/${VERSION}/${DIST_NAME}.tar.gz"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT INT TERM

log "Downloading ${VERSION}"
curl -fsSL -o "${WORK_DIR}/${DIST_NAME}.tar.gz" "$ASSET_URL" \
    || die "download failed: $ASSET_URL (does that release tag exist?)"

log "Extracting"
tar -xzf "${WORK_DIR}/${DIST_NAME}.tar.gz" -C "$WORK_DIR"
[ -d "${WORK_DIR}/${DIST_NAME}" ] || die "unexpected archive layout: ${DIST_NAME}/ not found"

log "Installing (make ${MAKE_TARGET}; sudo may ask for your password)"
$SUDO make -C "${WORK_DIR}/${DIST_NAME}" "$MAKE_TARGET"

log "Installed ds4-translator ${VERSION}. Check it with: ds4-ctl status"
