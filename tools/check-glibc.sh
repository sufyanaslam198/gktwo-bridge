#!/usr/bin/env bash
# usage: tools/check-glibc.sh <elf>...  ; exit 0 iff every file is an ARM ELF needing only GLIBC <= 2.28
max=28; rc=0
[ $# -ge 1 ] || { echo "usage: $0 <elf>..." >&2; exit 2; }
for f in "$@"; do
  if ! hdr=$(arm-linux-gnueabihf-readelf -h "$f" 2>&1) || ! grep -q 'Machine:.*ARM' <<<"$hdr"; then
    echo "FAIL $f: not an ARM ELF"; rc=1; continue
  fi
  bad=$(arm-linux-gnueabihf-readelf -V "$f" 2>/dev/null | grep -o 'GLIBC_2\.[0-9]*' | sort -u \
        | awk -F. -v m=$max '$2+0 > m')
  if [ -n "$bad" ]; then
    echo "FAIL $f: needs newer than GLIBC_2.$max:"; echo "$bad" | sed 's/^/  /'; rc=1
  else
    echo "ok   $f"
  fi
done
exit $rc
