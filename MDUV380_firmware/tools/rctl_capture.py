#!/usr/bin/env python3
"""Захоплення сирих DMR data-burst із форк-рації по USB — для реверсу стокового протоколу
керування (RCTL_COMPAT.md, крок 2.1).

НАВІЩО. Стокова TYT MD-UV3x0 має Radio Check / Remote Mon. / Radio Enable / Radio Disable.
Щоб зробити наш формат сумісним, треба СПОЧАТКУ побачити точні байти, які стокова шле в
ефір. Форк-рація (RX) ловить їх у кільце, цей скрипт їх викачує й друкує в hex.

USB-команди (ті самі, що sms_diag.py, через 'C'-канал CPS):
  0x98 [arm]   — 1 почати ловити / 0 зупинити
  0x99         — викласти захоплене
  0x9A         — скинути кільце

ТИПОВИЙ СЕАНС РЕВЕРСУ:
  1. Форк-рація (ця) і стокова TYT на СПІЛЬНОМУ каналі (частота/TS/CC/TG), мінімальна
     потужність, рації поруч. Канал тихий (жодного іншого DMR-трафіку).
  2.  python3 rctl_capture.py --arm
  3. На СТОКОВІЙ TYT зробити ОДНУ дію (напр. Radio Check) на DMR ID форк-рації.
  4.  python3 rctl_capture.py            # надрукує захоплені burst
  5. Повторити для Remote Mon. / Radio Enable / Radio Disable, щоразу --arm перед дією.
  6. Надіслати роздруківки — за ними робимо байт-у-байт розбір формату.

Запуск із Windows або WSL; автовизначення рації OpenGD77 (1FC9:0094)."""
import sys, time, struct
import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094
BURST_LEN = 12

# Людські назви data-типів DMR (rxDataType з reg 0x51 [7:4]).
TYPE_NAMES = {
    0: "PI-hdr", 1: "VLC-hdr", 2: "TLC", 3: "CSBK",
    4: "MBC-hdr", 5: "MBC-cont", 6: "data-hdr", 7: "rate-1/2",
    8: "rate-3/4", 10: "rate-1", 13: "IdleFill",
}


def find_port():
    for p in list_ports.comports():
        if p.vid == APP_VID and p.pid == APP_PID:
            return p.device
    return None


