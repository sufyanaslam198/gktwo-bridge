#!/usr/bin/env bash
# usage (inside gktwo-fw, from /src):  tools/dock.sh gktwo-fw 'fw/build-firmware.sh'
# Rebuilds /src/out/ChituUpgrade.bin from the stock /src/ChituUpgrade.bin using
# weashadow/UltraFirmwareToolkit @ 6286345. Fails (non-zero) on any error, including a
# mkfs.ubifs LEB overflow (the generated build.sh fixes the LEB count per partition).
# Work tree: /work/build/tk (fresh every run, on the Linux volume, never on a Windows folder).
# Env: SRC (repository, default /src), DATA (ChituUpgrade.bin, secrets/, out/; default $SRC),
#      BIN (ARM binaries dropbearmulti, gksdcp, libgkhook.so; default /work/bin).
set -euo pipefail

PIN=6286345080449c6b59015a897aafd75eb8479542
SRC=${SRC:-/src}
DATA=${DATA:-$SRC}
BIN=${BIN:-/work/bin}
B=/work/build
TK=$B/tk
SC=$TK/scripts
STOCK=$DATA/ChituUpgrade.bin
OUT=$DATA/out/ChituUpgrade.bin

die() { echo "build-firmware: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "must run as root (ubireader -k / mkfs.ubifs need it)"
[ -f "$STOCK" ] || die "missing $STOCK"

# --- 1. fresh toolkit clone, pinned
rm -rf "$TK"; mkdir -p "$B"
git clone -q https://github.com/weashadow/UltraFirmwareToolkit "$TK"
git -C "$TK" checkout -q "$PIN"
[ "$(git -C "$TK" rev-parse HEAD)" = "$PIN" ] || die "toolkit pin mismatch"

# --- 2. extract stock (toolkit splits into *.es + generates extract_partition.sh / build.sh)
cp "$STOCK" "$SC/ChituUpgrade.bin"
stock_ver=$(head -c 4096 "$STOCK" | tr -d '\0' | grep -a -m1 '^#upgrade_bin_version=' | cut -d= -f2)
cd "$SC"
java -jar UltraFirmwareToolkit.jar -extract ChituUpgrade.bin >/dev/null

# The generated scripts call `sudo`; we are already root in the container, so strip it.
# (They also lack `set -e`, so run them via `bash -e`.)
sed 's/^sudo //' extract_partition.sh > extract_partition.nosudo.sh
bash -e extract_partition.nosudo.sh
mkdir -p parameter   # empty partition: ubireader prints "No inodes found" but exits 0

# --- 3. apply changes. Trees are now at $SC/rootfs, $SC/customer, $SC/miservice, $SC/appconfigs.
# Everything is staged on Linux storage (/work/stage): /src (a Windows folder) does not preserve modes/links.
[ -f "$SRC/fw/rcS.hook" ] || die "missing fw/rcS.hook"
[ -d "$SRC/overlay/customer/gk" ] || die "missing overlay/customer/gk"
[ -x "$BIN/dropbearmulti" ] || die "missing $BIN/dropbearmulti (run deps/dropbear/build.sh in gktwo-cc)"
[ -x "$BIN/gksdcp" ] || die "missing $BIN/gksdcp (make -C gksdcp arm in gktwo-cc)"
[ -f "$BIN/libgkhook.so" ] || die "missing $BIN/libgkhook.so (make -C gkhook arm in gktwo-cc)"
[ -f "$SRC/overlay/customer/app0.sh" ] || die "missing overlay/customer/app0.sh"
[ -f "$DATA/secrets/gktwo_ed25519.pub" ] || die "missing secrets/gktwo_ed25519.pub"

# 3a. stage the overlay
STAGE=/work/stage
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -r "$SRC/overlay/customer/gk" "$STAGE/gk"
if grep -rIlq $'\r' "$STAGE/gk"; then
  die "CR byte found in staged text files: $(grep -rIl $'\r' "$STAGE/gk" | tr '\n' ' ')"
fi
mkdir -p "$STAGE/gk/bin" "$STAGE/gk/init.d" "$STAGE/gk/dropbear" "$STAGE/gk/lib"
cp "$BIN"/dropbearmulti "$STAGE/gk/bin/dropbearmulti"
cp "$BIN"/gksdcp "$STAGE/gk/bin/gksdcp"
cp "$BIN"/libgkhook.so "$STAGE/gk/lib/libgkhook.so"
for l in scp dbclient dropbearkey dropbear; do ln -sf dropbearmulti "$STAGE/gk/bin/$l"; done

# 3b. host keys: generated once into secrets/ (gitignored), then baked in so they survive reflashes
mkdir -p "$DATA/secrets/dropbear"
for t in rsa ecdsa ed25519; do
  k="$DATA/secrets/dropbear/dropbear_${t}_host_key"
  if [ ! -s "$k" ]; then
    rm -f "$k"
    qemu-arm-static -L "$SC/rootfs" "$BIN/dropbearmulti" dropbearkey -t "$t" -f "$k" >/dev/null
    [ -s "$k" ] || die "failed to generate $t host key"
  fi
  cp "$k" "$STAGE/gk/dropbear/dropbear_${t}_host_key"
done

# 3c. modes (explicit)
find "$STAGE/gk" -type d -exec chmod 755 {} +
chmod 700 "$STAGE/gk/dropbear"
chmod 600 "$STAGE/gk/dropbear/"*_host_key
chmod 755 "$STAGE/gk/customerStart" "$STAGE/gk/customer.sh" "$STAGE/gk/bin/dropbearmulti" "$STAGE/gk/bin/gksdcp" "$STAGE/gk/lib/libgkhook.so" "$STAGE/gk/init.d/"*

# 3d. apply to the extracted trees
marker='if [ -e /customer/modules_stage0.sh ]; then'
[ "$(grep -cxF "$marker" rootfs/etc/init.d/rcS)" = 1 ] || die "rcS marker line not found exactly once"
python3 -I - rootfs/etc/init.d/rcS "$SRC/fw/rcS.hook" "$marker" <<'PY'
import sys
p, hook, marker = sys.argv[1], open(sys.argv[2], "rb").read(), sys.argv[3].encode() + b"\n"
s = open(p, "rb").read()
assert s.count(marker) == 1
open(p, "wb").write(s.replace(marker, hook + marker))
PY

PWF=$DATA/secrets/root-password
mkdir -p "$DATA/secrets"
if [ ! -f "$PWF" ]; then
  gen=$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20 || true)
  [ "${#gen}" = 20 ] || die "could not generate a root password"
  tmpf=$(mktemp "$DATA/secrets/.root-password.XXXXXX")
  chmod 600 "$tmpf" 2>/dev/null || true
  printf '%s\n' "$gen" > "$tmpf"
  mv -n "$tmpf" "$PWF"
  rm -f "$tmpf"   # only left over when another build created the file first
  unset gen
  echo "build-firmware: generated a new root password in secrets/root-password (keep this file private)"
