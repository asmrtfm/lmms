/*
 * TrackBundle.cpp - export/import of tracks with mixer channels and automation
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

#include "TrackBundle.h"

#include <QDomDocument>

#include "AudioTrackBase.h"
#include "AutomationClip.h"
#include "DataFile.h"
#include "Engine.h"
#include "Mixer.h"
#include "Track.h"
#include "TrackContainer.h"


namespace lmms
{


bool TrackBundle::exportTrack(Track* track, const QString& filePath)
{
	return exportGroup(QList<Track*>{track}, filePath);
}


bool TrackBundle::exportGroup(const QList<Track*>& tracks, const QString& filePath)
{
	if (tracks.isEmpty()) { return false; }

	DataFile dataFile(DataFile::Type::TrackBundle);
	QDomDocument& doc = dataFile;
	QDomElement& content = dataFile.content();

	// Collect all mixer channel indices used by the tracks
	QSet<int> mixerChannelIndices;

	for (auto* track : tracks)
	{
		// Save the track using existing Track::saveState
		track->saveState(doc, content);

		// Collect mixer channel index if this is an audio track
		auto* audioTrack = dynamic_cast<AudioTrackBase*>(track);
		if (audioTrack)
		{
			int mixChIdx = audioTrack->mixerChannelModel()->value();
			if (mixChIdx > 0) // 0 = Master, always exists
			{
				mixerChannelIndices.insert(mixChIdx);
			}
		}
	}

	// Save mixer channel definitions for all used channels
	for (int idx : mixerChannelIndices)
	{
		QDomElement mixerDef = saveMixerChannelDef(doc, idx);
		content.appendChild(mixerDef);
	}

	// Save automation clips that target models in these tracks
	QList<AutomationClip*> allAutomation;
	for (auto* track : tracks)
	{
		auto clips = findAutomationForTrack(track);
		for (auto* clip : clips)
		{
			if (!allAutomation.contains(clip))
			{
				allAutomation.append(clip);
			}
		}
	}

	if (!allAutomation.isEmpty())
	{
		QDomElement autoElem = doc.createElement("bundled_automation");
		content.appendChild(autoElem);
		for (auto* ac : allAutomation)
		{
			ac->saveState(doc, autoElem);
		}
	}

	// Write file with bundled resources (samples, etc.)
	return dataFile.writeFile(filePath, true);
}


QDomElement TrackBundle::saveMixerChannelDef(QDomDocument& doc, int idx)
{
	Mixer* mixer = Engine::mixer();
	MixerChannel* ch = mixer->mixerChannel(idx);

	QDomElement mixerDef = doc.createElement("bundled_mixer_channel");

	// Store name as the primary identifier (NOT the index)
	mixerDef.setAttribute("name", ch->m_name);
	mixerDef.setAttribute("original_index", idx);

	// Volume, mute, solo
	ch->m_volumeModel.saveSettings(doc, mixerDef, "volume");
	ch->m_muteModel.saveSettings(doc, mixerDef, "muted");
	ch->m_soloModel.saveSettings(doc, mixerDef, "soloed");

	// Color
	if (const auto& color = ch->color())
	{
		mixerDef.setAttribute("color", color->name());
	}

	// Effect chain
	ch->m_fxChain.saveState(doc, mixerDef);

	// Sends — store by name, not index
	for (const auto& send : ch->m_sends)
	{
		QDomElement sendElem = doc.createElement("send");
		MixerChannel* receiver = send->receiver();
		sendElem.setAttribute("channel_name", receiver->m_name);
		sendElem.setAttribute("original_channel_index", receiver->m_channelIndex);
		send->amount()->saveSettings(doc, sendElem, "amount");
		mixerDef.appendChild(sendElem);
	}

	return mixerDef;
}


QList<AutomationClip*> TrackBundle::findAutomationForTrack(Track* track)
{
	QList<AutomationClip*> result;

	// Use Qt's findChildren to discover all AutomatableModel instances
	// in the track's QObject tree
	auto models = track->findChildren<AutomatableModel*>();

	for (auto* model : models)
	{
		auto clips = AutomationClip::clipsForModel(model);
		for (auto* clip : clips)
		{
			if (!result.contains(clip))
			{
				result.append(clip);
			}
		}
	}

	return result;
}


int TrackBundle::resolveOrCreateMixerChannel(const QDomElement& mixerDef)
{
	QString channelName = mixerDef.attribute("name");
	Mixer* mixer = Engine::mixer();

	// Try to find existing channel by name
	for (int i = 0; i < mixer->numChannels(); i++)
	{
		if (mixer->mixerChannel(i)->m_name == channelName)
		{
			return i;
		}
	}

	// Not found — create a new channel
	int newIdx = mixer->createChannel();
	MixerChannel* ch = mixer->mixerChannel(newIdx);

	// Apply settings from the bundle
	ch->m_name = channelName;
	ch->m_volumeModel.loadSettings(mixerDef, "volume");
	ch->m_muteModel.loadSettings(mixerDef, "muted");
	ch->m_soloModel.loadSettings(mixerDef, "soloed");

	if (mixerDef.hasAttribute("color"))
	{
		ch->setColor(QColor{mixerDef.attribute("color")});
	}

	// Restore effect chain
	QDomElement fxChainElem = mixerDef.firstChildElement(ch->m_fxChain.nodeName());
	if (!fxChainElem.isNull())
	{
		ch->m_fxChain.restoreState(fxChainElem);
	}

	// Restore sends — resolve targets by name
	QDomNodeList sends = mixerDef.elementsByTagName("send");
	for (int i = 0; i < sends.count(); i++)
	{
		QDomElement sendElem = sends.at(i).toElement();
		QString targetName = sendElem.attribute("channel_name");

		// Find target channel by name (default to Master = 0)
		int targetIdx = 0;
		for (int j = 0; j < mixer->numChannels(); j++)
		{
			if (mixer->mixerChannel(j)->m_name == targetName)
			{
				targetIdx = j;
				break;
			}
		}

		MixerRoute* route = mixer->createChannelSend(newIdx, targetIdx);
		if (route)
		{
			route->amount()->loadSettings(sendElem, "amount");
		}
	}

	return newIdx;
}


void TrackBundle::patchMixerChannelIndex(QDomElement& trackElem, int newIdx)
{
	// The track XML structure is:
	//   <track type="0" name="...">
	//     <instrumenttrack>
	//       <mixch value="3"/>
	//       ...
	//     </instrumenttrack>
	//     ...
	//   </track>
	//
	// We need to find the instrumenttrack or sampletrack child element
	// and replace the mixch value.

	QDomNodeList children = trackElem.childNodes();
	for (int i = 0; i < children.count(); i++)
	{
		QDomElement child = children.at(i).toElement();
		if (child.isNull()) { continue; }

		QString tag = child.tagName();
		if (tag == "instrumenttrack" || tag == "sampletrack")
		{
			// IntModel saves as <mixch value="N"/>, find and update it
			QDomNodeList mixchNodes = child.elementsByTagName("mixch");
			for (int j = 0; j < mixchNodes.count(); j++)
			{
				mixchNodes.at(j).toElement().setAttribute("value", newIdx);
			}
			// Also check direct attribute (older format)
			if (child.hasAttribute("mixch"))
			{
				child.setAttribute("mixch", newIdx);
			}
		}
	}
}


QList<Track*> TrackBundle::importBundle(const QString& filePath, TrackContainer* tc)
{
	DataFile dataFile(filePath);
	QDomElement content = dataFile.content();
	QList<Track*> result;

	// Build a map from original mixer channel index to new index
	QMap<int, int> mixerChannelMap;

	// First, handle mixer channel creation (must exist before track loads)
	QDomNodeList mixerDefs = content.elementsByTagName("bundled_mixer_channel");
	for (int i = 0; i < mixerDefs.count(); i++)
	{
		QDomElement mixerDef = mixerDefs.at(i).toElement();
		int originalIdx = mixerDef.attribute("original_index").toInt();
		int newIdx = resolveOrCreateMixerChannel(mixerDef);
		mixerChannelMap[originalIdx] = newIdx;
	}

	// Load tracks
	QDomNodeList trackNodes = content.elementsByTagName("track");
	for (int i = 0; i < trackNodes.count(); i++)
	{
		QDomElement trackElem = trackNodes.at(i).toElement();

		// Patch the mixer channel index if the channel was remapped
		auto type = static_cast<Track::Type>(trackElem.attribute("type").toInt());
		if (type == Track::Type::Instrument || type == Track::Type::Sample)
		{
			// Find what mixer channel this track was using
			// and map it to the new index
			for (auto it = mixerChannelMap.constBegin(); it != mixerChannelMap.constEnd(); ++it)
			{
				// Check if this track uses this mixer channel
				// by scanning its sub-element for a matching mixch value
				QDomElement subElem = trackElem.firstChildElement(
					type == Track::Type::Instrument ? "instrumenttrack" : "sampletrack");
				if (!subElem.isNull())
				{
					QDomElement mixchElem = subElem.firstChildElement("mixch");
					if (!mixchElem.isNull())
					{
						int currentMixCh = mixchElem.attribute("value").toInt();
						if (mixerChannelMap.contains(currentMixCh))
						{
							patchMixerChannelIndex(trackElem, mixerChannelMap[currentMixCh]);
						}
						break;
					}
				}
			}
		}

		// Use existing Track::create which handles type dispatch and restoreState
		Track* t = Track::create(trackElem, tc);
		if (t)
		{
			result.append(t);
		}
	}

	// Resolve automation IDs (existing mechanism)
	AutomationClip::resolveAllIDs();

	return result;
}


} // namespace lmms
