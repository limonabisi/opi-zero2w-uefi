/*
 * Minimal ISO9660 reader with Rock Ridge names: look up a path, read a file.
 * Reads go through host_disk_read(), so it works on a raw ISO / USB stick.
 */
#include <string.h>
#include <stdlib.h>
#include "x64e.h"

#define SECTOR 2048

static int
lower (int c)
{
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* compare a path component with a directory record name */
static int
name_eq (const char *want, size_t wl, const char *have, size_t hl)
{
  size_t i;

  /* strip ";1" version and a trailing '.' of plain ISO9660 names */
  for (i = 0; i < hl; i++) {
    if (have[i] == ';') {
      hl = i;
      break;
    }
  }

  if (hl > 0 && have[hl - 1] == '.') {
    hl--;
  }

  if (wl != hl) {
    return 0;
  }

  for (i = 0; i < wl; i++) {
    if (lower (want[i]) != lower (have[i])) {
      return 0;
    }
  }

  return 1;
}

/* Rock Ridge "NM" name inside the system use area, if any */
static int
rr_name (const uint8_t *rec, int len, char *out, int outsz)
{
  int name_len = rec[32];
  int p        = 33 + name_len + ((name_len & 1) ? 0 : 1);
  int n        = 0;

  while (p + 4 <= len) {
    int l = rec[p + 2];

    if (l < 4 || p + l > len) {
      break;
    }

    if (rec[p] == 'N' && rec[p + 1] == 'M') {
      int c = l - 5;

      if (n + c >= outsz) {
        c = outsz - 1 - n;
      }

      memcpy (out + n, rec + p + 5, c);
      n += c;
    }

    p += l;
  }

  out[n] = 0;
  return n;
}

int
iso_open (iso_t *iso, void *disk)
{
  uint8_t pvd[SECTOR];

  iso->disk = disk;
  if (host_disk_read (disk, 16 * SECTOR, pvd, SECTOR) || pvd[0] != 1 ||
      memcmp (pvd + 1, "CD001", 5) != 0) {
    return -1;
  }

  iso->root_lba  = pvd[156 + 2] | (pvd[156 + 3] << 8) | (pvd[156 + 4] << 16) | ((uint32_t)pvd[156 + 5] << 24);
  iso->root_size = pvd[156 + 10] | (pvd[156 + 11] << 8) | (pvd[156 + 12] << 16) | ((uint32_t)pvd[156 + 13] << 24);
  return 0;
}

/* find one name in a directory extent */
static int
dir_find (iso_t *iso, uint32_t lba, uint32_t size, const char *name, size_t nl,
          uint32_t *out_lba, uint32_t *out_size, int *is_dir)
{
  uint8_t  sec[SECTOR];
  uint32_t off;
  char     rr[256];

  for (off = 0; off < size; off += SECTOR) {
    uint32_t p = 0;

    if (host_disk_read (iso->disk, (uint64_t)(lba + off / SECTOR) * SECTOR, sec, SECTOR)) {
      return -1;
    }

    while (p < SECTOR) {
      uint8_t *r   = sec + p;
      int      len = r[0];

      if (len == 0) {
        break;                                  /* rest of sector unused */
      }

      if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) {
        p += len;                               /* . and .. */
        continue;
      }

      if ((rr_name (r, len, rr, sizeof (rr)) > 0 && name_eq (name, nl, rr, strlen (rr))) ||
          name_eq (name, nl, (const char *)r + 33, r[32])) {
        *out_lba  = r[2] | (r[3] << 8) | (r[4] << 16) | ((uint32_t)r[5] << 24);
        *out_size = r[10] | (r[11] << 8) | (r[12] << 16) | ((uint32_t)r[13] << 24);
        *is_dir   = (r[25] & 2) != 0;
        return 0;
      }

      p += len;
    }
  }

  return -1;
}

int
iso_lookup (iso_t *iso, const char *path, uint32_t *lba, uint32_t *size)
{
  uint32_t cur = iso->root_lba, cur_size = iso->root_size;
  int      is_dir = 1;

  while (*path == '/') {
    path++;
  }

  while (*path) {
    const char *e = strchr (path, '/');
    size_t      l = e ? (size_t)(e - path) : strlen (path);

    if (!is_dir || dir_find (iso, cur, cur_size, path, l, &cur, &cur_size, &is_dir)) {
      return -1;
    }

    path += l;
    while (*path == '/') {
      path++;
    }
  }

  *lba  = cur;
  *size = cur_size;
  return is_dir ? 1 : 0;
}

/* read a whole file; returns a host_alloc'ed buffer (NUL terminated) */
uint8_t *
iso_read_file (iso_t *iso, const char *path, size_t *size)
{
  uint32_t lba, sz;
  uint8_t *buf;

  if (iso_lookup (iso, path, &lba, &sz) != 0) {
    return NULL;
  }

  buf = host_alloc (sz + SECTOR + 1);
  if (!buf) {
    return NULL;
  }

  if (host_disk_read (iso->disk, (uint64_t)lba * SECTOR, buf, (sz + SECTOR - 1) & ~(SECTOR - 1))) {
    return NULL;
  }

  buf[sz] = 0;
  *size   = sz;
  return buf;
}

/* read a whole file into dst in 1 MB steps, reporting progress */
int
iso_read_into (iso_t *iso, const char *path, uint8_t *dst, size_t max, size_t *size, const char *what)
{
  uint32_t lba, sz, done = 0;

  if (iso_lookup (iso, path, &lba, &sz) != 0 || sz > max) {
    return -1;
  }

  while (done < sz) {
    uint32_t chunk = sz - done > 0x100000 ? 0x100000 : sz - done;

    if (host_disk_read (iso->disk, (uint64_t)lba * SECTOR + done, dst + done, chunk)) {
      return -1;
    }

    done += chunk;
    host_progress (what, (unsigned)((uint64_t)done * 100 / sz));
  }

  *size = sz;
  return 0;
}
