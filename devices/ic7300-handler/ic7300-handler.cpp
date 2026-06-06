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
 */

#include	"ic7300-handler.h"
#include	"radio.h"
#include	<QSettings>
#include	<stdio.h>

// IC-7300 outputs stereo 16-bit IQ at 192 kHz over USB audio.
static constexpr int IC7300_RATE   = 192000;
// Frames per callback block (~5 ms).  Must be power-of-2 for smooth I/O.
static constexpr int IC7300_FRAMES = 1024;

// Static conversion buffer — avoids per-callback heap allocation.
// The callback runs on a single dedicated PortAudio thread, so no locking needed.
static std::complex<float> ic7300ConvBuf [IC7300_FRAMES];

//----------------------------------------------------------------------
// PortAudio callback — called from the PA thread, not the Qt main thread.
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

	// Convert interleaved int16 stereo → complex<float>  (L=I, R=Q)
	for (unsigned long i = 0; i < framesPerBuffer; i++)
	   ic7300ConvBuf [i] = std::complex<float> (src [2 * i]     / 32768.0f,
	                                             src [2 * i + 1] / 32768.0f);

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
	                                   vfoFrequency (14070000) {
	(void)mr;
	_I_Buffer  = b;
	mySettings = s;
	outputRate = IC7300_RATE;

	setupUi (&myFrame);
	myFrame. show ();

	PaError err = Pa_Initialize ();
	if (err != paNoError) {
	   fprintf (stderr, "IC-7300: Pa_Initialize failed: %s\n",
	            Pa_GetErrorText (err));
	   throw 42;
	}

	populateDevices ();

	// Restore the previously selected device name
	QString saved = s -> value ("ic7300/device", ""). toString ();
	int idx = deviceCombo -> findText (saved);
	if (idx >= 0)
	   deviceCombo -> setCurrentIndex (idx);

	connect (deviceCombo, SIGNAL (activated (int)),
	         this, SLOT (deviceSelected (int)));

	paDeviceIndex = deviceCombo -> currentData (). toInt ();
	if (paDeviceIndex < 0) {
	   statusLabel -> setText ("No stereo input device found");
	   fprintf (stderr, "IC-7300: no suitable PortAudio input device\n");
	   Pa_Terminate ();
	   throw 43;
	}

	if (!openDevice (paDeviceIndex)) {
	   Pa_Terminate ();
	   throw 44;
	}
}

//----------------------------------------------------------------------
// Populate combo box with all PortAudio devices that have ≥2 input channels
//----------------------------------------------------------------------
void	ic7300Handler::populateDevices () {
	int n = Pa_GetDeviceCount ();
	deviceCombo -> clear ();
	for (int i = 0; i < n; i++) {
	   const PaDeviceInfo *info = Pa_GetDeviceInfo (i);
	   if (info && info -> maxInputChannels >= 2)
	      deviceCombo -> addItem (
	            QString::fromLocal8Bit (info -> name), i);
	}
}

//----------------------------------------------------------------------
// Open the PortAudio stream on the given device index
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

	PaStreamParameters params;
	params. device                    = idx;
	params. channelCount              = 2;
	params. sampleFormat              = paInt16;
	params. suggestedLatency          = info -> defaultLowInputLatency;
	params. hostApiSpecificStreamInfo = nullptr;

	PaError err = Pa_OpenStream (&paStream,
	                              &params,
	                              nullptr,		// no output
	                              IC7300_RATE,
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

	statusLabel -> setText (
	      QString ("%1  |  192000 Hz  |  16-bit IQ")
	      .arg (QString::fromLocal8Bit (info -> name)));
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
	statusLabel -> setText (statusLabel -> text () + "  [running]");
	return true;
}

void	ic7300Handler::stopReader () {
	if (paStream && Pa_IsStreamActive (paStream) == 1)
	   Pa_StopStream (paStream);
}

void	ic7300Handler::setVFOFrequency (int32_t f) {
	vfoFrequency = f;
	// Optional: add CI-V serial command here to tune the radio
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
// Slot — user picked a different device from the combo box
//----------------------------------------------------------------------
void	ic7300Handler::deviceSelected (int comboIndex) {
int	idx = deviceCombo -> itemData (comboIndex). toInt ();
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
