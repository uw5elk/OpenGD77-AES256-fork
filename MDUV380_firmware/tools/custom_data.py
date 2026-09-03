#!/usr/bin/env python3
"""custom_data.py — правильний (не спрощений) читач/писар "OpenGD77" custom-data
регіону в SPI-флеші (FLASH_ADDRESS_OFFSET=0x20000..+0x10000, тобто 64 КБ), який ЦІЛИЙ
розділяється між БАГАТЬМА типами блоків одночасно: зображення заставки, тема дня/ночі,
AES-ключі (лише селектор), повідомлення+пресети SMS, і тепер RCTL-конфіг.

Побайтова Python-копія алгоритму прошивки (codeplug.c):
  codeplugGetOpenGD77CustomDataStartAddressForType() — послідовне сканування заголовків
    [dataType:int32][dataLength:int32] від зміщення 12, доки не знайде потрібний тип
    АБО не натрапить на порожній ("0xFFFFFFFF"/EMPTY) слот;
  codeplugGetOpenGD77CustomDataFirstEmptySlot() — перший порожній слот, з перевіркою,
    що новий блок влізе до MAX_BLOCK_ADDRESS (0x10000);
  codeplugSetOpenGD77CustomData() — записує лише В ІСНУЮЧИЙ блок ТІЄЇ Ж довжини, або
    в перший порожній слот; довжину існуючого блоку міняти НЕ можна.

*** Чому цей файл існує окремо, а не просто копіює підхід aes_key_store.py: ***
Оригінальний aes_key_store.py записує свій AESK-блок БЕЗУМОВНО за адресою FLASH_BASE
(тобто вважає, що це ЄДИНИЙ або ПЕРШИЙ блок у регіоні) — це працювало, бо на практиці
AES-блок майже завжди був першим записаним. Але регіон реально спільний з темами,
заставкою, повідомленнями тощо; сліпе записування "з початку" могло б ЗАТЕРТИ будь-який
блок, що фізично лежить після точки запису. Для RCTL-конфігу (майже напевно НЕ перший
блок на вже налаштованій рації — теми/повідомлення там, скоріш за все, вже є) так робити
не можна. Тому: правильний scan-find-or-append + запис, що зачіпає ЛИШЕ адреси свого
блоку (через sector-safe flash_write_span, що коректно розбиває запис по МЕЖАХ 4КБ
секторів -- на відміну від flash_write_block(), який готує лише ОДИН сектор і не
призначений для запису, що перетинає межу).
"""
import struct
import aes_key_store as aks

FLASH_BASE = aks.FLASH_BASE          # 0x20000 (FLASH_ADDRESS_OFFSET)
MAX_BLOCK_ADDRESS = 0x10000          # той самий ліміт, що й MAX_BLOCK_ADDRESS у codeplug.c
HDR_LEN = 8                          # sizeof(codeplugCustomDataBlockHeader_t): int32+int32
REGION_MAGIC = b"OpenGD77"

TYPE_EMPTY = -1  # CODEPLUG_CUSTOM_DATA_TYPE_EMPTY (0xFFFFFFFF) - як int32 зі знаком


def _read_region(ser, addr, length):
    """addr тут -- зміщення ВІД FLASH_BASE (як у прошивці), не абсолютна адреса."""
    return aks.flash_read(ser, FLASH_BASE + addr, length)


def find_or_alloc_block(ser, data_type, new_len):
    """Побайтова копія двох функцій codeplug.c разом. Повертає (kind, addr, existLen):
      ("existing", addr, existLen) -- блок цього типу вже є (existLen -- його поточна
          довжина; якщо existLen != new_len, ЗАПИС ЗАБОРОНЕНО -- прошивка теж це не
          дозволяє, довжину блоку міняти не можна);
      ("empty", addr, None)        -- вільний слот знайдено, є місце для new_len байт;
      (None, None, None)           -- регіон не в форматі "OpenGD77", або місця немає.
    """
    magic = _read_region(ser, 0, 12)
    if magic[:8] != REGION_MAGIC:
        return (None, None, None)

    addr = 12
    while addr < MAX_BLOCK_ADDRESS:
        hdr = _read_region(ser, addr, HDR_LEN)
        dtype, dlen = struct.unpack("<ii", hdr)
        if dtype == data_type:
            return ("existing", addr, dlen)
        if dlen == 0 or dlen == TYPE_EMPTY:
            if (addr + HDR_LEN + new_len) < MAX_BLOCK_ADDRESS:
                return ("empty", addr, None)
            return (None, None, None)   # порожній слот є, але новий блок туди не влізе
        addr += HDR_LEN + dlen
    return (None, None, None)


def flash_write_span(ser, addr, data):
    """Sector-safe запис: розбиває [addr, addr+len(data)) по межах 4КБ-секторів і
    викликає aes_key_store.flash_write_block() окремо на кожен сектор -- та функція
    готує/стирає РІВНО ОДИН сектор і не призначена для запису, що його перетинає.
    addr тут -- АБСОЛЮТНА адреса SPI-флешу (вже з урахуванням FLASH_BASE)."""
    pos = 0
    n = len(data)
    while pos < n:
        sector_base = ((addr + pos) // 4096) * 4096
        sector_end = sector_base + 4096
        room_in_sector = sector_end - (addr + pos)
        chunk_len = min(n - pos, room_in_sector)
        chunk = data[pos: pos + chunk_len]
        aks.flash_write_block(ser, addr + pos, chunk)
        pos += chunk_len


def write_block(ser, data_type, payload):
    """Записати/оновити один custom-data блок типу data_type з вмістом payload.
    Дозволяє лише запис ТІЄЇ Ж довжини в існуючий блок (як і прошивка) або створення
    нового блоку в першому вільному слоті. Повертає (True, "") або (False, "причина")."""
    new_len = len(payload)
    kind, addr, existLen = find_or_alloc_block(ser, data_type, new_len)
    if kind is None:
        return False, "регіон не в форматі OpenGD77, або немає вільного місця в custom-data"
    if kind == "existing" and existLen != new_len:
        return False, ("існуючий блок типу %d має іншу довжину (%d, а не %d) -- "
                        "прошивка так само забороняє міняти довжину блоку" % (data_type, existLen, new_len))

    header = struct.pack("<ii", data_type, new_len)
    flash_write_span(ser, FLASH_BASE + addr, header + payload)
    return True, ("оновлено існуючий блок @+%d" % addr) if kind == "existing" else ("створено новий блок @+%d" % addr)


def read_block(ser, data_type, max_len):
    """Прочитати вміст блоку (без заголовка) або None, якщо блоку немає."""
    kind, addr, existLen = find_or_alloc_block(ser, data_type, 0)
    if kind != "existing":
        return None
    n = min(existLen, max_len)
    return _read_region(ser, addr + HDR_LEN, n)
