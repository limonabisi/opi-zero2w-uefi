/* Intel 8254 PIT (0x40-0x43) and port 0x61, driven by the host clock. */
#include "x64e.h"

#define PIT_HZ  1193182ULL

static uint64_t
ticks_since (pit_chan_t *c, uint64_t now)
{
  if (now <= c->start_ns) {
    return 0;
  }

  return (now - c->start_ns) * PIT_HZ / 1000000000ULL;
}

static uint64_t
ns_for_ticks (uint64_t t)
{
  return t * 1000000000ULL / PIT_HZ;
}

static uint32_t
reload_of (pit_chan_t *c)
{
  return c->reload ? c->reload : 0x10000;
}

static uint16_t
pit_count (pit_chan_t *c, uint64_t now)
{
  uint64_t d = ticks_since (c, now);
  uint32_t r = reload_of (c);

  switch (c->mode) {
    case 2:
      return (uint16_t)(r - (d % r));
    case 3:
      return (uint16_t)(r - ((d * 2) % r));
    default:            /* 0, 1, 4, 5: count down and wrap */
      return (uint16_t)(r - d);
  }
}

int
pit_out (machine_t *m, int ch, uint64_t now)
{
  pit_chan_t *c = &m->pit[ch];
  uint64_t    d = ticks_since (c, now);
  uint32_t    r = reload_of (c);

  if (!c->armed) {
    return c->mode == 0 ? 0 : 1;
  }

  switch (c->mode) {
    case 0:
      return d >= r;
    case 1:
      return d >= r;
    case 2:
      return (d % r) != (r - 1);
    case 3:
      return (d % r) < ((r + 1) / 2);
    case 4:
    case 5:
      return d != r;
    default:
      return 1;
  }
}

static void
pit_arm (machine_t *m, int ch, uint64_t now)
{
  pit_chan_t *c = &m->pit[ch];

  c->start_ns   = now;
  c->armed      = 1;
  c->null_count = 0;
  if (ch == 0) {
    c->next_irq_ns = now + ns_for_ticks (reload_of (c));
  }
}

void
pit_tick (machine_t *m, uint64_t now)
{
  pit_chan_t *c = &m->pit[0];
  uint64_t    period;

  if (!c->armed || c->next_irq_ns == 0 || now < c->next_irq_ns) {
    return;
  }

  pic_set_irq (m, 0, 1);
  pic_set_irq (m, 0, 0);

  if (c->mode == 2 || c->mode == 3) {
    period          = ns_for_ticks (reload_of (c));
    c->next_irq_ns += period;
    if (c->next_irq_ns <= now) {          /* fell behind: do not burst */
      c->next_irq_ns = now + period;
    }
  } else {
    c->next_irq_ns = 0;                   /* one-shot */
  }
}

uint64_t
pit_next_event (machine_t *m)
{
  pit_chan_t *c = &m->pit[0];

  if (!c->armed || c->next_irq_ns == 0) {
    return UINT64_MAX;
  }

  return c->next_irq_ns;
}

static void
pit_latch (machine_t *m, int ch, uint64_t now)
{
  pit_chan_t *c = &m->pit[ch];

  if (!c->latched) {
    c->latched        = 1;
    c->latch_value    = pit_count (c, now);
    c->latch_lsb_next = 1;
  }
}

void
pit_io_write (machine_t *m, uint16_t port, uint8_t val)
{
  uint64_t    now = host_now_ns ();
  pit_chan_t *c;
  int         ch, i;

  if (port == 0x61) {
    int old_gate = m->pit[2].gate;
    m->port61    = val & 0x0f;
    m->pit[2].gate = val & 1;
    if (!old_gate && m->pit[2].gate) {
      c = &m->pit[2];
      if (c->mode == 1 || c->mode == 2 || c->mode == 3 || c->mode == 5) {
        pit_arm (m, 2, now);
      }
    }

    return;
  }

  if (port == 0x43) {
    ch = val >> 6;
    if (ch == 3) {
      /* read-back */
      for (i = 0; i < 3; i++) {
        if (!(val & (2 << i))) {
          continue;
        }

        c = &m->pit[i];
        if (!(val & 0x20)) {
          pit_latch (m, i, now);
        }

        if (!(val & 0x10) && !c->status_latched) {
          c->status_latched = 1;
          c->status         = (pit_out (m, i, now) << 7) | (c->null_count << 6) |
                              (c->rw << 4) | (c->mode << 1) | c->bcd;
        }
      }

      return;
    }

    c = &m->pit[ch];
    if (((val >> 4) & 3) == 0) {
      pit_latch (m, ch, now);
      return;
    }

    c->rw             = (val >> 4) & 3;
    c->mode           = (val >> 1) & 7;
    if (c->mode > 5) {
      c->mode -= 4;
    }

    c->bcd            = val & 1;
    c->write_lsb_next = 1;
    c->read_lsb_next  = 1;
    c->latched        = 0;
    c->null_count     = 1;
    c->armed          = 0;
    if (ch == 0) {
      c->next_irq_ns = 0;
    }

    return;
  }

  ch = port & 3;
  c  = &m->pit[ch];
  switch (c->rw) {
    case 1:
      c->reload = val;
      pit_arm (m, ch, now);
      break;
    case 2:
      c->reload = val << 8;
      pit_arm (m, ch, now);
      break;
    case 3:
      if (c->write_lsb_next) {
        c->reload         = (c->reload & 0xff00) | val;
        c->write_lsb_next = 0;
        if (c->mode == 0) {
          c->armed = 0;     /* counting stops while a new count is written */
        }
      } else {
        c->reload         = (c->reload & 0x00ff) | (val << 8);
        c->write_lsb_next = 1;
        pit_arm (m, ch, now);
      }

      break;
  }
}

uint32_t
pit_io_read (machine_t *m, uint16_t port)
{
  uint64_t    now = host_now_ns ();
  pit_chan_t *c;
  uint16_t    v;
  uint8_t     r;

  if (port == 0x61) {
    /* bit 4: refresh toggles every ~15 us, bit 5: OUT2 */
    r = m->port61 & 0x0f;
    if ((now / 15000) & 1) {
      r |= 0x10;
    }

    if (pit_out (m, 2, now)) {
      r |= 0x20;
    }

    return r;
  }

  if (port == 0x43) {
    return 0xff;
  }

  c = &m->pit[port & 3];
  if (c->status_latched) {
    c->status_latched = 0;
    return c->status;
  }

  if (c->latched) {
    v = c->latch_value;
    if (c->rw == 3) {
      if (c->latch_lsb_next) {
        c->latch_lsb_next = 0;
        return v & 0xff;
      }

      c->latched = 0;
      return v >> 8;
    }

    c->latched = 0;
    return c->rw == 2 ? v >> 8 : v & 0xff;
  }

  v = c->armed ? pit_count (c, now) : c->reload;
  switch (c->rw) {
    case 1:
      return v & 0xff;
    case 2:
      return v >> 8;
    default:
      if (c->read_lsb_next) {
        c->read_lsb_next = 0;
        return v & 0xff;
      }

      c->read_lsb_next = 1;
      return v >> 8;
  }
}

void
pit_init (machine_t *m)
{
  int i;

  for (i = 0; i < 3; i++) {
    m->pit[i]      = (pit_chan_t){ 0 };
    m->pit[i].mode = 3;
    m->pit[i].rw   = 3;
    m->pit[i].write_lsb_next = 1;
    m->pit[i].read_lsb_next  = 1;
    m->pit[i].gate = (i != 2);
  }
}
