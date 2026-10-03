/*
 * VGA with Bochs VBE (00:02.0, 1234:1111) for the PC BIOS machine: text,
 * planar 16-colour, CGA and 256-colour modes plus VBE linear modes, drawn
 * into the host frame buffer (32 bpp), centred. The video BIOS is SeaVGABIOS.
 */
#include <string.h>
#include "pc.h"

#define VRAM_SIZE   (8u << 20)
#define VBE_NB      0x0b
#define VBE_ENABLED 0x01
#define VBE_GETCAPS 0x02
#define VBE_8BITDAC 0x20
#define VBE_NOCLEAR 0x80

typedef struct {
  pci_dev_t pci;
  uint8_t  *vram;
  uint32_t  lfb_mapped;               /* guest address the VRAM is mapped at, 0: none */
  uint32_t  latch;
  uint8_t   sr_index, sr[8];
  uint8_t   gr_index, gr[16];
  uint8_t   ar_index, ar[21];
  int       ar_flip;
  uint8_t   cr_index, cr[256];
  uint8_t   msr, fcr, st00, st01;
  uint8_t   dac_state, dac_sub, dac_read, dac_write, dac_cache[3], pel_mask;
  uint8_t   palette[768];
  uint16_t  vbe_index, vbe[VBE_NB];
  uint32_t  vbe_start, vbe_line, bank;
  /* host screen */
  uint32_t  last_w, last_h;
  uint32_t  pal32[256];
} vga_t;

