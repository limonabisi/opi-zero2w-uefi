/*
 * x64 Engine - UEFI host (Orange Pi Zero 2W firmware).
 *
 * The firmware keeps running underneath the x86 machine: GOP memory is the
 * guest framebuffer, the USB keyboard feeds the PS/2 controller, BlockIo /
 * SimpleFileSystem back the virtio disks, a 1 ms timer event kicks the CPU
 * loop. F12 leaves the machine and returns to the firmware.
 */
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/CpuLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#include "x64e.h"

int64_t efi_get_timer_ns (void);

/* ------------------------------------------------------------ printf */
static int
is_flag (char c)
{
  return c == '-' || c == '+' || c == ' ' || c == '#' || c == '.' || c == '*' ||
         c == 'l' || c == 'L' || (c >= '0' && c <= '9');
}

static void
fix_format (const char *in, char *out, size_t outsz)
{
  size_t o = 0;

  while (*in && o + 1 < outsz) {
    if (*in != '%') {
      out[o++] = *in++;
      continue;
    }

    out[o++] = *in++;
    while (*in && o + 1 < outsz && is_flag (*in)) {
      out[o++] = *in++;
    }

    if (*in && o + 1 < outsz) {
      out[o++] = (*in == 's') ? 'a' : *in;
      in++;
    }
  }

  out[o] = 0;
}

int
x64e_snprintf (char *buf, size_t size, const char *fmt, ...)
{
  char    f[256];
  VA_LIST ap;
  UINTN   n;

  fix_format (fmt, f, sizeof (f));
  VA_START (ap, fmt);
  n = AsciiVSPrint (buf, size, f, ap);
  VA_END (ap);
  return (int)n;
}

void
host_log (const char *fmt, ...)
{
  char    f[256], out[512];
  VA_LIST ap;
  UINTN   n;

  fix_format (fmt, f, sizeof (f));
  VA_START (ap, fmt);
  n = AsciiVSPrint (out, sizeof (out), f, ap);
  VA_END (ap);
  SerialPortWrite ((UINT8 *)out, n);
}

/* ------------------------------------------------------------- time */
static uint64_t mT0;

uint64_t
host_now_ns (void)
{
  return (uint64_t)efi_get_timer_ns () - mT0;
}

static EFI_EVENT  mKickEvent;
static uc_engine *mKickUc;

static VOID EFIAPI
KickNotify (EFI_EVENT Event, VOID *Context)
{
  if (mKickUc != NULL) {
    uc_emu_stop (mKickUc);
  }
}

void
host_start_kick_timer (uc_engine *uc, uint32_t period_us)
{
  mKickUc = uc;
  if (mKickEvent == NULL) {
    gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, KickNotify, NULL, &mKickEvent);
  }

  gBS->SetTimer (mKickEvent, TimerPeriodic, (UINT64)period_us * 10);
}

static void
stop_kick_timer (void)
{
  if (mKickEvent != NULL) {
    gBS->SetTimer (mKickEvent, TimerCancel, 0);
    gBS->CloseEvent (mKickEvent);
    mKickEvent = NULL;
  }

  mKickUc = NULL;
}

/* --------------------------------------------------------- console */
void
host_console_write (const char *buf, size_t len)
{
  SerialPortWrite ((UINT8 *)buf, len);     /* guest COM1 -> board UART */
}

int
host_console_read (void)
{
  UINT8 c;

  if (SerialPortPoll () && SerialPortRead (&c, 1) == 1) {
    return c;
  }

  return -1;
}

static int
key_available (void)
{
  return gBS->CheckEvent (gST->ConIn->WaitForKey) == EFI_SUCCESS;
}

void
host_idle_until (uint64_t deadline)
{
  while (host_now_ns () < deadline && !key_available () && !SerialPortPoll ()) {
    CpuSleep ();                           /* WFI: the 1 ms tick wakes us */
  }
}

