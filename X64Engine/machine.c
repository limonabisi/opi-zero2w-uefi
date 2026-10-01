/* PC machine: RAM, port I/O dispatch, main loop. */
#include <string.h>
#include <stdlib.h>
#include "x64e.h"

static uint32_t
io_in (uc_engine *uc, uint32_t port, int size, void *opaque)
{
  machine_t *m = opaque;
  uint32_t   v;

  (void)uc;
  m->io_exits++;
  switch (port) {
    case 0x20: case 0x21: case 0xa0: case 0xa1: case 0x4d0: case 0x4d1:
      v = pic_io_read (m, (uint16_t)port);
      break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x61:
      v = pit_io_read (m, (uint16_t)port);
      break;
    case 0x70: case 0x71:
      v = cmos_io_read (m, (uint16_t)port);
      break;
    case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
    case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
      v = uart_io_read (m, (uint16_t)port);
      break;
    case 0x60: case 0x64:
      v = i8042_io_read (m, (uint16_t)port);
      break;
    case 0x92:
      v = 0x02;                   /* A20 enabled */
      break;
    case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff: case 0xcf8:
      v = pci_io_read (m, (uint16_t)port, size);
      break;
    default:
      v = 0xffffffff;
      if (m->vnet.present) {
        uint32_t base = pci_bar_io (&m->vnet.pci, 0);
        if (base && port >= base && port < base + 0x40) {
          vnet_io_read (m, &m->vnet, (uint16_t)(port - base), size, &v);
          break;
        }
      }

      for (int i = 0; i < m->nvblk; i++) {
        uint32_t base = pci_bar_io (&m->vblk[i].pci, 0);
        if (base && port >= base && port < base + 0x40) {
          vblk_io_read (m, &m->vblk[i], (uint16_t)(port - base), size, &v);
          break;
        }
      }

      break;
  }

  if (size == 1) {
    v &= 0xff;
  } else if (size == 2) {
    v &= 0xffff;
  }

  return v;
}

static void
io_out (uc_engine *uc, uint32_t port, int size, uint32_t val, void *opaque)
{
  machine_t *m = opaque;

  (void)uc;
  (void)size;
  m->io_exits++;
  switch (port) {
    case 0x20: case 0x21: case 0xa0: case 0xa1: case 0x4d0: case 0x4d1:
      pic_io_write (m, (uint16_t)port, (uint8_t)val);
      break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x61:
      pit_io_write (m, (uint16_t)port, (uint8_t)val);
      break;
    case 0x70: case 0x71:
      cmos_io_write (m, (uint16_t)port, (uint8_t)val);
      break;
    case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
    case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
      uart_io_write (m, (uint16_t)port, (uint8_t)val);
      break;
    case 0x60: case 0x64:
      i8042_io_write (m, (uint16_t)port, (uint8_t)val);
      break;
    case 0x92:
      if (val & 1) {
        m->reset_request = 1;     /* fast reset */
      }

      break;
    case 0xcf9:
      if (val & 4) {
        m->reset_request = 1;
      }

      break;
    case 0xcf8: case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
      pci_io_write (m, (uint16_t)port, size, val);
      break;
    default:
      if (m->vnet.present) {
        uint32_t base = pci_bar_io (&m->vnet.pci, 0);
        if (base && port >= base && port < base + 0x40) {
          vnet_io_write (m, &m->vnet, (uint16_t)(port - base), size, val);
          break;
        }
      }

      for (int i = 0; i < m->nvblk; i++) {
        uint32_t base = pci_bar_io (&m->vblk[i].pci, 0);
        if (base && port >= base && port < base + 0x40) {
          vblk_io_write (m, &m->vblk[i], (uint16_t)(port - base), size, val);
          break;
        }
      }

      break;
  }
}

static uint32_t
parse_uint (const char *s)
{
  uint32_t v = 0;

  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (uint32_t)(*s++ - '0');
  }

  return v;
}

static uint64_t trace_ring[256];
static unsigned trace_pos;

static uint64_t trace_prev_start, trace_prev_end;
static uint64_t trace_from[256];

