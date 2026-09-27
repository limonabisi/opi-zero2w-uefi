/** @file
  OpiSetup - graphical firmware setup for the Orange Pi Zero 2W.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPI_SETUP_H_
#define OPI_SETUP_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>

//
// Assets (tools/gen_assets.py)
//
typedef struct {
  UINT16    Code;
  INT8      W, H;
  INT8      X, Y;          // offset of the bitmap from the pen position / line top
  INT8      Advance;
  UINT32    Offset;        // into the 4-bit alpha data
} GLYPH;

typedef struct {
  INT32          LineHeight;
  INT32          Ascent;
  UINT32         Count;
  CONST GLYPH    *Glyphs;
  CONST UINT8    *Data;
} FONT;

typedef struct {
  INT32          W, H;
  CONST UINT8    *Data;
} ALPHA_BITMAP;

//
// Canvas
//
typedef UINT32 COLOR;          // 0x00RRGGBB

typedef struct {
  UINT32    *Buf;
  INT32     W, H;
} CANVAS;

extern CANVAS  gCanvas;

VOID  GfxFill (INT32 X, INT32 Y, INT32 W, INT32 H, COLOR C);
VOID  GfxBlend (INT32 X, INT32 Y, INT32 W, INT32 H, COLOR C, UINT32 Alpha);
VOID  GfxRoundRect (INT32 X, INT32 Y, INT32 W, INT32 H, INT32 R, COLOR C);
VOID  GfxShadow (INT32 X, INT32 Y, INT32 W, INT32 H, INT32 R);
VOID  GfxCircle (INT32 Cx, INT32 Cy, INT32 R, COLOR C);
VOID  GfxAlpha (CONST ALPHA_BITMAP *Bm, INT32 X, INT32 Y, COLOR C);
INT32 GfxText (CONST FONT *F, INT32 X, INT32 Y, CONST CHAR16 *S, COLOR C);
INT32 GfxTextWidth (CONST FONT *F, CONST CHAR16 *S);
VOID  GfxTriangle (INT32 X, INT32 Y, INT32 Size, BOOLEAN Up, COLOR C);

extern CONST FONT          *gFontSmall, *gFontBody, *gFontMedium, *gFontNav, *gFontTitle, *gFontLogo, *gFontChip;
extern CONST ALPHA_BITMAP  *gIconHome, *gIconSliders, *gIconClock, *gIconShield, *gIconStartup,
                           *gIconPower, *gIconChevron, *gIconBack, *gIconCheck, *gIconChip;

//
// Palette
//
#define COL_SIDEBAR        0x1B1E24
#define COL_SIDEBAR_SEL    0x2B3038
#define COL_ACCENT         0xF28C28
#define COL_ACCENT_SOFT    0xFFF3E6
#define COL_BG             0xF2F3F5
#define COL_CARD           0xFFFFFF
#define COL_LINE           0xE5E7EB
#define COL_TEXT           0x1F2328
#define COL_TEXT_2         0x6B7280
#define COL_TEXT_SIDE      0xC9CDD4
#define COL_WHITE          0xFFFFFF
#define COL_TOGGLE_OFF     0xC4C8CF
#define COL_DANGER         0xD64545
#define COL_PIN            0x9A5A1C
#define COL_CHIP_TEXT      0x1B1E24

//
// Strings (Turkish / English)
//
typedef enum {
  StrSetup, StrMain, StrConfig, StrDateTime, StrSecurity, StrStartup, StrExit,
  StrMainSub, StrConfigSub, StrDateTimeSub, StrSecuritySub, StrStartupSub, StrExitSub,
  StrSystem, StrBoard, StrSoc, StrCpu, StrCpuTemp, StrCpuSec, StrCpuSpeed, StrCpuMax, StrCompatSec, StrX64Bridge, StrX64Desc, StrMemory, StrFirmware, StrFirmwareSec, StrBuildDate, StrUefi,
  StrDisplay, StrStorage, StrNoStorage, StrConsole,
  StrDisplaySec, StrHdmiRes, StrAuto, StrHdmiNote, StrGeneral, StrLanguage, StrTimeout, StrSeconds,
  StrNow, StrDate, StrTime, StrYear, StrMonth, StrDay, StrHour, StrMinute, StrApplyTime, StrTimeSet,
  StrSecureBoot, StrSbDesc, StrSbOff, StrSbOn, StrSbState, StrSbActive, StrSbInactive, StrSbFailed, StrNotSupported, StrTpm, StrNone, StrAcpi, StrAcpiOn, StrVirtSec, StrVirt, StrVirtOn, StrVirtOff, StrVGic, StrPassword, StrNotSet,
  StrBootOrder, StrBootOrderHint, StrBootNow, StrBootFailed, StrOneTime,
  StrSaveExit, StrSaveRestart, StrDiscardExit, StrRestart, StrShutdown, StrShell, StrClassic,
  StrSaveExitDesc, StrSaveRestartDesc, StrDiscardExitDesc, StrRestartDesc, StrShutdownDesc, StrShellDesc, StrClassicDesc, StrBootManager, StrBootManagerDesc,
  StrKeyMove, StrKeyChange, StrKeyBack, StrKeySave,
  StrYes, StrNo, StrOk, StrQuitNoSave, StrQuitNoSaveMsg, StrSaveQ, StrSaveMsg, StrRestartNeeded,
  StrBootQ, StrBootMsg, StrBootMenu, StrBootMenuSub, StrEnterSetup, StrContinueBoot, StrSaved, StrTurkish, StrEnglish,
  StrCount
} STR_ID;

extern UINTN  gLanguage;       // 0 = Turkish, 1 = English
CONST CHAR16 *S (STR_ID Id);

//
// System information (SysInfo.c)
//
typedef struct {
  CHAR16    Board[64];
  CHAR16    Soc[64];
  CHAR16    Cpu[64];
  CHAR16    Memory[64];
  CHAR16    Firmware[64];
  CHAR16    BuildDate[32];
  CHAR16    Uefi[32];
  CHAR16    Display[48];
  CHAR16    Storage[4][64];
  UINTN     StorageCount;
} SYS_INFO;

VOID  SysInfoCollect (SYS_INFO *Info);

//
// Secure Boot (SecureBoot.c)
//
BOOLEAN     SbPkEnrolled (VOID);
BOOLEAN     SbActive (VOID);
EFI_STATUS  SbEnable (VOID);
EFI_STATUS  SbDisable (VOID);

#endif