void
host_poll_input (machine_t *m)
{
  EFI_INPUT_KEY k;

  while (gST->ConIn->ReadKeyStroke (gST->ConIn, &k) == EFI_SUCCESS) {
    switch (k.ScanCode) {
      case SCAN_NULL:
        kbd_type_char (m, (int)k.UnicodeChar);
        break;
      case SCAN_UP:        kbd_type_key (m, KEY_UP); break;
      case SCAN_DOWN:      kbd_type_key (m, KEY_DOWN); break;
      case SCAN_RIGHT:     kbd_type_key (m, KEY_RIGHT); break;
      case SCAN_LEFT:      kbd_type_key (m, KEY_LEFT); break;
      case SCAN_HOME:      kbd_type_key (m, KEY_HOME); break;
      case SCAN_END:       kbd_type_key (m, KEY_END); break;
      case SCAN_INSERT:    kbd_type_key (m, KEY_INSERT); break;
      case SCAN_DELETE:    kbd_type_key (m, KEY_DELETE); break;
      case SCAN_PAGE_UP:   kbd_type_key (m, KEY_PGUP); break;
      case SCAN_PAGE_DOWN: kbd_type_key (m, KEY_PGDN); break;
      case SCAN_ESC:       kbd_type_char (m, 27); break;
      case SCAN_F12:       m->quit = 1; break;
      default:
        if (k.ScanCode >= SCAN_F1 && k.ScanCode <= SCAN_F10) {
          kbd_type_key (m, KEY_F1 + (k.ScanCode - SCAN_F1));
        } else if (k.ScanCode == SCAN_F11) {
          kbd_type_key (m, KEY_F1 + 10);
        }

        break;
    }
  }
}

/* ------------------------------------------------------------ status */
void
host_progress (const char *what, unsigned percent)
{
  static unsigned last = 1000;

  if (percent == last) {
    return;
  }

  last = percent;
  Print (L"\r  %a... %3u%%   ", what, percent);
  if (percent == 100) {
    Print (L"\n");
    last = 1000;
  }
}

/* free conventional memory in MB */
static UINT64
free_memory_mb (void)
{
  EFI_MEMORY_DESCRIPTOR *map = NULL, *d;
  UINTN                 size = 0, key, dsize;
  UINT32                ver;
  UINT64                pages = 0;
  EFI_STATUS            st;

  st = gBS->GetMemoryMap (&size, NULL, &key, &dsize, &ver);
  if (st != EFI_BUFFER_TOO_SMALL) {
    return 0;
  }

  size += 8 * dsize;
  map   = AllocatePool (size);
  if (map == NULL || EFI_ERROR (gBS->GetMemoryMap (&size, map, &key, &dsize, &ver))) {
    return 0;
  }

  for (d = map; (UINT8 *)d < (UINT8 *)map + size; d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)d + dsize)) {
    if (d->Type == EfiConventionalMemory) {
      pages += d->NumberOfPages;
    }
  }

  FreePool (map);
  return pages * EFI_PAGE_SIZE / (1024 * 1024);
}

/* ---------------------------------------------------------- memory */
void *
host_alloc (size_t size)
{
  void *p = AllocatePages (EFI_SIZE_TO_PAGES (size));

  if (p != NULL) {
    ZeroMem (p, size);
  }

  return p;
}

void
host_free (void *p, size_t size)
{
  if (p != NULL) {
    FreePages (p, EFI_SIZE_TO_PAGES (size));
  }
}

/* ------------------------------------------------------------ disks */
typedef struct {
  EFI_BLOCK_IO_PROTOCOL *Bio;
  EFI_FILE_PROTOCOL     *File;
  UINT32                BlockSize;
  UINT64                Size;
  UINT8                 *Bounce;           /* 64 KB */
  CHAR16                Name[80];
} HDISK;

#define BOUNCE_SIZE  0x10000

