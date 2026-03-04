/*
 * AudioTrackBase.cpp - base class for tracks that produce audio output
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

#include "AudioTrackBase.h"

#include "Engine.h"
#include "Mixer.h"
#include "panning_constants.h"
#include "volume.h"


namespace lmms
{


AudioTrackBase::AudioTrackBase(Track::Type type, TrackContainer* tc,
                               const QString& audioPortName)
	: Track(type, tc)
	, m_volumeModel(DefaultVolume, MinVolume, MaxVolume, 0.1f, this, tr("Volume"))
	, m_panningModel(DefaultPanning, PanningLeft, PanningRight, 0.1f, this, tr("Panning"))
	, m_mixerChannelModel(0, 0, 0, this, tr("Mixer channel"))
	, m_audioPort(audioPortName, true, &m_volumeModel, &m_panningModel, &m_mutedModel)
{
	m_panningModel.setCenterValue(DefaultPanning);
	m_mixerChannelModel.setRange(0, Engine::mixer()->numChannels() - 1, 1);

	connect(&m_mixerChannelModel, &IntModel::dataChanged,
	        this, &AudioTrackBase::updateMixerChannel);
}


void AudioTrackBase::saveAudioSettings(QDomDocument& doc, QDomElement& elem)
{
	m_volumeModel.saveSettings(doc, elem, "vol");
	m_panningModel.saveSettings(doc, elem, "pan");
	m_mixerChannelModel.saveSettings(doc, elem, "mixch");
}


void AudioTrackBase::loadAudioSettings(const QDomElement& elem)
{
	m_volumeModel.loadSettings(elem, "vol");
	m_panningModel.loadSettings(elem, "pan");
	m_mixerChannelModel.setRange(0, Engine::mixer()->numChannels() - 1);
	m_mixerChannelModel.loadSettings(elem, "mixch");
}


void AudioTrackBase::updateMixerChannel()
{
	m_audioPort.setNextMixerChannel(m_mixerChannelModel.value());
}


} // namespace lmms