static const uint32_t mask16[16] = {
  0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
  0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

static inline int
vbe_on (vga_t *v)
{
  return (v->vbe[4] & VBE_ENABLED) != 0;
}

/* ------------------------------------------------ 0xA0000 - 0xBFFFF ---- */
static int
mem_addr (vga_t *v, uint32_t *addr)
{
  uint32_t a = *addr;

  switch ((v->gr[6] >> 2) & 3) {
    case 0:
      break;
    case 1:
      if (a >= 0x10000) {
        return 0;
      }

      a += v->bank;
      break;
    case 2:
      if (a < 0x10000 || a >= 0x18000) {
        return 0;
      }

      a -= 0x10000;
      break;
    default:
      if (a < 0x18000) {
        return 0;
      }

      a -= 0x18000;
      break;
  }

  *addr = a;
  return 1;
}

static uint8_t
mem_readb (vga_t *v, uint32_t addr)
{
  uint32_t ret;

  if (!mem_addr (v, &addr)) {
    return 0xff;
  }

  if (v->sr[4] & 0x08) {
    return addr < VRAM_SIZE ? v->vram[addr] : 0xff;
  }

  if (v->gr[5] & 0x10) {
    uint32_t plane = (v->gr[4] & 2) | (addr & 1);

    addr = ((addr & ~1u) << 1) | plane;
    return addr < VRAM_SIZE ? v->vram[addr] : 0xff;
  }

  if (addr * 4 >= VRAM_SIZE) {
    return 0xff;
  }

  memcpy (&v->latch, v->vram + addr * 4, 4);
  if (!(v->gr[5] & 0x08)) {
    return (uint8_t)(v->latch >> ((v->gr[4] & 3) * 8));
  }

  ret  = (v->latch ^ mask16[v->gr[2] & 15]) & mask16[v->gr[7] & 15];
  ret |= ret >> 16;
  ret |= ret >> 8;
  return (uint8_t) ~ret;
}

static void
mem_writeb (vga_t *v, uint32_t addr, uint32_t val)
{
  uint32_t bit_mask, write_mask, cur;
  int      b;

  if (!mem_addr (v, &addr)) {
    return;
  }

  if (v->sr[4] & 0x08) {
    if ((v->sr[2] & (1 << (addr & 3))) && addr < VRAM_SIZE) {
      v->vram[addr] = (uint8_t)val;
    }

    return;
  }

  if (v->gr[5] & 0x10) {
    uint32_t plane = (v->gr[4] & 2) | (addr & 1);

    if (v->sr[2] & (1 << plane)) {
      addr = ((addr & ~1u) << 1) | plane;
      if (addr < VRAM_SIZE) {
        v->vram[addr] = (uint8_t)val;
      }
    }

    return;
  }

  if (addr * 4 >= VRAM_SIZE) {
    return;
  }

  switch (v->gr[5] & 3) {
    default:
    case 0: {
      uint32_t set_mask = mask16[v->gr[1] & 15];

      b        = v->gr[3] & 7;
      val      = ((val >> b) | (val << (8 - b))) & 0xff;
      val     |= val << 8;
      val     |= val << 16;
      val      = (val & ~set_mask) | (mask16[v->gr[0] & 15] & set_mask);
      bit_mask = v->gr[8];
      break;
    }
    case 1:
      val = v->latch;
      goto do_write;
    case 2:
      val      = mask16[val & 15];
      bit_mask = v->gr[8];
      break;
    case 3:
      b        = v->gr[3] & 7;
      val      = ((val >> b) | (val << (8 - b))) & 0xff;
      bit_mask = v->gr[8] & val;
      val      = mask16[v->gr[0] & 15];
      break;
  }

  switch (v->gr[3] >> 3) {
    case 1: val &= v->latch; break;
    case 2: val |= v->latch; break;
    case 3: val ^= v->latch; break;
    default: break;
  }

  bit_mask |= bit_mask << 8;
  bit_mask |= bit_mask << 16;
  val       = (val & bit_mask) | (v->latch & ~bit_mask);
do_write:
  write_mask = mask16[v->sr[2] & 15];
  memcpy (&cur, v->vram + addr * 4, 4);
  cur = (cur & ~write_mask) | (val & write_mask);
  memcpy (v->vram + addr * 4, &cur, 4);
}

static uint64_t
vga_mem_read (uc_engine *uc, uint64_t off, unsigned size, void *opaque)
{
  vga_t   *v = ((machine_t *)opaque)->vga;
  uint64_t r = 0;
  unsigned i;

  (void)uc;
  for (i = 0; i < size; i++) {
    r |= (uint64_t)mem_readb (v, (uint32_t)off + i) << (8 * i);
  }

  return r;
}

static void
vga_mem_write (uc_engine *uc, uint64_t off, unsigned size, uint64_t val, void *opaque)
{
  vga_t   *v = ((machine_t *)opaque)->vga;
  unsigned i;

  (void)uc;
  for (i = 0; i < size; i++) {
    mem_writeb (v, (uint32_t)off + i, (uint32_t)(val >> (8 * i)) & 0xff);
  }
}

/* ---------------------------------------------------------- Bochs VBE -- */
static uint32_t
vbe_max (machine_t *m, int y)
{
  uint32_t w = m->fb ? m->fb_w : 1024, h = m->fb ? m->fb_h : 768;

  if (w > 1920) {
    w = 1920;
  }

  if (h > 1200) {
    h = 1200;
  }

  return y ? h : w;
}

static void
vbe_fixup (vga_t *v)
{
  uint32_t bpp = v->vbe[3], bytes = bpp == 4 ? 0 : (bpp + 7) / 8;

  if (v->vbe[6] < v->vbe[1]) {
    v->vbe[6] = v->vbe[1];
  }

  v->vbe_line  = bpp == 4 ? (uint32_t)v->vbe[6] >> 3 : (uint32_t)v->vbe[6] * bytes;
  v->vbe_start = bpp == 4 ? (uint32_t)v->vbe[9] * v->vbe_line + (v->vbe[8] >> 3)
                 : (uint32_t)v->vbe[9] * v->vbe_line + (uint32_t)v->vbe[8] * bytes;
}

static uint32_t
vbe_read (machine_t *m, vga_t *v)
{
  unsigned i = v->vbe_index;

  if (i >= VBE_NB) {
    return 0;
  }

  if (v->vbe[4] & VBE_GETCAPS) {
    switch (i) {
      case 1: return vbe_max (m, 0);
      case 2: return vbe_max (m, 1);
      case 3: return 32;
      default: break;
    }
  }

  if (i == 0x0a) {
    return VRAM_SIZE >> 16;
  }

  if (i == 7 && v->vbe_line) {
    return VRAM_SIZE / v->vbe_line > 0xffff ? 0xffff : VRAM_SIZE / v->vbe_line;
  }

  return v->vbe[i];
}

static void
vbe_write (machine_t *m, vga_t *v, uint32_t val)
{
  unsigned i = v->vbe_index;

  if (i >= VBE_NB) {
    return;
  }

  switch (i) {
    case 0:
      if ((val & 0xfff0) == 0xb0c0) {
        v->vbe[0] = (uint16_t)val;
      }

      break;
    case 1: case 2:
      v->vbe[i] = (uint16_t)val;
      break;
    case 3:
      if (val == 0) {
        val = 8;
      }

      if (val == 4 || val == 8 || val == 15 || val == 16 || val == 24 || val == 32) {
        v->vbe[3] = (uint16_t)val;
      }

      break;
    case 5:
      v->vbe[5] = (uint16_t)val;
      v->bank   = (val & 0x7f) << 16;
      break;
    case 4:
      if ((val & VBE_ENABLED) && !(v->vbe[4] & VBE_ENABLED)) {
        uint32_t w = v->vbe[1], h = v->vbe[2];

        if (w == 0 || h == 0 || w > vbe_max (m, 0) || h > 4096) {
          val &= ~VBE_ENABLED;
        } else {
          v->vbe[6] = (uint16_t)w;
          v->vbe[7] = (uint16_t)h;
          v->vbe[8] = v->vbe[9] = 0;
          vbe_fixup (v);
          if (!(val & VBE_NOCLEAR)) {
            uint64_t n = (uint64_t)v->vbe_line * h * (v->vbe[3] == 4 ? 4 : 1);

            memset (v->vram, 0, n > VRAM_SIZE ? VRAM_SIZE : (size_t)n);
          }

          /* what the VGA registers of such a mode look like (as the Bochs card does) */
          v->gr[6]     = (v->gr[6] & ~0x0c) | 0x04 | 0x01;
          v->cr[0x17] |= 3;
          v->cr[0x13]  = (uint8_t)(v->vbe_line >> 3);
          v->cr[0x01]  = (uint8_t)((w >> 3) - 1);
          v->cr[0x12]  = (uint8_t)(h - 1);
          v->cr[0x07]  = (uint8_t)((v->cr[0x07] & ~0x42) | (((h - 1) >> 7) & 0x02) | (((h - 1) >> 3) & 0x40));
          v->cr[0x18]  = 0xff;
          v->cr[0x07] |= 0x10;
          v->cr[0x09]  = (v->cr[0x09] & ~0x9f) | 0x40;
          if (v->vbe[3] == 4) {
            v->sr[1]  &= ~8;
            v->sr[4]  &= ~8;
            v->sr[2]  |= 0x0f;
            v->gr[5]  &= ~0x60;
          } else {
            v->sr[4]  |= 0x08 | 0x02;
            v->sr[2]  |= 0x0f;
            v->gr[5]   = (v->gr[5] & ~0x60) | 0x40;
          }
        }
      } else if (!(val & VBE_ENABLED)) {
        v->bank = 0;
      }

      v->vbe[4] = (uint16_t)val;
      break;
    case 6:
      v->vbe[6] = (uint16_t)val;
      vbe_fixup (v);
      break;
    case 8: case 9:
      v->vbe[i] = (uint16_t)val;
      vbe_fixup (v);
      break;
    default:
      break;
  }
}

/* --------------------------------------------------------------- ports -- */
static int
crtc_port (vga_t *v, uint16_t port)
{
  /* 0x3Bx in monochrome emulation, 0x3Dx in colour */
  if (v->msr & 1) {
    return port >= 0x3d0 && port <= 0x3df;
  }

  return port >= 0x3b0 && port <= 0x3bf;
}

static uint8_t
port_readb (machine_t *m, vga_t *v, uint16_t port)
{
  uint8_t r = 0xff;

  if ((port >= 0x3b0 && port <= 0x3bf) || (port >= 0x3d0 && port <= 0x3df)) {
    if (!crtc_port (v, port)) {
      return 0xff;
    }

    switch (port & 0x0f) {
      case 4: return v->cr_index;
      case 5: return v->cr[v->cr_index];
      case 0x0a:
        v->st01   ^= 0x09;            /* retrace bits toggle so that waits end */
        v->ar_flip = 0;
        return v->st01;
      default: return 0xff;
    }
  }

  switch (port) {
    case 0x3c0:
      r = v->ar_flip ? 0 : v->ar_index;
      break;
    case 0x3c1:
      r = (v->ar_index & 0x1f) < 21 ? v->ar[v->ar_index & 0x1f] : 0;
      break;
    case 0x3c2: r = v->st00; break;
    case 0x3c4: r = v->sr_index; break;
    case 0x3c5: r = v->sr[v->sr_index & 7]; break;
    case 0x3c6: r = v->pel_mask; break;
    case 0x3c7: r = v->dac_state; break;
    case 0x3c8: r = v->dac_write; break;
    case 0x3c9:
      r = v->palette[v->dac_read * 3 + v->dac_sub];
      if (++v->dac_sub == 3) {
        v->dac_sub = 0;
        v->dac_read++;
      }

      break;
    case 0x3ca: r = v->fcr; break;
    case 0x3cc: r = v->msr; break;
    case 0x3ce: r = v->gr_index; break;
    case 0x3cf: r = v->gr[v->gr_index & 15]; break;
    default: break;
  }

  (void)m;
  return r;
}

static void
port_writeb (machine_t *m, vga_t *v, uint16_t port, uint8_t val)
{
  static const uint8_t sr_mask[8] = { 0x03, 0x3d, 0x0f, 0x3f, 0x0e, 0x00, 0x00, 0xff };
  static const uint8_t gr_mask[16] = { 0x0f, 0x0f, 0x0f, 0x1f, 0x03, 0x7b, 0x0f, 0x0f, 0xff, 0, 0, 0, 0, 0, 0, 0 };

  (void)m;
  if ((port >= 0x3b0 && port <= 0x3bf) || (port >= 0x3d0 && port <= 0x3df)) {
    if (!crtc_port (v, port)) {
      return;
    }

    switch (port & 0x0f) {
      case 4:
        v->cr_index = val;
        break;
      case 5:
        /* CR0-7 are write protected by CR11 bit 7 (except the line compare bit) */
        if ((v->cr[0x11] & 0x80) && v->cr_index <= 7) {
          if (v->cr_index == 7) {
            v->cr[7] = (v->cr[7] & ~0x10) | (val & 0x10);
          }

          break;
        }

        v->cr[v->cr_index] = val;
        break;
      case 0x0a:
        v->fcr = val & 0x10;
        break;
      default:
        break;
    }

    return;
  }

  switch (port) {
    case 0x3c0:
      if (!v->ar_flip) {
        v->ar_index = val & 0x3f;
      } else {
        unsigned i = v->ar_index & 0x1f;

        if (i < 16) {
          v->ar[i] = val & 0x3f;
        } else if (i == 0x10) {
          v->ar[i] = val & ~0x10;
        } else if (i == 0x11) {
          v->ar[i] = val;
        } else if (i == 0x12) {
          v->ar[i] = val & ~0xc0;
        } else if (i == 0x13 || i == 0x14) {
          v->ar[i] = val & 0x0f;
        }
      }

      v->ar_flip ^= 1;
      break;
    case 0x3c2:
      v->msr = val & ~0x10;
      break;
    case 0x3c4: v->sr_index = val & 7; break;
    case 0x3c5: v->sr[v->sr_index] = val & sr_mask[v->sr_index]; break;
    case 0x3c6: v->pel_mask = val; break;
    case 0x3c7:
      v->dac_read  = val;
      v->dac_sub   = 0;
      v->dac_state = 3;
      break;
    case 0x3c8:
      v->dac_write = val;
      v->dac_sub   = 0;
      v->dac_state = 0;
      break;
    case 0x3c9:
      v->dac_cache[v->dac_sub] = val;
      if (++v->dac_sub == 3) {
        memcpy (v->palette + v->dac_write * 3, v->dac_cache, 3);
        v->dac_sub = 0;
        v->dac_write++;
      }

      break;
    case 0x3ce: v->gr_index = val & 0x0f; break;
    case 0x3cf: v->gr[v->gr_index] = val & gr_mask[v->gr_index]; break;
    default: break;
  }
}

static int
vga_port (uint16_t port)
{
  return (port >= 0x3b4 && port <= 0x3b5) || port == 0x3ba || (port >= 0x3c0 && port <= 0x3cf) ||
         (port >= 0x3d4 && port <= 0x3d5) || port == 0x3da;
}

int
vga_io_read (machine_t *m, uint16_t port, int size, uint32_t *val)
{
  vga_t *v = m->vga;
  int    i;

  if (port == 0x1ce) {
    *val = v->vbe_index;
    return 1;
  }

  if (port == 0x1cf || port == 0x1d0) {
    *val = vbe_read (m, v);
    return 1;
  }

  if (!vga_port (port)) {
    return 0;
  }

  *val = 0;
  for (i = 0; i < size; i++) {
    *val |= (uint32_t)port_readb (m, v, (uint16_t)(port + i)) << (8 * i);
  }

  return 1;
}

int
vga_io_write (machine_t *m, uint16_t port, int size, uint32_t val)
{
  vga_t *v = m->vga;
  int    i;

  if (port == 0x1ce) {
    v->vbe_index = (uint16_t)val;
    return 1;
  }

  if (port == 0x1cf || port == 0x1d0) {
    vbe_write (m, v, val & 0xffff);
    return 1;
  }

  if (!vga_port (port)) {
    return 0;
  }

  for (i = 0; i < size; i++) {
    port_writeb (m, v, (uint16_t)(port + i), (uint8_t)(val >> (8 * i)));
  }

  return 1;
}

/* ------------------------------------------------------------- drawing -- */
static uint32_t
dac_colour (vga_t *v, unsigned i)
{
  const uint8_t *p = v->palette + (i & 0xff) * 3;

  if (v->vbe[4] & VBE_8BITDAC) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
  }

#define C6(x)  ((uint32_t)((((x) & 0x3f) << 2) | (((x) & 0x3f) >> 4)))
  return (C6 (p[0]) << 16) | (C6 (p[1]) << 8) | C6 (p[2]);
#undef C6
}

