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
#include <Protocol/UsbIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/Cpu.h>
#include <Guid/FileInfo.h>

#include "x64e.h"
#include "x64e_el.h"

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

static EFI_SIMPLE_NETWORK_PROTOCOL *mSnp;
static uint64_t                     mNextPoll;

void
host_idle_until (uint64_t deadline)
{
  if (mSnp != NULL && deadline > mNextPoll) {
    deadline = mNextPoll;                  /* wake up for the next network poll */
  }

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
  UINT64  Logical;                         /* offset in the file */
  UINT64  Dev;                             /* offset on the device region */
  UINT64  Len;
} EXTENT;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL *Bio;
  EFI_FILE_PROTOCOL     *File;
  EXTENT                *Ext;              /* file on a raw-read file system (exFAT) */
  UINT32                NExt;
  UINT32                BlockSize;
  UINT64                Base;              /* byte offset of the region on the device */
  UINT64                Size;
  UINT8                 *Bounce;           /* 64 KB */
  CHAR16                Name[96];
} HDISK;

#define BOUNCE_SIZE  0x10000

static int
bio_rw (HDISK *d, int write, uint64_t off, uint8_t *buf, uint32_t len)
{
  EFI_BLOCK_IO_PROTOCOL *b  = d->Bio;
  UINT32                 bs = d->BlockSize;

  if (off + len > d->Size || off + len < off) {
    return -1;                             /* never touch anything outside the region */
  }

  off += d->Base;
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

/* read a file that is a list of extents on its device region */
static int
ext_read (HDISK *d, uint64_t off, uint8_t *buf, uint32_t len)
{
  UINT32 lo = 0, hi = d->NExt;

  if (off + len > d->Size || off + len < off) {
    return -1;
  }

  while (len > 0) {
    EXTENT *e;
    UINT64  n;

    /* binary search: last extent with Logical <= off */
    lo = 0;
    hi = d->NExt;
    while (hi - lo > 1) {
      UINT32 mid = (lo + hi) / 2;

      if (d->Ext[mid].Logical <= off) {
        lo = mid;
      } else {
        hi = mid;
      }
    }

    e = &d->Ext[lo];
    if (off < e->Logical || off >= e->Logical + e->Len) {
      return -1;
    }

    n = e->Logical + e->Len - off;
    if (n > len) {
      n = len;
    }

    {
      UINT64 size = d->Size;
      int    r;

      d->Size = (UINT64)-1;                /* region bound checked above */
      r       = bio_rw (d, 0, e->Dev + (off - e->Logical), buf, (uint32_t)n);
      d->Size = size;
      if (r) {
        return -1;
      }
    }

    off += n;
    buf += n;
    len -= (uint32_t)n;
  }

  return 0;
}

int
host_disk_read (void *disk, uint64_t off, void *buf, uint32_t len)
{
  HDISK *d = disk;
  UINTN  n = len;

  if (d->Ext != NULL) {
    return ext_read (d, off, buf, len);
  }

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

  if (d->Ext != NULL) {
    return -1;                             /* ISOs on exFAT are read-only */
  }

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
#define MAX_SOURCES  24

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
scan_fat_dir (EFI_FILE_PROTOCOL *dir, UINTN depth)
{
  EFI_FILE_PROTOCOL *f;
  UINTN             bufsz = SIZE_OF_EFI_FILE_INFO + 512;
  UINT8             *buf  = AllocatePool (bufsz);

  if (buf == NULL) {
    return;
  }

  while (mSrcCount < MAX_SOURCES) {
    EFI_FILE_INFO *fi  = (EFI_FILE_INFO *)buf;
    UINTN          len = bufsz;
    UINTN          nl;

    if (EFI_ERROR (dir->Read (dir, &len, buf)) || len == 0) {
      break;
    }

    nl = StrLen (fi->FileName);
    if (fi->Attribute & EFI_FILE_DIRECTORY) {
      if (depth < 3 && fi->FileName[0] != L'.' && StrCmp (fi->FileName, L"System Volume Information") != 0 &&
          !EFI_ERROR (dir->Open (dir, &f, fi->FileName, EFI_FILE_MODE_READ, 0))) {
        scan_fat_dir (f, depth + 1);
        f->Close (f);
      }

      continue;
    }

    if (nl < 5 || !ends_with_iso (fi->FileName + nl - 4)) {
      continue;
    }

    if (!EFI_ERROR (dir->Open (dir, &f, fi->FileName, EFI_FILE_MODE_READ, 0))) {
      HDISK *d = &mSrc[mSrcCount];

      ZeroMem (d, sizeof (*d));
      d->File = f;
      d->Size = fi->FileSize;
      UnicodeSPrint (d->Name, sizeof (d->Name), L"%s (%lu MB)", fi->FileName,
                     DivU64x32 (fi->FileSize, 1024 * 1024));
      if (is_iso (d)) {
        mSrcCount++;
      } else {
        f->Close (f);
      }
    }
  }

  FreePool (buf);
}

static void
scan_iso_files (void)
{
  EFI_HANDLE                      *h;
  UINTN                           n, i;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL               *root;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &n, &h))) {
    return;
  }

  for (i = 0; i < n && mSrcCount < MAX_SOURCES; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs)) ||
        EFI_ERROR (fs->OpenVolume (fs, &root))) {
      continue;
    }

    scan_fat_dir (root, 0);
  }

  FreePool (h);
}

