/*
 * The "PC with a BIOS" machine (see pc.h): i440FX / PIIX3 / PIIX4 chipset
 * glue, fw_cfg for SeaBIOS, ACPI power management ports, interrupt routing
 * and reset.
 */
#include <string.h>
#include "pc.h"

#define FW_FILES      2
#define FW_FILE_BASE  0x20

typedef struct {
  pci_dev_t      isa, pm;
  const uint8_t *bios, *vgabios;
  size_t         bios_size, vgabios_size;
  uint8_t       *bios_high;
  /* fw_cfg */
  uint16_t       fw_sel;
  uint32_t       fw_off;
  uint8_t        fw_dir[4 + FW_FILES * 64];
  uint8_t        fw_small[8];
  /* ACPI PM block */
  uint16_t       pm1_sts, pm1_en, pm1_cnt;
  uint32_t       tmr_overflows;
  int            sci;
  uint8_t        apm_cnt, apm_sts, port92;
  uint16_t       intx[4];             /* per PIRQ: bit per PCI device asserting it */
  uint64_t       last_draw;
} pc_t;

static const char boot_order[] = "";

/* ------------------------------------------------------- interrupts ---- */
void
cpu_irq_update (machine_t *m)
{
  int level = apic_has_interrupt (m) || (m->pic_out && apic_accepts_pic (m));

  if (level != m->cpu_irq_level) {
    m->cpu_irq_level = level;
    uc_x86_set_irq_line (m->uc, level);
  }
}

static int
pc_irq_ack (void *opaque)
{
  machine_t *m = opaque;
  int        v;

  if (apic_has_interrupt (m)) {
    v = apic_ack_interrupt (m);
    m->irqs++;
    cpu_irq_update (m);
    return v;
  }

  if (m->pic_out && apic_accepts_pic (m)) {
    return pic_ack (m);
  }

  cpu_irq_update (m);
  return -1;
}

/* PCI INTx: through the PIIX3 PIRQ registers to an ISA interrupt */
void
pci_set_intx (machine_t *m, pci_dev_t *d, int level)
{
  pc_t    *p = m->pc;
  int      pirq, i, idx = 0, any = 0;
  uint8_t  irq;

  if (d->cfg[0x3d] == 0) {
    return;
  }

  for (i = 0; i < m->npci; i++) {
    if (m->pci[i] == d) {
      idx = i;
    }
  }

  pirq = (d->slot - 1 + d->cfg[0x3d] - 1) & 3;
  if (level) {
    p->intx[pirq] |= (uint16_t)(1u << idx);
  } else {
    p->intx[pirq] &= (uint16_t) ~(1u << idx);
  }

  irq = p->isa.cfg[0x60 + pirq];
  if (irq & 0x80) {
    return;
  }

  for (i = 0; i < 4; i++) {
    if (p->isa.cfg[0x60 + i] == irq && p->intx[i]) {
      any = 1;
    }
  }

  pic_set_irq (m, irq & 15, any);
}

uint8_t *
machine_ram (machine_t *m, uint64_t gpa, uint64_t len)
{
  if (gpa + len > m->ram_size || gpa + len < gpa) {
    return NULL;
  }

  return m->ram + gpa;
}

/* ----------------------------------------------------------- fw_cfg ---- */
static const uint8_t *
fw_item (machine_t *m, uint32_t *len)
{
  pc_t *p = m->pc;

  memset (p->fw_small, 0, sizeof (p->fw_small));
  *len = sizeof (p->fw_small);
  switch (p->fw_sel) {
    case 0x00: *len = 4; return (const uint8_t *)"QEMU";
    case 0x01: p->fw_small[0] = 1; return p->fw_small;        /* interface version, no DMA */
    case 0x05: case 0x0f: p->fw_small[0] = 1; return p->fw_small;   /* CPUs */
    case 0x8002: p->fw_small[0] = 1; return p->fw_small;      /* IRQ0 override */
    case 0x19: *len = sizeof (p->fw_dir); return p->fw_dir;
    case FW_FILE_BASE: *len = (uint32_t)p->vgabios_size; return p->vgabios;
    case FW_FILE_BASE + 1: *len = sizeof (boot_order) - 1; return (const uint8_t *)boot_order;
    default: return p->fw_small;
  }
}

