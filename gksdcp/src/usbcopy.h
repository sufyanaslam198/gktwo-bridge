// USB drive: UsbDiskStatus (checked every 2 s, pushed on change) and the copy of a USB file to internal
// storage that print-from-USB (Cmd 128 /usb/...) runs before it starts the print.
#ifndef GKSDCP_USBCOPY_H
#define GKSDCP_USBCOPY_H

// 0 ok / -1 not a readable regular file / -2 no CTB magic (first 4 bytes 0x12fd0106 or 0x12fd0107, little-endian).
// *size gets the file size (may be NULL).
int usbcopy_probe(const char *src_abs, long long *size);

// Copies src_abs to <files dir>/dst_name through the temp file .<dst_name>.usb.part (flock'ed), 256 KB per
// loop iteration, then fsync + rename. done(ok, err) runs on the loop thread when it ends: ok 1 on success,
// ok 0 with the reason on failure (partial copy already removed) and "cancelled" after usbcopy_cancel.
// An existing file of that name is replaced only while the hook is reachable and idle, complete or in error.
// 0 started / -2 refused for now (the hook is not idle or unreachable, an upload or copy runs, the name is in use,
// the storage is not mounted) / -1 refused otherwise (*why is set, a static string; done is not called); other
// refusals: source not a CTB, not enough space (free - size < the -R reserve), temp file not creatable.
// The caller sends the reason, after its response.
int usbcopy_begin(const char *src_abs, const char *dst_name, void (*done)(int ok, const char *err),
                  const char **why);
void usbcopy_cancel(void);
// 1 while a copy runs (progress in bytes through done/total, either may be NULL).
int usbcopy_active(long long *done, long long *total);

// Starts the UsbDiskStatus check and the copy stepper. Call after files_init, before loop_run.
void usbcopy_init(void);
#endif
