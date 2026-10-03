/*
 * Unpack the initrd before the x86 kernel sees it.
 *
 * A Linux initrd is a chain of cpio archives, each plain or compressed.
 * The kernel would decompress it itself - as translated x86 code, which
 * takes the better part of a minute for a distribution's 50-130 MB image.
 * Doing it here, natively, takes a few seconds; the kernel is then handed
 * the same archives uncompressed, which it accepts just as well.
 *
 * gzip (deflate) and LZ4 (the legacy frame format that the kernel uses) are
 * decoded; anything else, or anything that does not check out (CRC, sizes),
 * makes the caller keep the initrd exactly as it was.
 */
#include <string.h>
#include "x64e.h"

/* ------------------------------------------------------------ inflate */
typedef struct {
  const uint8_t *in, *in_end;
  uint64_t       bits;
  unsigned       nbits;
  uint8_t       *out, *out_start, *out_end;
} inf_t;

#define FAST_BITS  10

typedef struct {
  uint16_t fast[1 << FAST_BITS];        /* (symbol << 4) | length, 0 = longer code */
  uint16_t count[16];
  uint16_t symbol[288];
} huff_t;

static void
refill (inf_t *s)
{
  while (s->nbits <= 56 && s->in < s->in_end) {
    s->bits  |= (uint64_t)*s->in++ << s->nbits;
    s->nbits += 8;
  }
}

static uint32_t
getbits (inf_t *s, unsigned n)
{
  uint32_t v;

  if (s->nbits < n) {
    refill (s);
  }

  v         = (uint32_t)(s->bits & ((1ULL << n) - 1));
  s->bits >>= n;
  s->nbits  = s->nbits >= n ? s->nbits - n : 0;
  return v;
}

static int
huff_build (huff_t *h, const uint8_t *len, int n)
{
  uint16_t offs[16], code = 0, next[16];
  int      i, left = 1;

  memset (h->count, 0, sizeof (h->count));
  memset (h->fast, 0, sizeof (h->fast));
  for (i = 0; i < n; i++) {
    h->count[len[i]]++;
  }

  h->count[0] = 0;
  for (i = 1; i < 16; i++) {
    left = (left << 1) - h->count[i];
    if (left < 0) {
      return -1;                          /* over-subscribed */
    }
  }

  offs[1] = 0;
  for (i = 1; i < 15; i++) {
    offs[i + 1] = offs[i] + h->count[i];
  }

  for (i = 0; i < n; i++) {
    if (len[i]) {
      h->symbol[offs[len[i]]++] = (uint16_t)i;
    }
  }

  /* canonical codes, bit-reversed, into the fast table */
  for (i = 1; i < 16; i++) {
    code    = (uint16_t)((code + h->count[i - 1]) << 1);
    next[i] = code;
  }

  for (i = 0; i < n; i++) {
    unsigned l = len[i], c, r = 0, k;

    if (l == 0 || l > FAST_BITS) {
      continue;
    }

    c = next[l]++;
    for (k = 0; k < l; k++) {
      r |= ((c >> k) & 1) << (l - 1 - k);
    }

    for (k = r; k < (1u << FAST_BITS); k += 1u << l) {
      h->fast[k] = (uint16_t)((i << 4) | l);
    }
  }

  return 0;
}

static int
huff_decode (inf_t *s, const huff_t *h)
{
  unsigned e;
  int      code = 0, first = 0, index = 0, len;

  if (s->nbits < 15) {
    refill (s);
  }

  e = h->fast[s->bits & ((1u << FAST_BITS) - 1)];
  if (e) {
    unsigned l = e & 15;

    if (l > s->nbits) {
      return -1;
    }

    s->bits >>= l;
    s->nbits -= l;
    return (int)(e >> 4);
  }

  /* a code longer than the fast table: bit by bit */
  for (len = 1; len <= 15; len++) {
    int count = h->count[len];

    if (s->nbits == 0) {
      return -1;
    }

    code     |= (int)(s->bits & 1);
    s->bits >>= 1;
    s->nbits--;
    if (code - count < first) {
      return h->symbol[index + (code - first)];
    }

    index  += count;
    first  += count;
    first <<= 1;
    code  <<= 1;
  }

  return -1;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577 };