static void
fw_dir_add (pc_t *p, int n, const char *name, uint32_t size)
{
  uint8_t *e = p->fw_dir + 4 + n * 64;

  e[0] = (uint8_t)(size >> 24); e[1] = (uint8_t)(size >> 16); e[2] = (uint8_t)(size >> 8); e[3] = (uint8_t)size;
  e[4] = 0; e[5] = (uint8_t)(FW_FILE_BASE + n);
  strncpy ((char *)e + 8, name, 55);
}

/* ------------------------------------------------------------ ACPI PM -- */
static uint32_t
pm_timer (uint64_t now)
{
  return (uint32_t)((now / 1000000000ULL) * 3579545ULL + (now % 1000000000ULL) * 3579545ULL / 1000000000ULL);
}

static void
pm_update (machine_t *m, uint64_t now)
{
  pc_t    *p = m->pc;
  uint32_t ov = pm_timer (now) >> 23;
  int      level;

  if (ov != p->tmr_overflows) {
    p->tmr_overflows = ov;
    p->pm1_sts      |= 1;
  }

  level = (p->pm1_sts & p->pm1_en & 0x0521) != 0 && (p->pm1_cnt & 1);
  if (level != p->sci) {
    p->sci = level;
    pic_set_irq (m, 9, level);
  }
}

static int
pm_io (machine_t *m, uint16_t port, int size, uint32_t *val, int write)
{
  pc_t    *p    = m->pc;
  uint16_t base = (uint16_t)((p->pm.cfg[0x40] | (p->pm.cfg[0x41] << 8)) & 0xffc0);
  uint64_t now;
  unsigned off;

  if (base == 0 || port < base || port >= base + 0x40) {
    return 0;
  }

  off = port - base;
  now = host_now_ns ();
  if (!write) {
    uint32_t v = 0;

    switch (off & ~1u) {
      case 0x00: pm_update (m, now); v = p->pm1_sts; break;
      case 0x02: v = p->pm1_en; break;
      case 0x04: v = p->pm1_cnt; break;
      case 0x08: case 0x0a: v = pm_timer (now) & 0xffffff; break;
      default: break;
    }

    if (off >= 0x08 && off < 0x0c) {
      v >>= 8 * (off - 8);
    } else {
      v >>= 8 * (off & 1);
    }

    *val = v;
    return 1;
  }

  switch (off) {
    case 0x00:
      p->pm1_sts &= (uint16_t) ~*val;
      break;
    case 0x01:
      p->pm1_sts &= (uint16_t) ~(*val << 8);
      break;
    case 0x02:
      p->pm1_en = size == 1 ? (uint16_t)((p->pm1_en & 0xff00) | (*val & 0xff)) : (uint16_t)*val;
      break;
    case 0x03:
      p->pm1_en = (uint16_t)((p->pm1_en & 0x00ff) | ((*val & 0xff) << 8));
      break;
    case 0x04: case 0x05: {
      uint16_t v = off == 5 ? (uint16_t)(*val << 8) : (uint16_t)*val;

      if (size == 1 && off == 4) {
        v = (uint16_t)((p->pm1_cnt & 0xff00) | (v & 0xff));
      }

      p->pm1_cnt = v & ~0x2000;
      if (v & 0x2000) {
        /* SLP_EN: SeaBIOS's DSDT has S5 as type 0 */
        if (((v >> 10) & 7) == 0) {
          host_log ("\nx64e: the x86 system powered off\n");
          m->quit = 1;
          uc_emu_stop (m->uc);
        }
      }

      break;
    }
    default:
      break;
  }

  pm_update (m, now);
  return 1;
}

