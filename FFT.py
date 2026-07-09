#!/usr/bin/env python3
"""Live waveform + FULL-SPAN FFT off the ESP32's /capture endpoint.
Axis follows the firmware's measured sample rate (X-Sample-Rate header).
Electronics removal: dark-frame baseline subtraction + optional manual notches.

Keys (click the plot window first so it has keyboard focus):
  b  capture baseline  -- do this with TUBE OFF, DC line connected
  s  toggle baseline subtraction
  n  toggle manual notches
"""
import collections
import requests, numpy as np
import matplotlib
print("matplotlib backend:", matplotlib.get_backend())
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

# ---------------- config ----------------
IP          = "192.168.1.42"       # <-- set to the IP shown on the DIAG screen
FS_FALLBACK = 20000.0              # used ONLY if firmware sends no header
AVG_N       = 12                   # live spectra in the running average
BASE_N      = 12                   # captures averaged into the baseline
NOTCH_HZ    = [7300.0]             # manual blanks (display only), toggle 'n'
NOTCH_BW    = 400.0                # +/- Hz around each notch
# -----------------------------------------

url  = f"http://{IP}/capture"
sess = requests.Session()

def fetch():
    r = sess.get(url, timeout=6)
    r.raise_for_status()
    fs = float(r.headers.get("X-Sample-Rate", FS_FALLBACK))
    return fs, np.asarray(r.text.split(), dtype=float)

print(f"probing {url} ...")
r0 = sess.get(url, timeout=6); r0.raise_for_status()
hdr = r0.headers.get("X-Sample-Rate")
if hdr is None:
    print(f"NOTE: no X-Sample-Rate header (older firmware?) -- assuming {FS_FALLBACK:.0f} Sps")
fs = float(hdr) if hdr else FS_FALLBACK
x  = np.asarray(r0.text.split(), dtype=float)
N  = len(x)
print(f"OK: {N} samples @ {fs:.1f} Sps  ->  span 0 to {fs/2:.0f} Hz, bin {fs/N:.2f} Hz")
print("controls: click plot window, then  b = capture baseline (TUBE OFF)   s = toggle subtract   n = toggle notch")

win    = np.hamming(N)
freqs  = np.fft.rfftfreq(N, 1.0/fs)
spects = collections.deque(maxlen=AVG_N)   # recent power spectra
baseline = None
sub_on   = False
notch_on = bool(NOTCH_HZ)

def power(sig):
    X = np.fft.rfft((sig - sig.mean()) * win)
    return np.abs(X) ** 2

def capture_baseline():
    global baseline, sub_on
    print(f"\n== BASELINE: TUBE OFF, DC line connected. Grabbing {BASE_N} captures ==")
    acc = []
    for i in range(BASE_N):
        try:
            _, s = fetch()
            if len(s) != N:
                print("  length changed, skipping"); continue
            acc.append(power(s))
            print(f"  {i+1}/{BASE_N}")
        except Exception as e:
            print("  capture failed:", e)
    if acc:
        baseline = np.mean(np.stack(acc), axis=0)
        sub_on = True
        print("== baseline stored, subtraction ON ('s' toggles) ==\n")

def on_key(ev):
    global sub_on, notch_on
    if ev.key == 'b':
        capture_baseline()
    elif ev.key == 's':
        sub_on = not sub_on
        print("subtraction:", "ON" if sub_on else "OFF")
    elif ev.key == 'n':
        notch_on = not notch_on
        print("notches:", "ON" if notch_on else "OFF")

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7))
(wave_ln,) = ax1.plot([], [], lw=0.8)
ax1.set_xlabel("ms"); ax1.set_ylabel("ADC counts"); ax1.set_title("waveform")
(base_ln,) = ax2.plot([], [], lw=0.8, color="0.75", label="baseline (electronics)")
(spec_ln,) = ax2.plot([], [], lw=1.0, label="live")
peak_vl = ax2.axvline(0