static void
trace_block (uc_engine *uc, uint64_t addr, uint32_t size, void *opaque)
{
  static int hi_count;
  if ((addr >> 63) && hi_count < 12) {
    machine_t *m = opaque;
    uint64_t   pa = 0;
    int        i;
    hi_count++;
    machine_v2p (m, addr, &pa);
    host_log ("blk %llx size %u pa %llx:", (unsigned long long)addr, size, (unsigned long long)pa);
    for (i = 0; i < (int)size && i < 24; i++) host_log (" %02x", m->ram[pa + i]);
    host_log ("\n");
  }
  /* record only non-sequential control transfers */
  if (addr != trace_prev_end) {
    trace_from[trace_pos & 255]    = trace_prev_start;
    trace_ring[trace_pos++ & 255] = addr;
  }

  trace_prev_start = addr;
  trace_prev_end   = addr + size;
}

void
machine_trace_dump (void)
{
  unsigned i;

  host_log ("last jumps (from block -> to):");
  for (i = 0; i < 96; i++) {
    host_log ("%s%llx->%llx", (i % 3) ? "  " : "\n  ",
              (unsigned long long)trace_from[(trace_pos - 96 + i) & 255],
              (unsigned long long)trace_ring[(trace_pos - 96 + i) & 255]);
  }

  host_log ("\n");
}

static uint64_t stop_every, stop_count;

static void
stop_block (uc_engine *uc, uint64_t addr, uint32_t size, void *opaque)
{
  if (++stop_count % stop_every == 0) {
    uc_emu_stop (uc);
  }
}

static bool
mem_invalid (uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
             int64_t value, void *opaque)
{
  uint64_t rip = uc_x86_get_pc64 (uc);

  (void)opaque;
  host_log ("x64e: unmapped %s at 0x%llx size %d (rip 0x%llx)\n",
            type == UC_MEM_WRITE_UNMAPPED ? "write" :
            type == UC_MEM_FETCH_UNMAPPED ? "fetch" : "read",
            (unsigned long long)addr, size, (unsigned long long)rip);
  (void)value;
  return false;
}

/* guest virtual -> physical with the current page tables (4-level) */
int
machine_v2p (machine_t *m, uint64_t va, uint64_t *pa)
{
  uint64_t s[23], table, e;
  int      level, shift;

  uc_x86_get_state64 (m->uc, s);
  if (!(s[18] & 0x80000000ULL)) {
    *pa = va & 0xffffffffULL;
    return *pa < m->ram_size ? 0 : -1;
  }

  table = s[20] & 0x000ffffffffff000ULL;
  for (level = 4; level >= 1; level--) {
    shift = 12 + 9 * (level - 1);
    if (table + 4096 > m->ram_size) {
      return -1;
    }

    e = *(uint64_t *)(m->ram + table + ((va >> shift) & 511) * 8);
    if (!(e & 1)) {
      return -1;
    }

    if (level > 1 && level < 4 && (e & 0x80)) {
      *pa = (e & 0x000fffffffffe000ULL & ~((1ULL << shift) - 1)) | (va & ((1ULL << shift) - 1));
      return *pa < m->ram_size ? 0 : -1;
    }

    table = e & 0x000ffffffffff000ULL;
  }

  *pa = table | (va & 0xfff);
  return *pa < m->ram_size ? 0 : -1;
}

static int
vread (machine_t *m, uint64_t va, void *buf, size_t n)
{
  uint8_t *b = buf;
  uint64_t pa;
  size_t   i;

  for (i = 0; i < n; i++) {
    if (machine_v2p (m, va + i, &pa)) {
      return -1;
    }

    b[i] = m->ram[pa];
  }

  return 0;
}

