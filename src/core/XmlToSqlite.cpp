/*
 * XmlToSqlite.cpp - converts LMMS XML (QDomDocument) to SQLite .lmms-db format
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

#include "XmlToSqlite.h"

#include <sqlite3.h>

#include <QDomElement>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include <cstdio>

namespace lmms
{

namespace
{

constexpr int DEFAULT_PATTERN_LENGTH = 192;

void logMsg(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[XmlToSqlite] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

// Normalize a track label using the same rules as Track::normalizeTrackNames:
// 1. Remove "Clone of" prefix  2. Replace whitespace with underscores
// 3. Append _001, _002 etc. for conflicts
QString normalizeTrackLabel(const QString& rawName, const QSet<QString>& existingNames)
{
	static const QRegularExpression cloneJank(
		R"([Cc]lone[\s+\-_]+of[\s+\-_]+)", QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression whitespace(R"(\s+)");

	QString candidate = rawName;
	candidate.remove(cloneJank);
	candidate = candidate.trimmed();
	candidate.replace(whitespace, "_");

	if (!existingNames.contains(candidate)) { return candidate; }

	// Conflict: append or increment 3-digit counter
	int len = candidate.length();
	bool hasCounter = (len >= 3
		&& candidate[len - 3].isDigit() && candidate[len - 3] == '0'
		&& candidate[len - 2].isDigit()
		&& candidate[len - 1].isDigit());

	if (hasCounter)
	{
		QString base = candidate.left(len - 3);
		int existing = candidate.mid(len - 3).toInt();
		for (int c = existing + 1; c < 1000; ++c)
		{
			candidate = QString("%1%2").arg(base).arg(c, 3, 10, QChar('0'));
			if (!existingNames.contains(candidate)) { return candidate; }
		}
	}
	else
	{
		QString base = candidate + "_";
		for (int c = 1; c < 1000; ++c)
		{
			candidate = QString("%1%2").arg(base).arg(c, 3, 10, QChar('0'));
			if (!existingNames.contains(candidate)) { return candidate; }
		}
	}
	return candidate;
}

// RAII sqlite3 wrapper
class SqliteDb
{
public:
	SqliteDb() : m_db(nullptr) {}
	~SqliteDb() { close(); }

	bool open(const QString& path)
	{
		int rc = sqlite3_open(path.toUtf8().constData(), &m_db);
		if (rc != SQLITE_OK)
		{
			logMsg("ERROR: Failed to open database: %s", sqlite3_errmsg(m_db));
			return false;
		}
		exec("PRAGMA journal_mode=WAL");
		exec("PRAGMA foreign_keys=ON");
		return true;
	}

	void close()
	{
		if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
	}

	bool exec(const char* sql)
	{
		char* errMsg = nullptr;
		int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
		if (rc != SQLITE_OK)
		{
			logMsg("SQL error: %s — %s", sql, errMsg ? errMsg : "unknown");
			sqlite3_free(errMsg);
			return false;
		}
		return true;
	}

	sqlite3* get() const { return m_db; }

	sqlite3_int64 lastInsertId() const { return sqlite3_last_insert_rowid(m_db); }

private:
	sqlite3* m_db;
};

// RAII sqlite3_stmt
class Stmt
{
public:
	Stmt(sqlite3* db, const char* sql) : m_stmt(nullptr)
	{
		if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK)
		{
			logMsg("SQL prepare error: %s — %s", sql, sqlite3_errmsg(db));
			m_stmt = nullptr;
		}
	}
	~Stmt() { if (m_stmt) sqlite3_finalize(m_stmt); }

	bool valid() const { return m_stmt != nullptr; }

	void bindInt(int idx, int val) { sqlite3_bind_int(m_stmt, idx, val); }
	void bindInt64(int idx, sqlite3_int64 val) { sqlite3_bind_int64(m_stmt, idx, val); }
	void bindDouble(int idx, double val) { sqlite3_bind_double(m_stmt, idx, val); }
	void bindText(int idx, const QString& val)
	{
		QByteArray utf8 = val.toUtf8();
		sqlite3_bind_text(m_stmt, idx, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
	}
	void bindNull(int idx) { sqlite3_bind_null(m_stmt, idx); }

	bool step() { return sqlite3_step(m_stmt) == SQLITE_ROW; }

	bool exec()
	{
		int rc = sqlite3_step(m_stmt);
		return rc == SQLITE_DONE || rc == SQLITE_ROW;
	}

	void reset() { sqlite3_reset(m_stmt); sqlite3_clear_bindings(m_stmt); }

	Stmt(const Stmt&) = delete;
	Stmt& operator=(const Stmt&) = delete;

private:
	sqlite3_stmt* m_stmt;
};

// Convert a QDomElement and all its children to a JSON object
QJsonObject elemToJson(const QDomElement& elem)
{
	QJsonObject obj;

	// Attributes become key-value pairs
	auto attrs = elem.attributes();
	for (int i = 0; i < attrs.count(); ++i)
	{
		auto attr = attrs.item(i).toAttr();
		obj.insert(attr.name(), QJsonValue(attr.value()));
	}

	// Runtime-only elements that LMMS generates at load time (never persisted in .mmp)
	static const QSet<QString> runtimeOnlyTags = {"journallingObject"};

	// Child elements become nested objects or arrays
	// Track every child element occurrence to preserve interleaved ordering
	QJsonArray orderArr;

	auto child = elem.firstChildElement();
	while (!child.isNull())
	{
		QString tag = child.tagName();
		if (runtimeOnlyTags.contains(tag))
		{
			child = child.nextSiblingElement();
			continue;
		}
		QJsonObject childObj = elemToJson(child);

		// Record every child element in sequence for faithful reconstruction
		orderArr.append(tag);

		if (obj.contains(tag))
		{
			// Convert to array if not already
			auto existing = obj.value(tag);
			if (existing.isArray())
			{
				QJsonArray arr = existing.toArray();
				arr.append(childObj);
				obj.insert(tag, arr);
			}
			else
			{
				QJsonArray arr;
				arr.append(existing);
				arr.append(childObj);
				obj.insert(tag, arr);
			}
		}
		else
		{
			obj.insert(tag, childObj);
		}

		child = child.nextSiblingElement();
	}

	// Store element order for faithful XML reconstruction
	if (!orderArr.isEmpty())
	{
		obj.insert("_order", orderArr);
	}

	// Capture direct text/CDATA content (not descendant text)
	// QDomElement::text() concatenates ALL descendant text which is wrong here
	QString textContent;
	QDomNode textNode = elem.firstChild();
	while (!textNode.isNull())
	{
		if (textNode.isText() || textNode.isCDATASection())
		{
			textContent += textNode.toText().data();
		}
		textNode = textNode.nextSibling();
	}
	textContent = textContent.trimmed();
	if (!textContent.isEmpty())
	{
		obj.insert("_text", textContent);
	}

	return obj;
}

// Serialize a QJsonObject to a compact JSON string
QString jsonToString(const QJsonObject& obj)
{
	if (obj.isEmpty()) { return "{}"; }
	return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// Serialize a QJsonArray to a compact JSON string
QString jsonArrayToString(const QJsonArray& arr)
{
	if (arr.isEmpty()) { return "[]"; }
	return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// Capture all attributes NOT in knownKeys as a QJsonObject
QJsonObject extraAttrs(const QDomElement& elem, const QSet<QString>& knownKeys)
{
	QJsonObject result;
	auto attrs = elem.attributes();
	for (int i = 0; i < attrs.count(); ++i)
	{
		auto attr = attrs.item(i).toAttr();
		if (!knownKeys.contains(attr.name()))
		{
			result.insert(attr.name(), attr.value());
		}
	}
	return result;
}

// Capture all child elements whose tags are NOT in knownTags as JSON
QJsonObject extraChildren(const QDomElement& elem, const QSet<QString>& knownTags)
{
	QJsonObject result;
	auto child = elem.firstChildElement();
	while (!child.isNull())
	{
		if (!knownTags.contains(child.tagName()))
		{
			QJsonObject childObj = elemToJson(child);
			if (result.contains(child.tagName()))
			{
				auto existing = result.value(child.tagName());
				if (existing.isArray())
				{
					QJsonArray arr = existing.toArray();
					arr.append(childObj);
					result.insert(child.tagName(), arr);
				}
				else
				{
					QJsonArray arr;
					arr.append(existing);
					arr.append(childObj);
					result.insert(child.tagName(), arr);
				}
			}
			else
			{
				result.insert(child.tagName(), childObj);
			}
		}
		child = child.nextSiblingElement();
	}
	return result;
}

// Find child elements by tag name, also checking legacy names
QList<QDomElement> findClipElements(const QDomElement& parent, const QString& tagName)
{
	// Map of legacy → current names
	static const QMap<QString, QString> legacyMap = {
		{"automationpattern", "automationclip"},
		{"bbtco", "patternclip"},
		{"pattern", "midiclip"},
		{"sampletco", "sampleclip"},
	};

	QList<QDomElement> results;
	auto child = parent.firstChildElement(tagName);
	while (!child.isNull())
	{
		results.append(child);
		child = child.nextSiblingElement(tagName);
	}

	// Check legacy names
	for (auto it = legacyMap.constBegin(); it != legacyMap.constEnd(); ++it)
	{
		if (it.value() == tagName)
		{
			auto legacy = parent.firstChildElement(it.key());
			while (!legacy.isNull())
			{
				results.append(legacy);
				legacy = legacy.nextSiblingElement(it.key());
			}
		}
	}

	return results;
}

// Insert all effects from an <fxchain> into the database
void insertEffectsFromChain(SqliteDb& db, const QDomElement& fxchain,
	const char* ownerType, sqlite3_int64 ownerId)
{
	static const QSet<QString> knownAttrs = {"name", "on", "wet", "gate"};

	// Capture fxchain-level enabled attribute
	int fxchainEnabled = fxchain.attribute("enabled", "1").toInt();

	int fxIdx = 0;
	auto fxElem = fxchain.firstChildElement("effect");
	while (!fxElem.isNull())
	{
		QString pluginName = fxElem.attribute("name", "unknown");
		int enabled = fxElem.attribute("on", "1").toInt();
		double wet = fxElem.attribute("wet", "1.0").toDouble();
		double gate = fxElem.attribute("gate", "0.0").toDouble();

		QJsonObject params;
		// Capture extra attributes on <effect> beyond name/on/wet/gate
		QJsonObject fxExtras = extraAttrs(fxElem, knownAttrs);
		if (!fxExtras.isEmpty())
		{
			params.insert("_extra_attrs", fxExtras);
		}
		// Capture child elements
		auto child = fxElem.firstChildElement();
		while (!child.isNull())
		{
			if (child.tagName() == "key") { params.insert("_key", elemToJson(child)); }
			else { params.insert(child.tagName(), elemToJson(child)); }
			child = child.nextSiblingElement();
		}

		Stmt fxStmt(db.get(),
			"INSERT INTO effect (owner_type, owner_id, plugin_name, sort_order, enabled, wet, gate, fxchain_enabled, params_json) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
		if (fxStmt.valid())
		{
			fxStmt.bindText(1, ownerType);
			fxStmt.bindInt64(2, ownerId);
			fxStmt.bindText(3, pluginName);
			fxStmt.bindInt(4, fxIdx);
			fxStmt.bindInt(5, enabled);
			fxStmt.bindDouble(6, wet);
			fxStmt.bindDouble(7, gate);
			fxStmt.bindInt(8, fxchainEnabled);
			fxStmt.bindText(9, jsonToString(params));
			fxStmt.exec();
		}
		fxIdx++;
		fxElem = fxElem.nextSiblingElement("effect");
	}

	// If fxchain has no effects but has attributes, store a placeholder row
	// so we don't lose the fxchain enabled state
	if (fxIdx == 0 && fxchain.hasAttributes())
	{
		Stmt fxStmt(db.get(),
			"INSERT INTO effect (owner_type, owner_id, plugin_name, sort_order, enabled, wet, gate, fxchain_enabled, params_json) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
		if (fxStmt.valid())
		{
			fxStmt.bindText(1, ownerType);
			fxStmt.bindInt64(2, ownerId);
			fxStmt.bindText(3, "_fxchain_metadata");  // sentinel: not a real effect
			fxStmt.bindInt(4, 0);
			fxStmt.bindInt(5, 0);
			fxStmt.bindDouble(6, 0);
			fxStmt.bindDouble(7, 0);
			fxStmt.bindInt(8, fxchainEnabled);
			fxStmt.bindText(9, "{}");
			fxStmt.exec();
		}
	}
}

// Extract instrument track data from a <track type="0"> element
struct InstrumentTrackData
{
	QString name;
	int muted = 0;
	int solo = 0;
	double volume = 100;
	double panning = 0;
	double pitch = 0;
	int pitchRange = 1;
	int mixerChannelId = -1; // -1 means NULL
	int baseNote = 69;
	int useMasterPitch = 1;
	QString color;
	QString instrumentPlugin;
	QString instrumentParamsJson;
	QString soundShapingJson;
	QString arpJson;
	QString chordJson;
	QString midiJson;
	QString microtunerJson;
	QString trackExtraJson;
	QString instrumenttrackExtraJson;
	bool valid = false;
};

InstrumentTrackData extractInstrumentTrackData(const QDomElement& trackElem)
{
	InstrumentTrackData data;
	auto itElem = trackElem.firstChildElement("instrumenttrack");
	if (itElem.isNull()) { return data; }

	data.valid = true;
	data.name = trackElem.attribute("name", "");
	data.muted = trackElem.attribute("muted", "0").toInt();
	data.solo = trackElem.attribute("solo", "0").toInt();
	data.volume = itElem.attribute("vol", "100").toDouble();
	data.panning = itElem.attribute("pan", "0").toDouble();
	data.pitch = itElem.attribute("pitch", "0").toDouble();
	data.pitchRange = itElem.attribute("pitchrange", "1").toInt();

	QString mixch = itElem.attribute("mixch", itElem.attribute("fxch", ""));
	if (!mixch.isEmpty()) { data.mixerChannelId = mixch.toInt(); }

	data.baseNote = itElem.attribute("basenote", "69").toInt();
	data.useMasterPitch = itElem.attribute("usemasterpitch", "1").toInt();
	data.color = trackElem.attribute("color", "");

	// Capture ALL extra track attributes
	static const QSet<QString> knownTrackAttrs = {"type", "name", "muted", "solo", "color"};
	data.trackExtraJson = jsonToString(extraAttrs(trackElem, knownTrackAttrs));

	// Capture ALL extra instrumenttrack attributes + unknown children
	static const QSet<QString> knownItAttrs = {"vol", "pan", "pitch", "pitchrange", "mixch", "fxch", "basenote", "usemasterpitch"};
	QJsonObject itExtras = extraAttrs(itElem, knownItAttrs);
	static const QSet<QString> knownItChildren = {"instrument", "eldata", "arpeggiator", "chordcreator", "midiport", "fxchain", "microtuner"};
	QJsonObject unknownChildren = extraChildren(itElem, knownItChildren);
	if (!unknownChildren.isEmpty())
	{
		itExtras.insert("_children", unknownChildren);
	}
	data.instrumenttrackExtraJson = jsonToString(itExtras);

	// Instrument plugin
	auto instrument = itElem.firstChildElement("instrument");
	if (!instrument.isNull())
	{
		data.instrumentPlugin = instrument.attribute("name", "unknown");
		QJsonObject pluginParams;
		auto pluginChild = instrument.firstChildElement();
		QDomElement keyElem;
		while (!pluginChild.isNull())
		{
			if (pluginChild.tagName() == "key")
			{
				keyElem = pluginChild;
			}
			else if (pluginParams.isEmpty())
			{
				pluginParams = elemToJson(pluginChild);
			}
			pluginChild = pluginChild.nextSiblingElement();
		}
		// Store <key> alongside plugin params for round-trip fidelity
		if (!keyElem.isNull())
		{
			pluginParams.insert("_key", elemToJson(keyElem));
		}
		data.instrumentParamsJson = jsonToString(pluginParams);
	}
	if (data.instrumentPlugin.isEmpty()) { data.instrumentPlugin = "unknown"; }
	if (data.instrumentParamsJson.isEmpty()) { data.instrumentParamsJson = "{}"; }

	// Sound shaping
	auto eldata = itElem.firstChildElement("eldata");
	data.soundShapingJson = eldata.isNull() ? "{}" : jsonToString(elemToJson(eldata));

	// Arpeggiator
	auto arp = itElem.firstChildElement("arpeggiator");
	data.arpJson = arp.isNull() ? "{}" : jsonToString(elemToJson(arp));

	// Chord creator
	auto chord = itElem.firstChildElement("chordcreator");
	data.chordJson = chord.isNull() ? "{}" : jsonToString(elemToJson(chord));

	// MIDI port
	auto midi = itElem.firstChildElement("midiport");
	data.midiJson = midi.isNull() ? "{}" : jsonToString(elemToJson(midi));

	// Microtuner
	auto micro = itElem.firstChildElement("microtuner");
	data.microtunerJson = micro.isNull() ? "{}" : jsonToString(elemToJson(micro));

	return data;
}

sqlite3_int64 insertInstrumentTrack(SqliteDb& db, const InstrumentTrackData& data,
	int sortOrder, const char* containerType = "patternstore")
{
	Stmt stmt(db.get(),
		"INSERT INTO instrument_track "
		"(container_type, name, volume, panning, pitch, pitch_range, mixer_channel_id, base_note, "
		"use_master_pitch, muted, solo, color, sort_order, "
		"instrument_plugin, instrument_params_json, sound_shaping_json, "
		"arpeggio_json, chord_creator_json, midi_port_json, microtuner_json, "
		"track_extra_json, instrumenttrack_extra_json) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
	if (!stmt.valid()) { return -1; }

	stmt.bindText(1, containerType);
	stmt.bindText(2, data.name);
	stmt.bindDouble(3, data.volume);
	stmt.bindDouble(4, data.panning);
	stmt.bindDouble(5, data.pitch);
	stmt.bindInt(6, data.pitchRange);
	if (data.mixerChannelId >= 0) { stmt.bindInt(7, data.mixerChannelId); }
	else { stmt.bindNull(7); }
	stmt.bindInt(8, data.baseNote);
	stmt.bindInt(9, data.useMasterPitch);
	stmt.bindInt(10, data.muted);
	stmt.bindInt(11, data.solo);
	if (!data.color.isEmpty()) { stmt.bindText(12, data.color); }
	else { stmt.bindNull(12); }
	stmt.bindInt(13, sortOrder);
	stmt.bindText(14, data.instrumentPlugin);
	stmt.bindText(15, data.instrumentParamsJson);
	stmt.bindText(16, data.soundShapingJson);
	stmt.bindText(17, data.arpJson);
	stmt.bindText(18, data.chordJson);
	stmt.bindText(19, data.midiJson);
	stmt.bindText(20, data.microtunerJson);
	stmt.bindText(21, data.trackExtraJson);
	stmt.bindText(22, data.instrumenttrackExtraJson);
	stmt.exec();

	return db.lastInsertId();
}

int convertNotes(SqliteDb& db, const QDomElement& clipElem, sqlite3_int64 midiClipId)
{
	static const QSet<QString> knownNoteAttrs = {"pos", "len", "key", "vol", "pan", "type"};

	int count = 0;
	auto noteElem = clipElem.firstChildElement("note");
	while (!noteElem.isNull())
	{
		int pos = noteElem.attribute("pos", "0").toInt();
		int len = noteElem.attribute("len", "0").toInt();
		int key = noteElem.attribute("key", "69").toInt();
		int vol = noteElem.attribute("vol", "100").toInt();
		int pan = noteElem.attribute("pan", "0").toInt();
		int noteType = noteElem.attribute("type", "0").toInt();
		QJsonObject noteExtras = extraAttrs(noteElem, knownNoteAttrs);

		Stmt stmt(db.get(),
			"INSERT INTO note (midi_clip_id, position, length, key, volume, panning, note_type, extra_json) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindInt64(1, midiClipId);
			stmt.bindInt(2, pos);
			stmt.bindInt(3, len);
			stmt.bindInt(4, key);
			stmt.bindInt(5, vol);
			stmt.bindInt(6, pan);
			stmt.bindInt(7, noteType);
			stmt.bindText(8, jsonToString(noteExtras));
			stmt.exec();
		}
		sqlite3_int64 noteId = db.lastInsertId();

		// Check for detuning automation
		auto detuning = noteElem.firstChildElement("detuning");
		if (!detuning.isNull())
		{
			auto autoPat = detuning.firstChildElement("automationpattern");
			if (autoPat.isNull()) { autoPat = detuning.firstChildElement("automationclip"); }
			if (!autoPat.isNull())
			{
				auto timeElem = autoPat.firstChildElement("time");
				while (!timeElem.isNull())
				{
					int tPos = timeElem.attribute("pos", "0").toInt();
					double tValue = timeElem.attribute("value", "0").toDouble();
					QString outValStr = timeElem.attribute("outValue", "");
					double tInTan = timeElem.attribute("inTan", "0").toDouble();
					double tOutTan = timeElem.attribute("outTan", "0").toDouble();

					Stmt dtStmt(db.get(),
						"INSERT INTO note_detuning (note_id, position, value, out_value, in_tangent, out_tangent) "
						"VALUES (?, ?, ?, ?, ?, ?)");
					if (dtStmt.valid())
					{
						dtStmt.bindInt64(1, noteId);
						dtStmt.bindInt(2, tPos);
						dtStmt.bindDouble(3, tValue);
						if (!outValStr.isEmpty()) { dtStmt.bindDouble(4, outValStr.toDouble()); }
						else { dtStmt.bindNull(4); }
						dtStmt.bindDouble(5, tInTan);
						dtStmt.bindDouble(6, tOutTan);
						dtStmt.exec();
					}

					timeElem = timeElem.nextSiblingElement("time");
				}
			}
		}

		count++;
		noteElem = noteElem.nextSiblingElement("note");
	}
	return count;
}

void convertProjectMetadata(SqliteDb& db, const QDomElement& root)
{
	auto head = root.firstChildElement("head");
	if (head.isNull())
	{
		head = root.firstChildElement().firstChildElement("head");
	}

	double bpm = 140;
	int tsNum = 4, tsDen = 4;
	double masterVol = 100, masterPitch = 0;
	QString extraJson = "{}";

	// Root element attributes
	QString lmmsVersion = root.attribute("version", "30");
	QString projectType = root.attribute("type", "song");
	QString creator = root.attribute("creator", "LMMS");
	QString creatorVersion = root.attribute("creatorversion", "1.3.0-alpha");

	if (!head.isNull())
	{
		bpm = head.attribute("bpm", "140").toDouble();
		tsNum = head.attribute("timesig_numerator", "4").toInt();
		tsDen = head.attribute("timesig_denominator", "4").toInt();
		masterVol = head.attribute("mastervol", "100").toDouble();
		masterPitch = head.attribute("masterpitch", "0").toDouble();

		static const QSet<QString> knownHeadAttrs = {"bpm", "timesig_numerator", "timesig_denominator", "mastervol", "masterpitch"};
		extraJson = jsonToString(extraAttrs(head, knownHeadAttrs));
	}

	Stmt stmt(db.get(),
		"INSERT INTO project (id, name, bpm, timesig_numerator, timesig_denominator, "
		"master_volume, master_pitch, lmms_version, project_type, creator, creator_version, extra_json) "
		"VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
	if (stmt.valid())
	{
		stmt.bindText(1, "");
		stmt.bindDouble(2, bpm);
		stmt.bindInt(3, tsNum);
		stmt.bindInt(4, tsDen);
		stmt.bindDouble(5, masterVol);
		stmt.bindDouble(6, masterPitch);
		stmt.bindText(7, lmmsVersion);
		stmt.bindText(8, projectType);
		stmt.bindText(9, creator);
		stmt.bindText(10, creatorVersion);
		stmt.bindText(11, extraJson);
		stmt.exec();
	}
	logMsg("Project: bpm=%g, time_sig=%d/%d, version=%s", bpm, tsNum, tsDen,
		lmmsVersion.toUtf8().constData());
}

// Store song-level UI elements (pianoroll, automationeditor, projectnotes, timeline, etc.)
void convertSongUiElements(SqliteDb& db, const QDomElement& songElem)
{
	// All child elements of <song> that are NOT trackcontainer, mixer, or controllers
	static const QSet<QString> knownSongChildren = {
		"trackcontainer", "mixer", "fxmixer", "controllers", "scales", "keymaps", "track"
	};

	auto child = songElem.firstChildElement();
	while (!child.isNull())
	{
		if (!knownSongChildren.contains(child.tagName()))
		{
			QString elemName = child.tagName();
			QJsonObject attrsObj = elemToJson(child);

			// Separate text content from attributes for elements like projectnotes
			QString contentText;
			QString rawText = child.text().trimmed();
			if (!rawText.isEmpty())
			{
				contentText = rawText;
				// Remove _text from attrsObj since we store it separately
				attrsObj.remove("_text");
			}

			// Separate attributes from child elements
			QJsonObject pureAttrs;
			QJsonObject childrenObj;
			for (auto it = attrsObj.begin(); it != attrsObj.end(); ++it)
			{
				if (it.value().isObject() || it.value().isArray())
				{
					childrenObj.insert(it.key(), it.value());
				}
				else
				{
					pureAttrs.insert(it.key(), it.value());
				}
			}

			Stmt stmt(db.get(),
				"INSERT INTO song_ui_element (element_name, attributes_json, children_json, content_text) "
				"VALUES (?, ?, ?, ?)");
			if (stmt.valid())
			{
				stmt.bindText(1, elemName);
				stmt.bindText(2, jsonToString(pureAttrs));
				stmt.bindText(3, jsonToString(childrenObj));
				if (!contentText.isEmpty()) { stmt.bindText(4, contentText); }
				else { stmt.bindNull(4); }
				stmt.exec();
			}
		}
		child = child.nextSiblingElement();
	}
}

// Store trackcontainer window state
void convertTrackcontainerState(SqliteDb& db, const QDomElement& tcElem, const QString& containerType)
{
	static const QSet<QString> knownTcAttrs = {"type", "visible", "minimized", "maximized", "x", "y", "width", "height"};

	int visible = tcElem.attribute("visible", "1").toInt();
	int minimized = tcElem.attribute("minimized", "0").toInt();
	int maximized = tcElem.attribute("maximized", "0").toInt();
	int x = tcElem.attribute("x", "0").toInt();
	int y = tcElem.attribute("y", "0").toInt();
	int width = tcElem.attribute("width", "1600").toInt();
	int height = tcElem.attribute("height", "900").toInt();
	QJsonObject extras = extraAttrs(tcElem, knownTcAttrs);

	Stmt stmt(db.get(),
		"INSERT INTO trackcontainer_state (container_type, visible, minimized, maximized, x, y, width, height, extra_json) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
	if (stmt.valid())
	{
		stmt.bindText(1, containerType);
		stmt.bindInt(2, visible);
		stmt.bindInt(3, minimized);
		stmt.bindInt(4, maximized);
		stmt.bindInt(5, x);
		stmt.bindInt(6, y);
		stmt.bindInt(7, width);
		stmt.bindInt(8, height);
		stmt.bindText(9, jsonToString(extras));
		stmt.exec();
	}
}

void convertMixer(SqliteDb& db, const QDomElement& root)
{
	// Find mixer element
	auto mixer = root.firstChildElement("song").firstChildElement("mixer");
	if (mixer.isNull())
	{
		auto songElem = root.firstChildElement("song");
		if (!songElem.isNull())
		{
			mixer = songElem.firstChildElement("mixer");
			if (mixer.isNull()) { mixer = songElem.firstChildElement("fxmixer"); }
		}
	}

	if (mixer.isNull())
	{
		logMsg("No mixer found");
		return;
	}

	// Store mixer window state
	{
		static const QSet<QString> knownMixerAttrs = {"visible", "minimized", "maximized", "x", "y", "width", "height"};
		int visible = mixer.attribute("visible", "0").toInt();
		int minimized = mixer.attribute("minimized", "0").toInt();
		int maximized = mixer.attribute("maximized", "0").toInt();
		int x = mixer.attribute("x", "0").toInt();
		int y = mixer.attribute("y", "0").toInt();
		int width = mixer.attribute("width", "865").toInt();
		int height = mixer.attribute("height", "278").toInt();
		QJsonObject extras = extraAttrs(mixer, knownMixerAttrs);

		Stmt stmt(db.get(),
			"INSERT INTO mixer_state (id, visible, minimized, maximized, x, y, width, height, extra_json) "
			"VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindInt(1, visible);
			stmt.bindInt(2, minimized);
			stmt.bindInt(3, maximized);
			stmt.bindInt(4, x);
			stmt.bindInt(5, y);
			stmt.bindInt(6, width);
			stmt.bindInt(7, height);
			stmt.bindText(8, jsonToString(extras));
			stmt.exec();
		}
	}

	// Find mixer channels
	QList<QDomElement> channels;
	auto ch = mixer.firstChildElement("mixerchannel");
	if (ch.isNull()) { ch = mixer.firstChildElement("fxchannel"); }
	while (!ch.isNull())
	{
		channels.append(ch);
		ch = ch.nextSiblingElement();
		while (!ch.isNull() && ch.tagName() != "mixerchannel" && ch.tagName() != "fxchannel")
		{
			ch = ch.nextSiblingElement();
		}
	}

	if (channels.isEmpty())
	{
		logMsg("No mixer channels found");
		return;
	}

	// Known mixer channel attributes — including color now
	static const QSet<QString> knownChAttrs = {"num", "name", "volume", "muted", "soloed", "color"};

	struct DeferredRoute { int from; int to; double amount; };
	QList<DeferredRoute> deferredRoutes;
	int effectCount = 0;

	for (const auto& chElem : channels)
	{
		int chNum = chElem.attribute("num", "0").toInt();
		QString chName = chElem.attribute("name", "");
		double chVol = chElem.attribute("volume", "1.0").toDouble();
		int chMuted = chElem.attribute("muted", "0").toInt();
		int chSoloed = chElem.attribute("soloed", "0").toInt();
		QString chColor = chElem.attribute("color", "");

		// Capture extra attributes and unknown children (NOT send, fxchain, or connection)
		QJsonObject chExtras = extraAttrs(chElem, knownChAttrs);
		static const QSet<QString> knownChChildren = {"send", "fxchain", "connection"};
		QJsonObject chChildExtras = extraChildren(chElem, knownChChildren);
		if (!chChildExtras.isEmpty())
		{
			chExtras.insert("_children", chChildExtras);
		}

		Stmt stmt(db.get(),
			"INSERT INTO mixer_channel (id, name, volume, muted, soloed, color, sort_order, extra_json) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindInt(1, chNum);
			stmt.bindText(2, chName);
			stmt.bindDouble(3, chVol);
			stmt.bindInt(4, chMuted);
			stmt.bindInt(5, chSoloed);
			if (!chColor.isEmpty()) { stmt.bindText(6, chColor); }
			else { stmt.bindNull(6); }
			stmt.bindInt(7, chNum);
			stmt.bindText(8, jsonToString(chExtras));
			stmt.exec();
		}

		// Collect sends for deferred insertion
		auto sendElem = chElem.firstChildElement("send");
		while (!sendElem.isNull())
		{
			int toChannel = sendElem.attribute("channel", "0").toInt();
			double amount = sendElem.attribute("amount", "1.0").toDouble();
			deferredRoutes.append({chNum, toChannel, amount});
			sendElem = sendElem.nextSiblingElement("send");
		}

		// Effects
		auto fxchain = chElem.firstChildElement("fxchain");
		if (!fxchain.isNull())
		{
			insertEffectsFromChain(db, fxchain, "mixer_channel", chNum);
			effectCount++;
		}

		// Controller connections on mixer channel parameters
		auto connElem = chElem.firstChildElement("connection");
		while (!connElem.isNull())
		{
			// Each <connection> contains a child element named after the parameter (e.g. <muted>)
			// and that child may contain a controller connection element
			auto paramChild = connElem.firstChildElement();
			while (!paramChild.isNull())
			{
				QString paramName = paramChild.tagName();
				// Look for controller connection inside
				auto ctrlConn = paramChild.firstChildElement();
				while (!ctrlConn.isNull())
				{
					QJsonObject connJson = elemToJson(ctrlConn);
					Stmt ccStmt(db.get(),
						"INSERT INTO controller_connection (owner_type, owner_id, param_name, controller_id, connection_json) "
						"VALUES (?, ?, ?, ?, ?)");
					if (ccStmt.valid())
					{
						ccStmt.bindText(1, "mixer_channel");
						ccStmt.bindInt(2, chNum);
						ccStmt.bindText(3, paramName);
						ccStmt.bindNull(4); // inline controller, not in controller table
						ccStmt.bindText(5, jsonToString(connJson));
						ccStmt.exec();
					}
					ctrlConn = ctrlConn.nextSiblingElement();
				}
				paramChild = paramChild.nextSiblingElement();
			}
			connElem = connElem.nextSiblingElement("connection");
		}
	}

	// Insert routes now that all channels exist
	for (const auto& route : deferredRoutes)
	{
		Stmt stmt(db.get(),
			"INSERT INTO mixer_route (from_channel_id, to_channel_id, amount) VALUES (?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindInt(1, route.from);
			stmt.bindInt(2, route.to);
			stmt.bindDouble(3, route.amount);
			stmt.exec();
		}
	}

	logMsg("Mixer: %d channels, %d routes, %d effect chains",
		channels.size(), deferredRoutes.size(), effectCount);
}

void convertPatternstore(SqliteDb& db, const QDomElement& pstoreElem,
	QMap<int, sqlite3_int64>& patternIdMap)
{
	// Store patternstore trackcontainer state
	convertTrackcontainerState(db, pstoreElem, "patternstore");

	// First pass: discover all pattern indices
	QSet<int> allPatternIndices;
	auto trackElem = pstoreElem.firstChildElement("track");
	while (!trackElem.isNull())
	{
		auto clips = findClipElements(trackElem, "midiclip");
		for (const auto& clip : clips)
		{
			int pos = clip.attribute("pos", "0").toInt();
			int patternIdx = pos / DEFAULT_PATTERN_LENGTH;
			allPatternIndices.insert(patternIdx);
		}
		trackElem = trackElem.nextSiblingElement("track");
	}

	// Create pattern rows
	QList<int> sorted = allPatternIndices.values();
	std::sort(sorted.begin(), sorted.end());
	for (int idx : sorted)
	{
		Stmt stmt(db.get(), "INSERT INTO pattern (name, sort_order) VALUES (?, ?)");
		if (stmt.valid())
		{
			stmt.bindText(1, QString("Pattern %1").arg(idx));
			stmt.bindInt(2, idx);
			stmt.exec();
		}
		patternIdMap.insert(idx, db.lastInsertId());
	}
	logMsg("PatternStore: %d patterns discovered", sorted.size());

	// Known midi clip attributes
	static const QSet<QString> knownMcAttrs = {"pos", "type", "steps", "muted", "mute", "name", "color", "len"};

	// Second pass: convert instrument tracks and their midiclips
	QSet<QString> usedNames;
	int itCount = 0, mcCount = 0, noteCount = 0;
	trackElem = pstoreElem.firstChildElement("track");
	while (!trackElem.isNull())
	{
		if (trackElem.attribute("type") != "0")
		{
			trackElem = trackElem.nextSiblingElement("track");
			continue;
		}

		auto data = extractInstrumentTrackData(trackElem);
		if (!data.valid)
		{
			trackElem = trackElem.nextSiblingElement("track");
			continue;
		}

		// Normalize track label (same rules as Track::normalizeTrackNames)
		data.name = normalizeTrackLabel(data.name, usedNames);
		usedNames.insert(data.name);

		sqlite3_int64 itId = insertInstrumentTrack(db, data, itCount);

		// Effects on the instrumenttrack element
		auto itElem = trackElem.firstChildElement("instrumenttrack");
		if (!itElem.isNull())
		{
			auto fxchain = itElem.firstChildElement("fxchain");
			if (!fxchain.isNull())
			{
				insertEffectsFromChain(db, fxchain, "instrument_track", itId);
			}
		}

		itCount++;

		// Convert midiclips
		auto clips = findClipElements(trackElem, "midiclip");
		for (const auto& clipElem : clips)
		{
			int pos = clipElem.attribute("pos", "0").toInt();
			int patternIdx = pos / DEFAULT_PATTERN_LENGTH;
			auto patternIt = patternIdMap.find(patternIdx);
			if (patternIt == patternIdMap.end()) { continue; }

			int clipType = clipElem.attribute("type", "1").toInt();
			int steps = clipElem.attribute("steps", "32").toInt();
			int muted = clipElem.attribute("muted",
				clipElem.attribute("mute", "0")).toInt();
			QString clipName = clipElem.attribute("name", "");
			QString clipColor = clipElem.attribute("color", "");
			QJsonObject mcExtras = extraAttrs(clipElem, knownMcAttrs);

			Stmt stmt(db.get(),
				"INSERT OR IGNORE INTO midi_clip "
				"(instrument_track_id, pattern_id, clip_type, steps, muted, name, color, extra_json) "
				"VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
			if (stmt.valid())
			{
				stmt.bindInt64(1, itId);
				stmt.bindInt64(2, *patternIt);
				stmt.bindInt(3, clipType);
				stmt.bindInt(4, steps);
				stmt.bindInt(5, muted);
				stmt.bindText(6, clipName);
				if (!clipColor.isEmpty()) { stmt.bindText(7, clipColor); }
				else { stmt.bindNull(7); }
				stmt.bindText(8, jsonToString(mcExtras));
				stmt.exec();
			}
			sqlite3_int64 mcId = db.lastInsertId();
			if (mcId > 0)
			{
				mcCount++;
				noteCount += convertNotes(db, clipElem, mcId);
			}
		}

		trackElem = trackElem.nextSiblingElement("track");
	}

	logMsg("PatternStore: %d instrument tracks, %d midi clips, %d notes",
		itCount, mcCount, noteCount);
}

// Convert song-level instrument tracks (type=0 in song trackcontainer)
void convertSongInstrumentTracks(SqliteDb& db, const QDomElement& songTc)
{
	static const QSet<QString> knownMcAttrs = {"pos", "type", "steps", "muted", "mute", "name", "color", "len"};

	QSet<QString> usedNames;
	int itCount = 0, mcCount = 0, noteCount = 0;
	auto trackElem = songTc.firstChildElement("track");
	while (!trackElem.isNull())
	{
		if (trackElem.attribute("type") != "0")
		{
			trackElem = trackElem.nextSiblingElement("track");
			continue;
		}

		auto data = extractInstrumentTrackData(trackElem);
		if (!data.valid)
		{
			trackElem = trackElem.nextSiblingElement("track");
			continue;
		}

		// Normalize track label (same rules as Track::normalizeTrackNames)
		data.name = normalizeTrackLabel(data.name, usedNames);
		usedNames.insert(data.name);

		sqlite3_int64 itId = insertInstrumentTrack(db, data, itCount, "song");

		// Effects on the instrumenttrack element
		auto itElem = trackElem.firstChildElement("instrumenttrack");
		if (!itElem.isNull())
		{
			auto fxchain = itElem.firstChildElement("fxchain");
			if (!fxchain.isNull())
			{
				insertEffectsFromChain(db, fxchain, "instrument_track", itId);
			}
		}

		itCount++;

		// Convert midiclips (positioned on song timeline, no pattern_id)
		auto clips = findClipElements(trackElem, "midiclip");
		for (const auto& clipElem : clips)
		{
			int clipPos = clipElem.attribute("pos", "0").toInt();
			int clipLen = clipElem.attribute("len", "0").toInt();
			int clipType = clipElem.attribute("type", "1").toInt();
			int steps = clipElem.attribute("steps", "32").toInt();
			int muted = clipElem.attribute("muted",
				clipElem.attribute("mute", "0")).toInt();
			QString clipName = clipElem.attribute("name", "");
			QString clipColor = clipElem.attribute("color", "");
			QJsonObject mcExtras = extraAttrs(clipElem, knownMcAttrs);

			Stmt stmt(db.get(),
				"INSERT INTO midi_clip "
				"(instrument_track_id, position, length, clip_type, steps, muted, name, color, extra_json) "
				"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
			if (stmt.valid())
			{
				stmt.bindInt64(1, itId);
				stmt.bindInt(2, clipPos);
				stmt.bindInt(3, clipLen);
				stmt.bindInt(4, clipType);
				stmt.bindInt(5, steps);
				stmt.bindInt(6, muted);
				stmt.bindText(7, clipName);
				if (!clipColor.isEmpty()) { stmt.bindText(8, clipColor); }
				else { stmt.bindNull(8); }
				stmt.bindText(9, jsonToString(mcExtras));
				stmt.exec();
			}
			sqlite3_int64 mcId = db.lastInsertId();
			if (mcId > 0)
			{
				mcCount++;
				noteCount += convertNotes(db, clipElem, mcId);
			}
		}

		trackElem = trackElem.nextSiblingElement("track");
	}

	if (itCount > 0)
	{
		logMsg("Song instrument tracks: %d tracks, %d clips, %d notes",
			itCount, mcCount, noteCount);
	}
}

void convertPatternTracks(SqliteDb& db, const QDomElement& songTc,
	QMap<int, sqlite3_int64>& patternIdMap)
{
	static const QSet<QString> knownTrackAttrs = {"type", "name", "muted", "solo", "color"};
	static const QSet<QString> knownPcAttrs = {"pos", "len", "off", "muted", "name", "color"};

	QSet<QString> usedNames;
	int ptCount = 0, pcCount = 0;
	int patternTrackIdx = 0;

	auto trackElem = songTc.firstChildElement("track");
	while (!trackElem.isNull())
	{
		if (trackElem.attribute("type") != "1")
		{
			trackElem = trackElem.nextSiblingElement("track");
			continue;
		}

		QString rawName = trackElem.attribute("name", QString("Pattern Track %1").arg(patternTrackIdx));
		QString name = normalizeTrackLabel(rawName, usedNames);
		usedNames.insert(name);
		int muted = trackElem.attribute("muted", "0").toInt();
		int solo = trackElem.attribute("solo", "0").toInt();
		QString color = trackElem.attribute("color", "");
		QJsonObject trackExtras = extraAttrs(trackElem, knownTrackAttrs);

		sqlite3_int64 thisPatternId = patternIdMap.value(patternTrackIdx, -1);

		Stmt stmt(db.get(),
			"INSERT INTO pattern_track (name, muted, solo, color, sort_order, extra_json) VALUES (?, ?, ?, ?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindText(1, name);
			stmt.bindInt(2, muted);
			stmt.bindInt(3, solo);
			if (!color.isEmpty()) { stmt.bindText(4, color); }
			else { stmt.bindNull(4); }
			stmt.bindInt(5, patternTrackIdx);
			stmt.bindText(6, jsonToString(trackExtras));
			stmt.exec();
		}
		sqlite3_int64 ptId = db.lastInsertId();
		ptCount++;

		// Convert patternclips
		auto clips = findClipElements(trackElem, "patternclip");
		for (const auto& clipElem : clips)
		{
			int clipPos = clipElem.attribute("pos", "0").toInt();
			int clipLen = clipElem.attribute("len", "0").toInt();
			int clipOff = clipElem.attribute("off", "0").toInt();
			int clipMuted = clipElem.attribute("muted", "0").toInt();
			QString clipName = clipElem.attribute("name", "");
			QString clipColor = clipElem.attribute("color", "");
			QJsonObject clipExtras = extraAttrs(clipElem, knownPcAttrs);

			if (thisPatternId < 0)
			{
				// Create pattern on demand
				Stmt pStmt(db.get(),
					"INSERT INTO pattern (name, sort_order) VALUES (?, ?)");
				if (pStmt.valid())
				{
					pStmt.bindText(1, QString("Pattern %1").arg(patternTrackIdx));
					pStmt.bindInt(2, patternTrackIdx);
					pStmt.exec();
				}
				thisPatternId = db.lastInsertId();
				patternIdMap.insert(patternTrackIdx, thisPatternId);
			}

			Stmt cStmt(db.get(),
				"INSERT INTO pattern_clip "
				"(pattern_track_id, pattern_id, position, length, start_offset, muted, name, color, extra_json) "
				"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
			if (cStmt.valid())
			{
				cStmt.bindInt64(1, ptId);
				cStmt.bindInt64(2, thisPatternId);
				cStmt.bindInt(3, clipPos);
				cStmt.bindInt(4, clipLen);
				cStmt.bindInt(5, clipOff);
				cStmt.bindInt(6, clipMuted);
				cStmt.bindText(7, clipName);
				if (!clipColor.isEmpty()) { cStmt.bindText(8, clipColor); }
				else { cStmt.bindNull(8); }
				cStmt.bindText(9, jsonToString(clipExtras));
				cStmt.exec();
			}
			pcCount++;
		}

		patternTrackIdx++;
		trackElem = trackElem.nextSiblingElement("track");
	}

	logMsg("Pattern tracks: %d tracks, %d clips", ptCount, pcCount);
}

void convertAutomationTracks(SqliteDb& db, const QDomElement& parentElem,
	const QString& containerType = "song_tc")
{
	static const QSet<QString> knownTrackAttrs = {"type", "name", "muted", "solo", "color"};
	static const QSet<QString> knownAcAttrs = {"pos", "len", "prog", "tens", "mute", "muted", "name", "color"};

	int atCount = 0, acCount = 0, anCount = 0;

	auto trackElem = parentElem.firstChildElement("track");
	while (!trackElem.isNull())
	{
		QString trackType = trackElem.attribute("type", "");
		if (trackType != "5" && trackType != "6")
		{
			trackElem = trackElem.nextSiblingElement("track");
			continue;
		}

		QString name = trackElem.attribute("name", QString("Automation Track %1").arg(atCount));
		int muted = trackElem.attribute("muted", "0").toInt();
		int solo = trackElem.attribute("solo", "0").toInt();
		QString color = trackElem.attribute("color", "");
		QJsonObject trackExtras = extraAttrs(trackElem, knownTrackAttrs);

		int trackTypeInt = trackElem.attribute("type", "5").toInt();

		Stmt stmt(db.get(),
			"INSERT INTO automation_track (name, muted, solo, color, sort_order, track_type, container_type, extra_json)"
			" VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindText(1, name);
			stmt.bindInt(2, muted);
			stmt.bindInt(3, solo);
			if (!color.isEmpty()) { stmt.bindText(4, color); }
			else { stmt.bindNull(4); }
			stmt.bindInt(5, atCount);
			stmt.bindInt(6, trackTypeInt);
			stmt.bindText(7, containerType);
			stmt.bindText(8, jsonToString(trackExtras));
			stmt.exec();
		}
		sqlite3_int64 atId = db.lastInsertId();
		atCount++;

		// Automation clips
		auto clips = findClipElements(trackElem, "automationclip");
		for (const auto& clipElem : clips)
		{
			int clipPos = clipElem.attribute("pos", "0").toInt();
			int clipLen = clipElem.attribute("len", "0").toInt();
			int progression = clipElem.attribute("prog", "1").toInt();
			double tension = clipElem.attribute("tens", "1.0").toDouble();
			int clipMuted = clipElem.attribute("mute",
				clipElem.attribute("muted", "0")).toInt();
			QString clipName = clipElem.attribute("name", "");
			QString clipColor = clipElem.attribute("color", "");

			// Capture extra attrs and unknown children
			QJsonObject clipExtras = extraAttrs(clipElem, knownAcAttrs);
			static const QSet<QString> knownAcChildren = {"time", "object"};
			QJsonObject clipChildExtras = extraChildren(clipElem, knownAcChildren);
			if (!clipChildExtras.isEmpty())
			{
				clipExtras.insert("_children", clipChildExtras);
			}

			Stmt cStmt(db.get(),
				"INSERT INTO automation_clip "
				"(automation_track_id, position, length, progression_type, tension, muted, name, color, extra_json) "
				"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
			if (cStmt.valid())
			{
				cStmt.bindInt64(1, atId);
				cStmt.bindInt(2, clipPos);
				cStmt.bindInt(3, clipLen);
				cStmt.bindInt(4, progression);
				cStmt.bindDouble(5, tension);
				cStmt.bindInt(6, clipMuted);
				cStmt.bindText(7, clipName);
				if (!clipColor.isEmpty()) { cStmt.bindText(8, clipColor); }
				else { cStmt.bindNull(8); }
				cStmt.bindText(9, jsonToString(clipExtras));
				cStmt.exec();
			}
			sqlite3_int64 acId = db.lastInsertId();
			acCount++;

			// Time nodes
			auto timeElem = clipElem.firstChildElement("time");
			while (!timeElem.isNull())
			{
				int tPos = timeElem.attribute("pos", "0").toInt();
				double tValue = timeElem.attribute("value", "0").toDouble();
				double tOutValue = timeElem.attribute("outValue",
					timeElem.attribute("value", "0")).toDouble();
				double tInTan = timeElem.attribute("inTan", "0").toDouble();
				double tOutTan = timeElem.attribute("outTan", "0").toDouble();
				int tLocked = timeElem.attribute("lockedTan", "0").toInt();

				Stmt nStmt(db.get(),
					"INSERT INTO automation_node "
					"(automation_clip_id, position, in_value, out_value, in_tangent, out_tangent, locked_tangents) "
					"VALUES (?, ?, ?, ?, ?, ?, ?)");
				if (nStmt.valid())
				{
					nStmt.bindInt64(1, acId);
					nStmt.bindInt(2, tPos);
					nStmt.bindDouble(3, tValue);
					nStmt.bindDouble(4, tOutValue);
					nStmt.bindDouble(5, tInTan);
					nStmt.bindDouble(6, tOutTan);
					nStmt.bindInt(7, tLocked);
					nStmt.exec();
				}
				anCount++;
				timeElem = timeElem.nextSiblingElement("time");
			}

			// Automation targets
			auto objElem = clipElem.firstChildElement("object");
			while (!objElem.isNull())
			{
				int objId = objElem.attribute("id", "0").toInt();
				Stmt tStmt(db.get(),
					"INSERT INTO automation_target (automation_clip_id, target_object_id) VALUES (?, ?)");
				if (tStmt.valid())
				{
					tStmt.bindInt64(1, acId);
					tStmt.bindInt(2, objId);
					tStmt.exec();
				}
				objElem = objElem.nextSiblingElement("object");
			}
		}

		trackElem = trackElem.nextSiblingElement("track");
	}

	if (atCount > 0)
	{
		logMsg("Automation: %d tracks, %d clips, %d nodes", atCount, acCount, anCount);
	}
}

void convertSampleTracks(SqliteDb& db, const QDomElement& parentElem)
{
	static const QSet<QString> knownTrackAttrs = {"type", "name", "muted", "solo", "color"};
	static const QSet<QString> knownStAttrs = {"vol", "pan", "mixch", "fxch"};
	static const QSet<QString> knownScAttrs = {"pos", "len", "src", "muted", "name", "color"};

	int stCount = 0, scCount = 0;

	auto trackElem = parentElem.firstChildElement("track");
	while (!trackElem.isNull())
	{
		if (trackElem.attribute("type") != "2")
		{
			trackElem = trackElem.nextSiblingElement("track");
			continue;
		}

		QString name = trackElem.attribute("name", QString("Sample Track %1").arg(stCount));
		int muted = trackElem.attribute("muted", "0").toInt();
		int solo = trackElem.attribute("solo", "0").toInt();
		QString color = trackElem.attribute("color", "");
		QJsonObject trackExtras = extraAttrs(trackElem, knownTrackAttrs);

		auto stElem = trackElem.firstChildElement("sampletrack");
		double vol = stElem.isNull() ? 100.0 : stElem.attribute("vol", "100").toDouble();
		double pan = stElem.isNull() ? 0.0 : stElem.attribute("pan", "0").toDouble();
		QString mixch = stElem.isNull() ? "" : stElem.attribute("mixch", stElem.attribute("fxch", ""));

		// Capture extra sampletrack attrs and unknown children
		QJsonObject stExtras;
		if (!stElem.isNull())
		{
			stExtras = extraAttrs(stElem, knownStAttrs);
			static const QSet<QString> knownStChildren = {"fxchain"};
			QJsonObject stChildExtras = extraChildren(stElem, knownStChildren);
			if (!stChildExtras.isEmpty())
			{
				stExtras.insert("_children", stChildExtras);
			}
		}

		// Merge track-level extras with sampletrack extras
		QJsonObject allExtras = trackExtras;
		if (!stExtras.isEmpty())
		{
			allExtras.insert("_sampletrack", stExtras);
		}

		Stmt stmt(db.get(),
			"INSERT INTO sample_track (name, volume, panning, mixer_channel_id, muted, solo, color, sort_order, extra_json) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindText(1, name);
			stmt.bindDouble(2, vol);
			stmt.bindDouble(3, pan);
			if (!mixch.isEmpty()) { stmt.bindInt(4, mixch.toInt()); }
			else { stmt.bindNull(4); }
			stmt.bindInt(5, muted);
			stmt.bindInt(6, solo);
			if (!color.isEmpty()) { stmt.bindText(7, color); }
			else { stmt.bindNull(7); }
			stmt.bindInt(8, stCount);
			stmt.bindText(9, jsonToString(allExtras));
			stmt.exec();
		}
		sqlite3_int64 stId = db.lastInsertId();

		// Effects on the sample track
		if (!stElem.isNull())
		{
			auto fxchain = stElem.firstChildElement("fxchain");
			if (!fxchain.isNull())
			{
				insertEffectsFromChain(db, fxchain, "sample_track", stId);
			}
		}

		stCount++;

		// Sample clips
		auto clips = findClipElements(trackElem, "sampleclip");
		for (const auto& clipElem : clips)
		{
			int clipPos = clipElem.attribute("pos", "0").toInt();
			int clipLen = clipElem.attribute("len", "0").toInt();
			QString src = clipElem.attribute("src", "");
			int clipMuted = clipElem.attribute("muted", "0").toInt();
			QString clipName = clipElem.attribute("name", "");
			QString clipColor = clipElem.attribute("color", "");
			QJsonObject clipExtras = extraAttrs(clipElem, knownScAttrs);

			Stmt cStmt(db.get(),
				"INSERT INTO sample_clip "
				"(sample_track_id, position, length, source_path, muted, name, color, extra_json) "
				"VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
			if (cStmt.valid())
			{
				cStmt.bindInt64(1, stId);
				cStmt.bindInt(2, clipPos);
				cStmt.bindInt(3, clipLen);
				cStmt.bindText(4, src);
				cStmt.bindInt(5, clipMuted);
				cStmt.bindText(6, clipName);
				if (!clipColor.isEmpty()) { cStmt.bindText(7, clipColor); }
				else { cStmt.bindNull(7); }
				cStmt.bindText(8, jsonToString(clipExtras));
				cStmt.exec();
			}
			scCount++;
		}

		trackElem = trackElem.nextSiblingElement("track");
	}

	if (stCount > 0)
	{
		logMsg("Sample tracks: %d tracks, %d clips", stCount, scCount);
	}
}

void convertControllers(SqliteDb& db, const QDomElement& root)
{
	auto songElem = root.firstChildElement("song");
	if (songElem.isNull()) { return; }

	auto controllers = songElem.firstChildElement("controllers");
	if (controllers.isNull()) { return; }

	int count = 0;
	auto ctrlElem = controllers.firstChildElement("controller");
	while (!ctrlElem.isNull())
	{
		QJsonObject params = elemToJson(ctrlElem);
		QString type = ctrlElem.attribute("type", "0");
		QString name = ctrlElem.attribute("name", "");

		Stmt stmt(db.get(),
			"INSERT INTO controller (type, name, params_json) VALUES (?, ?, ?)");
		if (stmt.valid())
		{
			stmt.bindText(1, type);
			stmt.bindText(2, name);
			stmt.bindText(3, jsonToString(params));
			stmt.exec();
		}
		count++;
		ctrlElem = ctrlElem.nextSiblingElement("controller");
	}

	if (count > 0)
	{
		logMsg("Controllers: %d", count);
	}
}

// Store scales and keymaps
void convertScalesAndKeymaps(SqliteDb& db, const QDomElement& songElem)
{
	// Scales
	auto scalesElem = songElem.firstChildElement("scales");
	if (!scalesElem.isNull())
	{
		int idx = 0;
		auto scaleElem = scalesElem.firstChildElement("scale");
		while (!scaleElem.isNull())
		{
			QString desc = scaleElem.attribute("description", "");
			QJsonArray intervals;
			auto interval = scaleElem.firstChildElement("interval");
			while (!interval.isNull())
			{
				QJsonObject intObj;
				intObj.insert("num", interval.attribute("num", "1"));
				intObj.insert("den", interval.attribute("den", "1"));
				intervals.append(intObj);
				interval = interval.nextSiblingElement("interval");
			}

			Stmt stmt(db.get(),
				"INSERT INTO scale (description, intervals_json, sort_order) VALUES (?, ?, ?)");
			if (stmt.valid())
			{
				stmt.bindText(1, desc);
				stmt.bindText(2, jsonArrayToString(intervals));
				stmt.bindInt(3, idx);
				stmt.exec();
			}
			idx++;
			scaleElem = scaleElem.nextSiblingElement("scale");
		}
		if (idx > 0) { logMsg("Scales: %d", idx); }
	}

	// Keymaps
	auto keymapsElem = songElem.firstChildElement("keymaps");
	if (!keymapsElem.isNull())
	{
		static const QSet<QString> knownKmAttrs = {"description", "base_key", "base_freq", "first_key", "last_key", "middle_key"};
		int idx = 0;
		auto kmElem = keymapsElem.firstChildElement("keymap");
		while (!kmElem.isNull())
		{
			QString desc = kmElem.attribute("description", "");
			int baseKey = kmElem.attribute("base_key", "69").toInt();
			double baseFreq = kmElem.attribute("base_freq", "440.0").toDouble();
			int firstKey = kmElem.attribute("first_key", "0").toInt();
			int lastKey = kmElem.attribute("last_key", "127").toInt();
			int middleKey = kmElem.attribute("middle_key", "60").toInt();
			QJsonObject extras = extraAttrs(kmElem, knownKmAttrs);

			Stmt stmt(db.get(),
				"INSERT INTO keymap (description, base_key, base_freq, first_key, last_key, middle_key, sort_order, extra_json) "
				"VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
			if (stmt.valid())
			{
				stmt.bindText(1, desc);
				stmt.bindInt(2, baseKey);
				stmt.bindDouble(3, baseFreq);
				stmt.bindInt(4, firstKey);
				stmt.bindInt(5, lastKey);
				stmt.bindInt(6, middleKey);
				stmt.bindInt(7, idx);
				stmt.bindText(8, jsonToString(extras));
				stmt.exec();
			}
			idx++;
			kmElem = kmElem.nextSiblingElement("keymap");
		}
		if (idx > 0) { logMsg("Keymaps: %d", idx); }
	}
}

// Create all tables from embedded schema
bool createSchema(SqliteDb& db)
{
	const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS project (
    id INTEGER PRIMARY KEY DEFAULT 1,
    name TEXT,
    bpm REAL NOT NULL DEFAULT 140,
    timesig_numerator INTEGER NOT NULL DEFAULT 4,
    timesig_denominator INTEGER NOT NULL DEFAULT 4,
    master_volume REAL NOT NULL DEFAULT 100,
    master_pitch REAL NOT NULL DEFAULT 0,
    lmms_version TEXT NOT NULL DEFAULT '30',
    project_type TEXT NOT NULL DEFAULT 'song',
    creator TEXT NOT NULL DEFAULT 'LMMS',
    creator_version TEXT NOT NULL DEFAULT '1.3.0-alpha',
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    modified_at TEXT NOT NULL DEFAULT (datetime('now')),
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS song_ui_element (
    id INTEGER PRIMARY KEY,
    element_name TEXT NOT NULL,
    attributes_json TEXT NOT NULL DEFAULT '{}',
    children_json TEXT NOT NULL DEFAULT '{}',
    content_text TEXT
);
CREATE TABLE IF NOT EXISTS trackcontainer_state (
    id INTEGER PRIMARY KEY,
    container_type TEXT NOT NULL UNIQUE,
    visible INTEGER NOT NULL DEFAULT 1,
    minimized INTEGER NOT NULL DEFAULT 0,
    maximized INTEGER NOT NULL DEFAULT 0,
    x INTEGER NOT NULL DEFAULT 0,
    y INTEGER NOT NULL DEFAULT 0,
    width INTEGER NOT NULL DEFAULT 1600,
    height INTEGER NOT NULL DEFAULT 900,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS mixer_state (
    id INTEGER PRIMARY KEY DEFAULT 1,
    visible INTEGER NOT NULL DEFAULT 0,
    minimized INTEGER NOT NULL DEFAULT 0,
    maximized INTEGER NOT NULL DEFAULT 0,
    x INTEGER NOT NULL DEFAULT 0,
    y INTEGER NOT NULL DEFAULT 0,
    width INTEGER NOT NULL DEFAULT 865,
    height INTEGER NOT NULL DEFAULT 278,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS mixer_channel (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    volume REAL NOT NULL DEFAULT 1.0,
    muted INTEGER NOT NULL DEFAULT 0,
    soloed INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS mixer_route (
    id INTEGER PRIMARY KEY,
    from_channel_id INTEGER NOT NULL REFERENCES mixer_channel(id),
    to_channel_id INTEGER NOT NULL REFERENCES mixer_channel(id),
    amount REAL NOT NULL DEFAULT 1.0
);
CREATE TABLE IF NOT EXISTS effect (
    id INTEGER PRIMARY KEY,
    owner_type TEXT NOT NULL,
    owner_id INTEGER NOT NULL,
    plugin_name TEXT NOT NULL,
    sort_order INTEGER NOT NULL DEFAULT 0,
    enabled INTEGER NOT NULL DEFAULT 1,
    wet REAL NOT NULL DEFAULT 1.0,
    gate REAL NOT NULL DEFAULT 0.0,
    fxchain_enabled INTEGER NOT NULL DEFAULT 1,
    params_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS instrument_track (
    id INTEGER PRIMARY KEY,
    container_type TEXT NOT NULL DEFAULT 'patternstore',
    name TEXT NOT NULL,
    volume REAL NOT NULL DEFAULT 100,
    panning REAL NOT NULL DEFAULT 0,
    pitch REAL NOT NULL DEFAULT 0,
    pitch_range INTEGER NOT NULL DEFAULT 1,
    mixer_channel_id INTEGER REFERENCES mixer_channel(id),
    base_note INTEGER NOT NULL DEFAULT 69,
    use_master_pitch INTEGER NOT NULL DEFAULT 1,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    instrument_plugin TEXT NOT NULL,
    instrument_params_json TEXT NOT NULL DEFAULT '{}',
    sound_shaping_json TEXT NOT NULL DEFAULT '{}',
    arpeggio_json TEXT NOT NULL DEFAULT '{}',
    chord_creator_json TEXT NOT NULL DEFAULT '{}',
    midi_port_json TEXT NOT NULL DEFAULT '{}',
    microtuner_json TEXT NOT NULL DEFAULT '{}',
    track_extra_json TEXT NOT NULL DEFAULT '{}',
    instrumenttrack_extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS pattern_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS pattern_clip (
    id INTEGER PRIMARY KEY,
    pattern_track_id INTEGER NOT NULL REFERENCES pattern_track(id),
    pattern_id INTEGER NOT NULL REFERENCES pattern(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    start_offset INTEGER NOT NULL DEFAULT 0,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS pattern (
    id INTEGER PRIMARY KEY,
    name TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS midi_clip (
    id INTEGER PRIMARY KEY,
    instrument_track_id INTEGER NOT NULL REFERENCES instrument_track(id),
    pattern_id INTEGER REFERENCES pattern(id),
    position INTEGER NOT NULL DEFAULT 0,
    length INTEGER NOT NULL DEFAULT 0,
    clip_type INTEGER NOT NULL DEFAULT 1,
    steps INTEGER NOT NULL DEFAULT 32,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS note (
    id INTEGER PRIMARY KEY,
    midi_clip_id INTEGER NOT NULL REFERENCES midi_clip(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    key INTEGER NOT NULL,
    volume INTEGER NOT NULL DEFAULT 100,
    panning INTEGER NOT NULL DEFAULT 0,
    note_type INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_note_clip_pos ON note(midi_clip_id, position);
CREATE TABLE IF NOT EXISTS note_detuning (
    id INTEGER PRIMARY KEY,
    note_id INTEGER NOT NULL REFERENCES note(id),
    position INTEGER NOT NULL,
    value REAL NOT NULL,
    out_value REAL,
    in_tangent REAL NOT NULL DEFAULT 0,
    out_tangent REAL NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS sample_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    volume REAL NOT NULL DEFAULT 100,
    panning REAL NOT NULL DEFAULT 0,
    mixer_channel_id INTEGER REFERENCES mixer_channel(id),
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS sample_clip (
    id INTEGER PRIMARY KEY,
    sample_track_id INTEGER NOT NULL REFERENCES sample_track(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    source_path TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS automation_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    track_type INTEGER NOT NULL DEFAULT 5,
    container_type TEXT NOT NULL DEFAULT 'song_tc',
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS automation_clip (
    id INTEGER PRIMARY KEY,
    automation_track_id INTEGER NOT NULL REFERENCES automation_track(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    progression_type INTEGER NOT NULL DEFAULT 1,
    tension REAL NOT NULL DEFAULT 1.0,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS automation_node (
    id INTEGER PRIMARY KEY,
    automation_clip_id INTEGER NOT NULL REFERENCES automation_clip(id),
    position INTEGER NOT NULL,
    in_value REAL NOT NULL,
    out_value REAL NOT NULL,
    in_tangent REAL NOT NULL DEFAULT 0,
    out_tangent REAL NOT NULL DEFAULT 0,
    locked_tangents INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_auto_node_clip_pos ON automation_node(automation_clip_id, position);
CREATE TABLE IF NOT EXISTS automation_target (
    id INTEGER PRIMARY KEY,
    automation_clip_id INTEGER NOT NULL REFERENCES automation_clip(id),
    target_object_id INTEGER NOT NULL,
    target_description TEXT
);
CREATE TABLE IF NOT EXISTS controller (
    id INTEGER PRIMARY KEY,
    type TEXT NOT NULL,
    name TEXT NOT NULL DEFAULT '',
    params_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS controller_connection (
    id INTEGER PRIMARY KEY,
    owner_type TEXT NOT NULL,
    owner_id INTEGER NOT NULL,
    param_name TEXT NOT NULL,
    controller_id INTEGER REFERENCES controller(id),
    connection_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS scale (
    id INTEGER PRIMARY KEY,
    description TEXT NOT NULL DEFAULT '',
    intervals_json TEXT NOT NULL DEFAULT '[]',
    sort_order INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS keymap (
    id INTEGER PRIMARY KEY,
    description TEXT NOT NULL DEFAULT '',
    base_key INTEGER NOT NULL DEFAULT 69,
    base_freq REAL NOT NULL DEFAULT 440.0,
    first_key INTEGER NOT NULL DEFAULT 0,
    last_key INTEGER NOT NULL DEFAULT 127,
    middle_key INTEGER NOT NULL DEFAULT 60,
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
)SQL";

	return db.exec(schema);
}

} // anonymous namespace


bool XmlToSqlite::convert(const QDomDocument& doc, const QString& dbPath)
{
	logMsg("Converting to %s", dbPath.toUtf8().constData());

	// Remove existing database file if present
	if (QFile::exists(dbPath))
	{
		QFile::remove(dbPath);
	}

	SqliteDb db;
	if (!db.open(dbPath))
	{
		return false;
	}

	if (!createSchema(db))
	{
		logMsg("ERROR: Failed to create schema");
		return false;
	}

	db.exec("BEGIN TRANSACTION");

	QDomElement root = doc.documentElement();

	// Project metadata (includes root element attributes)
	convertProjectMetadata(db, root);

	// Mixer (including window state)
	convertMixer(db, root);

	// Find the patternstore
	QDomElement patternstore;
	auto songElem = root.firstChildElement("song");
	if (!songElem.isNull())
	{
		// Store song trackcontainer state
		auto songTc = songElem.firstChildElement("trackcontainer");
		if (!songTc.isNull())
		{
			convertTrackcontainerState(db, songTc, "song");
		}

		// Search for patternstore inside pattern tracks
		auto track = songTc.firstChildElement("track");
		while (!track.isNull())
		{
			if (track.attribute("type") == "1")
			{
				auto ptSettings = track.firstChildElement("patterntrack");
				if (!ptSettings.isNull())
				{
					auto tc = ptSettings.firstChildElement("trackcontainer");
					if (!tc.isNull() && (tc.attribute("type") == "patternstore"
						|| tc.attribute("type") == "bbtrackcontainer"))
					{
						patternstore = tc;
						break;
					}
				}
			}
			track = track.nextSiblingElement("track");
		}

		// Song UI elements (pianoroll, automationeditor, projectnotes, timeline, etc.)
		convertSongUiElements(db, songElem);

		// Scales and keymaps
		convertScalesAndKeymaps(db, songElem);
	}

	QMap<int, sqlite3_int64> patternIdMap;

	// Patternstore
	if (!patternstore.isNull())
	{
		convertPatternstore(db, patternstore, patternIdMap);
	}
	else
	{
		logMsg("WARNING: No patternstore found");
	}

	// Song trackcontainer for pattern/automation/sample/instrument tracks
	auto songTc = songElem.firstChildElement("trackcontainer");
	if (!songTc.isNull())
	{
		convertSongInstrumentTracks(db, songTc);
		convertPatternTracks(db, songTc, patternIdMap);
		convertAutomationTracks(db, songTc, "song_tc");
		convertSampleTracks(db, songTc);
	}

	// Also check for automation/sample tracks directly in <song>
	if (!songElem.isNull())
	{
		convertAutomationTracks(db, songElem, "song");
		convertSampleTracks(db, songElem);
	}

	// Controllers
	convertControllers(db, root);

	db.exec("COMMIT");

	logMsg("Done! Saved to %s", dbPath.toUtf8().constData());
	return true;
}

} // namespace lmms
