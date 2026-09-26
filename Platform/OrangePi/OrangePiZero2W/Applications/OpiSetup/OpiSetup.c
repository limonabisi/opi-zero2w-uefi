/** @file
  OpiSetup - graphical firmware setup for the Orange Pi Zero 2W (H618).

  A sidebar + card style setup utility drawn directly on the GOP frame
  buffer. It is registered as the platform's Boot Manager Menu, so it
  opens on ESC/F2 during boot and whenever nothing bootable is found.
  Without a graphics output it hands over to the classic text UiApp.

  Keys: arrows, Enter, Esc, +/- (boot order), F10 (save and exit).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "OpiSetup.h"

#include <Guid/GlobalVariable.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextInEx.h>

//
// Variables
//
STATIC EFI_GUID  mHdmiVarGuid  = { 0x3d4b6c0e, 0x8a41, 0x4f3a, { 0x9b, 0x0e, 0x51, 0x6d, 0x2a, 0x7c, 0x44, 0x19 } };
STATIC EFI_GUID  mSetupVarGuid = { 0x9a1e3b57, 0x2c64, 0x4d8f, { 0xb0, 0x71, 0x5e, 0x8c, 0x3a, 0x42, 0x6d, 0x19 } };
STATIC EFI_GUID  mUiAppGuid    = { 0x462CAA21, 0x7614, 0x4503, { 0x83, 0x6E, 0x8A, 0xB6, 0xF4, 0x66, 0x23, 0x31 } };
STATIC EFI_GUID  mShellGuid    = { 0x7C04A583, 0x9E3E, 0x4f1c, { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 } };

//
// Layout (designed for 1280x720, stretches with the screen)
//
#define SIDE_W     300
#define PAD        44
#define LIST_TOP   128
#define ROW_H      56
#define ROW_H2     68
#define ROW_GAP    8
#define SECTION_H  36
#define FOOTER_H   52

typedef enum {
  ItSection,
  ItInfo,
  ItToggle,
  ItChoice,
  ItNumber,
  ItAction,
  ItBoot
} ITEM_TYPE;

typedef enum {
  ActNone,
  ActSaveExit,
  ActSaveRestart,
  ActDiscardExit,
  ActRestart,
  ActShutdown,
  ActShell,
  ActClassic,
  ActSetTime,
  ActBoot            // + boot entry index
} ACTION;

typedef struct {
  ITEM_TYPE       Type;
  CONST CHAR16    *Label;
  CONST CHAR16    *Desc;
  CHAR16          Value[80];
  UINTN           *Choice;
  CONST CHAR16    *Options[4];
  UINTN           OptCount;
  INT32           *Number;
  INT32           Min, Max;
  CONST CHAR16    *Suffix;
  UINTN           Action;
  BOOLEAN         Danger;
} ITEM;

#define MAX_ITEMS  32
#define MAX_BOOT   16

typedef struct {
  UINT16    Number;
  CHAR16    Desc[64];
} BOOT_ENTRY;

//
// State
//
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
STATIC EFI_HANDLE                    mImageHandle;
STATIC BOOLEAN                       mBootMenuActive;
STATIC VOID                          (*DrawBootMenuHook)(VOID);
STATIC SYS_INFO                      mInfo;

STATIC UINTN       mPage;                  // sidebar selection
STATIC BOOLEAN     mInContent;
STATIC UINTN       mItem;                  // focused item
STATIC INT32       mScroll;

STATIC UINTN       mHdmiMode, mHdmiModeOrig;
STATIC INT32       mTimeout, mTimeoutOrig;
STATIC UINTN       mLang, mLangOrig;
STATIC BOOT_ENTRY  mBoot[MAX_BOOT];
STATIC UINTN       mBootCount;
STATIC UINT16      mBootOrderOrig[MAX_BOOT];
STATIC UINT16      *mHiddenOrder;          // hidden/inactive entries, kept at the end
STATIC UINTN       mHiddenCount;
STATIC INT32       mYear, mMonth, mDay, mHour, mMinute;

STATIC ITEM   mItems[MAX_ITEMS];
STATIC UINTN  mItemCount;

typedef struct {
  CONST ALPHA_BITMAP    **Icon;
  STR_ID                Name;
  STR_ID                Sub;
} PAGE;

STATIC CONST PAGE  mPages[] = {
  { &gIconHome,    StrMain,     StrMainSub     },
  { &gIconSliders, StrConfig,   StrConfigSub   },
  { &gIconClock,   StrDateTime, StrDateTimeSub },
  { &gIconShield,  StrSecurity, StrSecuritySub },
  { &gIconStartup, StrStartup,  StrStartupSub  },
  { &gIconPower,   StrExit,     StrExitSub     },
};

#define PAGE_MAIN      0
#define PAGE_CONFIG    1
#define PAGE_DATETIME  2
#define PAGE_SECURITY  3
#define PAGE_STARTUP   4
#define PAGE_EXIT      5

// ---------------------------------------------------------------- items ---

STATIC
ITEM *
Add (
  ITEM_TYPE     Type,
  CONST CHAR16  *Label
  )
{
  ITEM  *It;

  if (mItemCount >= MAX_ITEMS) {
    return &mItems[MAX_ITEMS - 1];
  }

  It = &mItems[mItemCount++];
  ZeroMem (It, sizeof (*It));
  It->Type  = Type;
  It->Label = Label;
  return It;
}

STATIC
VOID
AddInfo (
  CONST CHAR16  *Label,
  CONST CHAR16  *Value
  )
{
  ITEM  *It;

  It = Add (ItInfo, Label);
  StrCpyS (It->Value, ARRAY_SIZE (It->Value), Value);
}

STATIC
ITEM *
AddNumber (
  CONST CHAR16  *Label,
  INT32         *Val,
  INT32         Min,
  INT32         Max,
  CONST CHAR16  *Suffix
  )
{
  ITEM  *It;

  It         = Add (ItNumber, Label);
  It->Number = Val;
  It->Min    = Min;
  It->Max    = Max;
  It->Suffix = Suffix;
  return It;
}

STATIC
VOID
AddAction (
  CONST CHAR16  *Label,
  CONST CHAR16  *Desc,
  UINTN         Action,
  BOOLEAN       Danger
  )
{
  ITEM  *It;

  It         = Add (ItAction, Label);
  It->Desc   = Desc;
  It->Action = Action;
  It->Danger = Danger;
}

STATIC
VOID
BuildPage (
  VOID
  )
{
  ITEM   *It;
  UINTN  Index;

  mItemCount = 0;

  switch (mPage) {
    case PAGE_MAIN:
      Add (ItSection, S (StrSystem));
      AddInfo (S (StrBoard), mInfo.Board);
      AddInfo (S (StrSoc), mInfo.Soc);
      AddInfo (S (StrCpu), mInfo.Cpu);
      AddInfo (S (StrMemory), mInfo.Memory);
      AddInfo (S (StrDisplay), mInfo.Display);
      AddInfo (S (StrConsole), L"UART0  ·  115200 8N1");
      Add (ItSection, S (StrFirmwareSec));
      AddInfo (S (StrFirmware), mInfo.Firmware);
      AddInfo (S (StrBuildDate), mInfo.BuildDate);
      AddInfo (S (StrUefi), mInfo.Uefi);
      Add (ItSection, S (StrStorage));
      if (mInfo.StorageCount == 0) {
        AddInfo (S (StrStorage), S (StrNoStorage));
      }

      for (Index = 0; Index < mInfo.StorageCount; Index++) {
        AddInfo (S (StrStorage), mInfo.Storage[Index]);
      }

      break;

    case PAGE_CONFIG:
      Add (ItSection, S (StrDisplaySec));
      It             = Add (ItChoice, S (StrHdmiRes));
      It->Desc       = S (StrHdmiNote);
      It->Choice     = &mHdmiMode;
      It->Options[0] = S (StrAuto);
      It->Options[1] = L"1280 x 720  ·  60 Hz";
      It->Options[2] = L"1920 x 1080  ·  60 Hz";
      It->OptCount   = 3;
      Add (ItSection, S (StrGeneral));
      It             = Add (ItChoice, S (StrLanguage));
      It->Choice     = &mLang;
      It->Options[0] = S (StrTurkish);
      It->Options[1] = S (StrEnglish);
      It->OptCount   = 2;
      AddNumber (S (StrTimeout), &mTimeout, 0, 30, S (StrSeconds));
      break;

    case PAGE_DATETIME:
      Add (ItSection, S (StrDate));
      AddNumber (S (StrDay), &mDay, 1, 31, NULL);
      AddNumber (S (StrMonth), &mMonth, 1, 12, NULL);
      AddNumber (S (StrYear), &mYear, 2024, 2099, NULL);
      Add (ItSection, S (StrTime));
      AddNumber (S (StrHour), &mHour, 0, 23, NULL);
      AddNumber (S (StrMinute), &mMinute, 0, 59, NULL);
      Add (ItSection, L"");
      AddAction (S (StrApplyTime), NULL, ActSetTime, FALSE);
      break;

    case PAGE_SECURITY:
      Add (ItSection, S (StrSecurity));
      AddInfo (S (StrSecureBoot), S (StrNotSupported));
      AddInfo (S (StrTpm), S (StrNone));
      AddInfo (S (StrPassword), S (StrNotSet));
      AddInfo (S (StrAcpi), S (StrAcpiOn));
      break;

    case PAGE_STARTUP:
      Add (ItSection, S (StrBootOrder));
      for (Index = 0; Index < mBootCount; Index++) {
        It         = Add (ItBoot, mBoot[Index].Desc);
        It->Action = ActBoot + Index;
        UnicodeSPrint (It->Value, sizeof (It->Value), L"%u", (UINT32)Index + 1);
      }

      if (mBootCount == 0) {
        AddInfo (S (StrStorage), S (StrNoStorage));
      }

      Add (ItSection, S (StrGeneral));
      AddNumber (S (StrTimeout), &mTimeout, 0, 30, S (StrSeconds));
      break;

    case PAGE_EXIT:
    default:
      Add (ItSection, S (StrExit));
      AddAction (S (StrSaveExit), S (StrSaveExitDesc), ActSaveExit, FALSE);
      AddAction (S (StrSaveRestart), S (StrSaveRestartDesc), ActSaveRestart, FALSE);
      AddAction (S (StrDiscardExit), S (StrDiscardExitDesc), ActDiscardExit, FALSE);
      AddAction (S (StrRestart), S (StrRestartDesc), ActRestart, FALSE);
      AddAction (S (StrShutdown), S (StrShutdownDesc), ActShutdown, TRUE);
      Add (ItSection, L"");
      AddAction (S (StrShell), S (StrShellDesc), ActShell, FALSE);
      AddAction (S (StrClassic), S (StrClassicDesc), ActClassic, FALSE);
      break;
  }
}

STATIC
BOOLEAN
Focusable (
  CONST ITEM  *It
  )
{
  return It->Type != ItSection;      // info rows too, so long pages can be scrolled
}

STATIC
INT32
ItemHeight (
  CONST ITEM  *It
  )
{
  if (It->Type == ItSection) {
    return (It->Label[0] == 0) ? 12 : SECTION_H;
  }

  return ((It->Desc != NULL) ? ROW_H2 : ROW_H) + ROW_GAP;
}

STATIC
VOID
FirstFocusable (
  VOID
  )
{
  for (mItem = 0; mItem < mItemCount; mItem++) {
    if (Focusable (&mItems[mItem])) {
      return;
    }
  }

  mItem = 0;
}

STATIC
BOOLEAN
PageHasFocusable (
  VOID
  )
{
  UINTN  I;

  for (I = 0; I < mItemCount; I++) {
    if (Focusable (&mItems[I])) {
      return TRUE;
    }
  }

  return FALSE;
}

// ------------------------------------------------------------ settings ---

STATIC
VOID
LoadSettings (
  VOID
  )
{
  UINT8                         B;
  UINT16                        T;
  UINTN                         Size;
  EFI_TIME                      Now;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Opts;
  UINTN                         OptCount;
  UINTN                         Index;

  Size = 1;
  B    = 0;
  gRT->GetVariable (L"HdmiMode", &mHdmiVarGuid, NULL, &Size, &B);
  mHdmiMode = (B <= 2) ? B : 0;

  Size = sizeof (T);
  T    = 3;
  gRT->GetVariable (L"Timeout", &gEfiGlobalVariableGuid, NULL, &Size, &T);
  mTimeout = (T > 30) ? 30 : T;

  Size = 1;
  B    = 1;                                           // English unless set
  gRT->GetVariable (L"Language", &mSetupVarGuid, NULL, &Size, &B);
  mLang     = B & 1;
  gLanguage = mLang;

  if (!EFI_ERROR (gRT->GetTime (&Now, NULL))) {
    mYear   = Now.Year;
    mMonth  = Now.Month;
    mDay    = Now.Day;
    mHour   = Now.Hour;
    mMinute = Now.Minute;
  }

  //
  // Boot options, in BootOrder order. Only active, visible ones can be
  // reordered here; the others keep their place at the end of BootOrder.
  //
  mBootCount   = 0;
  mHiddenCount = 0;
  Opts         = EfiBootManagerGetLoadOptions (&OptCount, LoadOptionTypeBoot);
  mHiddenOrder = AllocateZeroPool ((OptCount + 1) * sizeof (UINT16));
  for (Index = 0; Index < OptCount; Index++) {
    if (((Opts[Index].Attributes & LOAD_OPTION_ACTIVE) != 0) &&
        ((Opts[Index].Attributes & LOAD_OPTION_HIDDEN) == 0) &&
        (mBootCount < MAX_BOOT))
    {
      mBoot[mBootCount].Number = (UINT16)Opts[Index].OptionNumber;
      StrnCpyS (mBoot[mBootCount].Desc, ARRAY_SIZE (mBoot[0].Desc), Opts[Index].Description, ARRAY_SIZE (mBoot[0].Desc) - 1);
      mBootOrderOrig[mBootCount] = mBoot[mBootCount].Number;
      mBootCount++;
    } else if (mHiddenOrder != NULL) {
      mHiddenOrder[mHiddenCount++] = (UINT16)Opts[Index].OptionNumber;
    }
  }

  EfiBootManagerFreeLoadOptions (Opts, OptCount);

  mHdmiModeOrig = mHdmiMode;
  mTimeoutOrig  = mTimeout;
  mLangOrig     = mLang;
}

STATIC
BOOLEAN
BootOrderChanged (
  VOID
  )
{
  UINTN  I;

  for (I = 0; I < mBootCount; I++) {
    if (mBoot[I].Number != mBootOrderOrig[I]) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
Dirty (
  VOID
  )
{
  return (mHdmiMode != mHdmiModeOrig) || (mTimeout != mTimeoutOrig) ||
         (mLang != mLangOrig) || BootOrderChanged ();
}

STATIC
VOID
SaveSettings (
  VOID
  )
{
  UINT8   B;
  UINT16  T;
  UINT16  *Order;
  UINTN   I;

  B = (UINT8)mHdmiMode;
  gRT->SetVariable (L"HdmiMode", &mHdmiVarGuid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, 1, &B);

  T = (UINT16)mTimeout;
  gRT->SetVariable (
         L"Timeout",
         &gEfiGlobalVariableGuid,
         EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
         sizeof (T),
         &T
         );

  B = (UINT8)mLang;
  gRT->SetVariable (L"Language", &mSetupVarGuid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, 1, &B);

  if (BootOrderChanged ()) {
    Order = AllocatePool ((mBootCount + mHiddenCount) * sizeof (UINT16));
    if (Order != NULL) {
      for (I = 0; I < mBootCount; I++) {
        Order[I] = mBoot[I].Number;
      }

      for (I = 0; I < mHiddenCount; I++) {
        Order[mBootCount + I] = mHiddenOrder[I];
      }

      gRT->SetVariable (
             EFI_BOOT_ORDER_VARIABLE_NAME,
             &gEfiGlobalVariableGuid,
             EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
             (mBootCount + mHiddenCount) * sizeof (UINT16),
             Order
             );
      FreePool (Order);
    }
  }

  for (I = 0; I < mBootCount; I++) {
    mBootOrderOrig[I] = mBoot[I].Number;
  }

  mHdmiModeOrig = mHdmiMode;
  mTimeoutOrig  = mTimeout;
  mLangOrig     = mLang;
}

// ------------------------------------------------------------ drawing ---

STATIC
VOID
Present (
  VOID
  )
{
  mGop->Blt (
          mGop,
          (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)gCanvas.Buf,
          EfiBltBufferToVideo,
          0,
          0,
          0,
          0,
          gCanvas.W,
          gCanvas.H,
          0
          );
}

//
// The boot logo, drawn natively: an orange "H618" chip with pins, then
// "Zero 2W" and "UEFI · EDK2". (X, Y) is the top-left corner of the pins.
//
STATIC
VOID
DrawLogo (
  INT32  X,
  INT32  Y
  )
{
  INT32  Bx, By, I, P;
  INT32  Tw;

  Bx = X + 8;
  By = Y + 8;
  for (I = 0; I < 6; I++) {
    P = Bx + 7 + I * 9;
    GfxRoundRect (P, Y, 4, 12, 1, COL_PIN);                 // top
    GfxRoundRect (P, By + 58 - 4, 4, 12, 1, COL_PIN);       // bottom
    P = By + 7 + I * 9;
    GfxRoundRect (X, P, 12, 4, 1, COL_PIN);                 // left
    GfxRoundRect (Bx + 58 - 4, P, 12, 4, 1, COL_PIN);       // right
  }

  GfxRoundRect (Bx, By, 58, 58, 7, COL_ACCENT);
  Tw = GfxTextWidth (gFontChip, L"H618");
  GfxText (gFontChip, Bx + (58 - Tw) / 2, By + (58 - gFontChip->LineHeight) / 2 + 1, L"H618", COL_CHIP_TEXT);

  GfxText (gFontLogo, X + 88, Y + 6, L"Zero 2W", COL_WHITE);
  GfxText (gFontSmall, X + 90, Y + 44, L"UEFI  ·  EDK2", COL_ACCENT);
}

STATIC
VOID
DrawSidebar (
  VOID
  )
{
  INT32         Y;
  UINTN         I;
  BOOLEAN       Sel;
  COLOR         Fg;

  GfxFill (0, 0, SIDE_W, gCanvas.H, COL_SIDEBAR);

  DrawLogo (24, 18);

  Y = 124;
  for (I = 0; I < ARRAY_SIZE (mPages); I++) {
    Sel = (I == mPage);
    if (Sel) {
      GfxRoundRect (16, Y, SIDE_W - 32, 46, 10, mInContent ? COL_SIDEBAR_SEL : COL_ACCENT);
      if (mInContent) {
        GfxRoundRect (16, Y + 10, 4, 26, 2, COL_ACCENT);
      }
    }

    Fg = Sel ? COL_WHITE : COL_TEXT_SIDE;
    GfxAlpha (*mPages[I].Icon, 34, Y + 12, Fg);
    GfxText (gFontNav, 70, Y + 11, S (mPages[I].Name), Fg);
    Y += 54;
  }

  // SoC badge
  GfxFill (24, gCanvas.H - 96, SIDE_W - 48, 1, 0x30353D);
  GfxText (gFontMedium, 28, gCanvas.H - 78, L"Allwinner H618", COL_WHITE);
  GfxText (gFontSmall, 28, gCanvas.H - 54, mInfo.Firmware, 0x8A919C);
}

STATIC
VOID
DrawToggle (
  INT32    X,
  INT32    Y,
  BOOLEAN  On
  )
{
  GfxRoundRect (X, Y, 48, 26, 13, On ? COL_ACCENT : COL_TOGGLE_OFF);
  GfxCircle (On ? X + 35 : X + 13, Y + 13, 10, COL_WHITE);
}

STATIC
VOID
DrawKeycap (
  INT32         *X,
  INT32         Y,
  CONST CHAR16  *Key,
  BOOLEAN       Arrows,
  CONST CHAR16  *Label
  )
{
  INT32  W;

  W = Arrows ? 40 : GfxTextWidth (gFontSmall, Key) + 18;
  GfxRoundRect (*X, Y, W, 26, 6, COL_WHITE);
  GfxFill (*X + 2, Y + 25, W - 4, 1, COL_LINE);
  if (Arrows) {
    GfxTriangle (*X + 7, Y + 9, 6, TRUE, COL_TEXT);
    GfxTriangle (*X + 21, Y + 11, 6, FALSE, COL_TEXT);
  } else {
    GfxText (gFontSmall, *X + 9, Y + 3, Key, COL_TEXT);
  }

  *X += W + 8;
  *X += GfxText (gFontSmall, *X, Y + 3, Label, COL_TEXT_2) + 22;
}

STATIC
VOID
ChoiceBox (
  CONST ITEM  *It,
  INT32       RowX,
  INT32       RowY,
  INT32       RowW,
  INT32       RowH,
  INT32       *BoxX,
  INT32       *BoxY
  )
{
  *BoxX = RowX + RowW - 20 - 250;
  *BoxY = RowY + (RowH - 38) / 2;
}

STATIC
VOID
DrawRow (
  CONST ITEM  *It,
  INT32       X,
  INT32       Y,
  INT32       W,
  BOOLEAN     Focus
  )
{
  INT32         H;
  INT32         Ty;
  INT32         Vx;
  INT32         Bx, By;
  CHAR16        Buf[48];
  COLOR         LabelCol;

  H = ((It->Desc != NULL) ? ROW_H2 : ROW_H);
  GfxRoundRect (X, Y, W, H, 12, Focus ? COL_ACCENT_SOFT : COL_CARD);
  if (Focus) {
    GfxRoundRect (X, Y + 12, 4, H - 24, 2, COL_ACCENT);
  }

  LabelCol = It->Danger ? COL_DANGER : COL_TEXT;
  if (It->Type == ItBoot) {
    GfxCircle (X + 34, Y + H / 2, 14, Focus ? COL_ACCENT : 0xEEF0F3);
    GfxText (gFontMedium, X + 34 - GfxTextWidth (gFontMedium, It->Value) / 2, Y + H / 2 - 12, It->Value, Focus ? COL_WHITE : COL_TEXT_2);
    GfxText (gFontBody, X + 62, Y + (H - gFontBody->LineHeight) / 2 + 1, It->Label, LabelCol);
  } else if (It->Desc != NULL) {
    GfxText (gFontBody, X + 22, Y + 12, It->Label, LabelCol);
    GfxText (gFontSmall, X + 22, Y + 38, It->Desc, COL_TEXT_2);
  } else {
    GfxText (gFontBody, X + 22, Y + (H - gFontBody->LineHeight) / 2 + 1, It->Label, LabelCol);
  }

  Ty = Y + (H - gFontBody->LineHeight) / 2 + 1;
  switch (It->Type) {
    case ItInfo:
      Vx = X + W - 22 - GfxTextWidth (gFontBody, It->Value);
      GfxText (gFontBody, Vx, Ty, It->Value, COL_TEXT_2);
      break;

    case ItToggle:
      DrawToggle (X + W - 22 - 48, Y + (H - 26) / 2, (BOOLEAN)(*It->Choice != 0));
      break;

    case ItChoice:
      ChoiceBox (It, X, Y, W, H, &Bx, &By);
      GfxRoundRect (Bx, By, 250, 38, 9, Focus ? COL_ACCENT : COL_LINE);
      GfxRoundRect (Bx + 1, By + 1, 248, 36, 8, COL_WHITE);
      GfxText (gFontBody, Bx + 14, By + (38 - gFontBody->LineHeight) / 2 + 1, It->Options[*It->Choice], COL_TEXT);
      GfxTriangle (Bx + 250 - 26, By + 16, 6, FALSE, Focus ? COL_ACCENT : COL_TEXT_2);
      break;

    case ItNumber:
      UnicodeSPrint (Buf, sizeof (Buf), (It->Suffix != NULL) ? L"%d %s" : L"%02d", *It->Number, It->Suffix);
      Vx = X + W - 22;
      GfxText (gFontMedium, Vx - 12, Ty, L">", Focus ? COL_ACCENT : COL_TEXT_2);
      Vx -= 12 + 14 + GfxTextWidth (gFontMedium, Buf);
      GfxText (gFontMedium, Vx, Ty, Buf, COL_TEXT);
      GfxText (gFontMedium, Vx - 26, Ty, L"<", Focus ? COL_ACCENT : COL_TEXT_2);
      break;

    case ItAction:
      GfxAlpha (gIconChevron, X + W - 22 - 22, Y + (H - 22) / 2, Focus ? COL_ACCENT : COL_TEXT_2);
      break;

    case ItBoot:
      if (Focus) {
        CONST CHAR16  *Hint = S (StrBootOrderHint);
        GfxText (gFontSmall, X + W - 22 - GfxTextWidth (gFontSmall, Hint), Y + (H - gFontSmall->LineHeight) / 2, Hint, COL_TEXT_2);
      }

      break;

    default:
      break;
  }
}

STATIC
VOID
DrawClock (
  INT32  RightX
  )
{
  EFI_TIME  Now;
  CHAR16    Buf[40];

  if (EFI_ERROR (gRT->GetTime (&Now, NULL))) {
    return;
  }

  UnicodeSPrint (Buf, sizeof (Buf), L"%02u.%02u.%04u   %02u:%02u:%02u", Now.Day, Now.Month, Now.Year, Now.Hour, Now.Minute, Now.Second);
  GfxText (gFontMedium, RightX - GfxTextWidth (gFontMedium, Buf), 44, Buf, COL_TEXT_2);
}

STATIC
VOID
DrawContent (
  VOID
  )
{
  INT32  X, W, Y;
  INT32  ListBottom;
  INT32  FocusTop, FocusBottom;
  UINTN  I;
  INT32  Kx;

  X = SIDE_W + PAD;
  W = gCanvas.W - X - PAD;

  GfxFill (SIDE_W, 0, gCanvas.W - SIDE_W, gCanvas.H, COL_BG);

  //
  // keep the focused row visible
  //
  ListBottom = gCanvas.H - FOOTER_H - 10;
  Y          = 0;
  FocusTop   = FocusBottom = 0;
  for (I = 0; I < mItemCount; I++) {
    if (I == mItem) {
      FocusTop    = Y;
      FocusBottom = Y + ItemHeight (&mItems[I]);
    }

    Y += ItemHeight (&mItems[I]);
  }

  if (!mInContent) {
    mScroll = 0;
  } else if (FocusBottom - mScroll > ListBottom - LIST_TOP) {
    mScroll = FocusBottom - (ListBottom - LIST_TOP);
  } else if (FocusTop - mScroll < 0) {
    mScroll = FocusTop;
  }

  if ((mItem < mItemCount) && (mItem > 0) && (mItems[mItem - 1].Type == ItSection) && (mScroll > FocusTop - SECTION_H)) {
    mScroll = MAX (0, FocusTop - SECTION_H);
  }

  Y = LIST_TOP - mScroll;
  for (I = 0; I < mItemCount; I++) {
    INT32  H = ItemHeight (&mItems[I]);
    if ((Y + H > LIST_TOP - 4) && (Y < ListBottom)) {
      if (mItems[I].Type == ItSection) {
        GfxText (gFontMedium, X + 4, Y + 6, mItems[I].Label, COL_TEXT_2);
      } else {
        DrawRow (&mItems[I], X, Y, W, mInContent && (I == mItem));
      }
    }

    Y += H;
  }

  // mask rows scrolled under the header / footer, then draw the header
  GfxFill (SIDE_W, 0, gCanvas.W - SIDE_W, LIST_TOP - 8, COL_BG);
  GfxFill (SIDE_W, ListBottom, gCanvas.W - SIDE_W, gCanvas.H - ListBottom, COL_BG);
  if (mScroll > 0) {
    for (I = 0; I < 16; I++) {
      GfxBlend (SIDE_W, LIST_TOP - 8 + (INT32)I, gCanvas.W - SIDE_W, 1, COL_BG, 255 - (UINT32)I * 16);
    }
  }

  if (Y > ListBottom) {
    for (I = 0; I < 24; I++) {
      GfxBlend (SIDE_W, ListBottom - 24 + (INT32)I, gCanvas.W - SIDE_W, 1, COL_BG, (UINT32)I * 10 + 15);
    }
  }

  GfxText (gFontTitle, X, 30, S (mPages[mPage].Name), COL_TEXT);
  GfxText (gFontBody, X, 78, S (mPages[mPage].Sub), COL_TEXT_2);
  DrawClock (X + W);

  // footer: key hints
  GfxFill (X, gCanvas.H - FOOTER_H, W, 1, COL_LINE);
  Kx = X;
  DrawKeycap (&Kx, gCanvas.H - FOOTER_H + 13, NULL, TRUE, S (StrKeyMove));
  DrawKeycap (&Kx, gCanvas.H - FOOTER_H + 13, L"Enter", FALSE, S (StrKeyChange));
  DrawKeycap (&Kx, gCanvas.H - FOOTER_H + 13, L"Esc", FALSE, S (StrKeyBack));
  DrawKeycap (&Kx, gCanvas.H - FOOTER_H + 13, L"F10", FALSE, S (StrKeySave));
}

STATIC
VOID
Render (
  VOID
  )
{
  if (mBootMenuActive) {
    DrawBootMenuHook ();
    return;
  }

  DrawContent ();
  DrawSidebar ();
}

// ------------------------------------------------------------- input ---

STATIC EFI_EVENT  mTick;

STATIC
EFI_INPUT_KEY
WaitKey (
  BOOLEAN  *Timeout
  )
{
  EFI_INPUT_KEY  Key;
  EFI_EVENT      Events[2];
  UINTN          Which;

  *Timeout  = FALSE;
  Events[0] = gST->ConIn->WaitForKey;
  Events[1] = mTick;
  ZeroMem (&Key, sizeof (Key));
  for ( ; ; ) {
    if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      return Key;
    }

    gBS->WaitForEvent (2, Events, &Which);
    if (Which == 1) {
      *Timeout = TRUE;
      return Key;
    }
  }
}

//
// Modal dialog: returns TRUE for the primary button.
//
STATIC
BOOLEAN
Dialog (
  CONST CHAR16  *Title,
  CONST CHAR16  *Msg,
  CONST CHAR16  *Primary,
  CONST CHAR16  *Secondary       // NULL: single button
  )
{
  INT32          Dw, Dh, Dx, Dy;
  UINTN          Sel;
  EFI_INPUT_KEY  Key;
  BOOLEAN        Tick;
  UINT32         *Saved;
  UINTN          Bytes;

  Bytes = (UINTN)gCanvas.W * gCanvas.H * 4;
  Saved = AllocateCopyPool (Bytes, gCanvas.Buf);
  Sel   = 0;
  for ( ; ; ) {
    if (Saved != NULL) {
      CopyMem (gCanvas.Buf, Saved, Bytes);
    }

    GfxBlend (0, 0, gCanvas.W, gCanvas.H, 0x0B0D10, 120);
    Dw = 560;
    Dh = 210;
    Dx = (gCanvas.W - Dw) / 2;
    Dy = (gCanvas.H - Dh) / 2;
    GfxShadow (Dx, Dy, Dw, Dh, 16);
    GfxRoundRect (Dx, Dy, Dw, Dh, 16, COL_CARD);
    GfxText (gFontNav, Dx + 32, Dy + 30, Title, COL_TEXT);
    GfxText (gFontBody, Dx + 32, Dy + 68, Msg, COL_TEXT_2);

    {
      INT32  Bw1 = GfxTextWidth (gFontMedium, Primary) + 48;
      INT32  Bx  = Dx + Dw - 32 - Bw1;
      INT32  By  = Dy + Dh - 30 - 44;
      if (Sel == 0) {
        GfxRoundRect (Bx - 3, By - 3, Bw1 + 6, 50, 13, 0xF8C58E);
      }

      GfxRoundRect (Bx, By, Bw1, 44, 10, COL_ACCENT);
      GfxText (gFontMedium, Bx + 24, By + 11, Primary, COL_WHITE);
      if (Secondary != NULL) {
        INT32  Bw2 = GfxTextWidth (gFontMedium, Secondary) + 48;
        Bx -= Bw2 + 14;
        if (Sel == 1) {
          GfxRoundRect (Bx - 3, By - 3, Bw2 + 6, 50, 13, 0xC9CDD4);
        }

        GfxRoundRect (Bx, By, Bw2, 44, 10, 0xEEF0F3);
        GfxText (gFontMedium, Bx + 24, By + 11, Secondary, COL_TEXT);
      }
    }

    Present ();
    Key = WaitKey (&Tick);
    if (Tick) {
      continue;
    }

    if (Secondary != NULL && ((Key.ScanCode == SCAN_LEFT) || (Key.ScanCode == SCAN_RIGHT) || (Key.UnicodeChar == L'\t'))) {
      Sel ^= 1;
    } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      break;
    } else if (Key.ScanCode == SCAN_ESC) {
      Sel = (Secondary != NULL) ? 1 : 0;
      break;
    }
  }

  if (Saved != NULL) {
    CopyMem (gCanvas.Buf, Saved, Bytes);
    FreePool (Saved);
  }

  return (BOOLEAN)(Sel == 0);
}

//
// Drop-down list for a choice item. Returns TRUE if the value changed.
//
STATIC
BOOLEAN
Dropdown (
  ITEM  *It
  )
{
  INT32          X, W, RowX, RowY, RowH;
  INT32          Bx, By, Lh;
  UINTN          I, Sel;
  EFI_INPUT_KEY  Key;
  BOOLEAN        Tick;
  UINT32         *Saved;
  UINTN          Bytes;

  // locate the row on screen
  X    = SIDE_W + PAD;
  W    = gCanvas.W - X - PAD;
  RowX = X;
  RowY = LIST_TOP - mScroll;
  for (I = 0; I < mItem; I++) {
    RowY += ItemHeight (&mItems[I]);
  }

  RowH = (It->Desc != NULL) ? ROW_H2 : ROW_H;
  ChoiceBox (It, RowX, RowY, W, RowH, &Bx, &By);
  Lh = 42;
  By = By + 44;
  if (By + (INT32)It->OptCount * Lh + 16 > gCanvas.H - 8) {
    By = By - 44 - 8 - (INT32)It->OptCount * Lh - 16;
  }

  Bytes = (UINTN)gCanvas.W * gCanvas.H * 4;
  Saved = AllocateCopyPool (Bytes, gCanvas.Buf);
  Sel   = *It->Choice;
  for ( ; ; ) {
    if (Saved != NULL) {
      CopyMem (gCanvas.Buf, Saved, Bytes);
    }

    GfxShadow (Bx, By, 250, (INT32)It->OptCount * Lh + 12, 12);
    GfxRoundRect (Bx, By, 250, (INT32)It->OptCount * Lh + 12, 12, COL_CARD);
    for (I = 0; I < It->OptCount; I++) {
      INT32  Oy = By + 6 + (INT32)I * Lh;
      if (I == Sel) {
        GfxRoundRect (Bx + 6, Oy, 238, Lh, 8, COL_ACCENT_SOFT);
      }

      GfxText (gFontBody, Bx + 18, Oy + (Lh - gFontBody->LineHeight) / 2 + 1, It->Options[I], (I == Sel) ? COL_TEXT : COL_TEXT_2);
      if (I == *It->Choice) {
        GfxAlpha (gIconCheck, Bx + 250 - 38, Oy + (Lh - 22) / 2, COL_ACCENT);
      }
    }

    Present ();
    Key = WaitKey (&Tick);
    if (Tick) {
      continue;
    }

    if ((Key.ScanCode == SCAN_UP) && (Sel > 0)) {
      Sel--;
    } else if ((Key.ScanCode == SCAN_DOWN) && (Sel + 1 < It->OptCount)) {
      Sel++;
    } else if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.ScanCode == SCAN_ESC)) {
      break;
    }
  }

  if (Saved != NULL) {
    FreePool (Saved);
  }

  if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) && (Sel != *It->Choice)) {
    *It->Choice = Sel;
    return TRUE;
  }

  return FALSE;
}

// ------------------------------------------------------------ actions ---

STATIC
EFI_STATUS
StartFvApp (
  EFI_GUID  *FileGuid
  )
{
  EFI_LOADED_IMAGE_PROTOCOL          *Li;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  Node;
  EFI_DEVICE_PATH_PROTOCOL           *Dp;
  EFI_HANDLE                         Image;
  EFI_STATUS                         Status;

  Status = gBS->HandleProtocol (mImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EfiInitializeFwVolDevicepathNode (&Node, FileGuid);
  Dp = AppendDevicePathNode (DevicePathFromHandle (Li->DeviceHandle), (EFI_DEVICE_PATH_PROTOCOL *)&Node);
  if (Dp == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LoadImage (TRUE, mImageHandle, Dp, NULL, 0, &Image);
  FreePool (Dp);
  if (!EFI_ERROR (Status)) {
    gST->ConOut->ClearScreen (gST->ConOut);
    Status = gBS->StartImage (Image, NULL, NULL);
  }

  return Status;
}

STATIC
VOID
BootOption (
  UINT16  Number
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  Opt;
  CHAR16                        Name[16];

  UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Number);
  if (EFI_ERROR (EfiBootManagerVariableToLoadOption (Name, &Opt))) {
    return;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  EfiBootManagerBoot (&Opt);
  if (EFI_ERROR (Opt.Status)) {
    Render ();
    Dialog (S (StrOneTime), S (StrBootFailed), S (StrOk), NULL);
  }

  EfiBootManagerFreeLoadOption (&Opt);
}

//
// Returns TRUE when setup should exit.
//
STATIC
BOOLEAN
DoAction (
  UINTN  Action
  )
{
  EFI_TIME  Now;
  BOOLEAN   HdmiChanged;

  HdmiChanged = (BOOLEAN)(mHdmiMode != mHdmiModeOrig);

  switch (Action) {
    case ActSaveExit:
      if (!Dialog (S (StrSaveQ), HdmiChanged ? S (StrRestartNeeded) : S (StrSaveMsg), S (StrYes), S (StrNo))) {
        return FALSE;
      }

      SaveSettings ();
      if (HdmiChanged) {
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      }

      return TRUE;

    case ActSaveRestart:
      SaveSettings ();
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      return TRUE;

    case ActDiscardExit:
      if (Dirty () && !Dialog (S (StrQuitNoSave), S (StrQuitNoSaveMsg), S (StrYes), S (StrNo))) {
        return FALSE;
      }

      return TRUE;

    case ActRestart:
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      return TRUE;

    case ActShutdown:
      if (Dialog (S (StrShutdown), S (StrShutdownDesc), S (StrYes), S (StrNo))) {
        gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
      }

      return FALSE;

    case ActShell:
      StartFvApp (&mShellGuid);
      return FALSE;

    case ActClassic:
      StartFvApp (&mUiAppGuid);
      return FALSE;

    case ActSetTime:
      if (!EFI_ERROR (gRT->GetTime (&Now, NULL))) {
        Now.Year   = (UINT16)mYear;
        Now.Month  = (UINT8)mMonth;
        Now.Day    = (UINT8)mDay;
        Now.Hour   = (UINT8)mHour;
        Now.Minute = (UINT8)mMinute;
        Now.Second = 0;
        gRT->SetTime (&Now);
        Dialog (S (StrDateTime), S (StrTimeSet), S (StrOk), NULL);
      }

      return FALSE;

    default:
      if (Action >= ActBoot) {
        if (Dialog (S (StrBootQ), S (StrBootMsg), S (StrYes), S (StrNo))) {
          BootOption (mBoot[Action - ActBoot].Number);
        }
      }

      return FALSE;
  }
}

// --------------------------------------------------------------- main ---

STATIC
BOOLEAN
HandleContentKey (
  EFI_INPUT_KEY  Key
  )
{
  ITEM        *It;
  UINTN       I;
  BOOT_ENTRY  Tmp;

  It = &mItems[mItem];

  if (Key.ScanCode == SCAN_UP) {
    for (I = mItem; I > 0; I--) {
      if (Focusable (&mItems[I - 1])) {
        mItem = I - 1;
        break;
      }
    }
  } else if (Key.ScanCode == SCAN_DOWN) {
    for (I = mItem + 1; I < mItemCount; I++) {
      if (Focusable (&mItems[I])) {
        mItem = I;
        break;
      }
    }
  } else if ((Key.ScanCode == SCAN_LEFT) || (Key.ScanCode == SCAN_RIGHT)) {
    INT32  Dir = (Key.ScanCode == SCAN_RIGHT) ? 1 : -1;
    if (It->Type == ItNumber) {
      *It->Number += Dir;
      if (*It->Number > It->Max) {
        *It->Number = It->Min;
      }

      if (*It->Number < It->Min) {
        *It->Number = It->Max;
      }
    } else if (It->Type == ItChoice) {
      *It->Choice = (*It->Choice + It->OptCount + Dir) % It->OptCount;
      if (It->Choice == &mLang) {
        gLanguage = mLang;
        BuildPage ();
      }
    } else if (Dir < 0) {
      mInContent = FALSE;
    }
  } else if (Key.ScanCode == SCAN_ESC) {
    mInContent = FALSE;
  } else if ((Key.UnicodeChar == L'+') || (Key.UnicodeChar == L'-')) {
    if (It->Type == ItBoot) {
      I = It->Action - ActBoot;
      if ((Key.UnicodeChar == L'-') && (I + 1 < mBootCount)) {
        Tmp = mBoot[I]; mBoot[I] = mBoot[I + 1]; mBoot[I + 1] = Tmp;
        mItem++;
      } else if ((Key.UnicodeChar == L'+') && (I > 0)) {
        Tmp = mBoot[I]; mBoot[I] = mBoot[I - 1]; mBoot[I - 1] = Tmp;
        mItem--;
      }

      BuildPage ();
    } else if (It->Type == ItNumber) {
      *It->Number += (Key.UnicodeChar == L'+') ? 1 : -1;
      *It->Number  = MAX (It->Min, MIN (It->Max, *It->Number));
    }
  } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
    switch (It->Type) {
      case ItToggle:
        *It->Choice = !*It->Choice;
        break;
      case ItChoice:
        if (Dropdown (It) && (It->Choice == &mLang)) {
          gLanguage = mLang;
          BuildPage ();
        }

        break;
      case ItAction:
      case ItBoot:
        return DoAction (It->Action);
      default:
        break;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
RunTextFallback (
  VOID
  )
{
  return StartFvApp (&mUiAppGuid);
}

// ---------------------------------------------------------- boot menu ---
//
// ESC during boot starts this application with the load option data
// "bootmenu": a one-time boot device picker instead of the full setup.
//
STATIC BOOLEAN  mBootMenu;
STATIC BOOLEAN  mReadyLogged;
STATIC UINTN    mBmSel;

#define BM_W      600
#define BM_ROW_H  52

STATIC
UINTN
BootMenuCount (
  VOID
  )
{
  return mBootCount + 2;          // + "Setup" + "UEFI Shell"
}

STATIC
VOID
DrawBootMenu (
  VOID
  )
{
  INT32         X, Y, H, I, Rows, Kx;
  BOOLEAN       Sel;
  CONST CHAR16  *Label;
  CHAR16        Num[8];

  GfxFill (0, 0, gCanvas.W, gCanvas.H, COL_SIDEBAR);
  DrawLogo (40, 32);

  Rows = (INT32)BootMenuCount ();
  H    = 112 + (INT32)mBootCount * (BM_ROW_H + 6) + 28 + 2 * (BM_ROW_H + 6) + 18;
  if (mBootCount == 0) {
    H += BM_ROW_H + 6;
  }

  X = (gCanvas.W - BM_W) / 2;
  Y = MAX (120, (gCanvas.H - H) / 2 + 20);
  GfxShadow (X, Y, BM_W, H, 16);
  GfxRoundRect (X, Y, BM_W, H, 16, COL_CARD);
  GfxText (gFontTitle, X + 32, Y + 26, S (StrBootMenu), COL_TEXT);
  GfxText (gFontBody, X + 32, Y + 68, S (StrBootMenuSub), COL_TEXT_2);

  Y += 112;
  if (mBootCount == 0) {
    GfxText (gFontBody, X + 32, Y + 14, S (StrNoStorage), COL_TEXT_2);
    Y += BM_ROW_H + 6;
  }

  for (I = 0; I < Rows; I++) {
    if (I == (INT32)mBootCount) {
      GfxFill (X + 24, Y + 10, BM_W - 48, 1, COL_LINE);
      Y += 28;
    }

    Sel   = (I == (INT32)mBmSel);
    Label = (I < (INT32)mBootCount) ? mBoot[I].Desc : (I == (INT32)mBootCount) ? S (StrEnterSetup) : S (StrShell);
    if (Sel) {
      GfxRoundRect (X + 16, Y, BM_W - 32, BM_ROW_H, 10, COL_ACCENT_SOFT);
      GfxRoundRect (X + 16, Y + 13, 4, BM_ROW_H - 26, 2, COL_ACCENT);
    }

    if (I < (INT32)mBootCount) {
      GfxCircle (X + 48, Y + BM_ROW_H / 2, 13, Sel ? COL_ACCENT : COL_LINE);
      UnicodeSPrint (Num, sizeof (Num), L"%d", I + 1);
      GfxText (
        gFontSmall,
        X + 48 - GfxTextWidth (gFontSmall, Num) / 2,
        Y + (BM_ROW_H - gFontSmall->LineHeight) / 2 + 1,
        Num,
        Sel ? COL_WHITE : COL_TEXT_2
        );
    } else {
      GfxAlpha ((I == (INT32)mBootCount) ? gIconSliders : gIconStartup, X + 37, Y + (BM_ROW_H - 22) / 2, Sel ? COL_ACCENT : COL_TEXT_2);
    }

    GfxText (gFontMedium, X + 76, Y + (BM_ROW_H - gFontMedium->LineHeight) / 2 + 1, Label, COL_TEXT);
    if (Sel) {
      GfxAlpha (gIconChevron, X + BM_W - 52, Y + (BM_ROW_H - 22) / 2, COL_ACCENT);
    }

    Y += BM_ROW_H + 6;
  }

  // key hints
  Kx = X + 8;
  DrawKeycap (&Kx, gCanvas.H - 44, NULL, TRUE, S (StrKeyMove));
  DrawKeycap (&Kx, gCanvas.H - 44, L"Enter", FALSE, S (StrBootNow));
  DrawKeycap (&Kx, gCanvas.H - 44, L"Esc", FALSE, S (StrContinueBoot));
}

STATIC
BOOLEAN
IsBootMenuRequest (
  VOID
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Li;

  if (EFI_ERROR (gBS->HandleProtocol (mImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) ||
      (Li->LoadOptions == NULL) || (Li->LoadOptionsSize < sizeof (L"bootmenu") - sizeof (CHAR16)))
  {
    return FALSE;
  }

  return CompareMem (Li->LoadOptions, L"bootmenu", sizeof (L"bootmenu") - sizeof (CHAR16)) == 0;
}

//
// Returns TRUE to open the full setup, FALSE to continue booting.
//
STATIC
BOOLEAN
RunBootMenu (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  BOOLEAN        Tick;

  mBootMenu       = TRUE;
  mBootMenuActive = TRUE;
  DrawBootMenuHook = DrawBootMenu;
  mBmSel          = 0;
  DEBUG ((DEBUG_ERROR, "OpiSetup: boot menu\n"));
  for ( ; ; ) {
    Render ();
    Present ();
    if (!mReadyLogged) {
      DEBUG ((DEBUG_ERROR, "OpiSetup: ready\n"));
      mReadyLogged = TRUE;
    }
    Key = WaitKey (&Tick);
    if (Tick) {
      continue;
    }

    if ((Key.ScanCode == SCAN_UP) && (mBmSel > 0)) {
      mBmSel--;
    } else if ((Key.ScanCode == SCAN_DOWN) && (mBmSel + 1 < BootMenuCount ())) {
      mBmSel++;
    } else if (Key.ScanCode == SCAN_ESC) {
      mBootMenu = mBootMenuActive = FALSE;
      return FALSE;
    } else if ((Key.ScanCode == SCAN_F2) || ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) && (mBmSel == mBootCount))) {
      mBootMenu = mBootMenuActive = FALSE;
      return TRUE;
    } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      if (mBmSel < mBootCount) {
        BootOption (mBoot[mBmSel].Number);
      } else {
        StartFvApp (&mShellGuid);
      }
    }
  }
}


EFI_STATUS
EFIAPI
OpiSetupMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS     Status;
  EFI_INPUT_KEY  Key;
  BOOLEAN        Tick;
  BOOLEAN        Quit;

  mImageHandle = ImageHandle;

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&mGop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  }

  if (EFI_ERROR (Status)) {
    return RunTextFallback ();
  }

  gCanvas.W   = (INT32)mGop->Mode->Info->HorizontalResolution;
  gCanvas.H   = (INT32)mGop->Mode->Info->VerticalResolution;
  gCanvas.Buf = AllocatePool ((UINTN)gCanvas.W * gCanvas.H * 4);
  if (gCanvas.Buf == NULL) {
    return RunTextFallback ();
  }

  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &mTick);
  gBS->SetTimer (mTick, TimerPeriodic, 10000000);     // 1 s, for the clock

  SysInfoCollect (&mInfo);
  LoadSettings ();

  if (IsBootMenuRequest () && !RunBootMenu ()) {
    gBS->CloseEvent (mTick);
    FreePool (gCanvas.Buf);
    gST->ConOut->ClearScreen (gST->ConOut);
    return EFI_SUCCESS;
  }

  mPage      = PAGE_MAIN;
  mInContent = FALSE;
  BuildPage ();
  FirstFocusable ();

  Quit = FALSE;
  while (!Quit) {
    Render ();
    Present ();
    if (!mReadyLogged) {
      DEBUG ((DEBUG_ERROR, "OpiSetup: ready\n"));
      mReadyLogged = TRUE;
    }

    Key = WaitKey (&Tick);
    if (Tick) {
      continue;
    }

    if (Key.ScanCode == SCAN_F10) {
      Quit = DoAction (ActSaveExit);
      continue;
    }

    if (mInContent) {
      Quit = HandleContentKey (Key);
      continue;
    }

    // sidebar
    if ((Key.ScanCode == SCAN_UP) && (mPage > 0)) {
      mPage--;
    } else if ((Key.ScanCode == SCAN_DOWN) && (mPage + 1 < ARRAY_SIZE (mPages))) {
      mPage++;
    } else if ((Key.ScanCode == SCAN_RIGHT) || (Key.UnicodeChar == CHAR_CARRIAGE_RETURN)) {
      if (PageHasFocusable ()) {
        mInContent = TRUE;
      }

      continue;
    } else if (Key.ScanCode == SCAN_ESC) {
      Quit = DoAction (ActDiscardExit);
      continue;
    } else {
      continue;
    }

    BuildPage ();
    FirstFocusable ();
    mScroll = 0;
  }

  gBS->CloseEvent (mTick);
  FreePool (gCanvas.Buf);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, TRUE);
  return EFI_SUCCESS;
}
