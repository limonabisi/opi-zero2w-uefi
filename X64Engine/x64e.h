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
#ifdef X64E_UEFI
#include <unicorn.h>
#else
#include <unicorn/unicorn.h>
#endif

#ifdef X64E_UEFI
/* EDK2 PrintLib: %s is UTF-16 there; the shim rewrites %s to %a */
int x64e_snprintf (char *buf, size_t size, const char *fmt, ...);
#undef snprintf
#define snprintf x64e_snprintf
#endif

/* ------------------------------------------------------------ host --- */
uint64_t host_now_ns (void);
void     host_console_write (const char *buf, size_t len);
int      host_console_read (void);             /* -1 if nothing pending */
void     host_idle_until (uint64_t deadline_ns);/* sleep, wake on input  */
void     host_start_kick_timer (uc_engine *uc, uint32_t period_us);
void     host_log (const char *fmt, ...);
void    *host_alloc (size_t size);             /* zeroed, page aligned   */
struct machine;
void     host_poll_input (struct machine *m);   /* keyboard etc.         */
void     host_progress (const char *what, unsigned percent);

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
  uint8_t q[256];
  uint8_t head, tail;
  uint8_t out, obf, ccb, pending, kbd_param, scanning, last_was_cmd, irq_level;
} i8042_t;

void     i8042_init (machine_t *m);
uint32_t i8042_io_read (machine_t *m, uint16_t port);
void     i8042_io_write (machine_t *m, uint16_t port, uint8_t val);
void     i8042_key (machine_t *m, uint8_t code);

enum { KEY_UP = 0x100, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END, KEY_INSERT,
       KEY_DELETE, KEY_PGUP, KEY_PGDN, KEY_F1 };
void     kbd_type_char (machine_t *m, int c);
void     kbd_type_key (machine_t *m, int key);

/* ------------------------------------------------------------ pci ---- */
typedef struct {
  int      slot;
  uint8_t  cfg[256];
  uint32_t bar_size[6];
  uint8_t  bar_io[6];
} pci_dev_t;

void     pci_init (machine_t *m);
void     pci_register (machine_t *m, pci_dev_t *d);
void     pci_init_config (pci_dev_t *d, uint16_t vendor, uint16_t device, uint32_t class_rev,
                          uint16_t sub_vendor, uint16_t sub_device);
uint32_t pci_bar_io (pci_dev_t *d, int b);
uint32_t pci_io_read (machine_t *m, uint16_t port, int size);
void     pci_io_write (machine_t *m, uint16_t port, int size, uint32_t val);

/* ----------------------------------------------------- virtio-blk ---- */
typedef struct {
  pci_dev_t pci;
  void     *disk;
  uint64_t  disk_size;
  int       readonly;
  uint32_t  guest_features, pfn;
  uint16_t  last_avail;
  uint8_t   status, isr;
  uint64_t  requests;
} vblk_t;

int  host_disk_read (void *disk, uint64_t off, void *buf, uint32_t len);
int  host_disk_write (void *disk, uint64_t off, const void *buf, uint32_t len);
void vblk_init (machine_t *m, vblk_t *v, int slot, int irq, void *disk, uint64_t size, int ro);
int  vblk_io_read (machine_t *m, vblk_t *v, uint16_t off, int size, uint32_t *val);
void vblk_io_write (machine_t *m, vblk_t *v, uint16_t off, int size, uint32_t val);

/* ----------------------------------------------------- virtio-net ---- */
typedef struct {
  pci_dev_t pci;
  int       present;
  uint8_t   mac[6];
  uint32_t  guest_features, pfn[2];
  uint16_t  last_avail[2], qsel;
  uint8_t   status, isr;
  uint8_t   pending[1600];
  uint32_t  pending_len;
  uint64_t  rx_frames, tx_frames;
} vnet_t;

