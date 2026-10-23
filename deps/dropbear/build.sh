#!/usr/bin/env bash
# usage: tools/dock.sh gktwo-cc 'deps/dropbear/build.sh'
# Builds Dropbear 2022.83 multi-binary for ARMv7 hf / glibc 2.28 -> /work/bin/dropbearmulti.
# Source download is cached in /src/deps/dropbear/src (gitignored); build tree is /work/deps/dropbear.
set -euo pipefail
VER=2022.83
SHA=bc5a121ffbc94b5171ad5ebe01be42746d50aa797c9549a4639894a16749443b
URL=https://matt.ucc.asn.au/dropbear/releases/dropbear-$VER.tar.bz2
SRCDIR=/src/deps/dropbear/src
TARBALL=$SRCDIR/dropbear-$VER.tar.bz2
BUILD=/work/deps/dropbear

mkdir -p "$SRCDIR" /work/bin
[ -f "$TARBALL" ] || wget -q -O "$TARBALL" "$URL"
echo "$SHA  $TARBALL" | sha256sum -c - || { echo "dropbear: SHA-256 mismatch, aborting" >&2; rm -f "$TARBALL"; exit 1; }

rm -rf "$BUILD"; mkdir -p "$BUILD"
tar -xjf "$TARBALL" -C "$BUILD" --strip-components=1
cd "$BUILD"
cat > localoptions.h <<'OPT'
#define DEFAULT_PATH "/customer/gk/bin:/usr/bin:/bin:/usr/sbin:/sbin"
#define DEFAULT_ROOT_PATH "/customer/gk/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define DROPBEAR_SFTPSERVER 0
OPT
./configure --host=arm-linux-gnueabihf --disable-zlib --disable-lastlog --disable-utmp \
  --disable-utmpx --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline \
  CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2" >/dev/null
make -j"$(nproc)" PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 >/dev/null
arm-linux-gnueabihf-strip dropbearmulti
cp dropbearmulti /work/bin/dropbearmulti
/src/tools/check-glibc.sh /work/bin/dropbearmulti
ls -l /work/bin/dropbearmulti
