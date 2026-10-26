#!/usr/bin/env bash
# usage (inside gktwo-fw, from /src, needs --privileged):
#   DOCK_ARGS=--privileged tools/dock.sh gktwo-fw 'fw/e2e-ssh-test.sh'
# Assembles a root from the BUILT image's extracted trees (/work/verify/new, produced by
# fw/verify-image.sh), runs the real R96dropbear init script in a chroot under qemu-user,
# and tests SSH login (key + password), scp, host key identity, and stop.
# Env: SRC (repository, default /src), DATA (secrets/, out/; default $SRC).
set -uo pipefail

export SRC=${SRC:-/src}
export DATA=${DATA:-$SRC}
IMG=${1:-$DATA/out/ChituUpgrade.bin}
V=/work/verify/new
R=/work/e2e/root
KEY=/tmp/e2e_key   # copy with 0600 (the /src mount shows 0777)
cp "$DATA/secrets/gktwo_ed25519" "$KEY" && chmod 600 "$KEY"
PWFILE=/tmp/e2e_pw   # copy with 0600; sshpass -f keeps the password out of the process list
(umask 077; head -n1 "$DATA/secrets/root-password" | tr -d '\r\n' > "$PWFILE")
HK=$DATA/secrets/dropbear/dropbear_ed25519_host_key
SSHO=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -p 22)
FAILS=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1${2:+: $2}"; FAILS=$((FAILS+1)); }

cleanup() {
  [ -f "$R/tmp/dropbear.pid" ] && kill "$(cat "$R/tmp/dropbear.pid")" 2>/dev/null
  [ -f "$R/tmp/gksdcp.pid" ] && kill "$(cat "$R/tmp/gksdcp.pid")" 2>/dev/null
  pkill -f 'dropbearmulti' 2>/dev/null
  pkill -f 'gksdcp -d /media/emmc' 2>/dev/null
  rm -f /tmp/e2e_sdcp.py
  for m in media/emmc media dev/pts dev proc tmp; do umount -l "$R/$m" 2>/dev/null; done
}
trap 'cleanup; rm -f "$PWFILE"' EXIT

[ -f "$IMG" ] || { echo "no image $IMG" >&2; exit 2; }
# the extracted tree must come from this image
if [ "$(sha256sum "$V/ChituUpgrade.bin" 2>/dev/null | cut -d' ' -f1)" != "$(sha256sum "$IMG" | cut -d' ' -f1)" ]; then
  echo "re-extracting $IMG via verify-image.sh..."
  ALLOW_CHANGED="rootfs/etc/init.d/rcS rootfs/etc/shadow customer/app0.sh" EXPECT_ADDED=$SRC/fw/expected-added.txt \
    "$SRC/fw/verify-image.sh" "$IMG" >/dev/null || { echo "verify failed" >&2; exit 1; }
fi

# --- assemble the chroot
cleanup
rm -rf "$R"; mkdir -p "$R"
cp -a "$V/rootfs/." "$R/"
mkdir -p "$R/customer"; cp -a "$V/customer/." "$R/customer/"
mkdir -p "$R/dev" "$R/proc" "$R/tmp" "$R/root"
mount -t proc proc "$R/proc" || fail mount_proc
mount --bind /dev "$R/dev" || fail mount_dev
mount -t devpts devpts "$R/dev/pts" 2>/dev/null || true
mount -t tmpfs tmpfs "$R/tmp" || fail mount_tmp
chmod 1777 "$R/tmp"

# ARM execution: binfmt_misc entry, else copy qemu-arm-static in
if ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -qi arm; then
  echo "binfmt_misc arm entry present"
else
  mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc 2>/dev/null
  if ! ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -qi arm; then
    echo "no binfmt arm entry; copying qemu-arm-static into chroot"
    cp /usr/bin/qemu-arm-static "$R/usr/bin/qemu-arm-static"
  fi
fi
chroot "$R" /bin/busybox true 2>/dev/null || chroot "$R" /bin/sh -c true || { echo "chroot cannot run ARM shell" >&2; exit 1; }

