# cwskimmer

A real-time CW (Morse code) skimmer for amateur radio. Connects to an SDR or the Icom IC-7300, computes a spectrum up to 500 times per second, and simultaneously decodes Morse code across up to 48 channels — all without manual tuning.

![cw skimmer screenshot](/cw-skimmer.png?raw=true)

---

## Features

- **48-channel simultaneous CW decode** across a ~4.7 kHz wide slice of spectrum
- **500 FFT frames/second** — 2048-point FFT over a 192 kHz IQ stream (SDR mode)
- **Automatic speed detection**, 5–40 WPM via PARIS timing
- **Live waterfall + spectrum display** of the full 192 kHz band and the 48-bin decode window
- **Tunable center frequency** via mouse wheel, left-click on spectrum, or numeric keypad
- **Selectable decode window** — center bin and width adjustable at runtime
- **IC-7300 CI-V integration** — the app reads and sets the radio's VFO frequency automatically via the Icom CI-V serial protocol; turning the VFO knob updates the skimmer display in real time
- **Multiple SDR backends**: RTL-SDR, SDRplay RSP v2/v3, HackRF One, IC-7300 USB audio, WAV file replay

---

## How It Works

The core pipeline (SDR / wideband mode):

```
SDR hardware → IQ samples (192 kHz) → RingBuffer
    → 2048-pt FFT every 2 ms
        → 48 × elementHandler  (~94 Hz/bin)
            → CW tone/space detector → Morse decoder → text display
```

**Bin width**: 192000 / 2048 ≈ **93.75 Hz** — tight enough that a single CW signal usually spans 1–2 bins.

**Timing**: Each bin receives ~500 magnitude samples per second. A PARIS dot at 1 WPM = 1200 ms = 600 samples; at 20 WPM = 30 samples. The decoder auto-calibrates dot length from the shortest observed tone in a 14-element sliding window.

**AGC per bin**: A decaying-average peak detector per `elementHandler` adjusts decode thresholds to band noise automatically.

---

## Supported Hardware

| Device | CONFIG flag | Notes |
|---|---|---|
| RTL-SDR (DVB-T dongle) | `CONFIG += rtlsdr` | 8-bit, 192 kHz IQ, requires `librtlsdr` |
| SDRplay RSP1/RSP2/RSPdx (API v2) | `CONFIG += sdrplay-v2` | requires Mirics API installer |
| SDRplay (API v3) | `CONFIG += sdrplay-v3` | preferred for all current RSP devices |
| HackRF One | `CONFIG += hackrf` | 8-bit, requires `libhackrf` |
| Icom IC-7300 | `CONFIG += ic7300` | USB AF audio at 48 kHz + CI-V VFO control — see below |
| WAV file | always included | 16-bit stereo IQ `.wav` at 192 kHz |

---

## IC-7300 Setup

### What the IC-7300 provides over USB

The IC-7300 is **not an SDR**. Its USB connection exposes two separate interfaces:

| Interface | Purpose |
|---|---|
| USB Audio Class | Narrow-band AF receive audio (NOT IQ). Max 48 kHz. Bandwidth limited to IF filter width (~2.4 kHz SSB, ~3.6 kHz CW-wide). |
| USB CDC Serial | Icom CI-V control port for reading and setting the VFO frequency. |

Because the audio bandwidth is narrow, cwskimmer operates as a **single-channel CW decoder** when using the IC-7300, not a 48-channel wideband skimmer. For wideband skimming use a dedicated SDR.

### Audio setup

1. Connect the IC-7300 via USB cable.
2. On the radio: **MENU → SET → Connectors → USB(AF)**
   - **Output Select** → `AF`
   - **MOD Input** → `USB` *(only needed for transmitting digital modes)*
3. The radio appears in your OS as **"USB Audio CODEC"** at 48000 Hz.
4. Verify: `arecord -l | grep -i codec`

### CI-V frequency control setup

CI-V lets the app read and set the radio's VFO automatically. When you turn the VFO knob on the radio, the display in cwskimmer updates immediately; when you click a frequency in the spectrum, the radio tunes to it.

