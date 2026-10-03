/*
 * Local APIC (0xFEE00000) and IO-APIC (0xFEC00000) for the PC BIOS machine.
 * One CPU. The APIC timer counts nanoseconds (a 1 GHz bus clock).
 */
#include <string.h>
#include "pc.h"

#define APIC_BASE     0xfee00000ULL
#define IOAPIC_BASE   0xfec00000ULL
#define IOAPIC_PINS   24

#define LVT_TIMER  0
#define LVT_LINT0  3
#define LVT_LINT1  4
#define LVT_MASKED 0x10000u

typedef struct {
  uint64_t base_msr;
  uint32_t id, tpr, ldr, dfr, svr, esr;
  uint32_t isr[8], tmr[8], irr[8];
  uint32_t lvt[6];
  uint32_t icr[2];
  uint32_t divide, initial_count;
  uint64_t count_start;                 /* ns */
  uint64_t next_timer;                  /* ns, 0: not armed */
  /* IO-APIC */
  uint32_t io_sel, io_id;
  uint64_t redir[IOAPIC_PINS];
  uint32_t pin_level;                   /* bit per pin */
} apic_t;

#define REDIR_MASKED   (1ULL << 16)
#define REDIR_LEVEL    (1ULL << 15)
#define REDIR_REMOTE   (1ULL << 14)

static inline void bit_set (uint32_t *a, int v)   { a[v >> 5] |= 1u << (v & 31); }
static inline void bit_clr (uint32_t *a, int v)   { a[v >> 5] &= ~(1u << (v & 31)); }
static inline int  bit_get (uint32_t *a, int v)   { return (a[v >> 5] >> (v & 31)) & 1; }

static int
highest (uint32_t *a)
{
  int i;

  for (i = 7; i >= 0; i--) {
    if (a[i]) {
      return i * 32 + 31 - __builtin_clz (a[i]);
    }
  }

  return -1;
}

static int
apic_enabled (apic_t *a)
{
  return (a->base_msr & 0x800) && (a->svr & 0x100);
}

static uint32_t
apic_ppr (apic_t *a)
{
  int isrv = highest (a->isr);

  if (isrv < 0) {
    isrv = 0;
  }

  return ((a->tpr & 0xf0) >= (uint32_t)(isrv & 0xf0)) ? a->tpr : (uint32_t)(isrv & 0xf0);
}

int
apic_has_interrupt (machine_t *m)
{
  apic_t *a = m->apic;
  int     v;

  if (!apic_enabled (a)) {
    return 0;
  }

  v = highest (a->irr);
  return v >= 16 && (uint32_t)(v & 0xf0) > (apic_ppr (a) & 0xf0);
}

int
apic_ack_interrupt (machine_t *m)
{
  apic_t *a = m->apic;
  int     v = highest (a->irr);

  bit_clr (a->irr, v);
  bit_set (a->isr, v);
  return v;
}

int
apic_accepts_pic (machine_t *m)
{
  apic_t *a = m->apic;

  if (!apic_enabled (a)) {
    return 1;
  }

  /* virtual wire: LINT0 as ExtINT, or IO-APIC pin 0 as ExtINT */
  if (!(a->lvt[LVT_LINT0] & LVT_MASKED) && ((a->lvt[LVT_LINT0] >> 8) & 7) == 7) {
    return 1;
  }

  return !(a->redir[0] & REDIR_MASKED) && ((a->redir[0] >> 8) & 7) == 7;
}

static void
apic_set_irq (machine_t *m, int vector, int level_triggered)
{
  apic_t *a = m->apic;

  if (vector < 16) {
    a->esr |= 0x40;
    return;
  }

  bit_set (a->irr, vector);
  if (level_triggered) {
    bit_set (a->tmr, vector);
  } else {
    bit_clr (a->tmr, vector);
  }

  cpu_irq_update (m);
}

