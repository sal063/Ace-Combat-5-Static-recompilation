import sys
import wave

import numpy as np

BUZZ_LO, BUZZ_HI = 13000.0, 15500.0


def bands(freqs, power, rate):
    edges = [(0, 500), (500, 2000), (2000, 4000), (4000, 8000),
             (8000, 13000), (13000, 15500), (15500, rate / 2)]
    total = power.sum()
    if total <= 0:
        return []
    out = []
    for lo, hi in edges:
        sel = (freqs >= lo) & (freqs < hi)
        out.append((lo, hi, 100.0 * power[sel].sum() / total))
    return out


def load(path):
    try:
        with wave.open(path, "rb") as w:
            ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            n = w.getnframes()
            if n > 0 and width == 2:
                return ch, width, rate, w.readframes(n), False
    except (wave.Error, EOFError):
        pass
    with open(path, "rb") as f:
        raw = f.read()[44:]
    raw = raw[:len(raw) - (len(raw) % 4)]
    return 2, 2, 48000, raw, True


def main(path):
    ch, width, rate, raw, partial = load(path)
    n = len(raw) // (ch * width)
    if partial:
        print("note        : header not finalised (run still in flight); "
              "assuming %d ch / %d Hz / s16 from ps2_audio.c\n" % (ch, rate))
    if width != 2:
        sys.exit("expected 16-bit samples, got %d-bit" % (width * 8))
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    if ch > 1:
        x = x.reshape(-1, ch)
        mono = x.mean(axis=1)
    else:
        mono = x

    dur = len(mono) / float(rate)
    peak = float(np.abs(mono).max()) if len(mono) else 0.0
    rms = float(np.sqrt(np.mean(mono ** 2))) if len(mono) else 0.0
    nonzero = float(np.count_nonzero(mono)) / max(1, len(mono))

    print("file        : %s" % path)
    print("format      : %d ch, %d Hz, %.1f s (%d frames)" % (ch, rate, dur, n))
    print("peak        : %.0f (%.1f%% of full scale)" % (peak, 100.0 * peak / 32768.0))
    print("rms         : %.1f (%.2f%% of full scale)" % (rms, 100.0 * rms / 32768.0))
    print("non-silent  : %.2f%% of samples" % (100.0 * nonzero))

    if rms <= 0.0:
        print("\nVERDICT: SILENT -- nothing was mixed.")
        return 1

    nfft = 4096
    step = nfft // 2
    win = np.hanning(nfft)
    acc = np.zeros(nfft // 2 + 1)
    used = 0
    for i in range(0, len(mono) - nfft, step):
        seg = mono[i:i + nfft]
        if np.sqrt(np.mean(seg ** 2)) < 1.0:
            continue
        acc += np.abs(np.fft.rfft(seg * win)) ** 2
        used += 1
    if used == 0:
        print("\nVERDICT: SILENT -- no frame rose above the noise floor.")
        return 1
    acc /= used
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)

    p = acc[1:] + 1e-20
    flatness = float(np.exp(np.mean(np.log(p))) / np.mean(p))
    peak_hz = float(freqs[1:][np.argmax(acc[1:])])

    print("frames used : %d of %d" % (used, max(1, (len(mono) - nfft) // step)))
    print("flatness    : %.4f  (0 = pure tone, 1 = white noise)" % flatness)
    print("peak freq   : %.0f Hz" % peak_hz)
    print("\nenergy by band:")
    buzz = 0.0
    low = 0.0
    for lo, hi, pct in bands(freqs, acc, rate):
        bar = "#" * int(round(pct / 2.0))
        print("  %6.0f-%6.0f Hz  %6.2f%%  %s" % (lo, hi, pct, bar))
        if lo >= BUZZ_LO and hi <= BUZZ_HI:
            buzz = pct
        if hi <= 4000:
            low += pct

    print()
    verdict = []
    if buzz > 50.0:
        verdict.append("FAIL: %.1f%% of the energy is in the 13-15.5 kHz band, "
                       "which is the known ADPCM-misdecode signature" % buzz)
    if flatness < 0.005:
        verdict.append("FAIL: flatness %.4f is a near-pure tone, not audio"
                       % flatness)
    if low < 5.0:
        verdict.append("WARN: only %.1f%% of the energy is below 4 kHz; real "
                       "game audio is bottom-heavy" % low)
    if not verdict:
        print("VERDICT: PLAUSIBLE AUDIO -- %.1f%% of energy below 4 kHz, "
              "no misdecode signature." % low)
        return 0
    for v in verdict:
        print("VERDICT: " + v)
    return 0 if all(v.startswith("WARN") for v in verdict) else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: python tools/analyze_wav.py out/run.wav")
    sys.exit(main(sys.argv[1]))
