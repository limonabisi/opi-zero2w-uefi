#!/usr/bin/env python3
"""Generate OpiSetup font atlases, icons and logos as a C header.

Fonts: Poppins (SIL Open Font License 1.1), rendered with anti-aliasing
into 4-bit alpha glyph bitmaps. Icons and logo are drawn here (original
artwork), 4x supersampled.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONTDIR = '/usr/share/fonts/truetype/google-fonts/'
CHARS = [chr(c) for c in range(32, 127)] + list('çğıöşüÇĞİÖŞÜ°·…–')

FONTS = [  # (C name, file, pixel size)
    ('FontSmall', 'Poppins-Regular.ttf', 14),
    ('FontBody', 'Poppins-Regular.ttf', 17),
    ('FontMedium', 'Poppins-Medium.ttf', 17),
    ('FontNav', 'Poppins-Medium.ttf', 18),
    ('FontTitle', 'Poppins-Bold.ttf', 30),
    ('FontLogo', 'Poppins-Bold.ttf', 26),
    ('FontChip', 'Poppins-Bold.ttf', 16),
]

out = []
def emit(s): out.append(s)

def pack4(alpha_rows):
    """4-bit alpha, two pixels per byte (high nibble first)."""
    data = []
    for row in alpha_rows:
        for i in range(0, len(row), 2):
            a = row[i] >> 4
            b = (row[i + 1] >> 4) if i + 1 < len(row) else 0
            data.append((a << 4) | b)
    return data

def font(cname, fname, size):
    f = ImageFont.truetype(FONTDIR + fname, size)
    ascent, descent = f.getmetrics()
    glyphs = []
    blob = []
    for ch in CHARS:
        bbox = f.getbbox(ch)  # (x0,y0,x1,y1) relative to origin at ascender top
        adv = round(f.getlength(ch))
        if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glyphs.append((ord(ch), 0, 0, 0, 0, adv, len(blob)))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        img = Image.new('L', (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=f, fill=255)
        rows = [[img.getpixel((x, y)) for x in range(w)] for y in range(h)]
        off = len(blob)
        blob += pack4(rows)
        glyphs.append((ord(ch), w, h, x0, y0, adv, off))
    emit(f'STATIC CONST UINT8 m{cname}Data[] = {{')
    for i in range(0, len(blob), 24):
        emit('  ' + ', '.join('0x%02x' % b for b in blob[i:i + 24]) + ',')
    emit('};')
    emit(f'STATIC CONST GLYPH m{cname}Glyphs[] = {{')
    for g in glyphs:
        emit('  { 0x%04x, %d, %d, %d, %d, %d, %d },' % g)
    emit('};')
    emit(f'STATIC CONST FONT m{cname} = {{ {ascent + descent}, {ascent}, {len(glyphs)}, m{cname}Glyphs, m{cname}Data }};')
    return len(blob)

def bitmap(cname, img):
    """8-bit alpha bitmap -> 4 bit."""
    w, h = img.size
    rows = [[img.getpixel((x, y)) for x in range(w)] for y in range(h)]
    blob = pack4(rows)
    emit(f'STATIC CONST UINT8 m{cname}Data[] = {{')
    for i in range(0, len(blob), 24):
        emit('  ' + ', '.join('0x%02x' % b for b in blob[i:i + 24]) + ',')
    emit('};')
    emit(f'STATIC CONST ALPHA_BITMAP m{cname} = {{ {w}, {h}, m{cname}Data }};')
    return len(blob)

S = 4  # supersampling
def canvas(sz):
    img = Image.new('L', (sz * S, sz * S), 0)
    return img, ImageDraw.Draw(img)

def down(img, sz):
    return img.resize((sz, sz), Image.LANCZOS)

def icon_home(sz):
    img, d = canvas(sz); k = S; w = 2 * k
    d.line([(3*k, 11*k), (11*k, 3.5*k), (19*k, 11*k)], fill=255, width=w, joint='curve')
    d.line([(5.5*k, 9.5*k), (5.5*k, 18.5*k), (16.5*k, 18.5*k), (16.5*k, 9.5*k)], fill=255, width=w, joint='curve')
    d.rectangle([(9.5*k, 13*k), (12.5*k, 18.5*k)], fill=255)
    return down(img, sz)

def icon_sliders(sz):
    img, d = canvas(sz); k = S; w = 2 * k
    for x, y in ((6, 7), (15, 14), (9, 17)):
        pass
    for row, knob in ((5, 7), (11, 15), (17, 9)):
        d.line([(3*k, row*k), (19*k, row*k)], fill=255, width=w)
        d.ellipse([((knob-2.6)*k, (row-2.6)*k), ((knob+2.6)*k, (row+2.6)*k)], fill=255)
        d.ellipse([((knob-1.2)*k, (row-1.2)*k), ((knob+1.2)*k, (row+1.2)*k)], fill=0)
    return down(img, sz)

def icon_clock(sz):
    img, d = canvas(sz); k = S; w = 2 * k
    d.ellipse([(3*k, 3*k), (19*k, 19*k)], outline=255, width=w)
    d.line([(11*k, 6.5*k), (11*k, 11*k), (14.5*k, 13*k)], fill=255, width=w, joint='curve')
    return down(img, sz)

def icon_shield(sz):
    img, d = canvas(sz); k = S; w = 2 * k
    pts = [(11*k, 2.5*k), (18*k, 5.5*k), (17.5*k, 11*k), (15*k, 16*k), (11*k, 19.5*k), (7*k, 16*k), (4.5*k, 11*k), (4*k, 5.5*k), (11*k, 2.5*k)]
    d.line(pts, fill=255, width=w, joint='curve')
    d.line([(7.5*k, 11*k), (10*k, 13.5*k), (14.5*k, 8.5*k)], fill=255, width=w, joint='curve')
    return down(img, sz)

def icon_rocket(sz):   # "startup": play-arrow in a rounded square
    img, d = canvas(sz); k = S; w = 2 * k
    d.rounded_rectangle([(3*k, 3*k), (19*k, 19*k)], radius=4*k, outline=255, width=w)
    d.polygon([(9*k, 7*k), (15.5*k, 11*k), (9*k, 15*k)], fill=255)
    return down(img, sz)

def icon_power(sz):
    img, d = canvas(sz); k = S; w = 2 * k
    d.arc([(3.5*k, 4*k), (18.5*k, 19*k)], start=-60, end=240, fill=255, width=w)
    d.line([(11*k, 2.5*k), (11*k, 10.5*k)], fill=255, width=w)
    return down(img, sz)

def icon_chevron(sz):
    img, d = canvas(sz); k = S; w = 2 * k
    d.line([(8*k, 5*k), (14*k, 11*k), (8*k, 17*k)], fill=255, width=w, joint='curve')
    return down(img, sz)

def icon_back(sz):
    img, d = canvas(sz); k = S; w = 2 * k
    d.line([(4*k, 11*k), (18*k, 11*k)], fill=255, width=w)
    d.line([(10*k, 5*k), (4*k, 11*k), (10*k, 17*k)], fill=255, width=w, joint='curve')
    return down(img, sz)

def icon_check(sz):
    img, d = canvas(sz); k = S; w = int(2.4 * k)
    d.line([(5*k, 11.5*k), (9.5*k, 16*k), (17.5*k, 6.5*k)], fill=255, width=w, joint='curve')
    return down(img, sz)

def icon_chip(sz):
    """Small SoC package, used by the H618 badge."""
    img = Image.new('L', (sz * S, sz * S), 0); d = ImageDraw.Draw(img); k = S * sz / 22
    d.rounded_rectangle([(5*k, 5*k), (17*k, 17*k)], radius=2*k, outline=255, width=int(1.8*k))
    for i in (8, 11, 14):
        for a, b in (((i*k, 1.5*k), (i*k, 5*k)), ((i*k, 17*k), (i*k, 20.5*k)), ((1.5*k, i*k), (5*k, i*k)), ((17*k, i*k), (20.5*k, i*k))):
            d.line([a, b], fill=255, width=int(1.6*k))
    d.rectangle([(8.5*k, 8.5*k), (13.5*k, 13.5*k)], fill=255)
    return img.resize((sz, sz), Image.LANCZOS)

emit('// Generated by tools/gen_assets.py - do not edit.')
emit('// Fonts: Poppins, Copyright 2020 The Poppins Project Authors, SIL Open Font License 1.1.')
total = 0
for f in FONTS:
    total += font(*f)
for name, fn, sz in (('IconHome', icon_home, 22), ('IconSliders', icon_sliders, 22), ('IconClock', icon_clock, 22),
                     ('IconShield', icon_shield, 22), ('IconStartup', icon_rocket, 22), ('IconPower', icon_power, 22),
                     ('IconChevron', icon_chevron, 22), ('IconBack', icon_back, 22), ('IconCheck', icon_check, 22),
                     ('IconChip', icon_chip, 34)):
    total += bitmap(name, fn(sz))
open(sys.argv[1], 'w').write('\n'.join(out) + '\n')
print('asset bytes:', total, file=sys.stderr)