static int
bio_rw (HDISK *d, int write, uint64_t off, uint8_t *buf, uint32_t len)
{
  EFI_BLOCK_IO_PROTOCOL *b  = d->Bio;
  UINT32                 bs = d->BlockSize;

  while (len > 0) {
    UINT64     lba   = off / bs;
    UINT32     skip  = (UINT32)(off % bs);
    UINT32     chunk = BOUNCE_SIZE - skip;
    UINT32     span;
    EFI_STATUS s;

    if (chunk > len) {
      chunk = len;
    }

    span = (skip + chunk + bs - 1) / bs * bs;
    if (write && (skip != 0 || (chunk % bs) != 0)) {
      s = b->ReadBlocks (b, b->Media->MediaId, lba, span, d->Bounce);
      if (EFI_ERROR (s)) {
        return -1;
      }
    }

    if (write) {
      CopyMem (d->Bounce + skip, buf, chunk);
      s = b->WriteBlocks (b, b->Media->MediaId, lba, span, d->Bounce);
    } else {
      s = b->ReadBlocks (b, b->Media->MediaId, lba, span, d->Bounce);
      CopyMem (buf, d->Bounce + skip, chunk);
    }

    if (EFI_ERROR (s)) {
      return -1;
    }

    off += chunk;
    buf += chunk;
    len -= chunk;
  }

  return 0;
}

int
host_disk_read (void *disk, uint64_t off, void *buf, uint32_t len)
{
  HDISK *d = disk;
  UINTN  n = len;

  if (d->File != NULL) {
    if (EFI_ERROR (d->File->SetPosition (d->File, off)) ||
        EFI_ERROR (d->File->Read (d->File, &n, buf)) || n != len) {
      return -1;
    }

    return 0;
  }

  return bio_rw (d, 0, off, buf, len);
}

int
host_disk_write (void *disk, uint64_t off, const void *buf, uint32_t len)
{
  HDISK *d = disk;
  UINTN  n = len;

  if (d->File != NULL) {
    if (EFI_ERROR (d->File->SetPosition (d->File, off)) ||
        EFI_ERROR (d->File->Write (d->File, &n, (void *)buf)) || n != len) {
      return -1;
    }

    return 0;
  }

  return bio_rw (d, 1, off, (uint8_t *)buf, len);
}

/* ------------------------------------------------------ ISO sources */
#define MAX_SOURCES  12

static HDISK mSrc[MAX_SOURCES];
static UINTN mSrcCount;

static int
is_iso (HDISK *d)
{
  uint8_t pvd[8];

  return host_disk_read (d, 16 * 2048, pvd, sizeof (pvd)) == 0 && pvd[0] == 1 &&
         CompareMem (pvd + 1, "CD001", 5) == 0;
}

static void
scan_block_devices (void)
{
  EFI_HANDLE            *h;
  UINTN                 n, i;
  EFI_BLOCK_IO_PROTOCOL *b;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &n, &h))) {
    return;
  }

  for (i = 0; i < n && mSrcCount < MAX_SOURCES; i++) {
    HDISK *d = &mSrc[mSrcCount];

    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiBlockIoProtocolGuid, (VOID **)&b)) ||
        b->Media->LogicalPartition || !b->Media->MediaPresent) {
      continue;
    }

    ZeroMem (d, sizeof (*d));
    d->Bio       = b;
    d->BlockSize = b->Media->BlockSize;
    d->Size      = MultU64x32 (b->Media->LastBlock + 1, b->Media->BlockSize);
    d->Bounce    = AllocatePages (EFI_SIZE_TO_PAGES (BOUNCE_SIZE));
    if (d->Bounce != NULL && is_iso (d)) {
      UnicodeSPrint (d->Name, sizeof (d->Name), L"Disk %u  -  ISO written to the drive (%lu MB)",
                     (UINT32)mSrcCount + 1, DivU64x32 (d->Size, 1024 * 1024));
      mSrcCount++;
    } else if (d->Bounce != NULL) {
      FreePages (d->Bounce, EFI_SIZE_TO_PAGES (BOUNCE_SIZE));
    }
  }

  FreePool (h);
}

static BOOLEAN
ends_with_iso (CONST CHAR16 *e)
{
  return e[0] == L'.' && (e[1] | 0x20) == L'i' && (e[2] | 0x20) == L's' && (e[3] | 0x20) == L'o';
}

