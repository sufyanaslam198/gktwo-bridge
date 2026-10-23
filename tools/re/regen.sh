#!/bin/bash
# Regenerate every static artefact used by docs/reverse-engineering/chitu-print-control.md.
# Run inside the gktwo-fw container:
#   tools/dock.sh gktwo-fw 'bash tools/re/regen.sh'
# Output goes to /work/re (Docker volume), never into the repo.
source "$(dirname "$0")/env.sh"
T="$(cd "$(dirname "$0")" && pwd)"
arm-linux-gnueabihf-objdump -d --no-show-raw-insn "$CH" > "$OUT/dis.txt"      # full disassembly (ARM mode only, no Thumb)
arm-linux-gnueabihf-nm -n "$CH" > "$OUT/nm.txt"                               # mangled symbols
arm-linux-gnueabihf-nm -n -C "$CH" | grep -E ' [TtWw] ' > "$OUT/funcs.txt"    # demangled function list
arm-linux-gnueabihf-readelf --debug-dump=info "$CH" > "$OUT/info.txt" 2>&1    # DWARF: only 5 glibc CRT CUs (see doc section 0)
{
  echo "== DWARF compile units:"; grep -c DW_TAG_compile_unit "$OUT/info.txt"
  echo "== .debug_info size:"; arm-linux-gnueabihf-readelf -S "$CH" | grep -A1 debug_info
  echo "== gdb ptype print_context_t (expected: No symbol):"
  gdb-multiarch -q -batch -ex 'set architecture arm' -ex 'ptype print_context_t' "$CH" 2>&1 | tail -2
  echo "== file size / sha256:"; stat -c %s "$CH"; sha256sum "$CH"
  echo "== fingerprint bytes:"
  for a in 3de64 3a1fc 3a224 3a248 3a26c 39728 4fed8 4ffe0 13d3c 3ac18 3ac20; do python3 -I "$T/vaddr.py" hex $a 16; done
} > "$OUT/summary.txt"
cat "$OUT/summary.txt"
