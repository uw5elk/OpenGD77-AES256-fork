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

    if "--reset" in sys.argv:
        ser.write(bytes([ord("C"), 0x94])); ser.flush(); time.sleep(0.2)
        print("counters reset:", ser.read(8).hex())
        return

    ser.write(bytes([ord("C"), 0x93])); ser.flush(); time.sleep(0.3)
    r = ser.read(128)
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