/* ------------------------------------------------------------ IO-APIC -- */
static void
ioapic_service (machine_t *m, int pin)
{
  apic_t  *a = m->apic;
  uint64_t e = a->redir[pin];
  int      mode = (int)((e >> 8) & 7);

  if (e & REDIR_MASKED) {
    return;
  }

  if (e & REDIR_LEVEL) {
    if (e & REDIR_REMOTE) {
      return;
    }

    if (mode <= 1) {
      a->redir[pin] |= REDIR_REMOTE;
    }
  }

  if (mode <= 1) {                    /* fixed, lowest priority */
    apic_set_irq (m, (int)(e & 0xff), (e & REDIR_LEVEL) != 0);
  } else {
    cpu_irq_update (m);               /* ExtINT */
  }
}

void
ioapic_set_irq (machine_t *m, int pin, int level)
{
  apic_t  *a = m->apic;
  uint32_t bit = 1u << pin;
  int      was;

  if (!a || pin >= IOAPIC_PINS) {
    return;
  }

  was = (a->pin_level & bit) != 0;
  if (level) {
    a->pin_level |= bit;
    if ((a->redir[pin] & REDIR_LEVEL) || !was) {
      ioapic_service (m, pin);
    }
  } else {
    a->pin_level &= ~bit;
  }
}

static void
ioapic_eoi (machine_t *m, int vector)
{
  apic_t *a = m->apic;
  int     i;

  for (i = 0; i < IOAPIC_PINS; i++) {
    if ((a->redir[i] & REDIR_REMOTE) && (int)(a->redir[i] & 0xff) == vector) {
      a->redir[i] &= ~REDIR_REMOTE;
      if (a->pin_level & (1u << i)) {
        ioapic_service (m, i);
      }
    }
  }
}

static uint64_t
ioapic_read (uc_engine *uc, uint64_t off, unsigned size, void *opaque)
{
  machine_t *m = opaque;
  apic_t    *a = m->apic;
  unsigned   r;

  (void)uc; (void)size;
  if ((off & 0xff) == 0x00) {
    return a->io_sel;
  }

  if ((off & 0xff) != 0x10) {
    return 0;
  }

  r = a->io_sel;
  if (r == 0) {
    return a->io_id << 24;
  }

  if (r == 1) {
    return 0x11 | ((IOAPIC_PINS - 1) << 16);
  }

  if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
    uint64_t e = a->redir[(r - 0x10) >> 1];

    return (r & 1) ? (uint32_t)(e >> 32) : (uint32_t)e;
  }

  return 0;
}

static void
ioapic_write (uc_engine *uc, uint64_t off, unsigned size, uint64_t val, void *opaque)
{
  machine_t *m = opaque;
  apic_t    *a = m->apic;
  unsigned   r;

  (void)uc; (void)size;
  if ((off & 0xff) == 0x00) {
    a->io_sel = (uint32_t)val & 0xff;
    return;
  }

  if ((off & 0xff) == 0x40) {         /* EOI register (version 0x20); harmless */
    ioapic_eoi (m, (int)(val & 0xff));
    return;
  }

  if ((off & 0xff) != 0x10) {
    return;
  }

  r = a->io_sel;
  if (r == 0) {
    a->io_id = ((uint32_t)val >> 24) & 0xf;
  } else if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
    int       pin = (int)((r - 0x10) >> 1);
    uint64_t *e   = &a->redir[pin];

    if (r & 1) {
      *e = (*e & 0xffffffffULL) | (val << 32);
    } else {
      /* remote IRR and delivery status are read-only */
      *e = (*e & 0xffffffff00005000ULL) | ((uint32_t)val & ~0x5000u);
      if (!(*e & REDIR_LEVEL)) {
        *e &= ~REDIR_REMOTE;
      }
    }

    if ((*e & REDIR_LEVEL) && (a->pin_level & (1u << pin))) {
      ioapic_service (m, pin);
    }

    cpu_irq_update (m);
  }
}

