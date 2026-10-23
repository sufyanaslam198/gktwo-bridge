# Live checks for chitu V1.3.3 (run on the host against gdbserver on the printer):
#   printer:  gdbserver --attach :2345 $(pidof chitu)
#   host:     gdb-multiarch -q -ex "target remote <printer-ip>:2345" -x tools/re/live-checks.gdb <chitu-binary>
#             (<chitu-binary> = the stock chitu file, e.g. /work/tk/scripts/customer/resources/chitu)
# (cd /customer/resources; chitu has no relevant DWARF, so everything is raw addresses.)
set architecture arm
set pagination off
define pstate
  set $pr = *(unsigned int*)0x101754
  printf "printer_t* = %#x\n", $pr
  if $pr != 0
    printf "state=%d layer=%u pauseflag=%u total_est_ms=%llu\n", *(int*)($pr+0x260), *(unsigned*)($pr+0x258), *(unsigned char*)($pr+0x268), *(unsigned long long*)($pr+0x270)
    set $fi = *(unsigned int*)($pr+0xbc) + 0x1004
    printf "file_info: xres=%u yres=%u layer_h=%f total_layers=%u\n", *(unsigned*)$fi, *(unsigned*)($fi+4), *(float*)($fi+8), *(unsigned*)($fi+12)
    printf "err=%d stopflag705=%u pend706=%u\n", *(int*)($pr+0x25c), *(unsigned char*)($pr+705), *(unsigned char*)($pr+706)
  end
  printf "ui: last=%d cur=%d next=%d param=%#x updatecb=%#x msgbox_busy=%u\n", *(int*)0x1078e8, *(int*)0x1078ec, *(int*)0x1078f0, *(unsigned*)0x1078f4, *(unsigned*)0x1078f8, *(unsigned char*)0x101224
  x/s 0x101348
end
document pstate
Dump printer_t + ui globals (call while idle, printing, paused, finished).
end
# Typical session:  (gdb) pstate ; (gdb) continue ; ^C ; (gdb) pstate
