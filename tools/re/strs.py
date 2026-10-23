#!/usr/bin/env python3
"""usage: strs.py <regex>  -> printable strings in the chitu image matching regex, with vaddr (PT_LOAD mapped)."""
import re, struct, sys, os
P = os.environ.get('CH', '/work/tk/scripts/customer/resources/chitu')
d = open(P, 'rb').read()
phoff, = struct.unpack_from('<I', d, 0x1c); es, n = struct.unpack_from('<HH', d, 0x2a)
loads = [struct.unpack_from('<8I', d, phoff + i * es) for i in range(n)]
loads = [(l[2], l[1], l[4]) for l in loads if l[0] == 1]
rx = re.compile(sys.argv[1])
for m in re.finditer(rb'[\x20-\x7e]{3,}', d):
    s = m.group().decode()
    if rx.search(s):
        o = m.start(); va = next((v + o - f for v, f, sz in loads if f <= o < f + sz), None)
        print('%s %r' % ('%08x' % va if va else '--------', s))
