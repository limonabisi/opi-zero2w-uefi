/** @file
  Tiny software renderer for OpiSetup: rectangles, anti-aliased rounded
  rectangles and circles, 4-bit alpha glyphs/icons.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "OpiSetup.h"
#include "Assets.h"

CANVAS  gCanvas;

CONST FONT  *gFontSmall  = &mFontSmall;
CONST FONT  *gFontBody   = &mFontBody;
CONST FONT  *gFontMedium = &mFontMedium;
CONST FONT  *gFontNav    = &mFontNav;
CONST FONT  *gFontTitle  = &mFontTitle;
CONST FONT  *gFontLogo   = &mFontLogo;
CONST FONT  *gFontChip   = &mFontChip;

CONST ALPHA_BITMAP  *gIconHome    = &mIconHome;
CONST ALPHA_BITMAP  *gIconSliders = &mIconSliders;
CONST ALPHA_BITMAP  *gIconClock   = &mIconClock;
CONST ALPHA_BITMAP  *gIconShield  = &mIconShield;
CONST ALPHA_BITMAP  *gIconStartup = &mIconStartup;
CONST ALPHA_BITMAP  *gIconPower   = &mIconPower;
CONST ALPHA_BITMAP  *gIconChevron = &mIconChevron;
CONST ALPHA_BITMAP  *gIconBack    = &mIconBack;
CONST ALPHA_BITMAP  *gIconCheck   = &mIconCheck;
CONST ALPHA_BITMAP  *gIconChip    = &mIconChip;

STATIC
inline
VOID
Plot (
  INT32   X,
  INT32   Y,
  COLOR   C,
  UINT32  A      // 0..255
  )
{
  UINT32  *P;
  UINT32  D;
  UINT32  R, G, B;

  if ((X < 0) || (Y < 0) || (X >= gCanvas.W) || (Y >= gCanvas.H) || (A == 0)) {
    return;
  }

  P = &gCanvas.Buf[Y * gCanvas.W + X];
  if (A >= 255) {
    *P = C;
    return;
  }

  D  = *P;
  R  = (((C >> 16) & 0xFF) * A + ((D >> 16) & 0xFF) * (255 - A)) / 255;
  G  = (((C >> 8) & 0xFF) * A + ((D >> 8) & 0xFF) * (255 - A)) / 255;
  B  = ((C & 0xFF) * A + (D & 0xFF) * (255 - A)) / 255;
  *P = (R << 16) | (G << 8) | B;
}

VOID
GfxFill (
  INT32  X,
  INT32  Y,
  INT32  W,
  INT32  H,
  COLOR  C
  )
{
  INT32  Yy;
  INT32  Xx;
  INT32  X1;
  INT32  Y1;

  X1 = MIN (X + W, gCanvas.W);
  Y1 = MIN (Y + H, gCanvas.H);
  X  = MAX (X, 0);
  Y  = MAX (Y, 0);
  for (Yy = Y; Yy < Y1; Yy++) {
    UINT32  *Row = &gCanvas.Buf[Yy * gCanvas.W];
    for (Xx = X; Xx < X1; Xx++) {
      Row[Xx] = C;
    }
  }
}

VOID
GfxBlend (
  INT32   X,
  INT32   Y,
  INT32   W,
  INT32   H,
  COLOR   C,
  UINT32  Alpha
  )
{
  INT32  Yy;
  INT32  Xx;

  for (Yy = Y; Yy < Y + H; Yy++) {
    for (Xx = X; Xx < X + W; Xx++) {
      Plot (Xx, Yy, C, Alpha);
    }
  }
}

//
// Coverage (0..255) of pixel (Px, Py) by a circle of radius R pixels
// centred on the pixel corner (Cx, Cy); 4x4 supersampled, 1/8 px units.
//
STATIC
UINT32
Coverage (
  INT32  Px,
  INT32  Py,
  INT32  Cx,
  INT32  Cy,
  INT32  R
  )
{
  INT32   Sx;
  INT32   Sy;
  UINT32  Hits;
  INT32   Dx;
  INT32   Dy;
  INT32   R8;

  R8   = R * 8;
  Hits = 0;
  for (Sy = 0; Sy < 4; Sy++) {
    Dy = Py * 8 + Sy * 2 + 1 - Cy * 8;
    for (Sx = 0; Sx < 4; Sx++) {
      Dx = Px * 8 + Sx * 2 + 1 - Cx * 8;
      if ((Dx * Dx + Dy * Dy) <= R8 * R8) {
        Hits++;
      }
    }
  }

  return (Hits * 255) / 16;
}

VOID
GfxRoundRect (
  INT32  X,
  INT32  Y,
  INT32  W,
  INT32  H,
  INT32  R,
  COLOR  C
  )
{
  INT32  Yy;
  INT32  Xx;

  if (R * 2 > H) {
    R = H / 2;
  }

  if (R * 2 > W) {
    R = W / 2;
  }

  if (R <= 0) {
    GfxFill (X, Y, W, H, C);
    return;
  }

  GfxFill (X + R, Y, W - 2 * R, H, C);
  GfxFill (X, Y + R, R, H - 2 * R, C);
  GfxFill (X + W - R, Y + R, R, H - 2 * R, C);

  for (Yy = 0; Yy < R; Yy++) {
    for (Xx = 0; Xx < R; Xx++) {
      Plot (X + Xx, Y + Yy, C, Coverage (Xx, Yy, R, R, R));
      Plot (X + W - R + Xx, Y + Yy, C, Coverage (Xx, Yy, 0, R, R));
      Plot (X + Xx, Y + H - R + Yy, C, Coverage (Xx, Yy, R, 0, R));
      Plot (X + W - R + Xx, Y + H - R + Yy, C, Coverage (Xx, Yy, 0, 0, R));
    }
  }
}

VOID
GfxShadow (
  INT32  X,
  INT32  Y,
  INT32  W,
  INT32  H,
  INT32  R
  )
{
  INT32  I;

  for (I = 6; I >= 1; I--) {
    // soft layered shadow, offset down a little
    INT32  A = 7 - I;
    INT32  Xs = X - I;
    INT32  Ys = Y - I + 3;
    INT32  Yy, Xx;
    for (Yy = Ys; Yy < Ys + H + 2 * I; Yy++) {
      for (Xx = Xs; Xx < Xs + W + 2 * I; Xx++) {
        if ((Yy > Y + R) && (Yy < Y + H - R) && (Xx > X + R) && (Xx < X + W - R)) {
          continue;     // hidden under the card anyway
        }

        Plot (Xx, Yy, 0x000000, (UINT32)A * 3);
      }
    }
  }
}

VOID
GfxCircle (
  INT32  Cx,
  INT32  Cy,
  INT32  R,
  COLOR  C
  )
{
  INT32  Yy;
  INT32  Xx;

  for (Yy = -R - 1; Yy <= R; Yy++) {
    for (Xx = -R - 1; Xx <= R; Xx++) {
      Plot (Cx + Xx, Cy + Yy, C, Coverage (Xx, Yy, 0, 0, R));
    }
  }
}

VOID
GfxAlpha (
  CONST ALPHA_BITMAP  *Bm,
  INT32               X,
  INT32               Y,
  COLOR               C
  )
{
  INT32   Yy;
  INT32   Xx;
  UINTN   RowBytes;
  UINT8   Byte;
  UINT32  A;

  RowBytes = (Bm->W + 1) / 2;
  for (Yy = 0; Yy < Bm->H; Yy++) {
    for (Xx = 0; Xx < Bm->W; Xx++) {
      Byte = Bm->Data[Yy * RowBytes + Xx / 2];
      A    = ((Xx & 1) == 0) ? (Byte >> 4) : (Byte & 0xF);
      Plot (X + Xx, Y + Yy, C, A * 17);
    }
  }
}

STATIC
CONST GLYPH *
FindGlyph (
  CONST FONT  *F,
  CHAR16      Ch
  )
{
  UINT32  I;

  if ((Ch >= 32) && (Ch < 127)) {
    return &F->Glyphs[Ch - 32];
  }

  for (I = 95; I < F->Count; I++) {
    if (F->Glyphs[I].Code == Ch) {
      return &F->Glyphs[I];
    }
  }

  return &F->Glyphs['?' - 32];
}

INT32
GfxTextWidth (
  CONST FONT    *F,
  CONST CHAR16  *S
  )
{
  INT32  W;

  W = 0;
  while (*S != 0) {
    W += FindGlyph (F, *S++)->Advance;
  }

  return W;
}

//
// Y is the top of the text line.
//
INT32
GfxText (
  CONST FONT    *F,
  INT32         X,
  INT32         Y,
  CONST CHAR16  *S,
  COLOR         C
  )
{
  CONST GLYPH  *G;
  INT32        X0;
  INT32        Yy;
  INT32        Xx;
  UINTN        RowBytes;
  UINT8        Byte;
  UINT32       A;

  X0 = X;
  while (*S != 0) {
    G        = FindGlyph (F, *S++);
    RowBytes = (G->W + 1) / 2;
    for (Yy = 0; Yy < G->H; Yy++) {
      for (Xx = 0; Xx < G->W; Xx++) {
        Byte = F->Data[G->Offset + Yy * RowBytes + Xx / 2];
        A    = ((Xx & 1) == 0) ? (Byte >> 4) : (Byte & 0xF);
        Plot (X + G->X + Xx, Y + G->Y + Yy, C, A * 17);
      }
    }

    X += G->Advance;
  }

  return X - X0;
}

VOID
GfxTriangle (
  INT32    X,
  INT32    Y,
  INT32    Size,
  BOOLEAN  Up,
  COLOR    C
  )
{
  INT32  I;

  for (I = 0; I < Size; I++) {
    INT32  Row = Up ? (Y + I) : (Y + Size - 1 - I);
    GfxFill (X + Size - 1 - I, Row, 2 * I + 1, 1, C);
  }
}
