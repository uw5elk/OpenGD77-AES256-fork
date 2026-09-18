#!/usr/bin/env python3
"""rctl_config.py — записати/прочитати блок "RCTL" (бінарний перемикач "приймати команди
віддаленого керування" -- functions/dmr_rctl_cfg.c) через CPS-протокол по USB, аналогічно
aes_key_store.py, але через ПРАВИЛЬНИЙ, безпечний для сусідніх блоків читач/писар
custom_data.py (див. коментар там -- регіон спільний з темами/заставкою/повідомленнями,
тож "писати з початку" як робить старий aes_key_store.py тут НЕ можна).

Модель довіри (ВИПРАВЛЕНО 2026-09-03, за прямою вказівкою користувача: "якщо ввімкнено --
можуть керувати всі, якщо ні -- то ніхто, так роблять і Motorola, і Hytera"): жодного
списку довірених ID тут немає -- лише той самий AES-ключ каналу (що й голос/SMS) як межа.

ЄДИНА ТОЧКА ПРАВДИ = МАСКА (2026-09-18). Раніше був окремий байт enabled, який міг
розійтися з маскою (прошивальник лишав enabled=0, а меню рації показувало "все On").
Тепер enabled ПОХІДНИЙ від маски: непорожня маска = RCTL увімкнено, порожня = вимкнено.
build_payload() виставляє байт enabled сам, рівно як прошивка (dmrRctlConfigSetAllow).
За замовчуванням, поки блоку немає -- маска 0 (fail closed, як "фічі в збірці нема").

Формат payload (8 байт, дзеркалить dmrRctlOnFlashCfg_t у dmr_rctl_cfg.c):
    magic[4]="RCTL"  version=5  enabled(похідний)  allow  monitorSecs

allow -- ОДИН байт, дві незалежні половини (dmr_rctl_cfg.c/.h):
  ловер-нібл (0x0F)  -- бітова маска ДОЗВОЛЕНИХ КОМАНД (2026-09-05, за зразком
                        Motorola/Hytera: радіоперевірка/прослуховування/стан вмикаються
                        незалежно). Блок версії 2 прошивка читає як "лише радіоперевірка".
  аппер-нібл (0xF0)  -- ВИГЛЯД (версія 5): біт 0x10 = сховати пункт «Доступ RCTL» в
                        меню Опцій на рації. НЕ окреме поле -- довжину вже існуючого
                        блоку в custom-data не можна змінити (і прошивка, і custom_data.py
                        тут відмовляють), а ця половина байта на будь-якому блоці версій
                        <5 завжди була нульовою -- тож "видимий" читається й без міграції.

monitorSecs (байт 7, версія 4) -- тривалість відповіді на Monitor; 0 = типова (30 с).

Використання:
  python3 rctl_config.py --show               # прочитати поточний стан
  python3 rctl_config.py --allow check,monitor # дозволити конкретні команди (маска)
  python3 rctl_config.py --enable             # увімкнути (лише радіоперевірка, якщо не було)
  python3 rctl_config.py --disable            # вимкнути RCTL (маска = 0)
  python3 rctl_config.py --hide-menu          # сховати пункт «Доступ RCTL» в меню Опцій
  python3 rctl_config.py --show-menu          # показати пункт «Доступ RCTL» в меню Опцій
"""
import argparse, sys
import aes_key_store as aks
import custom_data as cd

TYPE_RCTL_CONFIG = 9   # CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG (codeplug.h) -- 9-й елемент enum
VERSION = 5              # 2026-09-18: додано вигляд меню (аппер-нібл allow), розмір блока той самий
PAYLOAD_LEN = 4 + 1 + 1 + 1 + 1   # = 8


ALLOW_CHECK   = 1 << 0
ALLOW_MONITOR = 1 << 1
ALLOW_STUN    = 1 << 2
ALLOW_REVIVE  = 1 << 3
ALLOW_ALL     = ALLOW_CHECK | ALLOW_MONITOR | ALLOW_STUN | ALLOW_REVIVE

ALLOW_NAMES = [("check", ALLOW_CHECK), ("monitor", ALLOW_MONITOR),
               ("stun", ALLOW_STUN), ("revive", ALLOW_REVIVE)]

UI_HIDE_ACCESS_MENU = 0x10   # DMR_RCTL_UI_HIDE_ACCESS_MENU -- та сама верхня половина allow


def build_payload(allow, monitor_secs=0, menu_hidden=False):
    """2026-09-18: enabled БІЛЬШЕ НЕ окремий параметр -- він похідний від маски
    (enabled = (allow & ALLOW_ALL) != 0), рівно як у прошивці (dmrRctlConfigSetAllow).
    Порожня маска = RCTL вимкнено. Так поля у флеші не можуть розійтися."""
    allow &= ALLOW_ALL
    p = bytearray(PAYLOAD_LEN)
    p[0:4] = b"RCTL"
    p[4] = VERSION
    p[5] = 1 if allow != 0 else 0                    # enabled похідний від маски
    p[6] = allow | (UI_HIDE_ACCESS_MENU if menu_hidden else 0)
    p[7] = monitor_secs & 0xFF
    return bytes(p)