/*
 * ISO files on exFAT (Ventoy's default data partition, most big USB drives):
 * the firmware has no exFAT driver, so read the directory tree ourselves
 * and hand the ISO to the machine as a list of extents on the partition.
 */
typedef struct {
  HDISK   Part;                            /* the partition, Base 0 */
  UINT32  ClusShift;                       /* log2 bytes per cluster */
  UINT64  FatOff;
  UINT64  HeapOff;
  UINT32  ClusterCount;
  UINT8   FatPage[4096];
  UINT64  FatPageOff;
} EXFAT;

static UINT32
exfat_next (EXFAT *fs, UINT32 c)
{
  UINT64 off = fs->FatOff + (UINT64)c * 4;
  UINT64 pg  = off & ~4095ULL;

  if (pg != fs->FatPageOff) {
    if (host_disk_read (&fs->Part, pg, fs->FatPage, sizeof (fs->FatPage))) {
      return 0xFFFFFFFF;
    }

    fs->FatPageOff = pg;
  }

  return *(UINT32 *)(fs->FatPage + (off - pg));
}

/* extents of a cluster chain; Len = 0 means "until the end of the chain" */
static EXTENT *
exfat_extents (EXFAT *fs, UINT32 first, UINT64 len, BOOLEAN nofat, UINT32 *count, UINT64 *total)
{
  UINT64  csize = 1ULL << fs->ClusShift;
  UINT32  max, n = 0, c = first, walked = 0;
  EXTENT  *e;

  *count = 0;
  *total = 0;
  if (first < 2 || first - 2 >= fs->ClusterCount) {
    return NULL;
  }

  if (nofat) {
    e = AllocateZeroPool (sizeof (EXTENT));
    if (e != NULL) {
      e->Dev = fs->HeapOff + ((UINT64)(first - 2) << fs->ClusShift);
      e->Len = len;
      *count = 1;
      *total = len;
    }

    return e;
  }

  max = 64;
  e   = AllocateZeroPool (max * sizeof (EXTENT));
  while (e != NULL && c >= 2 && c - 2 < fs->ClusterCount && walked++ <= fs->ClusterCount) {
    UINT64 dev = fs->HeapOff + ((UINT64)(c - 2) << fs->ClusShift);

    if (len != 0 && *total >= len) {
      break;
    }

    if (n > 0 && e[n - 1].Dev + e[n - 1].Len == dev) {
      e[n - 1].Len += csize;
    } else {
      if (n == max) {
        EXTENT *bigger = ReallocatePool (max * sizeof (EXTENT), 2 * max * sizeof (EXTENT), e);

        if (bigger == NULL) {
          FreePool (e);
          return NULL;
        }

        e    = bigger;
        max *= 2;
      }

      e[n].Logical = *total;
      e[n].Dev     = dev;
      e[n].Len     = csize;
      n++;
    }

    *total += csize;
    c       = exfat_next (fs, c);
  }

  if (e != NULL && len != 0) {
    if (*total < len) {                    /* chain shorter than the file */
      FreePool (e);
      return NULL;
    }

    e[n - 1].Len -= *total - len;          /* last cluster is partly used */
    *total        = len;
  }

  *count = n;
  return e;
}

static void
exfat_add_iso (EXFAT *fs, CONST CHAR16 *name, UINT32 first, UINT64 len, BOOLEAN nofat)
{
  HDISK  *d = &mSrc[mSrcCount];
  UINT32  n;
  UINT64  total;

  ZeroMem (d, sizeof (*d));
  d->Ext = exfat_extents (fs, first, len, nofat, &n, &total);
  if (d->Ext == NULL) {
    return;
  }

  d->NExt      = n;
  d->Bio       = fs->Part.Bio;
  d->BlockSize = fs->Part.BlockSize;
  d->Base      = fs->Part.Base;
  d->Size      = len;
  d->Bounce    = AllocatePages (EFI_SIZE_TO_PAGES (BOUNCE_SIZE));
  UnicodeSPrint (d->Name, sizeof (d->Name), L"%s (%lu MB, USB)", name, DivU64x32 (len, 1024 * 1024));
  if (d->Bounce != NULL && is_iso (d)) {
    mSrcCount++;
    return;
  }

  if (d->Bounce != NULL) {
    FreePages (d->Bounce, EFI_SIZE_TO_PAGES (BOUNCE_SIZE));
  }

  FreePool (d->Ext);
  ZeroMem (d, sizeof (*d));
}

