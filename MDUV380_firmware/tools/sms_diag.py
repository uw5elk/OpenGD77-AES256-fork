#!/usr/bin/env python3
"""Read the fork's encrypted-SMS RX diagnostics over USB CPS.
  0x93 = counters (d/hOk/hBad/bOk/bBad/pdu/msg + txActive)
  0x94 = reset counters      (--reset)
  0x95 = dump last reassembled PDU + metadata   (--pdu)
Run from Windows or WSL; auto-detects the OpenGD77 radio (1FC9:0094)."""
import sys, time, struct
import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094

def find_port():
    for p in list_ports.comports():
        if p.vid == APP_VID and p.pid == APP_PID:
            return p.device
    return None

def main():
    port = find_port()
    if not port:
        sys.exit("radio not found (USB 1FC9:0094)")
    ser = serial.Serial(port, 115200, timeout=0.5)

    # Підбір подачі квитанції без перепрошивки: --ack <повторів> <пауза_мс>
    if "--ack" in sys.argv:
        i = sys.argv.index("--ack")
        try:
            reps = int(sys.argv[i + 1]); dly = int(sys.argv[i + 2])
        except (IndexError, ValueError):
            sys.exit("вжиток: sms_diag.py --ack <повторів 1..6> <пауза мс, напр. 80>")
        ser.write(bytes([ord("C"), 0xB2, reps & 0xFF, dly & 0xFF, (dly >> 8) & 0xFF]))
        ser.flush(); time.sleep(0.2); ser.read(8)
        print("подача квитанції: %d повтор(ів), пауза %d мс" % (reps, dly))
        print("(діє до вимкнення рації; тепер надішли SMS зі стокової й подивись на її екран)")
        return

    if "--reset" in sys.argv:
        ser.write(bytes([ord("C"), 0x94])); ser.flush(); time.sleep(0.2)
        print("counters reset:", ser.read(8).hex())
        return

    ser.write(bytes([ord("C"), 0x93])); ser.flush(); time.sleep(0.3)
    r = ser.read(256)   # 3+28+1 + 64 (типи) + 32 (квитанція) = 128; із запасом
    if len(r) >= 3 + 28 + 1 and r[0] == ord("C"):
        vals = struct.unpack_from("<7I", r, 3)
        tx = r[3 + 28]
        names = ["d", "hOk", "hBad", "bOk", "bBad", "pdu", "msg"]
        print("RX diag: " + "  ".join(f"{k}={v}" for k, v in zip(names, vals)) + f"  txActive={tx}")
        # Гістограма типів бурстів (16x uint32), дописана в кінець відповіді (нова прошивка).
        if len(r) >= 3 + 29 + 64:
            types = struct.unpack_from("<16I", r, 3 + 29)
            tnames = {0:"PI-hdr",1:"VLC-hdr",2:"TLC",3:"CSBK",4:"MBC-hdr",5:"MBC-cont",
                      6:"data-hdr",7:"rate-1/2",8:"rate-3/4",9:"reserved9",10:"rate-1",
                      13:"IdleFill"}
            shown = [f"{tnames.get(i, f'тип{i}')}={v}" for i, v in enumerate(types) if v]
            print("Типи бурстів: " + ("  ".join(shown) if shown else "(порожньо)"))
            print("  -> навантаження SMS іде тим типом, що не data-hdr/CSBK (напр. rate-3/4).")
        # Діагностика квитанції (8x uint32), дописана після гістограми.
        if len(r) >= 3 + 29 + 64 + 32:
            off = 3 + 29 + 64
            seen, queued, sent, stale, h0, h1, grp, forus = struct.unpack_from("<8I", r, off)
            if len(r) >= off + 40:
                rev, reps = struct.unpack_from("<2I", r, off + 32)
                print("Ревізія квитанції у прошивці: %d (повторів у черзі: %d)" % (rev, reps))
                if rev < 7:
                    print("  УВАГА: залита СТАРА прошивка (rev<7). Прошийся свіжим бандлом.")
                if len(r) >= off + 44:
                    (dly,) = struct.unpack_from("<I", r, off + 40)
                    tgt = None
                    if len(r) >= off + 48:
                        (tgt,) = struct.unpack_from("<I", r, off + 44)
                    print("  Пауза до віддачі квитанції: %d мс%s  (еталон стокової ~80 мс)"
                          % (dly, ("  [ціль %d]" % tgt) if tgt is not None else ""))
                    if dly == 0:
                        print("    -- ще не міряно (квитанція не йшла після ввімкнення).")
                    elif dly > 200:
                        print("    -- ПІЗНО: вікно відправника вже закрите.")
                    elif dly < 25:
                        print("    -- РАНО: відправник може ще дотягувати термінатор.")
                    else:
                        print("    -- у межах норми.")
            else:
                print("Ревізія квитанції: невідома -- прошивка старіша за rev 5.")
            print("Квитанція: бачив=%d вчергу=%d вефір=%d кинуто=%d  "
                  "ост.заголовок=%02x %02x  груповий=%d нам=%d" %
                  (seen, queued, sent, stale, h0, h1, grp, forus))
            dpf, a = h0 & 0x0F, (h0 >> 6) & 1
            print("  заголовок: DPF=%d (%s), біт A(просить квитанцію)=%d, SAP=%d" % (
                dpf, {0: "UDT", 1: "Response", 2: "Unconfirmed", 3: "CONFIRMED"}.get(dpf, "?"),
                a, (h1 >> 4) & 0x0F))
            if seen == 0:
                print("  ДІАГНОЗ: відправник НЕ просить квитанції (треба CONFIRMED+A) "
                      "-- дивись DPF/біт A вище; у CPS увімкни підтверджену доставку.")
            elif queued == 0:
                print("  ДІАГНОЗ: просить, але відсіяв фільтр -- груповий(%d)/не нам(%d)." % (grp, forus))
            elif sent == 0:
                print("  ДІАГНОЗ: поставлено в чергу, але в ефір не пішло -- канал не звільнявся "
                      "(кинуто=%d). Відправник молотить ретрансміти впритул." % stale)
            else:
                print("  -> квитанція йшла в ефір %d раз(ів); якщо відправник усе одно повторює, "
                      "справа у формі/таймінгу квитанції." % sent)
        else:
            print("Квитанція: лічильників немає у відповіді (%d Б) -- у рації СТАРА прошивка, "
                  "без діагностики квитанції. Прошийся свіжим бандлом і повтори." % len(r))
    else:
        print("unexpected reply (%d B): %s" % (len(r), r.hex()))

    if "--pdu" in sys.argv:
        ser.write(bytes([ord("C"), 0x95])); ser.flush(); time.sleep(0.3)
        r = ser.read(512)
        if len(r) >= 3 and r[0] == ord("C"):
            n = (r[1] << 8) | r[2]; body = r[3:3 + n]
            if len(body) >= 8:
                plen = (body[0] << 8) | body[1]
                keyid, exp = body[2], body[3]
                peer = struct.unpack_from("<I", body, 4)[0]
                print(f"last PDU: len={plen} keyId={keyid} expBlocks={exp} peer={peer}")
                print("  raw:", body[8:8 + plen].hex())
        else:
            print("no PDU reply:", r.hex())

main()
