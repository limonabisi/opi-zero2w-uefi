/*
 * Direct Linux boot (x86 32-bit boot protocol): the protected-mode kernel is
 * entered at code32_start with flat segments, ESI = boot_params. The kernel's
 * own startup_32 switches to long mode.
 */
#include <string.h>
#include "x64e.h"

#define ZERO_PAGE    0x7000
#define GDT_ADDR     0x6000
#define CMDLINE_ADDR 0x20000
#define EBDA_START   0x9fc00

static uint32_t rd32 (const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16 (const uint8_t *p) { return p[0] | (p[1] << 8); }
static void     wr32 (uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void     wr16 (uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void     wr64 (uint8_t *p, uint64_t v) { wr32 (p, (uint32_t)v); wr32 (p + 4, (uint32_t)(v >> 32)); }

static void
e820_add (uint8_t *bp, uint64_t addr, uint64_t size, uint32_t type)
{
  uint8_t  n = bp[0x1e8];
  uint8_t *e = bp + 0x2d0 + n * 20;

  wr64 (e, addr);
  wr64 (e + 8, size);
  wr32 (e + 16, type);
  bp[0x1e8] = n + 1;
}

int
linux_boot_setup (machine_t *m, const uint8_t *k, size_t ksize,
                  const uint8_t *initrd, size_t isize, const char *cmdline)
{
  uint8_t  *bp = m->ram + ZERO_PAGE;
  uint32_t  setup_sects, hdr_len, pm_off, pm_size;
  uint64_t  load, pref, initrd_max, initrd_addr = 0;
  uint16_t  version;
  uint64_t  gdt[4] = { 0, 0, 0x00cf9a000000ffffULL, 0x00cf92000000ffffULL };
  uc_x86_mmr gdtr = { 0 };
  uint32_t  v32;
  uint16_t  sel;
  size_t    cl;

  if (ksize < 0x300 || rd32 (k + 0x202) != 0x53726448) {  /* "HdrS" */
    host_log ("x64e: not a bzImage\n");
    return -1;
  }

  version = rd16 (k + 0x206);
  if (version < 0x020c) {
    host_log ("x64e: boot protocol %x too old\n", version);
    return -1;
  }

  setup_sects = k[0x1f1] ? k[0x1f1] : 4;
  pm_off      = (setup_sects + 1) * 512;
  pm_size     = (uint32_t)(ksize - pm_off);

  /* zero page with a copy of the setup header */
  memset (bp, 0, 4096);
  hdr_len = 0x202 + k[0x201] - 0x1f1;
  memcpy (bp + 0x1f1, k + 0x1f1, hdr_len);

  bp[0x210] = 0xff;                                      /* type_of_loader */
  bp[0x211] |= 0x80 | 0x01;                              /* CAN_USE_HEAP, LOADED_HIGH */
  wr16 (bp + 0x224, 0xfe00);                             /* heap_end_ptr */

  cl = strlen (cmdline) + 1;
  memcpy (m->ram + CMDLINE_ADDR, cmdline, cl);
  wr32 (bp + 0x228, CMDLINE_ADDR);

  /* kernel at its preferred address (it is relocatable anyway) */
  pref = ((uint64_t)rd32 (k + 0x25c) << 32) | rd32 (k + 0x258);
  load = pref ? pref : 0x100000;
  if (load + rd32 (k + 0x260) > m->ram_size) {
    load = 0x100000;
  }

  memcpy (m->ram + load, k + pm_off, pm_size);
  wr32 (bp + 0x214, (uint32_t)load);                     /* code32_start */

  if (initrd && isize) {
    initrd_max  = rd32 (k + 0x22c);
    if (initrd_max == 0 || initrd_max >= m->ram_size) {
      initrd_max = m->ram_size - 1;
    }

    initrd_addr = (initrd_max + 1 - isize) & ~0xfffULL;
    if (initrd_addr < load + rd32 (k + 0x260)) {
      host_log ("x64e: not enough memory for the initrd\n");
      return -1;
    }

    memcpy (m->ram + initrd_addr, initrd, isize);
    wr32 (bp + 0x218, (uint32_t)initrd_addr);
    wr32 (bp + 0x21c, (uint32_t)isize);
  }

  e820_add (bp, 0, EBDA_START, 1);
  e820_add (bp, EBDA_START, 0x100000 - EBDA_START, 2);
  e820_add (bp, 0x100000, m->ram_size - 0x100000, 1);

  memcpy (m->ram + GDT_ADDR, gdt, sizeof (gdt));

  /* CPU state: 32-bit flat protected mode, interrupts off */
  gdtr.base  = GDT_ADDR;
  gdtr.limit = sizeof (gdt) - 1;
  uc_reg_write (m->uc, UC_X86_REG_GDTR, &gdtr);
  sel = 0x10;
  uc_reg_write (m->uc, UC_X86_REG_CS, &sel);
  sel = 0x18;
  uc_reg_write (m->uc, UC_X86_REG_DS, &sel);
  uc_reg_write (m->uc, UC_X86_REG_ES, &sel);
  uc_reg_write (m->uc, UC_X86_REG_SS, &sel);
  uc_reg_write (m->uc, UC_X86_REG_FS, &sel);
  uc_reg_write (m->uc, UC_X86_REG_GS, &sel);
  v32 = ZERO_PAGE;
  uc_reg_write (m->uc, UC_X86_REG_ESI, &v32);
  v32 = 0;
  uc_reg_write (m->uc, UC_X86_REG_EBP, &v32);
  uc_reg_write (m->uc, UC_X86_REG_EDI, &v32);
  uc_reg_write (m->uc, UC_X86_REG_EBX, &v32);
  v32 = 0x2;                                             /* EFLAGS: IF=0 */
  uc_reg_write (m->uc, UC_X86_REG_EFLAGS, &v32);
  v32 = (uint32_t)load;
  uc_reg_write (m->uc, UC_X86_REG_EIP, &v32);

  host_log ("x64e: kernel %u KB at 0x%llx, protocol %x, initrd %u KB at 0x%llx\n",
            (unsigned)(ksize / 1024), (unsigned long long)load, version,
            (unsigned)(isize / 1024), (unsigned long long)initrd_addr);
  return 0;
}
