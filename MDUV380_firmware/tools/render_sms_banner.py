#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Піксельний рендер повноекранного банера вхідного SMS (uiNotification.c).

Нічого не вигадує: гліфи -- зі справжнього UA-набору (HX8353E_charset_UA.h), кольори --
з таблиці themeDefaults у HX8353E_display.c, а логіка переносу по словах ПОВТОРЮЄ
displaySmsFullScreen() рядок у рядок. Тобто якщо тут текст обрізало чи виліз за край --
так само буде й на рації.

    python3 render_sms_banner.py            # -> sms_banner_preview.png
"""
import os, re
from PIL import Image

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 160, 128

# ---------- теми з реального коду -------------------------------------------
src = open(f'{FW}/application/source/hardware/HX8353E_display.c', encoding='utf-8').read()
tbl = src[src.index('static const uint32_t themeDefaults'):]
tbl = tbl[:tbl.index('\n\t};')]
themes = {}
for day in ('DAY', 'NIGHT'):
    chunk = tbl[tbl.index(f'[{day}] ='):]
    end = chunk.find('[NIGHT] =', 1)
    if end > 0:
        chunk = chunk[:end]
    themes[day] = {m.group(1): int(m.group(2), 16)
                   for m in re.finditer(r'\[(THEME_ITEM_\w+)\]\s*=\s*0x([0-9A-Fa-f]+)U', chunk)}

def rgb565(c):
    r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
    r, g, b = r & 0xF8, g & 0xFC, b & 0xF8
    return (r | r >> 5, g | g >> 6, b | b >> 5)

# ---------- шрифти ----------------------------------------------------------
fsrc = open(f'{FW}/application/include/hardware/HX8353E_charset_UA.h', encoding='latin-1').read()
def load_font(name):
    m = re.search(name + r'\s*\[\s*\]\s*=\s*\{(.*?)\};', fsrc, re.S)
    body = re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S))
    v = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body)]
    return dict(vals=v, start=v[2], end=v[3], w=v[4], h=v[5], bpc=v[7])

F2 = load_font('font_8x8')     # FONT_SIZE_2 -- рядок «ще N непрочит.»
F3 = load_font('font_8x16')    # FONT_SIZE_3 -- текст повідомлення (великий)

class FB:
    def __init__(self, bg):
        self.px = [[bg] * W for _ in range(H)]
    def fill(self, x, y, w, h, c):
        for yy in range(y, min(y + h, H)):
            for xx in range(max(0, x), min(x + w, W)):
                if yy >= 0:
                    self.px[yy][xx] = c
    def rect(self, x, y, w, h, c):
        self.fill(x, y, w, 1, c); self.fill(x, y + h - 1, w, 1, c)
        self.fill(x, y, 1, h, c); self.fill(x + w - 1, y, 1, h, c)
    def text(self, x, y, s, c, font, align='left'):
        data = s.encode('cp1251')
        if align == 'center':
            x = (W - font['w'] * len(data)) // 2
        for i, code in enumerate(data):
            if code == 0x20 or not (font['start'] <= code <= font['end']):
                continue
            off = 8 + (code - font['start']) * font['bpc']
            cols = font['vals'][off:off + font['bpc']]
            for cx in range(font['w']):
                for cy in range(font['h']):
                    idx = cx + (cy // 8) * font['w']
                    if idx >= len(cols):
                        continue
                    if (cols[idx] >> (cy % 8)) & 1:
                        px, py = x + i * font['w'] + cx, y + cy
                        if 0 <= px < W and 0 <= py < H:
                            self.px[py][px] = c

# ---------- дзеркало displaySmsFullScreen() ---------------------------------
def render(theme, msg, more):
    T = lambda k: rgb565(themes[theme][k])
    charW, lineH, cntH = 8, F3['h'], F2['h']
    perLine = (W - 6) // charW
    reserved = (cntH + 2) if more > 0 else 0
    maxLines = max(1, (H - 4 - reserved) // lineH)

    fb = FB(T('THEME_ITEM_BG_NOTIFICATION'))
    fb.rect(0, 0, W, H, T('THEME_ITEM_FG_DECORATION'))

    p, y, used = 0, 2, 0
    fg = T('THEME_ITEM_FG_NOTIFICATION')
    while p < len(msg) and used < maxLines:
        while p < len(msg) and msg[p] == ' ':
            p += 1
        if p >= len(msg):
            break
        n, last = 0, -1
        while p + n < len(msg) and msg[p + n] != '\n' and n < perLine:
            if msg[p + n] == ' ':
                last = n
            n += 1
        if p + n < len(msg) and msg[p + n] != '\n' and last > 0:
            n = last
        line = msg[p:p + n]
        p += n
        if p < len(msg) and msg[p] == '\n':
            p += 1
        used += 1
        if used == maxLines and p < len(msg) and n > 0:
            cut = n if n <= (perLine - 3) else (perLine - 3)
            line = line[:cut] + '...'
        fb.text(3, y, line, fg, F3)
        y += lineH

    if more > 0:
        fb.text(0, H - cntH - 2, "ще %d непрочит." % more,
                T('THEME_ITEM_FG_WARNING_NOTIFICATION'), F2, 'center')
    return fb

def to_img(fb, scale=3):
    im = Image.new('RGB', (W, H))
    im.putdata([c for row in fb.px for c in row])
    return im.resize((W * scale, H * scale), Image.NEAREST)

CASES = [
    ("SMS: Hello", 12),
    ("SMS: Виходимо на позицію через десять хвилин, підтвердьте готовність", 3),
    ("SMS: " + "Довге повідомлення на сто сорок чотири символи, щоб перевірити "
     "обрізання тексту й трикрапку в кінці останнього рядка банера", 0),
]

cols, rows = len(CASES), 2
pad, scale = 10, 3
sheet = Image.new('RGB', (cols * (W * scale + pad) + pad,
                          rows * (H * scale + pad) + pad), (24, 24, 28))
for r, theme in enumerate(('DAY', 'NIGHT')):
    for c, (msg, more) in enumerate(CASES):
        sheet.paste(to_img(render(theme, msg, more), scale),
                    (pad + c * (W * scale + pad), pad + r * (H * scale + pad)))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sms_banner_preview.png')
sheet.save(out)
print('рендер збережено:', out)
print('верхній ряд -- ДЕННА тема, нижній -- НІЧНА; колонки: короткe / середнє / довге (обрізане)')