/* --------------------------------------------------------- local APIC -- */
static unsigned
timer_shift (apic_t *a)
{
  unsigned v = ((a->divide & 3) | ((a->divide >> 1) & 4)) + 1;

  return v & 7;                         /* 7 + 1 -> divide by 1 */
}

static void
timer_arm (apic_t *a, uint64_t now)
{
  a->count_start = now;
  a->next_timer  = a->initial_count ? now + ((uint64_t)a->initial_count << timer_shift (a)) : 0;
  if (a->next_timer == 0 && a->initial_count) {
    a->next_timer = 1;
  }
}

static uint32_t
timer_count (apic_t *a, uint64_t now)
{
  uint64_t d;

  if (a->initial_count == 0) {
    return 0;
  }

  d = (now - a->count_start) >> timer_shift (a);
  if (a->lvt[LVT_TIMER] & 0x20000) {
    return a->initial_count - (uint32_t)(d % ((uint64_t)a->initial_count + 1));
  }

  return d >= a->initial_count ? 0 : a->initial_count - (uint32_t)d;
}

void
apic_tick (machine_t *m, uint64_t now)
{
  apic_t *a = m->apic;

  if (!a || a->next_timer == 0 || now < a->next_timer) {
    return;
  }

  if (a->lvt[LVT_TIMER] & 0x20000) {
    uint64_t period = ((uint64_t)a->initial_count + 1) << timer_shift (a);

    a->next_timer += period;
    if (a->next_timer <= now) {         /* far behind: do not flood */
      a->next_timer = now + period;
    }
  } else {
    a->next_timer = 0;
  }

  if (!(a->lvt[LVT_TIMER] & LVT_MASKED) && apic_enabled (a)) {
    apic_set_irq (m, (int)(a->lvt[LVT_TIMER] & 0xff), 0);
  }
}

uint64_t
apic_next_event (machine_t *m)
{
  apic_t *a = m->apic;

  return (a && a->next_timer) ? a->next_timer : ~0ULL;
}

static uint64_t
apic_read (uc_engine *uc, uint64_t off, unsigned size, void *opaque)
{
  machine_t *m = opaque;
  apic_t    *a = m->apic;
  unsigned   r = (unsigned)(off >> 4) & 0xff;
  uint32_t   v = 0;

  (void)uc;
  switch (r) {
    case 0x02: v = a->id << 24; break;
    case 0x03: v = 0x00050014; break;
    case 0x08: v = a->tpr; break;
    case 0x09: v = 0; break;
    case 0x0a: v = apic_ppr (a); break;
    case 0x0d: v = a->ldr; break;
    case 0x0e: v = a->dfr; break;
    case 0x0f: v = a->svr; break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
      v = a->isr[r & 7]; break;
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
      v = a->tmr[r & 7]; break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
      v = a->irr[r & 7]; break;
    case 0x28: v = a->esr; break;
    case 0x30: v = a->icr[0]; break;
    case 0x31: v = a->icr[1]; break;
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
      v = a->lvt[r - 0x32]; break;
    case 0x38: v = a->initial_count; break;
    case 0x39: v = timer_count (a, host_now_ns ()); break;
    case 0x3e: v = a->divide; break;
    default: break;
  }

  v >>= 8 * (off & 3);
  return size == 1 ? (v & 0xff) : size == 2 ? (v & 0xffff) : v;
}

