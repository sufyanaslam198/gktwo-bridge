#include "ident.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(void) {
  char id[17];
  CHECK(ident_id_from_mac("02:00:00:aa:bb:cc", id) == 0);
  CHECK(strcmp(id, "0000020000aabbcc") == 0);
  CHECK(ident_id_from_mac("02:00:00:AA:BB:CC\n", id) == 0);     // sysfs adds a newline; case folds
  CHECK(strcmp(id, "0000020000aabbcc") == 0);
  CHECK(ident_id_from_mac("02:00:00:aa:bb", id) == -1);
  CHECK(ident_id_from_mac("02:00:00:aa:bb:zz", id) == -1);
  CHECK(ident_id_from_mac("", id) == -1);

  CHECK(strlen(GK_BRAND_ID) == 32);
  CHECK(strcmp(BRIDGE_VERSION, "0.0.0") != 0);                  // the Makefile passed the VERSION file
  CHECK(strstr(ident_fw_version(), "-bridge-" BRIDGE_VERSION) != NULL);

  char tmpl[] = "/tmp/test_identXXXXXX";
  CHECK(mkdtemp(tmpl));
  char var[128];
  snprintf(var, sizeof var, "%s/sub/var", tmpl);     // does not exist yet

  CHECK(ident_init("nonexistent0", "0000aabbccddeeff", var) == 0);
  CHECK(strcmp(ident_mainboard_id(), "0000aabbccddeeff") == 0);
  CHECK(strcmp(ident_name(), "GKtwo") == 0);          // missing file = default

  CHECK(ident_set_name("Resin \xc3\xa9") == 0);        // valid UTF-8
  CHECK(strcmp(ident_name(), "Resin \xc3\xa9") == 0);
  CHECK(ident_init("nonexistent0", "0000aabbccddeeff", var) == 0);   // round-trip through disk
  CHECK(strcmp(ident_name(), "Resin \xc3\xa9") == 0);

  CHECK(ident_set_name("a\x01") == -1);
  CHECK(ident_set_name("") == -1);
  CHECK(ident_set_name("bad\xff") == -1);
  CHECK(ident_set_name("\xc3") == -1);                 // truncated sequence
  CHECK(ident_set_name("\xc0\x80") == -1);             // overlong
  char big[80];
  memset(big, 'x', 65); big[65] = 0;
  CHECK(ident_set_name(big) == -1);
  big[64] = 0;
  CHECK(ident_set_name(big) == 0);
  CHECK(strcmp(ident_name(), "Resin \xc3\xa9") != 0 && strlen(ident_name()) == 64);

  CHECK(ident_init("nonexistent0", NULL, var) == 0);  // no iface, no override
  CHECK(strcmp(ident_mainboard_id(), "0000000000000001") == 0);

  // lazy MAC retry: interface registers after start-up
  char sys[160], ifd[200], af[220];
  snprintf(sys, sizeof sys, "%s/sys", tmpl);
  snprintf(ifd, sizeof ifd, "%s/wlan9", sys);
  snprintf(af, sizeof af, "%s/address", ifd);
  ident_set_sysfs_root(sys);
  CHECK(ident_init("wlan9", NULL, var) == 0);
  CHECK(strcmp(ident_mainboard_id(), "0000000000000001") == 0);
  CHECK(mkdir(sys, 0755) == 0 && mkdir(ifd, 0755) == 0);
  FILE *f = fopen(af, "w");
  CHECK(f);
  fputs("02:00:00:aa:bb:cc\n", f);
  fclose(f);
  CHECK(strcmp(ident_mainboard_id(), "0000000000000001") == 0);   // rate limited: within 1 s
  sleep(2);
  CHECK(strcmp(ident_mainboard_id(), "0000020000aabbcc") == 0);
  CHECK(ident_init("wlan9", "0000aabbccddeeff", var) == 0);        // override wins, never re-read
  CHECK(strcmp(ident_mainboard_id(), "0000aabbccddeeff") == 0);

  struct in_addr a;
  char ip[16];
  inet_pton(AF_INET, "192.168.1.42", &a);
  CHECK(ident_ip_for(&a, ip) == 0 && strcmp(ip, "192.168.1.42") == 0);

  char cmd[160];
  snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl);
  CHECK(system(cmd) == 0);
  printf("test_ident ok\n");
  return 0;
}
