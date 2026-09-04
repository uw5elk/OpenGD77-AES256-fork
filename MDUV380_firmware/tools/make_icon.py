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
    #
    # Антена -- строго вертикальна, лівіше центру корпусу (як на більшості носимих
    # радіостанцій). Малюємо її заокругленим прямокутником, а не лінією: так у неї
    # рівний закруглений кінчик, і на дрібних розмірах він не "розсипається".
    if detailed:
        ant = (0.355, 0.085, 0.415, 0.360)   # x0, y0, x1, y1
        ant_r = 0.030
        body = (0.300, 0.335, 0.700, 0.905)
        body_r = 0.060
    else:
        ant = (0.320, 0.070, 0.430, 0.330)
        ant_r = 0.055
        body = (0.230, 0.290, 0.770, 0.930)
        body_r = 0.080

    # --- антена ---
    d.rounded_rectangle([u(ant[0]), u(ant[1]), u(ant[2]), u(ant[3])],
                        radius=u(ant_r), fill=BODY)

    # --- корпус ---
    d.rounded_rectangle([u(body[0]), u(body[1]), u(body[2]), u(body[3])],
                        radius=u(body_r), fill=BODY)

    if detailed:
        # екран
        d.rounded_rectangle([u(0.350), u(0.395), u(0.650), u(0.560)],
                            radius=u(0.020), fill=SCREEN)
        # решітка динаміка -- три смужки
        for i in range(3):
            y = 0.625 + i * 0.072
            d.rounded_rectangle([u(0.370), u(y), u(0.630), u(y + 0.038)],
                                radius=u(0.019), fill=YELLOW)
    else:
        # спрощено: великий екран і одна широка смуга динаміка
        d.rounded_rectangle([u(0.300), u(0.360), u(0.700), u(0.595)],
                            radius=u(0.032), fill=SCREEN)
        d.rounded_rectangle([u(0.320), u(0.680), u(0.680), u(0.775)],
                            radius=u(0.038), fill=YELLOW)

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