On the radio:

| Menu path | Setting |
|---|---|
| MENU → SET → Connectors → CI-V → **CI-V Address** | Note the value (default: **94** hex) |
| MENU → SET → Connectors → CI-V → **USB CI-V Baud Rate** | Set to **115200** (or match what you select in the app) |
| MENU → SET → Connectors → CI-V → **CI-V Transceive** | **ON** — enables automatic VFO broadcasts when the knob moves |

The CI-V serial port appears on Linux as `/dev/ttyUSB0` or `/dev/ttyACM0`. Verify:
```bash
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

If access is denied, add yourself to the `dialout` group:
```bash
sudo usermod -aG dialout $USER   # log out and back in
```

### Using CI-V in the app

1. Select **ic7300** in the device dialog; pick **"USB Audio CODEC"** from the audio combo.
2. In the **CI-V Frequency Control** section of the IC-7300 widget:
   - Select the serial port (e.g. `/dev/ttyUSB0 (IC-7300 USB Serial Port)`)
   - Set **Baud** to match the radio's USB CI-V baud rate (115200 recommended)
   - Set **Addr** to the radio's CI-V address (default `94` hex)
   - Click **Connect**
3. The status line shows the current VFO frequency. Turning the VFO knob updates it live.
4. Clicking a frequency in the waterfall sends a CI-V tune command to the radio.

CI-V is **optional** — the audio capture works independently whether CI-V is connected or not.

---

## Build Dependencies

### Linux (Debian/Ubuntu)

```bash
sudo apt install \
    qt5-qmake qtbase5-dev qtbase5-dev-tools \
    libqwt-qt5-dev \
    libfftw3-dev libsndfile1-dev libsamplerate0-dev \
    portaudio19-dev \
    libusb-1.0-0-dev
```

For IC-7300 CI-V support (included by default):
```bash
sudo apt install libqt5serialport5-dev
```

For RTL-SDR:
```bash
sudo apt install librtlsdr-dev
```

For HackRF:
```bash
sudo apt install libhackrf-dev
```

For SDRplay: download the API installer from [sdrplay.com/downloads](https://www.sdrplay.com/downloads/).

### Fedora / RHEL

```bash
sudo dnf install \
    qt5-qtbase-devel qwt-qt5-devel qt5-qtserialport-devel \
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
# binary: ./linux-bin/cwSkimmer
```

To build with a subset of device backends:

```bash
# RTL-SDR + IC-7300 only
qmake "CONFIG += rtlsdr ic7300" cwskimmer.pro && make -j$(nproc)

# Everything
qmake "CONFIG += rtlsdr sdrplay-v3 hackrf ic7300" cwskimmer.pro && make -j$(nproc)
```

---

## Running

```bash
./linux-bin/cwSkimmer
```

On first run a device selector dialog appears. The choice is saved and reused on the next run.

**RTL-SDR udev rule** (access without root):
```bash
sudo cp appimage/udev-rules-helper /etc/udev/rules.d/rtlsdr.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## Controls

| Control | Action |
|---|---|
| Mouse wheel on spectrum | Tune center frequency by step size |
| Left-click on spectrum | Jump to that frequency |
| Right-click on spectrum | Toggle waterfall / spectrum view |
| **Freq** button | Open numeric keypad for direct frequency entry |
| **Center bin** spinbox | Move the 48-bin decode window left/right |
| **Width** spinbox | Narrow or widen the decode window (1–48 bins) |
| **Mouse inc** spinbox | Set wheel step size in Hz |
| **Reset** button | Clear all decoded text |

---

## Architecture