static void
exfat_scan_dir (EXFAT *fs, UINT32 first, UINT64 len, BOOLEAN nofat, UINTN depth)
{
  EXTENT  *e;
  UINT32  n, i;
  UINT64  total, off;
  UINT8   *dir;
  HDISK   view;

  e = exfat_extents (fs, first, len, nofat, &n, &total);
  if (e == NULL || total == 0 || total > SIZE_4MB) {
    if (e != NULL) {
      FreePool (e);
    }

    return;
  }

  dir = AllocatePool ((UINTN)total);
  if (dir == NULL) {
    FreePool (e);
    return;
  }

  view      = fs->Part;
  view.Ext  = e;
  view.NExt = n;
  view.Size = total;
  if (host_disk_read (&view, 0, dir, (uint32_t)total)) {
    FreePool (dir);
    FreePool (e);
    return;
  }

  for (off = 0; off + 32 <= total && mSrcCount < MAX_SOURCES; off += 32) {
    UINT8   *ent = dir + off;
    UINT8   sec, attr, flags, nlen;
    UINT32  clus;
    UINT64  size;
    CHAR16  name[256];
    UINTN   k = 0;

    if (ent[0] == 0x00) {
      break;                               /* end of directory */
    }

    if (ent[0] != 0x85) {
      continue;                            /* not an in-use file entry */
    }

    sec  = ent[1];
    attr = ent[4];
    if (sec < 2 || off + 32 * (UINT64)(sec + 1) > total || dir[off + 32] != 0xC0) {
      continue;
    }

    flags = dir[off + 32 + 1];
    nlen  = dir[off + 32 + 3];
    clus  = *(UINT32 *)(dir + off + 32 + 20);
    size  = *(UINT64 *)(dir + off + 32 + 24);
    for (i = 2; i <= sec && k < nlen; i++) {
      UINT8  *ne = dir + off + 32 * (UINT64)i;
      UINTN  j;

      if (ne[0] != 0xC1) {
        break;
      }

      for (j = 0; j < 15 && k < nlen; j++) {
        name[k++] = *(UINT16 *)(ne + 2 + 2 * j);
      }
    }

    name[k] = 0;
    off    += 32 * (UINT64)sec;            /* skip the secondary entries */
    if (k == 0 || name[0] == L'.' || name[0] == L'$') {
      continue;
    }

    if (attr & 0x10) {
      if (depth < 3 && StrCmp (name, L"System Volume Information") != 0) {
        exfat_scan_dir (fs, clus, size, (flags & 2) != 0, depth + 1);
      }
    } else if (k >= 5 && ends_with_iso (name + k - 4) && size >= 64 * 1024) {
      exfat_add_iso (fs, name, clus, size, (flags & 2) != 0);
    }
  }

  FreePool (dir);
  FreePool (e);
}

static void
scan_exfat (void)
{
  EFI_HANDLE            *h;
  UINTN                 n, i;
  EFI_BLOCK_IO_PROTOCOL *b;
  EXFAT                 *fs;
  UINT8                 *bs;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &n, &h))) {
    return;
  }

  fs = AllocateZeroPool (sizeof (*fs));
  bs = AllocatePool (512);
  for (i = 0; fs != NULL && bs != NULL && i < n && mSrcCount < MAX_SOURCES; i++) {
    UINT8 sshift, cshift;

    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiBlockIoProtocolGuid, (VOID **)&b)) ||
        !b->Media->MediaPresent) {
      continue;
    }

    ZeroMem (fs, sizeof (*fs));
    fs->Part.Bio       = b;
    fs->Part.BlockSize = b->Media->BlockSize;
    fs->Part.Size      = MultU64x32 (b->Media->LastBlock + 1, b->Media->BlockSize);
    fs->Part.Bounce    = AllocatePages (EFI_SIZE_TO_PAGES (BOUNCE_SIZE));
    fs->FatPageOff     = (UINT64)-1;
    if (fs->Part.Bounce == NULL) {
      continue;
    }

    if (host_disk_read (&fs->Part, 0, bs, 512) == 0 && CompareMem (bs + 3, "EXFAT   ", 8) == 0) {
      sshift = bs[0x6C];
      cshift = bs[0x6D];
      if (sshift >= 9 && sshift <= 12 && sshift + cshift <= 25) {
        fs->ClusShift    = sshift + cshift;
        fs->FatOff       = (UINT64)*(UINT32 *)(bs + 0x50) << sshift;
        fs->HeapOff      = (UINT64)*(UINT32 *)(bs + 0x58) << sshift;
        fs->ClusterCount = *(UINT32 *)(bs + 0x5C);
        host_log ("x64e: exFAT volume found, looking for ISOs\n");
        exfat_scan_dir (fs, *(UINT32 *)(bs + 0x60), 0, FALSE, 0);
      }
    }

    FreePages (fs->Part.Bounce, EFI_SIZE_TO_PAGES (BOUNCE_SIZE));
  }

  if (fs != NULL) {
    FreePool (fs);
  }

  if (bs != NULL) {
    FreePool (bs);
  }

  FreePool (h);
}

/* ---------------------------------------------------------- network */
/*
 * The x86 machine's network card talks straight to a firmware SNP: phone
 * USB tethering (RNDIS / CDC-NCM) or a USB Ethernet adapter (CDC-ECM).
 * The guest uses the adapter's own MAC address, so the phone's DHCP server
 * simply sees "the computer".
 */
#define TX_SLOTS  32
#define TX_SIZE   1600