fi
chmod 600 "$PWF" 2>/dev/null || true
[ "$(stat -c %a "$PWF" 2>/dev/null)" = 600 ] || echo "build-firmware: WARNING secrets/root-password does not have mode 600 (a Windows folder can ignore chmod); keep the repository in a private folder" >&2
rootpw=$(head -n1 "$PWF" | tr -d '\r')
[ -n "$rootpw" ] || die "secrets/root-password is empty"
[ "${#rootpw}" -ge 8 ] || die "secrets/root-password is shorter than 8 characters"
[ "$rootpw" != "$(head -n1 "$SRC/secrets/root-password.example" 2>/dev/null | tr -d '\r')" ] || die "secrets/root-password still holds the example text; write your own password"
hash=$(printf '%s' "$rootpw" | openssl passwd -5 -stdin)   # random salt per build
unset rootpw
case "$hash" in '$5$'*) ;; *) die "openssl passwd did not return a SHA-256 hash";; esac
[ "$(grep -c '^root:' rootfs/etc/shadow)" = 1 ] || die "shadow root line not unique"
python3 -I - rootfs/etc/shadow "$hash" <<'PY'
import sys
p, h = sys.argv[1], sys.argv[2]
out = []
for l in open(p).read().split("\n"):
    if l.startswith("root:"):
        f = l.split(":"); f[1] = h; l = ":".join(f)
    out.append(l)
