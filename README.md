# cwskimmer

A real-time CW (Morse code) skimmer for amateur radio. Connects to an SDR, computes a 192 kHz wide spectrum 500 times per second, and simultaneously decodes Morse code across up to 48 channels — all without manual tuning.

![cw skimmer screenshot](/cw-skimmer.png?raw=true)

---

## Features

- **48-channel simultaneous CW decode** across a ~4.7 kHz wide slice of spectrum
- **500 FFT frames/second** using a 2048-point FFT over 192 kHz bandwidth
- **Automatic speed detection** (5–40 WPM via PARIS timing)
- **Live waterfall + spectrum display** of the full 192 kHz band and the 48-bin decode window
- **Tunable center frequency** via mouse wheel or numeric keypad
- **Selectable decode window** — width (number of bins) and center bin adjustable at runtime
- **Multiple SDR backends**: RTL-SDR, SDRplay v2/v3, HackRF, IC-7300 (USB audio), WAV file replay

---

## How It Works

The core pipeline:

```
SDR hardware → IQ samples (192 kHz) → RingBuffer
    → 2048-pt FFT every 2 ms
        → 48 × elementHandler (each ~94 Hz wide bin)
            → CW tone/space detector → Morse decoder → text display
```

**Bin width**: 192000 / 2048 ≈ **93.75 Hz** per bin — tight enough that a single CW signal usually falls in 1–2 bins.

**Timing**: Each bin receives ~500 magnitude samples per second. A PARIS dot at 1 WPM = 1200 ms = 600 samples; at 20 WPM = 30 samples. The decoder auto-calibrates dot length from the shortest observed tone in a 14-element sliding window.

**AGC per bin**: Each `elementHandler` maintains a decaying-average peak detector so decode thresholds self-adjust to band noise.

---

## Supported Hardware

| Device | CONFIG flag | Notes |
|---|---|---|
| RTL-SDR (DVB-T dongle) | `CONFIG += rtlsdr` | 8-bit, requires `librtlsdr` |
| SDRplay RSP1/RSP2/RSPdx (API v2) | `CONFIG += sdrplay-v2` | requires Mirics API |
| SDRplay (API v3) | `CONFIG += sdrplay-v3` | preferred for newer RSP devices |
| HackRF One | `CONFIG += hackrf` | 8-bit, requires `libhackrf` |
| Icom IC-7300 | `CONFIG += ic7300` | USB AF audio at 48 kHz (single channel, narrow-band) — see below |
| WAV file | always included | 16-bit stereo IQ `.wav` at 192 kHz |

---

## IC-7300 Setup

### What the IC-7300 actually provides over USB

The IC-7300 is **not an SDR**. Its USB audio interface carries narrow-band **AF (audio-frequency) receive audio** — the same signal you would hear in headphones — not raw IQ samples.

| Parameter | IC-7300 USB Audio |
|---|---|
| Signal type | AF audio (NOT IQ) |
| Channels | Mono (same signal on L and R) |
| Sample rates | 8000 / 16000 / 22050 / 44100 / **48000** Hz |
| **192000 Hz** | **Not supported** |
| Bandwidth | Limited to IF filter width (≈ 2.4 kHz SSB, up to 3.6 kHz CW-wide) |

Because the input bandwidth is narrow, the cwskimmer acts as a **single-channel CW decoder** when using the IC-7300 — not a 48-channel wideband skimmer. For wideband skimming across 4+ kHz, use a dedicated SDR (RTL-SDR, SDRplay, HackRF).

### Radio setup

1. Connect the IC-7300 to the PC via USB cable.
2. On the radio: **MENU → SET → Connectors → USB(AF)**
   - Set **Output Select** → `AF`
   - Set **MOD Input** → `USB` *(only needed if you transmit digital modes)*
3. The radio will appear in your OS as **"USB Audio CODEC"** at 48000 Hz.

Verify it is visible:
```bash
arecord -l | grep -i codec
# or
pactl list sources short | grep -i codec
```

4. Build with `CONFIG += ic7300` (see Build section).
5. In the device selector, pick **ic7300**, then choose **"USB Audio CODEC"** from the drop-down.

The handler opens the device at its native sample rate (48000 Hz) automatically — no manual rate setting is needed in the menu.

### Frequency display

Because the IC-7300 USB audio carries demodulated AF (not raw RF), the **frequency display shows the VFO frequency you set manually** on the radio. There is no automatic frequency readback. The handler includes a `setVFOFrequency()` stub ready for optional CI-V serial control if you want to add automatic frequency tracking later.

---

## Build Dependencies

### Linux (Debian/Ubuntu)

```bash
sudo apt install \
    qt5-qmake qtbase5-dev libqwt-qt5-dev \
    libfftw3-dev libsndfile1-dev libsamplerate0-dev \
    libportaudio2 portaudio19-dev \
    libusb-1.0-0-dev
```

For RTL-SDR:
```bash
sudo apt install librtlsdr-dev
```

For HackRF:
```bash
sudo apt install libhackrf-dev
```