static void
scan_iso_files (void)
{
  EFI_HANDLE                      *h;
  UINTN                           n, i;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL               *root, *f;
  UINT8                           buf[SIZE_OF_EFI_FILE_INFO + 512];

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &n, &h))) {
    return;
  }

  for (i = 0; i < n && mSrcCount < MAX_SOURCES; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs)) ||
        EFI_ERROR (fs->OpenVolume (fs, &root))) {
      continue;
    }

    for (;;) {
      EFI_FILE_INFO *fi  = (EFI_FILE_INFO *)buf;
      UINTN          len = sizeof (buf);
      UINTN          nl;

      if (EFI_ERROR (root->Read (root, &len, buf)) || len == 0) {
        break;
      }

      nl = StrLen (fi->FileName);
      if ((fi->Attribute & EFI_FILE_DIRECTORY) || nl < 5 ||
          !ends_with_iso (fi->FileName + nl - 4)) {
        continue;
      }

      if (!EFI_ERROR (root->Open (root, &f, fi->FileName, EFI_FILE_MODE_READ, 0))) {
        HDISK *d = &mSrc[mSrcCount];

        ZeroMem (d, sizeof (*d));
        d->File = f;
        d->Size = fi->FileSize;
        UnicodeSPrint (d->Name, sizeof (d->Name), L"%s (%lu MB)", fi->FileName,
                       DivU64x32 (fi->FileSize, 1024 * 1024));
        if (is_iso (d) && mSrcCount < MAX_SOURCES) {
          mSrcCount++;
        } else {
          f->Close (f);
        }
      }

      if (mSrcCount >= MAX_SOURCES) {
        break;
      }
    }
  }

  FreePool (h);
}

/* ------------------------------------------------------------- menu */
static INTN
choose (CONST CHAR16 *Title, CHAR16 **Items, UINTN Count)
{
  UINTN         sel = 0, i, idx;
  EFI_INPUT_KEY k;

  for (;;) {
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"\n  x64 Engine - x86-64 PC for the Orange Pi Zero 2W\n\n  %s\n\n", Title);
    for (i = 0; i < Count; i++) {
      gST->ConOut->SetAttribute (gST->ConOut, i == sel ? EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
                                                        : EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
      Print (L"   %s   \n", Items[i]);
    }

    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    Print (L"\n  Up/Down: select   Enter: start   Esc: back\n"
           L"  In the x86 machine, F12 returns to the firmware.\n");

    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &idx);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &k))) {
      continue;
    }

    if (k.ScanCode == SCAN_UP && sel > 0) {
      sel--;
    } else if (k.ScanCode == SCAN_DOWN && sel + 1 < Count) {
      sel++;
    } else if (k.ScanCode == SCAN_ESC) {
      return -1;
    } else if (k.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return (INTN)sel;
    }
  }
}

static UINT64
epoch_seconds (void)
{
  EFI_TIME t;
  INT64    y, era, yoe, doy, doe, mo;

  if (EFI_ERROR (gRT->GetTime (&t, NULL))) {
    return 0;
  }

  /* days from civil (Howard Hinnant) */
  mo  = t.Month;
  y   = (INT64)t.Year - (mo <= 2);
  era = y / 400;
  yoe = y - era * 400;
  doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + t.Day - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (UINT64)(era * 146097 + doe - 719468) * 86400 + t.Hour * 3600 + t.Minute * 60 + t.Second;
}

