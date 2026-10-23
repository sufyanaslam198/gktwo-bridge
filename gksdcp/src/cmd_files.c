// File commands: SDCP Cmd 258 (file list), 259 (batch delete) and 192 (rename the printer).
#include "files.h"
#include "hookw.h"
#include "ident.h"
#include "sdcp.h"
#include "status.h"
#include "upload.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define LIST_MAX      500     // entries returned by one listing; bounds memory
#define MAX_DEPTH     8       // folder levels below a folder that is deleted
#define CLIENT_MAX    1100    // longest client path read from a request
#define ENTRY_NAME    201     // name_valid allows 200 bytes

struct entry {
  char name[ENTRY_NAME];
  long long mtime;
  int type;                   // 0 folder, 1 file
};

// 1 when path (after resolving symlinks) is root or inside it.
static int confined(const char *path, int storage) {
  char a[PATH_MAX], b[PATH_MAX];
  if (!realpath(path, a) || !realpath(files_root(storage), b)) return 0;
  size_t n = strlen(b);
  return strncmp(a, b, n) == 0 && (a[n] == 0 || a[n] == '/' || n == 1);
}

static int is_ctb(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && strcasecmp(dot, ".ctb") == 0;
}

// ---- Cmd 258 ----

static int entry_cmp(const void *a, const void *b) {
  const struct entry *x = a, *y = b;
  if (x->type != y->type) return x->type - y->type;                 // folders (0) first
  if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;    // newest first
  return strcmp(x->name, y->name);
}

static void list_reply(const struct sdcp_req *r, int ack) {
  sdcp_reply(r->conn, r->cmd, r->rid, ack, ",\"FileList\":[]");
}

static void cmd_list(struct sdcp_req *r) {
  char url[CLIENT_MAX] = "/local/", dir[1100], cdir[1100];
  int t = r->data >= 0 ? jget(r->d, r->data, "Url") : -1;
  if (t >= 0 && jstr(r->d, t, url, sizeof url) < 0) { list_reply(r, 1); return; }
  if (!url[0]) strcpy(url, "/local/");
  int st, rc = files_resolve(url, dir, sizeof dir, &st);
  if (rc == -2) { list_reply(r, 0); return; }                      // no USB drive: nothing to list
  struct stat sb;
  if (rc < 0 || stat(dir, &sb) < 0 || !S_ISDIR(sb.st_mode) || !confined(dir, st) ||
      files_client_path(dir, cdir, sizeof cdir) < 0) {
    list_reply(r, 1);
    return;
  }
  size_t cl = strlen(cdir);
  if (cl + 1 < sizeof cdir && cdir[cl - 1] != '/') { cdir[cl++] = '/'; cdir[cl] = 0; }

  DIR *d = opendir(dir);
  if (!d) { list_reply(r, 1); return; }
  struct entry *v = malloc(LIST_MAX * sizeof *v);
  if (!v) { closedir(d); list_reply(r, 1); return; }
  int n = 0, truncated = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (!name_valid(de->d_name)) continue;                          // also drops dot files
    struct stat es;
    if (fstatat(dirfd(d), de->d_name, &es, AT_SYMLINK_NOFOLLOW) < 0) continue;
    int type;
    if (S_ISDIR(es.st_mode)) type = 0;
    else if (S_ISREG(es.st_mode) && is_ctb(de->d_name)) type = 1;   // .part and other types never
    else continue;
    struct entry e;
    strcpy(e.name, de->d_name);                                     // name_valid: at most 200 bytes
    e.mtime = (long long)es.st_mtime;
    e.type = type;
    if (n < LIST_MAX) { v[n++] = e; continue; }
    // full: keep the LIST_MAX entries that sort first (folders, then newest), so a fresh upload always shows
    truncated = 1;
    int worst = 0;
    for (int i = 1; i < n; i++)
      if (entry_cmp(&v[i], &v[worst]) > 0) worst = i;
    if (entry_cmp(&e, &v[worst]) < 0) v[worst] = e;
  }
  closedir(d);
  if (truncated) fprintf(stderr, "gksdcp: listing of %s cut at %d entries\n", cdir, LIST_MAX);
  qsort(v, (size_t)n, sizeof *v, entry_cmp);

  struct statvfs sv;
  long long total = 0, used = 0;
  if (statvfs(files_root(st), &sv) == 0) {
    total = (long long)sv.f_blocks * (long long)sv.f_frsize;
    used = total - (long long)sv.f_bfree * (long long)sv.f_frsize;
  }
  size_t cap = 64 + (size_t)n * (cl + ENTRY_NAME + 160);
  char *buf = malloc(cap);
  if (!buf) { free(v); list_reply(r, 1); return; }
  struct jw w;
  jw_init(&w, buf, cap);
  jw_raw(&w, ",\"FileList\":[");
  for (int i = 0; i < n; i++) {
    char full[CLIENT_MAX + ENTRY_NAME];
    snprintf(full, sizeof full, "%s%s", cdir, v[i].name);
    if (i) jw_raw(&w, ",");
    jw_raw(&w, "{\"name\":"); jw_str(&w, full);
    jw_raw(&w, ",\"usedSize\":"); jw_int(&w, used);
    jw_raw(&w, ",\"totalSize\":"); jw_int(&w, total);
    jw_raw(&w, ",\"storageType\":"); jw_int(&w, st);
    jw_raw(&w, ",\"type\":"); jw_int(&w, v[i].type);
    jw_raw(&w, "}");
  }
  jw_raw(&w, "]");
  if (w.err) list_reply(r, 1);
  else sdcp_reply(r->conn, r->cmd, r->rid, 0, buf);
  free(buf);
  free(v);
}