/* the 16 attribute colours */
static void
palette16 (vga_t *v, uint32_t *pal)
{
  int i;

  for (i = 0; i < 16; i++) {
    unsigned c = v->ar[i];

    if (v->ar[0x10] & 0x80) {
      c = ((v->ar[0x14] & 0x0f) << 4) | (c & 0x0f);
    } else {
      c = ((v->ar[0x14] & 0x0c) << 4) | (c & 0x3f);
    }

    pal[i] = dac_colour (v, c & v->pel_mask);
  }
}

typedef struct {
  machine_t *m;
  uint32_t   w, h;                    /* guest picture */
  uint32_t   x0, y0, cw, ch;          /* where it goes, how much of it fits */
} canvas_t;

static void
canvas_open (machine_t *m, vga_t *v, canvas_t *c, uint32_t w, uint32_t h)
{
  c->m  = m;
  c->w  = w;
  c->h  = h;
  c->cw = w < m->fb_w ? w : m->fb_w;
  c->ch = h < m->fb_h ? h : m->fb_h;
  c->x0 = (m->fb_w - c->cw) / 2;
  c->y0 = (m->fb_h - c->ch) / 2;
  if (w != v->last_w || h != v->last_h) {
    uint32_t y;

    host_log ("x64e: screen %ux%u (%s", w, h, vbe_on (v) ? "VBE" : (v->gr[6] & 1) ? "VGA graphics" : "text");
    if (vbe_on (v)) {
      host_log (", %u bpp, line %u, start %u, frame buffer at %x", v->vbe[3], v->vbe_line, v->vbe_start, v->lfb_mapped);
    }

    host_log (")\n");

    for (y = 0; y < m->fb_h; y++) {
      memset (m->fb + (size_t)y * m->fb_stride, 0, (size_t)m->fb_w * 4);
    }

    v->last_w = w;
    v->last_h = h;
  }
}

