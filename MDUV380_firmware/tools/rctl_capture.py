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

    if "--rxdiag" in sys.argv:
        # Лічильники прийому СТОКОВИХ команд (форк як ціль). --rxdiag-reset щоб обнулити.
        reset = 1 if "--rxdiag-reset" in sys.argv else 0
        ser.write(bytes([ord("C"), 0x9B, reset])); ser.flush(); time.sleep(0.3)
        r = ser.read(64)
        if len(r) < 3 or r[0] != ord("C"):
            sys.exit(f"несподівана відповідь: {r.hex()}")
        n = (r[1] << 8) | r[2]; body = r[3:3 + n]
        if len(body) < 24:
            sys.exit(f"замало даних: {body.hex()}")
        vals = struct.unpack_from("<6I", body, 0)
        names = ["впізнано команд (seen)", "останній командир (ID)",
                 "виконано Check", "виконано Monitor", "виконано Enable", "виконано Disable"]
        print("Лічильники прийому стокових команд (форк як ціль):")
        for k, v in zip(names, vals):
            print(f"  {k}: {v}")
        if vals[0] == 0:
            print("\n0 впізнано: стокова-командир ще не слала команду на цей форк,")
            print("або форк її не приймає (перевір канал/адресу).")
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
        tname = TYPE_NAMES.get(typ, f"тип{typ}")
        hexb = " ".join(f"{x:02x}" for x in data)
        print(f"  #{seq:<3} {tname:<9} {hexb}")

    print("\nСкопіюй увесь цей вивід і надішли — за ним робимо розбір формату.")


if __name__ == "__main__":
    main()