static UINT8   *mTxBuf[TX_SLOTS];
static BOOLEAN mTxBusy[TX_SLOTS];
static uint64_t mBurstUntil;
static uint64_t mEmptyCost = 1000000ULL;      /* what a poll that finds nothing costs, ns */
static uint64_t mTxFrames;
static CHAR16  mNetName[400] = L"none - plug in a phone with USB tethering on, or a USB Ethernet adapter";

static void
net_recycle (void)
{
  VOID *buf;
  UINTN i;

  for (;;) {
    buf = NULL;
    if (EFI_ERROR (mSnp->GetStatus (mSnp, NULL, &buf)) || buf == NULL) {
      return;
    }

    for (i = 0; i < TX_SLOTS; i++) {
      if (mTxBuf[i] == buf) {
        mTxBusy[i] = FALSE;
      }
    }
  }
}

/*
 * No network adapter: list what is on the USB bus (vendor:product and the
 * interface class), so a phone that the network drivers do not take is
 * easy to tell from a phone that is not there at all.
 */
static void
usb_net_diag (void)
{
  EFI_HANDLE                    *h;
  UINTN                         n, i, len;
  EFI_USB_IO_PROTOCOL           *io;
  EFI_USB_DEVICE_DESCRIPTOR     dd;
  EFI_USB_INTERFACE_DESCRIPTOR  id;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &n, &h))) {
    UnicodeSPrint (mNetName, sizeof (mNetName), L"none - no USB devices seen");
    return;
  }

  UnicodeSPrint (mNetName, sizeof (mNetName), L"none - USB:");
  for (i = 0; i < n; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiUsbIoProtocolGuid, (VOID **)&io)) ||
        EFI_ERROR (io->UsbGetDeviceDescriptor (io, &dd)) ||
        EFI_ERROR (io->UsbGetInterfaceDescriptor (io, &id))) {
      continue;
    }

    len = StrLen (mNetName);
    if (len + 24 >= sizeof (mNetName) / sizeof (CHAR16)) {
      break;
    }

    UnicodeSPrint (mNetName + len, sizeof (mNetName) - len * sizeof (CHAR16), L" %04x:%04x/%02x%02x%02x",
                   dd.IdVendor, dd.IdProduct, id.InterfaceClass, id.InterfaceSubClass, id.InterfaceProtocol);
  }

  host_log ("x64e: no network adapter; %s\n", "USB interfaces listed in the menu");
  FreePool (h);
}

static int
net_open (uint8_t mac[6])
{
  EFI_HANDLE                  *h;
  UINTN                       n, i;
  EFI_SIMPLE_NETWORK_PROTOCOL *snp;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL, &n, &h))) {
    usb_net_diag ();
    return -1;
  }

  for (i = 0; i < n && mSnp == NULL; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiSimpleNetworkProtocolGuid, (VOID **)&snp))) {
      continue;
    }

    if (snp->Mode->State == EfiSimpleNetworkStopped) {
      snp->Start (snp);
    }

    if (snp->Mode->State == EfiSimpleNetworkStarted) {
      snp->Initialize (snp, 0, 0);
    }

    if (snp->Mode->State != EfiSimpleNetworkInitialized || snp->Mode->HwAddressSize != 6) {
      continue;
    }

    /*
     * Enable unicast + broadcast + multicast. A multicast list is passed on
     * purpose: the USB network drivers only program the device's packet
     * filter when one is given, and a phone passes nothing up until then.
     */
    {
      EFI_MAC_ADDRESS mc[2];
      UINT32          en = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;

      ZeroMem (mc, sizeof (mc));
      mc[0].Addr[0] = 0x33; mc[0].Addr[1] = 0x33; mc[0].Addr[5] = 0x01;   /* IPv6 all nodes */
      mc[1].Addr[0] = 0x01; mc[1].Addr[2] = 0x5e; mc[1].Addr[5] = 0x01;   /* IPv4 all hosts */
      if (snp->Mode->ReceiveFilterMask & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) {
        en |= EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST;
      }

      if (EFI_ERROR (snp->ReceiveFilters (snp, en, 0, FALSE, (en & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) ? 2 : 0,
                                          (en & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) ? mc : NULL))) {
        snp->ReceiveFilters (snp, EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST,
                             0, FALSE, 0, NULL);
      }
    }

    CopyMem (mac, &snp->Mode->CurrentAddress, 6);
    mSnp = snp;
  }

  FreePool (h);
  if (mSnp == NULL) {
    usb_net_diag ();
    return -1;
  }

  for (i = 0; i < TX_SLOTS; i++) {
    mTxBuf[i] = AllocatePool (TX_SIZE);
  }

  UnicodeSPrint (mNetName, sizeof (mNetName), L"USB network adapter %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  host_log ("x64e: network card on the USB adapter %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return 0;
}

/* time to leave between polls while traffic is flowing */
static uint64_t
net_gap (void)
{
  uint64_t gap = mEmptyCost * 3;

  return gap < 2000000ULL ? 2000000ULL : gap > 20000000ULL ? 20000000ULL : gap;
}