def parse_payload(payload):
    if len(payload) < PAYLOAD_LEN or payload[0:4] != b"RCTL":
        return None
    version = payload[4]
    raw = (payload[6] if version >= 3 else ALLOW_CHECK) & ALLOW_ALL
    enabled = payload[5] != 0
    # ЕФЕКТИВНА маска -- те, що рація реально виконує (гейт: спершу enabled, потім біт).
    # На legacy-блоці enabled=0 при raw!=0 ефективна маска = 0 (команди не приймаються,
    # доки не буде свідомого запису). Прошивальник показує саме ефективну.
    effective = raw if enabled else 0
    menu_hidden = bool(version >= 5 and (payload[6] & UI_HIDE_ACCESS_MENU))
    monitor_secs = payload[7] if version >= 4 else 0
    return {"version": version, "enabled": enabled,
            "allow": effective,                        # ефективна (для галочок/меню)
            "allow_raw": raw,                          # сира у флеші (діагностика)
            "allow_names": [n for n, b in ALLOW_NAMES if effective & b] or ["-"],
            "menu_hidden": menu_hidden, "monitor_secs": monitor_secs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--enable", action="store_true",
                     help="увімкнути RCTL (маска: наявна, або лише радіоперевірка, якщо блоку не було)")
    ap.add_argument("--disable", action="store_true",
                     help="вимкнути RCTL -- очистити маску (enabled стає 0 автоматично)")
    ap.add_argument("--show", action="store_true", help="лише прочитати поточний стан")
    ap.add_argument("--allow", default=None,
                     help="які команди дозволити, через кому: check,monitor,stun,revive або all/none. "
                          "Не вказано -- лишити як є (при --enable на порожньому блоці: лише check)")
    ap.add_argument("--hide-menu", action="store_true",
                     help="сховати пункт «Доступ RCTL» в меню Опцій на рації (лише вигляд -- "
                          "дозволи команд працюють незалежно)")
    ap.add_argument("--show-menu", action="store_true", help="показати пункт «Доступ RCTL» в меню Опцій")
    ap.add_argument("--port", default=None)
    a = ap.parse_args()

    if a.enable and a.disable:
        sys.exit("--enable і --disable разом не мають сенсу")
    if a.hide_menu and a.show_menu:
        sys.exit("--hide-menu і --show-menu разом не мають сенсу")

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

        if not (a.enable or a.disable or a.hide_menu or a.show_menu or a.allow is not None):
            print("нічого не змінюю (передай --enable/--disable, --allow <...> "
                  "і/або --hide-menu/--show-menu)")
            return

        # 2026-09-18: enabled БІЛЬШЕ НЕ окремий стан -- єдина точка правди це маска дозволів,
        # а enabled виводиться з неї (порожня маска = вимкнено). Тож --enable/--disable тут
        # лише зручні синоніми операцій над МАСКОЮ, а не окремий байт:
        #   --disable          -> маска = 0 (RCTL вимкнено)
        #   --allow <...>       -> маска = задане
        #   --enable без --allow-> лишити наявну маску, а якщо її нема -- лише радіоперевірка
        #                          (не роздаємо прав, яких ніхто не просив)
        # cur["allow"] -- ЕФЕКТИВНА маска (0, якщо на legacy-блоці enabled=0): пишемо саме її,
        # тож прихований дозвіл на "вимкненій" рації не воскресає (див. міграцію T20 у тесті).
        if a.disable:
            allow = 0
        elif a.allow is not None:
            txt = a.allow.strip().lower()
            if txt in ("all", "усі", "все"):
                allow = ALLOW_ALL
            elif txt in ("none", "-", ""):
                allow = 0
            else:
                allow = 0
                known = dict(ALLOW_NAMES)
                for part in (x.strip() for x in txt.split(",") if x.strip()):
                    if part not in known:
                        ap.error("невідома команда в --allow: %r (можна: %s, all, none)"
                                 % (part, ", ".join(n for n, _ in ALLOW_NAMES)))
                    allow |= known[part]
        elif a.enable:
            allow = (cur["allow"] if (cur and cur["allow"]) else ALLOW_CHECK)
        else:
            # лише --hide-menu/--show-menu -> маску не чіпаємо (беремо ефективну наявну)
            allow = cur["allow"] if cur else 0

        # вигляд меню: явно задане -> воно; інакше лишаємо як є (типово -- видимий).
        if a.hide_menu:
            menu_hidden = True
        elif a.show_menu:
            menu_hidden = False
        else:
            menu_hidden = cur["menu_hidden"] if cur else False

        # тривалість Monitor: тут не змінюємо -- лишаємо як є (0 = типова).
        monitor_secs = cur["monitor_secs"] if cur else 0

        payload = build_payload(allow, monitor_secs, menu_hidden)   # enabled похідний від маски
        ok, msg = cd.write_block(ser, TYPE_RCTL_CONFIG, payload)
        if not ok:
            sys.exit("ЗАПИС НЕ ВДАВСЯ: %s" % msg)
        print("записано:", msg,
              "-> RCTL =", ("увімкнено" if (allow & ALLOW_ALL) else "вимкнено"),
              ", дозволи =", ",".join(n for n, b in ALLOW_NAMES if allow & b) or "-",
              ", пункт меню =", ("схований" if menu_hidden else "видимий"))

        # звірка читанням назад
        rb = cd.read_block(ser, TYPE_RCTL_CONFIG, PAYLOAD_LEN)
        ok2 = (rb == payload)
        print("звірка читанням:", "OK" if ok2 else "НЕЗБІГ")
        if not ok2:
            print("  записано:", payload.hex())
            print("  прочитано:", (rb or b"").hex())


if __name__ == "__main__":
    main()
