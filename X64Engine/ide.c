/*
 * PIIX3 IDE controller (00:01.1): two legacy channels (0x1F0 / IRQ 14 and
 * 0x170 / IRQ 15) with bus-master DMA, ATA hard disks and ATAPI CD/DVD-ROM
 * drives. Everything completes at once; the disks are host_disk_*().
 */
#include <string.h>
#include "pc.h"

#define ST_ERR   0x01
#define ST_DRQ   0x08
#define ST_DSC   0x10
#define ST_DF    0x20
#define ST_DRDY  0x40
#define ST_BSY   0x80
#define ER_ABRT  0x04
#define ER_IDNF  0x10

#define BUF_SIZE   0x20000
#define MAX_MULT   16

enum { X_NONE = 0, X_ATA_IN, X_ATA_OUT, X_IDENT, X_PKT_CMD, X_PKT_IN };

typedef struct {
  int       present, cdrom, readonly;
  void     *disk;
  uint64_t  bytes, sectors;           /* 512-byte sectors (disk) / 2048-byte blocks (CD) */
  unsigned  cyls, heads, secs;
  uint8_t   feature, error, status, select;
  uint8_t   nsector, sector, lcyl, hcyl;
  uint8_t   hob_nsector, hob_sector, hob_lcyl, hob_hcyl;
  int       lba48, mult, mdma, udma;
  /* PIO data */
  uint8_t  *buf;
  uint32_t  pos, end;
  int       xfer;
  uint64_t  lba;                      /* ATA: next sector */
  uint32_t  left;                     /* ATA: sectors still to move */
  uint32_t  chunk;                    /* ATA: sectors in the buffer */
  /* ATAPI data-in: from memory (reply) or from the medium */
  uint8_t   reply[2048 + 64];
  int       from_disk;
  uint64_t  src_off;
  uint64_t  src_left;
  uint8_t   sense, asc;
  /* DMA */
  int       dma;                      /* 0 none, 1 read (to memory), 2 write */
} drive_t;

typedef struct {
  drive_t   d[2];
  int       sel;
  uint8_t   control;
  int       irq, irq_level, irq_pending;
  uint8_t   bm_cmd, bm_status;
  uint32_t  bm_prdt;
} chan_t;

typedef struct {
  pci_dev_t pci;
  chan_t    ch[2];
} ide_t;

static void
ide_irq (machine_t *m, chan_t *c, int pending)
{
  int level;

  c->irq_pending = pending;
  if (pending) {
    c->bm_status |= 4;
  }

  level = pending && !(c->control & 2);
  if (level != c->irq_level) {
    c->irq_level = level;
    pic_set_irq (m, c->irq, level);
  }
}

static void
put_str (uint16_t *w, const char *s, int words)
{
  int i;

  for (i = 0; i < words * 2; i++) {
    uint8_t ch = *s ? (uint8_t)*s++ : ' ';

    if (i & 1) {
      w[i / 2] |= ch;
    } else {
      w[i / 2] = (uint16_t)(ch << 8);
    }
  }
}

static void
set_signature (drive_t *d)
{
  d->select &= 0xf0;
  d->nsector = 1;
  d->sector  = 1;
  if (d->cdrom) {
    d->lcyl = 0x14;
    d->hcyl = 0xeb;
  } else if (d->present) {
    d->lcyl = 0;
    d->hcyl = 0;
  } else {
    d->lcyl = 0xff;
    d->hcyl = 0xff;
  }
}

static void
drive_reset (drive_t *d)
{
  d->xfer   = X_NONE;
  d->dma    = 0;
  d->pos    = d->end = 0;
  d->mult   = MAX_MULT;
  d->error  = 1;
  d->status = d->cdrom ? 0 : (ST_DRDY | ST_DSC);
  d->feature = 0;
  d->select  = 0xa0;
  set_signature (d);
}

static void
identify (drive_t *d)
{
  uint16_t *w = (uint16_t *)d->buf;

  memset (w, 0, 512);
  if (d->cdrom) {
    w[0] = 0x85c0;                          /* ATAPI CD-ROM, removable, 12-byte packets */
    put_str (w + 10, "X64E00002", 10);
    put_str (w + 23, "1.0", 4);
    put_str (w + 27, "X64E DVD-ROM", 20);
    w[49] = (1 << 9) | (1 << 8);            /* LBA, DMA */
    w[53] = 7;
    w[62] = 7;
    w[63] = (uint16_t)(7 | (d->mdma << 8));
    w[64] = 3;
    w[65] = 0xb4; w[66] = 0xb4; w[67] = 0x12c; w[68] = 0xb4;
    w[71] = 30; w[72] = 30;
    w[80] = 0x1e;
    w[88] = (uint16_t)(0x3f | (d->udma << 8));
    return;
  }

  w[0]  = 0x0040;
  w[1]  = (uint16_t)d->cyls;
  w[3]  = (uint16_t)d->heads;
  w[6]  = (uint16_t)d->secs;
  put_str (w + 10, "X64E00001", 10);
  put_str (w + 23, "1.0", 4);
  put_str (w + 27, "X64E HARDDISK", 20);
  w[47] = 0x8000 | MAX_MULT;
  w[49] = (1 << 11) | (1 << 9) | (1 << 8);  /* IORDY, LBA, DMA */
  w[51] = 0x200;
  w[53] = 7;
  w[54] = (uint16_t)d->cyls;
  w[55] = (uint16_t)d->heads;
  w[56] = (uint16_t)d->secs;
  {
    uint32_t chs = d->cyls * d->heads * d->secs;
    uint32_t l28 = d->sectors > 0x0fffffff ? 0x0fffffff : (uint32_t)d->sectors;

    w[57] = (uint16_t)chs; w[58] = (uint16_t)(chs >> 16);
    w[60] = (uint16_t)l28; w[61] = (uint16_t)(l28 >> 16);
  }

  w[59]  = (uint16_t)(0x100 | d->mult);
  w[63]  = (uint16_t)(7 | (d->mdma << 8));
  w[64]  = 3;
  w[65]  = 120; w[66] = 120; w[67] = 120; w[68] = 120;
  w[80]  = 0xf0;
  w[81]  = 0x16;
  w[82]  = (1 << 14) | (1 << 5);
  w[83]  = (1 << 14) | (1 << 13) | (1 << 12) | (1 << 10);
  w[84]  = 1 << 14;
  w[85]  = (1 << 14) | (1 << 5);
  w[86]  = (1 << 13) | (1 << 12) | (1 << 10);
  w[87]  = 1 << 14;
  w[88]  = (uint16_t)(0x3f | (d->udma << 8));
  w[93]  = 1 | (1 << 14) | 0x2000;
  w[100] = (uint16_t)d->sectors;
  w[101] = (uint16_t)(d->sectors >> 16);
  w[102] = (uint16_t)(d->sectors >> 32);
  w[103] = (uint16_t)(d->sectors >> 48);
}

