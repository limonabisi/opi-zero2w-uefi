/*
 * Boot entries of an ISO: parse GRUB's grub.cfg (menuentry / linux / initrd,
 * set / source / configfile, simple variables), falling back to isolinux /
 * syslinux configs. No script evaluation beyond that.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "x64e.h"

#define MAXVARS 32

typedef struct {
  char name[32];
  char value[256];
} var_t;

typedef struct {
  iso_t     *iso;
  bootlist_t *bl;
  var_t      vars[MAXVARS];
  int        nvars;
  int        depth;
} gctx_t;

static const char *
var_get (gctx_t *g, const char *name, size_t len)
{
  int i;

  for (i = 0; i < g->nvars; i++) {
    if (strlen (g->vars[i].name) == len && memcmp (g->vars[i].name, name, len) == 0) {
      return g->vars[i].value;
    }
  }

  if (len == 6 && memcmp (name, "prefix", 6) == 0) {
    return "/boot/grub";
  }

  if (len == 4 && memcmp (name, "root", 4) == 0) {
    return "";
  }

  return "";
}

static void
var_set (gctx_t *g, const char *name, const char *value)
{
  int i;

  for (i = 0; i < g->nvars; i++) {
    if (strcmp (g->vars[i].name, name) == 0) {
      break;
    }
  }

  if (i == g->nvars) {
    if (g->nvars == MAXVARS) {
      return;
    }

    g->nvars++;
  }

  strncpy (g->vars[i].name, name, sizeof (g->vars[i].name) - 1);
  strncpy (g->vars[i].value, value, sizeof (g->vars[i].value) - 1);
}

/* expand $var / ${var}, strip quotes; out must be large */
static void
expand (gctx_t *g, const char *in, char *out, size_t outsz)
{
  size_t o = 0;

  while (*in && o + 1 < outsz) {
    if (*in == '$') {
      const char *s;
      size_t      l;
      const char *v;

      in++;
      if (*in == '{') {
        s = ++in;
        while (*in && *in != '}') {
          in++;
        }

        l = in - s;
        if (*in) {
          in++;
        }
      } else {
        s = in;
        while ((*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z') ||
               (*in >= '0' && *in <= '9') || *in == '_') {
          in++;
        }

        l = in - s;
      }

      v = var_get (g, s, l);
      while (*v && o + 1 < outsz) {
        out[o++] = *v++;
      }

      continue;
    }

    if (*in == '"' || *in == '\'') {
      in++;
      continue;
    }

    out[o++] = *in++;
  }

  out[o] = 0;
}

static char *
skip_ws (char *p)
{
  while (*p == ' ' || *p == '\t') {
    p++;
  }

  return p;
}

static char *
word (char **pp)
{
  char *p = skip_ws (*pp), *s;

  if (*p == '"' || *p == '\'') {
    char q = *p++;

    s = p;
    while (*p && *p != q) {
      p++;
    }
  } else {
    s = p;
    while (*p && *p != ' ' && *p != '\t') {
      p++;
    }
  }

  if (*p) {
    *p++ = 0;
  }

  *pp = p;
  return s;
}

static void grub_parse_file (gctx_t *g, const char *path);

static void
grub_parse (gctx_t *g, char *text)
{
  char       *line, *next;
  bootent_t  *cur = NULL;
  int         brace = 0, entry_brace = 0;

  for (line = text; line && *line; line = next) {
    char  buf[1024], *p, *cmd;

    next = strchr (line, '\n');
    if (next) {
      *next++ = 0;
    }

    p = skip_ws (line);
    if (*p == '#' || *p == 0) {
      continue;
    }

    strncpy (buf, p, sizeof (buf) - 1);
    buf[sizeof (buf) - 1] = 0;
    p   = buf;
    cmd = word (&p);

    if (strcmp (cmd, "menuentry") == 0) {
      if (g->bl->count < MAX_BOOT_ENTRIES) {
        cur = &g->bl->e[g->bl->count];
        memset (cur, 0, sizeof (*cur));
        strncpy (cur->title, word (&p), sizeof (cur->title) - 1);
      }

      if (strchr (p, '{')) {
        brace++;
        entry_brace = brace;
      }

      continue;
    }

    if (strcmp (cmd, "submenu") == 0 || strcmp (cmd, "if") == 0 ||
        strcmp (cmd, "function") == 0) {
      if (strchr (p, '{')) {
        brace++;
      }

      continue;
    }

    if (cmd[0] == '}') {
      if (cur && brace == entry_brace) {
        if (cur->kernel[0]) {
          g->bl->count++;
        }

        cur         = NULL;
        entry_brace = 0;
      }

      if (brace > 0) {
        brace--;
      }

      continue;
    }

    if (strcmp (cmd, "set") == 0) {
      char *eq = strchr (p, '=');
      char  val[256];

      if (eq) {
        *eq = 0;
        expand (g, eq + 1, val, sizeof (val));
        var_set (g, skip_ws (p), val);
      }

      continue;
    }

    if ((strcmp (cmd, "source") == 0 || strcmp (cmd, "configfile") == 0) && g->depth < 4) {
      char path[256];

      expand (g, word (&p), path, sizeof (path));
      grub_parse_file (g, path);
      continue;
    }

    if (!cur) {
      continue;
    }

    if (strcmp (cmd, "linux") == 0 || strcmp (cmd, "linuxefi") == 0 ||
        strcmp (cmd, "linux16") == 0) {
      expand (g, word (&p), cur->kernel, sizeof (cur->kernel));
      expand (g, skip_ws (p), cur->args, sizeof (cur->args));
    } else if (strncmp (cmd, "initrd", 6) == 0) {
      while (*(p = skip_ws (p)) && cur->ninitrd < 4) {
        expand (g, word (&p), cur->initrd[cur->ninitrd], sizeof (cur->initrd[0]));
        cur->ninitrd++;
      }
    }
  }
}

static void
grub_parse_file (gctx_t *g, const char *path)
{
  size_t   sz;
  uint8_t *t = iso_read_file (g->iso, path, &sz);

  if (!t) {
    return;
  }

  g->depth++;
  grub_parse (g, (char *)t);
  g->depth--;
  host_free (t, sz);
}

/* ------------------------------------------------------------ isolinux */
static void sys_parse_file (iso_t *iso, bootlist_t *bl, const char *dir, const char *name, int depth);

static void
sys_parse (iso_t *iso, bootlist_t *bl, const char *dir, char *text, int depth)
{
  char      *line, *next;
  bootent_t *cur = NULL;

  for (line = text; line && *line; line = next) {
    char  buf[1024], *p, *cmd;
    int   i;

    next = strchr (line, '\n');
    if (next) {
      *next++ = 0;
    }

    strncpy (buf, skip_ws (line), sizeof (buf) - 1);
    buf[sizeof (buf) - 1] = 0;
    for (i = 0; buf[i]; i++) {
      if (buf[i] == '\r') {
        buf[i] = 0;
      }
    }

    p   = buf;
    cmd = word (&p);
    for (i = 0; cmd[i]; i++) {
      cmd[i] = (cmd[i] >= 'A' && cmd[i] <= 'Z') ? cmd[i] + 32 : cmd[i];
    }

    if (strcmp (cmd, "label") == 0) {
      if (cur && cur->kernel[0]) {
        bl->count++;
      }

      cur = NULL;
      if (bl->count < MAX_BOOT_ENTRIES) {
        cur = &bl->e[bl->count];
        memset (cur, 0, sizeof (*cur));
        strncpy (cur->title, skip_ws (p), sizeof (cur->title) - 1);
      }
    } else if ((strcmp (cmd, "include") == 0) && depth < 4) {
      sys_parse_file (iso, bl, dir, word (&p), depth + 1);
    } else if (cur && strcmp (cmd, "menu") == 0) {
      if (strcmp (word (&p), "label") == 0) {
        strncpy (cur->title, skip_ws (p), sizeof (cur->title) - 1);
      }
    } else if (cur && (strcmp (cmd, "kernel") == 0 || strcmp (cmd, "linux") == 0)) {
      char *k = word (&p);

      if (k[0] == '/') {
        strncpy (cur->kernel, k, sizeof (cur->kernel) - 1);
      } else {
        snprintf (cur->kernel, sizeof (cur->kernel), "%s/%s", dir, k);
      }
    } else if (cur && strcmp (cmd, "initrd") == 0) {
      char *r = word (&p);

      snprintf (cur->initrd[0], sizeof (cur->initrd[0]), "%s%s%s", r[0] == '/' ? "" : dir,
                r[0] == '/' ? "" : "/", r);
      cur->ninitrd = 1;
    } else if (cur && strcmp (cmd, "append") == 0) {
      char *a = skip_ws (p), out[512];
      int   o = 0;

      /* move initrd= into the entry, keep the rest as kernel args */
      while (*a) {
        char *w = word (&a);

        if (strncmp (w, "initrd=", 7) == 0) {
          char *r = w + 7, *c;

          while (r && *r && cur->ninitrd < 4) {
            c = strchr (r, ',');
            if (c) {
              *c = 0;
            }

            snprintf (cur->initrd[cur->ninitrd], sizeof (cur->initrd[0]), "%s%s%s",
                      r[0] == '/' ? "" : dir, r[0] == '/' ? "" : "/", r);
            cur->ninitrd++;
            r = c ? c + 1 : NULL;
          }
        } else {
          o += snprintf (out + o, sizeof (out) - o, "%s%s", o ? " " : "", w);
          if (o >= (int)sizeof (out)) {
            o = sizeof (out) - 1;
          }
        }
      }

      out[o] = 0;
      strncpy (cur->args, out, sizeof (cur->args) - 1);
    }
  }

  if (cur && cur->kernel[0]) {
    bl->count++;
  }
}

static void
sys_parse_file (iso_t *iso, bootlist_t *bl, const char *dir, const char *name, int depth)
{
  char     path[256];
  size_t   sz;
  uint8_t *t;

  snprintf (path, sizeof (path), "%s%s%s", name[0] == '/' ? "" : dir, name[0] == '/' ? "" : "/", name);
  t = iso_read_file (iso, path, &sz);
  if (!t) {
    return;
  }

  sys_parse (iso, bl, dir, (char *)t, depth);
  host_free (t, sz);
}

int
bootcfg_scan (iso_t *iso, bootlist_t *bl)
{
  static const char *grub_cfgs[] = { "/boot/grub/grub.cfg", "/boot/grub/loopback.cfg",
                                     "/EFI/BOOT/grub.cfg", "/boot/grub2/grub.cfg" };
  static const char *sys_dirs[]  = { "/isolinux", "/syslinux", "/boot/isolinux", "/boot/syslinux", "" };
  static const char *sys_names[] = { "isolinux.cfg", "syslinux.cfg" };
  gctx_t g;
  size_t i, j;

  memset (bl, 0, sizeof (*bl));
  for (i = 0; i < sizeof (grub_cfgs) / sizeof (grub_cfgs[0]) && bl->count == 0; i++) {
    memset (&g, 0, sizeof (g));
    g.iso = iso;
    g.bl  = bl;
    grub_parse_file (&g, grub_cfgs[i]);
  }

  for (i = 0; i < sizeof (sys_dirs) / sizeof (sys_dirs[0]) && bl->count == 0; i++) {
    for (j = 0; j < 2 && bl->count == 0; j++) {
      sys_parse_file (iso, bl, sys_dirs[i], sys_names[j], 0);
    }
  }

  return bl->count;
}

/* load an entry's kernel + initrds from the ISO and set up the Linux boot */
int
iso_boot (machine_t *m, iso_t *iso, bootent_t *e, const char *extra_args)
{
  uint8_t *k, *parts[4] = { 0 }, *initrd = NULL;
  size_t   ks, psz[4] = { 0 }, total = 0, off = 0;
  char     cmdline[1024];
  int      i, r;

  k = iso_read_file (iso, e->kernel, &ks);
  if (!k) {
    host_log ("x64e: kernel %s not found on the ISO\n", e->kernel);
    return -1;
  }

  for (i = 0; i < e->ninitrd; i++) {
    parts[i] = iso_read_file (iso, e->initrd[i], &psz[i]);
    if (!parts[i]) {
      host_log ("x64e: initrd %s not found on the ISO\n", e->initrd[i]);
      return -1;
    }

    total += (psz[i] + 3) & ~(size_t)3;
  }

  if (total) {
    initrd = host_alloc (total);
    for (i = 0; i < e->ninitrd; i++) {
      memcpy (initrd + off, parts[i], psz[i]);
      off += (psz[i] + 3) & ~(size_t)3;
      host_free (parts[i], psz[i] + 2049);
    }
  }

  snprintf (cmdline, sizeof (cmdline), "%s %s", e->args, extra_args ? extra_args : "");
  host_log ("x64e: booting \"%s\": %s (%u KB) initrd %u KB\nx64e: cmdline: %s\n", e->title,
            e->kernel, (unsigned)(ks / 1024), (unsigned)(total / 1024), cmdline);
  r = linux_boot_setup (m, k, ks, initrd, total, cmdline);
  host_free (k, ks + 2049);
  if (initrd) {
    host_free (initrd, total);
  }

  return r;
}