# --- start via the real init script
echo "--- R96dropbear start"
chroot "$R" /customer/gk/init.d/R96dropbear start
sleep 2
if [ -f "$R/tmp/dropbear.pid" ] && kill -0 "$(cat "$R/tmp/dropbear.pid")" 2>/dev/null; then pass dropbear_started; else fail dropbear_started; fi

# --- (a) key login
out=$(ssh -i "$KEY" "${SSHO[@]}" -o BatchMode=yes root@127.0.0.1 'id; uname -m; echo E2E_OK' 2>&1)
echo "$out"
if echo "$out" | grep -q 'uid=0' && echo "$out" | grep -q E2E_OK; then pass key_login; else fail key_login; fi

# --- (b) password login
out=$(sshpass -f "$PWFILE" ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no "${SSHO[@]}" root@127.0.0.1 'echo E2E_OK' 2>&1)
echo "$out"
if echo "$out" | grep -q E2E_OK; then pass password_login; else fail password_login; fi
# wrong passwords must fail with an authentication failure (sshpass status 5), not a connection error
WRONGF=/tmp/e2e_pw_wrong
for cand in derived gktwo; do
  if [ "$cand" = derived ]; then (umask 077; { cat "$PWFILE"; printf x; } > "$WRONGF")
  else (umask 077; printf gktwo > "$WRONGF"); fi
  out=$(sshpass -f "$WRONGF" ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no "${SSHO[@]}" root@127.0.0.1 'echo E2E_OK' 2>&1); rc=$?
  if echo "$out" | grep -q E2E_OK; then fail "wrong_password_rejected_$cand" "login succeeded"
  elif [ "$rc" = 5 ] || echo "$out" | grep -q 'Permission denied'; then pass "wrong_password_rejected_$cand"
  else fail "wrong_password_rejected_$cand" "not an authentication failure (rc=$rc)"; fi
done
rm -f "$WRONGF"

# --- (c) scp -O round trip
T=$(mktemp -d)
head -c 300001 /dev/urandom > "$T/in.bin"
if scp -O -i "$KEY" "${SSHO[@]/-p/-P}" "$T/in.bin" root@127.0.0.1:/tmp/e2e.bin >/dev/null 2>&1 &&
   scp -O -i "$KEY" "${SSHO[@]/-p/-P}" root@127.0.0.1:/tmp/e2e.bin "$T/out.bin" >/dev/null 2>&1 &&
   cmp -s "$T/in.bin" "$T/out.bin"; then pass scp_roundtrip; else fail scp_roundtrip; fi
rm -rf "$T"

# --- (d) host key fingerprint equals the baked secrets key
exp=$(chroot "$R" /customer/gk/bin/dropbearmulti dropbearkey -y -f /customer/gk/dropbear/dropbear_ed25519_host_key 2>&1 | grep -o 'SHA256:[^ ]*' | head -1)
exp2=$(cp "$HK" "$R/tmp/hk" && chroot "$R" /customer/gk/bin/dropbearmulti dropbearkey -y -f /tmp/hk 2>&1 | grep -o 'SHA256:[^ ]*' | head -1)
got=$(ssh-keyscan -t ed25519 -p 22 127.0.0.1 2>/dev/null | ssh-keygen -lf - | awk '{print $2}')
echo "secrets key: $exp2  presented: $got"
if [ -n "$got" ] && [ "$got" = "$exp2" ] && [ "$got" = "$exp" ]; then pass host_key_fingerprint; else fail host_key_fingerprint "exp=$exp exp2=$exp2 got=$got"; fi

# --- (e) stop
echo "--- R96dropbear stop"
pid=$(cat "$R/tmp/dropbear.pid" 2>/dev/null)
chroot "$R" /customer/gk/init.d/R96dropbear stop
sleep 1
if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null &&
   ! ssh -i "$KEY" "${SSHO[@]}" -o BatchMode=yes root@127.0.0.1 true >/dev/null 2>&1; then pass stop; else fail stop; fi