void
machine_dump (machine_t *m, const char *why)
{
  static const char *n[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                               "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
  uint64_t s[23], v;
  uint8_t  code[32];
  int      i;

  uc_x86_get_state64 (m->uc, s);
  host_log ("---- x64e state (%s) ----\n", why);
  for (i = 0; i < 16; i++) {
    host_log ("%-3s %016llx%s", n[i], (unsigned long long)s[i], (i & 3) == 3 ? "\n" : "  ");
  }

  host_log ("rip %016llx  rfl %08llx  cr0 %llx cr2 %llx cr3 %llx cr4 %llx efer %llx\n",
            (unsigned long long)s[16], (unsigned long long)s[17], (unsigned long long)s[18],
            (unsigned long long)s[19], (unsigned long long)s[20], (unsigned long long)s[21],
            (unsigned long long)s[22]);
  if (vread (m, s[16] - 16, code, sizeof (code)) == 0) {
    host_log ("code at rip-16:");
    for (i = 0; i < 32; i++) {
      host_log (" %02x", code[i]);
    }

    host_log ("\n");
  }

  {
    uint64_t pa;
    if (machine_v2p (m, s[16], &pa) == 0) {
      host_log ("rip -> phys %llx\n", (unsigned long long)pa);
    } else {
      host_log ("rip does not translate\n");
    }
  }

  host_log ("stack:");
  for (i = 0; i < 12; i++) {
    if (vread (m, s[4] + i * 8, &v, 8) != 0) {
      break;
    }

    host_log (" %llx", (unsigned long long)v);
  }

  host_log ("\n");
}

int
machine_init (machine_t *m, uint64_t ram_mb)
{
  uc_err     err;
  uc_hook    h;

  memset (m, 0, sizeof (*m));
  err = uc_open (UC_ARCH_X86, UC_MODE_32, &m->uc);
  if (err) {
    host_log ("x64e: uc_open: %s\n", uc_strerror (err));
    return -1;
  }

  uc_ctl_set_cpu_model (m->uc, UC_CPU_X86_QEMU64);
  uc_x86_set_system_mode (m->uc, getenv ("X64E_NOPCID") ? 3 : 1);
  uc_ctl_exits_enable (m->uc);          /* no stop address: we stop by kicks */

  m->ram_size = ram_mb << 20;
  m->ram      = host_alloc (m->ram_size);
  if (!m->ram) {
    return -1;
  }

  /* RAM below 3 GiB only for now: 0 - ram_size */
  err = uc_mem_map_ptr (m->uc, 0, m->ram_size, UC_PROT_ALL, m->ram);
  if (err) {
    host_log ("x64e: mem map: %s\n", uc_strerror (err));
    return -1;
  }

  uc_hook_add (m->uc, &h, UC_HOOK_INSN, io_in, m, 1, 0, UC_X86_INS_IN);
  uc_hook_add (m->uc, &h, UC_HOOK_INSN, io_out, m, 1, 0, UC_X86_INS_OUT);
  uc_hook_add (m->uc, &h, UC_HOOK_MEM_UNMAPPED, mem_invalid, m, 1, 0);
  if (getenv ("X64E_STOPEVERY")) {
    stop_every = (uint64_t)parse_uint (getenv ("X64E_STOPEVERY"));
    uc_hook_add (m->uc, &h, UC_HOOK_BLOCK, stop_block, m, 1, 0);
  }

  if (getenv ("X64E_BTRACE")) {
    uc_hook_add (m->uc, &h, UC_HOOK_BLOCK, trace_block, m, 1, 0);
  }

  m->cmos[0x0a] = 0x26;
  m->cmos[0x0b] = 0x02;
  pic_init (m);
  pit_init (m);
  uart_init (m);
  i8042_init (m);
  pci_init (m);
  return 0;
}

/* 32 bpp BGRX framebuffer shared with the host (GOP memory on the board) */
int
machine_set_fb (machine_t *m, void *fb, uint32_t w, uint32_t h, uint32_t stride)
{
  uint64_t size = ((uint64_t)stride * h + 0xffff) & ~0xffffULL;
  uc_err   err  = uc_mem_map_ptr (m->uc, FB_BASE, size, UC_PROT_READ | UC_PROT_WRITE, fb);

  if (err) {
    host_log ("x64e: framebuffer map: %s\n", uc_strerror (err));
    return -1;
  }

  m->fb        = fb;
  m->fb_w      = w;
  m->fb_h      = h;
  m->fb_stride = stride;
  return 0;
}

/* attach a disk as virtio-blk (slots 2, 3; IRQ 11, 10) */
int
machine_add_disk (machine_t *m, void *disk, uint64_t size, int readonly)
{
  static const int irqs[2] = { 11, 10 };

  if (m->nvblk >= 2) {
    return -1;
  }

  vblk_init (m, &m->vblk[m->nvblk], 2 + m->nvblk, irqs[m->nvblk], disk, size, readonly);
  m->nvblk++;
  return 0;
}

/* attach the network card (virtio-net, slot 4, IRQ 9) */
int
machine_add_net (machine_t *m, const uint8_t mac[6])
{
  vnet_init (m, &m->vnet, 4, 9, mac);
  return 0;
}

/* X64E_PCPROF: sample the guest PC at every return from the CPU loop */
#define PROF_N 8192
static uint64_t prof_pc[PROF_N];
static uint32_t prof_cnt[PROF_N];

static void
prof_add (uint64_t pc)
{
  uint32_t h = (uint32_t)((pc >> 4) * 2654435761u) % PROF_N, i;

  pc &= ~0xfULL;
  for (i = 0; i < 64; i++, h = (h + 1) % PROF_N) {
    if (prof_pc[h] == pc || prof_pc[h] == 0) {
      prof_pc[h] = pc;
      prof_cnt[h]++;
      return;
    }
  }
}

static void
prof_dump (void)
{
  int i, j;

  for (j = 0; j < 60; j++) {
    int best = -1;

    for (i = 0; i < PROF_N; i++) {
      if (prof_cnt[i] && (best < 0 || prof_cnt[i] > prof_cnt[best])) {
        best = i;
      }
    }

    if (best < 0) {
      break;
    }

    host_log ("PROF %llx %u\n", (unsigned long long)prof_pc[best], prof_cnt[best]);
    prof_cnt[best] = 0;
  }
}

void
machine_run (machine_t *m)
{
  int prof = getenv ("X64E_PCPROF") != NULL;
  uint64_t pc, now, next;
  uc_err   err;
  uint64_t last_report = 0, last_idle = 0;
  int      trace = getenv ("X64E_TRACE") != NULL;

  pc = uc_x86_get_pc64 (m->uc);
  if (!getenv ("X64E_NOKICK")) {
    host_start_kick_timer (m->uc, getenv ("X64E_KICK_US") ? parse_uint (getenv ("X64E_KICK_US")) : 1000);
  }
  for (;;) {
    now = host_now_ns ();
    pit_tick (m, now);
    uart_poll_input (m);
    host_poll_input (m);
    vnet_poll (m, &m->vnet);
    if (m->reset_request || m->quit) {
      host_log ("\nx64e: %s\n", m->quit ? "stopped" : "guest reset");
      return;
    }

    m->runs++;
    err = uc_emu_start (m->uc, pc, 0xfffffffffffff001ULL, 0, 0);
    pc  = uc_x86_get_pc64 (m->uc);
    if (prof) {
      prof_add (pc);
    }
    if (trace && m->runs > 2000000 && m->runs < 2000010) {
      machine_dump (m, "trace");
    }

    if (uc_x86_reset_requested (m->uc)) {
      host_log ("\nx64e: triple fault / reset\n");
      machine_dump (m, "reset");
      if (getenv ("X64E_BTRACE")) {
        machine_trace_dump ();
      }
      return;
    }

    if (err) {
      host_log ("\nx64e: CPU stopped: %s\n", uc_strerror (err));
      machine_dump (m, "fatal");
      return;
    }

    if (uc_x86_is_halted (m->uc)) {
      uint64_t fl = 0;

      /* HLT with interrupts off: the guest powered off or stopped for good */
      uc_reg_read (m->uc, UC_X86_REG_EFLAGS, &fl);
      if (!(fl & 0x200)) {
        host_log ("\nx64e: the x86 system halted (powered off)\n");
        return;
      }

      static int dumped;
      if (!dumped && getenv ("X64E_DUMP")) {
        dumped = 1;
        machine_dump (m, "first halt");
      }
      now  = host_now_ns ();
      next = pit_next_event (m);
      if (next > now + 10000000ULL) {
        next = now + 10000000ULL;
      }

      if (next > now) {
        host_idle_until (next);
        m->idle_ns += host_now_ns () - now;
      }
    }

    now = host_now_ns ();
    if (now - last_report > 30000000000ULL) {
      uint64_t rip = uc_x86_get_pc64 (m->uc);

      {
        uint64_t sh[10];

        uc_x64e_shadow_stats (m->uc, sh);
        if (sh[0] || sh[1]) {
          host_log ("\n[x64e] shadow: %llu fills, %llu slow (%llu #PF, %llu io, %llu code), %llu wprot, %llu drops,"
                    " %llu large inv, %llu tables, %llu rmap\n",
                    (unsigned long long)sh[0], (unsigned long long)sh[1], (unsigned long long)sh[6],
                    (unsigned long long)sh[7], (unsigned long long)sh[8], (unsigned long long)sh[2],
                    (unsigned long long)sh[3], (unsigned long long)sh[9], (unsigned long long)sh[4],
                    (unsigned long long)sh[5]);
        }
      }
      host_log ("\n[x64e] %llus: %llu runs, %llu io, %llu irqs, idle %llu%%, rip %llx\n",
                (unsigned long long)(now / 1000000000ULL),
                (unsigned long long)m->runs, (unsigned long long)m->io_exits,
                (unsigned long long)m->irqs,
                (unsigned long long)((m->idle_ns - last_idle) * 100 / (now - last_report + 1)),
                (unsigned long long)rip);
      last_idle   = m->idle_ns;
      if (prof) {
        prof_dump ();
      }
      last_report = now;
    }
  }
}