static void
ata_abort (machine_t *m, chan_t *c, drive_t *d, uint8_t err)
{
  d->status = ST_DRDY | ST_DSC | ST_ERR;
  d->error  = err;
  d->xfer   = X_NONE;
  d->dma    = 0;
  ide_irq (m, c, 1);
}

static void
ata_done (machine_t *m, chan_t *c, drive_t *d)
{
  d->status = ST_DRDY | ST_DSC;
  d->error  = 0;
  d->xfer   = X_NONE;
  ide_irq (m, c, 1);
}

static uint64_t
get_lba (drive_t *d)
{
  if (d->select & 0x40) {
    if (d->lba48) {
      return ((uint64_t)d->hob_hcyl << 40) | ((uint64_t)d->hob_lcyl << 32) | ((uint64_t)d->hob_sector << 24) |
             ((uint64_t)d->hcyl << 16) | ((uint64_t)d->lcyl << 8) | d->sector;
    }

    return ((uint64_t)(d->select & 0x0f) << 24) | ((uint64_t)d->hcyl << 16) | ((uint64_t)d->lcyl << 8) | d->sector;
  }

  return ((uint64_t)((d->hcyl << 8) | d->lcyl) * d->heads + (d->select & 0x0f)) * d->secs + d->sector - 1;
}

/* leave the registers pointing after the last sector moved */
static void
set_lba (drive_t *d, uint64_t lba)
{
  if (d->select & 0x40) {
    if (d->lba48) {
      d->hob_hcyl = (uint8_t)(lba >> 40); d->hob_lcyl = (uint8_t)(lba >> 32); d->hob_sector = (uint8_t)(lba >> 24);
    } else {
      d->select = (uint8_t)((d->select & 0xf0) | ((lba >> 24) & 0x0f));
    }

    d->hcyl = (uint8_t)(lba >> 16); d->lcyl = (uint8_t)(lba >> 8); d->sector = (uint8_t)lba;
  } else if (d->heads && d->secs) {
    uint32_t cyl = (uint32_t)(lba / (d->heads * d->secs)), r = (uint32_t)(lba % (d->heads * d->secs));

    d->hcyl   = (uint8_t)(cyl >> 8); d->lcyl = (uint8_t)cyl;
    d->select = (uint8_t)((d->select & 0xf0) | ((r / d->secs) & 0x0f));
    d->sector = (uint8_t)(r % d->secs + 1);
  }
}

static uint32_t
get_count (drive_t *d)
{
  if (d->lba48) {
    uint32_t n = ((uint32_t)d->hob_nsector << 8) | d->nsector;

    return n ? n : 65536;
  }

  return d->nsector ? d->nsector : 256;
}

/* PIO read: next block of sectors into the buffer */
static void
ata_read_next (machine_t *m, chan_t *c, drive_t *d, uint32_t per)
{
  uint32_t n = d->left < per ? d->left : per;

  if (d->lba + n > d->sectors || host_disk_read (d->disk, d->lba * 512, d->buf, n * 512)) {
    ata_abort (m, c, d, ER_ABRT | ER_IDNF);
    return;
  }

  d->chunk  = n;
  d->pos    = 0;
  d->end    = n * 512;
  d->xfer   = X_ATA_IN;
  d->status = ST_DRDY | ST_DSC | ST_DRQ;
  d->error  = 0;
  ide_irq (m, c, 1);
}

static void
ata_write_next (drive_t *d, uint32_t per)
{
  uint32_t n = d->left < per ? d->left : per;

  d->chunk  = n;
  d->pos    = 0;
  d->end    = n * 512;
  d->xfer   = X_ATA_OUT;
  d->status = ST_DRDY | ST_DSC | ST_DRQ;
  d->error  = 0;
}