/* ------------------------------------------------------------ ports ---- */
int
pc_io_read (machine_t *m, uint16_t port, int size, uint32_t *val)
{
  pc_t *p = m->pc;

  if (ide_io_read (m, port, size, val) || vga_io_read (m, port, size, val) ||
      rtc_io_read (m, port, size, val)) {
    return 1;
  }

  switch (port) {
    case 0x511: {
      uint32_t       len;
      const uint8_t *d = fw_item (m, &len);

      *val = (d && p->fw_off < len) ? d[p->fw_off] : 0;
      p->fw_off++;
      return 1;
    }
    case 0x510:
      *val = p->fw_sel;
      return 1;
    case 0x92:
      *val = p->port92 | 2;
      return 1;
    case 0x402:
      *val = 0xe9;
      return 1;
    case 0xb2: *val = p->apm_cnt; return 1;
    case 0xb3: *val = p->apm_sts; return 1;
    case 0xcf9: *val = 0; return 1;
    case 0xaf00: *val = 1; return 1;              /* CPU 0 present */
    default:
      break;
  }

  /* PCI hotplug and GPE blocks the DSDT looks at: nothing there */
  if ((port >= 0xae00 && port < 0xae20) || (port >= 0xaf00 && port < 0xaf20) ||
      (port >= 0xafe0 && port < 0xafe4)) {
    *val = 0;
    return 1;
  }

  return pm_io (m, port, size, val, 0);
}

int
pc_io_write (machine_t *m, uint16_t port, int size, uint32_t val)
{
  pc_t *p = m->pc;

  if (ide_io_write (m, port, size, val) || vga_io_write (m, port, size, val) ||
      rtc_io_write (m, port, size, val)) {
    return 1;
  }

  switch (port) {
    case 0x510:
      p->fw_sel = (uint16_t)val;
      p->fw_off = 0;
      return 1;
    case 0x511:
      return 1;
    case 0x402: {
      char c = (char)val;

      host_console_write (&c, 1);
      return 1;
    }
    case 0x92:
      p->port92 = val & 2;
      if (val & 1) {
        m->reset_request = 1;
        uc_emu_stop (m->uc);
      }

      return 1;
    case 0xcf9:
      if (val & 4) {
        m->reset_request = 1;
        uc_emu_stop (m->uc);
      }

      return 1;
    case 0xb2:
      p->apm_cnt = (uint8_t)val;
      if (val == 0xf1) {
        p->pm1_cnt |= 1;                  /* ACPI enable (no SMM here) */
      } else if (val == 0xf0) {
        p->pm1_cnt &= ~1;
      }

      return 1;
    case 0xb3:
      p->apm_sts = (uint8_t)val;
      return 1;
    case 0x80: case 0xed:                 /* POST code, I/O delay */
      return 1;
    default:
      break;
  }

  if ((port >= 0xae00 && port < 0xae20) || (port >= 0xaf00 && port < 0xaf20) ||
      (port >= 0xafe0 && port < 0xafe4)) {
    return 1;
  }

  return pm_io (m, port, size, &val, 1);
}

void
pc_tick (machine_t *m, uint64_t now)
{
  pc_t *p = m->pc;

  apic_tick (m, now);
  rtc_tick (m, now);
  pm_update (m, now);
  if (now - p->last_draw > 40000000ULL) {
    vga_update (m);
    p->last_draw = host_now_ns ();
  }
}

uint64_t
pc_next_event (machine_t *m)
{
  uint64_t a = apic_next_event (m), r = rtc_next_event (m);

  return a < r ? a : r;
}

