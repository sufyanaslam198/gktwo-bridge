#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/proto.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int parse(const char* s, struct gk_cmd* c) { memset(c, 0, sizeof *c); return proto_parse(s, strlen(s), c); }

// ends with exactly one \n, no NUL, < 2048
static int clean(const char* b, size_t n) {
  return n > 0 && n < PROTO_MAX && b[n - 1] == '\n' && memchr(b, 0, n) == NULL && memchr(b, '\n', n) == b + n - 1;
}

static void test_z(void) {
  struct gk_cmd c;
  static const int ok[] = { 1, 10, 100, -1, -10, -100 };
  char line[80];
  for (int i = 0; i < 6; i++) {  // test_proto_z_commands
    snprintf(line, sizeof line, "{\"cmd\":\"z_move\",\"mm\":%d}\n", ok[i]);
    CHECK(parse(line, &c) == 0 && c.kind == CMD_Z_MOVE && c.mm == ok[i]);
  }
  CHECK(parse("{\"mm\": -10 ,\"cmd\":\"z_move\"}", &c) == 0 && c.mm == -10);
  CHECK(parse("{\"cmd\":\"z_top\"}\n", &c) == 0 && c.kind == CMD_Z_TOP);
  CHECK(parse("{\"cmd\":\"z_home\"}\n", &c) == 0 && c.kind == CMD_Z_HOME);
  CHECK(parse("{\"cmd\":\"z_stop\"}\n", &c) == 0 && c.kind == CMD_Z_STOP);
  // test_proto_z_move_bad_mm
  const char* bad[] = {
    "{\"cmd\":\"z_move\",\"mm\":5}", "{\"cmd\":\"z_move\",\"mm\":1.5}", "{\"cmd\":\"z_move\",\"mm\":\"10\"}",
    "{\"cmd\":\"z_move\"}", "{\"cmd\":\"z_move\",\"mm\":1e2}", "{\"cmd\":\"z_move\",\"mm\":1000000000000}",
    "{\"cmd\":\"z_move\",\"mm\":0}", "{\"cmd\":\"z_move\",\"mm\":10.0}", "{\"cmd\":\"z_move\",\"mm\":-}",
    "{\"cmd\":\"z_move\",\"mm\":null}", "{\"cmd\":\"z_move\",\"mm\":[10]}", "{\"cmd\":\"z_move\",\"mm\":+10}",
  };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    if (parse(bad[i], &c) != -1) fprintf(stderr, "  bad mm accepted: %s\n", bad[i]);
    CHECK(parse(bad[i], &c) == -1);
  }
  // test_proto_status_z
  char b[PROTO_MAX];
  struct gk_status st;
  memset(&st, 0, sizeof st);
  strcpy(st.state, "idle");
  st.layer = st.total_layers = st.elapsed_s = st.remaining_s = -1; st.progress = -1;
  size_t n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n) && strstr(b, ",\"z\":null,\"phase\":null,\"screen\":null}\n") != NULL);
  st.z_avail = 1;
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n) && strstr(b, ",\"z\":{\"pos\":null,\"homed\":false,\"moving\":false},\"phase\":null,\"screen\":null}\n") != NULL);
  st.z_homed = 1; st.z_pos = 12.345; st.z_moving = 1;
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n) && strstr(b, ",\"z\":{\"pos\":12.35,\"homed\":true,\"moving\":true},\"phase\":null,\"screen\":null}\n") != NULL);
  st.z_pos = 0; st.z_moving = 0;
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n) && strstr(b, "\"pos\":0.00,") != NULL);
  double badpos[] = { NAN, INFINITY, 1e300 };
  for (int i = 0; i < 3; i++) {
    st.z_pos = badpos[i];
    n = proto_reply_status(b, sizeof b, &st);
    CHECK(clean(b, n) && strstr(b, "\"pos\":null,\"homed\":true") != NULL);
  }
  // worst case with z present still fits
  st.z_pos = 299.99; st.have_file = 1;
  memset(st.file, '"', sizeof st.file - 1); st.file[sizeof st.file - 1] = 0;
  memset(st.message, '\\', sizeof st.message - 1); st.message[sizeof st.message - 1] = 0;
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(n && n < PROTO_MAX && clean(b, n) && strstr(b, "\"pos\":299.99") != NULL);
}

static void test_status_phase_screen_json(void) {
  char b[PROTO_MAX];
  struct gk_status st;
  memset(&st, 0, sizeof st);
  strcpy(st.state, "idle");
  st.layer = st.total_layers = st.elapsed_s = st.remaining_s = -1; st.progress = -1;
  st.screen_x = 7680; st.screen_y = 4320;
  size_t n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n) && strstr(b, ",\"z\":null,\"phase\":null,\"screen\":[7680,4320]}\n") != NULL);
  strcpy(st.phase, "homing"); st.screen_x = st.screen_y = 0; strcpy(st.message, "x");
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n) && strstr(b, ",\"z\":null,\"phase\":\"homing\",\"screen\":null,\"message\":\"x\"}\n") != NULL);
}

