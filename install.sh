#!/bin/sh
# One-line installer for ds4-translator. Downloads a prebuilt release
# tarball from GitHub and runs `make install` on it, so no compiler is
# needed: the release ships the already-built daemon, ds4-ctl and Qt UI
# binaries, and the Makefile skips every build step when it sees them.
#
# Which version gets installed is decided by the URL this script came from:
#
#   latest release
#     curl -fsSL https://raw.githubusercontent.com/Billones142/ds4-translator/main/install.sh | sh
#   a specific release (the tag in the URL is the version installed)
#     curl -fsSL https://github.com/Billones142/ds4-translator/releases/download/v1.2.3/install.sh | sh
#
# That works because the release workflow publishes a copy of this script
# as a per-release asset with PINNED_VERSION rewritten to that tag. The
# copy on main keeps the placeholder below empty and asks the GitHub API
# for the latest release instead.
#
# POSIX sh on purpose: this is piped straight into whatever /bin/sh is.
set -eu

# Rewritten to the release tag by .github/workflows/release.yml when this
# script is published as a release asset. Empty here on purpose -- do not
# set it by hand; the workflow greps for this exact empty-string line.
PINNED_VERSION=""

REPO="Billones142/ds4-translator"
API="https://api.github.com/repos/${REPO}"
DOWNLOAD="https://github.com/${REPO}/releases/download"

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

for cmd in curl tar make; do
    command -v "$cmd" >/dev/null 2>&1 || die "'$cmd' is required but not installed"
done

# Everything `make install` does (writing under /usr/local and /etc,
# reloading udev, enabling and starting the systemd service) needs root, so
# the whole make step runs under sudo rather than re-prompting per file.
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    command -v sudo >/dev/null 2>&1 || die "not running as root and 'sudo' is not installed"
    SUDO="sudo"
fi

VERSION="$PINNED_VERSION"
if [ -z "$VERSION" ]; then
    log "Resolving latest release"
    # tag_name rather than the asset URL: the tarball name is derived from
    # the tag by the release workflow, so this one field is enough and the
    # parse stays trivial.
    VERSION=$(curl -fsSL "${API}/releases/latest" \
        | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -n 1)
    [ -n "$VERSION" ] || die "could not determine the latest release tag (GitHub API rate limit? install from a release-specific install.sh URL instead)"
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

log "Installing (make install; sudo may ask for your password)"
$SUDO make -C "${WORK_DIR}/${DIST_NAME}" install

log "Installed ds4-translator ${VERSION}. Check it with: ds4-ctl status"
