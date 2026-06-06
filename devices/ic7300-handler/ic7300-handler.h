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
 *    IC-7300 USB Audio IQ handler.
 *    The IC-7300 exposes its IQ stream as a standard USB audio device:
 *      stereo 16-bit PCM, Left = I, Right = Q, up to 192000 Hz.
 *    No special driver is required — PortAudio enumerates it as a normal
 *    input device (typically labelled "USB Audio CODEC").
 */

#ifndef __IC7300_HANDLER_H
#define	__IC7300_HANDLER_H

#include	<QFrame>
#include	<QString>
#include	<portaudio.h>
#include	"device-handler.h"
#include	"ringbuffer.h"
#include	"ui_ic7300-widget.h"

class	QSettings;
class	RadioInterface;

class	ic7300Handler : public deviceHandler, public Ui_ic7300Widget {
Q_OBJECT
public:
			ic7300Handler	(RadioInterface *,
		                         RingBuffer<std::complex<float>> *,
		                         QSettings *);
			~ic7300Handler	();

	void		setVFOFrequency	(int32_t)	override;
	int32_t		getVFOFrequency	()		override;
	bool		restartReader	()		override;
	void		stopReader	()		override;
	int16_t		bitDepth	()		override;

	// Public for access from the static PortAudio callback
	RingBuffer<std::complex<float>>	*_I_Buffer;
	int32_t				outputRate;
	void		newdataAvailable (int);

private:
	QFrame		myFrame;
	QSettings      *mySettings;
	PaStream       *paStream;
	int		paDeviceIndex;
	int32_t		vfoFrequency;

	static int	paCallback	(const void *,
		                         void *,
		                         unsigned long,
		                         const PaStreamCallbackTimeInfo *,
		                         PaStreamCallbackFlags,
		                         void *);
	void		populateDevices	();
	bool		openDevice	(int deviceIndex);

private slots:
	void		deviceSelected	(int comboIndex);
};

#endif