static inline uint32_t *
canvas_row (canvas_t *c, uint32_t y)
{
  return (uint32_t *)(c->m->fb + (size_t)(c->y0 + y) * c->m->fb_stride) + c->x0;
}

static void
draw_text (machine_t *m, vga_t *v)
{
  uint32_t pal[16];
  uint32_t cols   = v->cr[1] + 1u;
  uint32_t cheight = (v->cr[9] & 0x1f) + 1u;
  uint32_t height = (v->cr[0x12] | ((v->cr[7] & 0x02) << 7) | ((v->cr[7] & 0x40) << 3)) + 1u;
  uint32_t rows   = height / cheight;
  uint32_t cw     = (v->sr[1] & 1) ? 8 : 9;
  uint32_t line   = (uint32_t)v->cr[0x13] << 3;
  uint32_t start  = ((uint32_t)v->cr[0x0c] << 8 | v->cr[0x0d]) * 4;
  uint32_t cursor = ((uint32_t)v->cr[0x0e] << 8 | v->cr[0x0f]) * 4;
  uint32_t font[2];
  int      blink_on = (host_now_ns () / 250000000ULL) & 1;
  canvas_t c;
  uint32_t r, col, y, x;

  if (cols > 132 || rows == 0 || rows > 100) {
    return;
  }

  if (v->sr[1] & 8) {
    cw = 16;                          /* 40-column modes */
  }

  font[0] = (((v->sr[3] >> 4) & 1) | ((v->sr[3] << 1) & 6)) * 8192u * 4 + 2;
  font[1] = (((v->sr[3] >> 5) & 1) | ((v->sr[3] >> 1) & 6)) * 8192u * 4 + 2;
  palette16 (v, pal);
  canvas_open (m, v, &c, cols * cw, rows * cheight);
  for (r = 0; r < rows; r++) {
    for (col = 0; col < cols; col++) {
      uint32_t       a  = (start + r * line + col * 4) & (VRAM_SIZE - 1);
      uint8_t        ch = v->vram[a], at = v->vram[a + 1];
      uint32_t       fg = pal[at & 15], bg = pal[(v->ar[0x10] & 0x08) ? ((at >> 4) & 7) : (at >> 4)];
      const uint8_t *g  = v->vram + ((font[(at >> 3) & 1] + ch * 32u * 4) & (VRAM_SIZE - 1));
      int            cur = (a == (cursor & (VRAM_SIZE - 1))) && !(v->cr[0x0a] & 0x20) && blink_on;

      if (col * cw >= c.cw) {
        break;
      }

      for (y = 0; y < cheight && r * cheight + y < c.ch; y++) {
        uint32_t *p    = canvas_row (&c, r * cheight + y) + col * cw;
        uint32_t  bits = g[y * 4];

        if (cur && y >= (uint32_t)(v->cr[0x0a] & 0x1f) && y <= (uint32_t)(v->cr[0x0b] & 0x1f)) {
          bits = 0xff;
        }

        if (cw == 16) {
          for (x = 0; x < 16 && col * cw + x < c.cw; x++) {
            p[x] = (bits & (0x80 >> (x / 2))) ? fg : bg;
          }

          continue;
        }

        for (x = 0; x < 8 && col * cw + x < c.cw; x++) {
          p[x] = (bits & (0x80 >> x)) ? fg : bg;
        }

        if (cw == 9 && col * cw + 8 < c.cw) {
          /* ninth column: line-drawing characters continue, the rest is blank */
          p[8] = ((v->ar[0x10] & 0x04) && ch >= 0xc0 && ch <= 0xdf && (bits & 1)) ? fg : bg;
        }
      }
    }
  }
}

