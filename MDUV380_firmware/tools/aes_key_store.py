#!/usr/bin/env python3
"""Persistently store DMRA AES-256 keys on an OpenGD77-AES radio using the SAME
flash mechanism the OpenGD77 CPS uses (CPS_ACCESS_FLASH 'X' write commands).

*** УВАГА (2026-09-03): --key/--keyid ТУТ БІЛЬШЕ НЕ ПРАЦЮЮТЬ. ***
Пізніший коміт прошивки (5028cc3, "AES keys: read and write the stock TYT key
table") переніс МАТЕРІАЛ ключа з блоку нижче в стокову таблицю ключів TYT
MD-UV390 (SPI-флеш 0xD9F9C+0x64*keyId, реверс-інжинірингом, для сумісності з
CHIRP і зі стоковою прошивкою) — dmrAesLoadKeys() більше НЕ читає ключ звідси.
Цей скрипт і далі мовчки "успішно" записує --key в блок AESK нижче, але
прошивка ці байти вже ігнорує — реального ефекту на голосове шифрування це
НЕ має. --tx-key і далі працює як задокументовано (селектор активного
TX-ключа лишився саме тут, у заголовку AESK).
Робочі способи додати/видалити сам ключ сьогодні:
  - прямо з рації: меню "AES-ключі" (menuAESKeys.c) — без ПК;
  - CHIRP Read/Write кодплагу (формат ключів ідентичний стоковому);
  - tools/gui_flasher.py, кнопка "Керування AES-ключами..." (те саме, у GUI;
    коректна реалізація — tools/stock_key_table.py).
Дивись PLANS.md, розділ 15.

*** ВИПРАВЛЕННЯ (2026-09-03): читання/запис цього блоку тепер через custom_data.py. ***
До цього скрипт (а) шукав AES-блок лише в перших 1024 Б custom-data регіону — на рації
з великим блоком SMS-повідомлень AES-блок фізично міг лежати ДАЛІ й просто не знаходився;
(б) записував НАСЛІП з початку регіону, що могло затерти БУДЬ-ЯКИЙ інший блок (тему,
заставку, повідомлення), який фізично лежить у перших ~600 байтах. Обидва виправлено:
tools/custom_data.py — той самий scan-find-or-append алгоритм, що й у прошивці
(codeplug.c), без обмеження на 1024 Б і без ризику для сусідніх блоків.

The keys live in a standard OpenGD77 custom-data block (CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS
= 6) in the SPI-flash custom-data region (FLASH_ADDRESS_OFFSET = 0x20000 on MDUV380).
This is the same region/format the CPS manages themes, boot screens, DMR-ID data, etc.
The firmware still reads BYTE 5 of this block (the TX-key selector) via
codeplugGetOpenGD77CustomData() + dmrAesLoadKeys() — but no longer the key material.

Usage:
  python3 aes_key_store.py --key <64hex> [--keyid 1] [--tx-key N] [--port COM4]  # --key: NO-OP, see above
  python3 aes_key_store.py --tx-key N            # set active TX key only (still works)
  python3 aes_key_store.py --show                # dump the stored block

The block survives reboots (it's in flash). The radio enumerates as USB CDC 1fc9:0094.
"""
import argparse, struct, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

AES_VID, AES_PID = 0x1FC9, 0x0094
FLASH_BASE = 0x20000              # FLASH_ADDRESS_OFFSET (MDUV380); custom-data region start
SECTOR = FLASH_BASE // 4096       # = 32
CUSTOM_MAGIC = b"OpenGD77"
HDR_LEN = 12                      # 8-byte magic + 4 reserved, then blocks
TYPE_AES_KEYS = 6
AESK_BLOCK_LEN = 8 + 16 * 36      # 584: "AESK" + ver + txkey + rsvd2 + 16*(valid,keyid,rsvd2,key32)

def find_port():
    for p in list_ports.comports():
        if (p.vid == AES_VID) and (p.pid == AES_PID):
            return p.device
    return None

def show_cps(ser):
    ser.reset_input_buffer(); ser.write(bytes([ord("C"), 0])); ser.flush(); time.sleep(0.1); ser.read(64)

def flash_read(ser, addr, length):
    """CPS 'R' CPS_ACCESS_FLASH read -> bytes (raw SPI-flash address)."""
    out = b""
    while length > 0:
        n = min(length, 1024)
        req = bytes([ord("R"), 1, (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
                     (n >> 8) & 0xFF, n & 0xFF])
        ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.15)
        r = ser.read(n + 8)
        if len(r) < 3 or r[0] != ord("R"):
            raise RuntimeError("flash read failed @%06X: %r" % (addr, r[:8]))
        out += r[3:3 + n]
        addr += n; length -= n
    return out