int main(void) {
  struct gk_cmd c;
  CHECK(parse("{\"cmd\":\"status\"}\n", &c) == 0 && c.kind == CMD_STATUS);
  CHECK(parse("{\"cmd\":\"pause\"}", &c) == 0 && c.kind == CMD_PAUSE);
  CHECK(parse("{ \"cmd\" : \"resume\" }\r\n", &c) == 0 && c.kind == CMD_RESUME);
  CHECK(parse("{\"cmd\":\"stop\"}\n", &c) == 0 && c.kind == CMD_STOP);
  CHECK(parse("{\"cmd\":\"start\",\"path\":\"/media/emmc/x.ctb\"}\n", &c) == 0 && c.kind == CMD_START);
  CHECK(strcmp(c.path, "/media/emmc/x.ctb") == 0);
  CHECK(parse("{\"cmd\":\"start\",\"path\":\"/media/emmc/my file (1)+a%20b.ctb\"}\n", &c) == 0);
  CHECK(strcmp(c.path, "/media/emmc/my file (1)+a%20b.ctb") == 0);
  CHECK(parse("{\"path\":\"/media/emmc/k.ctb\",\"cmd\":\"start\"}\n", &c) == 0 && strcmp(c.path, "/media/emmc/k.ctb") == 0);
  // json_escape forms
  CHECK(parse("{\"cmd\":\"start\",\"path\":\"/a\\\"b\\\\c\\u0041\\u00e9.ctb\"}\n", &c) == 0);
  CHECK(strcmp(c.path, "/a\"b\\cA\xc3\xa9.ctb") == 0);
  // a path value that looks like a command must not confuse the scanner
  CHECK(parse("{\"cmd\":\"status\",\"path\":\"\\\"cmd\\\":\\\"stop\\\"\"}\n", &c) == 0 && c.kind == CMD_STATUS);
  // errors
  CHECK(parse("{\"cmd\":\"start\"}\n", &c) == -1);
  CHECK(parse("{\"cmd\":\"start\",\"path\":\"\"}\n", &c) == -1);
  CHECK(parse("{\"cmd\":\"start\",\"path\":\"\\u0000x\"}\n", &c) == -1);
  CHECK(parse("{\"cmd\":\"reboot\"}\n", &c) == -2);
  CHECK(parse("{}\n", &c) == -1);
  CHECK(parse("garbage\n", &c) == -1);
  CHECK(parse("{\"cmd\":\"status\"\n", &c) == -1);
  CHECK(parse("", &c) == -1);
  {
    char big[1400];
    memset(big, 'a', sizeof big);
    memcpy(big, "{\"cmd\":\"start\",\"path\":\"", 23);
    memcpy(big + sizeof big - 4, "\"}\n", 4);
    CHECK(proto_parse(big, sizeof big - 1, &c) == -1);  // path too long
  }

  char b[PROTO_MAX];
  size_t n = proto_reply_ok(b, sizeof b);
  CHECK(n && clean(b, n) && memcmp(b, "{\"ok\":true}\n", n) == 0);
  n = proto_reply_err(b, sizeof b, "wrong chitu version");
  CHECK(clean(b, n) && memcmp(b, "{\"ok\":false,\"error\":\"wrong chitu version\"}\n", n) == 0);
  n = proto_reply_err(b, sizeof b, "q\"\\\n\x01\x7f" "z");  // escape quote/backslash, strip controls
  CHECK(clean(b, n) && memcmp(b, "{\"ok\":false,\"error\":\"q\\\"\\\\z\"}\n", n) == 0);
  {
    char huge[3000];
    memset(huge, 'x', sizeof huge - 1); huge[sizeof huge - 1] = 0;
    n = proto_reply_err(b, sizeof b, huge);
    CHECK(n == 0 || clean(b, n));  // never an oversized reply
    CHECK(proto_reply_err(b, 10, "x") == 0);
    CHECK(proto_reply_ok(b, 5) == 0);
  }

  struct gk_status st;
  memset(&st, 0, sizeof st);
  strcpy(st.state, "idle");
  st.layer = st.total_layers = st.elapsed_s = st.remaining_s = -1; st.progress = -1;
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n));
  CHECK(memcmp(b, "{\"ok\":true,\"state\":\"idle\",\"file\":null,\"layer\":null,\"total_layers\":null,"
                  "\"progress\":null,\"elapsed_s\":null,\"remaining_s\":null,\"z\":null,\"phase\":null,\"screen\":null}\n", n) == 0);
  strcpy(st.state, "printing"); st.have_file = 1; strcpy(st.file, "/media/emmc/a \"b\".ctb");
  st.layer = 12; st.total_layers = 400; st.progress = 3.456; st.elapsed_s = 90; st.remaining_s = 3000;
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n));
  CHECK(memcmp(b, "{\"ok\":true,\"state\":\"printing\",\"file\":\"/media/emmc/a \\\"b\\\".ctb\",\"layer\":12,"
                  "\"total_layers\":400,\"progress\":3.46,\"elapsed_s\":90,\"remaining_s\":3000,\"z\":null,\"phase\":null,\"screen\":null}\n", n) == 0);
  // F8: a non-finite progress is null, never garbage from an out-of-range cast
  {
    double bad[] = { NAN, INFINITY, -INFINITY };
    for (int i = 0; i < 3; i++) {
      st.progress = bad[i];
      n = proto_reply_status(b, sizeof b, &st);
      CHECK(clean(b, n) && strstr(b, "\"progress\":null,\"elapsed_s\":90") != NULL);
    }
    st.progress = 3.456;
  }
  strcpy(st.message, "file check failed");
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(clean(b, n) && strstr(b, ",\"z\":null,\"phase\":null,\"screen\":null,\"message\":\"file check failed\"}\n") != NULL);
  // worst case: huge file and message full of escapes must still fit
  memset(st.file, '"', sizeof st.file - 1); st.file[sizeof st.file - 1] = 0;
  memset(st.message, '\\', sizeof st.message - 1); st.message[sizeof st.message - 1] = 0;
  n = proto_reply_status(b, sizeof b, &st);
  CHECK(n && clean(b, n));
  CHECK(proto_reply_status(b, 50, &st) == 0);

  test_z();
  test_status_phase_screen_json();
  if (fails) return 1;
  puts("test_proto OK");
  return 0;
}
