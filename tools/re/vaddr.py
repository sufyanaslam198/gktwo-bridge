#!/usr/bin/env python3
"""usage: vaddr.py str <addr>... | hex <addr> <n> | word <addr>...   - read the chitu ELF image by virtual address (PT_LOAD mapping)."""
import struct, sys, os
P = os.environ.get('CH', '/work/tk/scripts/customer/resources/chitu')
d = open(P, 'rb').read()
phoff, = struct.unpack_from('<I', d, 0x1c); phentsize, phnum = struct.unpack_from('<HH', d, 0x2a)
loads = []
for i in range(phnum):
    t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<8I', d, phoff + i * phentsize)
    if t == 1: loads.append((va, off, fs))
def foff(a):
    for va, off, fs in loads:
        if va <= a < va + fs: return off + a - va
    raise SystemExit('addr %x not file-backed' % a)
cmd = sys.argv[1]
if cmd == 'str':
    for a in sys.argv[2:]:
        o = foff(int(a, 16)); e = d.index(b'\0', o); print(a, repr(d[o:e].decode('latin1')))
elif cmd == 'hex':
    a = int(sys.argv[2], 16); n = int(sys.argv[3]); o = foff(a); print('vaddr %x fileoff %#x: %s' % (a, o, d[o:o+n].hex(' ')))
elif cmd == 'word':
    for a in sys.argv[2:]:
        print(a, '%#x' % struct.unpack_from('<I', d, foff(int(a, 16)))[0])
elif cmd == 'map':
    for va, off, fs in loads: print('PT_LOAD va=%#x off=%#x filesz=%#x' % (va, off, fs))
