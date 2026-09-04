#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Генератор іконки Windows-додатка (tools/app_icon.ico).

Малюється кодом, а не в редакторі, щоб її можна було перегенерувати після будь-якої
правки й щоб у репозиторії лежало ДЖЕРЕЛО, а не лише бінарник.

Задум: узагальнений силует носимої радіостанції на тлі кольорів українського
прапора. Свідомо НЕ використовуються ані логотип, ані фірмові написи TYT, ані
характерні риси промислового дизайну конкретної моделі -- лише впізнаваний загальний
образ "рація з антеною", який ні на що не претендує. Державний герб теж не беремо:
для іконки хобі-програми достатньо кольорів прапора.

Кольори прапора -- офіційні: синій #0057B7, жовтий #FFD700.

Малюємо у нормалізованих координатах (0..1) з передискретизацією 8x, тож одна й та
сама геометрія дає чіткий результат на всіх розмірах. Для дрібних розмірів (<=32px)
беремо спрощений варіант: дрібні деталі там усе одно перетворюються на кашу.

Запуск:  python3 MDUV380_firmware/tools/make_icon.py
"""
import os
import sys

from PIL import Image, ImageDraw

BLUE = (0x00, 0x57, 0xB7)
YELLOW = (0xFF, 0xD7, 0x00)
BODY = (0x10, 0x18, 0x28)      # темно-синій корпус -- контрастує і з синім, і з жовтим
SCREEN = (0x7B, 0xD2, 0xFF)    # той самий відтінок, що FG_NOTIFICATION у нічній темі рації
WAVE = (0xFF, 0xFF, 0xFF)

SS = 8  # передискретизація


def draw_icon(size, detailed=True):
    """Малює іконку розміром size x size."""
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = lambda v: int(round(v * S))  # нормалізовані координати -> пікселі

    # --- тло: заокруглений квадрат, верх синій, низ жовтий ---
    pad, radius = u(0.015), u(0.20)
    bg = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bg)
    bd.rounded_rectangle([pad, pad, S - pad - 1, S - pad - 1], radius=radius, fill=BLUE)
    bd.rectangle([pad, S // 2, S - pad - 1, S - pad - 1], fill=YELLOW)
    # повторно скругляємо низ, бо прямокутник вище зрізав нижні кути
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, S - pad - 1, S - pad - 1],
                                           radius=radius, fill=255)
    img.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(img)

    # Дрібні розміри (<=32px) отримують ОКРЕМУ, крупнішу геометрію: на 16px
    # деталі все одно зникають, тому там важливий лише впізнаваний силует, який
    # має займати якомога більше площі плитки.
    if detailed:
        tipx, tipy = 0.335, 0.150
        waves = (0.115, 0.180, 0.245)
        wave_w, ant_w = 0.030, 0.048
        ant = (0.335, 0.175, 0.395, 0.400)
        body = (0.330, 0.360, 0.670, 0.895)
        body_r = 0.055
    else:
        tipx, tipy = 0.300, 0.120
        waves = (0.140, 0.225)
        wave_w, ant_w = 0.055, 0.080
        ant = (0.300, 0.150, 0.370, 0.330)
        body = (0.240, 0.300, 0.760, 0.925)
        body_r = 0.075

    # --- радіохвилі від кінчика антени (білі -- добре видно на синьому) ---
    for r in waves:
        box = [u(tipx - r), u(tipy - r), u(tipx + r), u(tipy + r)]
        d.arc(box, start=-72, end=18, fill=WAVE, width=u(wave_w))

    # --- антена ---
    d.line([u(ant[0]), u(ant[1]), u(ant[2]), u(ant[3])], fill=BODY, width=u(ant_w))

    # --- корпус ---
    d.rounded_rectangle([u(body[0]), u(body[1]), u(body[2]), u(body[3])],
                        radius=u(body_r), fill=BODY)

    if detailed:
        # екран
        d.rounded_rectangle([u(0.375), u(0.415), u(0.625), u(0.570)],
                            radius=u(0.018), fill=SCREEN)
        # решітка динаміка -- три смужки
        for i in range(3):
            y = 0.635 + i * 0.070
            d.rounded_rectangle([u(0.395), u(y), u(0.605), u(y + 0.036)],
                                radius=u(0.018), fill=YELLOW)
    else:
        # спрощено: великий екран і одна широка смуга динаміка
        d.rounded_rectangle([u(0.310), u(0.370), u(0.690), u(0.590)],
                            radius=u(0.030), fill=SCREEN)
        d.rounded_rectangle([u(0.330), u(0.670), u(0.670), u(0.760)],
                            radius=u(0.035), fill=YELLOW)

    return img.resize((size, size), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    sizes = [256, 128, 64, 48, 32, 16]
    frames = [draw_icon(s, detailed=(s >= 48)) for s in sizes]

    ico = os.path.join(here, "app_icon.ico")
    frames[0].save(ico, format="ICO",
                   sizes=[(s, s) for s in sizes],
                   append_images=frames[1:])
    print("записано:", ico, os.path.getsize(ico), "байт")

    # прев'ю: усі розміри в ряд на нейтральному тлі, щоб оцінити читабельність
    prev_w = sum(s + 24 for s in sizes) + 24
    prev = Image.new("RGB", (prev_w, 256 + 60), (0x2A, 0x2A, 0x2E))
    x = 24
    for s, f in zip(sizes, frames):
        prev.paste(f, (x, 24 + (256 - s) // 2), f)
        x += s + 24
    png = os.path.join(here, "app_icon_preview.png")
    prev.save(png)
    print("прев'ю:", png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
