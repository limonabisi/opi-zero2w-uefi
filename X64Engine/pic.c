/* Intel 8259A pair (master 0x20/0x21, slave 0xA0/0xA1), ELCR 0x4D0/0x4D1. */
#include "x64e.h"
#include "pc.h"

static int
prio_highest (pic_t *p, uint8_t mask)
{
  int prio;

  if (mask == 0) {
    return 8;
  }

  prio = 0;
  while ((mask & (1 << ((prio + p->lowest_prio) & 7))) == 0) {
    prio++;
  }

  return prio;
}

/* IRQ (0..7) the chip would deliver now, or -1 */
static int
pic_get_irq (pic_t *p, int is_master)
{
  int     prio, cur;
  uint8_t mask;

  mask = p->irr & ~p->imr;
  prio = prio_highest (p, mask);
  if (prio == 8) {
    return -1;
  }

  mask = p->isr;
  if (p->special_mask) {
    mask &= ~p->imr;
  }

  if (p->special_fully_nested && is_master) {
    mask &= ~(1 << 2);
  }

  cur = prio_highest (p, mask);
  if (prio < cur) {
    return (prio + p->lowest_prio) & 7;
  }

  return -1;
}

static void
pic_update (machine_t *m)
{
  int irq;
  int level;

  /* slave output drives master IRQ2 */
  irq = pic_get_irq (&m->pic[1], 0);
  if (irq >= 0) {
    m->pic[0].irr |= 1 << 2;
  } else if (!(m->pic[0].last_level & (1 << 2))) {
    m->pic[0].irr &= ~(1 << 2);
  }

  irq   = pic_get_irq (&m->pic[0], 1);
  level = (irq >= 0);
  if (m->pc_bios) {
    /* the local APIC decides whether the CPU sees it */
    m->pic_out = level;
    cpu_irq_update (m);
    return;
  }

  if (level != m->cpu_irq_level) {
    m->cpu_irq_level = level;
    uc_x86_set_irq_line (m->uc, level);
  }
}

static void
pic_set_irq1 (pic_t *p, int irq, int level)
{
  uint8_t mask = 1 << irq;

  if (p->elcr & mask) {
    /* level triggered */
    if (level) {
      p->irr        |= mask;
      p->last_level |= mask;
    } else {
      p->irr        &= ~mask;
      p->last_level &= ~mask;
    }
  } else {
    /* edge triggered */
    if (level) {
      if ((p->last_level & mask) == 0) {
        p->irr |= mask;
      }

      p->last_level |= mask;
    } else {
      p->last_level &= ~mask;
    }
  }
}

void
pic_set_irq (machine_t *m, int irq, int level)
{
  pic_set_irq1 (&m->pic[irq >> 3], irq & 7, level);
  if (m->pc_bios) {
    ioapic_set_irq (m, irq == 0 ? 2 : irq, level);    /* IRQ0 is wired to IO-APIC pin 2 */
  }

  pic_update (m);
}

static void
pic_intack (pic_t *p, int irq)
{
  if (p->auto_eoi) {
    if (p->rotate_on_aeoi) {
      p->lowest_prio = (irq + 1) & 7;
    }
  } else {
    p->isr |= 1 << irq;
  }

  /* edge triggered: clear IRR on acknowledge */
  if (!(p->elcr & (1 << irq))) {
    p->irr &= ~(1 << irq);
  }
}

int
pic_ack (void *opaque)
{
  machine_t *m = opaque;
  int        irq, irq2, vec;

  irq = pic_get_irq (&m->pic[0], 1);
  if (irq < 0) {
    pic_update (m);
    return -1;
  }

  if (irq == 2) {
    irq2 = pic_get_irq (&m->pic[1], 0);
    if (irq2 >= 0) {
      pic_intack (&m->pic[1], irq2);
    } else {
      irq2 = 7;   /* spurious on slave */
    }

    vec = m->pic[1].base + irq2;
  } else {
    vec = m->pic[0].base + irq;
  }

  pic_intack (&m->pic[0], irq);
  m->irqs++;
  pic_update (m);
  return vec;
}

static void
pic_reset (pic_t *p)
{
  uint8_t elcr = p->elcr;

  *p      = (pic_t){ 0 };
  p->elcr = elcr;
}

static void
pic_write1 (machine_t *m, pic_t *p, int addr, uint8_t val)
{
  int prio, irq, cmd;

  if (addr == 0) {
    if (val & 0x10) {
      /* ICW1 */
      pic_reset (p);
      p->icw_step = 1;
      p->init4    = val & 1;
    } else if (val & 0x08) {
      /* OCW3 */
      if (val & 0x02) {
        p->read_isr = val & 1;
      }

      if (val & 0x40) {
        p->special_mask = (val >> 5) & 1;
      }
    } else {
      /* OCW2 */
      cmd = val >> 5;
      switch (cmd) {
        case 0:
        case 4:
          p->rotate_on_aeoi = cmd >> 2;
          break;
        case 1:   /* non-specific EOI */
        case 5:
          prio = prio_highest (p, p->isr);
          if (prio != 8) {
            irq      = (prio + p->lowest_prio) & 7;
            p->isr  &= ~(1 << irq);
            if (cmd == 5) {
              p->lowest_prio = (irq + 1) & 7;
            }
          }

          break;
        case 3:   /* specific EOI */
          irq     = val & 7;
          p->isr &= ~(1 << irq);
          break;
        case 6:
          p->lowest_prio = (val + 1) & 7;
          break;
        case 7:
          irq            = val & 7;
          p->isr        &= ~(1 << irq);
          p->lowest_prio = (irq + 1) & 7;
          break;
        default:
          break;
      }
    }
  } else {
    switch (p->icw_step) {
      case 0:   /* OCW1 */
        p->imr = val;
        break;
      case 1:
        p->base     = val & 0xf8;
        p->icw_step = 2;
        break;
      case 2:
        p->icw_step = p->init4 ? 3 : 0;
        break;
      case 3:
        p->special_fully_nested = (val >> 4) & 1;
        p->auto_eoi             = (val >> 1) & 1;
        p->icw_step             = 0;
        break;
    }
  }

  pic_update (m);
}

uint32_t
pic_io_read (machine_t *m, uint16_t port)
{
  pic_t *p;

  if (port == 0x4d0 || port == 0x4d1) {
    return m->pic[port & 1].elcr;
  }

  p = &m->pic[(port >> 7) & 1];
  if (port & 1) {
    return p->imr;
  }

  return p->read_isr ? p->isr : p->irr;
}

void
pic_io_write (machine_t *m, uint16_t port, uint8_t val)
{
  if (port == 0x4d0) {
    m->pic[0].elcr = val & 0xf8;
    return;
  }

  if (port == 0x4d1) {
    m->pic[1].elcr = val & 0xde;
    return;
  }

  pic_write1 (m, &m->pic[(port >> 7) & 1], port & 1, val);
}

void
pic_init (machine_t *m)
{
  pic_reset (&m->pic[0]);
  pic_reset (&m->pic[1]);
  uc_x86_set_irq_handler (m->uc, pic_ack, m);
}
