#!/usr/bin/env python3
"""Референсний білдер стокових TYT-команд керування (Radio Check / Remote Monitor /
Radio Enable / Radio Disable) — байт-у-байт як стокова рація шле їх в ефір.

Реверс зроблено із САМОГО ЕФІРУ (2026-09-05): захоплено сирі CSBK зі стокової TYT
інструментом tools/rctl_capture.py, розібрано, CRC перевірено. Self-test унизу відтворює
всі чотири захоплені команди й обидва типи преамбул РІВНО в ті самі байти.

ФОРМАТ (усе ВІДКРИТИМ ТЕКСТОМ, без AES):
  Команда — один CSBK (тип 3), 12 байт:
      [b0] [FID=0x10] [0x00] [cmd] [src:3] [dst:3] [crc:2]
    b0  — 0xA4 (опкод CSBK 0x24) для Check/Enable/Disable, 0x9D (опкод 0x1D) для Monitor;
          старший біт = Last Block.
    cmd — керуючий байт: Check=0x00, Monitor=0x01, Enable=0x7E, Disable=0x7F.
    src — DMR ID командира (хто шле), dst — DMR ID цілі. УВАГА: у команді порядок
          src,dst; у преамбулі — навпаки, dst,src (див. нижче). Так шле стокова.
  Перед командою йде послідовність преамбул CSBK, що рахують кадри вниз:
      [0xBD] [0x00] [0x00] [лічильник] [dst:3] [src:3] [crc:2]

  CRC — CRC-CCITT "d" (poly 0x1021, init 0, xor 0xFFFF) над 10 байтами вмісту, потім ^0xA5A5.
  Той самий CRC, що для преамбул SMS (tools/dmr_enc_sms.py), тож код спільний.
"""

# ---- CRC (спільний із dmr_enc_sms.py) ----
def _crc16d(data):
    crc = 0
    for byte in data:
        for k in range(7, -1, -1):
            bit = (byte >> k) & 1
            crc = (((crc << 1) ^ 0x1021) & 0xFFFF) if (((crc >> 15) & 1) ^ bit) else ((crc << 1) & 0xFFFF)
    return crc ^ 0xFFFF

def _csbk_crc(body10):
    v = _crc16d(body10) ^ 0xA5A5
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


# ---- команди ----
# (b0, cmd_byte) для кожної команди
CMD = {
    "check":   (0xA4, 0x00),   # Radio Check
    "monitor": (0x9D, 0x01),   # Remote Monitor
    "enable":  (0xA4, 0x7E),   # Radio Enable  (revive)
    "disable": (0xA4, 0x7F),   # Radio Disable (stun)
}
FID_MOTO = 0x10


def build_command(kind, src, dst):
    """12-байтний CSBK команди `kind` від src до dst (обидва — 24-бітні DMR ID)."""
    b0, cmd = CMD[kind]
    body = bytes([b0, FID_MOTO, 0x00, cmd,
                  (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
                  (dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF])
    return body + _csbk_crc(body)


def build_preamble(dst, src, countdown):
    """Один CSBK-преамбули (dst,src — порядок як у стокової: ціль перша)."""
    body = bytes([0xBD, 0x00, 0x00, countdown & 0xFF,
                  (dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF,
                  (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF])
    return body + _csbk_crc(body)


def build_sequence(kind, src, dst, preambles=17):
    """Повна послідовність: N преамбул (лічильник N-1..0) + сам CSBK команди."""
    out = [build_preamble(dst, src, preambles - 1 - i) for i in range(preambles)]
    out.append(build_command(kind, src, dst))
    return out


# ---- self-test: відтворити реальні захвати байт-у-байт ----
if __name__ == "__main__":
    SRC, DST = 2550333, 2550287   # стокова -> форк, з реального захвату 2026-09-05

    # захоплені зі стокової TYT команди (tools/rctl_capture.py)
    captured_cmd = {
        "check":   "a4 10 00 00 26 ea 3d 26 ea 0f 93 7c",
        "monitor": "9d 10 00 01 26 ea 3d 26 ea 0f a3 88",
        "enable":  "a4 10 00 7e 26 ea 3d 26 ea 0f 25 95",
        "disable": "a4 10 00 7f 26 ea 3d 26 ea 0f 9d f4",
    }
    # кілька захоплених преамбул (лічильник -> hex-байти)
    captured_pre = {
        0x02: "bd 00 00 02 26 ea 0f 26 ea 3d c6 69",
        0x10: "bd 00 00 10 26 ea 0f 26 ea 3d 91 f1",
        0x0a: "bd 00 00 0a 26 ea 0f 26 ea 3d 55 c4",
    }

    fails = 0
    for kind, hexs in captured_cmd.items():
        want = bytes(int(x, 16) for x in hexs.split())
        got = build_command(kind, SRC, DST)
        ok = got == want
        fails += not ok
        print(f"  {'OK  ' if ok else 'FAIL'} команда {kind:8} -> {got.hex()}")

    for cnt, hexs in captured_pre.items():
        want = bytes(int(x, 16) for x in hexs.split())
        got = build_preamble(DST, SRC, cnt)
        ok = got == want
        fails += not ok
        print(f"  {'OK  ' if ok else 'FAIL'} преамбула {cnt:#04x} -> {got.hex()}")

    print("\nусі кадри відтворено байт-у-байт" if not fails else f"\nПРОВАЛЕНО: {fails}")
    raise SystemExit(1 if fails else 0)
