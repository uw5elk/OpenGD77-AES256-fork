#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Перевіряє, що центр ФАРБИ кожної цифри шкали стоїть рівно на своїй поділці.
Малює той самий кадр, що й render_screen.py, і рахує пікселі, а не дивиться на око."""
import importlib.util, os, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('rs', os.path.join(HERE, 'render_screen.py'))
rs = importlib.util.module_from_spec(spec)
sys.stdout = open('/dev/null', 'w')          # рендер друкує своє
spec.loader.exec_module(rs)
sys.stdout = sys.__stdout__

fb = rs.draw_screen(rs.themes['NIGHT'], -85, 'PARROT', 'Приватний виклик',
                    'DMR TS1 500mW C1', '07:47')
bg = rs.rgb565(rs.themes['NIGHT']['THEME_ITEM_BG'])

def ink_cols(y0, y1, x0, x1):
    return [x for x in range(x0, x1 + 1)
            if any(fb.px[y][x] != bg for y in range(y0, y1 + 1))]

fails = 0
print(f"поділки y={rs.TICK_Y}..{rs.TICK_Y + rs.TICK_MAJ - 1}, цифри y={rs.SCALE_Y}..{rs.SCALE_Y + 6}\n")
for s in range(1, 10, 2):
    tick_x = rs.BAR_X + rs.pixpos(rs.S0 + s * 4)
    # фарба цифри шукається у вікні +-5px навколо поділки
    cols = ink_cols(rs.SCALE_Y, rs.SCALE_Y + 6, tick_x - 5, tick_x + 5)
    lo, hi = min(cols), max(cols)
    centre2 = lo + hi                                  # подвоєний центр, без втрати 0.5
    ok = (centre2 == 2 * tick_x)
    fails += (not ok)
    print(f"S{s}: поділка x={tick_x:3d} | фарба цифри {lo}..{hi}, центр {centre2/2:5.1f}  "
          f"{'OK' if ok else '<<< ЗСУВ ' + format(centre2/2 - tick_x, '+.1f')}")

# нічого не має налазити: сусідні цифри й значення
nine_x = rs.BAR_X + rs.pixpos(rs.S0 + 36)
nine_hi = max(ink_cols(rs.SCALE_Y, rs.SCALE_Y + 6, nine_x - 5, nine_x + 5))
# вікно ПІСЛЯ дев'ятки, інакше в нього потрапляє її ж хвіст
val_cols = ink_cols(rs.SCALE_Y, rs.SCALE_Y + 6, nine_hi + 1, 159)
gap = min(val_cols) - nine_hi - 1
print(f"\nпроміжок між фарбою '9' (кінець x={nine_hi}) і значенням (початок x={min(val_cols)}): {gap}px")
if gap < 1:
    print("<<< значення налазить на цифру 9"); fails += 1

# поділки і цифри не мають перетинатись по вертикалі
if rs.TICK_Y + rs.TICK_MAJ - 1 >= rs.SCALE_Y:
    print("<<< поділки залазять у рядок цифр"); fails += 1
else:
    print(f"вертикальний проміжок поділки->цифри: {rs.SCALE_Y - (rs.TICK_Y + rs.TICK_MAJ)}px")

print("\n" + ("Є ПОМИЛКИ" if fails else "Усе вирівняно"), f"(провалів: {fails})")

# зум смуги S-метра для ока
y0, y1 = rs.BLOCK_Y, rs.BLOCK_Y + rs.BLOCK_H - 1
img = Image.new('RGB', (160, y1 - y0 + 1))
img.putdata([p for row in fb.px[y0:y1 + 1] for p in row])
img.resize((160 * 9, (y1 - y0 + 1) * 9), Image.NEAREST).save(os.path.join(HERE, 'align_zoom.png'))
print('зум збережено:', os.path.join(HERE, 'align_zoom.png'))
sys.exit(1 if fails else 0)
