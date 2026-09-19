#!/usr/bin/env python3
"""callreplay_config.py — записати/прочитати блок "CRPL" (бінарний перемикач "Запис RX"
для "Переслухати" -- functions/callReplayPlayback.c, callReplayConfigLoad()/Save()) через
CPS-протокол по USB, тим самим custom_data.py, що й rctl_config.py.

ЧОМУ ОКРЕМИЙ custom-data блок, а не біт у settings.bitfieldOptions (як спершу
розглядалось): великий коментар біля BIT_UNUSED_1 у application/include/functions/settings.h
і біля callReplayConfigLoad()/Save() у application/include/functions/callReplay.h. Коротко --
CPS-запис "EEPROM" (usb_com.c, cpsHandleWriteCommand case 4) -- порожній no-op на STM32-
платформах (MDUV380 і подібні), тож біт у nonVolatileSettings звідси не записати; custom-data
блок -- той самий регіон і генерик API, що вже безпечно використовує RCTL.

Формат payload (6 байт, дзеркалить callReplayOnFlashCfg_t у callReplayPlayback.c):
    magic[4]="CRPL"  version=1  enabled

enabled -- ПРЯМА полярність (0=вимкнено, 1=увімкнено), на відміну від відкинутого
інвертованого біта: тут інверсія не потрібна, бо ВІДСУТНІСТЬ блоку (нова/нечіпана рація)
сама по собі читається прошивкою як типове значення (Увімкнено) -- callReplayConfigLoad()
нічого не пише, якщо блоку немає.

Використання:
  python3 callreplay_config.py --show       # прочитати поточний стан
  python3 callreplay_config.py --enable     # увімкнути запис RX
  python3 callreplay_config.py --disable    # вимкнути запис RX
"""
import argparse, sys
import aes_key_store as aks
import custom_data as cd

TYPE_CALL_REPLAY_CONFIG = 11  # CODEPLUG_CUSTOM_DATA_TYPE_CALL_REPLAY_CONFIG (codeplug.h) -- 11-й елемент enum
VERSION = 1
PAYLOAD_LEN = 4 + 1 + 1  # = 6


def build_payload(enabled):
    p = bytearray(PAYLOAD_LEN)
    p[0:4] = b"CRPL"
    p[4] = VERSION
    p[5] = 1 if enabled else 0
    return bytes(p)


def parse_payload(payload):
    if len(payload) < PAYLOAD_LEN or payload[0:4] != b"CRPL":
        return None
    return {"version": payload[4], "enabled": (payload[5] != 0)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--enable", action="store_true", help="увімкнути запис RX")
    ap.add_argument("--disable", action="store_true", help="вимкнути запис RX")
    ap.add_argument("--show", action="store_true", help="лише прочитати поточний стан")
    ap.add_argument("--port", default=None)
    a = ap.parse_args()

    if a.enable and a.disable:
        sys.exit("--enable і --disable разом не мають сенсу")

    port = a.port or aks.find_port()
    if not port:
        sys.exit("рацію не знайдено (1fc9:0094); передай --port")

    with aks.serial.Serial(port, 115200, timeout=0.6) as ser:
        aks.show_cps(ser)

        current = cd.read_block(ser, TYPE_CALL_REPLAY_CONFIG, PAYLOAD_LEN)
        cur = parse_payload(current) if current else None
        print("поточний стан:", cur if cur else "блоку немає (типово -- запис УВІМКНЕНО)")

        if a.show:
            return

        if not (a.enable or a.disable):
            print("нічого не змінюю (передай --enable або --disable)")
            return

        enabled = bool(a.enable)
        payload = build_payload(enabled)
        ok, msg = cd.write_block(ser, TYPE_CALL_REPLAY_CONFIG, payload)
        if not ok:
            sys.exit("ЗАПИС НЕ ВДАВСЯ: %s" % msg)
        print("записано:", msg, "-> Запис RX =", ("увімкнено" if enabled else "вимкнено"))

        # звірка читанням назад
        rb = cd.read_block(ser, TYPE_CALL_REPLAY_CONFIG, PAYLOAD_LEN)
        ok2 = (rb == payload)
        print("звірка читанням:", "OK" if ok2 else "НЕЗБІГ")
        if not ok2:
            print("  записано:", payload.hex())
            print("  прочитано:", (rb or b"").hex())


if __name__ == "__main__":
    main()
