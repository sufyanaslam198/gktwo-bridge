HK=/customer/gk/lib/libgkhook.so
if [ -r "$HK" ]; then LD_PRELOAD="$HK" /customer/resources/chitu & else /customer/resources/chitu & fi
