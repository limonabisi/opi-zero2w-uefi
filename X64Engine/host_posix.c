/* Linux host for development: x64e KERNEL [INITRD] [CMDLINE] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "x64e.h"

static uint64_t   t0;
static uc_engine *kick_uc;

uint64_t
host_now_ns (void)
{
  struct timespec ts;

  static uint64_t scale;

  if (!scale) {
    scale = getenv ("X64E_TIMESCALE") ? strtoull (getenv ("X64E_TIMESCALE"), NULL, 0) : 1;
  }

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec - t0) * scale;
}

void
host_console_write (const char *buf, size_t len)
{
  ssize_t r = write (1, buf, len);
  (void)r;
}

int
host_console_read (void)
{
  unsigned char c;
  struct pollfd p = { 0, POLLIN, 0 };

  if (poll (&p, 1, 0) > 0 && read (0, &c, 1) == 1) {
    return c;
  }

  return -1;
}

void
host_progress (const char *what, unsigned percent)
{
  (void)what;
  (void)percent;
}

void
host_poll_input (machine_t *m)
{
  (void)m;
}

static machine_t *fb_machine;

static void
fb_dump (void)
{
  static uint64_t last;
  uint64_t        now = host_now_ns ();
  FILE           *f;
  uint32_t        x, y;

  if (!fb_machine || !fb_machine->fb || !getenv ("X64E_FBDUMP") || now - last < 2000000000ULL) {
    return;
  }

  last = now;
  f    = fopen (getenv ("X64E_FBDUMP"), "wb");
  if (!f) {
    return;
  }

  fprintf (f, "P6\n%u %u\n255\n", fb_machine->fb_w, fb_machine->fb_h);
  for (y = 0; y < fb_machine->fb_h; y++) {
    for (x = 0; x < fb_machine->fb_w; x++) {
      uint8_t *p = fb_machine->fb + y * fb_machine->fb_stride + x * 4;
      fputc (p[2], f);
      fputc (p[1], f);
      fputc (p[0], f);
    }
  }

  fclose (f);
}

void
host_idle_until (uint64_t deadline)
{
  fb_dump ();
  uint64_t      now = host_now_ns ();
  struct pollfd p   = { 0, POLLIN, 0 };

  if (deadline > now) {
    poll (&p, 1, (int)((deadline - now + 999999) / 1000000));
  }
}

static int kick_mode;                     /* 0 stop, 1 kick only, 2 nothing */

static void
on_kick (int sig)
{
  (void)sig;
  if (kick_uc && kick_mode == 1) {
    uc_x86_kick (kick_uc);
  } else if (kick_uc && kick_mode == 0) {
    uc_emu_stop (kick_uc);
  }
}

void
host_start_kick_timer (uc_engine *uc, uint32_t period_us)
{
  struct itimerval it;
  struct sigaction sa;

  kick_uc   = uc;
  kick_mode = getenv ("X64E_KICK_ONLY") ? 1 : getenv ("X64E_KICK_NOOP") ? 2 : 0;
  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = on_kick;
  sa.sa_flags   = SA_RESTART;
  sigaction (SIGALRM, &sa, NULL);
  it.it_interval.tv_sec  = 0;
  it.it_interval.tv_usec = period_us;
  it.it_value            = it.it_interval;
  setitimer (ITIMER_REAL, &it, NULL);
}

void
host_log (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
}

void
host_free (void *p, size_t size)
{
  munmap (p, size);
}

void *
host_alloc (size_t size)
{
  void *p = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  return p == MAP_FAILED ? NULL : p;
}

