/*
 * Minimal read-only ext2/3/4 reader: enough to find /boot/grub/grub.cfg,
 * the kernel and the initrd of an installed Linux system on the x86
 * machine's hard disk. Extents and classic block maps, linear directory
 * scan (works for hashed directories too), symlinks.
 */
#include <string.h>
#include "x64e.h"

#define EXT4_MAGIC      0xEF53
#define INCOMPAT_64BIT  0x80
#define EXTENTS_FL      0x80000
#define INLINE_DATA_FL  0x10000000
#define S_IFMT          0xF000
#define S_IFDIR         0x4000
#define S_IFLNK         0xA000
#define MAX_BLK         65536

typedef struct {
  uint16_t mode;
  uint64_t size;
  uint32_t flags;
  uint8_t  block[60];
} inode_t;

static uint16_t rd16 (const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32 (const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int
dread (iso_t *fs, uint64_t off, void *buf, uint32_t len)
{
  return host_disk_read (fs->disk, fs->base + off, buf, len);
}

static uint8_t *
blkbuf (iso_t *fs)
{
  static uint8_t *b;

  if (!b) {
    b = host_alloc (MAX_BLK);
  }

  return b;
}

int
ext4_probe (iso_t *fs, void *disk, uint64_t base)
{
  uint8_t  sb[1024];
  uint32_t log_bs, incompat;

  memset (fs, 0, sizeof (*fs));
  fs->disk = disk;
  fs->base = base;
  if (dread (fs, 1024, sb, sizeof (sb)) || rd16 (sb + 0x38) != EXT4_MAGIC) {
    return -1;
  }

  log_bs = rd32 (sb + 0x18);
  if (log_bs > 6) {
    return -1;
  }

  fs->type             = FS_EXT4;
  fs->blksz            = 1024u << log_bs;
  fs->first_data_block = rd32 (sb + 0x14);
  fs->inodes_per_group = rd32 (sb + 0x28);
  fs->inode_size       = rd32 (sb + 0x4c) >= 1 ? rd16 (sb + 0x58) : 128;
  incompat             = rd32 (sb + 0x60);
  fs->desc_size        = (incompat & INCOMPAT_64BIT) && rd16 (sb + 0xfe) >= 64 ? rd16 (sb + 0xfe) : 32;
  if (fs->inodes_per_group == 0 || fs->inode_size < 128 || fs->inode_size > 1024) {
    return -1;
  }

  return 0;
}

static int
read_inode (iso_t *fs, uint32_t ino, inode_t *in)
{
  uint8_t  d[64], raw[256];
  uint32_t group = (ino - 1) / fs->inodes_per_group;
  uint32_t index = (ino - 1) % fs->inodes_per_group;
  uint64_t table;

  if (ino == 0 ||
      dread (fs, (uint64_t)(fs->first_data_block + 1) * fs->blksz + (uint64_t)group * fs->desc_size,
             d, fs->desc_size > 64 ? 64 : fs->desc_size)) {
    return -1;
  }

  table = rd32 (d + 8);
  if (fs->desc_size >= 64) {
    table |= (uint64_t)rd32 (d + 0x28) << 32;
  }

  if (dread (fs, table * fs->blksz + (uint64_t)index * fs->inode_size, raw, 160)) {
    return -1;
  }

  in->mode  = rd16 (raw);
  in->size  = rd32 (raw + 4) | ((uint64_t)rd32 (raw + 0x6c) << 32);
  in->flags = rd32 (raw + 0x20);
  memcpy (in->block, raw + 0x28, 60);
  return 0;
}

/* logical block -> physical block, and how many follow contiguously (0 = hole) */
static uint64_t
bmap (iso_t *fs, inode_t *in, uint32_t lb, uint32_t *run)
{
  uint8_t *b = blkbuf (fs);

  *run = 1;
  if (in->flags & EXTENTS_FL) {
    const uint8_t *node = in->block;
    int            level;

    for (level = 0; level < 8; level++) {
      uint16_t entries = rd16 (node + 2), depth = rd16 (node + 6), i;

      if (rd16 (node) != 0xF30A) {
        return 0;
      }

      if (depth == 0) {
        for (i = 0; i < entries; i++) {
          const uint8_t *e     = node + 12 + i * 12;
          uint32_t       first = rd32 (e);
          uint32_t       len   = rd16 (e + 4);
          uint64_t       start = ((uint64_t)rd16 (e + 6) << 32) | rd32 (e + 8);

          if (len > 32768) {
            len -= 32768;                     /* uninitialized: reads as zeros */
            if (lb >= first && lb < first + len) {
              *run = first + len - lb;
              return 0;
            }

            continue;
          }

          if (lb >= first && lb < first + len) {
            *run = first + len - lb;
            return start + (lb - first);
          }
        }

        return 0;
      }

      /* index node: last entry whose first block <= lb */
      {
        int      pick = -1;
        uint64_t leaf;

        for (i = 0; i < entries; i++) {
          if (rd32 (node + 12 + i * 12) <= lb) {
            pick = i;
          }
        }

        if (pick < 0) {
          return 0;
        }

        leaf = ((uint64_t)rd16 (node + 12 + pick * 12 + 8) << 32) | rd32 (node + 12 + pick * 12 + 4);
        if (dread (fs, leaf * fs->blksz, b, fs->blksz)) {
          return 0;
        }

        node = b;
      }
    }

    return 0;
  }

  /* classic block map */
  {
    uint32_t per = fs->blksz / 4;
    uint32_t ptr;

    if (lb < 12) {
      return rd32 (in->block + lb * 4);
    }

    lb -= 12;
    if (lb < per) {
      ptr = rd32 (in->block + 48);
      if (!ptr || dread (fs, (uint64_t)ptr * fs->blksz, b, fs->blksz)) {
        return 0;
      }

      return rd32 (b + lb * 4);
    }

    lb -= per;
    if (lb < per * per) {
      ptr = rd32 (in->block + 52);
      if (!ptr || dread (fs, (uint64_t)ptr * fs->blksz, b, fs->blksz)) {
        return 0;
      }

      ptr = rd32 (b + (lb / per) * 4);
      if (!ptr || dread (fs, (uint64_t)ptr * fs->blksz, b, fs->blksz)) {
        return 0;
      }

      return rd32 (b + (lb % per) * 4);
    }

    return 0;
  }
}

static int
read_data (iso_t *fs, inode_t *in, uint64_t off, uint8_t *dst, uint64_t len)
{
  if ((in->mode & S_IFMT) == S_IFLNK && in->size < 60) {
    memcpy (dst, in->block + off, len);       /* fast symlink */
    return 0;
  }

  if (in->flags & INLINE_DATA_FL) {
    if (off + len > 60) {
      return -1;
    }

    memcpy (dst, in->block + off, len);
    return 0;
  }

  while (len > 0) {
    uint32_t lb   = (uint32_t)(off / fs->blksz);
    uint32_t skip = (uint32_t)(off % fs->blksz), run;
    uint64_t pb   = bmap (fs, in, lb, &run);
    uint64_t n    = (uint64_t)run * fs->blksz - skip;

    if (n > len) {
      n = len;
    }

    if (n > 0x400000) {
      n = 0x400000;
    }

    if (pb == 0) {
      memset (dst, 0, n);
    } else if (dread (fs, pb * fs->blksz + skip, dst, (uint32_t)n)) {
      return -1;
    }

    dst += n;
    off += n;
    len -= n;
  }

  return 0;
}

/* find a name in a directory; returns the inode number or 0 */
static uint32_t
dir_find (iso_t *fs, inode_t *dir, const char *name, size_t nl)
{
  uint8_t *b = host_alloc (fs->blksz);
  uint64_t off;
  uint32_t found = 0;

  if (!b) {
    return 0;
  }

  for (off = 0; off < dir->size && !found; off += fs->blksz) {
    uint32_t p = 0;

    if (read_data (fs, dir, off, b, fs->blksz)) {
      break;
    }

    while (p + 8 <= fs->blksz) {
      uint32_t ino = rd32 (b + p);
      uint16_t rl  = rd16 (b + p + 4);
      uint8_t  l   = b[p + 6];

      if (rl < 8 || p + rl > fs->blksz) {
        break;
      }

      if (ino && l == nl && memcmp (b + p + 8, name, nl) == 0) {
        found = ino;
        break;
      }

      p += rl;
    }
  }

  host_free (b, fs->blksz);
  return found;
}

static uint32_t
resolve (iso_t *fs, const char *path, int depth, inode_t *out)
{
  uint32_t ino = 2;                           /* root */
  inode_t  in;
  char     parent[512] = "";

  if (depth > 8 || read_inode (fs, ino, &in)) {
    return 0;
  }

  while (*path == '/') {
    path++;
  }

  while (*path) {
    const char *e = strchr (path, '/');
    size_t      l = e ? (size_t)(e - path) : strlen (path);
    size_t      pl;

    if ((in.mode & S_IFMT) != S_IFDIR || (ino = dir_find (fs, &in, path, l)) == 0 ||
        read_inode (fs, ino, &in)) {
      return 0;
    }

    if ((in.mode & S_IFMT) == S_IFLNK && in.size < 400) {
      char target[1024];

      if (read_data (fs, &in, 0, (uint8_t *)target, in.size)) {
        return 0;
      }

      target[in.size] = 0;
      if (target[0] != '/') {                 /* relative to the parent directory */
        char rel[1024];

        snprintf (rel, sizeof (rel), "%s/%s", parent, target);
        strncpy (target, rel, sizeof (target) - 1);
        target[sizeof (target) - 1] = 0;
      }

      if (e) {
        size_t tl = strlen (target), el = strlen (e);

        if (tl + el >= sizeof (target)) {
          return 0;
        }

        memcpy (target + tl, e, el + 1);
      }
      return resolve (fs, target, depth + 1, out);
    }

    pl = strlen (parent);
    if (pl + l + 2 < sizeof (parent)) {
      parent[pl] = '/';
      memcpy (parent + pl + 1, path, l);
      parent[pl + 1 + l] = 0;
    }

    path += l;
    while (*path == '/') {
      path++;
    }
  }

  *out = in;
  return ino;
}

int
ext4_lookup (iso_t *fs, const char *path, uint32_t *ino, uint64_t *size, int *is_dir)
{
  inode_t in;

  *ino = resolve (fs, path, 0, &in);
  if (*ino == 0) {
    return -1;
  }

  *size   = in.size;
  *is_dir = (in.mode & S_IFMT) == S_IFDIR;
  return 0;
}

int
ext4_read (iso_t *fs, uint32_t ino, uint64_t off, void *dst, uint64_t len)
{
  inode_t in;

  if (read_inode (fs, ino, &in) || off + len > in.size) {
    return -1;
  }

  return read_data (fs, &in, off, dst, len);
}
