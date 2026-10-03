/*
 * MC146818 real-time clock and CMOS memory (0x70/0x71, IRQ 8) for the PC
 * BIOS machine: time of day, periodic / alarm / update interrupts.
 * Windows takes its clock tick from the periodic interrupt.
 */
#include <string.h>
#include "pc.h"

#define REG_A 0x0a
#define REG_B 0x0b
#define REG_C 0x0c
#define REG_D 0x0d
#define B_SET 0x80
#define B_PIE 0x40
#define B_AIE 0x20
#define B_UIE 0x10
#define B_DM  0x04
#define B_24H 0x02
#define C_IRQF 0x80
#define C_PF   0x40
#define C_AF   0x20
#define C_UF   0x10

typedef struct {
  uint8_t  index;
  uint8_t  cmos[128];
  int64_t  offset_s;            /* guest clock minus host clock, seconds */
  uint64_t next_periodic;       /* ns, 0: off */
  uint64_t period;
  uint64_t last_second;         /* guest second already reported */
  int      irq;
} rtc_t;

/* seconds since 1970 */
static int64_t
rtc_seconds (machine_t *m, rtc_t *r, uint64_t now)
{
  return (int64_t)((m->boot_ns + now) / 1000000000ULL) + r->offset_s;
}

static void
civil (int64_t days, int *y, int *mo, int *d)
{
  int64_t  z   = days + 719468;
  int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp  = (5 * doy + 2) / 153;

  *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
  *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y  = (int)(yoe + era * 400 + (*mo <= 2));
}

