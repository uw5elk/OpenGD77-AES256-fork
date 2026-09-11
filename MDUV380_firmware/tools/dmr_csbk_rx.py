#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Декодер DMR CSBK із сирого IQ-запису (HackRF Portapack .C16).

Написаний і ПЕРЕВІРЕНИЙ на реальному ефірі під час зняття квитанції RCTL
(баг #10, 2026-09-11): з його допомогою прочитано і команди RT4D, і квитанцію
стокової TYT, обидві з валідним CRC.

Ланцюг (усе підтверджено на живих записах):
  C16 IQ -> зняття зсуву несучої -> ФНЧ -> FM-дискримінатор ->
  matched filter (integrate&dump) -> пошук фази символу -> 4 рівні (k-means) ->
  ETSI-мапінг символ->дибіт -> пошук DMR-синхро -> BPTC(196,96) -> CRC

Ключові параметри, здобуті емпірично (НЕ вгадані -- звірені по CRC):
  * мапінг ETSI: рівні -3,-1,+1,+3 -> дибіти 11,10,00,01 (старший біт перший)
  * структура burst-а: 98 info | 10 slot-type | 48 sync | 10 slot-type | 98 info
  * BPTC: deinterleave d[i]=raw[(i*181)%196]; матриця 13x15 з індексу 1,
    рядково; інфо = рядки 0..8 x стовпці 0..10, зі зсувом 3 (перші 3 -- резерв)
  * CSBK CRC: crc16-CCITT(poly 0x1021, init 0) ^ 0x5A5A, big-endian,
    над першими 10 байтами (0x5A5A = 0xFFFF ^ 0xA5A5)

Приклад:
    python3 dmr_csbk_rx.py capture.C16 --rate 500000
    python3 dmr_csbk_rx.py capture.C16 --rate 25000 --upsample 20

Порада щодо запису (дорого здобута): КЛІП убиває дані остаточно, слабкий
сигнал -- ні. Тримай пік |IQ| у межах 0.2..0.8. Якщо рація поруч --
зніми антену з Portapack і постав гейни в мінімум.
"""
import argparse, sys
import numpy as np

try:
    from scipy import signal as sg
except ImportError:
    sys.exit("потрібен scipy: pip install scipy")

BAUD = 4800.0

# 48-бітні синхропослідовності DMR (ETSI TS 102 361-1)
SYNC = {
    'BS_DATA':  0xDFF57D75DF5D, 'BS_VOICE': 0x755FD7DF75F7,
    'MS_DATA':  0xD5D7F77FD757, 'MS_VOICE': 0x7F7D5DD57DFD,
    'MS_RC':    0x77D55F7DFD77,
    'T1_DATA':  0xF7FDD5DDFD55, 'T2_DATA':  0xD7557F5FF7F5,
}
ETSI = ['11', '10', '00', '01']   # рівні від найнижчого до найвищого


def _bits(v, n=48):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]

SYNCB = {k: _bits(v) for k, v in SYNC.items()}


def crc16_ccitt(data):
    c = 0
    for by in data:
        for k in range(7, -1, -1):
            b = (by >> k) & 1
            c = (((c << 1) ^ 0x1021) & 0xFFFF) if (((c >> 15) & 1) ^ b) else ((c << 1) & 0xFFFF)
    return c


def bits_to_bytes(bits):
    n = (len(bits) // 8) * 8
    return bytes(sum(bits[i + j] << (7 - j) for j in range(8)) for i in range(0, n, 8))


# ---- Hamming FEC усередині BPTC (рятує граничні записи) ----
def _h15(d):
    return [d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8],
            d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9],
            d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10],
            d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10]]


def _h13(d):
    return [d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6],
            d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7],
            d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8],
            d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8]]


def _fix(vec, n, k, parf):
    p = parf(vec[:k])
    if all(p[i] == vec[k + i] for i in range(4)):
        return vec
    for i in range(n):
        t = vec[:]
        t[i] ^= 1
        pp = parf(t[:k])
        if all(pp[j] == t[k + j] for j in range(4)):
            return t
    return vec


def bptc_196_96(info196, fec=True):
    d = [info196[(i * 181) % 196] for i in range(196)]
    if not fec:
        return [d[1 + r * 15 + c] for r in range(9) for c in range(11)][3:99]
    M = [[d[1 + r * 15 + c] for c in range(15)] for r in range(13)]
    for _ in range(2):
        for r in range(13):
            M[r] = _fix(M[r][:], 15, 11, _h15)
        for c in range(15):
            col = _fix([M[r][c] for r in range(13)], 13, 9, _h13)
            for r in range(13):
                M[r][c] = col[r]
    return [M[r][c] for r in range(9) for c in range(11)][3:99]


def decode_csbk(bits, p, fec=True):
    """p -- індекс біта, де починається 48-бітна синхра."""
    L0, R0 = p - 10 - 98, p + 48 + 10
    if L0 < 0 or R0 + 98 > len(bits):
        return None
    by = bits_to_bytes(bptc_196_96(bits[L0:L0 + 98] + bits[R0:R0 + 98], fec))
    if len(by) < 12:
        return None
    ok = (crc16_ccitt(by[:10]) ^ 0x5A5A) == ((by[10] << 8) | by[11])
    return by[:12], ok


def demod_candidates(seg, fs, keep=6):
    """Повертає кілька варіантів бітового потоку (різні зсуви/фази)."""
    spb = fs / BAUD
    mag = np.abs(seg[1:])
    strong = mag > mag.max() * 0.3
    inst = np.angle(seg[1:] * np.conj(seg[:-1])) * fs / (2 * np.pi)
    base = np.median(inst[strong]) if strong.any() else 0.0
    taps = sg.firwin(151, 7000 / (fs / 2))
    out = []
    for doff in (-600, -300, 0, 300, 600):
        s = seg * np.exp(-2j * np.pi * (base + doff) / fs * np.arange(seg.size))
        s = sg.lfilter(taps, 1, s)
        disc = np.angle(s[1:] * np.conj(s[:-1])) * fs / (2 * np.pi)
        k = int(round(spb))
        mf = np.convolve(disc, np.ones(k) / k, mode='same')
        for ph in np.arange(0, spb, 2.0):
            c = np.round(np.arange(spb + ph, disc.size - spb, spb)).astype(int)
            c = c[(c > 0) & (c < mf.size)]
            if c.size < 40:
                continue
            v = mf[c] - np.mean(mf[c])
            cen = np.sort(np.percentile(v, [12, 37, 62, 87]))
            for _ in range(20):
                lab = np.abs(v[:, None] - cen[None, :]).argmin(1)
                for j in range(4):
                    if (lab == j).any():
                        cen[j] = v[lab == j].mean()
                cen = np.sort(cen)
            lab = np.abs(v[:, None] - cen[None, :]).argmin(1)
            score = 1 - np.mean((v - cen[lab]) ** 2) / np.var(v)
            bits = []
            for l in lab:
                bits.extend([int(ETSI[l][0]), int(ETSI[l][1])])
            out.append((score, bits))
    out.sort(key=lambda x: -x[0])
    return [b for _, b in out[:keep]]


def find_bursts(iq, fs, frac=0.06):
    env = np.abs(iq)
    w = max(1, int(0.001 * fs))
    envs = np.convolve(env, np.ones(w) / w, mode='same')
    pk = envs.max()
    idx = np.where(envs > pk * frac)[0]
    if idx.size == 0:
        return [], pk
    regs, s, p = [], idx[0], idx[0]
    for i in idx[1:]:
        if i - p > int(0.008 * fs):
            regs.append((s, p))
            s = i
        p = i
    regs.append((s, p))
    return regs, pk


def main():
    ap = argparse.ArgumentParser(description="Декодер DMR CSBK із .C16 IQ (Portapack)")
    ap.add_argument("infile")
    ap.add_argument("--rate", type=int, required=True, help="частота дискретизації запису, Гц")
    ap.add_argument("--upsample", type=int, default=1, help="передискретизація (для низьких rate, напр. 20 для 25k)")
    ap.add_argument("--sync-tol", type=int, default=3, help="скільки бітових помилок дозволяти в синхрі")
    a = ap.parse_args()

    raw = np.fromfile(a.infile, dtype='<i2').astype(np.float32) / 32768.0
    iq = raw[0::2] + 1j * raw[1::2]
    fs = a.rate
    if a.upsample > 1:
        iq = sg.resample_poly(iq, a.upsample, 1)
        fs *= a.upsample

    peak = float(np.max(np.abs(iq)))
    print("тривалість %.2f с   пік |IQ| = %.3f  %s" % (
        iq.size / fs, peak,
        "<-- КЛІП! запис зіпсований" if peak > 1.2 else ("<-- дуже слабко" if peak < 0.02 else "")))

    regs, pk = find_bursts(iq, fs)
    print("знайдено регіонів: %d" % len(regs))

    seen = {}
    for (s, e) in regs:
        t0, t1 = s / fs, e / fs
        # ковзні вікна по ~33 мс (один burst) з кроком 6 мс
        t = max(0.0, t0 - 0.004)
        while t < t1:
            seg = iq[int(t * fs):int((t + 0.033) * fs)]
            if seg.size < int(0.02 * fs):
                break
            for bits in demod_candidates(seg, fs):
                for name, sb in SYNCB.items():
                    for p in range(0, len(bits) - 48):
                        if sum(1 for i in range(48) if bits[p + i] != sb[i]) <= a.sync_tol:
                            r = decode_csbk(bits, p)
                            if r and r[1]:
                                seen.setdefault(r[0].hex(), (round(t, 3), name))
            t += 0.006

    print("\n=== декодовані CSBK (CRC OK) ===")
    if not seen:
        print("  нічого. Перевір --rate; переконайся, що запис без кліпу.")
        return
    for hx, (t, name) in sorted(seen.items(), key=lambda x: x[1][0]):
        by = bytes.fromhex(hx)
        print("  %.3fс %-8s  %s %s %s %s   addr1=%d addr2=%d" % (
            t, name, by[:2].hex(), by[2:4].hex(), by[4:7].hex(), by[7:10].hex(),
            int(by[4:7].hex(), 16), int(by[7:10].hex(), 16)))


if __name__ == "__main__":
    main()
