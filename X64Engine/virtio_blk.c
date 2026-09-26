/*
 * virtio-blk, legacy PCI transport (virtio 0.9.5): I/O BAR 0, one queue,
 * INTx on a level-triggered PIC line. Requests are served synchronously.
 */
#include <string.h>
#include "x64e.h"

#define QSIZE        256
#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_T_FLUSH  4
#define VIRTIO_BLK_T_GET_ID 8
#define F_RO         (1u << 5)
#define F_BLK_SIZE   (1u << 6)
#define F_SEG_MAX    (1u << 2)
#define F_FLUSH      (1u << 9)

static void *
gpa (machine_t *m, uint64_t addr, uint64_t len)
{
  if (addr + len > m->ram_size || addr + len < addr) {
    return NULL;
  }

  return m->ram + addr;
}

static void
vblk_irq (machine_t *m, vblk_t *v)
{
  pic_set_irq (m, v->pci.cfg[0x3c] & 15, v->isr ? 1 : 0);
}

static void
vblk_process (machine_t *m, vblk_t *v)
{
  uint8_t  *base = gpa (m, (uint64_t)v->pfn << 12, 3 * 4096 + QSIZE * 24);
  uint8_t  *desc, *avail, *used;
  uint16_t  avail_idx, used_idx;
  int       done = 0;

  if (!base || v->pfn == 0) {
    return;
  }

  desc  = base;
  avail = base + QSIZE * 16;
  used  = base + ((QSIZE * 16 + 6 + QSIZE * 2 + 4095) & ~4095);

  avail_idx = *(uint16_t *)(avail + 2);
  while (v->last_avail != avail_idx) {
    uint16_t head = *(uint16_t *)(avail + 4 + (v->last_avail % QSIZE) * 2);
    uint16_t i    = head;
    uint32_t type = 0, written = 0;
    uint64_t sector = 0, off;
    uint8_t *status = NULL;
    int      n      = 0, first = 1, ok = 1;

    v->last_avail++;
    for (;;) {
      uint8_t *d     = desc + (i % QSIZE) * 16;
      uint64_t addr  = *(uint64_t *)d;
      uint32_t len   = *(uint32_t *)(d + 8);
      uint16_t flags = *(uint16_t *)(d + 12);
      uint16_t next  = *(uint16_t *)(d + 14);
      uint8_t *p     = gpa (m, addr, len);

      if (!p) {
        ok = 0;
        break;
      }

      if (first) {
        first  = 0;
        type   = *(uint32_t *)p;
        sector = *(uint64_t *)(p + 8);
        off    = sector * 512;
      } else if (!(flags & 1) && (flags & 2) && len == 1) {
        status = p;             /* last, device-writable, one byte */
      } else if (type == VIRTIO_BLK_T_IN && (flags & 2)) {
        if (host_disk_read (v->disk, off, p, len)) {
          ok = 0;
        }

        off     += len;
        written += len;
      } else if (type == VIRTIO_BLK_T_OUT && !(flags & 2)) {
        if (v->readonly || host_disk_write (v->disk, off, p, len)) {
          ok = 0;
        }

        off += len;
      } else if (type == VIRTIO_BLK_T_GET_ID && (flags & 2)) {
        memset (p, 0, len);
        memcpy (p, "x64e-disk", len < 9 ? len : 9);
        written += len;
      }

      if (!(flags & 1) || ++n > QSIZE) {
        break;
      }

      i = next;
    }

    if (status) {
      *status  = ok ? 0 : 1;
      written += 1;
    }

    used_idx = *(uint16_t *)(used + 2);
    *(uint32_t *)(used + 4 + (used_idx % QSIZE) * 8)     = head;
    *(uint32_t *)(used + 4 + (used_idx % QSIZE) * 8 + 4) = written;
    __sync_synchronize ();
    *(uint16_t *)(used + 2) = used_idx + 1;
    done++;
    v->requests++;
  }

  if (done) {
    v->isr |= 1;
    vblk_irq (m, v);
  }
}

int
vblk_io_read (machine_t *m, vblk_t *v, uint16_t off, int size, uint32_t *val)
{
  uint64_t cap = v->disk_size / 512;
  uint8_t  cfg[24];
  uint32_t r = 0;
  int      i;

  switch (off) {
    case 0x00: r = F_BLK_SIZE | F_SEG_MAX | (v->readonly ? F_RO : F_FLUSH); break;
    case 0x04: r = v->guest_features; break;
    case 0x08: r = v->pfn; break;
    case 0x0c: r = QSIZE; break;
    case 0x0e: r = 0; break;
    case 0x12: r = v->status; break;
    case 0x13:
      r      = v->isr;
      v->isr = 0;
      vblk_irq (m, v);
      break;
    default:
      if (off >= 0x14 && off < 0x14 + sizeof (cfg)) {
        memset (cfg, 0, sizeof (cfg));
        memcpy (cfg, &cap, 8);                        /* capacity  */
        *(uint32_t *)(cfg + 12) = 126;                /* seg_max   */
        *(uint32_t *)(cfg + 20) = 512;                /* blk_size  */
        for (i = 0; i < size && off - 0x14 + i < (int)sizeof (cfg); i++) {
          r |= (uint32_t)cfg[off - 0x14 + i] << (8 * i);
        }

        *val = r;
        return 0;
      }

      r = 0;
  }

  *val = r;
  return 0;
}

void
vblk_io_write (machine_t *m, vblk_t *v, uint16_t off, int size, uint32_t val)
{
  switch (off) {
    case 0x04: v->guest_features = val; break;
    case 0x08: v->pfn = val; break;
    case 0x0e: break;                       /* queue select: only queue 0 */
    case 0x10: vblk_process (m, v); break;  /* notify */
    case 0x12:
      v->status = (uint8_t)val;
      if (val == 0) {                       /* reset */
        v->pfn = 0;
        v->last_avail = 0;
        v->isr = 0;
        vblk_irq (m, v);
      }

      break;
    default:
      break;
  }
}

void
vblk_init (machine_t *m, vblk_t *v, int slot, int irq, void *disk, uint64_t size, int ro)
{
  memset (v, 0, sizeof (*v));
  v->disk      = disk;
  v->disk_size = size;
  v->readonly  = ro;
  v->pci.slot  = slot;
  pci_init_config (&v->pci, 0x1af4, 0x1001, 0x01000000, 0x1af4, 2);
  v->pci.bar_size[0] = 0x40;
  v->pci.bar_io[0]   = 1;
  v->pci.cfg[0x10]   = (uint8_t)(0xc000 + slot * 0x100) | 1;
  v->pci.cfg[0x11]   = (0xc000 + slot * 0x100) >> 8;
  v->pci.cfg[0x3c]   = irq;
  v->pci.cfg[0x3d]   = 1;                   /* INTA */
  m->pic[irq >> 3].elcr |= 1 << (irq & 7);  /* PCI interrupts are level */
  pci_register (m, &v->pci);
}
