/*
 * x64 Engine - an x86-64 PC for the Orange Pi Zero 2W firmware.
 *
 * The machine model is plain C with no OS dependencies; everything the
 * host must provide (time, console, files, idle) goes through host_*().
 * host_posix.c runs it on Linux for development, host_uefi.c in firmware.
 */
#ifndef X64E_H
#define X64E_H

#include <stdint.h>
#include <stddef.h>
#include <unicorn/unicorn.h>

/* ------------------------------------------------------------ host --- */
uint64_t host_now_ns (void);
void     host_console_write (const char *buf, size_t len);
int      host_console_read (void);             /* -1 if nothing pending */
void     host_idle_until (uint64_t deadline_ns);/* sleep, wake on input  */
void     host_start_kick_timer (uc_engine *uc, uint32_t period_us);
void     host_log (const char *fmt, ...);
void    *host_alloc (size_t size);             /* zeroed, page aligned   */

/* ------------------------------------------------------------ irq ---- */
typedef struct {
  uint8_t  irr, isr, imr, elcr;
  uint8_t  base, icw_step, icw4, read_isr, auto_eoi, special_mask;
  uint8_t  last_level;                  /* pin levels for edge detection */
  uint8_t  rotate_on_aeoi, lowest_prio, init4, special_fully_nested;
} pic_t;

typedef struct machine machine_t;

void pic_init (machine_t *m);
void pic_set_irq (machine_t *m, int irq, int level);
int  pic_ack (void *opaque);          /* unicorn callback, returns vector */
uint32_t pic_io_read (machine_t *m, uint16_t port);
void     pic_io_write (machine_t *m, uint16_t port, uint8_t val);

/* ------------------------------------------------------------ pit ---- */
typedef struct {
  uint16_t reload;          /* 0 means 65536                              */
  uint8_t  mode, rw, bcd;
  uint8_t  write_lsb_next, read_lsb_next, write_latch;
  uint8_t  latched, latch_lsb_next;
  uint16_t latch_value;
  uint8_t  status_latched, status;
  uint8_t  gate, null_count;
  uint64_t start_ns;        /* time the current count started            */
  uint64_t next_irq_ns;     /* channel 0 only                           */
  uint8_t  armed;
} pit_chan_t;

void     pit_init (machine_t *m);
uint32_t pit_io_read (machine_t *m, uint16_t port);
void     pit_io_write (machine_t *m, uint16_t port, uint8_t val);
int      pit_out (machine_t *m, int ch, uint64_t now);
void     pit_tick (machine_t *m, uint64_t now);      /* raise IRQ0 if due */
uint64_t pit_next_event (machine_t *m);

/* ----------------------------------------------------------- uart ---- */
typedef struct {
  uint8_t ier, lcr, mcr, scr, fcr, dll, dlm;
  uint8_t thre_pending;
  uint8_t rx[256];
  uint16_t rx_head, rx_tail;
  uint8_t  irq_level;
} uart_t;

void     uart_init (machine_t *m);
uint32_t uart_io_read (machine_t *m, uint16_t port);
void     uart_io_write (machine_t *m, uint16_t port, uint8_t val);
void     uart_poll_input (machine_t *m);

/* --------------------------------------------------------- i8042 ---- */
typedef struct {
  uint8_t q[64];
  uint8_t head, tail;
  uint8_t out, obf, ccb, pending, kbd_param, scanning, last_was_cmd, irq_level;
} i8042_t;

void     i8042_init (machine_t *m);
uint32_t i8042_io_read (machine_t *m, uint16_t port);
void     i8042_io_write (machine_t *m, uint16_t port, uint8_t val);
void     i8042_key (machine_t *m, uint8_t code);

/* ----------------------------------------------------------- cmos ---- */
uint32_t cmos_io_read (machine_t *m, uint16_t port);
void     cmos_io_write (machine_t *m, uint16_t port, uint8_t val);

/* -------------------------------------------------------- machine ---- */
struct machine {
  uc_engine  *uc;
  uint8_t    *ram;
  uint64_t    ram_size;
  pic_t       pic[2];
  int         cpu_irq_level;
  pit_chan_t  pit[3];
  uint8_t     port61;
  uart_t      uart;
  i8042_t     kbd;
  int         reset_request;
  uint8_t     cmos_index;
  uint8_t     cmos[128];
  uint64_t    boot_ns;
  /* statistics */
  uint64_t    io_exits, irqs, runs;
};

/* boot */
int linux_boot_setup (machine_t *m, const uint8_t *kernel, size_t ksize,
                      const uint8_t *initrd, size_t isize, const char *cmdline);

int  machine_init (machine_t *m, uint64_t ram_mb);
void machine_dump (machine_t *m, const char *why);
int  machine_v2p (machine_t *m, uint64_t va, uint64_t *pa);
void machine_run (machine_t *m);

#endif
