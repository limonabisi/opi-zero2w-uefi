/*
 * virtio-net, legacy PCI transport (virtio 0.9.5): I/O BAR 0, queue 0 = RX,
 * queue 1 = TX, INTx on a level-triggered PIC line. Frames go to and come
 * from the host through host_net_send() / host_net_recv(), unchanged, so
 * the x86 system sits directly on the host's network (phone USB tethering,
 * USB Ethernet). The card uses the host adapter's MAC address.
 */
#include <string.h>
#include "x64e.h"

#define QSIZE        256
#define HDR_LEN      10               /* virtio_net_hdr, no MRG_RXBUF */
#define F_MAC        (1u << 5)
#define F_STATUS     (1u << 16)
#define FRAME_MAX    1600

static void *
gpa (machine_t *m, uint64_t addr, uint64_t len)
{
  if (addr + len > m->ram_size || addr + len < addr) {
    return NULL;
  }

  return m->ram + addr;
}

static void
vnet_irq (machine_t *m, vnet_t *v)
{
  pic_set_irq (m, v->pci.cfg[0x3c] & 15, v->isr ? 1 : 0);
}

static uint8_t *
ring (machine_t *m, vnet_t *v, int q, uint8_t **avail, uint8_t **used)
{
  uint8_t *base;

  if (v->pfn[q] == 0) {
    return NULL;
  }

  base = gpa (m, (uint64_t)v->pfn[q] << 12, 3 * 4096 + QSIZE * 24);
  if (!base) {
    return NULL;
  }

  *avail = base + QSIZE * 16;
  *used  = base + ((QSIZE * 16 + 6 + QSIZE * 2 + 4095) & ~4095);
  return base;
}

static void
put_used (uint8_t *used, uint16_t head, uint32_t len)
{
  uint16_t idx = *(uint16_t *)(used + 2);

  *(uint32_t *)(used + 4 + (idx % QSIZE) * 8)     = head;
  *(uint32_t *)(used + 4 + (idx % QSIZE) * 8 + 4) = len;
  __sync_synchronize ();
  *(uint16_t *)(used + 2) = idx + 1;
}

/* guest -> host */
static void
vnet_tx (machine_t *m, vnet_t *v)
{
  uint8_t  *desc, *avail, *used;
  uint8_t   frame[HDR_LEN + FRAME_MAX];
  uint16_t  avail_idx;
  int       done = 0;

  desc = ring (m, v, 1, &avail, &used);
  if (!desc) {
    return;
  }

  avail_idx = *(uint16_t *)(avail + 2);
  while (v->last_avail[1] != avail_idx) {
    uint16_t head = *(uint16_t *)(avail + 4 + (v->last_avail[1] % QSIZE) * 2);
    uint16_t i    = head;
    uint32_t n    = 0;
    int      hops = 0;

    v->last_avail[1]++;
    for (;;) {
      uint8_t *d     = desc + (i % QSIZE) * 16;
      uint64_t addr  = *(uint64_t *)d;
      uint32_t len   = *(uint32_t *)(d + 8);
      uint16_t flags = *(uint16_t *)(d + 12);
      uint8_t *p     = gpa (m, addr, len);

      if (p && !(flags & 2)) {
        uint32_t c = len;

        if (n + c > sizeof (frame)) {
          c = (uint32_t)sizeof (frame) - n;
        }

        memcpy (frame + n, p, c);
        n += c;
      }

      if (!(flags & 1) || ++hops > QSIZE) {
        break;
      }

      i = *(uint16_t *)(d + 14);
    }

    if (n > HDR_LEN) {
      host_net_send (frame + HDR_LEN, n - HDR_LEN);
      v->tx_frames++;
    }

    put_used (used, head, 0);
    done++;
  }

  if (done) {
    v->isr |= 1;
    vnet_irq (m, v);
  }
}

/* host -> guest: returns 1 if the frame was delivered */
static int
vnet_rx_one (machine_t *m, vnet_t *v, const uint8_t *buf, uint32_t len)
{
  uint8_t  *desc, *avail, *used;
  uint8_t   hdr[HDR_LEN];
  uint16_t  head, i;
  uint32_t  total = HDR_LEN + len, off = 0;
  int       hops  = 0;

  desc = ring (m, v, 0, &avail, &used);
  if (!desc || !(v->status & 4) || v->last_avail[0] == *(uint16_t *)(avail + 2)) {
    return 0;                         /* no receive buffer posted */
  }

  memset (hdr, 0, sizeof (hdr));
  head = *(uint16_t *)(avail + 4 + (v->last_avail[0] % QSIZE) * 2);
  v->last_avail[0]++;
  i = head;
  for (;;) {
    uint8_t *d     = desc + (i % QSIZE) * 16;
    uint64_t addr  = *(uint64_t *)d;
    uint32_t dlen  = *(uint32_t *)(d + 8);
    uint16_t flags = *(uint16_t *)(d + 12);
    uint8_t *p     = gpa (m, addr, dlen);

    if (p && (flags & 2)) {
      uint32_t k = 0;

      while (k < dlen && off < total) {
        p[k++] = off < HDR_LEN ? hdr[off] : buf[off - HDR_LEN];
        off++;
      }

      uc_x86_invalidate_host (m->uc, p, k);
    }

    if (off >= total || !(flags & 1) || ++hops > QSIZE) {
      break;
    }

    i = *(uint16_t *)(d + 14);
  }

  put_used (used, head, off);
  v->rx_frames++;
  return 1;
}

