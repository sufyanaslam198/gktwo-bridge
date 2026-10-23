// Printer identity: MainboardID, persisted display name, brand ID, firmware version string.
#ifndef GKSDCP_IDENT_H
#define GKSDCP_IDENT_H
#include <netinet/in.h>

#define GK_BRAND_ID    "da6371eab4f602742504df5e03abd18b"   // fixed project identifier (SDCP "Id")
#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "0.0.0"   // the Makefile sets it from the VERSION file
#endif
#define GK_NAME_DEFAULT "GKtwo"
#define GK_NAME_MAX    64

// "02:00:00:aa:bb:cc" (trailing whitespace allowed) -> "0000020000aabbcc". 0 ok / -1 malformed.
int ident_id_from_mac(const char *mac, char out[17]);

// MainboardID: override_id if given (16 hex), else the MAC of iface, else 0000000000000001 (logged).
// Loads the display name from <var_dir>/name (missing or invalid file = default). 0 ok / -1 bad override.
int ident_init(const char *iface, const char *override_id, const char *var_dir);
// While the fallback ID is in use, re-reads the MAC (at most once a second) and switches to the real ID.
const char *ident_mainboard_id(void);
// Test seam: directory holding <iface>/address instead of /sys/class/net. Call before ident_init.
void ident_set_sysfs_root(const char *dir);
const char *ident_name(void);
// 1..64 bytes of valid UTF-8 without control bytes; persisted to <var_dir>/name (created if needed)
// via a temp file and rename. 0 ok / -1 invalid or write failed (the name is then unchanged).
int ident_set_name(const char *name);
// 1 if s is strict UTF-8 (no overlong forms, surrogates, > U+10FFFF) without control bytes (< 0x20, 0x7f).
int ident_utf8_ok(const char *s);
// mkdir -p (mode 0755, path up to 255 bytes). 0 ok / -1.
int ident_mkdir_p(const char *dir);
const char *ident_fw_version(void);   // FW_VERSION "-bridge-" BRIDGE_VERSION
// Dotted-quad text of an IPv4 address. 0 ok / -1.
int ident_ip_for(const struct in_addr *local, char out[16]);
#endif
