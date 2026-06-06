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
 * -------------------------------------------------------------------------
 * IC-7300 USB Audio — what the radio actually provides
 * -------------------------------------------------------------------------
 * The IC-7300 exposes a standard USB Audio Class device that carries
 * AF (audio-frequency) receive audio — NOT raw IQ samples.
 *
 *   Supported sample rates : 8000, 16000, 22050, 44100, 48000 Hz
 *   Channels               : mono (the radio sends the same audio on L & R)
 *   Format                 : 16-bit PCM
 *   IQ output              : NOT available over USB on the IC-7300
 *   192 kHz                : NOT supported
 *
 * This handler captures the AF audio at the device's native rate
 * (Pa_GetDeviceInfo → defaultSampleRate, typically 48000 Hz), converts
 * each real AF sample to a complex<float> with Q = 0, and feeds the ring
 * buffer exactly as the other device handlers do.
 *
 * Practical consequence for the skimmer
 * --------------------------------------
 * Because the input is narrow-band AF audio (limited by the IC-7300's
 * IF filter, typically 2.4 kHz for SSB or up to ~3 kHz for the wide CW
 * filter), the skimmer effectively operates as a SINGLE-CHANNEL CW
 * decoder rather than a 48-channel wideband skimmer.  For wideband
 * skimming across 4+ kHz, a dedicated SDR (RTL-SDR, SDRplay, HackRF)
 * is needed.
 *
 * Radio setup
 * -----------
 * IC-7300 menu: MENU → SET → Connectors → USB(AF):
 *   Output Select : AF (default)
 *   MOD Input     : USB  (for transmit; not needed for receive-only skimming)
 * The radio will appear in your OS as "USB Audio CODEC".
 */

#include	"ic7300-handler.h"
#include	"radio.h"
#include	<QSettings>
#include	<stdio.h>

// Frames per callback block.  Power-of-2 for smooth I/O; 512 @ 48 kHz ≈ 10 ms.
static constexpr int IC7300_FRAMES = 512;

// Static conversion buffer — one slot per frame, avoids per-callback allocation.
// Safe because the PA callback runs on a single dedicated thread.
static std::complex<float> ic7300ConvBuf [IC7300_FRAMES];

//----------------------------------------------------------------------
// PortAudio callback — runs on the PA thread, not the Qt main thread.
//----------------------------------------------------------------------
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

	// The IC-7300 sends mono AF audio; both channels carry the same signal.
	// Store as complex with Q = 0 so the downstream FFT pipeline receives
	// real-valued input (spectrum folds onto positive frequencies only).
	int channels = self -> inputChannels;
	for (unsigned long i = 0; i < framesPerBuffer; i++)
	   ic7300ConvBuf [i] = std::complex<float> (src [i * channels] / 32768.0f,
	                                             0.0f);

	self -> _I_Buffer -> putDataIntoBuffer (ic7300ConvBuf, (int32_t)framesPerBuffer);

	if (self -> _I_Buffer -> GetRingBufferReadAvailable () >
	                              (uint32_t)(self -> outputRate / 10))
	   self -> newdataAvailable (self -> outputRate / 10);

	return paContinue;
}

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------
	ic7300Handler::ic7300Handler (RadioInterface *mr,
	                               RingBuffer<std::complex<float>> *b,
	                               QSettings *s) :
	                                   myFrame (nullptr),
	                                   paStream (nullptr),
	                                   paDeviceIndex (-1),
	                                   inputChannels (1),
	                                   vfoFrequency (14070000) {
	(void)mr;
	_I_Buffer  = b;
	mySettings = s;
	outputRate = 48000;		// updated by openDevice() to actual device rate

	setupUi (&myFrame);
	myFrame. show ();

	PaError err = Pa_Initialize ();
	if (err != paNoError) {
	   fprintf (stderr, "IC-7300: Pa_Initialize failed: %s\n",
	            Pa_GetErrorText (err));
	   throw 42;
	}

	populateDevices ();

	// Restore previously selected device
	QString saved = s -> value ("ic7300/device", ""). toString ();
	int idx = deviceCombo -> findText (saved);
	if (idx >= 0)
	   deviceCombo -> setCurrentIndex (idx);

	connect (deviceCombo, SIGNAL (activated (int)),
	         this, SLOT (deviceSelected (int)));

	paDeviceIndex = deviceCombo -> currentData (). toInt ();
	if (paDeviceIndex < 0) {
	   statusLabel -> setText ("No audio input device found");
	   fprintf (stderr, "IC-7300: no PortAudio input device available\n");
	   Pa_Terminate ();
	   throw 43;
	}

	if (!openDevice (paDeviceIndex)) {
	   Pa_Terminate ();
	   throw 44;
	}
}

