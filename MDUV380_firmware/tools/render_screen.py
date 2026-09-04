#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Піксельний рендер головного екрана з S-метром для ДЕННОЇ і НІЧНОЇ тем.

Нічого не вигадує: кольори беруться з таблиці themeDefaults у HX8353E_display.c,
гліфи -- зі справжнього шрифту HX8353E_charset_UA.h (той самий, що йде в українську
збірку), координати -- з констант SMETER_* у uiUtilities.c, а кольори проганяються
через те саме квантування RGB565, що й на живому дисплеї.
"""
import re, sys
from PIL import Image, ImageDraw, ImageFont

FW = '/root/work/repo/MDUV380_firmware'
W, H = 160, 128

# ---------- 1. Теми з реального коду ----------------------------------------
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
print(f"тем прочитано: {list(themes)}; елементів: {len(themes['DAY'])}/{len(themes['NIGHT'])}")

def rgb565(c):
    """Те саме квантування, що робить RGB888_TO_PLATFORM_COLOUR_FORMAT (5-6-5 біт)."""
    r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
    r, g, b = r & 0xF8, g & 0xFC, b & 0xF8
    return (r | r >> 5, g | g >> 6, b | b >> 5)

# ---------- 2. Шрифти зі справжнього UA-набору ------------------------------
fsrc = open(f'{FW}/application/include/hardware/HX8353E_charset_UA.h', encoding='latin-1').read()
def load_font(name):
    m = re.search(name + r'\s*\[\s*\]\s*=\s*\{(.*?)\};', fsrc, re.S)
    body = re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S))
    v = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body)]
    return dict(vals=v, start=v[2], end=v[3], w=v[4], h=v[5], bpc=v[7])
F1 = load_font('font_6x8')       # FONT_SIZE_1
F3 = load_font('font_8x16')      # FONT_SIZE_3
print(f"font_6x8: {F1['w']}x{F1['h']} bpc={F1['bpc']}   font_8x16: {F3['w']}x{F3['h']} bpc={F3['bpc']}")

class FB:
    def __init__(self, bg):
        self.px = [[bg] * W for _ in range(H)]
    def fill(self, x, y, w, h, c):
        for yy in range(y, min(y + h, H)):
            for xx in range(x, min(x + w, W)):
                if xx >= 0 and yy >= 0:
                    self.px[yy][xx] = c
    def rect(self, x, y, w, h, c):          # displayDrawRect -- лише контур
        self.fill(x, y, w, 1, c); self.fill(x, y + h - 1, w, 1, c)
        self.fill(x, y, 1, h, c); self.fill(x + w - 1, y, 1, h, c)
    def text(self, x, y, s, c, font, align='left'):
        """Копія displayPrintCore: малює ЛИШЕ засвічені пікселі, фон не чіпає."""
        data = s.encode('cp1251')
        n = len(data)
        if align == 'center':
            x = (W - font['w'] * n) // 2
        elif align == 'right':
            x = W - font['w'] * n
        for i, code in enumerate(data):
            if code == 0x20:
                continue
            off = 8 + (code - font['start']) * font['bpc']
            cols = font['vals'][off:off + font['bpc']]
            for cx in range(font['w']):
                for cy in range(font['h']):
                    byte = cols[cx + (cy // 8) * font['w']]
                    if (byte >> (cy % 8)) & 1:
                        px, py = x + i * font['w'] + cx, y + cy
                        if 0 <= px < W and 0 <= py < H:
                            self.px[py][px] = c
        return x

# ---------- 3. Константи S-метра з uiUtilities.c ----------------------------
u = open(f'{FW}/application/source/user_interface/uiUtilities.c', encoding='utf-8').read()
def const(name):
    return int(re.search(r'#define\s+' + name + r'\s+(\d+)', u).group(1))
BLOCK_Y = const('SMETER_BLOCK_Y'); BLOCK_H = const('SMETER_BLOCK_H')
FRAME_X = const('SMETER_FRAME_X');  FRAME_W = const('SMETER_FRAME_W')
FRAME_H = const('SMETER_FRAME_H');  BAR_X   = const('SMETER_BAR_X')
BAR_W   = const('SMETER_BAR_WIDTH'); BAR_H  = const('SMETER_BAR_H')
VAL_R   = const('SMETER_VALUE_RIGHT'); FONT_W = const('SMETER_FONT_W')
VAL_MIN = const('SMETER_VALUE_MIN_X')
S9_POS  = const('SMETER_S9_POS');   ABOVE   = const('SMETER_ABOVE_S9_DB')
S0, S9DB = -129, -93
BAR_Y = BLOCK_Y + 2; SCALE_Y = BLOCK_Y + 15
def const_expr(name):
    """Як const(), але терпить #define у вигляді виразу: (SMETER_BLOCK_Y + 12)."""
    body = re.search(r'#define\s+' + name + r'\s+(.+)', u).group(1).split('//')[0].strip()
    return eval(body, {}, {'SMETER_BLOCK_Y': BLOCK_Y})