int
host_net_send (const void *frame, uint32_t len)
{
  UINTN i;

  if (mSnp == NULL || len > TX_SIZE) {
    return -1;
  }

  net_recycle ();
  for (i = 0; i < TX_SLOTS; i++) {
    if (!mTxBusy[i] && mTxBuf[i] != NULL) {
      CopyMem (mTxBuf[i], frame, len);
      if (EFI_ERROR (mSnp->Transmit (mSnp, 0, len, mTxBuf[i], NULL, NULL, NULL))) {
        return -1;
      }

      mTxBusy[i]   = TRUE;
      mTxFrames++;
      mBurstUntil  = host_now_ns () + 300000000ULL;   /* an answer is likely: look often */
      if (mNextPoll > host_now_ns () + net_gap ()) {
        mNextPoll = host_now_ns () + net_gap ();
      }
      return 0;
    }
  }

  return -1;                               /* all buffers in flight: drop */
}

int
host_net_recv (void *frame, uint32_t max)
{
  UINTN    len = max;
  uint64_t now;

  if (mSnp == NULL) {
    return 0;
  }

  /* each empty poll costs a USB bulk-in timeout: back off when quiet */
  now = host_now_ns ();
  if (now < mNextPoll) {
    return 0;
  }

  {
    static uint64_t spent, calls, frames, last_report;
    EFI_STATUS      st  = mSnp->Receive (mSnp, NULL, &len, frame, NULL, NULL, NULL);
    uint64_t        end = host_now_ns ();

    spent += end - now;
    calls++;
    if (!EFI_ERROR (st) && len > 0) {
      frames++;
    }

    if (end - last_report > 30000000000ULL) {
      host_log ("x64e: net: %lu frames out, %lu in, %lu polls, %lu ms spent polling\n", mTxFrames, frames, calls,
                spent / 1000000);
      last_report = end;
    }

    /*
     * An empty poll costs a USB bulk-in timeout (a few ms) during which the
     * x86 CPU stands still. While traffic flows, leave three times that cost
     * between polls (the adapter queues what arrives meanwhile); when the
     * link is quiet, poll every 50 ms.
     */
    if (EFI_ERROR (st) || len == 0) {
      mEmptyCost = (mEmptyCost * 7 + (end - now)) / 8;
      mNextPoll  = end + (end < mBurstUntil ? net_gap () : 50000000ULL);
      return 0;
    }

    mBurstUntil = end + 300000000ULL;
  }

  mNextPoll = 0;
  return (int)len;
}

/* -------------------------------------------------------- hard disk */
/*
 * The x86 machine's hard disk: an x64disk*.img file on a FAT drive, or
 * the free space of the microSD card behind the firmware's own partition
 * (the card image only uses the first 64 MB).
 */
static HDISK   mHd;
static BOOLEAN mHdPresent;
static iso_t   mHdFs;
static BOOLEAN mHdLinux;

static BOOLEAN
path_is_prefix (EFI_DEVICE_PATH_PROTOCOL *a, EFI_DEVICE_PATH_PROTOCOL *b)
{
  UINTN la = GetDevicePathSize (a) - END_DEVICE_PATH_LENGTH;

  return la <= GetDevicePathSize (b) && CompareMem (a, b, la) == 0;
}

static void
find_sd_free_space (EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_DEVICE_PATH_PROTOCOL  *part, *dp;
  EFI_HANDLE                *h;
  UINTN                     n, i, k;
  EFI_BLOCK_IO_PROTOCOL     *b;
  UINT8                     *mbr;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&li)) ||
      EFI_ERROR (gBS->HandleProtocol (li->DeviceHandle, &gEfiDevicePathProtocolGuid, (VOID **)&part)) ||
      EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &n, &h))) {
    return;
  }

  for (i = 0; i < n && !mHdPresent; i++) {
    UINT64 end = 64ULL << 20, size;

    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiBlockIoProtocolGuid, (VOID **)&b)) ||
        b->Media->LogicalPartition || !b->Media->MediaPresent || b->Media->ReadOnly ||
        EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiDevicePathProtocolGuid, (VOID **)&dp)) ||
        !path_is_prefix (dp, part) || b->Media->BlockSize != 512) {
      continue;
    }

    /* the disk the engine was loaded from: free space after the last partition */
    mbr = AllocatePool (512);
    if (mbr == NULL || EFI_ERROR (b->ReadBlocks (b, b->Media->MediaId, 0, 512, mbr)) ||
        mbr[510] != 0x55 || mbr[511] != 0xaa) {
      if (mbr) FreePool (mbr);
      continue;
    }

    for (k = 0; k < 4; k++) {
      UINT8  *p     = mbr + 446 + k * 16;
      UINT64 pend   = ((UINT64)(p[8] | (p[9] << 8) | (p[10] << 16) | ((UINT32)p[11] << 24)) +
                       (p[12] | (p[13] << 8) | (p[14] << 16) | ((UINT32)p[15] << 24))) * 512;

      if (p[4] == 0xee) {
        end = MAX_UINT64;                  /* GPT: leave it alone */
      } else if (p[4] != 0 && pend > end) {
        end = pend;
      }
    }

    FreePool (mbr);
    end  = (end + (1ULL << 20) - 1) & ~((1ULL << 20) - 1);
    size = MultU64x32 (b->Media->LastBlock + 1, 512);
    if (end == MAX_UINT64 || end >= size || size - end < (2ULL << 30)) {
      continue;
    }

    ZeroMem (&mHd, sizeof (mHd));
    mHd.Bio       = b;
    mHd.BlockSize = 512;
    mHd.Base      = end;
    mHd.Size      = size - end;
    mHd.Bounce    = AllocatePages (EFI_SIZE_TO_PAGES (BOUNCE_SIZE));
    if (mHd.Bounce != NULL) {
      UnicodeSPrint (mHd.Name, sizeof (mHd.Name), L"microSD card free space (%lu GB)",
                     DivU64x32 (mHd.Size, 1024 * 1024 * 1024));
      mHdPresent = TRUE;
    }
  }

  FreePool (h);
}