static const uint8_t  dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                         7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int
inflate_codes (inf_t *s, const huff_t *ll, const huff_t *dd)
{
  for (;;) {
    int sym = huff_decode (s, ll);

    if (sym < 0) {
      return -1;
    }

    if (sym < 256) {
      if (s->out >= s->out_end) {
        return -1;
      }

      *s->out++ = (uint8_t)sym;
    } else if (sym == 256) {
      return 0;
    } else {
      unsigned len, dist;
      uint8_t  *from;

      sym -= 257;
      if (sym >= 29) {
        return -1;
      }

      len = len_base[sym] + getbits (s, len_extra[sym]);
      sym = huff_decode (s, dd);
      if (sym < 0 || sym >= 30) {
        return -1;
      }

      dist = dist_base[sym] + getbits (s, dist_extra[sym]);
      if (dist > (size_t)(s->out - s->out_start) || len > (size_t)(s->out_end - s->out)) {
        return -1;
      }

      from = s->out - dist;
      if (dist >= len) {
        memcpy (s->out, from, len);
        s->out += len;
      } else {
        while (len--) {
          *s->out++ = *from++;
        }
      }
    }
  }
}

/* raw deflate; returns 0 and leaves s->in after the last byte used */
static int
inflate_raw (inf_t *s)
{
  static huff_t  ll, dd, cl;             /* 9 KB: not on the stack */
  static uint8_t lens[320];
  static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
  int            last;

  do {
    unsigned type;

    last = (int)getbits (s, 1);
    type = getbits (s, 2);
    if (type == 0) {
      unsigned len, nlen;

      /* stored: byte aligned, bytes in the bit buffer go back */
      s->bits >>= s->nbits & 7;
      s->nbits -= s->nbits & 7;
      len       = getbits (s, 16);
      nlen      = getbits (s, 16);
      if ((len ^ 0xffff) != nlen) {
        return -1;
      }

      while (s->nbits >= 8 && len > 0) {
        if (s->out >= s->out_end) {
          return -1;
        }

        *s->out++ = (uint8_t)s->bits;
        s->bits >>= 8;
        s->nbits -= 8;
        len--;
      }

      if (len > (size_t)(s->in_end - s->in) || len > (size_t)(s->out_end - s->out)) {
        return -1;
      }

      memcpy (s->out, s->in, len);
      s->out += len;
      s->in  += len;
    } else if (type == 1) {
      int i;

      for (i = 0; i < 144; i++) lens[i] = 8;
      for (; i < 256; i++) lens[i] = 9;
      for (; i < 280; i++) lens[i] = 7;
      for (; i < 288; i++) lens[i] = 8;
      huff_build (&ll, lens, 288);
      for (i = 0; i < 30; i++) lens[i] = 5;
      huff_build (&dd, lens, 30);
      if (inflate_codes (s, &ll, &dd)) {
        return -1;
      }
    } else if (type == 2) {
      unsigned nlen = getbits (s, 5) + 257, ndist = getbits (s, 5) + 1, ncode = getbits (s, 4) + 4, i;

      if (nlen > 286 || ndist > 30) {
        return -1;
      }

      memset (lens, 0, 19);
      for (i = 0; i < ncode; i++) {
        lens[order[i]] = (uint8_t)getbits (s, 3);
      }

      if (huff_build (&cl, lens, 19)) {
        return -1;
      }

      for (i = 0; i < nlen + ndist;) {
        int      sym = huff_decode (s, &cl);
        unsigned rep, val = 0;

        if (sym < 0) {
          return -1;
        }

        if (sym < 16) {
          lens[i++] = (uint8_t)sym;
          continue;
        }

        if (sym == 16) {
          if (i == 0) {
            return -1;
          }

          val = lens[i - 1];
          rep = 3 + getbits (s, 2);
        } else if (sym == 17) {
          rep = 3 + getbits (s, 3);
        } else {
          rep = 11 + getbits (s, 7);
        }

        if (i + rep > nlen + ndist) {
          return -1;
        }

        while (rep--) {
          lens[i++] = (uint8_t)val;
        }
      }

      if (lens[256] == 0 || huff_build (&ll, lens, (int)nlen) || huff_build (&dd, lens + nlen, (int)ndist)) {
        return -1;
      }

      if (inflate_codes (s, &ll, &dd)) {
        return -1;
      }
    } else {
      return -1;
    }
  } while (!last);

  /* whole bytes still in the bit buffer were not used */
  s->in   -= s->nbits / 8;
  s->nbits = 0;
  s->bits  = 0;
  return 0;
}

