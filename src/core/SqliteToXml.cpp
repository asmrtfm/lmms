/*
 * SqliteToXml.cpp - converts .lmms-db (SQLite) files to XML for the DataFile pipeline
 *
 * Copyright (c) 2026
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

#include "SqliteToXml.h"

#include <sqlite3.h>

#include <QDomDocument>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTextStream>
#include <QVariant>

#include <cstdio>

namespace lmms
{

namespace
{

constexpr int DEFAULT_PATTERN_LENGTH = 192;

// Helper: log to stderr
void logMsg(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[SqliteToXml] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

// RAII wrapper for sqlite3_stmt
class SqliteStmt
{
public:
	SqliteStmt(sqlite3* db, const char* sql)
		: m_stmt(nullptr)
	{
		int rc = sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr);
		if (rc != SQLITE_OK)
		{
			logMsg("SQL prepare error: %s — %s", sql, sqlite3_errmsg(db));
			m_stmt = nullptr;
		}
	}

	~SqliteStmt()
	{
		if (m_stmt) { sqlite3_finalize(m_stmt); }
	}

	bool valid() const { return m_stmt != nullptr; }
	sqlite3_stmt* get() const { return m_stmt; }

	bool step()
	{
		return m_stmt && sqlite3_step(m_stmt) == SQLITE_ROW;
	}

	void reset()
	{
		if (m_stmt) { sqlite3_reset(m_stmt); }
	}

	void bindInt(int idx, int val)
	{
		if (m_stmt) { sqlite3_bind_int(m_stmt, idx, val); }
	}

	void bindText(int idx, const char* val)
	{
		if (m_stmt) { sqlite3_bind_text(m_stmt, idx, val, -1, SQLITE_TRANSIENT); }
	}

	int colInt(int idx) const { return sqlite3_column_int(m_stmt, idx); }
	double colDouble(int idx) const { return sqlite3_column_double(m_stmt, idx); }

	QString colText(int idx) const
	{
		const unsigned char* t = sqlite3_column_text(m_stmt, idx);
		return t ? QString::fromUtf8(reinterpret_cast<const char*>(t)) : QString();
	}

	bool colIsNull(int idx) const
	{
		return sqlite3_column_type(m_stmt, idx) == SQLITE_NULL;
	}

	// Disallow copy
	SqliteStmt(const SqliteStmt&) = delete;
	SqliteStmt& operator=(const SqliteStmt&) = delete;

private:
	sqlite3_stmt* m_stmt;
};

// Convert a JSON object (parsed from a text column) into XML child elements/attributes
// This is the C++ equivalent of dict_to_xml() in lmms_export.py
void jsonToXml(QDomDocument& doc, QDomElement& parent, const QJsonObject& obj)
{
	for (auto it = obj.begin(); it != obj.end(); ++it)
	{
		const QString& key = it.key();
		const QJsonValue& val = it.value();

		if (val.isObject())
		{
			QDomElement child = doc.createElement(key);
			QJsonObject childObj = val.toObject();
			jsonToXml(doc, child, childObj);
			parent.appendChild(child);
		}
		else if (val.isArray())
		{
			QJsonArray arr = val.toArray();
			for (const auto& item : arr)
			{
				QDomElement child = doc.createElement(key);
				if (item.isObject())
				{
					QJsonObject childObj = item.toObject();
					jsonToXml(doc, child, childObj);
				}
				else
				{
					child.appendChild(doc.createTextNode(
						item.isDouble()
							? QString::number(item.toDouble(), 'g', 15)
							: item.toString()));
				}
				parent.appendChild(child);
			}
		}
		else if (val.isDouble())
		{
			// Check if it's actually an integer value
			double d = val.toDouble();
			if (d == static_cast<double>(static_cast<long long>(d)) && d >= -1e15 && d <= 1e15)
			{
				parent.setAttribute(key, QString::number(static_cast<long long>(d)));
			}
			else
			{
				parent.setAttribute(key, QString::number(d, 'g', 15));
			}
		}
		else
		{
			parent.setAttribute(key, val.toString());
		}
	}
}

// Parse a JSON text column and apply it as XML children/attributes on parent
void applyJsonColumn(QDomDocument& doc, QDomElement& parent, const QString& jsonText)
{
	if (jsonText.isEmpty() || jsonText == "{}")
	{
		return;
	}

	QJsonParseError err;
	QJsonDocument jdoc = QJsonDocument::fromJson(jsonText.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError)
	{
		logMsg("JSON parse error: %s", err.errorString().toUtf8().constData());
		return;
	}

	if (jdoc.isObject())
	{
		QJsonObject obj = jdoc.object();
		jsonToXml(doc, parent, obj);
	}
}

// Apply extra_json: simple values as attributes, _children as child elements, other dicts as child elements
// This mirrors apply_extra_json() from lmms_export.py
void applyExtraJson(QDomDocument& doc, QDomElement& elem, const QString& jsonText)
{
	if (jsonText.isEmpty() || jsonText == "{}")
	{
		return;
	}

	QJsonParseError err;
	QJsonDocument jdoc = QJsonDocument::fromJson(jsonText.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError)
	{
		logMsg("extra_json parse error: %s", err.errorString().toUtf8().constData());
		return;
	}
	if (!jdoc.isObject()) { return; }

	QJsonObject obj = jdoc.object();

	// Extract _children before applying the rest
	QJsonObject children;
	bool hasChildren = false;
	if (obj.contains("_children"))
	{
		QJsonValue childrenVal = obj.take("_children");
		if (childrenVal.isObject())
		{
			children = childrenVal.toObject();
			hasChildren = true;
		}
	}

	// Apply remaining keys (attrs and dict/array child elements)
	jsonToXml(doc, elem, obj);

	// Apply _children as child elements
	if (hasChildren)
	{
		jsonToXml(doc, elem, children);
	}
}

// Apply extra_json but only simple (non-dict, non-array) values as attributes
// Used for track-level extras where we only want attributes like mutedBeforeSolo
void applyExtraJsonAttrsOnly(QDomElement& elem, const QString& jsonText)
{
	if (jsonText.isEmpty() || jsonText == "{}")
	{
		return;
	}

	QJsonParseError err;
	QJsonDocument jdoc = QJsonDocument::fromJson(jsonText.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError)
	{
		logMsg("extra_json parse error: %s", err.errorString().toUtf8().constData());
		return;
	}
	if (!jdoc.isObject()) { return; }

	QJsonObject obj = jdoc.object();
	for (auto it = obj.begin(); it != obj.end(); ++it)
	{
		const QJsonValue& val = it.value();
		if (val.isObject() || val.isArray()) { continue; }
		if (val.isDouble())
		{
			double d = val.toDouble();
			if (d == static_cast<double>(static_cast<long long>(d)) && d >= -1e15 && d <= 1e15)
			{
				elem.setAttribute(it.key(), QString::number(static_cast<long long>(d)));
			}
			else
			{
				elem.setAttribute(it.key(), QString::number(d, 'g', 15));
			}
		}
		else
		{
			elem.setAttribute(it.key(), val.toString());
		}
	}
}

// Export effects chain for a given owner
void exportEffects(sqlite3* db, QDomDocument& doc, QDomElement& parent,
	const char* ownerType, int ownerId)
{
	SqliteStmt stmt(db,
		"SELECT * FROM effect WHERE owner_type = ? AND owner_id = ? ORDER BY sort_order");
	if (!stmt.valid()) { return; }

	stmt.bindText(1, ownerType);
	stmt.bindInt(2, ownerId);

	QDomElement fxchain = doc.createElement("fxchain");
	int count = 0;

	while (stmt.step())
	{
		QDomElement fxElem = doc.createElement("effect");
		fxElem.setAttribute("name", stmt.colText(3));  // plugin_name
		fxElem.setAttribute("on", stmt.colInt(5));      // enabled
		fxElem.setAttribute("wet", QString::number(stmt.colDouble(6), 'g', 15)); // wet
		fxElem.setAttribute("gate", QString::number(stmt.colDouble(7), 'g', 15)); // gate

		// Plugin params from JSON — restore extra attrs, rename _key back to key
		QString paramsJson = stmt.colText(8); // params_json
		if (!paramsJson.isEmpty() && paramsJson != "{}")
		{
			QJsonParseError parseErr;
			QJsonDocument jdoc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseErr);
			if (parseErr.error == QJsonParseError::NoError && jdoc.isObject())
			{
				QJsonObject obj = jdoc.object();
				// Restore extra attributes (autoquit, etc.) directly on <effect>
				if (obj.contains("_extra_attrs"))
				{
					QJsonObject extras = obj.take("_extra_attrs").toObject();
					for (auto it = extras.begin(); it != extras.end(); ++it)
					{
						fxElem.setAttribute(it.key(), it.value().toString());
					}
				}
				if (obj.contains("_key"))
				{
					obj.insert("key", obj.take("_key"));
				}
				jsonToXml(doc, fxElem, obj);
			}
		}

		fxchain.appendChild(fxElem);
		count++;
	}

	fxchain.setAttribute("numofeffects", count);
	fxchain.setAttribute("enabled", count > 0 ? 1 : 0);
	parent.appendChild(fxchain);
}

// Export project metadata into <head>
void exportProjectMetadata(sqlite3* db, QDomDocument& doc, QDomElement& head)
{
	SqliteStmt stmt(db, "SELECT * FROM project WHERE id = 1");
	if (!stmt.valid() || !stmt.step())
	{
		logMsg("WARNING: No project metadata found");
		return;
	}

	// Column indices: 0=id, 1=name, 2=bpm, 3=timesig_num, 4=timesig_denom,
	//                 5=master_volume, 6=master_pitch, 7=created_at, 8=modified_at, 9=extra_json
	head.setAttribute("bpm", QString::number(stmt.colDouble(2), 'g', 15));
	head.setAttribute("timesig_numerator", stmt.colInt(3));
	head.setAttribute("timesig_denominator", stmt.colInt(4));
	head.setAttribute("mastervol", QString::number(stmt.colDouble(5), 'g', 15));
	head.setAttribute("masterpitch", QString::number(stmt.colDouble(6), 'g', 15));

	// Restore extra head attributes from extra_json (column 9)
	applyExtraJson(doc, head, stmt.colText(9));

	logMsg("Project: bpm=%g, time_sig=%d/%d",
		stmt.colDouble(2), stmt.colInt(3), stmt.colInt(4));
}

// Export mixer channels and routing
void exportMixer(sqlite3* db, QDomDocument& doc, QDomElement& songElem)
{
	SqliteStmt chStmt(db, "SELECT * FROM mixer_channel ORDER BY sort_order");
	if (!chStmt.valid()) { return; }

	QDomElement mixerElem = doc.createElement("mixer");
	int channelCount = 0;

	while (chStmt.step())
	{
		int chId = chStmt.colInt(0);
		QDomElement chElem = doc.createElement("mixerchannel");
		chElem.setAttribute("num", chId);
		chElem.setAttribute("name", chStmt.colText(1));
		chElem.setAttribute("volume", QString::number(chStmt.colDouble(2), 'g', 15));
		chElem.setAttribute("muted", chStmt.colInt(3));
		chElem.setAttribute("soloed", chStmt.colInt(4));
		if (!chStmt.colIsNull(5))
		{
			chElem.setAttribute("color", chStmt.colText(5));
		}

		// Restore extra mixer channel attributes and children (column 7)
		applyExtraJson(doc, chElem, chStmt.colText(7));

		// Effects chain
		exportEffects(db, doc, chElem, "mixer_channel", chId);

		// Sends (routing)
		SqliteStmt routeStmt(db,
			"SELECT * FROM mixer_route WHERE from_channel_id = ?");
		if (routeStmt.valid())
		{
			routeStmt.bindInt(1, chId);
			while (routeStmt.step())
			{
				QDomElement sendElem = doc.createElement("send");
				sendElem.setAttribute("channel", routeStmt.colInt(2)); // to_channel_id
				sendElem.setAttribute("amount", QString::number(routeStmt.colDouble(3), 'g', 15));
				chElem.appendChild(sendElem);
			}
		}

		mixerElem.appendChild(chElem);
		channelCount++;
	}

	if (channelCount > 0)
	{
		songElem.appendChild(mixerElem);
		logMsg("Mixer: %d channels", channelCount);
	}
}

// Build a complete <track type="0"> element for an instrument track
QDomElement buildInstrumentTrackElement(sqlite3* db, QDomDocument& doc,
	QDomElement& parent, SqliteStmt& itStmt)
{
	// Column indices for instrument_track:
	// 0=id, 1=name, 2=volume, 3=panning, 4=pitch, 5=pitch_range,
	// 6=mixer_channel_id, 7=base_note, 8=use_master_pitch,
	// 9=muted, 10=solo, 11=color, 12=sort_order,
	// 13=instrument_plugin, 14=instrument_params_json,
	// 15=sound_shaping_json, 16=arpeggio_json, 17=chord_creator_json,
	// 18=midi_port_json, 19=microtuner_json,
	// 20=track_extra_json, 21=instrumenttrack_extra_json

	int itId = itStmt.colInt(0);

	QDomElement trackElem = doc.createElement("track");
	trackElem.setAttribute("type", 0);
	trackElem.setAttribute("name", itStmt.colText(1));
	trackElem.setAttribute("muted", itStmt.colInt(9));
	trackElem.setAttribute("solo", itStmt.colInt(10));
	if (!itStmt.colIsNull(11))
	{
		trackElem.setAttribute("color", itStmt.colText(11));
	}

	// Restore extra <track> attributes (mutedBeforeSolo, etc.) from track_extra_json (column 20)
	applyExtraJsonAttrsOnly(trackElem, itStmt.colText(20));

	// <instrumenttrack> settings element
	QDomElement itElem = doc.createElement("instrumenttrack");
	itElem.setAttribute("vol", QString::number(itStmt.colDouble(2), 'g', 15));
	itElem.setAttribute("pan", QString::number(itStmt.colDouble(3), 'g', 15));
	itElem.setAttribute("pitch", QString::number(itStmt.colDouble(4), 'g', 15));
	itElem.setAttribute("pitchrange", itStmt.colInt(5));
	if (!itStmt.colIsNull(6))
	{
		itElem.setAttribute("mixch", itStmt.colInt(6));
	}
	itElem.setAttribute("basenote", itStmt.colInt(7));
	itElem.setAttribute("usemasterpitch", itStmt.colInt(8));

	// Parse instrumenttrack_extra_json (column 21) — we'll apply attrs now and children later
	QString itExtrasJson = itStmt.colText(21);
	QJsonObject itExtrasChildren;
	bool hasItExtrasChildren = false;
	if (!itExtrasJson.isEmpty() && itExtrasJson != "{}")
	{
		QJsonParseError parseErr;
		QJsonDocument jdoc = QJsonDocument::fromJson(itExtrasJson.toUtf8(), &parseErr);
		if (parseErr.error == QJsonParseError::NoError && jdoc.isObject())
		{
			QJsonObject obj = jdoc.object();
			// Extract _children before applying
			if (obj.contains("_children"))
			{
				QJsonValue childrenVal = obj.take("_children");
				if (childrenVal.isObject())
				{
					itExtrasChildren = childrenVal.toObject();
					hasItExtrasChildren = true;
				}
			}
			// Apply non-dict, non-array values as attributes (enablecc, firstkey, lastkey, etc.)
			for (auto it = obj.begin(); it != obj.end(); ++it)
			{
				const QJsonValue& val = it.value();
				if (val.isObject() || val.isArray()) { continue; }
				if (val.isDouble())
				{
					double d = val.toDouble();
					if (d == static_cast<double>(static_cast<long long>(d)) && d >= -1e15 && d <= 1e15)
					{
						itElem.setAttribute(it.key(), QString::number(static_cast<long long>(d)));
					}
					else
					{
						itElem.setAttribute(it.key(), QString::number(d, 'g', 15));
					}
				}
				else
				{
					itElem.setAttribute(it.key(), val.toString());
				}
			}
		}
	}

	// Instrument plugin — params must be a child element named after the plugin
	// e.g. <instrument name="tripleoscillator"><tripleoscillator vol0="100" .../></instrument>
	QString pluginName = itStmt.colText(13);
	QDomElement instrumentElem = doc.createElement("instrument");
	instrumentElem.setAttribute("name", pluginName);
	QString pluginParamsJson = itStmt.colText(14);
	if (!pluginParamsJson.isEmpty() && pluginParamsJson != "{}")
	{
		QJsonParseError parseErr;
		QJsonDocument jdoc = QJsonDocument::fromJson(pluginParamsJson.toUtf8(), &parseErr);
		if (parseErr.error == QJsonParseError::NoError && jdoc.isObject())
		{
			QJsonObject obj = jdoc.object();
			// Extract _key and emit as <key> sibling of plugin element
			QJsonValue keyVal;
			if (obj.contains("_key"))
			{
				keyVal = obj.take("_key");
			}
			QDomElement pluginChild = doc.createElement(pluginName);
			jsonToXml(doc, pluginChild, obj);
			instrumentElem.appendChild(pluginChild);
			// Emit <key> element if present
			if (keyVal.isObject())
			{
				QDomElement keyElem = doc.createElement("key");
				QJsonObject keyObj = keyVal.toObject();
				jsonToXml(doc, keyElem, keyObj);
				instrumentElem.appendChild(keyElem);
			}
		}
	}
	itElem.appendChild(instrumentElem);

	// Sound shaping (eldata)
	QString soundShapingJson = itStmt.colText(15);
	if (!soundShapingJson.isEmpty() && soundShapingJson != "{}")
	{
		QDomElement eldataElem = doc.createElement("eldata");
		applyJsonColumn(doc, eldataElem, soundShapingJson);
		itElem.appendChild(eldataElem);
	}

	// Chord creator
	QString chordJson = itStmt.colText(17);
	if (!chordJson.isEmpty() && chordJson != "{}")
	{
		QDomElement chordElem = doc.createElement("chordcreator");
		applyJsonColumn(doc, chordElem, chordJson);
		itElem.appendChild(chordElem);
	}

	// Arpeggiator
	QString arpJson = itStmt.colText(16);
	if (!arpJson.isEmpty() && arpJson != "{}")
	{
		QDomElement arpElem = doc.createElement("arpeggiator");
		applyJsonColumn(doc, arpElem, arpJson);
		itElem.appendChild(arpElem);
	}

	// MIDI port
	QString midiJson = itStmt.colText(18);
	if (!midiJson.isEmpty() && midiJson != "{}")
	{
		QDomElement midiElem = doc.createElement("midiport");
		applyJsonColumn(doc, midiElem, midiJson);
		itElem.appendChild(midiElem);
	}

	// Microtuner
	QString microtunerJson = itStmt.colText(19);
	if (!microtunerJson.isEmpty() && microtunerJson != "{}")
	{
		QDomElement microtunerElem = doc.createElement("microtuner");
		applyJsonColumn(doc, microtunerElem, microtunerJson);
		itElem.appendChild(microtunerElem);
	}

	// Apply instrumenttrack extra _children (midicontrollers, etc.) after all known children
	if (hasItExtrasChildren)
	{
		jsonToXml(doc, itElem, itExtrasChildren);
	}

	// Effects
	exportEffects(db, doc, itElem, "instrument_track", itId);

	trackElem.appendChild(itElem);
	parent.appendChild(trackElem);
	return trackElem;
}

// Export patternstore with all instrument tracks and their midiclips
void exportPatternstore(sqlite3* db, QDomDocument& doc, QDomElement& patternstoreElem,
	const QMap<int, int>& patternIdToIndex)
{
	SqliteStmt itStmt(db, "SELECT * FROM instrument_track ORDER BY sort_order");
	if (!itStmt.valid()) { return; }

	int trackCount = 0;
	while (itStmt.step())
	{
		int itId = itStmt.colInt(0);
		QDomElement trackElem = buildInstrumentTrackElement(db, doc, patternstoreElem, itStmt);

		// Add midiclips for this instrument track
		SqliteStmt mcStmt(db,
			"SELECT mc.*, p.sort_order as pattern_sort_order "
			"FROM midi_clip mc "
			"JOIN pattern p ON mc.pattern_id = p.id "
			"WHERE mc.instrument_track_id = ? "
			"ORDER BY p.sort_order");
		if (!mcStmt.valid()) { continue; }
		mcStmt.bindInt(1, itId);

		while (mcStmt.step())
		{
			int patternId = mcStmt.colInt(2); // pattern_id
			int patternIdx = patternIdToIndex.value(patternId, 0);
			int pos = patternIdx * DEFAULT_PATTERN_LENGTH;

			QDomElement mcElem = doc.createElement("midiclip");
			mcElem.setAttribute("type", mcStmt.colInt(3));   // clip_type
			mcElem.setAttribute("name", mcStmt.colText(6));  // name
			mcElem.setAttribute("pos", pos);
			mcElem.setAttribute("muted", mcStmt.colInt(5));  // muted
			mcElem.setAttribute("steps", mcStmt.colInt(4));  // steps
			if (!mcStmt.colIsNull(7))
			{
				mcElem.setAttribute("color", mcStmt.colText(7));
			}

			// Restore extra midi clip attributes (column 8: extra_json)
			applyExtraJson(doc, mcElem, mcStmt.colText(8));

			// Notes
			SqliteStmt noteStmt(db,
				"SELECT * FROM note WHERE midi_clip_id = ? ORDER BY position");
			if (noteStmt.valid())
			{
				noteStmt.bindInt(1, mcStmt.colInt(0)); // mc.id
				while (noteStmt.step())
				{
					int noteId = noteStmt.colInt(0);
					QDomElement noteElem = doc.createElement("note");
					noteElem.setAttribute("pos", noteStmt.colInt(2));    // position
					noteElem.setAttribute("len", noteStmt.colInt(3));    // length
					noteElem.setAttribute("key", noteStmt.colInt(4));    // key
					noteElem.setAttribute("vol", noteStmt.colInt(5));    // volume
					noteElem.setAttribute("pan", noteStmt.colInt(6));    // panning
					noteElem.setAttribute("type", noteStmt.colInt(7));   // note_type

					// Restore extra note attributes (column 8: extra_json)
					applyExtraJson(doc, noteElem, noteStmt.colText(8));

					// Note detuning (rare)
					SqliteStmt dtStmt(db,
						"SELECT * FROM note_detuning WHERE note_id = ? ORDER BY position");
					if (dtStmt.valid())
					{
						dtStmt.bindInt(1, noteId);
						bool hasDetuning = false;
						QDomElement detuningElem;
						QDomElement autoElem;

						while (dtStmt.step())
						{
							if (!hasDetuning)
							{
								detuningElem = doc.createElement("detuning");
								autoElem = doc.createElement("automationpattern");
								hasDetuning = true;
							}

							QDomElement timeElem = doc.createElement("time");
							timeElem.setAttribute("pos", dtStmt.colInt(2));
							timeElem.setAttribute("value",
								QString::number(dtStmt.colDouble(3), 'g', 15));
							if (!dtStmt.colIsNull(4))
							{
								timeElem.setAttribute("outValue",
									QString::number(dtStmt.colDouble(4), 'g', 15));
							}
							timeElem.setAttribute("inTan",
								QString::number(dtStmt.colDouble(5), 'g', 15));
							timeElem.setAttribute("outTan",
								QString::number(dtStmt.colDouble(6), 'g', 15));
							autoElem.appendChild(timeElem);
						}

						if (hasDetuning)
						{
							detuningElem.appendChild(autoElem);
							noteElem.appendChild(detuningElem);
						}
					}

					mcElem.appendChild(noteElem);
				}
			}

			trackElem.appendChild(mcElem);
		}

		trackCount++;
	}

	logMsg("PatternStore: %d instrument tracks", trackCount);
}

// Export pattern tracks with their clips
void exportPatternTracks(sqlite3* db, QDomDocument& doc, QDomElement& songTc,
	QDomElement& patternstoreElem, const QMap<int, int>& patternIdToIndex)
{
	SqliteStmt ptStmt(db, "SELECT * FROM pattern_track ORDER BY sort_order");
	if (!ptStmt.valid()) { return; }

	bool firstPatternTrack = true;
	int trackCount = 0;

	while (ptStmt.step())
	{
		int ptId = ptStmt.colInt(0);

		QDomElement trackElem = doc.createElement("track");
		trackElem.setAttribute("type", 1);
		trackElem.setAttribute("name", ptStmt.colText(1));
		trackElem.setAttribute("muted", ptStmt.colInt(2));
		trackElem.setAttribute("solo", ptStmt.colInt(3));
		trackElem.setAttribute("mutedBeforeSolo", 0);
		if (!ptStmt.colIsNull(4))
		{
			trackElem.setAttribute("color", ptStmt.colText(4));
		}

		// Restore extra pattern track attributes (column 6: extra_json)
		applyExtraJson(doc, trackElem, ptStmt.colText(6));

		QDomElement ptSettings = doc.createElement("patterntrack");

		// First pattern track contains the patternstore
		if (firstPatternTrack)
		{
			ptSettings.appendChild(patternstoreElem);
			firstPatternTrack = false;
		}

		trackElem.appendChild(ptSettings);

		// Pattern clips for this track
		SqliteStmt clipStmt(db,
			"SELECT * FROM pattern_clip WHERE pattern_track_id = ? ORDER BY position");
		if (clipStmt.valid())
		{
			clipStmt.bindInt(1, ptId);
			while (clipStmt.step())
			{
				QDomElement clipElem = doc.createElement("patternclip");
				clipElem.setAttribute("pos", clipStmt.colInt(3));    // position
				clipElem.setAttribute("len", clipStmt.colInt(4));    // length
				clipElem.setAttribute("off", clipStmt.colInt(5));    // start_offset
				clipElem.setAttribute("muted", clipStmt.colInt(6));  // muted
				clipElem.setAttribute("name", clipStmt.colText(7));  // name
				if (!clipStmt.colIsNull(8))
				{
					clipElem.setAttribute("color", clipStmt.colText(8));
				}

				// Restore extra pattern clip attributes (column 9: extra_json)
				applyExtraJson(doc, clipElem, clipStmt.colText(9));

				trackElem.appendChild(clipElem);
			}
		}

		songTc.appendChild(trackElem);
		trackCount++;
	}

	logMsg("Pattern tracks: %d tracks", trackCount);
}

// Export automation tracks
void exportAutomationTracks(sqlite3* db, QDomDocument& doc, QDomElement& parentElem)
{
	SqliteStmt atStmt(db, "SELECT * FROM automation_track ORDER BY sort_order");
	if (!atStmt.valid()) { return; }

	int trackCount = 0;
	while (atStmt.step())
	{
		int atId = atStmt.colInt(0);

		QDomElement trackElem = doc.createElement("track");
		trackElem.setAttribute("type", 5);
		trackElem.setAttribute("name", atStmt.colText(1));
		trackElem.setAttribute("muted", atStmt.colInt(2));
		trackElem.setAttribute("solo", atStmt.colInt(3));
		trackElem.setAttribute("mutedBeforeSolo", 0);
		if (!atStmt.colIsNull(4))
		{
			trackElem.setAttribute("color", atStmt.colText(4));
		}

		// Restore extra automation track attributes (column 6: extra_json)
		applyExtraJson(doc, trackElem, atStmt.colText(6));

		QDomElement atSettings = doc.createElement("automationtrack");
		trackElem.appendChild(atSettings);

		// Automation clips
		SqliteStmt clipStmt(db,
			"SELECT * FROM automation_clip WHERE automation_track_id = ? ORDER BY position");
		if (clipStmt.valid())
		{
			clipStmt.bindInt(1, atId);
			while (clipStmt.step())
			{
				int clipId = clipStmt.colInt(0);

				QDomElement clipElem = doc.createElement("automationclip");
				clipElem.setAttribute("pos", clipStmt.colInt(2));     // position
				clipElem.setAttribute("len", clipStmt.colInt(3));     // length
				clipElem.setAttribute("prog", clipStmt.colInt(4));    // progression_type
				clipElem.setAttribute("tens",
					QString::number(clipStmt.colDouble(5), 'g', 15)); // tension
				clipElem.setAttribute("mute", clipStmt.colInt(6));    // muted
				clipElem.setAttribute("name", clipStmt.colText(7));   // name
				if (!clipStmt.colIsNull(8))
				{
					clipElem.setAttribute("color", clipStmt.colText(8));
				}

				// Restore extra automation clip attributes (column 9: extra_json)
				applyExtraJson(doc, clipElem, clipStmt.colText(9));

				// Time nodes
				SqliteStmt nodeStmt(db,
					"SELECT * FROM automation_node WHERE automation_clip_id = ? ORDER BY position");
				if (nodeStmt.valid())
				{
					nodeStmt.bindInt(1, clipId);
					while (nodeStmt.step())
					{
						QDomElement timeElem = doc.createElement("time");
						timeElem.setAttribute("pos", nodeStmt.colInt(2));
						timeElem.setAttribute("value",
							QString::number(nodeStmt.colDouble(3), 'g', 15));
						timeElem.setAttribute("outValue",
							QString::number(nodeStmt.colDouble(4), 'g', 15));
						timeElem.setAttribute("inTan",
							QString::number(nodeStmt.colDouble(5), 'g', 15));
						timeElem.setAttribute("outTan",
							QString::number(nodeStmt.colDouble(6), 'g', 15));
						timeElem.setAttribute("lockedTan", nodeStmt.colInt(7));
						clipElem.appendChild(timeElem);
					}
				}

				// Automation targets (object references)
				SqliteStmt targetStmt(db,
					"SELECT * FROM automation_target WHERE automation_clip_id = ?");
				if (targetStmt.valid())
				{
					targetStmt.bindInt(1, clipId);
					while (targetStmt.step())
					{
						QDomElement objElem = doc.createElement("object");
						objElem.setAttribute("id", targetStmt.colInt(2)); // target_object_id
						clipElem.appendChild(objElem);
					}
				}

				trackElem.appendChild(clipElem);
			}
		}

		parentElem.appendChild(trackElem);
		trackCount++;
	}

	if (trackCount > 0)
	{
		logMsg("Automation: %d tracks", trackCount);
	}
}

// Export sample tracks
void exportSampleTracks(sqlite3* db, QDomDocument& doc, QDomElement& parentElem)
{
	SqliteStmt stStmt(db, "SELECT * FROM sample_track ORDER BY sort_order");
	if (!stStmt.valid()) { return; }

	int trackCount = 0;
	while (stStmt.step())
	{
		int stId = stStmt.colInt(0);

		QDomElement trackElem = doc.createElement("track");
		trackElem.setAttribute("type", 2);
		trackElem.setAttribute("name", stStmt.colText(1));
		trackElem.setAttribute("muted", stStmt.colInt(5));
		trackElem.setAttribute("solo", stStmt.colInt(6));
		trackElem.setAttribute("mutedBeforeSolo", 0);
		if (!stStmt.colIsNull(7))
		{
			trackElem.setAttribute("color", stStmt.colText(7));
		}

		// Restore extra sample track attributes (column 9: extra_json)
		applyExtraJson(doc, trackElem, stStmt.colText(9));

		QDomElement stElem = doc.createElement("sampletrack");
		stElem.setAttribute("vol",
			QString::number(stStmt.colDouble(2), 'g', 15));
		stElem.setAttribute("pan",
			QString::number(stStmt.colDouble(3), 'g', 15));
		if (!stStmt.colIsNull(4))
		{
			stElem.setAttribute("mixch", stStmt.colInt(4));
		}

		exportEffects(db, doc, stElem, "sample_track", stId);
		trackElem.appendChild(stElem);

		// Sample clips
		SqliteStmt clipStmt(db,
			"SELECT * FROM sample_clip WHERE sample_track_id = ? ORDER BY position");
		if (clipStmt.valid())
		{
			clipStmt.bindInt(1, stId);
			while (clipStmt.step())
			{
				QDomElement clipElem = doc.createElement("sampleclip");
				clipElem.setAttribute("pos", clipStmt.colInt(2));    // position
				clipElem.setAttribute("len", clipStmt.colInt(3));    // length
				clipElem.setAttribute("src", clipStmt.colText(4));   // source_path
				clipElem.setAttribute("muted", clipStmt.colInt(5));  // muted
				clipElem.setAttribute("name", clipStmt.colText(6));  // name
				if (!clipStmt.colIsNull(7))
				{
					clipElem.setAttribute("color", clipStmt.colText(7));
				}

				// Restore extra sample clip attributes (column 8: extra_json)
				applyExtraJson(doc, clipElem, clipStmt.colText(8));

				trackElem.appendChild(clipElem);
			}
		}

		parentElem.appendChild(trackElem);
		trackCount++;
	}

	if (trackCount > 0)
	{
		logMsg("Sample tracks: %d tracks", trackCount);
	}
}

// Export controllers
void exportControllers(sqlite3* db, QDomDocument& doc, QDomElement& songElem)
{
	SqliteStmt ctrlStmt(db, "SELECT * FROM controller ORDER BY id");
	if (!ctrlStmt.valid()) { return; }

	QDomElement controllersElem = doc.createElement("controllers");
	int count = 0;

	while (ctrlStmt.step())
	{
		QDomElement ctrlElem = doc.createElement("controller");
		QString paramsJson = ctrlStmt.colText(3); // params_json
		applyJsonColumn(doc, ctrlElem, paramsJson);
		controllersElem.appendChild(ctrlElem);
		count++;
	}

	if (count > 0)
	{
		songElem.appendChild(controllersElem);
		logMsg("Controllers: %d", count);
	}
}

} // anonymous namespace


bool SqliteToXml::isSqliteProject(const QString& fileName)
{
	return fileName.endsWith(".lmms-db", Qt::CaseInsensitive);
}


QByteArray SqliteToXml::convert(const QString& dbPath)
{
	logMsg("Reading %s", dbPath.toUtf8().constData());

	sqlite3* db = nullptr;
	int rc = sqlite3_open_v2(dbPath.toUtf8().constData(), &db,
		SQLITE_OPEN_READONLY, nullptr);
	if (rc != SQLITE_OK)
	{
		logMsg("ERROR: Failed to open database: %s", sqlite3_errmsg(db));
		if (db) { sqlite3_close(db); }
		return {};
	}

	// Build the pattern_id → index mapping
	QMap<int, int> patternIdToIndex;
	{
		SqliteStmt pStmt(db, "SELECT id, sort_order FROM pattern ORDER BY sort_order");
		if (pStmt.valid())
		{
			while (pStmt.step())
			{
				patternIdToIndex.insert(pStmt.colInt(0), pStmt.colInt(1));
			}
		}
	}

	// Build XML document
	QDomDocument doc;
	QDomProcessingInstruction xmlDecl = doc.createProcessingInstruction(
		"xml", "version=\"1.0\"");
	doc.appendChild(xmlDecl);

	QDomElement root = doc.createElement("lmms-project");
	root.setAttribute("version", "30");
	root.setAttribute("type", "song");
	root.setAttribute("creator", "LMMS");
	root.setAttribute("creatorversion", "1.3.0-alpha");
	doc.appendChild(root);

	// <head>
	QDomElement head = doc.createElement("head");
	exportProjectMetadata(db, doc, head);
	root.appendChild(head);

	// <song>
	QDomElement song = doc.createElement("song");

	// Song trackcontainer
	QDomElement songTc = doc.createElement("trackcontainer");
	songTc.setAttribute("type", "song");
	songTc.setAttribute("visible", 1);
	songTc.setAttribute("minimized", 0);
	songTc.setAttribute("maximized", 0);
	songTc.setAttribute("x", 0);
	songTc.setAttribute("y", 0);
	songTc.setAttribute("width", 1600);
	songTc.setAttribute("height", 900);

	// Build patternstore as a detached element
	QDomElement patternstore = doc.createElement("trackcontainer");
	patternstore.setAttribute("type", "patternstore");
	patternstore.setAttribute("visible", 1);
	patternstore.setAttribute("minimized", 0);
	patternstore.setAttribute("maximized", 1);
	patternstore.setAttribute("x", 0);
	patternstore.setAttribute("y", 0);
	patternstore.setAttribute("width", 1527);
	patternstore.setAttribute("height", 768);

	// Populate patternstore with instrument tracks
	exportPatternstore(db, doc, patternstore, patternIdToIndex);

	// Pattern tracks (first one gets the patternstore)
	exportPatternTracks(db, doc, songTc, patternstore, patternIdToIndex);

	// Automation tracks (in song trackcontainer)
	exportAutomationTracks(db, doc, songTc);

	// Sample tracks (in song trackcontainer)
	exportSampleTracks(db, doc, songTc);

	song.appendChild(songTc);

	// Mixer
	exportMixer(db, doc, song);

	// Controllers
	exportControllers(db, doc, song);

	root.appendChild(song);

	sqlite3_close(db);

	// Serialize to XML bytes
	QByteArray xmlData;
	QTextStream stream(&xmlData);
	stream.setCodec("UTF-8");
	doc.save(stream, 2);
	stream.flush();

	logMsg("Done! Generated %d bytes of XML", xmlData.size());
	return xmlData;
}

} // namespace lmms
