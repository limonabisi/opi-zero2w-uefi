/* Host keys -> PC scan code set 1 (US layout) for the i8042 keyboard. */
#include "x64e.h"

/* set-1 make codes for ASCII 0x20..0x7e; bit 7 of shift[] = needs Shift */
static const uint8_t ascii_code[95] = {
  0x39, 0x02, 0x28, 0x04, 0x05, 0x06, 0x08, 0x28, 0x0a, 0x0b, 0x09, 0x0d, 0x33, 0x0c, 0x34, 0x35,  /*  !"#$%&'()*+,-./ */
  0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x27, 0x27, 0x33, 0x0d, 0x34, 0x35,  /* 0-9:;<=>? */
  0x03, 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,  /* @A-O */
  0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x1a, 0x2b, 0x1b, 0x07, 0x0c,  /* P-Z[\]^_ */
  0x29, 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,  /* `a-o */
  0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x1a, 0x2b, 0x1b, 0x29         /* p-z{|}~ */
};

static int
needs_shift (int c)
{
  if (c >= 'A' && c <= 'Z') {
    return 1;
  }

  switch (c) {
    case '!': case '"': case '#': case '$': case '%': case '&': case '(': case ')':
    case '*': case '+': case ':': case '<': case '>': case '?': case '@': case '^':
    case '_': case '{': case '|': case '}': case '~':
      return 1;
    default:
      return 0;
  }
}

static void
tap (machine_t *m, uint8_t code, int ext)
{
  if (ext) {
    i8042_key (m, 0xe0);
  }

  i8042_key (m, code);
  if (ext) {
    i8042_key (m, 0xe0);
  }

  i8042_key (m, code | 0x80);
}

/* one character: printable ASCII, \r / \n (Enter), \t, \b, ESC, Ctrl+letter */
void
kbd_type_char (machine_t *m, int c)
{
  int shift;

  if (c == '\r' || c == '\n') {
    tap (m, 0x1c, 0);
    return;
  }

  if (c == '\t') {
    tap (m, 0x0f, 0);
    return;
  }

  if (c == 8 || c == 127) {
    tap (m, 0x0e, 0);
    return;
  }

  if (c == 27) {
    tap (m, 0x01, 0);
    return;
  }

  if (c >= 1 && c <= 26) {                     /* Ctrl+A .. Ctrl+Z */
    i8042_key (m, 0x1d);
    tap (m, ascii_code['a' + c - 1 - 0x20], 0);
    i8042_key (m, 0x9d);
    return;
  }

  if (c < 0x20 || c > 0x7e) {
    return;
  }

  shift = needs_shift (c);
  if (shift) {
    i8042_key (m, 0x2a);
  }

  tap (m, ascii_code[c - 0x20], 0);
  if (shift) {
    i8042_key (m, 0xaa);
  }
}

/* special keys */
void
kbd_type_key (machine_t *m, int key)
{
  static const uint8_t fkeys[12] = { 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58 };

  switch (key) {
    case KEY_UP:     tap (m, 0x48, 1); break;
    case KEY_DOWN:   tap (m, 0x50, 1); break;
    case KEY_RIGHT:  tap (m, 0x4d, 1); break;
    case KEY_LEFT:   tap (m, 0x4b, 1); break;
    case KEY_HOME:   tap (m, 0x47, 1); break;
    case KEY_END:    tap (m, 0x4f, 1); break;
    case KEY_INSERT: tap (m, 0x52, 1); break;
    case KEY_DELETE: tap (m, 0x53, 1); break;
    case KEY_PGUP:   tap (m, 0x49, 1); break;
    case KEY_PGDN:   tap (m, 0x51, 1); break;
    default:
      if (key >= KEY_F1 && key <= KEY_F1 + 11) {
        tap (m, fkeys[key - KEY_F1], 0);
      }

      break;
  }
}
