#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Декодер DMR data-burst-ів із .C16 (реверс SMS-квитанції, баг #11).

Ланцюг DSP спільний із dmr_csbk_rx.py (перевірений на ефірі). Тут інше:
замість самих CSBK витягуємо КОЖЕН data-sync burst, женемо через BPTC(196,96)
(rate-1/2) і показуємо 12 байтів + CRC для кількох масок:
  * 0x5A5A -- CSBK/preamble;
  * 0x3333 -- data header (ETSI: crc ^0xCCCC над 80 бітами; наша crc без фін.-XOR -> 0x3333).
Data header відповіді (ACK/квитанція) кодується саме rate-1/2, тож має тут проявитись.
Rate-3/4 (payload confirmed) BPTC-ом не декодується -- його поки лишаємо (треба треліс).

Мета: знайти байти квитанції, яку стокова TYT шле у відповідь на Confirmed SMS.
"""
import argparse, sys
import numpy as np
from scipy import signal as sg

BAUD = 4800.0
SYNC = {
    'BS_DATA':  0xDFF57D75DF5D, 'BS_VOICE': 0x755FD7DF75F7,
    'MS_DATA':  0xD5D7F77FD757, 'MS_VOICE': 0x7F7D5DD57DFD,
    'MS_RC':    0x77D55F7DFD77,
    'T1_DATA':  0xF7FDD5DDFD55, 'T2_DATA':  0xD7557F5FF7F5,
}
ETSI = ['11', '10', '00', '01']

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

def _h15(d):
    return [d[0]^d[1]^d[2]^d[3]^d[5]^d[7]^d[8], d[1]^d[2]^d[3]^d[4]^d[6]^d[8]^d[9],
            d[2]^d[3]^d[4]^d[5]^d[7]^d[9]^d[10], d[0]^d[1]^d[2]^d[4]^d[6]^d[7]^d[10]]
def _h13(d):
    return [d[0]^d[1]^d[3]^d[5]^d[6], d[0]^d[1]^d[2]^d[4]^d[6]^d[7],
            d[0]^d[1]^d[2]^d[3]^d[5]^d[7]^d[8], d[0]^d[2]^d[4]^d[5]^d[8]]
def _fix(vec, n, k, parf):
    p = parf(vec[:k])
    if all(p[i] == vec[k+i] for i in range(4)): return vec
    for i in range(n):
        t = vec[:]; t[i] ^= 1; pp = parf(t[:k])
        if all(pp[j] == t[k+j] for j in range(4)): return t
    return vec
def bptc_196_96(info196, fec=True):
    d = [info196[(i*181) % 196] for i in range(196)]
    if not fec:
        return [d[1+r*15+c] for r in range(9) for c in range(11)][3:99]
    M = [[d[1+r*15+c] for c in range(15)] for r in range(13)]
    for _ in range(2):
        for r in range(13): M[r] = _fix(M[r][:], 15, 11, _h15)
        for c in range(15):
            col = _fix([M[r][c] for r in range(13)], 13, 9, _h13)
            for r in range(13): M[r][c] = col[r]
    return [M[r][c] for r in range(9) for c in range(11)][3:99]

def decode_hdr(bits, p, fec=True):
    L0, R0 = p - 10 - 98, p + 48 + 10
    if L0 < 0 or R0 + 98 > len(bits): return None
    by = bits_to_bytes(bptc_196_96(bits[L0:L0+98] + bits[R0:R0+98], fec))
    if len(by) < 12: return None
    crc = crc16_ccitt(by[:10]); rx = (by[10] << 8) | by[11]
    masks = {'csbk5A5A': 0x5A5A, 'hdr3333': 0x3333, 'raw0000': 0x0000, 'ffff': 0xFFFF}
    okmask = next((n for n, m in masks.items() if (crc ^ m) == rx), None)
    return by[:12], okmask

def demod_candidates(seg, fs, keep=8):
    spb = fs / BAUD
    inst = np.angle(seg[1:] * np.conj(seg[:-1])) * fs / (2*np.pi)
    mag = np.abs(seg[1:]); strong = mag > mag.max()*0.3
    base = np.median(inst[strong]) if strong.any() else 0.0
    taps = sg.firwin(151, 7000/(fs/2))
    out = []
    for doff in (-600, -300, 0, 300, 600):
        s = seg * np.exp(-2j*np.pi*(base+doff)/fs*np.arange(seg.size))
        s = sg.lfilter(taps, 1, s)
        disc = np.angle(s[1:]*np.conj(s[:-1])) * fs/(2*np.pi)
        k = int(round(spb)); mf = np.convolve(disc, np.ones(k)/k, mode='same')
        for ph in np.arange(0, spb, 2.0):
            c = np.round(np.arange(spb+ph, disc.size-spb, spb)).astype(int)
            c = c[(c > 0) & (c < mf.size)]
            if c.size < 40: continue
            v = mf[c] - np.mean(mf[c])
            cen = np.sort(np.percentile(v, [12,37,62,87]))
            for _ in range(20):
                lab = np.abs(v[:,None]-cen[None,:]).argmin(1)
                for j in range(4):
                    if (lab==j).any(): cen[j] = v[lab==j].mean()
                cen = np.sort(cen)
            lab = np.abs(v[:,None]-cen[None,:]).argmin(1)
            score = 1 - np.mean((v-cen[lab])**2)/np.var(v)
            bits = []
            for l in lab: bits.extend([int(ETSI[l][0]), int(ETSI[l][1])])
            out.append((score, bits))
    out.sort(key=lambda x: -x[0])
    return [b for _, b in out[:keep]]

def find_bursts(iq, fs, frac=0.06):
    env = np.abs(iq); w = max(1, int(0.001*fs))
    envs = np.convolve(env, np.ones(w)/w, mode='same'); pk = envs.max()
    idx = np.where(envs > pk*frac)[0]
    if idx.size == 0: return [], pk
    regs, s, p = [], idx[0], idx[0]
    for i in idx[1:]:
        if i - p > int(0.008*fs): regs.append((s, p)); s = i
        p = i
    regs.append((s, p)); return regs, pk

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile"); ap.add_argument("--rate", type=int, required=True)
    ap.add_argument("--sync-tol", type=int, default=4)
    a = ap.parse_args()
    raw = np.fromfile(a.infile, dtype='<i2').astype(np.float32)/32768.0
    iq = raw[0::2] + 1j*raw[1::2]; fs = a.rate
    peak = float(np.max(np.abs(iq)))
    print("тривалість %.2f с   пік |IQ| = %.3f" % (iq.size/fs, peak))
    regs, pk = find_bursts(iq, fs)
    print("регіонів: %d" % len(regs))
    # (час, sync, hex12, маска) -- унікалізуємо по (час-округл, hex)
    seen = {}
    for (s, e) in regs:
        t0, t1 = s/fs, e/fs; t = max(0.0, t0 - 0.004)
        while t < t1:
            seg = iq[int(t*fs):int((t+0.033)*fs)]
            if seg.size < int(0.02*fs): break
            for bits in demod_candidates(seg, fs):
                for name, sb in SYNCB.items():
                    for p in range(0, len(bits)-48):
                        if sum(1 for i in range(48) if bits[p+i] != sb[i]) <= a.sync_tol:
                            r = decode_hdr(bits, p)
                            if r and r[1]:  # тільки з валідним CRC хоч по одній масці
                                key = (round(t*100), r[0].hex())
                                seen.setdefault(key, (round(t,3), name, r[0], r[1]))
            t += 0.005
    print("\n=== data/CSBK burst-и з валідним CRC ===")
    if not seen:
        print("  нічого."); return
    for (_, (t, name, by, mask)) in sorted(seen.items(), key=lambda x: x[1][0]):
        print("  %.3fс %-8s [%-8s] %s  a1=%d a2=%d" % (
            t, name, mask, by.hex(),
            int.from_bytes(by[4:7],'big') if len(by)>=7 else 0,
            int.from_bytes(by[7:10],'big') if len(by)>=10 else 0))

if __name__ == "__main__":
    main()
