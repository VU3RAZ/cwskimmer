#
/*
 *    Copyright (C) 2024
 *    Rahul (VU3RAZ)
 *
 *    This file is part of the cwSkimmer
 *
 *    cwSkimmer is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    cwSkimmer is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with cwSkimmer; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ── IC-7300 USB audio ────────────────────────────────────────────────────────
 * The IC-7300 presents a USB Audio Class device carrying narrow-band AF
 * receive audio (NOT raw IQ).  Supported sample rates: up to 48000 Hz.
 * Radio setup: MENU → SET → Connectors → USB(AF) → Output Select: AF
 *
 * ── IC-7300 CI-V frequency control ───────────────────────────────────────────
 * The IC-7300 also exposes a USB CDC serial port for Icom CI-V control.
 * On Linux it appears as /dev/ttyUSB0 or /dev/ttyACM0.
 *
 * CI-V frame format:   FE FE <dst> <src> <cmd> [data...] FD
 * Controller address:  0xE0  (this software)
 * Radio address:       0x94  (IC-7300 default; configurable in radio menu)
 *
 * Commands used:
 *   0x03  Read operating frequency  (request → radio → response with 5 BCD bytes)
 *   0x05  Set operating frequency   (send 5 BCD bytes → radio)
 *   0x00  Transceive frequency      (radio broadcasts this when VFO changes)
 *
 * Frequency encoding: 5 bytes BCD, little-endian (LSB first), 2 digits/byte.
 *   Example: 14.070.000 Hz → 0x00 0x00 0x07 0x14 0x00
 *
 * Radio CI-V menu:
 *   MENU → SET → Connectors → CI-V → CI-V Address    (default: 94 hex)
 *   MENU → SET → Connectors → CI-V → USB CI-V Baud Rate  (default: Auto)
 *   MENU → SET → Connectors → CI-V → CI-V Transceive     (enable for auto VFO updates)
 */

#include	"ic7300-handler.h"
#include	"radio.h"
#include	<QSettings>
#include	<QSerialPortInfo>
#include	<stdio.h>

static constexpr int IC7300_FRAMES       = 512;   // PA frames/callback (~10 ms @ 48 kHz)
static constexpr int CIV_CTRL_ADDR       = 0xE0;  // our controller address
static constexpr int CIV_POLL_INTERVAL   = 2000;  // ms — fallback poll when transceive off

// Static buffer for the PA callback (single PA thread; no locking needed).
static std::complex<float> ic7300ConvBuf [IC7300_FRAMES];

// ═══════════════════════════════════════════════════════════════════════════
// PortAudio callback
// ═══════════════════════════════════════════════════════════════════════════
int	ic7300Handler::paCallback (const void *inputBuffer,
	                            void *,
	                            unsigned long framesPerBuffer,
	                            const PaStreamCallbackTimeInfo *,
	                            PaStreamCallbackFlags,
	                            void *userData) {
ic7300Handler *self = static_cast<ic7300Handler *> (userData);
const int16_t *src  = static_cast<const int16_t *> (inputBuffer);

	if (!src)
	   return paContinue;

	if (framesPerBuffer > IC7300_FRAMES)
	   framesPerBuffer = IC7300_FRAMES;

	int ch = self -> inputChannels;
	for (unsigned long i = 0; i < framesPerBuffer; i++)
	   ic7300ConvBuf [i] = std::complex<float> (src [i * ch] / 32768.0f, 0.0f);

	self -> _I_Buffer -> putDataIntoBuffer (ic7300ConvBuf, (int32_t)framesPerBuffer);

	if (self -> _I_Buffer -> GetRingBufferReadAvailable () >
	                              (uint32_t)(self -> outputRate / 10))
	   self -> newdataAvailable (self -> outputRate / 10);

	return paContinue;
}

