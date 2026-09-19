#!/usr/bin/env python3
"""mem_report.py -- звіт про СТАТИЧНЕ використання пам'яті готової збірки
(RAM/CCMRAM), з готового .elf (arm-none-eabi-gcc). Розвідка
docs/recording-feasibility.md (2026-09-19): п.1.3 того звіту мав лише
статичний здогад "правдоподібно достатньо" -- цей скрипт замінює здогад на
виміряні числа, для КОЖНОЇ конфігурації матриці збірки (build.yml кличе його
з кожного build/build-ukrainian job'у одразу після make).

РОЗМІТКА ПАМ'ЯТІ (STM32F405VGTX_FLASH.ld -- звідти й узято ці числа, не
вигадані тут):
    RAM    (0x20000000, 128 КБ = 131072 Б) <- вихідні секції .data + .bss +
                                               ._user_heap_stack (лінкер:
                                               "Uninitialized data section
                                               into RAM" + резерв під
                                               newlib heap/stack)
    CCMRAM (0x10000000,  64 КБ =  65536 Б) <- вихідна секція .ccmram; туди
                                               ж KEEP()-ом влито й
                                               .aes_ccmram (форкові AES-буфери,
                                               навмисно ПІСЛЯ ccm-даних кодека,
                                               щоб не зсунути жорстко зашиті
                                               адреси ambebuffer_*) -- у вже
                                               злінкованому .elf це ОДНА
                                               вихідна секція, окремо в
                                               символах не видно й не треба.

Що робить:
  1. `arm-none-eabi-size -A <elf>`      -> зайнято/вільно по RAM і CCMRAM.
  2. `arm-none-eabi-nm --format=sysv -S --size-sort <elf>`
                                        -> топ-N найбільших об'єктів окремо
                                           в RAM (.data/.bss/._user_heap_stack)
                                           і в CCMRAM (.ccmram).
  3. Окрема перевірка одного конкретного символу (за замовчуванням --
     screenNotificationBufData, п.4 задачі 2026-09-19) -- де він і скільки
     важить, щоб питання "чи справді він у CCM" мало виміряну відповідь,
     а не греп по коду.

Використання (у CI, після make):
    python3 tools/mem_report.py build/openuv380-10w.elf --top 20

Для перевірки самого парсера БЕЗ ARM-тулчейну (я саме так це й перевіряв --
пісочниця без arm-none-eabi-* тулів):
    python3 tools/mem_report.py --self-test
"""
import argparse
import re
import subprocess
import sys

RAM_ORIGIN = 0x20000000
RAM_SIZE = 128 * 1024
CCMRAM_ORIGIN = 0x10000000
CCMRAM_SIZE = 64 * 1024

RAM_SECTIONS = (".data", ".bss", "._user_heap_stack")
CCM_SECTIONS = (".ccmram",)

DEFAULT_WATCH_SYMBOL = "screenNotificationBufData"


def run_tool(argv):
    try:
        return subprocess.run(argv, check=True, capture_output=True, text=True).stdout
    except FileNotFoundError:
        sys.exit(
            "ПОМИЛКА: не знайдено '%s' -- потрібен ARM-тулчейн (arm-none-eabi-*) у PATH."
            % argv[0]
        )
    except subprocess.CalledProcessError as e:
        sys.exit("ПОМИЛКА виклику %s:\n%s" % (" ".join(argv), e.stderr))


def parse_int_hex(text):
    """Value/Size у виводі `nm --format=sysv -S` -- завжди HEX (з чи без
    префіксу '0x', залежно від версії binutils)."""
    text = text.strip()
    if not text:
        return None
    text = text[2:] if text.lower().startswith("0x") else text
    try:
        return int(text, 16)
    except ValueError:
        return None


def parse_int_dec(text):
    """Value/Size у виводі `size -A` -- завжди ДЕСЯТКОВЕ число (на відміну
    від `nm --format=sysv`). ВАЖЛИВО: не можна пробувати hex тут -- десяткове
    "2048" є коректним і як hex (0x2048 = 8264), тож "спробувати hex, а тоді
    decimal" мовчки дає геть інше число замість помилки парсингу."""
    text = text.strip()
    if not text:
        return None
    try:
        return int(text, 10)
    except ValueError:
        return None