static uint32_t
crc32 (const uint8_t *p, size_t n)
{
  static uint32_t tab[8][256];
  uint32_t        c = 0xffffffffu;
  unsigned        i, k;

  if (tab[0][1] == 0) {
    for (i = 0; i < 256; i++) {
      uint32_t v = i;

      for (k = 0; k < 8; k++) {
        v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1)));
      }

      tab[0][i] = v;
    }

    for (i = 0; i < 256; i++) {
      for (k = 1; k < 8; k++) {
        tab[k][i] = (tab[k - 1][i] >> 8) ^ tab[0][tab[k - 1][i] & 255];
      }
    }
  }

  while (n >= 8) {
    uint32_t a, b;

    memcpy (&a, p, 4);
    memcpy (&b, p + 4, 4);
    a ^= c;
    c  = tab[7][a & 255] ^ tab[6][(a >> 8) & 255] ^ tab[5][(a >> 16) & 255] ^ tab[4][a >> 24] ^
         tab[3][b & 255] ^ tab[2][(b >> 8) & 255] ^ tab[1][(b >> 16) & 255] ^ tab[0][b >> 24];
    p += 8;
    n -= 8;
  }

  while (n--) {
    c = (c >> 8) ^ tab[0][(c ^ *p++) & 255];
  }

  return ~c;
}