static BOOLEAN
wcase_eq (CONST CHAR16 *a, CONST CHAR16 *b, UINTN n)
{
  for ( ; n > 0; n--, a++, b++) {
    CHAR16 x = (*a >= L'A' && *a <= L'Z') ? *a + 32 : *a;
    CHAR16 y = (*b >= L'A' && *b <= L'Z') ? *b + 32 : *b;

    if (x != y) {
      return FALSE;
    }
  }

  return TRUE;
}

static void
find_disk_file (void)
{
  EFI_HANDLE                      *h;
  UINTN                           n, i;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL               *root, *f;
  UINT8                           buf[SIZE_OF_EFI_FILE_INFO + 512];

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &n, &h))) {
    return;
  }

  for (i = 0; i < n && !mHdPresent; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs)) ||
        EFI_ERROR (fs->OpenVolume (fs, &root))) {
      continue;
    }

    for (;;) {
      EFI_FILE_INFO *fi  = (EFI_FILE_INFO *)buf;
      UINTN          len = sizeof (buf), nl;

      if (EFI_ERROR (root->Read (root, &len, buf)) || len == 0) {
        break;
      }

      nl = StrLen (fi->FileName);
      if ((fi->Attribute & (EFI_FILE_DIRECTORY | EFI_FILE_READ_ONLY)) || nl < 11 ||
          !wcase_eq (fi->FileName, L"x64disk", 7) || !wcase_eq (fi->FileName + nl - 4, L".img", 4) ||
          fi->FileSize < (256ULL << 20)) {
        continue;
      }

      if (!EFI_ERROR (root->Open (root, &f, fi->FileName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
        ZeroMem (&mHd, sizeof (mHd));
        mHd.File = f;
        mHd.Size = fi->FileSize;
        UnicodeSPrint (mHd.Name, sizeof (mHd.Name), L"%s (%lu MB)", fi->FileName,
                       DivU64x32 (fi->FileSize, 1024 * 1024));
        mHdPresent = TRUE;
        break;
      }
    }
  }

  FreePool (h);
}

static void
flush_disks (void)
{
  if (mHdPresent) {
    if (mHd.File != NULL) {
      mHd.File->Flush (mHd.File);
    } else if (mHd.Bio != NULL) {
      mHd.Bio->FlushBlocks (mHd.Bio);
    }
  }
}

/* ------------------------------------------------------------ config */
/*
 * Optional \EFI\X64ENGINE\X64E.CFG next to the engine, "key=value" lines:
 *   shadow=0   no shadow MMU (all guest accesses through the softmmu)
 *   el1=0      stay at EL2 (implies shadow=0)
 *   aslr=1     leave address space randomization on in the x86 system
 *              (off by default: with it every new process runs the same
 *              programs at new addresses, and all their code has to be
 *              translated again)
 */
static int mCfgShadow = 1, mCfgEl1 = 1, mCfgAslr = 0;

static void
read_config (EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL       *li;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL               *root, *f;
  char                            buf[512];
  UINTN                           n = sizeof (buf) - 1, i;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&li)) ||
      EFI_ERROR (gBS->HandleProtocol (li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs)) ||
      EFI_ERROR (fs->OpenVolume (fs, &root))) {
    return;
  }

  if (EFI_ERROR (root->Open (root, &f, L"\\EFI\\X64ENGINE\\X64E.CFG", EFI_FILE_MODE_READ, 0))) {
    return;
  }

  if (!EFI_ERROR (f->Read (f, &n, buf))) {
    buf[n] = 0;
    for (i = 0; i < n; i++) {
      if (strncmp (buf + i, "shadow=", 7) == 0) {
        mCfgShadow = buf[i + 7] == '1';
      } else if (strncmp (buf + i, "el1=", 4) == 0) {
        mCfgEl1 = buf[i + 4] == '1';
      } else if (strncmp (buf + i, "aslr=", 5) == 0) {
        mCfgAslr = buf[i + 5] == '1';
      }
    }
  }

  f->Close (f);
  host_log ("x64e: X64E.CFG: shadow=%d el1=%d aslr=%d\n", mCfgShadow, mCfgEl1, mCfgAslr);
}