TICK_Y = const_expr('SMETER_TICK_Y'); TICK_MAJ = const('SMETER_TICK_H_MAJOR'); TICK_MIN = const('SMETER_TICK_H_MINOR')
print(f"константи: блок y={BLOCK_Y}..{BLOCK_Y+BLOCK_H-1}, рамка x={FRAME_X}..{FRAME_X+FRAME_W-1} "
      f"y={BLOCK_Y}..{BLOCK_Y+FRAME_H-1}, смуга {BAR_X}..{BAR_X+BAR_W-1}, S9 на {S9_POS} ({S9_POS*100//BAR_W}%)")

def pixpos(dbm):
    """Копія smeterPixelPos() з uiUtilities.c."""
    if dbm <= S0:  return 0
    if dbm <= S9DB: return ((dbm - S0) * S9_POS) // (S9DB - S0)
    above = max(0, min(dbm - S9DB, ABOVE))
    return S9_POS + (above * (BAR_W - S9_POS)) // ABOVE

def draw_screen(theme, dbm, contact, callinfo, header_l, header_r):
    T = lambda k: rgb565(theme[k])
    fb = FB(T('THEME_ITEM_BG'))
    # шапка
    fb.fill(0, 0, W, 10, T('THEME_ITEM_BG_HEADER_TEXT'))
    fb.text(0, 2, header_l, T('THEME_ITEM_FG_HEADER_TEXT'), F1)
    fb.text(0, 2, header_r, T('THEME_ITEM_FG_HEADER_TEXT'), F1, 'right')
    # рядок позивного і рядок типу виклику
    fb.text(0, 24, contact,  T('THEME_ITEM_FG_CHANNEL_CONTACT'), F3, 'center')
    fb.text(0, 64, callinfo, T('THEME_ITEM_FG_CHANNEL_CONTACT_INFO'), F3, 'center')
    # ---- блок S-метра, крок у крок як drawSMeterBlock() ----
    bw = max(0, min(pixpos(dbm), BAR_W))
    fb.fill(0, BLOCK_Y, W, BLOCK_H, T('THEME_ITEM_BG'))
    if bw:
        fb.fill(BAR_X, BAR_Y, bw, BAR_H, T('THEME_ITEM_FG_RSSI_BAR'))
    if bw > S9_POS:
        fb.fill(BAR_X + S9_POS, BAR_Y, bw - S9_POS, BAR_H, T('THEME_ITEM_FG_RSSI_BAR_S9P'))
    # displayDrawRect(..., true) == передній план (див. коментар у drawSMeterBlock)
    fb.rect(FRAME_X, BLOCK_Y, FRAME_W, FRAME_H, T('THEME_ITEM_FG_DECORATION'))
    # поділки: довгі під 1,3,5,7,9, короткі під парними
    for s_ in range(1, 10):
        tx = BAR_X + pixpos(S0 + s_ * 4)
        th = TICK_MAJ if (s_ % 2) else TICK_MIN
        fb.fill(tx, TICK_Y, 1, th, T('THEME_ITEM_FG_DECORATION'))
    fb.fill(BAR_X + S9_POS, TICK_Y, 1, TICK_MAJ, T('THEME_ITEM_FG_RSSI_BAR_S9P'))
    for s_ in range(1, 10, 2):
        fb.text(BAR_X + pixpos(S0 + s_ * 4) - FONT_W // 2, SCALE_Y, str(s_),
                T('THEME_ITEM_FG_DECORATION'), F1)
    buf = f'{dbm}'
    tx = max(VAL_MIN, VAL_R - len(buf) * FONT_W)
    fb.text(tx, SCALE_Y, buf, T('THEME_ITEM_FG_DEFAULT'), F1)   # значення -- на рядку шкали
    return fb

# ---------- 4. Збірка картинки ----------------------------------------------
SCALE, PAD, GAP, TOP = 4, 24, 40, 46
shots = [
    ('ДЕННА  ·  -131 дБм (немає сигналу)', 'DAY',   -131),
    ('ДЕННА  ·  -114 дБм (S3.75)',         'DAY',   -114),
    ('НІЧНА  ·  -101 дБм (S7)',            'NIGHT', -101),
    ('НІЧНА  ·  -85 дБм (S9+8)',           'NIGHT',  -85),
]
cols, rows = 2, 2
iw, ih = W * SCALE, H * SCALE
out = Image.new('RGB', (PAD * 2 + cols * iw + (cols - 1) * GAP,
                        TOP + rows * (ih + TOP) - 10), (26, 26, 28))
d = ImageDraw.Draw(out)
try:
    fnt = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 22)
except OSError:
    fnt = ImageFont.load_default()

for idx, (label, theme, dbm) in enumerate(shots):
    fb = draw_screen(themes[theme], dbm, 'PARROT', 'Приватний виклик',
                     'DMR TS1 500mW C1', '07:47')
    img = Image.new('RGB', (W, H))
    img.putdata([p for row in fb.px for p in row])
    img = img.resize((iw, ih), Image.NEAREST)
    cx = PAD + (idx % cols) * (iw + GAP)
    cy = TOP + (idx // cols) * (ih + TOP)
    d.text((cx, cy - 30), label, font=fnt, fill=(235, 235, 235))
    out.paste(img, (cx, cy))
    d.rectangle([cx - 1, cy - 1, cx + iw, cy + ih], outline=(90, 90, 95))

path = '/tmp/claude-0/-home-claude/7814cbab-09c5-5734-b4d8-b300fee9c685/scratchpad/smeter_preview6.png'
out.save(path)
print('збережено:', path, out.size)