/* ---------------------------------------------------------------- DMA -- */
/* run the whole transfer through the PRD table; returns 0 when complete */
static int
dma_run (machine_t *m, chan_t *c, drive_t *d)
{
  uint32_t prd = c->bm_prdt & ~3u;
  uint64_t off, left;
  int      guard = 0, write = d->dma == 2;

  if (d->cdrom) {
    off  = d->src_off;
    left = d->src_left;
  } else {
    off  = d->lba * 512;
    left = (uint64_t)d->left * 512;
  }

  while (left > 0 && guard++ < 8192) {
    uint8_t *e = machine_ram (m, prd, 8), *p;
    uint32_t addr, len, n;

    if (!e) {
      break;
    }

    memcpy (&addr, e, 4);
    memcpy (&len, e + 4, 4);
    n = len & 0xffff;
    if (n == 0) {
      n = 0x10000;
    }

    if (n > left) {
      n = (uint32_t)left;
    }

    p = machine_ram (m, addr, n);
    if (!p) {
      break;
    }

    if (write) {
      if (d->readonly || host_disk_write (d->disk, off, p, n)) {
        break;
      }
    } else {
      if (d->cdrom && !d->from_disk) {
        memcpy (p, d->reply + off, n);
      } else if (host_disk_read (d->disk, off, p, n)) {
        break;
      }

      uc_x86_invalidate_host (m->uc, p, n);
    }

    off  += n;
    left -= n;
    prd  += 8;
    if (len & 0x80000000u) {
      break;
    }
  }

  c->bm_cmd    &= ~1;
  c->bm_status &= ~1;
  d->dma        = 0;
  if (d->cdrom) {
    /* a short PRD table ends the command too: the host asked for less */
    d->src_left = 0;
    d->nsector  = 3;
    d->status   = ST_DRDY | ST_DSC;
    d->error    = 0;
    d->xfer     = X_NONE;
    ide_irq (m, c, 1);
    return 0;
  }

  if (left > 0) {
    c->bm_status |= 2;
    set_lba (d, off / 512);
    ata_abort (m, c, d, ER_ABRT);
    return -1;
  }

  set_lba (d, off / 512);
  d->left = 0;
  ata_done (m, c, d);
  return 0;
}

static void
dma_start (machine_t *m, chan_t *c, drive_t *d, int dir)
{
  d->dma    = dir;
  d->status = ST_DRDY | ST_DSC | ST_DRQ;
  if (c->bm_cmd & 1) {
    dma_run (m, c, d);
  }
}

/* -------------------------------------------------------------- ATAPI -- */
static void
atapi_ok (machine_t *m, chan_t *c, drive_t *d)
{
  d->error   = 0;
  d->status  = ST_DRDY | ST_DSC;
  d->nsector = 3;                     /* I/O, C/D: command complete */
  d->xfer    = X_NONE;
  d->sense   = 0;
  d->asc     = 0;
  ide_irq (m, c, 1);
}

static void
atapi_error (machine_t *m, chan_t *c, drive_t *d, uint8_t sense, uint8_t asc)
{
  d->error   = (uint8_t)(sense << 4);
  d->status  = ST_DRDY | ST_ERR;
  d->nsector = 3;
  d->xfer    = X_NONE;
  d->dma     = 0;
  d->sense   = sense;
  d->asc     = asc;
  ide_irq (m, c, 1);
}

/* next PIO piece of an ATAPI data-in transfer */
static void
atapi_in_next (machine_t *m, chan_t *c, drive_t *d)
{
  uint32_t limit, n;

  if (d->src_left == 0) {
    atapi_ok (m, c, d);
    return;
  }

  limit = d->lcyl | (d->hcyl << 8);
  if (limit == 0 || limit == 0xffff) {
    limit = 0xfffe;
  }

  n = d->src_left > limit ? limit : (uint32_t)d->src_left;
  if (n > 1 && (n & 1) && d->src_left > n) {
    n--;
  }

  if (d->from_disk) {
    if (host_disk_read (d->disk, d->src_off, d->buf, n)) {
      atapi_error (m, c, d, 3, 0x11);           /* medium error, unrecovered read */
      return;
    }
  } else {
    memcpy (d->buf, d->reply + d->src_off, n);
  }

  d->src_off  += n;
  d->src_left -= n;
  d->pos       = 0;
  d->end       = n;
  d->lcyl      = (uint8_t)n;
  d->hcyl      = (uint8_t)(n >> 8);
  d->nsector   = 2;                   /* I/O: data to the host */
  d->status    = ST_DRDY | ST_DSC | ST_DRQ;
  d->xfer      = X_PKT_IN;
  ide_irq (m, c, 1);
}

static void
atapi_reply (machine_t *m, chan_t *c, drive_t *d, uint32_t len, uint32_t alloc)
{
  d->from_disk = 0;
  d->src_off   = 0;
  d->src_left  = len < alloc ? len : alloc;
  if (d->dma && d->src_left) {
    dma_start (m, c, d, 1);
  } else {
    d->dma = 0;
    atapi_in_next (m, c, d);
  }
}

static void
atapi_read (machine_t *m, chan_t *c, drive_t *d, uint32_t lba, uint32_t blocks)
{
  if (blocks == 0) {
    atapi_ok (m, c, d);
    return;
  }

  if ((uint64_t)lba + blocks > d->sectors) {
    atapi_error (m, c, d, 5, 0x21);             /* LBA out of range */
    return;
  }

  d->from_disk = 1;
  d->src_off   = (uint64_t)lba * 2048;
  d->src_left  = (uint64_t)blocks * 2048;
  if (d->dma) {
    dma_start (m, c, d, 1);
  } else {
    atapi_in_next (m, c, d);
  }
}

