/*
 * i8042 keyboard controller (0x60/0x64) with a PS/2 keyboard (IRQ 1) and,
 * on the PC BIOS machine, a PS/2 mouse on the auxiliary port (IRQ 12).
 * Keys arrive from the host as PC scan code set 1 (the controller's
 * translation mode, which is what OSes enable).
 */
#include "x64e.h"

#define ST_OBF   0x01
#define ST_SYS   0x04
#define ST_CMD   0x08
#define ST_AUX   0x20

#define SRC_KBD  1
#define SRC_AUX  2

static void
kq_push (machine_t *m, uint8_t b)
{
  i8042_t *k = &m->kbd;

  if ((uint8_t)(k->head - k->tail) < 255) {
    k->q[k->head++ % sizeof (k->q)] = b;
  }
}

static void
mq_push (machine_t *m, uint8_t b)
{
  i8042_t *k = &m->kbd;

  if ((uint8_t)(k->mhead - k->mtail) < 255) {
    k->mq[k->mhead++ % sizeof (k->mq)] = b;
  }
}

/* a reply from the controller itself: not held back by a disabled port */
static void
cq_push_src (machine_t *m, uint8_t b, uint8_t src)
{
  i8042_t *k = &m->kbd;

  if ((uint8_t)(k->chead - k->ctail) < sizeof (k->cq)) {
    k->csrc[k->chead % sizeof (k->cq)] = src;
    k->cq[k->chead++ % sizeof (k->cq)] = b;
  }
}

static void
cq_push (machine_t *m, uint8_t b)
{
  cq_push_src (m, b, SRC_KBD);
}

static void
kbd_update_irq (machine_t *m)
{
  i8042_t *k = &m->kbd;
  int      level, aux, fresh = 0;

  if (!k->obf) {
    if (k->chead != k->ctail) {
      k->src = k->csrc[k->ctail % sizeof (k->cq)];
      k->out = k->cq[k->ctail++ % sizeof (k->cq)];
      k->obf = fresh = 1;
    } else if (k->head != k->tail && !(k->ccb & 0x10)) {
      k->out = k->q[k->tail++ % sizeof (k->q)];
      k->src = SRC_KBD;
      k->obf = fresh = 1;
    } else if (k->mhead != k->mtail && !(k->ccb & 0x20)) {
      k->out = k->mq[k->mtail++ % sizeof (k->mq)];
      k->src = SRC_AUX;
      k->obf = fresh = 1;
    }
  }

  level = k->obf && k->src == SRC_KBD && (k->ccb & 0x01);
  aux   = k->obf && k->src == SRC_AUX && (k->ccb & 0x02);
  if (fresh && level && k->irq_level) {
    /* IRQ1 is edge triggered: every new byte needs a new rising edge */
    pic_set_irq (m, 1, 0);
    pic_set_irq (m, 1, 1);
  } else if (level != k->irq_level) {
    k->irq_level = level;
    pic_set_irq (m, 1, level);
  }

  if (fresh && aux && k->aux_irq_level) {
    pic_set_irq (m, 12, 0);
    pic_set_irq (m, 12, 1);
  } else if (aux != k->aux_irq_level) {
    k->aux_irq_level = aux;
    pic_set_irq (m, 12, aux);
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

/* ---------------------------------------------------------- mouse ------ */
static void
mouse_packet (machine_t *m)
{
  i8042_t *k = &m->kbd;
  int      dx = k->mouse_dx, dy = k->mouse_dy, dz = k->mouse_dz;

  if (dx > 255) dx = 255;
  if (dx < -255) dx = -255;
  if (dy > 255) dy = 255;
  if (dy < -255) dy = -255;
  if (dz > 7) dz = 7;
  if (dz < -7) dz = -7;
  k->mouse_dx -= dx;
  k->mouse_dy -= dy;
  k->mouse_dz -= dz;
  mq_push (m, (uint8_t)(0x08 | (k->mouse_buttons & 7) | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0)));
  mq_push (m, (uint8_t)dx);
  mq_push (m, (uint8_t)dy);
  if (k->mouse_id == 3) {
    mq_push (m, (uint8_t)dz);
  } else if (k->mouse_id == 4) {
    mq_push (m, (uint8_t)((dz & 0x0f) | ((k->mouse_buttons & 0x18) << 1)));
  }
}

/* host pointer: relative motion (dy up is positive), wheel, buttons (bit 0 left, 1 right, 2 middle) */
void
i8042_mouse (machine_t *m, int dx, int dy, int dz, int buttons)
{
  i8042_t *k = &m->kbd;

  if (!m->pc_bios || !k->mouse_on) {
    return;
  }

  k->mouse_dx     += dx;
  k->mouse_dy     += dy;
  k->mouse_dz     += dz;
  k->mouse_buttons = (uint8_t)buttons;
  if ((uint8_t)(k->mhead - k->mtail) < 240) {
    mouse_packet (m);
    kbd_update_irq (m);
  }
}

static void
mouse_defaults (i8042_t *k)
{
  k->mouse_on    = 0;
  k->mouse_rate  = 100;
  k->mouse_res   = 2;
  k->mouse_scale = 0;
  k->mouse_wrap  = 0;
  k->mouse_dx    = k->mouse_dy = k->mouse_dz = 0;
}

static void
mouse_command (machine_t *m, uint8_t v)
{
  i8042_t *k = &m->kbd;

  if (k->mouse_wrap && v != 0xff && v != 0xec) {
    mq_push (m, v);
    return;
  }

  if (k->mouse_param) {
    uint8_t cmd = k->mouse_param;

    k->mouse_param = 0;
    mq_push (m, 0xfa);
    if (cmd == 0xf3) {
      k->mouse_rate   = v;
      k->mouse_seq[0] = k->mouse_seq[1];
      k->mouse_seq[1] = k->mouse_seq[2];
      k->mouse_seq[2] = v;
      /* the "knock" that turns on the wheel / the 5 button protocol */
      if (k->mouse_seq[0] == 200 && k->mouse_seq[1] == 100 && k->mouse_seq[2] == 80 && k->mouse_id == 0) {
        k->mouse_id = 3;
      } else if (k->mouse_seq[0] == 200 && k->mouse_seq[1] == 200 && k->mouse_seq[2] == 80 && k->mouse_id == 3) {
        k->mouse_id = 4;
      }
    } else if (cmd == 0xe8) {
      k->mouse_res = v;
    }

    return;
  }

  switch (v) {
    case 0xff:
      mouse_defaults (k);
      k->mouse_id = 0;
      mq_push (m, 0xfa);
      mq_push (m, 0xaa);
      mq_push (m, 0x00);
      break;
    case 0xf6:
      mouse_defaults (k);
      mq_push (m, 0xfa);
      break;
    case 0xf5:
      k->mouse_on = 0;
      mq_push (m, 0xfa);
      break;
    case 0xf4:
      k->mouse_on = 1;
      mq_push (m, 0xfa);
      break;
    case 0xf3: case 0xe8:
      k->mouse_param = v;
      mq_push (m, 0xfa);
      break;
    case 0xf2:
      mq_push (m, 0xfa);
      mq_push (m, k->mouse_id);
      break;
    case 0xe9:
      mq_push (m, 0xfa);
      mq_push (m, (uint8_t)((k->mouse_on ? 0x20 : 0) | (k->mouse_scale ? 0x10 : 0) | (k->mouse_buttons & 7)));
      mq_push (m, k->mouse_res);
      mq_push (m, k->mouse_rate);
      break;
    case 0xe6: case 0xe7:
      k->mouse_scale = v == 0xe7;
      mq_push (m, 0xfa);
      break;
    case 0xeb:
      mq_push (m, 0xfa);
      mouse_packet (m);
      break;
    case 0xee:
      k->mouse_wrap = 1;
      mq_push (m, 0xfa);
      break;
    case 0xec:
      k->mouse_wrap = 0;
      mq_push (m, 0xfa);
      break;
    default:                    /* 0xEA stream mode, 0xF0 remote mode, ... */
      mq_push (m, 0xfa);
      break;
  }
}

/* -------------------------------------------------------- keyboard ----- */
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
    v = (k->ccb & ST_SYS) | 0x10 | (k->last_was_cmd ? ST_CMD : 0);   /* 0x10: not inhibited */
    if (k->obf) {
      v |= ST_OBF | (k->src == SRC_AUX ? ST_AUX : 0);
    }

    return v;
  }

  v      = k->out;
  k->obf = 0;
  kbd_update_irq (m);
  return v;
}

