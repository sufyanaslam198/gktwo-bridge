#!/usr/bin/env bash
# usage (inside gktwo-fw, from /src):  fw/verify-image.sh <ChituUpgrade.bin>
#   tools/dock.sh gktwo-fw 'fw/verify-image.sh /src/out/ChituUpgrade.bin'
#
# Compares <image> against the stock image ($DATA/ChituUpgrade.bin). Prints PASS/FAIL per
# check and "VERIFY OK" if everything passed (exit 0), else "VERIFY FAILED" (exit 1).
#
# Env:
#   SRC            the repository (fw/, tools/, overlay/). Default: /src.
#   DATA           the folder with ChituUpgrade.bin and secrets/. Default: $SRC.
#   ALLOW_CHANGED  space-separated paths allowed to differ from stock (content, mode, owner,
#                  link target) or to be absent. Paths are prefixed with the partition tree
#                  name, e.g. "rootfs/etc/init.d/rcS rootfs/etc/shadow". Default: none.
#   EXPECT_ADDED   file of lines "<path> <mode> [link-target]" (same path form, mode in octal,
#                  e.g. "customer/gk/bin/gksdcp 755"). Every listed path must exist in the new
#                  image with exactly that mode / link target. Anything else added fails
#                  (ancestor directories of listed paths are tolerated). Unset = expect
#                  no additions at all.
#
# What ubireader_extract_files -k preserves (verified on the stock image, run as root):
#   - mode bits including setuid (rootfs/usr/bin/busybox is 4755)   -> preserved
#   - uid/gid (rootfs/var/www is www-data 33:33)                     -> preserved
#   - symlinks and their targets                                     -> preserved
#   - mtimes (to the second)                                         -> preserved, not compared
#   - device nodes: none appear in any stock partition (extracted trees hold only files,
#     dirs and symlinks); anything else would be compared by type only.
# So uid/gid ARE comparable and are compared. Requires root (the container is).
set -euo pipefail

IMG=${1:?usage: $0 <image>}
[ -f "$IMG" ] || { echo "no such image: $IMG" >&2; exit 2; }
SRC=${SRC:-/src}
DATA=${DATA:-$SRC}
STOCK=$DATA/ChituUpgrade.bin
PIN=6286345080449c6b59015a897aafd75eb8479542
V=/work/verify
TREES="rootfs customer miservice appconfigs"
BOOT="ipl.es ipl_cust.es uboot.es cis.es cis.es.1 kernel.es logo.es"

mkdir -p "$V"

ensure_toolkit() {  # pinned toolkit clone, reused across runs
  if [ ! -f "$V/toolkit/scripts/UltraFirmwareToolkit.jar" ] || \
     [ "$(git -C "$V/toolkit" rev-parse HEAD 2>/dev/null)" != "$PIN" ]; then
    rm -rf "$V/toolkit"
    git clone -q https://github.com/weashadow/UltraFirmwareToolkit "$V/toolkit"
    git -C "$V/toolkit" checkout -q "$PIN"
  fi
}

extract() {  # extract <image> <dest-dir>: toolkit split into *.es, then ubireader per partition
  local img=$1 dest=$2
  rm -rf "$dest"; mkdir -p "$dest"
  cp -a "$V/toolkit/scripts/." "$dest/"
  cp "$img" "$dest/ChituUpgrade.bin"
  ( cd "$dest"
    java -jar UltraFirmwareToolkit.jar -extract ChituUpgrade.bin >/dev/null
    sed 's/^sudo //' extract_partition.sh > extract_partition.nosudo.sh
    bash -e extract_partition.nosudo.sh >extract.log 2>&1 || { cat extract.log; exit 1; }
    mkdir -p parameter )   # empty partition: ubireader prints "No inodes found"
}

ensure_toolkit
stock_sha=$(sha256sum "$STOCK" | cut -d' ' -f1)
if [ "$(cat "$V/stock/.sha256" 2>/dev/null || true)" != "$stock_sha" ]; then
  echo "extracting stock image (cached under $V/stock)..."
  extract "$STOCK" "$V/stock"
  echo "$stock_sha" > "$V/stock/.sha256"
