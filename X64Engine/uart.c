/* 16550A UART at 0x3F8 (COM1, IRQ4) and MC146818 RTC/CMOS at 0x70/0x71. */
#include "x64e.h"

#define LSR_DR    0x01
#define LSR_THRE  0x20
#define LSR_TEMT  0x40

static int
rx_count (uart_t *u)
{
  return (uint16_t)(u->rx_head - u->rx_tail);
}

static void
uart_update_irq (machine_t *m)
{
  uart_t *u = &m->uart;
  int     level;

  level = ((u->ier & 1) && rx_count (u)) || ((u->ier & 2) && u->thre_pending);
  /* OUT2 in MCR gates the interrupt line on PCs */
  if (!(u->mcr & 0x08)) {
    level = 0;
  }

  if (level != u->irq_level) {
    u->irq_level = level;
    pic_set_irq (m, 4, level);
  }
}

static void
kbd_poll_input (machine_t *m)
{
  static int esc;              /* 0, 1 = ESC seen, 2 = ESC [ seen */
  int        c;

  while ((c = host_console_read ()) >= 0) {
    if (esc == 1) {
      if (c == '[') {
        esc = 2;
        continue;
      }

      kbd_type_char (m, 27);
      esc = 0;
    } else if (esc == 2) {
      esc = 0;
      switch (c) {
        case 'A': kbd_type_key (m, KEY_UP); continue;
        case 'B': kbd_type_key (m, KEY_DOWN); continue;
        case 'C': kbd_type_key (m, KEY_RIGHT); continue;
        case 'D': kbd_type_key (m, KEY_LEFT); continue;
        case 'H': kbd_type_key (m, KEY_HOME); continue;
        case 'F': kbd_type_key (m, KEY_END); continue;
        default: continue;
      }
    }

    if (c == 27) {
      esc = 1;
      continue;
    }

    kbd_type_char (m, c);
  }
}

void
uart_poll_input (machine_t *m)
{
  uart_t *u = &m->uart;
  int     c;

  if (m->console_to_kbd) {
    kbd_poll_input (m);
    return;
  }

  while (rx_count (u) < (int)sizeof (u->rx) && (c = host_console_read ()) >= 0) {
    u->rx[u->rx_head++ % sizeof (u->rx)] = (uint8_t)c;
  }

  uart_update_irq (m);
}

uint32_t
uart_io_read (machine_t *m, uint16_t port)
{
  uart_t  *u = &m->uart;
  uint32_t r = 0;

  switch (port & 7) {
    case 0:
      if (u->lcr & 0x80) {
        return u->dll;
      }

      if (rx_count (u)) {
        r = u->rx[u->rx_tail++ % sizeof (u->rx)];
      }

      uart_update_irq (m);
      return r;
    case 1:
      return (u->lcr & 0x80) ? u->dlm : u->ier;
    case 2:
      /* IIR */
      if ((u->ier & 1) && rx_count (u)) {
        r = 0x04;
      } else if ((u->ier & 2) && u->thre_pending) {
        r = 0x02;
        u->thre_pending = 0;
      } else {
        r = 0x01;
      }

      if (u->fcr & 1) {
        r |= 0xc0;
      }

      uart_update_irq (m);
      return r;
    case 3:
      return u->lcr;
    case 4:
      return u->mcr;
    case 5:
      r = LSR_THRE | LSR_TEMT;
      if (rx_count (u)) {
        r |= LSR_DR;
      }

      return r;
    case 6:
      /* MSR: loopback maps MCR bits, otherwise CTS+DSR+DCD */
      if (u->mcr & 0x10) {
        return ((u->mcr & 0x0c) << 4) | ((u->mcr & 0x01) << 5) | ((u->mcr & 0x02) << 3);
      }

      return 0xb0;
    default:
      return u->scr;
  }
}