static void
reset_cpu (machine_t *m)
{
  m->reset_request = 1;
  uc_emu_stop (m->uc);
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
        cq_push (m, k->ccb);
        break;
      case 0x60:
      case 0xd1:
      case 0xd2:
      case 0xd3:
      case 0xd4:
        k->pending = v;
        break;
      case 0xa7:
        k->ccb |= 0x20;
        break;
      case 0xa8:
        k->ccb &= ~0x20;
        break;
      case 0xa9:                /* test the auxiliary port: there is a mouse only on the PC BIOS machine */
        cq_push (m, m->pc_bios ? 0x00 : 0xff);
        break;
      case 0xaa:                /* self test */
        k->ccb |= ST_SYS;
        cq_push (m, 0x55);
        break;
      case 0xab:                /* test keyboard port */
        cq_push (m, 0x00);
        break;
      case 0xad:
        k->ccb |= 0x10;
        break;
      case 0xae:
        k->ccb &= ~0x10;
        break;
      case 0xc0:                /* input port */
        cq_push (m, 0x80);
        break;
      case 0xd0:                /* output port: A20 on, no reset */
        cq_push (m, (uint8_t)(0x03 | (k->obf && k->src == SRC_KBD ? 0x10 : 0) |
                              (k->obf && k->src == SRC_AUX ? 0x20 : 0)));
        break;
      case 0xfe:
        reset_cpu (m);
        break;
      default:
        if (v >= 0xf0 && !(v & 1)) {
          reset_cpu (m);        /* pulse the reset line */
        }

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
        reset_cpu (m);
      }

      break;
    case 0xd2:
      cq_push (m, v);
      break;
    case 0xd3:                  /* loop back through the auxiliary port */
      if (m->pc_bios) {
        cq_push_src (m, v, SRC_AUX);
      }

      break;
    case 0xd4:
      if (m->pc_bios) {
        k->ccb &= ~0x20;        /* talking to the mouse turns its clock on */
        mouse_command (m, v);
      }

      break;
    default:
      if (m->pc_bios) {
        k->ccb &= ~0x10;        /* talking to the keyboard turns its clock on */
      }

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
  mouse_defaults (&m->kbd);
}