open(p, "w").write("\n".join(out))
PY

mkdir -m 700 rootfs/root/.ssh
install -m 600 "$DATA/secrets/gktwo_ed25519.pub" rootfs/root/.ssh/authorized_keys
cp -a "$STAGE/gk" customer/gk
# stock customer/app0.sh ("/customer/resources/chitu&", no shebang, no newline) -> LD_PRELOAD launcher
[ "$(cat customer/app0.sh)" = '/customer/resources/chitu&' ] || die "stock customer/app0.sh is not the expected one-liner"
install -m 755 -o 0 -g 0 "$SRC/overlay/customer/app0.sh" customer/app0.sh
chown -R 0:0 rootfs/root/.ssh customer/gk
chmod 700 rootfs/root/.ssh

# --- 4. repack: generated build.sh = mkfs.ubifs + crc32 per partition + toolkit -build
sed 's/^sudo //' build.sh > build.nosudo.sh
# The toolkit prompts interactively ("<x>.out.es has same size as original, use it?" for each
# repacked partition, and "increase the firmware version?"). Answer yes to all so the repacked
# partitions are used and the version is bumped. (Fixed supply of answers, not `yes`, so
# pipefail does not trip on SIGPIPE.)
printf 'y\n%.0s' {1..30} | bash -e build.nosudo.sh
# customer partition usage: mkfs.ubifs output holds only the used LEBs (max from build.sh -c N)
cmax=$(sed -n 's/^.*mkfs.ubifs .* -e \([0-9]*\) -c \([0-9]*\) .*-o customer.merged.out.es.*$/\2/p' build.nosudo.sh)
leb=$(sed -n 's/^.*mkfs.ubifs .* -e \([0-9]*\) -c \([0-9]*\) .*-o customer.merged.out.es.*$/\1/p' build.nosudo.sh)
if [ -n "$cmax" ] && [ -n "$leb" ]; then
  csz=$(stat -c %s customer.merged.out.es)
  echo "customer partition: $(( (csz + leb - 1) / leb )) of $cmax LEBs used (image $csz bytes, LEB $leb)"
fi
[ -s ChituUpgrade.bin.out ] || die "toolkit produced no ChituUpgrade.bin.out"
# Toolkit bug: its version bump mangles the string (GKtwoV1.3.3 -> "VGKtwo1.3.4"). The header is
# a plain-text script with no checksum of its own, so fix-header-version.py rewrites it in place
# (same length) and fails unless exactly the mangled or the correct form is present.
python3 -I "$SRC/fw/fix-header-version.py" ChituUpgrade.bin.out

# --- 5. publish + report
mkdir -p "$DATA/out"
cp ChituUpgrade.bin.out "$OUT"
new_ver=$(head -c 4096 "$OUT" | tr -d '\0' | grep -a -m1 '^#upgrade_bin_version=' | cut -d= -f2)
[ -n "$new_ver" ] || die "no #upgrade_bin_version in output"
[[ "$new_ver" =~ ^GKtwoV[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "malformed version header: $new_ver"
[[ "$stock_ver" =~ ^GKtwoV[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "malformed stock version: $stock_ver"
[ "$(printf '%s
%s
' "${stock_ver#GKtwoV}" "${new_ver#GKtwoV}" | sort -V | tail -1)" = "${new_ver#GKtwoV}" ]   && [ "$new_ver" != "$stock_ver" ] || die "version $new_ver is not greater than stock $stock_ver"
echo "stock version: $stock_ver"
echo "#upgrade_bin_version=$new_ver"
ls -l "$OUT"