def parse_size_A(text):
    """Розбирає вивід `arm-none-eabi-size -A <elf>` -> {секція: розмір_байт}.
    Формат рядка: "<.section>   <size>   <addr>" -- беремо лише рядки, де
    перший токен починається з '.', і читаємо розмір як ДЕСЯТКОВЕ число
    (стандартний `size -A`, на відміну від `nm --format=sysv`, завжди друкує
    розмір колонки size у decimal)."""
    sizes = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2 or not parts[0].startswith("."):
            continue
        size = parse_int_dec(parts[1])
        if size is not None:
            sizes[parts[0]] = size
    return sizes


def parse_nm_sysv(text):
    """Розбирає вивід `arm-none-eabi-nm --format=sysv -S <elf>` -> список
    кортежів (name, section, size_bytes). Формат sysv -- рядки, розділені
    символом '|': Name | Value | Class | Type | Size | Line | Section.
    Пропускаємо заголовок, порожні рядки, символи без Size (недовизначені,
    функції без -S тощо) і Size == 0."""
    out = []
    for line in text.splitlines():
        if "|" not in line:
            continue
        cols = [c.strip() for c in line.split("|")]
        if len(cols) < 7:
            continue
        name, _value, _cls, _type, size_s, _line, section = cols[:7]
        if name == "Name" or not name:
            continue  # заголовок таблиці
        size = parse_int_hex(size_s)
        if not size:
            continue
        out.append((name, section, size))
    return out


def region_totals(size_sections, sections, budget_bytes):
    used = sum(size_sections.get(s, 0) for s in sections)
    free = budget_bytes - used
    pct = (used * 100.0 / budget_bytes) if budget_bytes else 0.0
    return used, free, pct


