#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Перевірка, чи влазять українські підписи меню у рядок екрана.

ЩО САМЕ ОБМЕЖУЄ. Не пікселі. Пункт меню збирається як `snprintf(buf, SCREEN_LINE_BUFFER_SIZE,
"%s:%s", підпис, значення)`, а SCREEN_LINE_BUFFER_SIZE = 17 (uiGlobals.h:300), тобто
16 символів + NUL. Усе, що довше, snprintf МОВЧКИ обрізає -- без попередження й без
жодного сліду в коді. На екрані це виглядає як обірваний підпис (напр. "Настройка R"
замість "Настройка Rx"), і саме так це й помічають -- очима, на живій рації.

ДЖЕРЕЛО БЮДЖЕТУ. В english.h біля багатьох рядків стоїть коментар виду
`// MaxLen: 16 (with ':' + .on or .off)` -- це задум автора: скільки лишається на сам
підпис з урахуванням того, що додається праворуч. Скрипт бере саме ці числа, а не вигадує
свої, і додатково перевіряє реальні пари "підпис + значення", які видно в коді меню.

Кодування: ukrainian.h та інші *_ua.h -- cp1251, по одному байту на символ, тож довжина в
байтах дорівнює довжині в символах. Саме так їх бачить і прошивка.

Запуск:  python3 MDUV380_firmware/tools/check_menu_widths.py
Код виходу 1, якщо є переповнення -- можна ставити в CI.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, '..', 'application')
LANG = os.path.join(BASE, 'include', 'user_interface', 'languages')

LINE_CHARS = 16          # SCREEN_LINE_BUFFER_SIZE (17) - 1 на NUL


OUR_PLATFORM = 'PLATFORM_MDUV380'

# Пункти, де числове значення ЗАВІДОМО коротке, тож малий залишок місця -- не проблема.
# Тільки з обґрунтуванням: без нього це перетворюється на глушилку попереджень.
NUMERIC_OK = {
    'text_size': 'значення лише 1 або 2 (BIT_UI_USES_DOUBLE_HEIGHT), одна цифра',
}


def platform_gated_cases(src):
    """Множина міток case, які лежать під #if defined(PLATFORM_...) чужої платформи.

    Без цього перевірка репортує, напр., "Trackball:Високий" -- пункт, що збирається лише
    для MD2017 і на нашій рації не існує взагалі. Простий скан препроцесорних гілок:
    точний парсер тут не потрібен, бо в цих файлах гілки платформ не вкладені одна в одну.
    """
    gated, stack = set(), []
    for line in src.splitlines():
        s = line.strip()
        m = re.match(r'#\s*if\s+defined\(\s*(PLATFORM_\w+)\s*\)\s*$', s)
        if m:
            stack.append(m.group(1) != OUR_PLATFORM)
            continue
        if re.match(r'#\s*(else|elif)\b', s) and stack:
            stack[-1] = not stack[-1]
            continue
        if re.match(r'#\s*endif\b', s):
            if stack:
                stack.pop()
            continue
        cm = re.match(r'case\s+([A-Za-z_0-9]+)\s*:', s)
        if cm and any(stack):
            gated.add(cm.group(1))
    return gated


def load_strings(path, encoding):
    """Повертає {ключ: значення} з мовного файлу."""
    txt = open(path, 'rb').read().decode(encoding, errors='replace')
    return {m.group(1): m.group(2)
            for m in re.finditer(r'\.(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"', txt)}