For SDRplay: download the API installer from [sdrplay.com](https://www.sdrplay.com/downloads/).

### Fedora / RHEL

```bash
sudo dnf install qt5-qtbase-devel qwt-qt5-devel \
    fftw-devel libsndfile-devel libsamplerate-devel \
    portaudio-devel libusb1-devel
```

---

## Build

```bash
git clone https://github.com/VU3RAZ/cwskimmer.git
cd cwskimmer
qmake cwskimmer.pro
make -j$(nproc)
# binary lands in ./linux-bin/cwSkimmer
```

To build with specific device support, edit `cwskimmer.pro` or pass on the command line:

```bash
# All devices
qmake "CONFIG += rtlsdr sdrplay-v3 hackrf ic7300" cwskimmer.pro

# RTL-SDR only
qmake "CONFIG += rtlsdr" cwskimmer.pro
```

---

## Running

```bash
./linux-bin/cwSkimmer
```

On first run a device selector dialog appears. Your choice is saved in `~/.config/` and reused next time.

**Udev rules** (so the dongle is accessible without root):

```bash
# RTL-SDR
sudo cp appimage/udev-rules-helper /etc/udev/rules.d/rtlsdr.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## Controls

| Control | Action |
|---|---|
| Mouse wheel on spectrum | Tune center frequency by step increment |
| Left-click on spectrum | Jump to that frequency |
| Right-click on spectrum | Toggle waterfall / spectrum view |
| **Freq** button | Open numeric keypad for direct frequency entry |
| **Center bin** spinbox | Move the 48-bin decode window left/right |
| **Width** spinbox | Narrow or widen the decode window (1–48 bins) |
| **Mouse inc** spinbox | Set wheel step size (Hz) |
| **Reset** button | Clear all decoded text |

---

## Architecture

```
cwskimmer/
├── main.cpp                      — entry point, QSettings init
├── radio.cpp / radio.h           — main window, FFT loop, device glue
├── element-handler.cpp / .h      — per-bin CW decoder (AGC, state machine, Morse lookup)
├── fft-complex.cpp / .h          — Cooley-Tukey FFT (power-of-2) + Bluestein (arbitrary)
├── output-list.cpp / .h          — scrolling text output table (48 rows)
├── radio-constants.h             — frequency macros, DSPCOMPLEX typedef, get_db()
│
├── devices/
│   ├── device-handler.h / .cpp   — abstract base class (QThread + dataAvailable signal)
│   ├── deviceselect.cpp / .h     — device picker dialog
│   ├── filereader/               — WAV file IQ replay
│   ├── rtlsdr-handler/           — RTL-SDR via librtlsdr (dynamically loaded)
│   ├── sdrplay-handler-v2/       — SDRplay Mirics API v2
│   ├── sdrplay-handler-v3/       — SDRplay API v3
│   ├── hackrf-handler/           — HackRF via libhackrf
│   └── ic7300-handler/           — IC-7300 USB audio IQ via PortAudio
│
├── filters/
│   ├── fir-filters.cpp / .h      — FIR lowpass, bandpass, decimating, Hilbert
│   └── iir-filters.cpp / .h      — IIR filters
│
├── scopes-qwt6/
│   ├── fft-scope                 — 192 kHz wideband spectrum/waterfall
│   ├── spectrum-scope            — 48-bin narrow spectrum
│   └── waterfall-scope           — time-frequency waterfall
│
└── various/
    ├── ringbuffer.h              — lock-free single-producer/single-consumer ring buffer
    ├── averager.cpp / .h         — sliding-window moving average
    └── popup-keypad.cpp / .h     — on-screen numeric keypad
```

**Signal flow in detail:**

1. Device thread fills `RingBuffer<complex<float>>` at 192 kHz, emits `dataAvailable(int)`.
2. `RadioInterface::sampleHandler()` drains the buffer in 384-sample (2 ms) chunks, zero-pads to 2048, calls `processBuffer()`.
3. `processBuffer()` runs the FFT, extracts magnitude at the 48 bin indices, feeds each `elementHandler::process(magnitude, signalLevel)`.
4. Each `elementHandler` runs a 3-state machine (IDLE → TONE → SPACE → IDLE), records durations, and calls `add()` every time an element completes.
5. `add()` accumulates a 14-element circular queue of tone/space durations, estimates dot length, looks up the Morse symbol, and emits `updateText(binIndex, wpm, decoded_string)`.
6. `outputList` receives the signal and updates the UI table row for that bin.

---

## Known Issues / Planned Fixes

- `rtlsdr-handler.cpp:197` — gain enumeration loop has `i++` instead of `i--` (infinite loop / crash on multi-gain devices). **Fix pending.**
- `filereader.cpp:77` — division by zero if `playTime == 0` during progress bar update.
- `radio.cpp:259` — `sleep(1)` in GUI thread on quit; causes 1-second freeze.
- `radio.cpp:337` — `QWheelEvent::delta()` deprecated; should use `angleDelta().y()`.
- Hardcoded `192000` / `192` literals in `radio.cpp` (lines 287, 318, 347) should use `inputRate`.
- FFT twiddle table re-allocated via `malloc` on every 2ms call — needs caching for fixed size.

---

## License

GNU General Public License v2.0 — see [COPYING](COPYING).

Original author: Jan van Katwijk (J.vanKatwijk@gmail.com), Lazy Chair Computing.  
IC-7300 support and additional fixes: VU3RAZ.