static void be16 (uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32 (uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rbe16 (const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t rbe32 (const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void
lba_addr (uint8_t *p, uint32_t lba, int msf)
{
  if (msf) {
    lba += 150;
    p[0] = 0; p[1] = (uint8_t)(lba / 75 / 60); p[2] = (uint8_t)(lba / 75 % 60); p[3] = (uint8_t)(lba % 75);
  } else {
    be32 (p, lba);
  }
}

static int
is_dvd (drive_t *d)
{
  return d->sectors > 99u * 60 * 75;
}

static void
atapi_command (machine_t *m, chan_t *c, drive_t *d)
{
  uint8_t  cdb[12];
  uint8_t *r = d->reply;
  uint32_t n;

  memcpy (cdb, d->buf, 12);
  memset (r, 0, sizeof (d->reply));
  d->dma = (d->feature & 1) ? 1 : 0;
  switch (cdb[0]) {
    case 0x00:                        /* TEST UNIT READY */
    case 0x1b:                        /* START STOP UNIT */
    case 0x1e:                        /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2b:                        /* SEEK */
    case 0x2f:                        /* VERIFY */
    case 0x35:                        /* SYNCHRONIZE CACHE */
    case 0xbb:                        /* SET CD SPEED */
      atapi_ok (m, c, d);
      break;
    case 0x03:                        /* REQUEST SENSE */
      r[0]  = 0x70;
      r[2]  = d->sense;
      r[7]  = 10;
      r[12] = d->asc;
      n     = cdb[4];
      d->sense = 0;
      d->asc   = 0;
      atapi_reply (m, c, d, 18, n);
      break;
    case 0x12:                        /* INQUIRY */
      r[0] = 0x05;
      r[1] = 0x80;
      r[2] = 0x00;
      r[3] = 0x21;
      r[4] = 31;
      memcpy (r + 8, "X64E    ", 8);
      memcpy (r + 16, "DVD-ROM         ", 16);
      memcpy (r + 32, "1.0 ", 4);
      atapi_reply (m, c, d, 36, cdb[4]);
      break;
    case 0x25:                        /* READ CAPACITY */
      be32 (r, (uint32_t)d->sectors - 1);
      be32 (r + 4, 2048);
      atapi_reply (m, c, d, 8, 8);
      break;
    case 0x23:                        /* READ FORMAT CAPACITIES */
      r[3] = 8;
      be32 (r + 4, (uint32_t)d->sectors);
      r[8] = 2;
      r[10] = 0x08;                   /* 2048 */
      atapi_reply (m, c, d, 12, rbe16 (cdb + 7));
      break;
    case 0x28:                        /* READ (10) */
      atapi_read (m, c, d, rbe32 (cdb + 2), rbe16 (cdb + 7));
      break;
    case 0xa8:                        /* READ (12) */
      atapi_read (m, c, d, rbe32 (cdb + 2), rbe32 (cdb + 6));
      break;
    case 0xbe:                        /* READ CD: user data only */
      n = ((uint32_t)cdb[6] << 16) | ((uint32_t)cdb[7] << 8) | cdb[8];
      if ((cdb[9] & 0xf8) == 0x00 || n == 0) {
        atapi_ok (m, c, d);
      } else if ((cdb[9] & 0xf8) == 0x10) {
        atapi_read (m, c, d, rbe32 (cdb + 2), n);
      } else {
        atapi_error (m, c, d, 5, 0x24);
      }

      break;
    case 0x43: {                      /* READ TOC */
      int      msf = (cdb[1] >> 1) & 1, fmt = cdb[2] & 0x0f;
      uint32_t alloc = rbe16 (cdb + 7);

      if (fmt == 0) {
        fmt = cdb[9] >> 6;
      }

      if (fmt == 0) {
        uint8_t *q = r + 4;

        if (cdb[6] > 1 && cdb[6] != 0xaa) {
          atapi_error (m, c, d, 5, 0x24);
          break;
        }

        r[2] = 1; r[3] = 1;
        if (cdb[6] <= 1) {
          q[1] = 0x14; q[2] = 1;
          lba_addr (q + 4, 0, msf);
          q += 8;
        }

        q[1] = 0x16; q[2] = 0xaa;
        lba_addr (q + 4, (uint32_t)d->sectors, msf);
        q += 8;
        be16 (r, (uint32_t)(q - r - 2));
        atapi_reply (m, c, d, (uint32_t)(q - r), alloc);
      } else if (fmt == 1) {
        be16 (r, 0x0a);
        r[2] = 1; r[3] = 1;
        r[5] = 0x14; r[6] = 1;
        atapi_reply (m, c, d, 12, alloc);
      } else if (fmt == 2) {
        /* raw TOC: points A0, A1, A2 and track 1 */
        static const uint8_t pts[4] = { 0xa0, 0xa1, 0xa2, 1 };
        uint8_t *q = r + 4;
        int      i;

        r[2] = 1; r[3] = 1;
        for (i = 0; i < 4; i++, q += 11) {
          q[0] = 1; q[1] = 0x14; q[3] = pts[i];
          if (i < 2) {
            q[8] = 1;
          } else {
            uint8_t a[4];

            lba_addr (a, i == 2 ? (uint32_t)d->sectors : 0, 1);
            q[8] = a[1]; q[9] = a[2]; q[10] = a[3];
          }
        }

        be16 (r, (uint32_t)(q - r - 2));
        atapi_reply (m, c, d, (uint32_t)(q - r), alloc);
      } else {
        atapi_error (m, c, d, 5, 0x24);
      }

      break;
    }
    case 0x46: {                      /* GET CONFIGURATION */
      uint32_t alloc = rbe16 (cdb + 7);
      uint8_t *q = r + 8;

      be16 (r + 6, is_dvd (d) ? 0x10 : 0x08);
      /* profile list */
      be16 (q, 0); q[2] = 0x03; q[3] = 8;
      be16 (q + 4, 0x10); q[6] = is_dvd (d);
      be16 (q + 8, 0x08); q[10] = !is_dvd (d);
      q += 12;
      /* core */
      be16 (q, 1); q[2] = 0x0b; q[3] = 8;
      be32 (q + 4, 2); q[8] = 1;
      q += 12;
      /* removable medium */
      be16 (q, 3); q[2] = 0x03; q[3] = 4;
      q[4] = 0x29;
      q += 8;
      be32 (r, (uint32_t)(q - r - 4));
      atapi_reply (m, c, d, (uint32_t)(q - r), alloc);
      break;
    }
    case 0x4a: {                      /* GET EVENT STATUS NOTIFICATION */
      uint32_t alloc = rbe16 (cdb + 7);

      if (!(cdb[1] & 1)) {
        atapi_error (m, c, d, 5, 0x24);
        break;
      }

      r[3] = 0x10;                    /* media class supported */
      if (cdb[4] & 0x10) {
        be16 (r, 4);
        r[2] = 4;
        r[4] = 0;                     /* no change */
        r[5] = 2;                     /* medium present */
        atapi_reply (m, c, d, 8, alloc);
      } else {
        r[2] = 0x80;
        atapi_reply (m, c, d, 4, alloc);
      }

      break;
    }
    case 0x51:                        /* READ DISC INFORMATION */
      be16 (r, 32);
      r[2] = 0x0e; r[3] = 1; r[4] = 1; r[5] = 1; r[6] = 1; r[7] = 0x20;
      atapi_reply (m, c, d, 34, rbe16 (cdb + 7));
      break;
    case 0x5a: {                      /* MODE SENSE (10) */
      uint32_t alloc = rbe16 (cdb + 7);

      switch (cdb[2] & 0x3f) {
        case 0x01:
          be16 (r, 16 - 2);
          r[8] = 0x01; r[9] = 0x06; r[11] = 0x05;
          atapi_reply (m, c, d, 16, alloc);
          break;
        case 0x0e:
          be16 (r, 24 - 2);
          r[8] = 0x0e; r[9] = 0x0e; r[10] = 0x04;
          r[16] = 1; r[17] = 0xff; r[18] = 2; r[19] = 0xff;
          atapi_reply (m, c, d, 24, alloc);
          break;
        case 0x2a:
          be16 (r, 28 - 2);
          r[8]  = 0x2a; r[9] = 0x12;
          r[10] = 0x3b; r[12] = 0x71; r[13] = 3 << 5;
          r[14] = (1 << 0) | (1 << 3) | (1 << 5);
          be16 (r + 16, 706); be16 (r + 18, 2); be16 (r + 20, 512); be16 (r + 22, 706);
          atapi_reply (m, c, d, 28, alloc);
          break;
        default:
          atapi_error (m, c, d, 5, 0x24);
          break;
      }

      break;
    }
    case 0xad: {                      /* READ DVD STRUCTURE */
      uint32_t alloc = rbe16 (cdb + 8);

      if (!is_dvd (d)) {
        atapi_error (m, c, d, 5, 0x30);         /* incompatible format */
        break;
      }

      switch (cdb[7]) {
        case 0x00:
          be16 (r, 2048 + 2);
          r[4] = 1; r[5] = 0x0f; r[6] = 1;
          be32 (r + 8, 0x30000);
          be32 (r + 12, 0x30000 + (uint32_t)d->sectors - 1);
          atapi_reply (m, c, d, 2048 + 4, alloc);
          break;
        case 0x01:
          be16 (r, 4 + 2);
          atapi_reply (m, c, d, 8, alloc);
          break;
        case 0x04:
          be16 (r, 2048 + 2);
          atapi_reply (m, c, d, 2048 + 4, alloc);
          break;
        default:
          atapi_error (m, c, d, 5, 0x24);
          break;
      }

      break;
    }
    case 0xbd:                        /* MECHANISM STATUS */
      r[5] = 1;
      atapi_reply (m, c, d, 8, rbe16 (cdb + 8));
      break;
    default:
      atapi_error (m, c, d, 5, 0x20);           /* invalid command */
      break;
  }
}

/* ---------------------------------------------------------- commands -- */
static void
ata_command (machine_t *m, chan_t *c, drive_t *d, uint8_t cmd)
{
  uint32_t n;

  ide_irq (m, c, 0);
  if (!d->present) {
    return;
  }

  d->lba48 = 0;
  d->xfer  = X_NONE;
  d->dma   = 0;
  if (d->cdrom) {
    switch (cmd) {
      case 0xa1:                      /* IDENTIFY PACKET DEVICE */
        identify (d);
        d->pos = 0; d->end = 512; d->xfer = X_IDENT;
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        d->error  = 0;
        ide_irq (m, c, 1);
        return;
      case 0xa0:                      /* PACKET */
        d->pos = 0; d->end = 12; d->xfer = X_PKT_CMD;
        d->nsector = 1;               /* C/D: command to the device */
        d->status  = ST_DRDY | ST_DSC | ST_DRQ;
        d->error   = 0;
        return;
      case 0x08:                      /* DEVICE RESET */
        drive_reset (d);
        d->select = (uint8_t)(0xa0 | ((d == &c->d[1]) << 4));
        return;
      case 0xec:                      /* IDENTIFY: not an ATA disk; leave the signature */
        set_signature (d);
        ata_abort (m, c, d, ER_ABRT);
        return;
      case 0xef:
        break;                        /* SET FEATURES, below */
      case 0x00: case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe7:
        ata_done (m, c, d);
        return;
      case 0xe5:
        d->nsector = 0xff;
        ata_done (m, c, d);
        return;
      default:
        ata_abort (m, c, d, ER_ABRT);
        return;
    }
  }

  switch (cmd) {
    case 0xec:                        /* IDENTIFY DEVICE */
      identify (d);
      d->pos = 0; d->end = 512; d->xfer = X_IDENT;
      d->status = ST_DRDY | ST_DSC | ST_DRQ;
      d->error  = 0;
      ide_irq (m, c, 1);
      break;
    case 0xa1:
      ata_abort (m, c, d, ER_ABRT);
      break;
    case 0xef:                        /* SET FEATURES */
      if (d->feature == 0x03) {
        d->mdma = d->udma = 0;
        if ((d->nsector >> 3) == 4) {
          d->mdma = 1 << (d->nsector & 7);
        } else if ((d->nsector >> 3) == 8) {
          d->udma = 1 << (d->nsector & 7);
        }
      }

      ata_done (m, c, d);
      break;
    case 0x91:                        /* INITIALIZE DEVICE PARAMETERS */
    case 0x10:                        /* RECALIBRATE */
    case 0x70:                        /* SEEK */
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe6:    /* power management */
    case 0xe7: case 0xea:             /* FLUSH CACHE (EXT) */
    case 0x40: case 0x41: case 0x42:  /* READ VERIFY */
    case 0xf5:                        /* SECURITY FREEZE LOCK */
      ata_done (m, c, d);
      break;
    case 0xe5:                        /* CHECK POWER MODE: active */
      d->nsector = 0xff;
      ata_done (m, c, d);
      break;
    case 0x90:                        /* EXECUTE DEVICE DIAGNOSTIC */
      drive_reset (&c->d[0]);
      drive_reset (&c->d[1]);
      c->d[1].select |= 0x10;
      c->d[0].status = c->d[0].cdrom ? 0 : (ST_DRDY | ST_DSC);
      ide_irq (m, c, 1);
      break;
    case 0xc6:                        /* SET MULTIPLE MODE */
      if (d->nsector > MAX_MULT || (d->nsector & (d->nsector - 1))) {
        ata_abort (m, c, d, ER_ABRT);
      } else {
        d->mult = d->nsector;
        ata_done (m, c, d);
      }

      break;
    case 0xf8:                        /* READ NATIVE MAX ADDRESS */
      d->select |= 0x40;
      set_lba (d, (d->sectors > 0x10000000 ? 0x10000000 : d->sectors) - 1);
      ata_done (m, c, d);
      break;
    case 0x27:                        /* READ NATIVE MAX ADDRESS EXT */
      d->lba48   = 1;
      d->select |= 0x40;
      set_lba (d, d->sectors - 1);
      ata_done (m, c, d);
      break;
    case 0x24: case 0x29:             /* READ SECTORS / MULTIPLE EXT */
      d->lba48 = 1;
      /* fall through */
    case 0x20: case 0x21: case 0xc4:  /* READ SECTORS, READ MULTIPLE */
      d->lba  = get_lba (d);
      d->left = get_count (d);
      ata_read_next (m, c, d, (cmd == 0xc4 || cmd == 0x29) && d->mult ? (uint32_t)d->mult : 1);
      break;
    case 0x34: case 0x39:             /* WRITE SECTORS / MULTIPLE EXT */
      d->lba48 = 1;
      /* fall through */
    case 0x30: case 0x31: case 0xc5:  /* WRITE SECTORS, WRITE MULTIPLE */
      d->lba  = get_lba (d);
      d->left = get_count (d);
      if (d->readonly || d->lba + d->left > d->sectors) {
        ata_abort (m, c, d, ER_ABRT | ER_IDNF);
        break;
      }

      ata_write_next (d, (cmd == 0xc5 || cmd == 0x39) && d->mult ? (uint32_t)d->mult : 1);
      break;
    case 0x25: case 0x35:             /* READ / WRITE DMA EXT */
      d->lba48 = 1;
      /* fall through */
    case 0xc8: case 0xc9: case 0xca: case 0xcb:
      n       = (cmd == 0xca || cmd == 0xcb || cmd == 0x35) ? 2 : 1;
      d->lba  = get_lba (d);
      d->left = get_count (d);
      if (d->lba + d->left > d->sectors || (n == 2 && d->readonly)) {
        ata_abort (m, c, d, ER_ABRT | ER_IDNF);
        break;
      }

      dma_start (m, c, d, (int)n);
      break;
    case 0x00:                        /* NOP */
    default:
      ata_abort (m, c, d, ER_ABRT);
      break;
  }
}

/* ------------------------------------------------------- data port ----- */
static uint32_t
data_read (machine_t *m, chan_t *c, drive_t *d, int size)
{
  uint32_t v = 0;
  int      i;

  if (d->xfer != X_ATA_IN && d->xfer != X_IDENT && d->xfer != X_PKT_IN) {
    return 0;
  }

  for (i = 0; i < size && d->pos < d->end; i++) {
    v |= (uint32_t)d->buf[d->pos++] << (8 * i);
  }

  if (d->pos < d->end) {
    return v;
  }

  switch (d->xfer) {
    case X_IDENT:
      d->xfer   = X_NONE;
      d->status = ST_DRDY | ST_DSC;
      break;
    case X_ATA_IN: {
      uint32_t per = d->chunk;

      d->lba  += d->chunk;
      d->left -= d->chunk;
      set_lba (d, d->left ? d->lba : d->lba - 1);
      if (d->left) {
        ata_read_next (m, c, d, per);
      } else {
        d->xfer   = X_NONE;
        d->status = ST_DRDY | ST_DSC;
      }

      break;
    }
    case X_PKT_IN:
      atapi_in_next (m, c, d);
      break;
    default:
      break;
  }

  return v;
}

static void
data_write (machine_t *m, chan_t *c, drive_t *d, int size, uint32_t val)
{
  int i;

  if (d->xfer != X_ATA_OUT && d->xfer != X_PKT_CMD) {
    return;
  }

  for (i = 0; i < size && d->pos < d->end; i++) {
    d->buf[d->pos++] = (uint8_t)(val >> (8 * i));
  }

  if (d->pos < d->end) {
    return;
  }

  if (d->xfer == X_PKT_CMD) {
    d->xfer = X_NONE;
    atapi_command (m, c, d);
    return;
  }

  if (host_disk_write (d->disk, d->lba * 512, d->buf, d->chunk * 512)) {
    ata_abort (m, c, d, ER_ABRT);
    return;
  }

  {
    uint32_t per = d->chunk;

    d->lba  += d->chunk;
    d->left -= d->chunk;
    set_lba (d, d->left ? d->lba : d->lba - 1);
    if (d->left) {
      ata_write_next (d, per);
      ide_irq (m, c, 1);
    } else {
      ata_done (m, c, d);
    }
  }
}

/* ----------------------------------------------------------- ports ----- */
static int
chan_read (machine_t *m, chan_t *c, unsigned reg, int size, uint32_t *val)
{
  drive_t *d   = &c->d[c->sel];
  int      hob = (c->control & 0x80) != 0;
  int      none = !c->d[0].present && !c->d[1].present;

  if (reg == 0) {
    *val = data_read (m, c, d, size);
    return 1;
  }

  if (none || !d->present) {
    /* a channel without the selected drive reads as zeros (status: not busy, not ready) */
    *val = (reg == 6) ? d->select : 0;
    if (reg == 7) {
      ide_irq (m, c, 0);
    }

    return 1;
  }

  switch (reg) {
    case 1: *val = d->error; break;
    case 2: *val = hob ? d->hob_nsector : d->nsector; break;
    case 3: *val = hob ? d->hob_sector : d->sector; break;
    case 4: *val = hob ? d->hob_lcyl : d->lcyl; break;
    case 5: *val = hob ? d->hob_hcyl : d->hcyl; break;
    case 6: *val = d->select; break;
    case 7:
      *val = (c->control & 4) ? ST_BSY : d->status;
      ide_irq (m, c, 0);
      break;
    default:                          /* alternate status */
      *val = (c->control & 4) ? ST_BSY : d->status;
      break;
  }

  return 1;
}

static int
chan_write (machine_t *m, chan_t *c, unsigned reg, int size, uint32_t val)
{
  drive_t *d = &c->d[c->sel];
  int      i;

  if (reg == 0) {
    data_write (m, c, d, size, val);
    return 1;
  }

  if (reg == 8) {                     /* device control */
    if ((val & 4) && !(c->control & 4)) {
      for (i = 0; i < 2; i++) {
        drive_reset (&c->d[i]);
        c->d[i].select = (uint8_t)(0xa0 | (i << 4));
      }

      c->sel = 0;
    }

    c->control = (uint8_t)val;
    ide_irq (m, c, c->irq_pending);
    return 1;
  }

  c->control &= 0x7f;                 /* any task file write clears HOB */
  switch (reg) {
    case 1:
      for (i = 0; i < 2; i++) {
        c->d[i].feature = (uint8_t)val;
      }

      break;
    case 2:
      for (i = 0; i < 2; i++) {
        c->d[i].hob_nsector = c->d[i].nsector; c->d[i].nsector = (uint8_t)val;
      }

      break;
    case 3:
      for (i = 0; i < 2; i++) {
        c->d[i].hob_sector = c->d[i].sector; c->d[i].sector = (uint8_t)val;
      }

      break;
    case 4:
      for (i = 0; i < 2; i++) {
        c->d[i].hob_lcyl = c->d[i].lcyl; c->d[i].lcyl = (uint8_t)val;
      }

      break;
    case 5:
      for (i = 0; i < 2; i++) {
        c->d[i].hob_hcyl = c->d[i].hcyl; c->d[i].hcyl = (uint8_t)val;
      }

      break;
    case 6:
      c->sel = (val >> 4) & 1;
      for (i = 0; i < 2; i++) {
        c->d[i].select = (uint8_t)(val | 0xa0);
      }

      break;
    case 7:
      ata_command (m, c, d, (uint8_t)val);
      break;
    default:
      break;
  }

  return 1;
}

static int
bm_io (machine_t *m, ide_t *s, uint16_t port, int size, uint32_t *val, int write)
{
  uint32_t base = pci_bar_io (&s->pci, 4);
  chan_t  *c;
  unsigned reg;

  if (!base || port < base || port >= base + 16) {
    return 0;
  }

  c   = &s->ch[(port - base) >> 3];
  reg = (port - base) & 7;
  if (!write) {
    switch (reg) {
      case 0: *val = c->bm_cmd; break;
      case 2: *val = c->bm_status; break;
      case 4: case 5: case 6: case 7:
        *val = c->bm_prdt >> (8 * (reg - 4));
        break;
      default: *val = 0xff; break;
    }

    return 1;
  }

  switch (reg) {
    case 0: {
      int start = (*val & 1) && !(c->bm_cmd & 1);

      c->bm_cmd = *val & 0x09;
      if (start) {
        drive_t *d = &c->d[c->sel];

        c->bm_status |= 1;
        if (d->dma) {
          dma_run (m, c, d);
        }
      } else if (!(*val & 1)) {
        c->bm_status &= ~1;
      }

      break;
    }
    case 2:
      c->bm_status = (uint8_t)((*val & 0x60) | (c->bm_status & 1) | (c->bm_status & ~*val & 0x06));
      break;
    case 4: case 5: case 6: case 7: {
      int i;

      for (i = 0; i < size && reg + i < 8; i++) {
        int sh = 8 * (reg - 4 + i);

        c->bm_prdt = (c->bm_prdt & ~(0xffu << sh)) | (((*val >> (8 * i)) & 0xff) << sh);
      }

      c->bm_prdt &= ~3u;
      break;
    }
    default:
      break;
  }

  return 1;
}

int
ide_io_read (machine_t *m, uint16_t port, int size, uint32_t *val)
{
  ide_t *s = m->ide;

  if (port >= 0x1f0 && port <= 0x1f7) {
    return chan_read (m, &s->ch[0], port - 0x1f0, size, val);
  }

  if (port >= 0x170 && port <= 0x177) {
    return chan_read (m, &s->ch[1], port - 0x170, size, val);
  }

  if (port == 0x3f6) {
    return chan_read (m, &s->ch[0], 8, size, val);
  }

  if (port == 0x376) {
    return chan_read (m, &s->ch[1], 8, size, val);
  }

  return bm_io (m, s, port, size, val, 0);
}

int
ide_io_write (machine_t *m, uint16_t port, int size, uint32_t val)
{
  ide_t *s = m->ide;

  if (port >= 0x1f0 && port <= 0x1f7) {
    return chan_write (m, &s->ch[0], port - 0x1f0, size, val);
  }

  if (port >= 0x170 && port <= 0x177) {
    return chan_write (m, &s->ch[1], port - 0x170, size, val);
  }

  if (port == 0x3f6) {
    return chan_write (m, &s->ch[0], 8, size, val);
  }

  if (port == 0x376) {
    return chan_write (m, &s->ch[1], 8, size, val);
  }

  return bm_io (m, s, port, size, &val, 1);
}

int
ide_attach (machine_t *m, int channel, int unit, void *disk, uint64_t size_bytes, int cdrom, int readonly)
{
  ide_t   *s = m->ide;
  drive_t *d;

  if (!s || channel < 0 || channel > 1 || unit < 0 || unit > 1) {
    return -1;
  }

  d           = &s->ch[channel].d[unit];
  d->present  = 1;
  d->cdrom    = cdrom;
  d->readonly = readonly || cdrom;
  d->disk     = disk;
  d->bytes    = size_bytes;
  if (cdrom) {
    d->sectors = size_bytes / 2048;
  } else {
    d->sectors = size_bytes / 512;
    d->heads   = 16;
    d->secs    = 63;
    d->cyls    = d->sectors / (16 * 63) > 16383 ? 16383 : (unsigned)(d->sectors / (16 * 63));
  }

  if (!d->buf) {
    d->buf = host_alloc (BUF_SIZE);
  }

  drive_reset (d);
  d->select = (uint8_t)(0xa0 | (unit << 4));
  return d->buf ? 0 : -1;
}

void
ide_reset (machine_t *m)
{
  ide_t *s = m->ide;
  int    i, j;

  s->pci.slot = 1; s->pci.fn = 1;
  pci_init_config (&s->pci, 0x8086, 0x7010, 0x01018000, 0x1af4, 0x1100);
  s->pci.cfg[0x04]   = 0x00;
  s->pci.cfg[0x06]   = 0x80; s->pci.cfg[0x07] = 0x02;
  s->pci.cfg[0x20]   = 0x01;
  s->pci.bar_size[4] = 16;
  s->pci.bar_io[4]   = 1;
  for (i = 0; i < 2; i++) {
    chan_t *c = &s->ch[i];

    c->irq       = i ? 15 : 14;
    c->sel       = 0;
    c->control   = 0;
    c->irq_level = c->irq_pending = 0;
    c->bm_cmd    = 0;
    c->bm_status = 0;
    c->bm_prdt   = 0;
    for (j = 0; j < 2; j++) {
      drive_reset (&c->d[j]);
      c->d[j].select = (uint8_t)(0xa0 | (j << 4));
      c->d[j].mdma   = c->d[j].udma = 0;
      c->d[j].sense  = c->d[j].asc = 0;
    }
  }
}

void
ide_init (machine_t *m)
{
  ide_t *s = m->ide = host_alloc (sizeof (ide_t));

  ide_reset (m);
  pci_register (m, &s->pci);
}
