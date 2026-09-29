#!/bin/sh
# One-line installer for ds4-translator. Downloads a prebuilt release
# tarball from GitHub and runs `make install` on it, so no compiler is
# needed: the release ships the already-built daemon, ds4-ctl and Qt UI
# binaries, and the Makefile skips every build step (and the hard
# build-toolchain checks) when it sees them.
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

for cmd in curl tar make sha256sum; do
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

# Release JSON for a tag (or latest). Printed to stdout; caller captures it.
# Uses the GitHub API so we can read tag_name and the asset digest in one go.
fetch_release_json() {
    # $1 empty -> latest; otherwise the tag name.
    if [ -n "${1:-}" ]; then
        curl -fsSL "${API}/releases/tags/${1}"
    else
        curl -fsSL "${API}/releases/latest"
    fi
}

# Extract "tag_name":"..." from a release JSON blob on stdin.
parse_tag_name() {
    sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1
}

# Extract the sha256 digest for asset $1 from a release JSON blob on stdin.
# GitHub serves digests as "sha256:<hex>" on each asset; we strip the prefix.
parse_asset_sha256() {
    asset_name=$1
    # Collapse the JSON enough that the asset name and its digest share a
    # line-ish neighbourhood, then pull the digest that follows this asset.
    # Prefer python if present (robust); fall back to a sed/tr scrape.
    if command -v python3 >/dev/null 2>&1; then
        ASSET_NAME="$asset_name" python3 -c '
import json, os, sys
name = os.environ["ASSET_NAME"]
data = json.load(sys.stdin)
for a in data.get("assets") or []:
    if a.get("name") == name:
        d = a.get("digest") or ""
        if d.startswith("sha256:"):
            print(d[len("sha256:"):])
            sys.exit(0)
        sys.exit("missing sha256 digest for " + name)
sys.exit("asset not found: " + name)
'
        return
    fi
    # Fallback without python: find the asset block, then its digest field.
    tr '\n' ' ' | sed -n "s/.*\"name\"[[:space:]]*:[[:space:]]*\"${asset_name}\"[^\"]*\"digest\"[[:space:]]*:[[:space:]]*\"sha256:\\([0-9a-fA-F]*\\)\".*/\\1/p" | head -n 1
}

validate_tag() {
    # Reject anything that is not a boring v-prefixed release tag before we
    # interpolate it into URLs and paths.
    printf '%s' "$1" | grep -Eq '^v[0-9][0-9A-Za-z._-]*$' \
        || die "unexpected release tag '$1' (expected e.g. v1.2.3)"
}

VERSION="$PINNED_VERSION"
RELEASE_JSON=""
if [ -z "$VERSION" ]; then
    log "Resolving latest release"
    RELEASE_JSON=$(fetch_release_json) \
        || die "could not fetch the latest release (GitHub API rate limit? install from a release-specific install.sh URL instead)"
    VERSION=$(printf '%s' "$RELEASE_JSON" | parse_tag_name)
    [ -n "$VERSION" ] || die "could not determine the latest release tag"
else
    log "Fetching release metadata for ${VERSION}"
    RELEASE_JSON=$(fetch_release_json "$VERSION") \
        || die "could not fetch release ${VERSION} (does that tag exist?)"
fi

validate_tag "$VERSION"

DIST_NAME="ds4-translator-${VERSION}"
ASSET_URL="${DOWNLOAD}/${VERSION}/${DIST_NAME}.tar.gz"
ASSET_NAME="${DIST_NAME}.tar.gz"

EXPECTED_SHA=$(printf '%s' "$RELEASE_JSON" | parse_asset_sha256 "$ASSET_NAME") \
    || die "could not read sha256 digest for ${ASSET_NAME} from the release metadata"
[ -n "$EXPECTED_SHA" ] || die "empty sha256 digest for ${ASSET_NAME}"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT INT TERM

log "Downloading ${VERSION}"
curl -fsSL -o "${WORK_DIR}/${ASSET_NAME}" "$ASSET_URL" \
    || die "download failed: $ASSET_URL (does that release tag exist?)"

log "Verifying sha256"
ACTUAL_SHA=$(sha256sum "${WORK_DIR}/${ASSET_NAME}" | awk '{print $1}')
[ "$ACTUAL_SHA" = "$EXPECTED_SHA" ] \
    || die "checksum mismatch for ${ASSET_NAME} (expected ${EXPECTED_SHA}, got ${ACTUAL_SHA})"

log "Extracting"
tar -xzf "${WORK_DIR}/${ASSET_NAME}" -C "$WORK_DIR"
[ -d "${WORK_DIR}/${DIST_NAME}" ] || die "unexpected archive layout: ${DIST_NAME}/ not found"

log "Installing (make install; sudo may ask for your password)"
$SUDO make -C "${WORK_DIR}/${DIST_NAME}" install

log "Installed ds4-translator ${VERSION}. Check it with: ds4-ctl status"