/* called from the machine loop: move frames from the host to the guest */
void
vnet_poll (machine_t *m, vnet_t *v)
{
  uint8_t  buf[FRAME_MAX];
  int      got = 0, n;

  if (!v->present || !(v->status & 4)) {
    return;
  }

  /* a frame held back because the guest had no buffer yet */
  if (v->pending_len) {
    if (!vnet_rx_one (m, v, v->pending, v->pending_len)) {
      return;
    }

    v->pending_len = 0;
    got++;
  }

  while (got < 32 && (n = host_net_recv (buf, sizeof (buf))) > 0) {
    if (!vnet_rx_one (m, v, buf, (uint32_t)n)) {
      memcpy (v->pending, buf, n);
      v->pending_len = (uint32_t)n;
      break;
    }

    got++;
  }

  if (got) {
    v->isr |= 1;
    vnet_irq (m, v);
  }
}

int
vnet_io_read (machine_t *m, vnet_t *v, uint16_t off, int size, uint32_t *val)
{
  uint8_t  cfg[8];
  uint32_t r = 0;
  int      i;

  switch (off) {
    case 0x00: r = F_MAC | F_STATUS; break;
    case 0x04: r = v->guest_features; break;
    case 0x08: r = v->pfn[v->qsel & 1]; break;
    case 0x0c: r = v->qsel < 2 ? QSIZE : 0; break;
    case 0x0e: r = v->qsel; break;
    case 0x12: r = v->status; break;
    case 0x13:
      r      = v->isr;
      v->isr = 0;
      vnet_irq (m, v);
      break;
    default:
      if (off >= 0x14 && off < 0x14 + sizeof (cfg)) {
        memcpy (cfg, v->mac, 6);
        cfg[6] = 1;                             /* link up */
        cfg[7] = 0;
        for (i = 0; i < size && off - 0x14 + i < (int)sizeof (cfg); i++) {
          r |= (uint32_t)cfg[off - 0x14 + i] << (8 * i);
        }
      }
  }

  *val = r;
  return 0;
}

void
vnet_io_write (machine_t *m, vnet_t *v, uint16_t off, int size, uint32_t val)
{
  (void)size;
  switch (off) {
    case 0x04: v->guest_features = val; break;
    case 0x08:
      if (v->qsel < 2) {
        v->pfn[v->qsel] = val;
        v->last_avail[v->qsel] = 0;
      }

      break;
    case 0x0e: v->qsel = (uint16_t)val; break;
    case 0x10:                                  /* notify */
      if ((val & 0xffff) == 1) {
        vnet_tx (m, v);
      } else {
        vnet_poll (m, v);
      }

      break;
    case 0x12:
      v->status = (uint8_t)val;
      if (val == 0) {                           /* reset */
        v->pfn[0] = v->pfn[1] = 0;
        v->last_avail[0] = v->last_avail[1] = 0;
        v->isr = 0;
        v->pending_len = 0;
        vnet_irq (m, v);
      }

      break;
    default:
      break;
  }
}

void
vnet_init (machine_t *m, vnet_t *v, int slot, int irq, const uint8_t mac[6])
{
  memset (v, 0, sizeof (*v));
  v->present  = 1;
  v->pci.slot = slot;
  memcpy (v->mac, mac, 6);
  pci_init_config (&v->pci, 0x1af4, 0x1000, 0x02000000, 0x1af4, 1);
  v->pci.bar_size[0] = 0x40;
  v->pci.bar_io[0]   = 1;
  v->pci.cfg[0x10]   = (uint8_t)(0xc000 + slot * 0x100) | 1;
  v->pci.cfg[0x11]   = (0xc000 + slot * 0x100) >> 8;
  v->pci.cfg[0x3c]   = irq;
  v->pci.cfg[0x3d]   = 1;                     /* INTA */
  m->pic[irq >> 3].elcr |= 1 << (irq & 7);    /* PCI interrupts are level */
  pci_register (m, &v->pci);
}
