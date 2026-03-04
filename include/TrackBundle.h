/*
 * TrackBundle.h - export/import of tracks with mixer channels and automation
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

#ifndef LMMS_TRACK_BUNDLE_H
#define LMMS_TRACK_BUNDLE_H

#include <QDomDocument>
#include <QList>
#include <QString>

#include "lmms_export.h"

namespace lmms
{

class AutomationClip;
class Track;
class TrackContainer;

class LMMS_EXPORT TrackBundle
{
public:
	//! Export a single track to a .lmms-track bundle file
	static bool exportTrack(Track* track, const QString& filePath);

	//! Export multiple tracks as a group to a .lmms-track bundle file
	static bool exportGroup(const QList<Track*>& tracks, const QString& filePath);

	//! Import a bundle into the given TrackContainer, returns created tracks
	static QList<Track*> importBundle(const QString& filePath, TrackContainer* tc);

private:
	//! Save the mixer channel definition for the given channel index
	static QDomElement saveMixerChannelDef(QDomDocument& doc, int mixerChannelIndex);

	//! Find all automation clips that target models owned by the given track
	static QList<AutomationClip*> findAutomationForTrack(Track* track);

	//! Resolve a mixer channel by name on import, creating if needed
	static int resolveOrCreateMixerChannel(const QDomElement& mixerDef);

	//! Patch the mixer channel index in a track's XML element before loading
	static void patchMixerChannelIndex(QDomElement& trackElem, int newIdx);
};

} // namespace lmms

#endif // LMMS_TRACK_BUNDLE_H
