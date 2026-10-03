/*
 * PCI configuration mechanism #1 (0xCF8/0xCFC) with a host bridge and the
 * virtio devices. Bus 0 only.
 */
#include <string.h>
#include "x64e.h"

static pci_dev_t *
pci_find (machine_t *m, uint32_t addr)
{
  unsigned bus = (addr >> 16) & 0xff;
  unsigned dev = (addr >> 11) & 0x1f;
  unsigned fn  = (addr >> 8) & 7;
  int      i;

  if (bus != 0) {
    return NULL;
  }

  for (i = 0; i < m->npci; i++) {
    if (m->pci[i]->slot == (int)dev && m->pci[i]->fn == (int)fn) {
      return m->pci[i];
    }
  }

  return NULL;
}

void
pci_register (machine_t *m, pci_dev_t *d)
{
  m->pci[m->npci++] = d;
}

void
pci_init_config (pci_dev_t *d, uint16_t vendor, uint16_t device, uint32_t class_rev,
                 uint16_t sub_vendor, uint16_t sub_device)
{
  uint8_t *c = d->cfg;

  memset (c, 0, sizeof (d->cfg));
  c[0x00] = vendor;       c[0x01] = vendor >> 8;
  c[0x02] = device;       c[0x03] = device >> 8;
  c[0x04] = 0x07;                               /* I/O, memory, bus master */
  c[0x06] = 0x00;         c[0x07] = 0x02;       /* status: medium devsel   */
  c[0x08] = class_rev;    c[0x09] = class_rev >> 8;
  c[0x0a] = class_rev >> 16; c[0x0b] = class_rev >> 24;
  c[0x2c] = sub_vendor;   c[0x2d] = sub_vendor >> 8;
  c[0x2e] = sub_device;   c[0x2f] = sub_device >> 8;
}

static uint32_t
cfg_read (pci_dev_t *d, unsigned reg, int size)
{
  uint32_t v = 0;
  int      i;

  for (i = 0; i < size; i++) {
    v |= (uint32_t)d->cfg[(reg + i) & 0xff] << (8 * i);
  }

  return v;
}

static void
cfg_write (pci_dev_t *d, unsigned reg, int size, uint32_t val)
{
  int i, b;

  for (i = 0; i < size; i++) {
    unsigned r = (reg + i) & 0xff;
    uint8_t  v = (uint8_t)(val >> (8 * i));

    /* BARs: only the bits outside the size are writable (sizing protocol) */
    if (r >= 0x10 && r < 0x28) {
      b = (r - 0x10) / 4;
      if (d->bar_size[b] == 0) {
        continue;
      }

      {
        uint32_t mask = ~(d->bar_size[b] - 1);
        uint32_t cur  = cfg_read (d, 0x10 + b * 4, 4);
        int      sh   = 8 * ((r - 0x10) & 3);
        uint32_t nv   = (cur & ~(0xffu << sh)) | ((uint32_t)v << sh);

        nv = (nv & mask) | (d->bar_io[b] ? 1 : (cur & 0xf));
        d->cfg[0x10 + b * 4 + 0] = nv;
        d->cfg[0x10 + b * 4 + 1] = nv >> 8;
        d->cfg[0x10 + b * 4 + 2] = nv >> 16;
        d->cfg[0x10 + b * 4 + 3] = nv >> 24;
      }

      continue;
    }

    /* writable: command, cache line, latency, interrupt line, config 0x40+ */
    if (r == 0x04 || r == 0x05 || r == 0x0c || r == 0x0d || r == 0x3c || r >= 0x40) {
      d->cfg[r] = v;
    }
  }
}

/* current I/O base of BAR b, or 0 if disabled */
uint32_t
pci_bar_io (pci_dev_t *d, int b)
{
  if (!(d->cfg[0x04] & 1)) {
    return 0;
  }

  return cfg_read (d, 0x10 + b * 4, 4) & ~3u;
}

/* current address of memory BAR b, or 0 if disabled */
uint32_t
pci_bar_mem (pci_dev_t *d, int b)
{
  if (!(d->cfg[0x04] & 2)) {
    return 0;
  }

  return cfg_read (d, 0x10 + b * 4, 4) & ~15u;
}

uint32_t
pci_io_read (machine_t *m, uint16_t port, int size)
{
  pci_dev_t *d;

  if (port == 0xcf8 && size == 4) {
    return m->pci_addr;
  }

  if (port >= 0xcfc && port <= 0xcff && (m->pci_addr & 0x80000000u)) {
    d = pci_find (m, m->pci_addr);
    if (!d) {
      return 0xffffffff;
    }

    return cfg_read (d, (m->pci_addr & 0xfc) + (port & 3), size);
  }

  return 0xffffffff;
}

void
pci_io_write (machine_t *m, uint16_t port, int size, uint32_t val)
{
  pci_dev_t *d;

  if (port == 0xcf8 && size == 4) {
    m->pci_addr = val;
    return;
  }

  if (port >= 0xcfc && port <= 0xcff && (m->pci_addr & 0x80000000u)) {
    d = pci_find (m, m->pci_addr);
    if (d) {
      cfg_write (d, (m->pci_addr & 0xfc) + (port & 3), size, val);
      if (d->changed) {
        d->changed (m, d, (m->pci_addr & 0xfc) + (port & 3));
      }
    }
  }
}

/* host bridge: Intel 440FX */
static pci_dev_t host_bridge;

void
pci_init (machine_t *m)
{
  host_bridge.slot = 0;
  pci_init_config (&host_bridge, 0x8086, 0x1237, 0x06000002, 0x1af4, 0x1100);
  pci_register (m, &host_bridge);
}