# --- (f) gksdcp via the real R97gksdcp init script (UDP 3000, WebSocket and upload 3030, page on port 80
#         when the image was built with WEBUI=1). At boot /media is the stock tmpfs and /media/emmc does not
#         exist until the stock app creates it and mounts the exFAT card on it, so replay exactly that:
#         absent -> plain dir on tmpfs (must refuse: RAM!) -> real fs mounted.
echo "--- R97gksdcp start"
umount -l "$R/media/emmc" 2>/dev/null; umount -l "$R/media" 2>/dev/null
rm -rf "$R/media" /work/e2e/emmc-store; mkdir -p "$R/media" /work/e2e/emmc-store
mount -t tmpfs tmpfs "$R/media" || fail tmpfs_media_mount
cat > /tmp/e2e_sdcp.py <<'PY'
import json, os, socket, sys
sys.path.insert(0, os.path.join(os.environ["SRC"], "tests", "gksdcp"))
from sdcp_client import discover, upload_chunk, ws_connect
cmd = sys.argv[1]
if cmd == "discover":
    print(json.dumps(discover(3000)))
elif cmd == "ws0":
    ws = ws_connect(3030)
    r = ws.wait_response(ws.request(0))
    ws.close()
    print(json.dumps(r["Data"]))
elif cmd == "upload":
    data = sys.argv[3].encode()
    f = {"S-File-MD5": "", "Check": "0", "Offset": 0, "Uuid": "3f2b8c1e-0d4a-4c55-9a7e-6b1f2d3c4e5f", "TotalSize": len(data)}
    st, js = upload_chunk(3030, f, sys.argv[2], data)
    print("http=%d %s" % (st, json.dumps(js)))
