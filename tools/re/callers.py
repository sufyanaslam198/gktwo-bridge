#!/usr/bin/env python3
"""usage: callers.py <sym|hexaddr>...  -> call sites (bl/b/blx) + literal-pool/data refs, with enclosing function.
Needs /work/re/dis.txt and /work/re/nm.txt (run regen.sh first)."""
import re, sys
nm = {}
for l in open('/work/re/nm.txt'):
    p = l.split()
    if len(p) == 3: nm.setdefault(p[2], int(p[0], 16))
dis = open('/work/re/dis.txt').read().split('\n')
for t in sys.argv[1:]:
    if re.fullmatch(r'(0x)?[0-9a-f]{5,8}', t): a = int(t, 16)
    else:
        a = nm.get(t) or next((v for k, v in nm.items() if k.startswith(t + '.')), None)
    print(f'== {t} @ {a:08x}' if a is not None else f'== {t} NOT FOUND')
    if a is None: continue
    fn = '?'
    pat = re.compile(r'\t(bl|b|blx)[a-z]{0,2}\t0*%x <' % a)
    for l in dis:
        m = re.match(r'^([0-9a-f]{8}) <(.*)>:', l)
        if m: fn = m.group(2); continue
        if pat.search(l): print('  ', l.split(':')[0].strip(), 'in', fn, '|', l.split('\t',1)[1].strip())
        elif re.search(r'\.word\t0x0*%x\b' % a, l): print('  data/litpool', l.split(':')[0].strip(), 'in', fn)
