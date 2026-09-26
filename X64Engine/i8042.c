/*
 * i8042 keyboard controller (0x60/0x64, IRQ1) with a PS/2 keyboard.
 * No mouse port. Keys arrive from the host as PC scan code set 1
 * (the controller's translation mode, which is what OSes enable).
 */
#include "x64e.h"

#define ST_OBF   0x01
#define ST_SYS   0x04
#define ST_CMD   0x08

static void
kq_push (machine_t *m, uint8_t b)
{
  i8042_t *k = &m->kbd;

  if ((uint8_t)(k->head - k->tail) < 255) {
    k->q[k->head++ % sizeof (k->q)] = b;
  }
}

static void
kbd_update_irq (machine_t *m)
{
  i8042_t *k = &m->kbd;
  int      level, fresh = 0;

  if (!k->obf && k->head != k->tail && !(k->ccb & 0x10)) {
    k->out = k->q[k->tail++ % sizeof (k->q)];
    k->obf = 1;
    fresh  = 1;
  }

  level = k->obf && (k->ccb & 0x01);
  if (fresh && level && k->irq_level) {
    /* IRQ1 is edge triggered: every new byte needs a new rising edge */
    pic_set_irq (m, 1, 0);
    pic_set_irq (m, 1, 1);
    return;
  }

  if (level != k->irq_level) {
    k->irq_level = level;
    pic_set_irq (m, 1, level);
  }
}

/* host key: set-1 make/break code bytes (0xE0 prefixes included) */
void
i8042_key (machine_t *m, uint8_t code)
{
  if (m->kbd.scanning) {
    kq_push (m, code);
    kbd_update_irq (m);
  }
}

static void
kbd_command (machine_t *m, uint8_t v)
{
  i8042_t *k = &m->kbd;

  if (k->kbd_param) {
    uint8_t cmd = k->kbd_param;

    k->kbd_param = 0;
    kq_push (m, 0xfa);
    if (cmd == 0xf0 && v == 0) {
      kq_push (m, 0x41);        /* set 2, translated */
    }

    return;
  }

  switch (v) {
    case 0xff:                  /* reset */
      kq_push (m, 0xfa);
      kq_push (m, 0xaa);
      k->scanning = 1;
      break;
    case 0xf2:                  /* identify */
      kq_push (m, 0xfa);
      kq_push (m, 0xab);
      kq_push (m, (k->ccb & 0x40) ? 0x41 : 0x83);
      break;
    case 0xf4:
      k->scanning = 1;
      kq_push (m, 0xfa);
      break;
    case 0xf5:
      k->scanning = 0;
      kq_push (m, 0xfa);
      break;
    case 0xf6:
      k->scanning = 1;
      kq_push (m, 0xfa);
      break;
    case 0xed:                  /* LEDs */
    case 0xf0:                  /* scan code set */
    case 0xf3:                  /* typematic */
      k->kbd_param = v;
      kq_push (m, 0xfa);
      break;
    case 0xee:
      kq_push (m, 0xee);
      break;
    default:
      kq_push (m, 0xfa);
      break;
  }
}

uint32_t
i8042_io_read (machine_t *m, uint16_t port)
{
  i8042_t *k = &m->kbd;
  uint8_t  v;

  if (port == 0x64) {
    v = ST_SYS | 0x10 | (k->last_was_cmd ? ST_CMD : 0);   /* 0x10: not inhibited */
    if (k->obf) {
      v |= ST_OBF;
    }

    return v;
  }

  v      = k->out;
  k->obf = 0;
  kbd_update_irq (m);
  return v;
}

void
i8042_io_write (machine_t *m, uint16_t port, uint8_t v)
{
  i8042_t *k = &m->kbd;

  if (port == 0x64) {
    k->last_was_cmd = 1;
    k->pending      = 0;
    switch (v) {
      case 0x20:
        kq_push (m, k->ccb);
        break;
      case 0x60:
      case 0xd1:
      case 0xd2:
      case 0xd3:
      case 0xd4:
        k->pending = v;
        break;
      case 0xa7:
      case 0xa8:
        break;
      case 0xa9:                /* test aux port: fail, there is no mouse */
        kq_push (m, 0xff);
        break;
      case 0xaa:                /* self test */
        kq_push (m, 0x55);
        break;
      case 0xab:                /* test keyboard port */
        kq_push (m, 0x00);
        break;
      case 0xad:
        k->ccb |= 0x10;
        break;
      case 0xae:
        k->ccb &= ~0x10;
        break;
      case 0xfe:
        m->reset_request = 1;
        break;
      default:
        break;
    }

    kbd_update_irq (m);
    return;
  }

  k->last_was_cmd = 0;
  switch (k->pending) {
    case 0x60:
      k->ccb = v;
      break;
    case 0xd1:                  /* output port: bit 0 low = reset */
      if (!(v & 1)) {
        m->reset_request = 1;
      }

      break;
    case 0xd2:
      kq_push (m, v);
      break;
    case 0xd3:
    case 0xd4:
      break;                    /* no mouse */
    default:
      kbd_command (m, v);
      break;
  }

  k->pending = 0;
  kbd_update_irq (m);
}

void
i8042_init (machine_t *m)
{
  m->kbd          = (i8042_t){ 0 };
  m->kbd.ccb      = 0x45;       /* IRQ1 on, system flag, translation */
  m->kbd.scanning = 1;
}