// ═══════════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════════
	ic7300Handler::ic7300Handler (RadioInterface *mr,
	                               RingBuffer<std::complex<float>> *b,
	                               QSettings *s) :
	                                   myFrame (nullptr),
	                                   paStream (nullptr),
	                                   paDeviceIndex (-1),
	                                   inputChannels (1),
	                                   vfoFrequency (14070000),
	                                   civPort (nullptr),
	                                   civAddress (0x94),
	                                   civPollTimer (nullptr) {
	(void)mr;
	_I_Buffer  = b;
	mySettings = s;
	outputRate = 48000;

	setupUi (&myFrame);
	myFrame. show ();

	// ── Audio init ──
	PaError err = Pa_Initialize ();
	if (err != paNoError) {
	   fprintf (stderr, "IC-7300: Pa_Initialize: %s\n", Pa_GetErrorText (err));
	   throw 42;
	}
	populateDevices ();

	QString savedDev = s -> value ("ic7300/device", ""). toString ();
	int devIdx = deviceCombo -> findText (savedDev);
	if (devIdx >= 0) deviceCombo -> setCurrentIndex (devIdx);

	connect (deviceCombo, SIGNAL (activated (int)),
	         this, SLOT (deviceSelected (int)));

	paDeviceIndex = deviceCombo -> currentData (). toInt ();
	if (paDeviceIndex < 0) {
	   statusLabel -> setText ("No audio input device found");
	   Pa_Terminate ();
	   throw 43;
	}
	if (!openDevice (paDeviceIndex)) {
	   Pa_Terminate ();
	   throw 44;
	}

	// ── CI-V init ──
	populateCIVPorts ();

	// Restore baud rate (default: 115200)
	QString savedBaud = s -> value ("ic7300/civBaud", "115200"). toString ();
	int baudIdx = civBaudCombo -> findText (savedBaud);
	civBaudCombo -> setCurrentIndex (baudIdx >= 0 ? baudIdx : 5);

	// Restore CI-V address (default: "94" hex)
	civAddressEdit -> setText (
	   s -> value ("ic7300/civAddr", "94"). toString ());

	// Restore last port (select if still present)
	QString savedPort = s -> value ("ic7300/civPort", ""). toString ();
	int portIdx = civPortCombo -> findData (savedPort);
	if (portIdx >= 0) civPortCombo -> setCurrentIndex (portIdx);

	connect (civConnectButton, SIGNAL (clicked ()),
	         this, SLOT (civConnectClicked ()));
	connect (civRefreshButton, SIGNAL (clicked ()),
	         this, SLOT (civRefreshPorts ()));
}

// ═══════════════════════════════════════════════════════════════════════════
// Destructor
// ═══════════════════════════════════════════════════════════════════════════
	ic7300Handler::~ic7300Handler () {
	stopReader ();
	closeCIV ();
	if (paStream) Pa_CloseStream (paStream);
	Pa_Terminate ();
	myFrame. hide ();
}