void
uart_io_write (machine_t *m, uint16_t port, uint8_t val)
{
  uart_t *u = &m->uart;
  char    c;

  switch (port & 7) {
    case 0:
      if (u->lcr & 0x80) {
        u->dll = val;
        return;
      }

      if (u->mcr & 0x10) {
        /* loopback */
        u->rx[u->rx_head++ % sizeof (u->rx)] = val;
      } else {
        c = (char)val;
        host_console_write (&c, 1);
      }

      u->thre_pending = 1;
      break;
    case 1:
      if (u->lcr & 0x80) {
        u->dlm = val;
        return;
      }

      if ((val & 2) && !(u->ier & 2)) {
        u->thre_pending = 1;
      }

      u->ier = val & 0x0f;
      break;
    case 2:
      u->fcr = val;
      if (val & 2) {
        u->rx_tail = u->rx_head;
      }

      break;
    case 3:
      u->lcr = val;
      break;
    case 4:
      u->mcr = val & 0x1f;
      break;
    case 7:
      u->scr = val;
      break;
    default:
      break;
  }

  uart_update_irq (m);
}

void
uart_init (machine_t *m)
{
  m->uart = (uart_t){ 0 };
}

/* ------------------------------------------------------------------ RTC */
typedef struct {
  int sec, min, hour, wday, mday, mon, year;   /* mon 1-12, full year */
} rtc_tm_t;

/* civil date from days since 1970-01-01 (Howard Hinnant's algorithm) */
static void
rtc_now (machine_t *m, rtc_tm_t *t)
{
  uint64_t s    = host_now_ns () / 1000000000ULL + m->boot_ns / 1000000000ULL;
  int64_t  days = (int64_t)(s / 86400), z, era, doe, yoe, doy, mp;
  uint32_t sod  = (uint32_t)(s % 86400);

  t->sec  = sod % 60;
  t->min  = (sod / 60) % 60;
  t->hour = sod / 3600;
  t->wday = (int)((days + 4) % 7);          /* 1970-01-01 was a Thursday */
  z       = days + 719468;
  era     = z / 146097;
  doe     = z - era * 146097;
  yoe     = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  doy     = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp      = (5 * doy + 2) / 153;
  t->mday = (int)(doy - (153 * mp + 2) / 5 + 1);
  t->mon  = (int)(mp < 10 ? mp + 3 : mp - 9);
  t->year = (int)(yoe + era * 400 + (t->mon <= 2));
}

static uint8_t
bcd (int v)
{
  return (uint8_t)(((v / 10) << 4) | (v % 10));
}

uint32_t
cmos_io_read (machine_t *m, uint16_t port)
{
  rtc_tm_t  t;
  int       binary, idx;
  int       v = -1;

  if (port == 0x70) {
    return 0xff;
  }

  idx    = m->cmos_index & 0x7f;
  binary = m->cmos[0x0b] & 0x04;
  if (idx <= 9 || idx == 0x32) {
    rtc_now (m, &t);
    switch (idx) {
      case 0: v = t.sec; break;
      case 2: v = t.min; break;
      case 4: v = t.hour; break;
      case 6: v = t.wday + 1; break;
      case 7: v = t.mday; break;
      case 8: v = t.mon; break;
      case 9: v = t.year % 100; break;
      case 0x32: v = t.year / 100; break;
      default: return m->cmos[idx];      /* alarms */
    }

    return binary ? (uint8_t)v : bcd (v);
  }

  switch (idx) {
    case 0x0a:
      return m->cmos[0x0a] & 0x7f;       /* never "update in progress" */
    case 0x0c:
      v            = m->cmos[0x0c];
      m->cmos[0x0c] = 0;
      return v;
    case 0x0d:
      return 0x80;                       /* battery good */
    default:
      return m->cmos[idx];
  }
}

void
cmos_io_write (machine_t *m, uint16_t port, uint8_t val)
{
  if (port == 0x70) {
    m->cmos_index = val;
    return;
  }

  if ((m->cmos_index & 0x7f) > 9 && (m->cmos_index & 0x7f) != 0x0c && (m->cmos_index & 0x7f) != 0x0d) {
    m->cmos[m->cmos_index & 0x7f] = val;
  }
}