/* ------------------------------------------------------- shadow MMU */
/*
 * Data aborts of direct guest accesses (TTBR1, see unicorn's cputlb.c):
 * the shadow MMU maps the page and the access is retried, or the
 * instruction is re-executed through the softmmu (guest page fault, I/O,
 * write to translated code).
 */
#define SHADOW_POOL  (16 * 1024 * 1024)

static uc_engine *mShadowUc;

static void
x64e_slowpath_entry (uint64_t host_pc)
{
  uc_x64e_slowpath (mShadowUc, host_pc);
}

static uint64_t mFaultCount, mFaultNs;

void
host_perf (uint64_t out[2])
{
  out[0] = mFaultCount;
  out[1] = mFaultNs;
}

STATIC VOID EFIAPI
X64eSyncHandler (IN EFI_EXCEPTION_TYPE Type, IN OUT EFI_SYSTEM_CONTEXT Ctx)
{
  EFI_SYSTEM_CONTEXT_AARCH64 *c  = Ctx.SystemContextAArch64;
  uint64_t                   t0 = host_now_ns ();
  int                        r  = mShadowUc != NULL ? uc_x64e_dabt (mShadowUc, c->FAR, c->ESR, c->ELR) : -1;

  mFaultCount++;
  mFaultNs += host_now_ns () - t0;
  if (r == 0) {
    return;
  }

  if (r == 1) {
    c->X0  = c->ELR;
    c->ELR = (UINT64)(UINTN)x64e_slowpath_entry;
    return;
  }

  host_log ("\nx64e: unexpected exception: ESR %lx FAR %lx ELR %lx SP %lx LR %lx\n",
            c->ESR, c->FAR, c->ELR, c->SP, c->LR);
  CpuDeadLoop ();
}

static EFI_CPU_ARCH_PROTOCOL *
shadow_start (uc_engine *uc)
{
  EFI_CPU_ARCH_PROTOCOL *cpu;
  void                  *pool;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&cpu))) {
    return NULL;
  }

  pool = AllocatePages (EFI_SIZE_TO_PAGES (SHADOW_POOL));
  if (pool == NULL) {
    return NULL;
  }

  mShadowUc = uc;
  if (cpu->RegisterInterruptHandler (cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, X64eSyncHandler) ==
      EFI_ALREADY_STARTED) {
    cpu->RegisterInterruptHandler (cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, NULL);
    cpu->RegisterInterruptHandler (cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, X64eSyncHandler);
  }

  if (uc_x64e_shadow_init (uc, pool, SHADOW_POOL) != 0) {
    cpu->RegisterInterruptHandler (cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, NULL);
    FreePages (pool, EFI_SIZE_TO_PAGES (SHADOW_POOL));
    mShadowUc = NULL;
    return NULL;
  }

  host_log ("x64e: shadow MMU on (direct guest memory access)\n");
  return cpu;
}

static void
shadow_stop (EFI_CPU_ARCH_PROTOCOL *cpu)
{
  if (cpu != NULL) {
    uc_x64e_shadow_fini (mShadowUc);
    cpu->RegisterInterruptHandler (cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, NULL);
  }
}