// ═══════════════════════════════════════════════════════════════════════════
// Audio — populate device combo
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::populateDevices () {
	int n = Pa_GetDeviceCount ();
	deviceCombo -> clear ();
	for (int i = 0; i < n; i++) {
	   const PaDeviceInfo *info = Pa_GetDeviceInfo (i);
	   if (info && info -> maxInputChannels >= 1)
	      deviceCombo -> addItem (
	            QString::fromLocal8Bit (info -> name), i);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// Audio — open PortAudio stream at device's native rate
// ═══════════════════════════════════════════════════════════════════════════
bool	ic7300Handler::openDevice (int idx) {
	if (paStream) {
	   Pa_CloseStream (paStream);
	   paStream = nullptr;
	}

	const PaDeviceInfo *info = Pa_GetDeviceInfo (idx);
	if (!info) {
	   statusLabel -> setText ("Invalid device");
	   return false;
	}

	int nativeRate = (int)info -> defaultSampleRate;
	int nativeCh   = (info -> maxInputChannels >= 2) ? 2 : 1;

	PaStreamParameters params;
	params. device                    = idx;
	params. channelCount              = nativeCh;
	params. sampleFormat              = paInt16;
	params. suggestedLatency          = info -> defaultLowInputLatency;
	params. hostApiSpecificStreamInfo = nullptr;

	PaError err = Pa_OpenStream (&paStream, &params, nullptr,
	                              nativeRate, IC7300_FRAMES,
	                              paClipOff, paCallback, this);
	if (err != paNoError) {
	   statusLabel -> setText (
	         QString ("Open failed: %1").arg (Pa_GetErrorText (err)));
	   paStream = nullptr;
	   return false;
	}

	outputRate    = nativeRate;
	inputChannels = nativeCh;
	statusLabel -> setText (
	      QString ("%1  |  %2 Hz  |  AF mono  |  16-bit")
	      .arg (QString::fromLocal8Bit (info -> name)).arg (nativeRate));
	fprintf (stderr, "IC-7300 audio: '%s' %d Hz %d ch\n",
	         info -> name, nativeRate, nativeCh);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Audio — deviceHandler interface
// ═══════════════════════════════════════════════════════════════════════════
bool	ic7300Handler::restartReader () {
	if (!paStream) return false;
	PaError err = Pa_StartStream (paStream);
	if (err != paNoError) {
	   fprintf (stderr, "IC-7300: Pa_StartStream: %s\n", Pa_GetErrorText (err));
	   return false;
	}
	return true;
}

void	ic7300Handler::stopReader () {
	if (paStream && Pa_IsStreamActive (paStream) == 1)
	   Pa_StopStream (paStream);
}

int16_t	ic7300Handler::bitDepth () { return 16; }

void	ic7300Handler::newdataAvailable (int n) { dataAvailable (n); }

// ═══════════════════════════════════════════════════════════════════════════
// Audio — slot: user changed the audio device combo
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::deviceSelected (int comboIndex) {
int	idx        = deviceCombo -> itemData (comboIndex). toInt ();
bool	wasRunning = paStream && (Pa_IsStreamActive (paStream) == 1);

	if (wasRunning) stopReader ();
	paDeviceIndex = idx;
	if (openDevice (idx)) {
	   mySettings -> setValue ("ic7300/device", deviceCombo -> currentText ());
	   if (wasRunning) restartReader ();
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — populate port combo from QSerialPortInfo
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::populateCIVPorts () {
	civPortCombo -> clear ();
	for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts ()) {
	   QString label = info. systemLocation ();
	   if (!info. description (). isEmpty ())
	      label += QString (" (%1)").arg (info. description ());
	   civPortCombo -> addItem (label, info. systemLocation ());
	}
	if (civPortCombo -> count () == 0)
	   civPortCombo -> addItem ("(no serial ports found)", "");
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — slot: refresh port list
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::civRefreshPorts () {
	QString current = civPortCombo -> currentData (). toString ();
	populateCIVPorts ();
	int idx = civPortCombo -> findData (current);
	if (idx >= 0) civPortCombo -> setCurrentIndex (idx);
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — slot: Connect / Disconnect button
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::civConnectClicked () {
	if (civPort && civPort -> isOpen ())
	   closeCIV ();
	else
	   openCIV ();
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — open serial port
// ═══════════════════════════════════════════════════════════════════════════
bool	ic7300Handler::openCIV () {
	QString portLoc = civPortCombo -> currentData (). toString ();
	if (portLoc. isEmpty ()) {
	   civStatusLabel -> setText ("Select a serial port first");
	   return false;
	}

	bool ok;
	civAddress = (uint8_t)(civAddressEdit -> text (). toUInt (&ok, 16));
	if (!ok || civAddress == 0) {
	   civStatusLabel -> setText ("Invalid CI-V address (e.g. 94)");
	   return false;
	}

	int baud = civBaudCombo -> currentText (). toInt ();

	civPort = new QSerialPort (this);
	civPort -> setPortName (portLoc);
	civPort -> setBaudRate (baud);
	civPort -> setDataBits (QSerialPort::Data8);
	civPort -> setStopBits (QSerialPort::OneStop);
	civPort -> setParity   (QSerialPort::NoParity);
	civPort -> setFlowControl (QSerialPort::NoFlowControl);

	if (!civPort -> open (QIODevice::ReadWrite)) {
	   civStatusLabel -> setText (
	         QString ("Failed: %1").arg (civPort -> errorString ()));
	   delete civPort;
	   civPort = nullptr;
	   return false;
	}

	connect (civPort, SIGNAL (readyRead ()), this, SLOT (civDataReady ()));

	// 2-second backup poll (primary updates come from CI-V transceive)
	civPollTimer = new QTimer (this);
	connect (civPollTimer, SIGNAL (timeout ()), this, SLOT (civPollFreq ()));
	civPollTimer -> start (CIV_POLL_INTERVAL);

	// Lock the UI controls while connected
	civConnectButton  -> setText ("Disconnect");
	civPortCombo      -> setEnabled (false);
	civBaudCombo      -> setEnabled (false);
	civAddressEdit    -> setEnabled (false);
	civRefreshButton  -> setEnabled (false);

	// Persist settings
	mySettings -> setValue ("ic7300/civPort", portLoc);
	mySettings -> setValue ("ic7300/civBaud", civBaudCombo -> currentText ());
	mySettings -> setValue ("ic7300/civAddr", civAddressEdit -> text ());

	civStatusLabel -> setText (
	      QString ("Connected  |  addr 0x%1  |  %2 baud")
	      .arg (civAddress, 2, 16, QLatin1Char ('0'))
	      .arg (baud));

	fprintf (stderr, "IC-7300 CI-V: %s  addr=0x%02X  baud=%d\n",
	         portLoc. toLocal8Bit (). constData (), civAddress, baud);

	// Request current VFO immediately
	sendCIVReadFreq ();
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — close serial port
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::closeCIV () {
	if (civPollTimer) {
	   civPollTimer -> stop ();
	   delete civPollTimer;
	   civPollTimer = nullptr;
	}
	if (civPort) {
	   disconnect (civPort, SIGNAL (readyRead ()), this, SLOT (civDataReady ()));
	   civPort -> close ();
	   delete civPort;
	   civPort = nullptr;
	}
	civRxBuffer. clear ();

	civConnectButton  -> setText ("Connect");
	civPortCombo      -> setEnabled (true);
	civBaudCombo      -> setEnabled (true);
	civAddressEdit    -> setEnabled (true);
	civRefreshButton  -> setEnabled (true);
	civStatusLabel    -> setText ("Not connected");
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — slot: serial data ready
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::civDataReady () {
	civRxBuffer. append (civPort -> readAll ());
	parseCIVBuffer ();
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — slot: poll timer fired (fallback when transceive is off)
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::civPollFreq () {
	sendCIVReadFreq ();
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — parse the receive buffer for complete CI-V frames
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::parseCIVBuffer () {
	while (true) {
	   // Find preamble FE FE
	   int start = -1;
	   for (int i = 0; i + 1 < civRxBuffer. size (); i++) {
	      if ((uint8_t)civRxBuffer [i]     == 0xFE &&
	          (uint8_t)civRxBuffer [i + 1] == 0xFE) {
	         start = i;
	         break;
	      }
	   }
	   if (start < 0) {
	      civRxBuffer. clear ();
	      return;
	   }
	   if (start > 0)
	      civRxBuffer = civRxBuffer. mid (start);

	   // Find end byte FD
	   int end = -1;
	   for (int i = 2; i < civRxBuffer. size (); i++) {
	      if ((uint8_t)civRxBuffer [i] == 0xFD) {
	         end = i;
	         break;
	      }
	   }
	   if (end < 0)
	      return;  // incomplete frame — wait for more data

	   QByteArray frame   = civRxBuffer. left (end + 1);
	   civRxBuffer        = civRxBuffer. mid  (end + 1);

	   // Minimum valid frame: FE FE dst src cmd FD = 6 bytes
	   if (frame. size () < 6) continue;

	   uint8_t src = (uint8_t)frame [3];
	   uint8_t cmd = (uint8_t)frame [4];

	   // Only process frames that originate from our radio
	   if (src != civAddress) continue;

	   // Command 0x03: response to our read-frequency request
	   // Command 0x00: transceive broadcast when VFO changes
	   // Both carry 5 BCD frequency bytes starting at frame[5]
	   if ((cmd == 0x03 || cmd == 0x00) && frame. size () >= 11) {
	      const uint8_t *bcd = reinterpret_cast<const uint8_t *>(
	                               frame. constData () + 5);
	      int32_t freq = civBCDtoFreq (bcd);
	      if (freq > 0 && freq != vfoFrequency) {
	         vfoFrequency = freq;  // update first to break the send-back loop
	         civStatusLabel -> setText (
	               QString ("VFO: %1 kHz")
	               .arg (freq / 1000.0, 0, 'f', 3));
	         emit frequencyChanged (freq);
	      }
	   }
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — send read-frequency request (command 0x03)
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::sendCIVReadFreq () {
	if (!civPort || !civPort -> isOpen ()) return;
	const char frame [] = {
	   (char)0xFE, (char)0xFE,
	   (char)civAddress,
	   (char)CIV_CTRL_ADDR,
	   (char)0x03,
	   (char)0xFD
	};
	civPort -> write (frame, sizeof frame);
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — send set-frequency command (command 0x05) with 5 BCD bytes
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::sendCIVSetFreq (int32_t freq) {
	if (!civPort || !civPort -> isOpen ()) return;
	uint8_t bcd [5];
	freqToCIVBCD (freq, bcd);
	QByteArray frame;
	frame. append ((char)0xFE);
	frame. append ((char)0xFE);
	frame. append ((char)civAddress);
	frame. append ((char)CIV_CTRL_ADDR);
	frame. append ((char)0x05);
	for (int i = 0; i < 5; i++)
	   frame. append ((char)bcd [i]);
	frame. append ((char)0xFD);
	civPort -> write (frame);
}

// ═══════════════════════════════════════════════════════════════════════════
// VFO frequency — called by RadioInterface when the user tunes in the app
// ═══════════════════════════════════════════════════════════════════════════
void	ic7300Handler::setVFOFrequency (int32_t f) {
	if (f == vfoFrequency) return;  // already at this freq — prevents CI-V echo loop
	vfoFrequency = f;
	if (civPort && civPort -> isOpen ())
	   sendCIVSetFreq (f);
}

int32_t	ic7300Handler::getVFOFrequency () {
	return vfoFrequency;
}

// ═══════════════════════════════════════════════════════════════════════════
// CI-V — BCD ↔ frequency conversion
//
// 5-byte BCD, little-endian, 2 digits/byte.
// Example: 14.070.000 Hz → {0x00, 0x00, 0x07, 0x14, 0x00}
// ═══════════════════════════════════════════════════════════════════════════
int32_t	ic7300Handler::civBCDtoFreq (const uint8_t *bcd) {
int32_t	freq = 0;
int32_t	mult = 1;
	for (int i = 0; i < 5; i++) {
	   freq += (int32_t)(bcd [i] & 0x0F)        * mult;  mult *= 10;
	   freq += (int32_t)((bcd [i] >> 4) & 0x0F) * mult;  mult *= 10;
	}
	return freq;
}

void	ic7300Handler::freqToCIVBCD (int32_t freq, uint8_t *bcd) {
	for (int i = 0; i < 5; i++) {
	   bcd [i]  = (uint8_t)(freq % 10);  freq /= 10;
	   bcd [i] |= (uint8_t)(freq % 10) << 4;  freq /= 10;
	}
}