def main():
    port = find_port()
    if not port:
        sys.exit("рацію не знайдено (USB 1FC9:0094) — увімкнена, в звичайному режимі, кабель?")
    ser = serial.Serial(port, 115200, timeout=0.5)

    if "--arm" in sys.argv:
        ser.write(bytes([ord("C"), 0x98, 0x01])); ser.flush(); time.sleep(0.2)
        r = ser.read(8)
        ok = len(r) >= 2 and r[0] == ord("C") and r[1] == 1
        print("захоплення УВІМКНЕНО" if ok else f"не вдалось увімкнути: {r.hex()}")
        print("→ тепер зроби ОДНУ дію на стоковій TYT, потім запусти без аргументів")
        return

    if "--disarm" in sys.argv:
        ser.write(bytes([ord("C"), 0x98, 0x00])); ser.flush(); time.sleep(0.2)
        ser.read(8)
        print("захоплення вимкнено")
        return

    if "--reset" in sys.argv:
        ser.write(bytes([ord("C"), 0x9A])); ser.flush(); time.sleep(0.2)
        ser.read(8)
        print("кільце очищено")
        return

    if "--rxdiag" in sys.argv or "--unlock" in sys.argv:
        # Лічильники прийому СТОКОВИХ команд (форк як ціль). Прапорці в байті [2]:
        #   біт0 (--rxdiag-reset) — обнулити лічильники;
        #   біт1 (--unlock)       — зняти блокування кабелем (recovery, якщо рацію
        #                           заблокувала стокова командою Disable по ефіру).
        flags = 0
        if "--rxdiag-reset" in sys.argv: flags |= 0x01
        if "--unlock" in sys.argv:       flags |= 0x02
        ser.write(bytes([ord("C"), 0x9B, flags])); ser.flush(); time.sleep(0.3)
        # Спочатку заголовок (3 байти), потім РІВНО стільки байтів, скільки
        # оголошено. Раніше тут було ser.read(64) -- і коли прошивка почала
        # віддавати 16 лічильників (3 + 64 = 67 байтів), останній тихо губився
        # і версія прошивки визначалась невірно.
        r = ser.read(3)
        if len(r) < 3 or r[0] != ord("C"):
            sys.exit(f"несподівана відповідь: {r.hex()}")
        n = (r[1] << 8) | r[2]
        body = ser.read(n) if n else b""
        if len(body) < 28:
            sys.exit(f"замало даних: {body.hex()}")
        # Стара прошивка віддає 7 лічильників, нова -- 10 (додано діагностику
        # шляху квитанції). Читаємо скільки є, щоб інструмент працював з обома.
        # Скільки лічильників віддала прошивка -- беремо з довжини відповіді, а не
        # зі списку відомих версій: інакше проміжна версія тихо з'їдає цілий розділ.
        nvals = min(len(body) // 4, 23)
        print(f"(прошивка віддала {nvals} лічильників)\n")
        vals = struct.unpack_from(f"<{nvals}I", body, 0)
        names = ["впізнано команд (seen)", "останній командир (ID)",
                 "виконано Check", "виконано Monitor", "виконано Enable", "виконано Disable"]
        print("Лічильники прийому стокових команд (форк як ціль):")
        for k, v in zip(names, vals[:6]):
            print(f"  {k}: {v}")
        print(f"  СТАН БЛОКУВАННЯ (inhibited): {'ТАК — рацію заблоковано' if vals[6] else 'ні'}")

        if nvals >= 10:
            csbk, ackseen, ackus = vals[7], vals[8], vals[9]
            print("\nШлях квитанції (форк як КОМАНДИР, після власного Radio Check):")
            print(f"  CSBK-бургстів прийнято: {csbk}")
            print(f"  з них розібрано як квитанцію: {ackseen}")
            print(f"  з них адресовано нам: {ackus}")
            if csbk == 0:
                print("  → ЕФІР НЕ ЧУЄМО взагалі. Рація не встигає повернутись у прийом")
                print("    або квитанції в ефірі немає (перевір канал/ціль).")
            elif ackseen == 0:
                print("  → Чуємо CSBK, але це не квитанція (інший формат або биті помилки).")
            elif ackus == 0:
                print("  → Квитанція є, але адресована не нам — звір DMR ID цієї рації.")
            else:
                print("  → Квитанцію прийнято. Якщо на екрані все одно хрест — вона прийшла")
                print("    пізніше за вікно очікування (4 с).")

        if nvals >= 15:
            wany, wdata, wfirst, winfo, txend = vals[10], vals[11], vals[12], vals[13], vals[14]
            itot = vals[15] if nvals >= 16 else None
            if itot is None:
                print("\n(ця прошивка ще без безумовного лічильника переривань)")
            else:
                print(f"\nПереривань \"прийнято дані\" від обнулення (безумовно): {itot}")
            if itot == 0:
                print("  УВАГА: нуль означає, що рація не чула ЖОДНОГО DMR-сигналу")
                print("  від обнулення — ні чужого, ні контрольного. Спершу перевір це.")
        if nvals >= 19:
            print(f"\nКоварт-монітор (форк як ціль):")
            print(f"  ключувань мікрофона (monStarted): {vals[17]}")
            print(f"  пропущено (зайнято/не цифровий): {vals[18]}")
            if nvals >= 20:
                print(f"  тривалість останнього монітору: {vals[19]} мс  (має бути ≈ виставлена тривалість)")
            if nvals >= 23:
                print(f"  інтервал повтору команди стоковою: {vals[20]} мс  <-- ЦЕ ВІКНО")
                print(f"  голос стартував через: {vals[21]} мс після команди")
                if vals[20] and vals[21]:
                    if vals[21] < vals[20]:
                        print("    => голос ВСТИГАЄ у вікно -> якщо помилка лишається, це ФОРМАТ, не таймінг")
                    else:
                        print("    => голос ПІЗНІШЕ за вікно -> треба пришвидшити (менше повторів квитанції)")
            print("\nВікно після ВЛАСНОЇ передачі (15 с), до будь-яких фільтрів:")
            print(f"  завершення передачі зайняло: {txend} мс")
            print(f"  переривань усього: {wany}")
            if nvals >= 17:
                wfa = vals[16]
                if wfa:
                    print(f"  ПЕРШЕ переривання (будь-яке) через: {wfa} мс  <-- ЦЕ МІРА ГЛУХОТИ")
                else:
                    print("  ПЕРШЕ переривання: не було взагалі")
            print(f"  з них data-синхронізація: {wdata}")
            if wfirst:
                names = {0:"PI-hdr",1:"VLC-hdr",2:"TLC",3:"CSBK",4:"MBC-hdr",5:"MBC-cont",6:"data-hdr",7:"rate-1/2",8:"rate-3/4",10:"rate-1",13:"IdleFill"}
                t = winfo & 0x0F
                print(f"  перший data-бургст через: {wfirst} мс")
                print(f"    тип: {t} ({names.get(t, '?')}), "
                      f"CRC: {'ок' if winfo & 0x100 else 'БИТИЙ'}, "
                      f"передача ще активна: {'ТАК' if winfo & 0x200 else 'ні'}")
            else:
                print("  перший data-бургст: НЕ БУЛО ВЗАГАЛІ")
            if wany == 0:
                print("  → Після власної передачі чіп не дав ЖОДНОГО переривання 15 с.")
                print("    Це не фільтр і не формат — рація справді глуха (або в ефірі тиша).")
            elif wdata and not wfirst:
                pass
        if "--unlock" in sys.argv:
            print("\n→ подано кабельну команду розблокування (recovery).")
            print("  Якщо стан вище ще 'ТАК' — повтори; має стати 'ні'.")
        if vals[0] == 0:
            print("\n0 впізнано: стокова-командир ще не слала команду на цей форк,")
            print("або форк її не приймає (перевір канал/адресу).")
        return

    if "--rctl-status" in sys.argv or "--rctl-on" in sys.argv or "--rctl-off" in sys.argv:
        # Дозволи RCTL (доступ + маска команд) прямо з ПК -- щоб після кожної прошивки
        # відновлювати їх однією командою, не лазячи в меню рації.
        #   --rctl-status          — лише показати поточний стан
        #   --rctl-on [маска]      — увімкнути доступ; маска (hex/dec) бітів дозволених команд,
        #                            за замовчуванням усі 4 (0x0F: Check|Monitor|Disable|Enable)
        #   --rctl-off             — вимкнути доступ (рація перестає приймати команди)
        write = None
        if "--rctl-on" in sys.argv:
            mask = 0x0F
            i = sys.argv.index("--rctl-on")
            if i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith("-"):
                mask = int(sys.argv[i + 1], 0) & 0x0F
            write = (1, mask)
        elif "--rctl-off" in sys.argv:
            write = (0, 0)

        if write is not None:
            ser.write(bytes([ord("C"), 0x9C, 0x01, write[0], write[1]]))
        else:
            ser.write(bytes([ord("C"), 0x9C, 0x00]))
        ser.flush(); time.sleep(0.3)
        r = ser.read(16)
        if len(r) < 5 or r[0] != ord("C"):
            sys.exit(f"несподівана відповідь: {r.hex()}")
        n = (r[1] << 8) | r[2]; body = r[3:3 + n]
        if len(body) < 2:
            sys.exit(f"замало даних: {body.hex()}")
        enabled, allow = body[0], body[1]
        bits = [("Radio Check", 0x01), ("Monitor", 0x02),
                ("Disable (сон)", 0x04), ("Enable (пробудження)", 0x08)]
        print(f"Доступ RCTL: {'УВІМКНЕНО' if enabled else 'вимкнено'}")
        print("Дозволені команди:")
        for nm, bit in bits:
            print(f"  {nm}: {'так' if (allow & bit) else 'ні'}")
        if write is not None:
            print("\n→ записано у флеш рації (переживе перезавантаження; злітає лише при прошивці).")
        return

    # За замовчуванням — викачати й надрукувати.
    ser.write(bytes([ord("C"), 0x99])); ser.flush(); time.sleep(0.3)
    r = ser.read(4096)
    if len(r) < 3 or r[0] != ord("C"):
        sys.exit(f"несподівана відповідь: {r.hex()}")
    n = (r[1] << 8) | r[2]
    body = r[3:3 + n]
    if len(body) < 4:
        sys.exit(f"замало даних ({len(body)} Б): {body.hex()}")

    count, drop_hi, drop_lo, armed = body[0], body[1], body[2], body[3]
    dropped = (drop_hi << 8) | drop_lo
    print(f"захоплено burst: {count}   відкинуто (кільце повне): {dropped}   armed: {armed}")
    if count == 0:
        print("\nНІЧОГО НЕ ЗАХОПЛЕНО. Можливі причини:")
        print("  • не викликано --arm перед дією на стоковій;")
        print("  • рації на різних каналах (частота/TS/колір-код/TG);")
        print("  • стокова шле це НЕ як data-burst (тоді шукати іншим шляхом).")
        print("  Підказка: спершу перевір лічильники sms_diag.py — чи 'd' узагалі росте.")
        return

    off = 4
    print()
    for i in range(count):
        if off + 2 + BURST_LEN > len(body):
            print("  (дамп обірвано — буфер USB)")
            break
        seq = body[off]; typ = body[off + 1]
        data = body[off + 2:off + 2 + BURST_LEN]
        off += 2 + BURST_LEN
        crcbad = (typ & 0x80) != 0          # старший біт type = CRC невалідний
        typ &= 0x0F
        tname = TYPE_NAMES.get(typ, f"тип{typ}")
        hexb = " ".join(f"{x:02x}" for x in data)
        mark = "  CRC!" if crcbad else ""    # burst із «поганим» CRC (напр. відповідь цілі)
        print(f"  #{seq:<3} {tname:<9} {hexb}{mark}")

    print("\nПозначка 'CRC!' — burst, який рація прийняла, але з невалідним CRC (нам якраз цікавий).")
    print("Скопіюй увесь цей вивід і надішли — за ним робимо розбір формату.")


if __name__ == "__main__":
    main()
