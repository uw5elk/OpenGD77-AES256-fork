#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Python-реалізація формату таблиці AES-ключів стокової TYT MD-UV390/MD-9600
(SPI-флеш, адреса STOCK_KEY_TABLE_BASE + 0x64*keyId) -- побайтова відповідність
логіці stockWrapKey()/stockUnwrapKey()/stockKeyWrite() у
application/source/crypto/dmr_aes_hook.c. Це і є формат, який прошивка ЗАРАЗ
реально читає для матеріалу ключа (див. коментар при STOCK_KEY_TABLE_BASE у
dmr_aes_hook.c) -- на відміну від старого "AESK"-блоку кастомних даних
(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS), який відтепер несе лише 1 байт: id
активного TX-ключа (те, що й далі пише aes_key_store.py --tx-key).

Навіщо саме такий формат: він ідентичний стоковій прошивці TYT, тому звичайний
CHIRP Read/Write кодплагу сам переносить ключі, і ключ переживає перепрошивку
fork<->стокова прошивка на тій самій рації -- жодного окремого механізму не
треба. Ключі keyA/keyB нижче -- ФІКСОВАНІ константи, однакові на кожній рації
(не похідні від UID), реверс-інжинірингом звірені побайтово з реальним
захопленням KEY1 (див. коментар у dmr_aes_hook.c).

Це чистий модуль без роботи з USB/серійним портом -- працює і тестується
окремо від рації (є юніт-тест нижче, offline round-trip: wrap(unwrap(x)) == x).
Читання/запис самої флеш-пам'яті через CPS-протокол -- у aes_key_store.py
(flash_read/flash_write_block), звідти й перевикористовуються.

