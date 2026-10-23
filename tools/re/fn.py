#!/usr/bin/env python3
"""usage: fn.py <exact-symbol|substring|0xaddr> [from_off to_off]  -> disassembly of that function from /work/re/dis.txt"""
import re, sys
t = sys.argv[1]
out = []; on = False
if t.startswith('0x'):
    key = '%08x' % int(t, 16); test = lambda m: m.group(1) == key
else:
    test = lambda m: m.group(2) == t or (t in m.group(2) and not any(c in t for c in ' '))
for l in open('/work/re/dis.txt'):
    m = re.match(r'^([0-9a-f]{8}) <(.*)>:', l)
    if m:
        if on: break
        if test(m): on = True
    if on: out.append(l.rstrip('\n'))
print('\n'.join(out))