```
cwskimmer/
├── main.cpp                       — entry point, QSettings init
├── radio.cpp / radio.h            — main window, FFT pipeline, device glue
├── element-handler.cpp / .h       — per-bin CW decoder (AGC, state machine, Morse lookup)
├── fft-complex.cpp / .h           — Cooley-Tukey radix-2 FFT; cached twiddle table
├── output-list.cpp / .h           — 48-row decoded-text table
├── radio-constants.h              — frequency macros, DSPCOMPLEX, get_db()
│
├── devices/
│   ├── device-handler.h / .cpp    — abstract base (QThread + dataAvailable signal)
│   ├── deviceselect.cpp / .h      — device picker dialog
│   ├── filereader/                — WAV file IQ replay (libsndfile)
│   ├── rtlsdr-handler/            — RTL-SDR via librtlsdr (dlopen, 2-stage decimating FIR)
│   ├── sdrplay-handler-v2/        — SDRplay Mirics API v2
│   ├── sdrplay-handler-v3/        — SDRplay API v3
│   ├── hackrf-handler/            — HackRF via libhackrf
│   └── ic7300-handler/            — IC-7300 USB AF audio (PortAudio) + CI-V (QSerialPort)
│
├── filters/
│   ├── fir-filters.cpp / .h       — FIR lowpass, bandpass, decimating, Hilbert, adaptive
│   └── iir-filters.cpp / .h       — IIR filters
│
├── scopes-qwt6/
│   ├── fft-scope                  — 192 kHz wideband spectrum / waterfall (Qwt)
│   ├── spectrum-scope             — 48-bin narrow-band spectrum
│   └── waterfall-scope            — time-frequency waterfall
│
└── various/
    ├── ringbuffer.h               — lock-free SPSC ring buffer (PortAudio lineage)
    ├── averager.cpp / .h          — O(1) running-sum sliding-window averager
    └── popup-keypad.cpp / .h      — on-screen numeric keypad
```

### Signal flow (wideband SDR mode)

1. **Device thread** fills `RingBuffer<complex<float>>` at 192 kHz; emits `dataAvailable(int)`.
2. **`sampleHandler()`** drains the ring buffer in 384-sample (2 ms) chunks, zero-pads to 2048, calls `processBuffer()`.
3. **`processBuffer()`** copies the chunk into `fftWorkBuf` (heap member, not stack), runs a 2048-pt FFT using a pre-computed twiddle table (no per-call malloc), extracts magnitude at each of the 48 active bin indices.
4. **`elementHandler::process()`** per bin: decaying-average AGC, 3-state machine (IDLE → TONE → SPACE → IDLE), appends tone/space durations to a 32-element circular queue.
5. **`elementHandler::add()`** every time a Morse element completes: sorts a 14-element window of recent durations; estimates dot length from shortest tone; classifies each tone as dot/dash (`> 1.5 × dotGuess`); looks up the Morse symbol; emits `updateText(binIndex, wpm, text)`.
6. **`outputList`** receives `updateText` and updates the table row for that bin.

### IC-7300 CI-V frequency sync

When CI-V is connected, a bidirectional frequency sync runs on the Qt main thread:

- **Radio → app**: CI-V transceive broadcasts (command `0x00`) or poll responses (`0x03`) are parsed in `parseCIVBuffer()`; a new VFO frequency triggers `emit frequencyChanged(freq)` → `RadioInterface::setFrequency()` updates the display and resets all decoder bin offsets.
- **App → radio**: clicking a frequency or using the mouse wheel calls `setVFOFrequency(f)`; if `f != vfoFrequency` a CI-V set-frequency frame (command `0x05`, 5-byte BCD) is sent on the serial port.
- **Loop prevention**: `vfoFrequency` is updated *before* emitting or sending, so the inevitable echo/response carrying the same value is silently discarded.

---

## Known Issues

- **IC-7300 narrow bandwidth** — USB audio is AF only (not IQ), so the skimmer decodes a single channel limited by the IF filter width (~2.4 kHz SSB, ~3.6 kHz CW-wide). Use a dedicated SDR for 48-channel wideband operation.

---

## License

GNU General Public License v2.0 — see [COPYING](COPYING).

Original author: Jan van Katwijk (J.vanKatwijk@gmail.com), Lazy Chair Computing.  
IC-7300 support, CI-V integration, bug fixes and optimisations: VU3RAZ.
