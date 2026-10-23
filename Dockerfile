# GKtwo Bridge build environment. One file, four stages:
#   cc      Debian buster: ARM cross compiler against glibc 2.28 (the C library of the printer)
#   fw      Debian bookworm: firmware toolkit dependencies, qemu, host gcc, Python, tests
#   addons  cc + source: builds dropbearmulti, gksdcp (with and without the web page), libgkhook.so
#   bridge  fw + the add-ons + the pipeline: builds an image from the folder mounted at /data
# The default target is bridge (the last stage). Developer images for tools/build-all.sh and tools/dock.sh:
#   docker build --target cc -t gktwo-cc .
#   docker build --target fw -t gktwo-fw .

FROM debian:buster AS cc
# buster is EOL: use the archive. The base image ships libbz2 1.0.6-9.2~deb10u2 from
# security, so the security archive is needed for bzip2 to install (version pin).
RUN echo "deb http://archive.debian.org/debian buster main" > /etc/apt/sources.list \
 && echo "deb http://archive.debian.org/debian-security buster/updates main" >> /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    crossbuild-essential-armhf make bzip2 wget ca-certificates file xxd \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM debian:bookworm AS fw
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    binwalk python3 python3-pip python3-lzo openjdk-17-jre-headless mtd-utils file xxd \
    gcc-arm-linux-gnueabihf libc6-dev-armhf-cross make git ca-certificates binutils-arm-linux-gnueabihf \
    libarchive-zip-perl gdb-multiarch build-essential python3-pytest curl bzip2 wget openssl \
    qemu-user-static sudo dos2unix squashfs-tools cpio openssh-client sshpass && rm -rf /var/lib/apt/lists/*
RUN pip3 install --break-system-packages ubi_reader
WORKDIR /work

FROM cc AS addons
COPY . /src
RUN deps/dropbear/build.sh \
 && make -C gksdcp arm WEBUI=1 \
 && make -C gksdcp arm WEBUI=0 BUILD=build-noweb \
 && make -C gkhook arm \
 && mkdir -p /out/bin /out/licenses \
 && cp /work/bin/dropbearmulti gksdcp/build/gksdcp gkhook/build/libgkhook.so /out/bin/ \
 && cp gksdcp/build-noweb/gksdcp /out/bin/gksdcp-noweb \
 && tools/check-glibc.sh /out/bin/* \
 && cp LICENSE /out/licenses/gktwo-bridge.txt \
 && cp /work/deps/dropbear/LICENSE /out/licenses/dropbear.txt \
 && sed -n '1,/\*\//p' gksdcp/src/jsmn.h > /out/licenses/jsmn.txt

# only the add-ons, for a release archive: docker build --target addons-out -o dist .
FROM scratch AS addons-out
COPY --from=addons /out /

FROM fw AS bridge
LABEL org.opencontainers.image.title="GKtwo Bridge" \
      org.opencontainers.image.description="Builds a GKtwo Bridge firmware image from the stock Uniformation GKtwo firmware" \
      org.opencontainers.image.source="https://github.com/sufyanaslam198/gktwo-bridge"
COPY . /src
COPY --from=addons /out /opt/gktwo
ENV SRC=/src DATA=/data BIN=/work/bin
WORKDIR /src
ENTRYPOINT ["/src/fw/image-entrypoint.sh"]