//----------------------------------------------------------------------
// Populate combo box with all PortAudio devices that have ≥1 input channel
//----------------------------------------------------------------------
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

//----------------------------------------------------------------------
// Open PortAudio stream at the device's native sample rate
//----------------------------------------------------------------------
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

	// Use the device's native rate; the IC-7300 tops out at 48000 Hz.
	int nativeRate = (int)info -> defaultSampleRate;
	int nativeCh   = (info -> maxInputChannels >= 2) ? 2 : 1;

	PaStreamParameters params;
	params. device                    = idx;
	params. channelCount              = nativeCh;
	params. sampleFormat              = paInt16;
	params. suggestedLatency          = info -> defaultLowInputLatency;
	params. hostApiSpecificStreamInfo = nullptr;

	PaError err = Pa_OpenStream (&paStream,
	                              &params,
	                              nullptr,
	                              nativeRate,
	                              IC7300_FRAMES,
	                              paClipOff,
	                              paCallback,
	                              this);
	if (err != paNoError) {
	   fprintf (stderr, "IC-7300: Pa_OpenStream: %s\n",
	            Pa_GetErrorText (err));
	   statusLabel -> setText (
	         QString ("Open failed: %1").arg (Pa_GetErrorText (err)));
	   paStream = nullptr;
	   return false;
	}

	outputRate    = nativeRate;
	inputChannels = nativeCh;

	statusLabel -> setText (
	      QString ("%1  |  %2 Hz  |  AF mono  |  16-bit")
	      .arg (QString::fromLocal8Bit (info -> name))
	      .arg (nativeRate));

	fprintf (stderr, "IC-7300: opened '%s' at %d Hz, %d ch\n",
	         info -> name, nativeRate, nativeCh);
	return true;
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
	ic7300Handler::~ic7300Handler () {
	stopReader ();
	if (paStream)
	   Pa_CloseStream (paStream);
	Pa_Terminate ();
	myFrame. hide ();
}

//----------------------------------------------------------------------
// deviceHandler interface
//----------------------------------------------------------------------
bool	ic7300Handler::restartReader () {
	if (!paStream)
	   return false;
	PaError err = Pa_StartStream (paStream);
	if (err != paNoError) {
	   fprintf (stderr, "IC-7300: Pa_StartStream: %s\n",
	            Pa_GetErrorText (err));
	   return false;
	}
	return true;
}

void	ic7300Handler::stopReader () {
	if (paStream && Pa_IsStreamActive (paStream) == 1)
	   Pa_StopStream (paStream);
}

void	ic7300Handler::setVFOFrequency (int32_t f) {
	vfoFrequency = f;
	// Optional: send CI-V command via serial port here to tune the radio
}

int32_t	ic7300Handler::getVFOFrequency () {
	return vfoFrequency;
}

int16_t	ic7300Handler::bitDepth () {
	return 16;
}

void	ic7300Handler::newdataAvailable (int n) {
	dataAvailable (n);
}

//----------------------------------------------------------------------
// Slot — user switched device in the combo box
//----------------------------------------------------------------------
void	ic7300Handler::deviceSelected (int comboIndex) {
int	idx        = deviceCombo -> itemData (comboIndex). toInt ();
bool	wasRunning = paStream && (Pa_IsStreamActive (paStream) == 1);

	if (wasRunning)
	   stopReader ();

	paDeviceIndex = idx;
	if (openDevice (idx)) {
	   mySettings -> setValue ("ic7300/device",
	                            deviceCombo -> currentText ());
	   if (wasRunning)
	      restartReader ();
	}
}
