#!/usr/bin/env bash
# Entrypoint of the gktwo-bridge image:
#   docker run --rm --privileged -v "$PWD:/data" ghcr.io/sufyanaslam198/gktwo-bridge
# Builds, verifies and tests a firmware image from the files in $DATA (the mounted folder):
#   in:  ChituUpgrade.bin (stock V1.3.3); optional secrets/gktwo_ed25519 + .pub, secrets/root-password
#   out: out/ChituUpgrade.bin, out/ChituUpgrade.bin.sha256; secrets/ gets the key, password and
#        host keys it lacked
# Env: WEBUI (1 = web page, the default; 0 = no web page). With arguments, runs them instead (e.g. bash).
set -euo pipefail
[ $# = 0 ] || exec "$@"

die() { echo "gktwo-bridge: $*" >&2; exit 1; }
echo "GKtwo Bridge $(cat "$SRC/VERSION")"
WEBUI=${WEBUI:-1}
case "$WEBUI" in 0|1) export WEBUI ;; *) die "WEBUI must be 0 or 1" ;; esac
mountpoint -q "$DATA" || die "no folder mounted at $DATA; add -v \"<your folder>:$DATA\""
[ -f "$DATA/ChituUpgrade.bin" ] || die "no ChituUpgrade.bin in the mounted folder; copy the stock firmware V1.3.3 there"
mkdir -p /tmp/privcheck
{ mount -t tmpfs tmpfs /tmp/privcheck && umount /tmp/privcheck; } 2>/dev/null \
  || die "the end-to-end test needs a privileged container; add --privileged"

# Files created here belong to root; give them to the owner of the mounted folder (Linux hosts).
owner=$(stat -c %u:%g "$DATA")
fix_owner() { [ "$owner" = 0:0 ] || chown -R "$owner" "$DATA/out" "$DATA/secrets" 2>/dev/null || true; }
trap fix_owner EXIT
mkdir -p "$DATA/out" "$DATA/secrets"

key=$DATA/secrets/gktwo_ed25519
if [ ! -e "$key" ]; then
  [ ! -e "$key.pub" ] || die "secrets/gktwo_ed25519.pub exists without secrets/gktwo_ed25519"
  ssh-keygen -q -t ed25519 -N "" -C gktwo-bridge -f "$key"
  echo "gktwo-bridge: generated a new SSH key in secrets/gktwo_ed25519 (keep this file private)"
fi
[ -f "$key.pub" ] || die "missing secrets/gktwo_ed25519.pub"

# the add-ons built in the image; gksdcp with or without the web page
mkdir -p "$BIN"
cp /opt/gktwo/bin/dropbearmulti /opt/gktwo/bin/libgkhook.so "$BIN/"
if [ "$WEBUI" = 1 ]; then cp /opt/gktwo/bin/gksdcp "$BIN/gksdcp"; else cp /opt/gktwo/bin/gksdcp-noweb "$BIN/gksdcp"; fi

export PHASE1=1 ALLOW_CHANGED="rootfs/etc/init.d/rcS rootfs/etc/shadow customer/app0.sh" EXPECT_ADDED=$SRC/fw/expected-added.txt
"$SRC/fw/build-firmware.sh"
"$SRC/fw/verify-image.sh" "$DATA/out/ChituUpgrade.bin"
"$SRC/fw/e2e-ssh-test.sh"
(cd "$DATA/out" && sha256sum ChituUpgrade.bin > ChituUpgrade.bin.sha256 && cat ChituUpgrade.bin.sha256)