/* ------------------------------------------------------------ reset ---- */
static void
chipset_reset (machine_t *m)
{
  pc_t    *p = m->pc;
  uint64_t ext;

  p->isa.slot = 1; p->isa.fn = 0;
  pci_init_config (&p->isa, 0x8086, 0x7000, 0x06010000, 0x1af4, 0x1100);
  p->isa.cfg[0x04] = 0x07;
  p->isa.cfg[0x0e] = 0x80;                      /* multifunction */
  p->isa.cfg[0x60] = p->isa.cfg[0x61] = p->isa.cfg[0x62] = p->isa.cfg[0x63] = 0x80;

  p->pm.slot = 1; p->pm.fn = 3;
  pci_init_config (&p->pm, 0x8086, 0x7113, 0x06800003, 0x1af4, 0x1100);
  p->pm.cfg[0x04] = 0x00;
  p->pm.cfg[0x06] = 0x80; p->pm.cfg[0x07] = 0x02;
  p->pm.cfg[0x3c] = 9;  p->pm.cfg[0x3d] = 1;
  p->pm.cfg[0x40] = 0x01;

  p->pm1_sts = p->pm1_en = p->pm1_cnt = 0;
  p->sci     = 0;
  p->port92  = 0;
  p->fw_sel  = 0;
  p->fw_off  = 0;
  memset (p->intx, 0, sizeof (p->intx));

  /* memory size for the BIOS */
  rtc_set_cmos (m, 0x10, 0x00);                 /* no floppy */
  rtc_set_cmos (m, 0x14, 0x06);                 /* FPU, PS/2 mouse */
  rtc_set_cmos (m, 0x15, 0x80); rtc_set_cmos (m, 0x16, 0x02);     /* 640 K */
  ext = (m->ram_size >> 10) - 1024;
  if (ext > 0xffff) {
    ext = 0xffff;
  }

  rtc_set_cmos (m, 0x17, (uint8_t)ext); rtc_set_cmos (m, 0x18, (uint8_t)(ext >> 8));
  rtc_set_cmos (m, 0x30, (uint8_t)ext); rtc_set_cmos (m, 0x31, (uint8_t)(ext >> 8));
  ext = m->ram_size > (16u << 20) ? (m->ram_size - (16u << 20)) >> 16 : 0;
  rtc_set_cmos (m, 0x34, (uint8_t)ext); rtc_set_cmos (m, 0x35, (uint8_t)(ext >> 8));
  rtc_set_cmos (m, 0x5b, 0); rtc_set_cmos (m, 0x5c, 0); rtc_set_cmos (m, 0x5d, 0);
  rtc_set_cmos (m, 0x5f, 0);                    /* one CPU */
}

void
pc_reset (machine_t *m)
{
  pc_t  *p    = m->pc;
  size_t low  = p->bios_size > 0x20000 ? 0x20000 : p->bios_size;
  int    i;

  /* devices */
  pic_init (m);
  pit_init (m);
  i8042_init (m);
  rtc_init (m);
  apic_reset (m);
  chipset_reset (m);
  ide_reset (m);
  vga_reset (m);
  for (i = 0; i < m->npci; i++) {
    if (m->pci[i]->slot == 0) {
      memset (m->pci[i]->cfg + 0x40, 0, 0xc0);  /* i440FX: PAM, SMRAM */
    }
  }

  m->pic_out       = 0;
  m->cpu_irq_level = 0;
  m->reset_request = 0;
  uc_x86_set_irq_handler (m->uc, pc_irq_ack, m);
  uc_x86_set_irq_line (m->uc, 0);

  /* the BIOS: its last 128 K below 1 MiB, all of it below 4 GiB */
  memcpy (m->ram + 0x100000 - low, p->bios + p->bios_size - low, low);
  uc_x86_invalidate_host (m->uc, m->ram + 0xc0000, 0x40000);
  uc_x86_reset_real (m->uc);
}

int
pc_setup (machine_t *m, const uint8_t *bios, size_t bios_size,
          const uint8_t *vgabios, size_t vgabios_size)
{
  pc_t *p;

  if (!m->pc_bios || bios_size == 0 || (bios_size & 0xffff) || bios_size > 0x400000) {
    host_log ("x64e: bad BIOS image\n");
    return -1;
  }

  p = m->pc      = host_alloc (sizeof (pc_t));
  p->bios        = bios;
  p->bios_size   = bios_size;
  p->vgabios     = vgabios;
  p->vgabios_size = vgabios_size;
  p->bios_high   = host_alloc (bios_size);
  if (!p->bios_high) {
    return -1;
  }

  memcpy (p->bios_high, bios, bios_size);
  if (uc_mem_map_ptr (m->uc, 0x100000000ULL - bios_size, bios_size, UC_PROT_READ | UC_PROT_EXEC, p->bios_high)) {
    host_log ("x64e: cannot map the BIOS\n");
    return -1;
  }

  p->fw_dir[3] = FW_FILES;
  fw_dir_add (p, 0, "pci1234,1111.rom", (uint32_t)vgabios_size);
  fw_dir_add (p, 1, "bootorder", sizeof (boot_order) - 1);

  rtc_init (m);
  apic_init (m);
  chipset_reset (m);
  pci_register (m, &p->isa);
  ide_init (m);
  pci_register (m, &p->pm);
  vga_init (m);
  pc_reset (m);
  return 0;
}