/* host network: raw Ethernet frames; recv returns the length, 0 if none */
int  host_net_send (const void *frame, uint32_t len);
int  host_net_recv (void *frame, uint32_t max);
extern int x64e_opt_nounpack;         /* 1: the x86 kernel unpacks its initrd itself */
extern int x64e_opt_nolookup;         /* 1: no inline block lookup */
extern int x64e_opt_nokchain;         /* 1: kernel blocks not chained across pages */
extern int x64e_opt_nobulk;           /* 1: no native REP MOVS / STOS */
void host_perf (uint64_t out[2]);
uint64_t host_free_mb (void);        /* memory the firmware still has free */     /* MMU faults handled, ns spent on them */
void vnet_init (machine_t *m, vnet_t *v, int slot, int irq, const uint8_t mac[6]);
void vnet_poll (machine_t *m, vnet_t *v);
int  vnet_io_read (machine_t *m, vnet_t *v, uint16_t off, int size, uint32_t *val);
void vnet_io_write (machine_t *m, vnet_t *v, uint16_t off, int size, uint32_t val);

/* ------------------------------------------------------ ISO / boot ---- */
/* a volume to boot from: an ISO9660 image or an ext2/3/4 partition */
enum { FS_ISO = 0, FS_EXT4 = 1 };

typedef struct {
  void    *disk;
  uint32_t root_lba, root_size;             /* ISO9660                  */
  int      type;
  uint64_t base;                            /* partition start (bytes)  */
  uint32_t blksz, first_data_block, inodes_per_group, inode_size, desc_size;
} iso_t;

int  ext4_probe (iso_t *fs, void *disk, uint64_t base);
int  ext4_lookup (iso_t *fs, const char *path, uint32_t *ino, uint64_t *size, int *is_dir);
int  ext4_read (iso_t *fs, uint32_t ino, uint64_t off, void *dst, uint64_t len);
/* find an installed Linux (ext2/3/4 with a GRUB config) on a disk */
int  disk_find_linux (void *disk, uint64_t disk_size, iso_t *fs);

#define MAX_BOOT_ENTRIES 16

typedef struct {
  char title[96];
  char kernel[160];
  char args[512];
  char initrd[4][160];
  int  ninitrd;
} bootent_t;

typedef struct {
  bootent_t e[MAX_BOOT_ENTRIES];
  int       count;
} bootlist_t;

void     host_free (void *p, size_t size);
int      iso_open (iso_t *iso, void *disk);
int      iso_lookup (iso_t *iso, const char *path, uint32_t *lba, uint32_t *size);
uint8_t *iso_read_file (iso_t *iso, const char *path, size_t *size);
int      iso_read_into (iso_t *iso, const char *path, uint8_t *dst, size_t max, size_t *size, const char *what);
int      bootcfg_scan (iso_t *iso, bootlist_t *bl);
int      iso_boot (machine_t *m, iso_t *iso, bootent_t *e, const char *extra_args);

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
  int         quit;
  int         console_to_kbd;     /* host console input goes to the PS/2 keyboard */
  pci_dev_t  *pci[8];
  int         npci;
  uint32_t    pci_addr;
  vblk_t      vblk[2];
  int         nvblk;
  vnet_t      vnet;
  /* linear framebuffer (guest physical FB_BASE) */
  uint8_t    *fb;
  uint32_t    fb_w, fb_h, fb_stride;
  uint8_t     cmos_index;
  uint8_t     cmos[128];
  uint64_t    boot_ns;
  uint64_t    initrd_addr;
  uint64_t    initrd_size;
  int         overlay;                /* F11: counters drawn on the screen */
  char        ov_line[4][200];
  uint64_t    kernel_end;             /* end of the memory the kernel image needs */
  /* statistics */
  uint64_t    io_exits, irqs, runs, idle_ns;
};

/* boot */
size_t initrd_unpack (const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);
void   linux_initrd_unpack (machine_t *m);
int linux_boot_setup (machine_t *m, const uint8_t *kernel, size_t ksize,
                      const uint8_t *initrd, size_t isize, const char *cmdline);

int  machine_init (machine_t *m, uint64_t ram_mb);
void machine_dump (machine_t *m, const char *why);
int  machine_add_disk (machine_t *m, void *disk, uint64_t size, int readonly);
int  machine_add_net (machine_t *m, const uint8_t mac[6]);
int  machine_set_fb (machine_t *m, void *fb, uint32_t w, uint32_t h, uint32_t stride);
#define FB_BASE  0xe0000000ULL
int  machine_v2p (machine_t *m, uint64_t va, uint64_t *pa);
void machine_run (machine_t *m);

#endif