static void
apic_write (uc_engine *uc, uint64_t off, unsigned size, uint64_t val64, void *opaque)
{
  machine_t *m = opaque;
  apic_t    *a = m->apic;
  unsigned   r = (unsigned)(off >> 4) & 0xff;
  uint32_t   val = (uint32_t)val64;
  int        v;

  (void)uc; (void)size;
  switch (r) {
    case 0x02: a->id = val >> 24; break;
    case 0x08:
      a->tpr = val & 0xff;
      cpu_irq_update (m);
      break;
    case 0x0b:                          /* EOI */
      v = highest (a->isr);
      if (v >= 0) {
        bit_clr (a->isr, v);
        if (bit_get (a->tmr, v)) {
          ioapic_eoi (m, v);
        }
      }

      cpu_irq_update (m);
      break;
    case 0x0d: a->ldr = val & 0xff000000u; break;
    case 0x0e: a->dfr = val | 0x0fffffffu; break;
    case 0x0f:
      a->svr = val & 0x1ff;
      if (!(a->svr & 0x100)) {
        for (v = 0; v < 6; v++) {
          a->lvt[v] |= LVT_MASKED;
        }
      }

      cpu_irq_update (m);
      break;
    case 0x28: a->esr = 0; break;
    case 0x30:
      a->icr[0] = val & ~0x1000u;       /* delivered at once */
      /* fixed interrupts to ourselves; INIT / SIPI have no other CPU to go to */
      if (((val >> 8) & 7) == 0) {
        unsigned sh = (val >> 18) & 3, dest = a->icr[1] >> 24;

        if (sh == 1 || sh == 2 || (sh == 0 && (dest == a->id || dest == 0xff ||
                                               ((val & 0x800) && (dest & (a->ldr >> 24)))))) {
          apic_set_irq (m, (int)(val & 0xff), 0);
        }
      }

      break;
    case 0x31: a->icr[1] = val; break;
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
      if (!(a->svr & 0x100)) {
        val |= LVT_MASKED;
      }

      a->lvt[r - 0x32] = val;
      if (r == 0x32) {
        /* mode change keeps the running count */
      }

      cpu_irq_update (m);
      break;
    case 0x38:
      a->initial_count = val;
      timer_arm (a, host_now_ns ());
      break;
    case 0x3e:
      a->divide = val & 0xb;
      if (a->initial_count) {
        uint32_t cur = timer_count (a, host_now_ns ());
        uint32_t ini = a->initial_count;

        (void)cur;
        a->initial_count = ini;
        timer_arm (a, host_now_ns ());
      }

      break;
    default: break;
  }
}

/* CR8 and the APIC base MSR, from the CPU core */
static uint64_t
apic_cpu_get (void *opaque, int what)
{
  machine_t *m = opaque;
  apic_t    *a = m->apic;

  return what == 0 ? (a->tpr >> 4) : a->base_msr;
}

static void
apic_cpu_set (void *opaque, int what, uint64_t val)
{
  machine_t *m = opaque;
  apic_t    *a = m->apic;

  if (what == 0) {
    a->tpr = (uint32_t)(val & 0xf) << 4;
  } else {
    /* the address cannot move; the enable bit can */
    a->base_msr = (val & 0x800) | APIC_BASE | 0x100;
  }

  cpu_irq_update (m);
}

void
apic_reset (machine_t *m)
{
  apic_t *a = m->apic;
  int     i;

  memset (a, 0, sizeof (*a));
  a->base_msr = APIC_BASE | 0x800 | 0x100;    /* enabled, boot CPU */
  a->svr      = 0xff;
  a->dfr      = 0xffffffffu;
  for (i = 0; i < 6; i++) {
    a->lvt[i] = LVT_MASKED;
  }

  for (i = 0; i < IOAPIC_PINS; i++) {
    a->redir[i] = REDIR_MASKED;
  }
}

void
apic_init (machine_t *m)
{
  m->apic = host_alloc (sizeof (apic_t));
  apic_reset (m);
  uc_mmio_map (m->uc, APIC_BASE, 0x1000, apic_read, m, apic_write, m);
  uc_mmio_map (m->uc, IOAPIC_BASE, 0x1000, ioapic_read, m, ioapic_write, m);
  uc_x86_set_apic_hooks (m->uc, apic_cpu_get, apic_cpu_set, m);
}