static int64_t
days_from_civil (int y, int mo, int d)
{
  int64_t  era;
  unsigned yoe, doy, doe;

  y  -= mo <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static uint8_t
to_reg (rtc_t *r, int v)
{
  return (r->cmos[REG_B] & B_DM) ? (uint8_t)v : (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int
from_reg (rtc_t *r, uint8_t v)
{
  return (r->cmos[REG_B] & B_DM) ? v : (v >> 4) * 10 + (v & 15);
}

/* fill the time registers from the clock */
static void
rtc_load (machine_t *m, rtc_t *r, uint64_t now)
{
  int64_t t = rtc_seconds (m, r, now);
  int64_t days = t / 86400;
  int     rem = (int)(t % 86400), y, mo, d, h;

  civil (days, &y, &mo, &d);
  h           = rem / 3600;
  r->cmos[0]  = to_reg (r, rem % 60);
  r->cmos[2]  = to_reg (r, (rem / 60) % 60);
  if (r->cmos[REG_B] & B_24H) {
    r->cmos[4] = to_reg (r, h);
  } else {
    r->cmos[4] = (uint8_t)(to_reg (r, (h % 12) ? (h % 12) : 12) | (h >= 12 ? 0x80 : 0));
  }

  r->cmos[6]    = (uint8_t)((days + 4) % 7 + 1);
  r->cmos[7]    = to_reg (r, d);
  r->cmos[8]    = to_reg (r, mo);
  r->cmos[9]    = to_reg (r, y % 100);
  r->cmos[0x32] = to_reg (r, y / 100);
}

/* the guest wrote the time registers: keep the difference to the host clock */
static void
rtc_store (machine_t *m, rtc_t *r, uint64_t now)
{
  int     h = r->cmos[4], y;
  int64_t t;

  if (r->cmos[REG_B] & B_24H) {
    h = from_reg (r, (uint8_t)h);
  } else {
    h = from_reg (r, (uint8_t)(h & 0x7f)) % 12 + ((h & 0x80) ? 12 : 0);
  }

  y = from_reg (r, r->cmos[0x32]) * 100 + from_reg (r, r->cmos[9]);
  if (y < 1980 || y > 2099) {
    y = 2000 + from_reg (r, r->cmos[9]);
  }

  t = days_from_civil (y, from_reg (r, r->cmos[8]), from_reg (r, r->cmos[7])) * 86400 +
      h * 3600 + from_reg (r, r->cmos[2]) * 60 + from_reg (r, r->cmos[0]);
  r->offset_s += t - rtc_seconds (m, r, now);
}

static void
rtc_irq (machine_t *m, rtc_t *r)
{
  int level = (r->cmos[REG_C] & r->cmos[REG_B] & (C_PF | C_AF | C_UF)) != 0;

  if (level) {
    r->cmos[REG_C] |= C_IRQF;
  } else {
    r->cmos[REG_C] &= ~C_IRQF;
  }

  if (level != r->irq) {
    r->irq = level;
    pic_set_irq (m, 8, level);
  }
}

static void
rtc_set_period (rtc_t *r, uint64_t now)
{
  unsigned rate = r->cmos[REG_A] & 15;

  /* the periodic flag is also polled without PIE, so it always runs */
  if (rate == 0 || (r->cmos[REG_A] & 0x70) != 0x20) {
    r->next_periodic = 0;
    return;
  }

  if (rate <= 2) {
    rate += 7;
  }

  r->period = 1000000000ULL * (1u << (rate - 1)) / 32768;
  if (r->next_periodic == 0 || r->next_periodic > now + r->period) {
    r->next_periodic = now + r->period;
  }
}

void
rtc_tick (machine_t *m, uint64_t now)
{
  rtc_t   *r = m->rtc;
  uint64_t sec;

  if (!r) {
    return;
  }

  if (r->next_periodic && now >= r->next_periodic) {
    /* one tick per call; a guest that fell behind gets the ticks it missed
       (it counts them to keep time), up to a second's worth */
    if (!(r->cmos[REG_C] & C_PF) || !(r->cmos[REG_B] & B_PIE)) {
      r->cmos[REG_C]   |= C_PF;
      r->next_periodic += r->period;
      if (r->next_periodic + 1000000000ULL < now) {
        r->next_periodic = now;
      }
    }
  }

  sec = (uint64_t)rtc_seconds (m, r, now);
  if (sec != r->last_second && !(r->cmos[REG_B] & B_SET)) {
    r->last_second  = sec;
    r->cmos[REG_C] |= C_UF;
    rtc_load (m, r, now);
    if (r->cmos[1] == r->cmos[0] && r->cmos[3] == r->cmos[2] && r->cmos[5] == r->cmos[4]) {
      r->cmos[REG_C] |= C_AF;
    } else if ((r->cmos[1] & 0xc0) == 0xc0 && (r->cmos[3] & 0xc0) == 0xc0 && (r->cmos[5] & 0xc0) == 0xc0) {
      r->cmos[REG_C] |= C_AF;
    }
  }

  rtc_irq (m, r);
}

uint64_t
rtc_next_event (machine_t *m)
{
  rtc_t *r = m->rtc;

  if (!r) {
    return ~0ULL;
  }

  if (r->next_periodic && (r->cmos[REG_B] & B_PIE)) {
    return r->next_periodic;
  }

  if (r->cmos[REG_B] & (B_UIE | B_AIE)) {
    return (host_now_ns () / 1000000000ULL + 1) * 1000000000ULL;
  }

  return ~0ULL;
}

int
rtc_io_read (machine_t *m, uint16_t port, int size, uint32_t *val)
{
  rtc_t   *r = m->rtc;
  unsigned i;
  uint64_t now;

  (void)size;
  if (port != 0x70 && port != 0x71) {
    return 0;
  }

  if (port == 0x70) {
    *val = 0xff;
    return 1;
  }

  i   = r->index & 0x7f;
  now = host_now_ns ();
  if (i <= 9 || i == 0x32) {
    if (!(r->cmos[REG_B] & B_SET) && i != 1 && i != 3 && i != 5) {
      rtc_load (m, r, now);
    }

    *val = r->cmos[i];
  } else if (i == REG_A) {
    /* update in progress during the last 2 ms of a second */
    *val = (r->cmos[REG_A] & 0x7f) | (((m->boot_ns + now) % 1000000000ULL) > 998000000ULL ? 0x80 : 0);
  } else if (i == REG_C) {
    rtc_tick (m, now);
    *val           = r->cmos[REG_C];
    r->cmos[REG_C] = 0;
    rtc_irq (m, r);
  } else if (i == REG_D) {
    *val = 0x80;
  } else {
    *val = r->cmos[i];
  }

  return 1;
}

int
rtc_io_write (machine_t *m, uint16_t port, int size, uint32_t val)
{
  rtc_t   *r = m->rtc;
  unsigned i;
  uint64_t now;

  (void)size;
  if (port != 0x70 && port != 0x71) {
    return 0;
  }

  if (port == 0x70) {
    r->index = (uint8_t)val;
    return 1;
  }

  i   = r->index & 0x7f;
  now = host_now_ns ();
  switch (i) {
    case 0: case 2: case 4: case 6: case 7: case 8: case 9: case 0x32:
      if (!(r->cmos[REG_B] & B_SET)) {
        rtc_load (m, r, now);
      }

      r->cmos[i] = (uint8_t)val;
      if (!(r->cmos[REG_B] & B_SET)) {
        rtc_store (m, r, now);
      }

      break;
    case REG_A:
      r->cmos[REG_A] = (uint8_t)val & 0x7f;
      rtc_set_period (r, now);
      break;
    case REG_B:
      if ((val & B_SET) && !(r->cmos[REG_B] & B_SET)) {
        rtc_load (m, r, now);               /* freeze */
      }

      if (!(val & B_SET) && (r->cmos[REG_B] & B_SET)) {
        uint8_t old = r->cmos[REG_B];

        r->cmos[REG_B] = (uint8_t)val;      /* formats as now selected */
        rtc_store (m, r, now);
        r->cmos[REG_B] = old;
      } else if ((val ^ r->cmos[REG_B]) & (B_DM | B_24H)) {
        r->cmos[REG_B] = (uint8_t)val;
        rtc_load (m, r, now);
      }

      r->cmos[REG_B] = (uint8_t)val;
      rtc_irq (m, r);
      break;
    case REG_C: case REG_D:
      break;
    default:
      r->cmos[i] = (uint8_t)val;
      break;
  }

  return 1;
}

void
rtc_set_cmos (machine_t *m, unsigned index, uint8_t val)
{
  ((rtc_t *)m->rtc)->cmos[index & 0x7f] = val;
}

void
rtc_init (machine_t *m)
{
  rtc_t *r = m->rtc;

  if (!r) {
    r = m->rtc = host_alloc (sizeof (rtc_t));
  }

  r->index       = 0;
  r->cmos[REG_A] = 0x26;
  r->cmos[REG_B] = B_24H;
  r->cmos[REG_C] = 0;
  r->irq         = 0;
  rtc_set_period (r, host_now_ns ());
}
