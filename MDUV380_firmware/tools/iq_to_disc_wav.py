#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Portapack C16 IQ -> дискримінований WAV для DSD-FME (реверс формату ACK, баг #10).

Portapack (Mayhem) пише сирий IQ у форматі C16: чергуються int16 I,Q (little-endian).
DSD-FME не їсть IQ напряму -- йому треба або RTL-SDR наживо, або WAV з ВИХОДУ FM-
дискримінатора (тобто вже продетектований сигнал). Цей скрипт робить саме таке
перетворення на ПК/у хмарі, без жодного заліза:

    C16 IQ  --(опц. зсув частоти)-->  FM-дискримінатор  -->  ресемпл 48 кГц  -->  WAV

Далі:
    dsd-fme -fs -i disc.wav -Z            # DMR (обидва слоти), -Z друкує сирі payload
    # CSBK-квитанція від стокової з'явиться рядком у hex (напр. a4 10 00 .. або a0/a1 ..).

Приклад:
    # файл Portapack: назва зазвичай містить частоту й sample rate
    python3 iq_to_disc_wav.py capture_446000000Hz_500000sps.C16 --rate 500000
    # якщо сигнал не по центру смуги, зсунь його в 0 Гц (напр. записував на 446.00, а
    # рація на 446.006 -> --shift 6000):
    python3 iq_to_disc_wav.py cap.C16 --rate 500000 --shift 6000

Вихід: <вхід>.disc.wav (16-bit mono, 48000 Гц).

Це НЕ декодер -- він лише готує сигнал для DSD-FME. Декодування (BPTC/CRC -> hex)
робить DSD-FME. Якщо DSD-FME під рукою немає, є альтернатива: залий сам C16 у чат --
розберу тут (буде повільніше й окремою домовленістю).
"""
import argparse, sys, wave
import numpy as np

DMR_SYMBOL_RATE = 4800.0     # Бод (для довідки; тут не використовуємо синхронізацію символів)
OUT_RATE = 48000


def read_c16(path):
    """C16 = чергуються int16 I,Q (LE). Повертає комплексний масив, нормований до [-1,1]."""
    raw = np.fromfile(path, dtype='<i2')
    if raw.size < 2:
        sys.exit("порожній або надто малий IQ-файл")
    if raw.size % 2:
        raw = raw[:-1]
    iq = raw.astype(np.float32).view()
    i = iq[0::2] / 32768.0
    q = iq[1::2] / 32768.0
    return i + 1j * q


def fm_discriminate(x):
    """Класичний FM-дискримінатор: миттєва похідна фази (angle(conj(x[n-1])*x[n]))."""
    if x.size < 2:
        return np.zeros(0, dtype=np.float32)
    prod = x[1:] * np.conj(x[:-1])
    disc = np.angle(prod).astype(np.float32)
    return disc


def freq_shift(x, shift_hz, fs):
    if not shift_hz:
        return x
    n = np.arange(x.size, dtype=np.float64)
    lo = np.exp(-2j * np.pi * (shift_hz / fs) * n).astype(np.complex64)
    return (x * lo).astype(np.complex64)


def resample_to(x, fs_in, fs_out):
    """Простий полінабірний ресемпл через лінійну інтерполяцію (для мовного тракту DSD цього
    досить: несуча дискримінатора -- це вже НЧ-сигнал 4800 Бод, а не РЧ)."""
    if fs_in == fs_out:
        return x
    n_out = int(round(x.size * fs_out / fs_in))
    if n_out <= 1:
        return np.zeros(0, dtype=np.float32)
    t_in = np.arange(x.size, dtype=np.float64)
    t_out = np.linspace(0, x.size - 1, n_out)
    return np.interp(t_out, t_in, x).astype(np.float32)


def write_wav(path, samples, rate=OUT_RATE):
    s = samples
    peak = float(np.max(np.abs(s))) if s.size else 1.0
    if peak < 1e-9:
        peak = 1.0
    pcm = np.clip(s / peak * 0.9, -1.0, 1.0)
    pcm16 = (pcm * 32767.0).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm16.tobytes())


def parse_rate_from_name(name):
    import re
    m = re.search(r'(\d+)\s*sps', name, re.I) or re.search(r'_(\d{5,})[._]', name)
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser(description="Portapack C16 IQ -> дискримінований WAV для DSD-FME")
    ap.add_argument("infile", help="вхідний .C16 IQ-файл з Portapack")
    ap.add_argument("--rate", type=int, default=None, help="частота дискретизації IQ (Гц); якщо не задано -- пробую взяти з назви файлу")
    ap.add_argument("--shift", type=float, default=0.0, help="зсув частоти в Гц (перенести сигнал у 0), якщо записував трохи збоку")
    ap.add_argument("--out", default=None, help="вихідний WAV (типово <вхід>.disc.wav)")
    a = ap.parse_args()

    fs = a.rate or parse_rate_from_name(a.infile)
    if not fs:
        sys.exit("не вдалось визначити sample rate: задай --rate (Гц)")

    x = read_c16(a.infile)
    print("IQ-семплів: %d  (%.2f с при %d sps)" % (x.size, x.size / fs, fs))
    x = freq_shift(x, a.shift, fs)
    disc = fm_discriminate(x)
    disc = resample_to(disc, fs, OUT_RATE)
    out = a.out or (a.infile + ".disc.wav")
    write_wav(out, disc, OUT_RATE)
    print("записав %s  (%d семплів, %d Гц, mono 16-bit)" % (out, disc.size, OUT_RATE))
    print("далі:  dsd-fme -fs -i %s -Z" % out)


if __name__ == "__main__":
    main()
