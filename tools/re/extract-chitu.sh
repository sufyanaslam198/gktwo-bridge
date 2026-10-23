#!/usr/bin/env bash
# usage (inside gktwo-fw, from /src):  tools/dock.sh gktwo-fw 'bash tools/re/extract-chitu.sh'
# Extracts the stock chitu binary from the stock /src/ChituUpgrade.bin (V1.3.3) to
# /work/tk/scripts/customer/resources/chitu, the path that tools/re/env.sh expects.
# Uses weashadow/UltraFirmwareToolkit at the same pinned commit as fw/build-firmware.sh.
# Safe to run again: the result is replaced each time. Work tree: a temp dir on /work.
set -euo pipefail

PIN=6286345080449c6b59015a897aafd75eb8479542
STOCK=/src/ChituUpgrade.bin
DEST=/work/tk/scripts/customer/resources
SIZE=1074744

die() { echo "extract-chitu: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "must run as root (ubireader needs it)"
[ -f "$STOCK" ] || die "missing $STOCK (copy the stock V1.3.3 ChituUpgrade.bin to the repo root)"

mkdir -p /work
T=$(mktemp -d /work/extract.XXXXXX)
trap 'rm -rf "$T"' EXIT

git clone -q https://github.com/weashadow/UltraFirmwareToolkit "$T/tk"
git -C "$T/tk" checkout -q "$PIN"
[ "$(git -C "$T/tk" rev-parse HEAD)" = "$PIN" ] || die "toolkit pin mismatch"

SC=$T/tk/scripts
cp "$STOCK" "$SC/ChituUpgrade.bin"
cd "$SC"
java -jar UltraFirmwareToolkit.jar -extract ChituUpgrade.bin >/dev/null
sed 's/^sudo //' extract_partition.sh > extract_partition.nosudo.sh
bash -e extract_partition.nosudo.sh >/dev/null

CH=$SC/customer/resources/chitu
[ -f "$CH" ] || die "chitu not found after extraction"
[ "$(stat -c %s "$CH")" = "$SIZE" ] || die "chitu size is $(stat -c %s "$CH"), expected $SIZE (is the image V1.3.3?)"

rm -rf /work/tk
mkdir -p "$DEST"
cp "$CH" "$DEST/chitu"
echo "extracted $DEST/chitu ($SIZE bytes)"
sha256sum "$DEST/chitu"
