"""Normalise the toolkit-mangled version header in place: usage fix-header-version.py <image>.
Toolkit's version bump turns GKtwoV1.3.3 into "VGKtwo1.3.4". Exactly one of these must hold in
the first 4096 bytes, else exit 1:
  (a) mangled '#upgrade_bin_version=VGKtwo' present -> rewrite to 'GKtwoV' (same length), re-check
  (b) correct '#upgrade_bin_version=GKtwoV' already present -> no-op
File length must be unchanged."""
import sys, os
p = sys.argv[1]
OLD, NEW = b"#upgrade_bin_version=VGKtwo", b"#upgrade_bin_version=GKtwoV"
size = os.path.getsize(p)
d = bytearray(open(p, "rb").read())
head = bytes(d[:4096])
has_old, has_new = head.count(OLD), head.count(NEW)
if has_old == 1 and has_new == 0:
    i = head.find(OLD)
    d[i:i + len(OLD)] = NEW
    open(p, "wb").write(d)
    head = bytes(open(p, "rb").read(4096))
    if head.count(NEW) != 1 or head.count(OLD) != 0:
        sys.exit("fix-header-version: re-check failed after patch")
    print("header version patched")
elif has_old == 0 and has_new == 1:
    print("header version already correct")
else:
    sys.exit("fix-header-version: neither/ambiguous version header (old=%d new=%d)" % (has_old, has_new))
if os.path.getsize(p) != size:
    sys.exit("fix-header-version: file length changed")