ПОПЕРЕДЖЕННЯ: якщо колись зміниться stockWrapKey()/формат запису в
dmr_aes_hook.c -- цей файл треба оновити СИНХРОННО, інакше записаний звідси
ключ рація прочитає як сміття (чи як "порожній слот").
"""

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# ---- геометрія таблиці (мусить точно збігатись з dmr_aes_hook.c) -----------------
STOCK_KEY_TABLE_BASE = 0xD9F9C
STOCK_KEY_ENTRY_LEN = 0x64        # 100 байт
STOCK_KEY_TYPE_OFF = 0
STOCK_KEY_NAME_OFF = 4            # UTF-16LE, 32 байти
STOCK_KEY_KEY_OFF = 36            # загорнутий (wrapped) 32-байтовий ключ
STOCK_KEY_TYPE_AES256 = 0x04
STOCK_KEY_TYPE_AES256B = 0x07     # теж AES-256, за реверс-інжинірингом
STOCK_KEY_MIN_ID = 1
STOCK_KEY_MAX_ID = 15


def entry_addr(key_id):
    """Адреса запису для keyId у SPI-флеш (сирі адреси, як в SPI_Flash_read/write)."""
    if not (STOCK_KEY_MIN_ID <= key_id <= STOCK_KEY_MAX_ID):
        raise ValueError("keyId має бути 1..15, отримано {}".format(key_id))
    return STOCK_KEY_TABLE_BASE + STOCK_KEY_ENTRY_LEN * key_id


# ---- фіксовані ключі загортання (ідентичні на кожній рації) ----------------------
_KEY_A = bytes([
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01,
    0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45,
])
_KEY_B = bytes([
    0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45,
    0x15, 0x32, 0x3a, 0x3c, 0x3f, 0x48, 0x60, 0x62, 0x65, 0x66, 0x7f, 0x06, 0x07, 0x08, 0x09, 0x0a,
])

# Стоковий сентінел "порожнього слоту" (unwrap дає це значення) -- той самий, що
# й s_stockBlankKey у dmr_aes_hook.c: 31 нульовий байт + останній байт 0x01.
BLANK_KEY = bytes(31) + b"\x01"


def _aes256_ecb_block(key32, block16):
    """Один блок (16 байт) AES-256-ECB без доповнення (padding) -- точний
    аналог aes256_ecb_encrypt()/aes256_ecb_decrypt() з dmr_aes.c: прошивка
    оперує рівно одним 16-байтовим блоком за раз, тому режим ECB без
    доповнення тут коректний (ніякого потокового шифрування, лише один блок)."""
    assert len(key32) == 32 and len(block16) == 16
    return Cipher(algorithms.AES(key32), modes.ECB()).encryptor().update(block16)


def _aes256_ecb_block_decrypt(key32, block16):
    assert len(key32) == 32 and len(block16) == 16
    return Cipher(algorithms.AES(key32), modes.ECB()).decryptor().update(block16)


def wrap_key(key32):
    """stockWrapKey(): реверс байтів key32, тоді AES-256-ECB-ENCRYPT кожної
    16-байтової половини під keyA/keyB окремо. Повертає загорнутий 32-байтовий
    блок для запису в поле STOCK_KEY_KEY_OFF."""
    if len(key32) != 32:
        raise ValueError("ключ має бути рівно 32 байти (64 hex-символи)")
    reversed_key = key32[::-1]
    half1 = _aes256_ecb_block(_KEY_A, reversed_key[0:16])
    half2 = _aes256_ecb_block(_KEY_B, reversed_key[16:32])
    return half1 + half2


def unwrap_key(wrapped32):
    """stockUnwrapKey(): обернена операція wrap_key() -- ECB-DECRYPT половин
    під keyA/keyB, тоді реверс байтів назад."""
    if len(wrapped32) != 32:
        raise ValueError("загорнутий ключ має бути рівно 32 байти")
    half1 = _aes256_ecb_block_decrypt(_KEY_A, wrapped32[0:16])
    half2 = _aes256_ecb_block_decrypt(_KEY_B, wrapped32[16:32])
    return (half1 + half2)[::-1]


def is_key_present(key32):
    """stockKeyIsPresent(): слот вважається зайнятим, якщо ключ не весь нуль і
    не дорівнює стоковому сентінелу "порожнього слоту" (BLANK_KEY)."""
    return key32 != bytes(32) and key32 != BLANK_KEY


def default_name(key_id):
    """stockDefaultName(): назва слоту за замовчуванням -- десяткове число
    keyId, закодоване UTF-16LE у 32-байтовому полі (як робить стокова
    прошивка для нових слотів)."""
    text = str(key_id)
    name = bytearray(32)
    for i, ch in enumerate(text):
        name[i * 2] = ord(ch)
        name[i * 2 + 1] = 0
    return bytes(name)


def parse_entry(entry100):
    """Розбирає 100-байтовий запис таблиці: (type, name32, wrapped_key32)."""
    if len(entry100) != STOCK_KEY_ENTRY_LEN:
        raise ValueError("запис таблиці має бути рівно {} байт".format(STOCK_KEY_ENTRY_LEN))
    entry_type = entry100[STOCK_KEY_TYPE_OFF]
    name = entry100[STOCK_KEY_NAME_OFF:STOCK_KEY_NAME_OFF + 32]
    wrapped = entry100[STOCK_KEY_KEY_OFF:STOCK_KEY_KEY_OFF + 32]
    return entry_type, name, wrapped


def build_entry(key_id, key32, existing_entry100=None):
    """stockKeyWrite(): будує новий 100-байтовий запис для запису у флеш.
    Якщо existing_entry100 передано і має валідний тип (AES-256) з непорожньою
    назвою -- назва зберігається (як робить прошивка); інакше -- назва за
    замовчуванням ("1".."15" у UTF-16LE)."""
    have_name = False
    name = None
    if existing_entry100 is not None:
        old_type, old_name, _ = parse_entry(existing_entry100)
        if old_type in (STOCK_KEY_TYPE_AES256, STOCK_KEY_TYPE_AES256B) and old_name[0:1] not in (b"\x00", b"\xff"):
            have_name = True
            name = old_name

    entry = bytearray(STOCK_KEY_ENTRY_LEN)
    entry[STOCK_KEY_TYPE_OFF] = STOCK_KEY_TYPE_AES256
    entry[STOCK_KEY_NAME_OFF:STOCK_KEY_NAME_OFF + 32] = name if have_name else default_name(key_id)
    entry[STOCK_KEY_KEY_OFF:STOCK_KEY_KEY_OFF + 32] = wrap_key(key32)
    # решта (pad, 32 байти після ключа) лишається нулями, як і в прошивці.
    return bytes(entry)


def _self_test():
    """Офлайн перевірка узгодженості (без рації): round-trip wrap/unwrap для
    кількох ключів, і що BLANK_KEY через wrap/unwrap повертається сам собою.
    Побайтову звірку з реальною рацією (справжній wrapped-блок KEY1 з флешу)
    цей тест НЕ робить -- для цього потрібне залізо, якого тут немає."""
    import os

    for _ in range(50):
        k = os.urandom(32)
        w = wrap_key(k)
        assert len(w) == 32
        u = unwrap_key(w)
        assert u == k, "round-trip mismatch"
        assert is_key_present(k) is True

    assert unwrap_key(wrap_key(BLANK_KEY)) == BLANK_KEY
    assert is_key_present(BLANK_KEY) is False
    assert is_key_present(bytes(32)) is False

    e = build_entry(3, os.urandom(32))
    t, n, w = parse_entry(e)
    assert t == STOCK_KEY_TYPE_AES256
    assert n == default_name(3)

    e2 = build_entry(3, os.urandom(32), existing_entry100=e)
    t2, n2, _ = parse_entry(e2)
    assert n2 == n, "назва слоту мала зберегтись при перезаписі ключа"

    assert entry_addr(1) == 0xD9F9C + 0x64
    assert entry_addr(15) == 0xD9F9C + 0x64 * 15

    print("stock_key_table: усі офлайн-перевірки пройшли (round-trip, порожній слот, збереження назви).")


if __name__ == "__main__":
    _self_test()
