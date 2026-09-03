#!/usr/bin/env python3
"""rctl_config.py — записати/прочитати блок "RCTL" (allowlist віддаленого керування,
Radio Check і т.д. -- functions/dmr_rctl_cfg.c) через CPS-протокол по USB, аналогічно
aes_key_store.py, але через ПРАВИЛЬНИЙ, безпечний для сусідніх блоків читач/писар
custom_data.py (див. коментар там -- регіон спільний з темами/заставкою/повідомленнями,
тож "писати з початку" як робить aes_key_store.py тут НЕ можна).

За замовчуванням, поки прошивка не отримала жодного цього блоку -- keeps enabled=0,
allowlist порожній (fail closed, той самий стан, що й "фічі в збірці нема").

Формат payload (40 байт, дзеркалить dmrRctlOnFlashCfg_t у dmr_rctl_cfg.c):
    magic[4]="RCTL"  version=1  enabled  numAllowed  reserved=0  allowedId[8] (u32 LE)

Використання:
  python3 rctl_config.py --show                              # прочитати поточний стан
  python3 rctl_config.py --enable --allow 1234567 --allow 7654321   # увімкнути + список
  python3 rctl_config.py --disable                            # вимкнути (список лишається)
"""
import argparse, struct, sys
import aes_key_store as aks
import custom_data as cd

TYPE_RCTL_CONFIG = 9   # CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG (codeplug.h) -- 9-й елемент enum
MAX_ALLOWED = 8        # DMR_RCTL_MAX_ALLOWED (dmr_rctl_pdu.h)
PAYLOAD_LEN = 4 + 1 + 1 + 1 + 1 + MAX_ALLOWED * 4   # = 40


def build_payload(enabled, allowed_ids):
    if len(allowed_ids) > MAX_ALLOWED:
        raise ValueError("максимум %d ID у allowlist (DMR_RCTL_MAX_ALLOWED)" % MAX_ALLOWED)
    ids = list(allowed_ids) + [0] * (MAX_ALLOWED - len(allowed_ids))
    p = bytearray(PAYLOAD_LEN)
    p[0:4] = b"RCTL"
    p[4] = 1                       # version
    p[5] = 1 if enabled else 0
    p[6] = len(allowed_ids) & 0xFF
    p[7] = 0                       # reserved
    for i, aid in enumerate(ids):
        struct.pack_into("<I", p, 8 + i * 4, aid & 0xFFFFFFFF)
    return bytes(p)


def parse_payload(payload):
    if len(payload) < PAYLOAD_LEN or payload[0:4] != b"RCTL":
        return None
    enabled = payload[5] != 0
    numAllowed = payload[6]
    ids = [struct.unpack_from("<I", payload, 8 + i * 4)[0] for i in range(min(numAllowed, MAX_ALLOWED))]
    return {"version": payload[4], "enabled": enabled, "allowedId": ids}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--enable", action="store_true", help="увімкнути приймання RCTL-команд")
    ap.add_argument("--disable", action="store_true", help="вимкнути приймання RCTL-команд")
    ap.add_argument("--allow", action="append", type=int, default=None,
                     help="дозволений DMR ID видавця команд (до 8 разів); якщо не вказано -- список НЕ змінюється")
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
        print("поточний стан:", cur if cur else "блоку RCTL немає (вимкнено за замовчуванням, allowlist порожній)")

        if a.show:
            return

        if not (a.enable or a.disable or a.allow is not None):
            print("нічого не змінюю (передай --enable/--disable і/або --allow ...)")
            return

        enabled = cur["enabled"] if cur else False
        if a.enable: enabled = True
        if a.disable: enabled = False
        allowed = a.allow if a.allow is not None else (cur["allowedId"] if cur else [])

        payload = build_payload(enabled, allowed)
        ok, msg = cd.write_block(ser, TYPE_RCTL_CONFIG, payload)
        if not ok:
            sys.exit("ЗАПИС НЕ ВДАВСЯ: %s" % msg)
        print("записано:", msg)

        # звірка читанням назад
        rb = cd.read_block(ser, TYPE_RCTL_CONFIG, PAYLOAD_LEN)
        ok2 = (rb == payload)
        print("звірка читанням:", "OK" if ok2 else "НЕЗБІГ")
        if not ok2:
            print("  записано:", payload.hex())
            print("  прочитано:", (rb or b"").hex())


if __name__ == "__main__":
    main()
