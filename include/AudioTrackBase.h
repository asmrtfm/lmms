/*
 * AudioTrackBase.h - base class for tracks that produce audio output
 *
 * Copyright (c) 2024 LMMS Developers
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef LMMS_AUDIO_TRACK_BASE_H
#define LMMS_AUDIO_TRACK_BASE_H

#include "AudioPort.h"
#include "Track.h"


namespace lmms
{


//! Base class for tracks that produce audio (InstrumentTrack, SampleTrack).
//! Provides shared volume, panning, mixer channel routing, and AudioPort.
class LMMS_EXPORT AudioTrackBase : public Track
{
	Q_OBJECT
public:
	AudioTrackBase(Track::Type type, TrackContainer* tc, const QString& audioPortName);

	FloatModel* volumeModel() { return &m_volumeModel; }
	FloatModel* panningModel() { return &m_panningModel; }
	IntModel* mixerChannelModel() { return &m_mixerChannelModel; }
	AudioPort* audioPort() { return &m_audioPort; }

protected:
	//! Save volume, panning, and mixer channel settings to XML
	void saveAudioSettings(QDomDocument& doc, QDomElement& elem);

	//! Load volume, panning, and mixer channel settings from XML
	void loadAudioSettings(const QDomElement& elem);

	FloatModel m_volumeModel;
	FloatModel m_panningModel;
	IntModel m_mixerChannelModel;
	AudioPort m_audioPort;

protected slots:
	void updateMixerChannel();
};


} // namespace lmms

#endif // LMMS_AUDIO_TRACK_BASE_H