def print_region_report(title, origin, budget, sections, size_sections):
    used, free, pct = region_totals(size_sections, sections, budget)
    print("== %s (0x%08X, %d Б = %d КБ) ==" % (title, origin, budget, budget // 1024))
    for s in sections:
        if s in size_sections:
            print("  %-20s %8d Б" % (s, size_sections[s]))
        else:
            print("  %-20s  (немає в цій збірці)" % s)
    print("  %-20s %8d Б (%.1f%% від %d КБ)" % ("ЗАЙНЯТО РАЗОМ:", used, pct, budget // 1024))
    print("  %-20s %8d Б" % ("ВІЛЬНО:", free))
    if free < 0:
        print("  !!! ПЕРЕПОВНЕННЯ РЕГІОНУ -- лінкер мав би сам впасти з помилкою; "
              "якщо це видно тут, щось не так із самим парсингом.")
    print()
    return used, free


def print_top_n(symbols, sections, n, label):
    filtered = [s for s in symbols if s[1] in sections]
    filtered.sort(key=lambda t: t[2], reverse=True)
    print("== Топ-%d об'єктів у %s ==" % (n, label))
    if not filtered:
        print("  (нічого не знайдено -- перевір формат виводу nm вручну, "
              "парсер міг не впізнати рядки)")
        print()
        return
    for i, (name, section, size) in enumerate(filtered[:n], 1):
        print("  %2d. %8d Б  %-8s %s" % (i, size, section, name))
    print()


def print_watch_symbol(symbols, name):
    hits = [s for s in symbols if s[0] == name]
    print("== Перевірка конкретного символу: %s ==" % name)
    if not hits:
        print("  НЕ ЗНАЙДЕНО у символьній таблиці (перевір, чи не викинутий "
              "--gc-sections, або що назва не змінилась).")
    else:
        for nm_name, section, size in hits:
            where = "CCMRAM" if section in CCM_SECTIONS else ("RAM" if section in RAM_SECTIONS else section)
            print("  %s: секція %s (%s), %d Б" % (nm_name, section, where, size))
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?", help="шлях до зібраного .elf")
    ap.add_argument("--top", type=int, default=20, help="скільки найбільших об'єктів показувати (типово 20)")
    ap.add_argument("--watch", default=DEFAULT_WATCH_SYMBOL,
                     help="який символ перевірити окремо (типово %s)" % DEFAULT_WATCH_SYMBOL)
    ap.add_argument("--size-tool", default="arm-none-eabi-size")
    ap.add_argument("--nm-tool", default="arm-none-eabi-nm")
    ap.add_argument("--self-test", action="store_true",
                     help="перевірити парсер на вбудованих зразках виводу, без ARM-тулчейну й без .elf")
    args = ap.parse_args()

    if args.self_test:
        run_self_test()
        return

    if not args.elf:
        ap.error("треба вказати шлях до .elf (або --self-test)")

    size_text = run_tool([args.size_tool, "-A", args.elf])
    nm_text = run_tool([args.nm_tool, "--format=sysv", "-S", "--size-sort", args.elf])

    size_sections = parse_size_A(size_text)
    symbols = parse_nm_sysv(nm_text)

    print("Файл: %s\n" % args.elf)
    print_region_report("RAM", RAM_ORIGIN, RAM_SIZE, RAM_SECTIONS, size_sections)
    print_region_report("CCMRAM", CCMRAM_ORIGIN, CCMRAM_SIZE, CCM_SECTIONS, size_sections)
    print_top_n(symbols, set(RAM_SECTIONS), args.top, "RAM (.data/.bss/._user_heap_stack)")
    print_top_n(symbols, set(CCM_SECTIONS), args.top, "CCMRAM (.ccmram)")
    print_watch_symbol(symbols, args.watch)


# ---------------------------------------------------------------------------
# Самоперевірка парсера -- вбудовані зразки виводу binutils, побудовані за
# документованим форматом `size -A` і `nm --format=sysv`. Дозволяє перевірити
# логіку розбору БЕЗ arm-none-eabi-тулчейну (недоступний у пісочниці, де це
# писалось -- див. STATUS.md/docs/recording-feasibility.md).
# ---------------------------------------------------------------------------

_SAMPLE_SIZE_A = """\
build/openuv380-10w.elf  :
section               size        addr
.isr_vector            428    134348800
.text               524288    134349312
.data                 2048    536870912
.bss                40960    536872960
._user_heap_stack     1536    536913920
.ccmram              40976    268435456
.ARM.attributes         50            0
Total               610286
"""

_SAMPLE_NM_SYSV = """\
Name                  Value           Class        Type         Size             Line  Section

screenNotificationBufData|000000002000c000 |B          |OBJECT      |0000000000009c00 |    |.ccmram
screenBufData        |0000000020001000    |B          |OBJECT      |0000000000009c00 |    |.bss
com_buffer           |0000000010000000    |B          |OBJECT      |0000000000000600 |    |.ccmram
smallThing           |0000000020000010    |B          |OBJECT      |0000000000000004 |    |.bss
notASymbolSection    |0000000020000020    |B          |OBJECT      |0000000000000000 |    |.bss
"""


def run_self_test():
    sizes = parse_size_A(_SAMPLE_SIZE_A)
    assert sizes[".data"] == 2048, sizes
    assert sizes[".bss"] == 40960, sizes
    assert sizes["._user_heap_stack"] == 1536, sizes
    assert sizes[".ccmram"] == 40976, sizes
    assert ".ARM.attributes" not in sizes or sizes[".ARM.attributes"] == 50

    used_ram, free_ram, pct_ram = region_totals(sizes, RAM_SECTIONS, RAM_SIZE)
    assert used_ram == 2048 + 40960 + 1536 == 44544, used_ram
    assert free_ram == RAM_SIZE - 44544, free_ram

    used_ccm, free_ccm, pct_ccm = region_totals(sizes, CCM_SECTIONS, CCMRAM_SIZE)
    assert used_ccm == 40976, used_ccm
    assert free_ccm == CCMRAM_SIZE - 40976, free_ccm

    symbols = parse_nm_sysv(_SAMPLE_NM_SYSV)
    # 4 символи з ненульовим розміром (notASymbolSection має size=0 -- відкидається)
    assert len(symbols) == 4, symbols

    ccm_syms = sorted((s for s in symbols if s[1] == ".ccmram"), key=lambda t: t[2], reverse=True)
    assert ccm_syms[0][0] == "screenNotificationBufData", ccm_syms
    assert ccm_syms[0][2] == 0x9C00 == 39936, ccm_syms

    ram_syms = sorted((s for s in symbols if s[1] == ".bss"), key=lambda t: t[2], reverse=True)
    assert ram_syms[0][0] == "screenBufData", ram_syms

    hits = [s for s in symbols if s[0] == "screenNotificationBufData"]
    assert len(hits) == 1 and hits[0][1] == ".ccmram"

    print("Самоперевірка парсера ПРОЙДЕНА: size -A і nm --format=sysv "
          "розбираються коректно на зразках (region-суми, топ-N, пошук символу).")


if __name__ == "__main__":
    main()