def flash_write_block(ser, addr, data):
    """Write `data` at raw SPI-flash `addr` via CPS 'X' prepare/send/commit.
    Only the touched bytes change; the rest of the 4 KB sector is preserved
    (the firmware reads the sector first, patches it, then erases+writes)."""
    sector = addr // 4096
    # 1) prepare sector (firmware reads it into its RAM buffer)
    req = bytes([ord("X"), 1, (sector >> 16) & 0xFF, (sector >> 8) & 0xFF, sector & 0xFF])
    ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.2)
    r = ser.read(8)
    if not r or r[0] == ord("-"):
        raise RuntimeError("prepare sector failed: %r" % r)
    # 2) send data (<=1528 per chunk; our block is 604 -> one chunk)
    off = 0
    while off < len(data):
        chunk = data[off:off + 1024]
        a = addr + off
        req = bytes([ord("X"), 2, (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF,
                     (len(chunk) >> 8) & 0xFF, len(chunk) & 0xFF]) + chunk
        ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.2)
        r = ser.read(8)
        if not r or r[0] == ord("-"):
            raise RuntimeError("send data failed @%06X: %r" % (a, r))
        off += len(chunk)
    # 3) commit (erase sector + write back)
    ser.reset_input_buffer(); ser.write(bytes([ord("X"), 3])); ser.flush(); time.sleep(0.5)
    r = ser.read(8)
    if not r or r[0] == ord("-"):
        raise RuntimeError("flash write/commit failed: %r" % r)

def fresh_payload():
    p = bytearray(AESK_BLOCK_LEN)
    p[0:4] = b"AESK"; p[4] = 1; p[5] = 0  # magic, version, txKeyId=0
    return p

def set_key(payload, keyid, key32):
    p = bytearray(payload)
    slot = -1; freeslot = -1
    for i in range(16):
        e = 8 + i * 36
        if p[e] == 1 and p[e + 1] == keyid: slot = i; break
        if freeslot < 0 and p[e] == 0: freeslot = i
    if slot < 0: slot = freeslot
    if slot < 0: raise RuntimeError("no free key slot")
    e = 8 + slot * 36
    p[e] = 1; p[e + 1] = keyid; p[e + 2] = p[e + 3] = 0; p[e + 4:e + 36] = key32
    return bytes(p), slot

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", help="AES-256 key, 64 hex chars")
    ap.add_argument("--keyid", type=int, default=1)
    ap.add_argument("--tx-key", dest="txkey", type=int, default=None, help="active TX key id (0=off)")
    ap.add_argument("--show", action="store_true", help="dump the stored AES_KEYS block")
    ap.add_argument("--port", default=None)
    a = ap.parse_args()
    port = a.port or find_port()
    if not port: sys.exit("radio not found (1fc9:0094); pass --port")

    key = None
    if a.key:
        key = bytes.fromhex(a.key.strip())
        if len(key) != 32: sys.exit("key must be 64 hex chars")
        if not (0 <= a.keyid <= 15): sys.exit("keyid 0..15")
        print("УВАГА: --key тут з 2026-09-03 НІЧОГО не робить для реального шифрування -- "
              "прошивка більше не читає ключ із цього блоку (див. коментар на початку файлу). "
              "Використай меню \"AES-ключі\" на рації, CHIRP, або gui_flasher.py.")

    with serial.Serial(port, 115200, timeout=0.6) as ser:
        show_cps(ser)
        # custom_data.py: правильний, безпечний для СУСІДНІХ блоків (тема/повідомлення/
        # заставка/RCTL) читач/писар -- дивись коментар там (2026-09-03: до цього тут
        # був прямий запис "з початку регіону", що міг зіпсувати будь-який блок, який
        # фізично лежить після AES-селектора, а читання дивилось лише в перші 1024 Б
        # регіону -- на рації з великим блоком повідомлень (SMS) AES-блок міг лежати
        # ДАЛІ й просто не знаходитись). Локальний import, щоб уникнути кругової
        # залежності: custom_data.py сам робить "import aes_key_store".
        import custom_data as cd
        payload = cd.read_block(ser, TYPE_AES_KEYS, AESK_BLOCK_LEN)
        print("AES-селектор блок:", ("знайдено, %d Б" % len(payload)) if payload else "відсутній")

        if a.show:
            pl = payload if payload else b""
            print("AESK payload present:", bool(pl), "len:", len(pl))
            if pl[:4] == b"AESK":
                print("  version=%d txKeyId=%d" % (pl[4], pl[5]))
                for i in range(16):
                    e = 8 + i * 36
                    if pl[e] == 1:
                        print("  slot %d: keyId=%d key=%s..." % (i, pl[e + 1], pl[e + 4:e + 8].hex()))
            return

        if (key is None) and (a.txkey is None):
            sys.exit("nothing to do: pass --key and/or --tx-key (or --show)")

        payload = bytearray(payload) if payload else fresh_payload()
        if payload[:4] != b"AESK":
            payload = fresh_payload()
        if key is not None:
            payload, slot = set_key(payload, a.keyid, key)
            payload = bytearray(payload)
            print("set key id %d in slot %d" % (a.keyid, slot))
        if a.txkey is not None:
            payload[5] = a.txkey & 0xFF
            print("set TX key id = %d" % a.txkey)

        ok, msg = cd.write_block(ser, TYPE_AES_KEYS, bytes(payload))
        if not ok:
            sys.exit("ЗАПИС НЕ ВДАВСЯ: %s" % msg)
        print("wrote %d bytes (%s)" % (AESK_BLOCK_LEN, msg))

        # verify read-back
        rb = cd.read_block(ser, TYPE_AES_KEYS, AESK_BLOCK_LEN)
        ok2 = rb == bytes(payload)
        print("read-back verify:", "OK" if ok2 else "MISMATCH")
        if not ok2:
            print("  wrote:", bytes(payload)[:24].hex())
            print("  read :", (rb or b"")[:24].hex())

if __name__ == "__main__":
    main()
