#!/usr/bin/env bash
# usage (Git Bash, project root):  tools/build-all.sh
# dropbear build (gktwo-cc) -> firmware build + verify (gktwo-fw, PHASE1=1). Non-zero on any failure.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
export WEBUI=${WEBUI:-1}   # 1 = the web page is built into gksdcp; passed to the containers below
DOCK_ARGS="-e WEBUI" tools/dock.sh gktwo-cc 'set -e
deps/dropbear/build.sh
make -C gksdcp clean arm WEBUI=$WEBUI
cp gksdcp/build/gksdcp /work/bin/gksdcp
tools/check-glibc.sh /work/bin/gksdcp
ls -l /work/bin/gksdcp
make -C gkhook arm
cp gkhook/build/libgkhook.so /work/bin/libgkhook.so
tools/check-glibc.sh /work/bin/libgkhook.so
ls -l /work/bin/libgkhook.so'
tools/dock.sh gktwo-fw 'set -e
export PHASE1=1 ALLOW_CHANGED="rootfs/etc/init.d/rcS rootfs/etc/shadow customer/app0.sh" EXPECT_ADDED=/src/fw/expected-added.txt
fw/build-firmware.sh
fw/verify-image.sh /src/out/ChituUpgrade.bin'
DOCK_ARGS="--privileged -e WEBUI" tools/dock.sh gktwo-fw 'fw/e2e-ssh-test.sh'
(cd out && sha256sum ChituUpgrade.bin > ChituUpgrade.bin.sha256 && cat ChituUpgrade.bin.sha256)