int
host_disk_read (void *disk, uint64_t off, void *buf, uint32_t len)
{
  return pread ((int)(intptr_t)disk, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

int
host_disk_write (void *disk, uint64_t off, const void *buf, uint32_t len)
{
  return pwrite ((int)(intptr_t)disk, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

/*
 * X64E_NET=LPORT:RPORT - raw Ethernet frames over UDP on 127.0.0.1, one
 * frame per datagram (QEMU "-netdev socket,udp=" compatible), e.g. against
 *   qemu-system-aarch64 -M none -netdev user,id=u -netdev socket,id=s,
 *     udp=127.0.0.1:LPORT,localaddr=127.0.0.1:RPORT
 *     -netdev hubport,id=h1,hubid=0,netdev=u -netdev hubport,id=h2,hubid=0,netdev=s
 */
static int                net_fd = -1;
static struct sockaddr_in net_peer;

static int
net_open (const char *spec)
{
  struct sockaddr_in me;
  unsigned           lport = 0, rport = 0;

  if (sscanf (spec, "%u:%u", &lport, &rport) != 2) {
    return -1;
  }

  net_fd = socket (AF_INET, SOCK_DGRAM, 0);
  memset (&me, 0, sizeof (me));
  me.sin_family      = AF_INET;
  me.sin_port        = htons (lport);
  me.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  if (net_fd < 0 || bind (net_fd, (struct sockaddr *)&me, sizeof (me))) {
    perror ("x64e: net");
    return -1;
  }

  fcntl (net_fd, F_SETFL, O_NONBLOCK);
  net_peer          = me;
  net_peer.sin_port = htons (rport);
  return 0;
}

int
host_net_send (const void *frame, uint32_t len)
{
  if (net_fd < 0) {
    return -1;
  }

  return sendto (net_fd, frame, len, 0, (struct sockaddr *)&net_peer, sizeof (net_peer)) == (ssize_t)len ? 0 : -1;
}

int x64e_opt_nobulk, x64e_opt_nokchain, x64e_opt_nolookup, x64e_opt_nounpack;

void
host_perf (uint64_t out[2])
{
  out[0] = out[1] = 0;
}

uint64_t
host_free_mb (void)
{
  return 0;
}

int
host_net_recv (void *frame, uint32_t max)
{
  ssize_t n;

  if (net_fd < 0) {
    return 0;
  }

  n = recv (net_fd, frame, max, 0);
  return n > 0 ? (int)n : 0;
}

static uint8_t *
load_file (const char *path, size_t *size)
{
  FILE    *f = fopen (path, "rb");
  uint8_t *b;
  long     n;

  if (!f) {
    perror (path);
    exit (1);
  }

  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  b = malloc (n);
  if (fread (b, 1, n, f) != (size_t)n) {
    exit (1);
  }

  fclose (f);
  *size = n;
  return b;
}

int
main (int argc, char **argv)
{
  static machine_t m;
  struct timespec  ts;
  uint8_t         *k = NULL, *ird = NULL;
  size_t           ks = 0, is = 0;
  const char      *cmd = "console=ttyS0 earlyprintk=serial nolapic noapic nokaslr";
  struct termios   tio;
  int              ram_mb = getenv ("X64E_RAM") ? atoi (getenv ("X64E_RAM")) : 512;

  if (argc < 2 && !getenv ("X64E_ISO") && !getenv ("X64E_HD")) {
    fprintf (stderr, "usage: %s bzImage [initrd] [cmdline]\n       X64E_ISO=file.iso [X64E_ENTRY=n] %s\n", argv[0], argv[0]);
    return 1;
  }

  x64e_opt_nobulk   = getenv ("X64E_NOBULK") != NULL;
  x64e_opt_nokchain = getenv ("X64E_NOKCHAIN") != NULL;
  x64e_opt_nolookup = getenv ("X64E_NOLOOKUP") != NULL;
  x64e_opt_nounpack = getenv ("X64E_NOUNPACK") != NULL;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  t0 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

  if (argc > 1) {
    k = load_file (argv[1], &ks);
  }

  if (argc > 2 && argv[2][0]) {
    ird = load_file (argv[2], &is);
  }

  if (argc > 3) {
    cmd = argv[3];
  }

  if (machine_init (&m, ram_mb)) {
    return 1;
  }

  clock_gettime (CLOCK_REALTIME, &ts);
  m.boot_ns = (uint64_t)ts.tv_sec * 1000000000ULL;

  /* X64E_FB=WxH: framebuffer, dumped to X64E_FBDUMP (PPM) every 2 s */
  if (getenv ("X64E_FB")) {
    unsigned w = 1024, h = 768;

    sscanf (getenv ("X64E_FB"), "%ux%u", &w, &h);
    machine_set_fb (&m, host_alloc ((size_t)w * h * 4 + 0x10000), w, h, w * 4);
  }

  /* X64E_DISK=file[:ro] (comma separated, up to two) */
  if (getenv ("X64E_DISK")) {
    char *list = strdup (getenv ("X64E_DISK")), *tok, *save;

    for (tok = strtok_r (list, ",", &save); tok; tok = strtok_r (NULL, ",", &save)) {
      int   ro = 0, fd;
      char *c  = strrchr (tok, ':');

      if (c && strcmp (c, ":ro") == 0) {
        *c = 0;
        ro = 1;
      }

      fd = open (tok, ro ? O_RDONLY : O_RDWR);
      if (fd < 0) {
        perror (tok);
        return 1;
      }

      machine_add_disk (&m, (void *)(intptr_t)fd, (uint64_t)lseek (fd, 0, SEEK_END), ro);
      fprintf (stderr, "x64e: disk %s%s\n", tok, ro ? " (read-only)" : "");
    }
  }

  if (getenv ("X64E_NET") && net_open (getenv ("X64E_NET")) == 0) {
    static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

    machine_add_net (&m, mac);
    fprintf (stderr, "x64e: network on udp %s\n", getenv ("X64E_NET"));
  }

  /* X64E_HD=file: the hard disk (vda, read-write); boots its installed Linux without X64E_ISO */
  if (getenv ("X64E_HD")) {
    int fd = open (getenv ("X64E_HD"), O_RDWR);

    if (fd < 0) {
      perror (getenv ("X64E_HD"));
      return 1;
    }

    machine_add_disk (&m, (void *)(intptr_t)fd, (uint64_t)lseek (fd, 0, SEEK_END), 0);
    if (!getenv ("X64E_ISO")) {
      static bootlist_t hbl;
      iso_t             fs;
      int               i;

      if (!disk_find_linux ((void *)(intptr_t)fd, (uint64_t)lseek (fd, 0, SEEK_END), &fs) ||
          bootcfg_scan (&fs, &hbl) == 0) {
        fprintf (stderr, "x64e: no installed system on %s\n", getenv ("X64E_HD"));
        return 1;
      }

      for (i = 0; i < hbl.count; i++) {
        fprintf (stderr, "  [%d] %s  (%s)\n", i, hbl.e[i].title, hbl.e[i].kernel);
      }

      i = getenv ("X64E_ENTRY") ? atoi (getenv ("X64E_ENTRY")) : 0;
      if (iso_boot (&m, &fs, &hbl.e[i < hbl.count ? i : 0], getenv ("X64E_APPEND") ? getenv ("X64E_APPEND") : "console=ttyS0")) {
        return 1;
      }
    }
  }

  if (getenv ("X64E_ISO")) {
    static bootlist_t bl;
    iso_t      iso;
    int        fd = open (getenv ("X64E_ISO"), O_RDONLY), i, sel;

    if (fd < 0) {
      perror (getenv ("X64E_ISO"));
      return 1;
    }

    machine_add_disk (&m, (void *)(intptr_t)fd, (uint64_t)lseek (fd, 0, SEEK_END), 1);
    if (iso_open (&iso, (void *)(intptr_t)fd) || bootcfg_scan (&iso, &bl) == 0) {
      fprintf (stderr, "x64e: no boot entries found on the ISO\n");
      return 1;
    }

    for (i = 0; i < bl.count; i++) {
      fprintf (stderr, "  [%d] %s\n", i, bl.e[i].title);
    }

    sel = getenv ("X64E_ENTRY") ? atoi (getenv ("X64E_ENTRY")) : 0;
    if (sel < 0 || sel >= bl.count) {
      sel = 0;
    }

    if (iso_boot (&m, &iso, &bl.e[sel], getenv ("X64E_APPEND") ? getenv ("X64E_APPEND") : "console=ttyS0 nolapic noapic")) {
      return 1;
    }
  } else if (!getenv ("X64E_HD") && linux_boot_setup (&m, k, ks, ird, is, cmd)) {
    return 1;
  }

  if (ird && !getenv ("X64E_ISO") && !getenv ("X64E_HD")) {
    linux_initrd_unpack (&m);
  }

  if (isatty (0)) {
    tcgetattr (0, &tio);
    cfmakeraw (&tio);
    tio.c_oflag |= OPOST | ONLCR;
    tcsetattr (0, TCSANOW, &tio);
  }

  m.console_to_kbd = getenv ("X64E_KBD") != NULL;
  m.overlay        = getenv ("X64E_OVERLAY") != NULL;
  fb_machine = &m;
  machine_run (&m);
  return 0;
}