fi
echo "extracting $IMG..."
extract "$IMG" "$V/new"

FAILS=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1${2:+: $2}"; FAILS=$((FAILS+1)); }

# --- header_version (format, force flag, and strictly greater than stock unless image IS stock)
vre='^GKtwoV[0-9]+\.[0-9]+\.[0-9]+$'
hdr=$(head -c 4096 "$IMG" | tr -d '\0' | grep -a -m1 '^#upgrade_bin_version=' || true)
shdr=$(head -c 4096 "$STOCK" | tr -d '\0' | grep -a -m1 '^#upgrade_bin_version=' || true)
ver=${hdr#\#upgrade_bin_version=}; sver=${shdr#\#upgrade_bin_version=}
hv_err=""
[[ "$ver" =~ $vre ]] || hv_err="malformed version '$hdr'"
head -c 4096 "$IMG" | grep -aq '^#upgrade_force=1' || hv_err="$hv_err upgrade_force=1 missing"
if [ -z "$hv_err" ]; then
  if [ "$(sha256sum "$IMG" | cut -d' ' -f1)" = "$stock_sha" ]; then
    [ "$ver" = "$sver" ] || hv_err="stock image version mismatch"
  else
    [ "$ver" != "$sver" ] && [ "$(printf '%s
%s
' "${sver#GKtwoV}" "${ver#GKtwoV}" | sort -V | tail -1)" = "${ver#GKtwoV}" ]       || hv_err="$ver not greater than stock $sver"
  fi
fi
if [ -z "$hv_err" ]; then pass header_version; echo "  $hdr (stock: $sver)"; else fail header_version "$hv_err"; fi

# --- boot_partitions_identical
bad=""
for f in $BOOT; do
  cmp -s "$V/stock/$f" "$V/new/$f" || bad="$bad $f"
done
if [ -z "$bad" ]; then pass boot_partitions_identical; else fail boot_partitions_identical "differ:$bad"; fi

# --- unchanged_files + added_files (one manifest walk, two verdicts)
cat > "$V/cmp.py" <<'PY'
import os, stat, hashlib

def manifest(base):
    m = {}
    for tree in os.environ["TREES"].split():
        root = os.path.join(base, tree)
        st = os.lstat(root)
        m[tree] = ("d", stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid, "")
        for dp, dns, fns in os.walk(root):
            for n in dns + fns:
                p = os.path.join(dp, n)
                rel = tree + "/" + os.path.relpath(p, root).replace(os.sep, "/")
                st = os.lstat(p)
                if stat.S_ISLNK(st.st_mode):
                    m[rel] = ("l", None, st.st_uid, st.st_gid, os.readlink(p))
                elif stat.S_ISDIR(st.st_mode):
                    m[rel] = ("d", stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid, "")
                elif stat.S_ISREG(st.st_mode):
                    h = hashlib.sha256()
                    with open(p, "rb") as f:
                        for c in iter(lambda: f.read(1 << 20), b""):
                            h.update(c)
                    m[rel] = ("f", stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid, h.hexdigest())
                else:
                    m[rel] = ("o%o" % stat.S_IFMT(st.st_mode), stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid, "")
    return m

a = manifest(os.environ["STOCK_DIR"])
b = manifest(os.environ["NEW_DIR"])
allow = set(os.environ["ALLOW_CHANGED"].split())
exp = {}
ef = os.environ["EXPECT_ADDED"]
if ef:
    for line in open(ef):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        exp[parts[0]] = (parts[1], parts[2] if len(parts) > 2 else None)

def desc(e):
    return "%s mode=%s uid=%s gid=%s %s" % (
        e[0], "-" if e[1] is None else "%o" % e[1], e[2], e[3], e[4][:16] if e[0] == "f" else e[4])

problems = []
for p, e in sorted(a.items()):
    if p in allow:
        continue
    n = b.get(p)
    if n is None:
        problems.append("missing in new: %s" % p)
    elif n != e:
        problems.append("changed: %s\n    stock: %s\n    new:   %s" % (p, desc(e), desc(n)))
print("UNCHANGED_COUNT %d" % (len(a) - len([p for p in a if p in allow])))
print("UNCHANGED_BAD %d" % len(problems))
for x in problems[:50]:
    print("  " + x)

added = sorted(set(b) - set(a))
aprob = []
ancestors = set()
for p in exp:
    parts = p.split("/")
    for i in range(1, len(parts)):
        ancestors.add("/".join(parts[:i]))
for p in added:
    if p in exp or p in ancestors or p in allow:
        continue
    aprob.append("unexpected addition: %s" % p)
for p, (mode, tgt) in sorted(exp.items()):
    e = b.get(p)
    if e is None:
        aprob.append("expected but absent: %s" % p)
        continue
    if e[0] == "l":
        if tgt is None or e[4] != tgt:
            aprob.append("%s: symlink target %r, expected %r" % (p, e[4], tgt))
    else:
        if tgt is not None:
            aprob.append("%s: expected symlink -> %s but is type %s" % (p, tgt, e[0]))
        if e[1] != int(mode, 8):
            aprob.append("%s: mode %o, expected %s" % (p, e[1], mode))
print("ADDED_COUNT %d" % len(added))
print("ADDED_BAD %d" % len(aprob))
for x in aprob[:50]:
    print("  " + x)
PY
rc=0
out=$(STOCK_DIR="$V/stock" NEW_DIR="$V/new" TREES="$TREES" \
      ALLOW_CHANGED="${ALLOW_CHANGED:-}" EXPECT_ADDED="${EXPECT_ADDED:-}" \
      python3 -I "$V/cmp.py") || rc=$?
if [ $rc -ne 0 ]; then
  fail manifest_compare "python rc=$rc"
else
  ub=$(echo "$out" | sed -n 's/^UNCHANGED_BAD //p')
  ab=$(echo "$out" | sed -n 's/^ADDED_BAD //p')
  if [ "$ub" = 0 ]; then
    pass unchanged_files
    echo "  $(echo "$out" | sed -n 's/^UNCHANGED_COUNT /compared entries: /p') (content, mode, uid/gid, link targets)"
  else
    fail unchanged_files "$ub problem(s)"
  fi
  if [ "$ab" = 0 ]; then
    pass added_files
    echo "  $(echo "$out" | sed -n 's/^ADDED_COUNT /added entries: /p')"
  else
    fail added_files "$ab problem(s)"
  fi
  echo "$out" | grep '^  ' || true
fi

# --- Phase 1 checks (PHASE1=1): boot hook, root password, dropbear, ssh keys
if [ "${PHASE1:-0}" = 1 ]; then
  NR=$V/new/rootfs; SR=$V/stock/rootfs; NC=$V/new/customer

  # rcs_hook: new rcS == stock rcS with fw/rcS.hook inserted immediately before the marker line
  if python3 -I - "$SR/etc/init.d/rcS" "$NR/etc/init.d/rcS" "$SRC/fw/rcS.hook" <<'PY'
import sys
stock = open(sys.argv[1], "rb").read()
new = open(sys.argv[2], "rb").read()
hook = open(sys.argv[3], "rb").read()
marker = b"if [ -e /customer/modules_stage0.sh ]; then\n"
if stock.count(marker) != 1:
    sys.exit("marker line not exactly once in stock rcS")
if not hook.endswith(b"\n"):
    sys.exit("rcS.hook lacks trailing newline")
exp = stock.replace(marker, hook + marker)
if new != exp:
    sys.exit("rcS differs from stock+hook")
PY
  then pass rcs_hook; else fail rcs_hook "rcS is not stock with hook inserted before modules_stage0 line"; fi

  # root_password: root hash verifies against secrets/root-password; everything else in shadow unchanged
  rp_err=""
  rline=$(grep -a '^root:' "$NR/etc/shadow" || true)
  rhash=$(echo "$rline" | cut -d: -f2)
  rsalt=$(echo "$rhash" | cut -d'$' -f3)
  if [ -z "$rline" ] || [ "${rhash:0:3}" != '$5$' ] || [ -z "$rsalt" ]; then
    rp_err="root line missing or not a SHA-256 crypt hash"
  elif [ ! -f "$DATA/secrets/root-password" ]; then
    rp_err="secrets/root-password is missing"
  elif [ "$(head -n1 "$DATA/secrets/root-password" | tr -d '\r' | openssl passwd -5 -salt "$rsalt" -stdin)" != "$rhash" ]; then
    rp_err="root hash does not verify against secrets/root-password"
  elif [ "$(grep -av '^root:' "$NR/etc/shadow" | sha256sum)" != "$(grep -av '^root:' "$SR/etc/shadow" | sha256sum)" ]; then
    rp_err="non-root shadow lines changed"
  elif [ "$(echo "$rline" | cut -d: -f1,3-)" != "$(grep -a '^root:' "$SR/etc/shadow" | cut -d: -f1,3-)" ]; then
    rp_err="root line fields other than the hash changed"
  fi
  if [ -z "$rp_err" ]; then pass root_password; else fail root_password "$rp_err"; fi

  # dropbear_runs
  dv=$( cd "$V/new" && qemu-arm-static -L "$NR" customer/gk/bin/dropbearmulti dropbear -V 2>&1 ) && drc=0 || drc=$?
  if [ "$drc" = 0 ] && echo "$dv" | grep -q 'Dropbear v2022.83'; then pass dropbear_runs
  else fail dropbear_runs "rc=$drc output='$dv'"; fi

  # glibc_ok: every regular ELF under customer/gk/bin
  elfs=$(find "$NC/gk/bin" -type f 2>/dev/null || true)
  if [ -z "$elfs" ]; then fail glibc_ok "no files under customer/gk/bin"
  elif g=$("$SRC/tools/check-glibc.sh" $elfs 2>&1); then pass glibc_ok
  else fail glibc_ok "$g"; fi

  # authorized_keys
  ak_err=""
  ssh_d=$NR/root/.ssh; ak=$ssh_d/authorized_keys
  [ "$(stat -c '%a %u:%g' "$ssh_d" 2>/dev/null)" = "700 0:0" ] || ak_err="$ak_err .ssh not 700 0:0;"
  [ "$(stat -c '%a %u:%g' "$ak" 2>/dev/null)" = "600 0:0" ] || ak_err="$ak_err authorized_keys not 600 0:0;"
  cmp -s "$ak" "$DATA/secrets/gktwo_ed25519.pub" || ak_err="$ak_err content differs from secrets/gktwo_ed25519.pub;"
  if [ -z "$ak_err" ]; then pass authorized_keys; else fail authorized_keys "$ak_err"; fi

  # host_keys
  hk_err=""
  for t in rsa ecdsa ed25519; do
    k=$NC/gk/dropbear/dropbear_${t}_host_key
    [ "$(stat -c '%a %u:%g' "$k" 2>/dev/null)" = "600 0:0" ] || hk_err="$hk_err $t not 600 0:0;"
    cmp -s "$k" "$DATA/secrets/dropbear/dropbear_${t}_host_key" || hk_err="$hk_err $t differs from secrets/dropbear;"
  done
  [ "$(stat -c '%a %u:%g' "$NC/gk/dropbear" 2>/dev/null)" = "700 0:0" ] || hk_err="$hk_err dropbear dir not 700 0:0;"
  if [ -z "$hk_err" ]; then pass host_keys; else fail host_keys "$hk_err"; fi

  # gksdcp_present: exists, mode 755, glibc ok
  gw=$NC/gk/bin/gksdcp
  gw_err=""
  [ -f "$gw" ] || gw_err="missing;"
  [ "$(stat -c '%a' "$gw" 2>/dev/null)" = 755 ] || gw_err="$gw_err mode not 755;"
  if [ -z "$gw_err" ]; then g=$("$SRC/tools/check-glibc.sh" "$gw" 2>&1) || gw_err="glibc: $g"; fi
  if [ -z "$gw_err" ]; then pass gksdcp_present; echo "  gksdcp size: $(stat -c %s "$gw") bytes"; else fail gksdcp_present "$gw_err"; fi

  # gksdcp_runs_arm: run the image's gksdcp under qemu-user; discovery answers M99999 with the protocol
  # version, and the upload probe (HEAD /uploadFile/upload) is 200
  gr_err=""
  if [ -f "$gw" ]; then
    rm -rf /work/gkv; mkdir -p /work/gkv   # disk-backed: gksdcp refuses tmpfs files dirs (-T is for the test anyway)
    qemu-arm-static -L "$NR" "$gw" -u 13000 -p 13030 -w 0 -d /work/gkv -s /tmp/nohook.sock -v /work/gkv -m 0000aabbccddeeff -T >/tmp/gkv.log 2>&1 &
    gpid=$!
    sleep 3
    dr=$(python3 -I - <<'PY' || true
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)
s.sendto(b"M99999", ("127.0.0.1", 13000))
print(s.recvfrom(4096)[0].decode("utf-8", "replace"))
PY
)
    hc=$(curl -s --max-time 5 -I -o /dev/null -w '%{http_code}' http://127.0.0.1:13030/uploadFile/upload || true)
    kill "$gpid" 2>/dev/null || true; pkill -f 'qemu-arm-static.*gksdcp' 2>/dev/null || true; wait "$gpid" 2>/dev/null || true
    echo "$dr" | grep -q '"ProtocolVersion":"V3.0.0"' || gr_err="discovery reply '$dr';"
    [ "$hc" = 200 ] || gr_err="$gr_err upload HEAD http=$hc;"
  else gr_err="no gksdcp"; fi
  if [ -z "$gr_err" ]; then pass gksdcp_runs_arm; else fail gksdcp_runs_arm "$gr_err"; fi

  # gkhook_present: exists, mode 755, glibc ok, ELF shared object (ET_DYN), stripped (no .symtab)
  hk=$NC/gk/lib/libgkhook.so
  hk_err=""
  [ -f "$hk" ] || hk_err="missing;"
  [ "$(stat -c '%a' "$hk" 2>/dev/null)" = 755 ] || hk_err="$hk_err mode not 755;"
  if [ -z "$hk_err" ]; then g=$("$SRC/tools/check-glibc.sh" "$hk" 2>&1) || hk_err="$hk_err glibc: $g;"; fi
  if [ -z "$hk_err" ]; then
    arm-linux-gnueabihf-readelf -h "$hk" 2>/dev/null | grep -Eq 'Type:[[:space:]]+DYN' || hk_err="$hk_err not a shared object (ET_DYN);"
    arm-linux-gnueabihf-readelf -S "$hk" 2>/dev/null | grep -q '\.symtab' && hk_err="$hk_err not stripped (.symtab present);"
  fi
  if [ -z "$hk_err" ]; then pass gkhook_present; echo "  libgkhook.so size: $(stat -c %s "$hk") bytes"; else fail gkhook_present "$hk_err"; fi

  # app0_preload: customer/app0.sh is byte-equal to the template, mode 755, root-owned
  a0=$NC/app0.sh
  a0_err=""
  cmp -s "$a0" "$SRC/overlay/customer/app0.sh" || a0_err="content differs from overlay/customer/app0.sh;"
  [ "$(stat -c '%a %u:%g' "$a0" 2>/dev/null)" = "755 0:0" ] || a0_err="$a0_err not 755 0:0;"
  if [ -z "$a0_err" ]; then pass app0_preload; else fail app0_preload "$a0_err"; fi

  # customer_fits: added content under customer/gk < 400 KB (device has ~476 KB free in /customer)
  used=$(find "$NC/gk" -type f -printf '%s
' | awk '{s+=$1} END{print s+0}')
  if [ "$used" -lt 409600 ]; then pass customer_fits; echo "  customer/gk total: $used bytes (limit 409600; device free ~476 KB)"
  else fail customer_fits "customer/gk is $used bytes, limit 409600"; fi
fi

if [ "$FAILS" -eq 0 ]; then echo "VERIFY OK"; else echo "VERIFY FAILED ($FAILS)"; exit 1; fi
