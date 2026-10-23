#!/bin/sh
# Runs every executable /customer/gk/init.d/R??* with "start", in name order.
for i in /customer/gk/init.d/R??*; do
    [ -f "$i" ] && [ -x "$i" ] || continue
    echo "== $(date) $i start" >>/tmp/gk-boot.log 2>&1
    "$i" start >>/tmp/gk-boot.log 2>&1
done
exit 0