static void
draw_graphics (machine_t *m, vga_t *v)
{
  uint32_t pal[256];
  uint32_t width  = (v->cr[1] + 1u) * 8;
  uint32_t height = (v->cr[0x12] | ((v->cr[7] & 0x02) << 7) | ((v->cr[7] & 0x40) << 3)) + 1u;
  uint32_t line   = (uint32_t)v->cr[0x13] << 3;
  uint32_t addr   = ((uint32_t)v->cr[0x0c] << 8 | v->cr[0x0d]) * 4;
  int      shift  = (v->gr[5] >> 5) & 3;
  int      dbl    = v->cr[9] >> 7;
  int      multi  = shift != 1 ? ((((v->cr[9] & 0x1f) + 1) << dbl) - 1) : dbl;
  int      run    = multi;
  int      half   = (v->sr[1] & 8) != 0;   /* dot clock / 2: pixels twice as wide */
  canvas_t c;
  uint32_t y, x, i;

  if (vbe_on (v) && v->vbe[3] == 4) {
    width  = v->vbe[1];
    height = v->vbe[2];
    line   = v->vbe_line * 4;
    addr   = v->vbe_start * 4;
    shift  = 0;
    multi  = run = 0;
    half   = 0;
  }

  if (width == 0 || height == 0 || width > 2048 || height > 2048) {
    return;
  }

  if (shift >= 2) {
    for (i = 0; i < 256; i++) {
      pal[i] = dac_colour (v, i & v->pel_mask);
    }
  } else {
    palette16 (v, pal);
  }

  canvas_open (m, v, &c, width, height);
  for (y = 0; y < height; y++) {
    uint32_t a = addr;

    if (y < c.ch) {
      uint32_t *p = canvas_row (&c, y);

      if (shift == 1 && !(v->cr[0x17] & 1)) {
        a = (a & ~(1u << 15)) | ((y >> dbl) & 1) << 15;      /* CGA: odd lines in the second bank */
      }

      if (shift >= 2) {
        /* 256 colours, one byte per pixel, two dots wide */
        for (x = 0; x < width && x < c.cw; x++) {
          p[x] = pal[v->vram[(a + x / 2) & (VRAM_SIZE - 1)]];
        }
      } else if (shift == 1) {
        /* CGA 4 colours: two bits per pixel from planes 0 and 1, two dots wide */
        for (x = 0; x < width && x < c.cw; x++) {
          uint32_t px = half ? x / 2 : x, b = (a + (px / 8) * 4) & (VRAM_SIZE - 1) & ~3u;
          uint32_t bits = ((uint32_t)v->vram[b + (((px / 4) & 1) ? 1 : 0)]);

          p[x] = pal[(bits >> (6 - 2 * (px & 3))) & 3];
        }
      } else {
        uint32_t pm = v->ar[0x12] & 0x0f;

        for (x = 0; x < width && x < c.cw; x++) {
          uint32_t       px = half ? x / 2 : x;
          const uint8_t *d  = v->vram + ((a + (px / 8) * 4) & (VRAM_SIZE - 1) & ~3u);
          uint32_t       bit = 7 - (px & 7);
          uint32_t       ix  = ((d[0] >> bit) & 1) | (((d[1] >> bit) & 1) << 1) |
                               (((d[2] >> bit) & 1) << 2) | (((d[3] >> bit) & 1) << 3);

          p[x] = pal[ix & pm];
        }
      }
    }

    if (run == 0) {
      addr += line;
      run   = multi;
    } else {
      run--;
    }
  }
}