/* ------------------------------------------------------------- menu */
static INTN
choose (CONST CHAR16 *Title, CHAR16 **Items, UINTN Count)
{
  UINTN         sel = 0, i, idx;
  EFI_INPUT_KEY k;

  for (;;) {
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"\n  x64 Engine - x86-64 PC for the Orange Pi Zero 2W\n\n");
    Print (L"  Hard disk: %s\n  Network:   %s\n\n  %s\n\n",
           mHdPresent ? mHd.Name : L"none - the microSD card has no free space and no x64disk.img was found",
           mNetName, Title);
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
  CHAR16                       *items[MAX_SOURCES + 1 > MAX_BOOT_ENTRIES ? MAX_SOURCES + 1 : MAX_BOOT_ENTRIES];
  CHAR16                       titles[MAX_BOOT_ENTRIES][96];
  CHAR16                       installed[128];
  iso_t                        iso, *vol;
  INTN                         sel, src, ent;
  UINTN                        i, first;
  UINT64                       free_mb, guest_mb;
  uint8_t                      mac[6];
  BOOLEAN                      have_net;

  mT0 = (uint64_t)efi_get_timer_ns ();
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Print (L"\n  x64 Engine: looking for ISOs, the hard disk and the network...\n");

  read_config (ImageHandle);
  scan_block_devices ();
  scan_iso_files ();
  scan_exfat ();
  find_disk_file ();
  if (!mHdPresent) {
    find_sd_free_space (ImageHandle);
  }

  if (mHdPresent) {
    mHdLinux = disk_find_linux (&mHd, mHd.Size, &mHdFs) ? TRUE : FALSE;
  }

  have_net = net_open (mac) == 0;

  if (mSrcCount == 0 && !mHdLinux) {
    Print (L"\n  x64 Engine: no x86-64 ISO found.\n\n"
           L"  Write an ISO to a USB drive (balenaEtcher), or copy the .iso file\n"
           L"  to a FAT32 or exFAT drive (Ventoy works too), then try again.\n\n  Press any key.\n");
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
    return EFI_NOT_FOUND;
  }

  for (;;) {
    first = 0;
    if (mHdLinux) {
      UnicodeSPrint (installed, sizeof (installed), L"Installed system on the hard disk");
      items[first++] = installed;
    }

    for (i = 0; i < mSrcCount; i++) {
      items[first + i] = mSrc[i].Name;
    }

    sel = choose (L"Choose what to start:", items, first + mSrcCount);
    if (sel < 0) {
      return EFI_ABORTED;
    }

    if ((UINTN)sel < first) {
      src = -1;
      vol = &mHdFs;
    } else {
      src = sel - (INTN)first;
      vol = &iso;
      if (iso_open (&iso, &mSrc[src])) {
        Print (L"\n  This is not a readable ISO. Press any key.\n");
        gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
        continue;
      }
    }

    if (bootcfg_scan (vol, &bl) == 0) {
      Print (L"\n  No boot entries found. Press any key.\n");
      gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
      continue;
    }

    for (i = 0; i < (UINTN)bl.count; i++) {
      UnicodeSPrint (titles[i], sizeof (titles[i]), L"%a", bl.e[i].title);
      items[i] = titles[i];
    }

    ent = bl.count == 1 ? 0 : choose (L"Choose the menu entry:", items, bl.count);
    if (ent >= 0) {
      break;
    }
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"\n  Starting the x86-64 machine...\n");

  /*
   * Guest RAM: what is free minus the translation cache (100 MB), the
   * kernel image being loaded and a reserve for the firmware's own drivers
   * (USB DMA buffers etc.).
   */
  free_mb  = free_memory_mb ();
  guest_mb = free_mb > 100 + 32 + 128 + 128 + 24 ? free_mb - 100 - 32 - 128 - 24 : 128;
  guest_mb = guest_mb > 1024 ? 1024 : guest_mb & ~31ULL;
  host_log ("x64e: %lu MB free, %lu MB for the x86 machine\n", free_mb, guest_mb);
  Print (L"  Memory: %lu MB free, %lu MB for the x86 machine\n", free_mb, guest_mb);
  if (machine_init (&m, guest_mb) != 0) {
    Print (L"  Not enough memory for the x86 machine. Press any key.\n");
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
    return EFI_OUT_OF_RESOURCES;
  }

  host_log ("x64e: %u MB guest RAM\n", (unsigned)(m.ram_size >> 20));
  m.boot_ns = epoch_seconds () * 1000000000ULL;

  /* the hard disk is always the first disk (vda), the ISO the second */
  if (mHdPresent) {
    machine_add_disk (&m, &mHd, mHd.Size, 0);
    host_log ("x64e: hard disk: %s\n", mHdLinux ? "installed system found" : "empty or unknown");
  }

  if (src >= 0) {
    machine_add_disk (&m, &mSrc[src], mSrc[src].Size, 1);
  }

  if (have_net) {
    machine_add_net (&m, mac);
  }

  Print (L"  Hard disk: %s\n  Network:   %s\n", mHdPresent ? mHd.Name : L"none", mNetName);

  if (!EFI_ERROR (gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&gop)) ||
      !EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop))) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;

    if (mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
      machine_set_fb (&m, (void *)(UINTN)gop->Mode->FrameBufferBase, mi->HorizontalResolution,
                      mi->VerticalResolution, mi->PixelsPerScanLine * 4);
    }
  }

  if (iso_boot (&m, vol, &bl.e[ent],
                mCfgAslr ? "console=ttyS0,115200 console=tty0 loglevel=6 nolapic noapic"
                         : "console=ttyS0,115200 console=tty0 loglevel=6 nolapic noapic norandmaps")) {
    Print (L"  Could not load the kernel. Press any key.\n");
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &i);
    return EFI_LOAD_ERROR;
  }

  if (m.fb != NULL) {
    SetMem (m.fb, (UINTN)m.fb_stride * m.fb_h, 0);
  }

  /*
   * Run at EL1 so that TTBR1 is free for the x86 address space (the
   * firmware keeps working there, see x64e_el.c).
   */
  {
    uint64_t fw_vbar = 0, fw_hcr = 0;
    BOOLEAN  el1     = FALSE;

    if (x64e_current_el () == 2 && mCfgEl1) {
      fw_vbar = x64e_read_vbar_el2 ();
      fw_hcr  = x64e_read_hcr_el2 ();
      x64e_enter_el1 ((uint64_t)(UINTN)x64e_el2_vectors,
                      X64E_TCR_EPD1 | (16ULL << 16) | (2ULL << 30) | (1ULL << 24) | (1ULL << 26) | (3ULL << 28));
      el1 = TRUE;
    }

    host_log ("x64e: running at EL%lu\n", x64e_current_el ());
    {
      EFI_CPU_ARCH_PROTOCOL *shadow = el1 && mCfgShadow ? shadow_start (m.uc) : NULL;

      machine_run (&m);
      shadow_stop (shadow);
    }

    if (el1) {
      x64e_leave_el1 (fw_vbar, fw_hcr);
      host_log ("x64e: back at EL%lu\n", x64e_current_el ());
    }
  }

  stop_kick_timer ();
  flush_disks ();

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