EFI_STATUS
EFIAPI
X64EngineMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  static machine_t             m;
  static bootlist_t            bl;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
  CHAR16                       *items[MAX_SOURCES > MAX_BOOT_ENTRIES ? MAX_SOURCES : MAX_BOOT_ENTRIES];
  CHAR16                       titles[MAX_BOOT_ENTRIES][96];
  iso_t                        iso;
  INTN                         src, ent;
  UINTN                        i;
  UINT64                       free_mb, guest_mb;

  mT0 = (uint64_t)efi_get_timer_ns ();
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  scan_block_devices ();
  scan_iso_files ();
  if (mSrcCount == 0) {
    Print (L"\n  x64 Engine: no x86-64 ISO found.\n\n"
           L"  Write an ISO to a USB drive (balenaEtcher) or copy the .iso file\n"
           L"  to the root of a FAT drive, then try again.\n\n  Press any key.\n");
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
    return EFI_NOT_FOUND;
  }

  for (;;) {
    for (i = 0; i < mSrcCount; i++) {
      items[i] = mSrc[i].Name;
    }

    src = choose (L"Choose the x86-64 ISO:", items, mSrcCount);
    if (src < 0) {
      return EFI_ABORTED;
    }

    if (iso_open (&iso, &mSrc[src]) || bootcfg_scan (&iso, &bl) == 0) {
      Print (L"\n  No boot entries found on this ISO. Press any key.\n");
      gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
      continue;
    }

    for (i = 0; i < (UINTN)bl.count; i++) {
      UnicodeSPrint (titles[i], sizeof (titles[i]), L"%a", bl.e[i].title);
      items[i] = titles[i];
    }

    ent = choose (L"Choose what to start:", items, bl.count);
    if (ent >= 0) {
      break;
    }
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"\n  Starting the x86-64 machine...\n");

  /*
   * Guest RAM: what is free minus the translation cache (100 MB), the
   * kernel image being loaded and a reserve for the firmware's own drivers
   * (USB DMA buffers etc.), at most 512 MB.
   */
  free_mb  = free_memory_mb ();
  guest_mb = free_mb > 100 + 32 + 128 + 128 ? free_mb - 100 - 32 - 128 : 128;
  guest_mb = guest_mb > 512 ? 512 : guest_mb & ~31ULL;
  host_log ("x64e: %lu MB free, %lu MB for the x86 machine\n", free_mb, guest_mb);
  Print (L"  Memory: %lu MB free, %lu MB for the x86 machine\n", free_mb, guest_mb);
  if (machine_init (&m, guest_mb) != 0) {
    Print (L"  Not enough memory for the x86 machine. Press any key.\n");
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
    return EFI_OUT_OF_RESOURCES;
  }

  host_log ("x64e: %u MB guest RAM\n", (unsigned)(m.ram_size >> 20));
  m.boot_ns = epoch_seconds () * 1000000000ULL;
  machine_add_disk (&m, &mSrc[src], mSrc[src].Size, 1);

  if (!EFI_ERROR (gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&gop)) ||
      !EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop))) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;

    if (mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
      machine_set_fb (&m, (void *)(UINTN)gop->Mode->FrameBufferBase, mi->HorizontalResolution,
                      mi->VerticalResolution, mi->PixelsPerScanLine * 4);
    }
  }

  if (iso_boot (&m, &iso, &bl.e[ent], "console=ttyS0,115200 console=tty0 loglevel=6 nolapic noapic")) {
    Print (L"  Could not load the kernel from the ISO. Press any key.\n");
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
    return EFI_LOAD_ERROR;
  }

  if (m.fb != NULL) {
    SetMem (m.fb, (UINTN)m.fb_stride * m.fb_h, 0);
  }

  machine_run (&m);
  stop_kick_timer ();

  /* back to the firmware: restore its console */
  gST->ConOut->Reset (gST->ConOut, FALSE);
  gST->ConOut->ClearScreen (gST->ConOut);
  return EFI_SUCCESS;
}

/* ------------------------------------------- libc bits the glue lacks */
char *
strchr (const char *s, int c)
{
  for (;; s++) {
    if (*s == (char)c) {
      return (char *)s;
    }

    if (*s == 0) {
      return NULL;
    }
  }
}

char *
strncpy (char *dest, const char *src, size_t n)
{
  size_t i;

  for (i = 0; i < n && src[i]; i++) {
    dest[i] = src[i];
  }

  for (; i < n; i++) {
    dest[i] = 0;
  }

  return dest;
}

int
strncmp (const char *a, const char *b, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++) {
    if (a[i] != b[i] || a[i] == 0) {
      return (unsigned char)a[i] - (unsigned char)b[i];
    }
  }

  return 0;
}