static void
draw_vbe (machine_t *m, vga_t *v)
{
  uint32_t w = v->vbe[1], h = v->vbe[2], bpp = v->vbe[3];
  canvas_t c;
  uint32_t y, x;

  if (w == 0 || h == 0 || v->vbe_line == 0) {
    return;
  }

  if (bpp == 8) {
    for (x = 0; x < 256; x++) {
      v->pal32[x] = dac_colour (v, x & v->pel_mask);
    }
  }

  canvas_open (m, v, &c, w, h);
  for (y = 0; y < c.ch; y++) {
    uint64_t       off = (uint64_t)v->vbe_start + (uint64_t)y * v->vbe_line;
    const uint8_t *s;
    uint32_t      *p = canvas_row (&c, y);

    if (off + v->vbe_line > VRAM_SIZE) {
      break;
    }

    s = v->vram + off;
    switch (bpp) {
      case 32:
        memcpy (p, s, (size_t)c.cw * 4);
        break;
      case 24:
        for (x = 0; x < c.cw; x++, s += 3) {
          p[x] = s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16);
        }

        break;
      case 16:
        for (x = 0; x < c.cw; x++, s += 2) {
          uint32_t px = s[0] | ((uint32_t)s[1] << 8);

          p[x] = ((px & 0xf800) << 8) | ((px & 0xe000) << 3) | ((px & 0x07e0) << 5) | ((px & 0x0600) >> 1) |
                 ((px & 0x001f) << 3) | ((px & 0x001c) >> 2);
        }

        break;
      case 15:
        for (x = 0; x < c.cw; x++, s += 2) {
          uint32_t px = s[0] | ((uint32_t)s[1] << 8);

          p[x] = ((px & 0x7c00) << 9) | ((px & 0x7000) << 4) | ((px & 0x03e0) << 6) | ((px & 0x0380) << 1) |
                 ((px & 0x001f) << 3) | ((px & 0x001c) >> 2);
        }

        break;
      default:
        for (x = 0; x < c.cw; x++) {
          p[x] = v->pal32[s[x]];
        }

        break;
    }
  }
}