// ---- Cmd 259 ----

// Removes the folder name (a path, or relative to parent) and everything in it, never following symlinks.
// dry: only checks that the depth limit holds, deletes nothing. 0 ok / -1.
static int rm_tree(int parent, const char *name, int depth, int dry) {
  int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  DIR *d = fdopendir(fd);
  if (!d) { close(fd); return -1; }
  int rc = 0;
  struct dirent *de;
  while (rc == 0 && (de = readdir(d)) != NULL) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    struct stat sb;
    if (fstatat(dirfd(d), de->d_name, &sb, AT_SYMLINK_NOFOLLOW) < 0) { rc = -1; break; }
    if (S_ISDIR(sb.st_mode)) {
      if (depth + 1 > MAX_DEPTH) rc = -1;
      else rc = rm_tree(dirfd(d), de->d_name, depth + 1, dry);
    } else if (!dry && unlinkat(dirfd(d), de->d_name, 0) < 0) {
      rc = -1;
    }
  }
  closedir(d);
  if (rc == 0 && !dry && unlinkat(parent, name, AT_REMOVEDIR) < 0) rc = -1;
  return rc;
}

// 0 deleted / -1 not deleted. print_file: the file gkhook reports as current, or "".
static int delete_item(const char *client, int want_dir, const char *print_file) {
  char abs[1100], parent[1100];
  int st;
  if (files_resolve(client, abs, sizeof abs, &st) < 0) return -1;
  if (strcmp(abs, files_root(st)) == 0) return -1;                  // never a storage root
  snprintf(parent, sizeof parent, "%s", abs);
  *strrchr(parent, '/') = 0;
  if (!confined(parent, st)) return -1;                             // a symlinked folder leads out of storage
  struct stat sb;
  if (lstat(abs, &sb) < 0) return -1;
  if (want_dir ? !S_ISDIR(sb.st_mode) : !S_ISREG(sb.st_mode)) return -1;
  if (print_file[0]) {
    size_t n = strlen(abs);
    if (want_dir ? (strncmp(print_file, abs, n) == 0 && print_file[n] == '/') : strcmp(print_file, abs) == 0)
      return -1;
  }
  if (!want_dir) return unlink(abs);
  if (rm_tree(AT_FDCWD, abs, 0, 1) < 0) return -1;                  // depth check first: nothing half-deleted
  return rm_tree(AT_FDCWD, abs, 0, 0);
}