PY
chroot "$R" /customer/gk/init.d/R97gksdcp start
sleep 2
gpid=$(cat "$R/tmp/gksdcp.pid" 2>/dev/null)
if [ -n "$gpid" ] && kill -0 "$gpid" 2>/dev/null; then pass gksdcp_started; else fail gksdcp_started; fi
out=$(chroot "$R" /customer/gk/init.d/R97gksdcp start)
if echo "$out" | grep -q 'already running' && [ "$(cat "$R/tmp/gksdcp.pid" 2>/dev/null)" = "$gpid" ] && kill -0 "$gpid" 2>/dev/null; then pass gksdcp_start_idempotent; else fail gksdcp_start_idempotent "$out"; fi
[ -d "$R/customer/gk/var" ] && pass gksdcp_var_dir || fail gksdcp_var_dir
out=$(python3 /tmp/e2e_sdcp.py discover 2>&1)
echo "$out"
if echo "$out" | grep -q '"ProtocolVersion": *"V3.0.0"'; then pass gksdcp_discovery; else fail gksdcp_discovery "$out"; fi
# /media/emmc absent: still up, upload refused: storage not mounted
out=$(python3 /tmp/e2e_sdcp.py upload e2e.ctb CTBDATA 2>&1)
if echo "$out" | grep -q 'http=503' && echo "$out" | grep -q 'storage not mounted' && kill -0 "$gpid" 2>/dev/null; then pass gksdcp_tolerates_missing_dir; else fail gksdcp_tolerates_missing_dir "$out"; fi
mkdir -p "$R/media/emmc"   # the stock app creates it (still on tmpfs) before mounting the card
out=$(python3 /tmp/e2e_sdcp.py upload e2e.ctb CTBDATA 2>&1)
echo "$out"
if echo "$out" | grep -q 'http=503' && echo "$out" | grep -q 'storage not mounted' && [ ! -e "$R/media/emmc/e2e.ctb" ]; then pass gksdcp_upload_refuses_tmpfs; else fail gksdcp_upload_refuses_tmpfs "$out"; fi
mount --bind /work/e2e/emmc-store "$R/media/emmc" || fail emmc_bind_mount   # stands in for the exFAT card
out=$(python3 /tmp/e2e_sdcp.py upload e2e.ctb CTBDATA 2>&1)
echo "$out"
if echo "$out" | grep -q 'http=200' && echo "$out" | grep -q '"success": *true' && [ "$(cat "$R/media/emmc/e2e.ctb" 2>/dev/null)" = CTBDATA ] && ! ls "$R/media/emmc"/.*.part >/dev/null 2>&1; then pass gksdcp_upload_ok_after_mount; else fail gksdcp_upload_ok_after_mount "$out"; fi
out=$(python3 /tmp/e2e_sdcp.py ws0 2>&1)
echo "$out"
if echo "$out" | grep -q '"Cmd": *0' && echo "$out" | grep -q '"Ack": *0'; then pass gksdcp_websocket_cmd0; else fail gksdcp_websocket_cmd0 "$out"; fi
if [ "${WEBUI:-1}" = 1 ]; then
  # Capture first: with pipefail, grep -q exiting early makes curl fail (exit 23) on a large page.
  page=$(curl -s --max-time 5 http://127.0.0.1:80/)
  if grep -qi '<html' <<<"$page"; then pass gksdcp_page; else fail gksdcp_page; fi
else
  if curl -s --max-time 3 -o /dev/null http://127.0.0.1:80/; then fail gksdcp_no_page "port 80 answers with WEBUI=0"; else pass gksdcp_no_page; fi
fi
echo "--- R97gksdcp restart"
chroot "$R" /customer/gk/init.d/R97gksdcp restart
npid=$(cat "$R/tmp/gksdcp.pid" 2>/dev/null)
sleep 1
out=$(python3 /tmp/e2e_sdcp.py discover 2>&1)
if [ -n "$npid" ] && [ "$npid" != "$gpid" ] && ! kill -0 "$gpid" 2>/dev/null && kill -0 "$npid" 2>/dev/null && echo "$out" | grep -q 'V3.0.0'; then
  pass gksdcp_restart_serving; else fail gksdcp_restart_serving "old=$gpid new=$npid $out"; fi
gpid=$npid
echo "--- R97gksdcp stop"
chroot "$R" /customer/gk/init.d/R97gksdcp stop
sleep 1
if [ -n "$gpid" ] && ! kill -0 "$gpid" 2>/dev/null && [ ! -f "$R/tmp/gksdcp.pid" ]; then pass gksdcp_stop; else fail gksdcp_stop; fi

# --- (g) libgkhook.so: the loader accepts it as LD_PRELOAD (a non-chitu process, so the hook stays
#         inert by design), and the shipped app0.sh launches with it, or falls back when it is absent.
echo "--- libgkhook preload"
HKSO=/customer/gk/lib/libgkhook.so
err=$(chroot "$R" /bin/sh -c "LD_PRELOAD=$HKSO /bin/true" 2>&1 >/dev/null); rc=$?
if [ "$rc" = 0 ] && ! echo "$err" | grep -Eqi 'ld\.so|cannot open shared object|undefined symbol|error while loading'; then pass gkhook_preload_loads; else fail gkhook_preload_loads "rc=$rc $err"; fi
# run the real app0.sh with chitu swapped for a probe that prints the LD_PRELOAD it received
probe="/bin/sh -c 'echo LP=\$LD_PRELOAD'"   # single-quoted inside, so the preload env is what it prints
sed "s#/customer/resources/chitu#$probe#g" "$R/customer/app0.sh" > "$R/tmp/app0probe.sh"
out=$(chroot "$R" /bin/sh /tmp/app0probe.sh; wait)
if [ "$out" = "LP=$HKSO" ]; then pass app0_uses_hook; else fail app0_uses_hook "got '$out'"; fi
mv "$R$HKSO" "$R$HKSO.hidden"
out=$(chroot "$R" /bin/sh /tmp/app0probe.sh; wait)
mv "$R$HKSO.hidden" "$R$HKSO"
if [ "$out" = "LP=" ]; then pass app0_falls_back_without_hook; else fail app0_falls_back_without_hook "got '$out'"; fi

if [ "$FAILS" = 0 ]; then echo "E2E OK"; else echo "E2E FAILED ($FAILS)"; exit 1; fi
