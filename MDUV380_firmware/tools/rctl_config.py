#!/usr/bin/env python3
"""rctl_config.py — записати/прочитати блок "RCTL" (бінарний перемикач "приймати команди
віддаленого керування" -- functions/dmr_rctl_cfg.c) через CPS-протокол по USB, аналогічно
aes_key_store.py, але через ПРАВИЛЬНИЙ, безпечний для сусідніх блоків читач/писар
custom_data.py (див. коментар там -- регіон спільний з темами/заставкою/повідомленнями,
тож "писати з початку" як робить старий aes_key_store.py тут НЕ можна).

Модель довіри (ВИПРАВЛЕНО 2026-09-03, за прямою вказівкою користувача: "якщо ввімкнено --
можуть керувати всі, якщо ні -- то ніхто, так роблять і Motorola, і Hytera"): жодного
списку довірених ID тут немає -- лише один прапорець enabled. Увімкнено = приймає команди
від будь-кого з правильним AES-ключем каналу (тим самим, що й голос/SMS); вимкнено = не
приймає ні від кого. За замовчуванням, поки прошивка не отримала жодного цього блоку --
enabled=0 (fail closed, той самий стан, що й "фічі в збірці нема").

Формат payload (8 байт, дзеркалить dmrRctlOnFlashCfg_t у dmr_rctl_cfg.c):
    magic[4]="RCTL"  version=2  enabled  reserved[2]

Використання:
  python3 rctl_config.py --show       # прочитати поточний стан
  python3 rctl_config.py --enable     # дозволити приймати команди від будь-кого з ключем
  python3 rctl_config.py --disable    # заборонити приймати команди (default)
"""
import argparse, sys
import aes_key_store as aks
import custom_data as cd

TYPE_RCTL_CONFIG = 9   # CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG (codeplug.h) -- 9-й елемент enum
VERSION = 2             # 2026-09-03: без allowlist
PAYLOAD_LEN = 4 + 1 + 1 + 2   # = 8


def build_payload(enabled):
    p = bytearray(PAYLOAD_LEN)
    p[0:4] = b"RCTL"
    p[4] = VERSION
    p[5] = 1 if enabled else 0
    p[6] = 0   # reserved
    p[7] = 0   # reserved
    return bytes(p)


def parse_payload(payload):
    if len(payload) < PAYLOAD_LEN or payload[0:4] != b"RCTL":
        return None
    return {"version": payload[4], "enabled": payload[5] != 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--enable", action="store_true",
                     help="дозволити команди RCTL від БУДЬ-КОГО з правильним канальним ключем")
    ap.add_argument("--disable", action="store_true", help="заборонити приймання команд RCTL (default)")
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

        current = cd.read_block(ser, TYPE_RCTL_CONFIG, PAYLOAD_LEN)
        cur = parse_payload(current) if current else None
        print("поточний стан:", cur if cur else "блоку RCTL немає (вимкнено за замовчуванням)")

        if a.show:
            return

        if not (a.enable or a.disable):
            print("нічого не змінюю (передай --enable або --disable)")
            return

        payload = build_payload(a.enable)
        ok, msg = cd.write_block(ser, TYPE_RCTL_CONFIG, payload)
        if not ok:
            sys.exit("ЗАПИС НЕ ВДАВСЯ: %s" % msg)
        print("записано:", msg, "-> enabled =", a.enable)

        # звірка читанням назад
        rb = cd.read_block(ser, TYPE_RCTL_CONFIG, PAYLOAD_LEN)
        ok2 = (rb == payload)
        print("звірка читанням:", "OK" if ok2 else "НЕЗБІГ")
        if not ok2:
            print("  записано:", payload.hex())
            print("  прочитано:", (rb or b"").hex())


if __name__ == "__main__":
    main()