def load_maxlen(path):
    """Повертає {ключ: (бюджет, текст_коментаря)} з коментарів // MaxLen: N у english.h."""
    out = {}
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\s*\.(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"\s*,\s*//\s*Max[Ll]en:?\s*(\d+)(.*)', line)
        if m:
            out[m.group(1)] = (int(m.group(3)), m.group(4).strip())
    return out


en = load_strings(os.path.join(LANG, 'english.h'), 'latin-1')
ua = load_strings(os.path.join(LANG, 'ukrainian.h'), 'cp1251')
budget = load_maxlen(os.path.join(LANG, 'english.h'))

fails = []

# --- 1. Український рядок проти оголошеного бюджету --------------------------
print('=== 1. Підпис довший за оголошений MaxLen ===')
over = []
for key, (lim, note) in sorted(budget.items()):
    if key not in ua:
        continue
    n = len(ua[key])
    if n > lim:
        over.append((n - lim, key, ua[key], n, lim, en.get(key, ''), note))
for d, key, s, n, lim, e, note in sorted(over, key=lambda x: -x[0]):
    print(f'  +{d}  {key:32} "{s}" = {n} > {lim}   (en: "{e}") {note}')
print(f'  разом: {len(over)}')
fails += over

# --- 2. Реальні пари "підпис:значення" з коду меню ---------------------------
# Беремо лише те, що однозначно видно: leftSide = currentLanguage->X разом із
# rightSideConst = ... currentLanguage->Y у тому самому case-блоці.
print('\n=== 2. Пари "підпис:значення" довші за 16 символів ===')
pairs = []
seen = set()
files = sorted(glob.glob(os.path.join(BASE, 'source', 'user_interface', 'menu*.c')) +
               glob.glob(os.path.join(BASE, 'source', 'user_interface', 'ui*.c')))
for path in files:
    src = open(path, encoding='utf-8', errors='replace').read()
    gated = platform_gated_cases(src)
    parts = re.split(r'\n\s*case\s+([A-Za-z_0-9]+)\s*:', src)
    for cname, blk in zip(parts[1::2], parts[2::2]):
        if cname in gated:
            continue                       # пункт іншої платформи -- у нас його немає
        blk = blk[:2500]
        lm = re.search(r'leftSide\s*=\s*currentLanguage->(\w+)', blk)
        if not lm or lm.group(1) not in ua:
            continue
        label = ua[lm.group(1)]
        vals = set(re.findall(r'rightSideConst\s*=\s*[^;]*?currentLanguage->(\w+)', blk))
        for am in re.finditer(r'const\s+char\s*\*\s*\w+\s*\[\s*\]\s*=\s*\{([^}]*)\}', blk):
            vals |= set(re.findall(r'currentLanguage->(\w+)', am.group(1)))
        cand = [(v, ua[v]) for v in vals if v in ua]
        if not cand:
            continue
        vkey, vtext = max(cand, key=lambda kv: len(kv[1]))
        total = len(label) + 1 + len(vtext)
        k = (lm.group(1), vkey)
        if total > LINE_CHARS and k not in seen:
            seen.add(k)
            pairs.append((total - LINE_CHARS, os.path.basename(path), lm.group(1), vkey,
                          f'{label}:{vtext}', total))
for d, f, lk, vk, line, total in sorted(pairs, key=lambda x: -x[0]):
    print(f'  +{d}  {f:26} {lk}/{vk}')
    print(f'        "{line}" = {total} > {LINE_CHARS}')
print(f'  разом: {len(pairs)}')
fails += pairs

# --- 3. Пункти з ЧИСЛОВИМ значенням ------------------------------------------
# Саме тут ховався реальний дефект: "Рівень потуж." (13) + ':' + "250" = 17 -> snprintf
# обрізав до 16, і оператор бачив ЗНАЧЕННЯ 25 замість 250. Частини 1 і 2 такого не ловлять:
# бюджет MaxLen у english.h стоїть не скрізь, а значення тут не з мовної таблиці, а з
# snprintf(rightSideVar, ...).
#
# Рахуємо ТОЧНО ту частину, яку можна порахувати: довжину літеральних шматків формату.
# Далі показуємо, скільки СИМВОЛІВ ЛИШАЄТЬСЯ на самі цифри. Менш ніж 3 -- підозріло:
# більшість числових налаштувань рації доходять до трьох цифр (0..255, потужність 250 мВт,
# кути, відсотки), а від'ємні -- до чотирьох.
MIN_DIGITS = 3

print('\n=== 3. Скільки місця лишається на числове значення ===')
tight = []
seen3 = set()
for path in files:
    src = open(path, encoding='utf-8', errors='replace').read()
    gated = platform_gated_cases(src)
    parts = re.split(r'\n\s*case\s+([A-Za-z_0-9]+)\s*:', src)
    for cname, blk in zip(parts[1::2], parts[2::2]):
        if cname in gated:
            continue
        blk = blk[:2500]
        lm = re.search(r'leftSide\s*=\s*currentLanguage->(\w+)', blk)
        sm = re.search(r'snprintf\(\s*rightSideVar\s*,[^,]+,\s*"((?:[^"\\]|\\.)*)"', blk)
        if not lm or not sm or lm.group(1) not in ua or lm.group(1) in NUMERIC_OK:
            continue
        fmt = sm.group(1)
        convs = re.findall(r'%[-+ #0]*\d*(?:\.\d+)*[a-zA-Z]', fmt)
        if any(c.endswith('s') for c in convs):
            continue                      # ширина рядкового аргументу тут невідома
        # "%%" друкується як ОДИН символ '%'. Без цієї заміни воно рахувалося за два, і
        # перевірка давала хибну тривогу на кожному пункті з відсотками (яскравість, шумодав).
        literal = re.sub(r'%[-+ #0]*\d*(?:\.\d+)*[a-zA-Z]', '', fmt).replace('%%', '%')
        # %03u тощо мають власну мінімальну ширину -- враховуємо її як уже зайняту
        fixed = sum(int(m) for m in re.findall(r'%[-+ #0]*(\d+)[a-zA-Z]', fmt))
        label = ua[lm.group(1)]
        left = LINE_CHARS - len(label) - 1 - len(literal) - fixed
        free_convs = len(convs) - len(re.findall(r'%[-+ #0]*\d+[a-zA-Z]', fmt))
        if free_convs <= 0:
            continue
        k = (lm.group(1), fmt)
        if left < MIN_DIGITS and k not in seen3:
            seen3.add(k)
            tight.append((left, os.path.basename(path), lm.group(1), label, fmt))
for left, f, key, label, fmt in sorted(tight, key=lambda x: x[0]):
    print(f'  {left:>2} символ(ів) на цифри  {f:26} {key}')
    print(f'        "{label}:" + формат "{fmt}"')
print(f'  разом підозрілих: {len(tight)}')
fails += tight

print(f'\n{"Є ПЕРЕПОВНЕННЯ" if fails else "Переповнень не знайдено"} (всього: {len(fails)})')
sys.exit(1 if fails else 0)