void
vga_update (machine_t *m)
{
  vga_t *v = m->vga;

  if (!v || !m->fb || !(v->ar_index & 0x20)) {
    return;                           /* no screen, or the display is blanked */
  }

  if (vbe_on (v) && v->vbe[3] != 4) {
    draw_vbe (m, v);
  } else if (v->gr[6] & 1) {
    draw_graphics (m, v);
  } else {
    draw_text (m, v);
  }
}

/* ----------------------------------------------------------------- PCI -- */
static void
vga_pci_changed (machine_t *m, pci_dev_t *d, unsigned reg)
{
  vga_t   *v    = d->opaque;
  uint32_t want = pci_bar_mem (d, 0);

  (void)reg;
  if (want < 0x80000000u || want > 0xfec00000u - VRAM_SIZE) {
    want = 0;                         /* not placed yet (or being sized) */
  }

  if (want == v->lfb_mapped) {
    return;
  }

  if (v->lfb_mapped) {
    uc_mem_unmap (m->uc, v->lfb_mapped, VRAM_SIZE);
    v->lfb_mapped = 0;
  }

  if (want && uc_mem_map_ptr (m->uc, want, VRAM_SIZE, UC_PROT_READ | UC_PROT_WRITE, v->vram) == UC_ERR_OK) {
    v->lfb_mapped = want;
  }
}

