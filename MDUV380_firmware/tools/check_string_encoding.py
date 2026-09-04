#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Шукає кириличні рядкові літерали, записані в UTF-8, у коді прошивки.

ЧОМУ ЦЕ ПОМИЛКА. Шрифт рації (`HX8353E_charset_UA.h`) індексується БАЙТОМ у кодуванні
cp1251: одна кирилична літера -- один байт. Якщо літерал у .c-файлі збережено як UTF-8,
кожна літера це ДВА байти, і на екрані виходить каша з двох чужих гліфів замість літери.
Компілятор при цьому мовчить: для нього це просто масив байтів.

Саме так (2026-09-04) на екрані запитувача виглядало сповіщення радіоперевірки
"Радіоперевірка: ID %lu на зв'язку" з `dmr_rctl_tx.c` -- єдине місце, де український
текст написали прямо в .c замість мовного файлу.

ПРАВИЛЬНО: український текст лежить у `languages/*_ua.h`, і ці файли збережені в cp1251
(див. коментар у їх шапці). Тому перевіряються лише `.c`; заголовки мов пропускаються.

Коментарі ігноруються -- у них UTF-8 доречний і читабельний у редакторі.

Запуск:  python3 MDUV380_firmware/tools/check_string_encoding.py
Код виходу 1, якщо знайдено хоч один такий літерал.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'application', 'source')


def strip_comments(data: bytes) -> bytes:
    """Прибирає /* */ і // коментарі, зберігаючи довжину (щоб не з'їхали номери рядків).

    Наївний прохід достатній: нас цікавлять лише байти >= 0x80 у літералах, а рядкові
    літерали з послідовністю "/*" усередині тут не трапляються.
    """
    out = bytearray(data)
    i, n = 0, len(data)
    while i < n - 1:
        if data[i:i + 2] == b'/*':
            j = data.find(b'*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != 0x0A:
                    out[k] = 0x20
            i = j
        elif data[i:i + 2] == b'//':
            j = data.find(b'\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = 0x20
            i = j
        elif data[i] == 0x22:                      # пропускаємо рядковий літерал цілком
            j = i + 1
            while j < n and data[j] != 0x22:
                j += 2 if data[j] == 0x5C else 1
            i = j + 1
        else:
            i += 1
    return bytes(out)


hits = []
for root, _dirs, files in os.walk(SRC):
    for name in sorted(files):
        if not name.endswith('.c'):
            continue
        path = os.path.join(root, name)
        code = strip_comments(open(path, 'rb').read())
        for num, line in enumerate(code.split(b'\n'), 1):
            for m in re.finditer(rb'"((?:[^"\\\n]|\\.)*)"', line):
                lit = m.group(1)
                if not any(b >= 0x80 for b in lit):
                    continue
                try:
                    text = lit.decode('utf-8')
                except UnicodeDecodeError:
                    continue                        # не UTF-8 -> найпевніше вже cp1251, добре
                if any('Ѐ' <= ch <= 'ӿ' for ch in text):
                    hits.append((os.path.relpath(path, SRC), num, text))

for f, num, text in hits:
    print(f'  {f}:{num}  "{text}"')
    print('        кирилиця в UTF-8 -> на екрані буде каша; текст має жити в languages/*_ua.h (cp1251)')

print(f'\n{"ЗНАЙДЕНО" if hits else "Чисто"}: {len(hits)} UTF-8 кириличних літералів у коді')
sys.exit(1 if hits else 0)