// NULL when files can be deleted now, else the notice.
static const char *delete_refusal(void) {
  struct hk_status h;
  hookw_status(&h);
  if (!h.hook_ok) return "printer state unknown";       // the touchscreen can print any file
  int ok = !strcmp(h.state, "idle") || !strcmp(h.state, "complete") || !strcmp(h.state, "error");
  return ok && !upload_busy() && !g_bridge.usb_copying && !cmd_print_start_pending() ? NULL : "printer busy";
}

// token of array key in Data.Data: -1 absent, -2 present but not an array.
static int array_tok(const struct sdcp_req *r, const char *key) {
  int t = jget(r->d, r->data, key);
  if (t < 0) return -1;
  return jarr_n(r->d, t) < 0 ? -2 : t;
}

static void cmd_delete(struct sdcp_req *r) {
  const char *why = delete_refusal();
  if (why) {
    sdcp_reply(r->conn, r->cmd, r->rid, 1, "");
    sdcp_notice(why);
    return;
  }
  int tok[2] = { -1, -1 };
  if (r->data >= 0) {
    tok[0] = array_tok(r, "FileList");
    tok[1] = array_tok(r, "FolderList");
  }
  if ((tok[0] == -1 && tok[1] == -1) || tok[0] == -2 || tok[1] == -2) {
    sdcp_reply(r->conn, r->cmd, r->rid, 1, "");
    return;
  }
  struct hk_status h;
  hookw_status(&h);
  const char *print_file = h.file;

  int total = (tok[0] >= 0 ? jarr_n(r->d, tok[0]) : 0) + (tok[1] >= 0 ? jarr_n(r->d, tok[1]) : 0);
  char **fails = calloc((size_t)total + 1, sizeof *fails);
  if (!fails) { sdcp_reply(r->conn, r->cmd, r->rid, 1, ""); return; }
  int nf = 0;
  for (int k = 0; k < 2; k++) {
    int cnt = tok[k] >= 0 ? jarr_n(r->d, tok[k]) : 0;
    for (int i = 0, e = tok[k] + 1; i < cnt; i++, e = jnext(r->d, e)) {
      char client[CLIENT_MAX];
      if (jstr(r->d, e, client, sizeof client) < 0) client[0] = 0;   // not a usable string
      if (!client[0] || delete_item(client, k == 1, print_file) < 0) fails[nf++] = strdup(client);
    }
  }
  size_t cap = 64;
  for (int i = 0; i < nf; i++) cap += (fails[i] ? strlen(fails[i]) * 6 : 0) + 4;
  char *buf = malloc(cap);
  if (buf) {
    struct jw w;
    jw_init(&w, buf, cap);
    if (nf) {
      jw_raw(&w, ",\"ErrData\":[");
      for (int i = 0; i < nf; i++) {
        if (i) jw_raw(&w, ",");
        jw_str(&w, fails[i] ? fails[i] : "");
      }
      jw_raw(&w, "]");
    }
    sdcp_reply(r->conn, r->cmd, r->rid, 0, w.err ? "" : buf);
  } else {
    sdcp_reply(r->conn, r->cmd, r->rid, 0, "");
  }
  for (int i = 0; i < nf; i++) free(fails[i]);
  free(fails);
  free(buf);
}

// ---- Cmd 192 ----

static void cmd_rename(struct sdcp_req *r) {
  char name[256];
  int t = r->data >= 0 ? jget(r->d, r->data, "Name") : -1;
  if (t < 0 || jstr(r->d, t, name, sizeof name) < 0 || ident_set_name(name) < 0) {
    sdcp_reply(r->conn, r->cmd, r->rid, 1, "");
    return;
  }
  sdcp_reply(r->conn, r->cmd, r->rid, 0, "");
  sdcp_push_attributes(0);
}

void cmd_files_init(void) {
  sdcp_register(192, cmd_rename);
  sdcp_register(258, cmd_list);
  sdcp_register(259, cmd_delete);
}