void
vga_reset (machine_t *m)
{
  vga_t   *v = m->vga;
  uint8_t *vram = v->vram;
  uint32_t mapped = v->lfb_mapped;
  pci_dev_t pci = v->pci;

  if (mapped) {
    uc_mem_unmap (m->uc, mapped, VRAM_SIZE);
  }

  memset (v, 0, sizeof (*v));
  v->vram        = vram;
  v->pci         = pci;
  v->pci.slot    = 2;
  v->pci.fn      = 0;
  v->pci.changed = vga_pci_changed;
  v->pci.opaque  = v;
  pci_init_config (&v->pci, 0x1234, 0x1111, 0x03000002, 0x1af4, 0x1100);
  v->pci.cfg[0x04]   = 0x00;
  v->pci.cfg[0x10]   = 0x08;          /* 32-bit, prefetchable */
  v->pci.bar_size[0] = VRAM_SIZE;
  v->msr      = 0x01;
  v->pel_mask = 0xff;
  v->vbe[0]   = 0xb0c5;
  v->st00     = 0x10;
  memset (v->vram, 0, VRAM_SIZE);
}

void
vga_init (machine_t *m)
{
  vga_t *v = m->vga = host_alloc (sizeof (vga_t));

  v->vram = host_alloc (VRAM_SIZE);
  vga_reset (m);
  pci_register (m, &v->pci);
  uc_mmio_map (m->uc, 0xa0000, 0x20000, vga_mem_read, m, vga_mem_write, m);
}