static uint32_t
le32 (const uint8_t *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* one gzip member; returns bytes of input used, 0 on error */
static size_t
gunzip (const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *produced)
{
  inf_t   s;
  size_t  p = 10;
  uint8_t flg;

  if (n < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
    return 0;
  }

  flg = in[3];
  if (flg & 4) {                          /* FEXTRA */
    if (p + 2 > n) {
      return 0;
    }

    p += 2 + (in[p] | (in[p + 1] << 8));
  }

  if (flg & 8) {                          /* FNAME */
    while (p < n && in[p++]) {
    }
  }

  if (flg & 16) {                         /* FCOMMENT */
    while (p < n && in[p++]) {
    }
  }

  if (flg & 2) {                          /* FHCRC */
    p += 2;
  }

  if (p + 8 > n) {
    return 0;
  }

  memset (&s, 0, sizeof (s));
  s.in        = in + p;
  s.in_end    = in + n;
  s.out       = out;
  s.out_start = out;
  s.out_end   = out + cap;
  if (inflate_raw (&s) || (size_t)(s.in_end - s.in) < 8) {
    return 0;
  }

  *produced = (size_t)(s.out - out);
  if (le32 (s.in + 4) != (uint32_t)*produced || le32 (s.in) != crc32 (out, *produced)) {
    return 0;
  }

  return (size_t)(s.in - in) + 8;
}

/* ---------------------------------------------------------------- lz4 */
static long
lz4_block (const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
  const uint8_t *ip = in, *ie = in + n;
  uint8_t       *op = out, *oe = out + cap;

  while (ip < ie) {
    unsigned token = *ip++, len = token >> 4, off;
    uint8_t  *from;

    if (len == 15) {
      unsigned b;

      do {
        if (ip >= ie) {
          return -1;
        }

        b    = *ip++;
        len += b;
      } while (b == 255);
    }

    if (len > (size_t)(ie - ip) || len > (size_t)(oe - op)) {
      return -1;
    }

    memcpy (op, ip, len);
    op += len;
    ip += len;
    if (ip >= ie) {
      break;                              /* the last sequence has no match */
    }

    if (ie - ip < 2) {
      return -1;
    }

    off = ip[0] | (ip[1] << 8);
    ip += 2;
    len = token & 15;
    if (len == 15) {
      unsigned b;

      do {
        if (ip >= ie) {
          return -1;
        }

        b    = *ip++;
        len += b;
      } while (b == 255);
    }

    len += 4;
    if (off == 0 || off > (size_t)(op - out) || len > (size_t)(oe - op)) {
      return -1;
    }

    from = op - off;
    if (off >= len) {
      memcpy (op, from, len);
      op += len;
    } else {
      while (len--) {
        *op++ = *from++;
      }
    }
  }

  return (long)(op - out);
}

#define LZ4_LEGACY_MAGIC      0x184c2102u
#define LZ4_COMPRESSBOUND_8M  ((8u << 20) + (8u << 20) / 255 + 16)

/* a legacy LZ4 frame (lz4 -l): blocks of [size][data] until something else */
static size_t
unlz4 (const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *produced)
{
  size_t p = 4, total = 0;

  if (n < 8 || le32 (in) != LZ4_LEGACY_MAGIC) {
    return 0;
  }

  while (p + 4 <= n) {
    uint32_t bs = le32 (in + p);
    long     r;

    if (bs == LZ4_LEGACY_MAGIC) {         /* frames glued together */
      p += 4;
      continue;
    }

    if (bs == 0 || bs > LZ4_COMPRESSBOUND_8M || bs > n - p - 4) {
      break;                              /* not a block: the frame ended before */
    }

    r = lz4_block (in + p + 4, bs, out + total, cap - total);
    if (r < 0) {
      if (total == 0) {
        return 0;
      }

      break;
    }

    total += (size_t)r;
    p     += 4 + bs;
    if (r < (8 << 20)) {
      break;                              /* a short block is the last one */
    }
  }

  if (total == 0) {
    return 0;
  }

  *produced = total;
  return p;
}

/* --------------------------------------------------------------- cpio */
static unsigned
hex8 (const uint8_t *p)
{
  unsigned v = 0, i;

  for (i = 0; i < 8; i++) {
    unsigned c = p[i];

    v = (v << 4) | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
  }

  return v;
}

/* length of an uncompressed "newc" cpio archive, through its trailer; 0 = broken */
static size_t
cpio_length (const uint8_t *in, size_t n)
{
  size_t p = 0;

  for (;;) {
    size_t namesize, filesize, next;

    if (p + 110 > n || memcmp (in + p, "07070", 5) != 0 || (in[p + 5] != '1' && in[p + 5] != '2')) {
      return 0;
    }

    filesize = hex8 (in + p + 6 + 6 * 8);
    namesize = hex8 (in + p + 6 + 11 * 8);
    next     = ((p + 110 + namesize + 3) & ~(size_t)3) + ((filesize + 3) & ~(size_t)3);
    if (namesize == 0 || p + 110 + namesize > n || next > n + 3) {
      return 0;
    }

    if (namesize == 11 && memcmp (in + p + 110, "TRAILER!!!", 10) == 0) {
      return next > n ? n : next;
    }

    p = next;
  }
}

/*
 * Unpack an initrd of IN_LEN bytes into OUT (CAP bytes; the two must not
 * overlap). Returns the unpacked length, or 0 if the initrd should be used
 * as it is (nothing compressed in it, an unknown format, an error, or no
 * room).
 */
size_t
initrd_unpack (const uint8_t *in, size_t in_len, uint8_t *out, size_t cap)
{
  size_t p = 0, o = 0;
  int    unpacked = 0;

  while (p < in_len) {
    size_t used = 0, got = 0;

    if (in[p] == 0) {                     /* padding between archives */
      p++;
      continue;
    }

    while (o & 3) {                       /* every archive starts 4-byte aligned */
      if (o >= cap) {
        return 0;
      }

      out[o++] = 0;
    }

    if (o >= cap) {
      return 0;
    }

    if (in_len - p >= 6 && memcmp (in + p, "07070", 5) == 0) {
      used = cpio_length (in + p, in_len - p);
      if (used == 0 || used > cap - o) {
        return 0;
      }

      memcpy (out + o, in + p, used);
      got = used;
    } else if (in[p] == 0x1f && in_len - p > 2 && in[p + 1] == 0x8b) {
      used     = gunzip (in + p, in_len - p, out + o, cap - o, &got);
      unpacked = 1;
    } else if (in_len - p > 4 && le32 (in + p) == LZ4_LEGACY_MAGIC) {
      used     = unlz4 (in + p, in_len - p, out + o, cap - o, &got);
      unpacked = 1;
    }

    if (used == 0) {
      return 0;                           /* zstd, xz, ...: the kernel does it */
    }

    host_progress ("Unpacking the initrd", (unsigned)((uint64_t)(p + used) * 100 / in_len));
    p += used;
    o += got;
  }

  if (!unpacked) {
    return 0;
  }

  return o;
}
