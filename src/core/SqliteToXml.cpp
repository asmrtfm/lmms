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

// Check if a table exists in the database (for backwards compatibility with v2 schemas)
bool tableExists(sqlite3* db, const char* tableName)
{
	QString sql = QString("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%1'").arg(tableName);
	SqliteStmt stmt(db, sql.toUtf8().constData());
	if (stmt.valid() && stmt.step())
	{
		return stmt.colInt(0) > 0;
	}
	return false;
}

// Check if a column exists in a table (for backwards compatibility)
bool columnExists(sqlite3* db, const char* tableName, const char* columnName)
{
	QString sql = QString("PRAGMA table_info(%1)").arg(tableName);
	SqliteStmt stmt(db, sql.toUtf8().constData());
	while (stmt.valid() && stmt.step())
	{
		if (stmt.colText(1) == columnName) { return true; }
	}
	return false;
}

// Forward declaration for mutual recursion
void jsonToXml(QDomDocument& doc, QDomElement& parent, const QJsonObject& obj);

// Emit a single JSON key-value pair as an XML element, attribute, or text node
void jsonKeyToXml(QDomDocument& doc, QDomElement& parent, const QString& key, const QJsonValue& val)
{
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

// Convert a JSON object (parsed from a text column) into XML child elements/attributes
// Uses _order array (if present) to preserve original XML element ordering
void jsonToXml(QDomDocument& doc, QDomElement& parent, const QJsonObject& obj)
{
	// Reserved keys that are not emitted as elements/attributes
	static const QSet<QString> reservedKeys = {"_order", "_text", "journallingObject"};

	// First pass: emit attributes and text content (non-element keys)
	for (auto it = obj.begin(); it != obj.end(); ++it)
	{
		const QString& key = it.key();
		const QJsonValue& val = it.value();

		if (reservedKeys.contains(key)) { continue; }

		// Only emit primitive values (attributes) in first pass
		if (!val.isObject() && !val.isArray())
		{
			jsonKeyToXml(doc, parent, key, val);
		}
	}

	// Restore text content
	if (obj.contains("_text"))
	{
		parent.appendChild(doc.createTextNode(obj.value("_text").toString()));
	}

	// Second pass: emit child elements in preserved order (or alphabetical fallback)
	QJsonArray order = obj.value("_order").toArray();
	if (!order.isEmpty())
	{
		// _order contains every child occurrence in sequence (tags may repeat)
		// For array-valued keys, consume elements one at a time in order
		QMap<QString, int> arrayIndex; // tracks next index to consume per array key
		QSet<QString> emittedSingles; // tracks non-array keys already emitted

		for (const auto& item : order)
		{
			QString key = item.toString();
			if (reservedKeys.contains(key) || !obj.contains(key)) { continue; }

			const QJsonValue& val = obj.value(key);
			if (val.isArray())
			{
				// Emit next element from the array
				QJsonArray arr = val.toArray();
				int idx = arrayIndex.value(key, 0);
				if (idx < arr.size())
				{
					QDomElement child = doc.createElement(key);
					const QJsonValue& arrItem = arr.at(idx);
					if (arrItem.isObject())
					{
						jsonToXml(doc, child, arrItem.toObject());
					}
					else
					{
						child.appendChild(doc.createTextNode(
							arrItem.isDouble()
								? QString::number(arrItem.toDouble(), 'g', 15)
								: arrItem.toString()));
					}
					parent.appendChild(child);
					arrayIndex[key] = idx + 1;
				}
			}
			else if (val.isObject())
			{
				if (!emittedSingles.contains(key))
				{
					jsonKeyToXml(doc, parent, key, val);
					emittedSingles.insert(key);
				}
			}
		}

		// Emit any remaining child elements not covered by _order (safety fallback)
		QSet<QString> allOrdered;
		for (const auto& item : order) { allOrdered.insert(item.toString()); }
		for (auto it = obj.begin(); it != obj.end(); ++it)
		{
			const QString& key = it.key();
			if (reservedKeys.contains(key) || allOrdered.contains(key)) { continue; }
			if (it.value().isObject() || it.value().isArray())
			{
				jsonKeyToXml(doc, parent, key, it.value());
			}
		}
	}
	else
	{
		// No order info - emit child elements in default (alphabetical) order
		for (auto it = obj.begin(); it != obj.end(); ++it)
		{
			const QString& key = it.key();
			if (reservedKeys.contains(key)) { continue; }
			if (it.value().isObject() || it.value().isArray())
			{
				jsonKeyToXml(doc, parent, key, it.value());
			}
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

// Apply extra_json: simple values as attributes, _children as child elements
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

	// Apply remaining keys
	jsonToXml(doc, elem, obj);

	// Apply _children as child elements
	if (hasChildren)
	{
		jsonToXml(doc, elem, children);
	}
}

// Apply extra_json but only simple (non-dict, non-array) values as attributes
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
	// Check if fxchain_enabled column exists (v3 schema)
	bool hasFxchainEnabled = columnExists(db, "effect", "fxchain_enabled");

	QString sql = hasFxchainEnabled
		? "SELECT * FROM effect WHERE owner_type = ? AND owner_id = ? ORDER BY sort_order"
		: "SELECT * FROM effect WHERE owner_type = ? AND owner_id = ? ORDER BY sort_order";

	SqliteStmt stmt(db, sql.toUtf8().constData());
	if (!stmt.valid()) { return; }

	stmt.bindText(1, ownerType);
	stmt.bindInt(2, ownerId);

	QDomElement fxchain = doc.createElement("fxchain");
	int count = 0;
	int fxchainEnabled = 1;  // default

	while (stmt.step())
	{
		QString pluginName = stmt.colText(3);  // plugin_name

		// Read fxchain_enabled from first row (column 8 in v3 schema)
		if (count == 0 && hasFxchainEnabled)
		{
			fxchainEnabled = stmt.colInt(8);
		}

		// Skip sentinel rows used to store fxchain metadata
		if (pluginName == "_fxchain_metadata")
		{
			continue;
		}

		QDomElement fxElem = doc.createElement("effect");
		fxElem.setAttribute("name", pluginName);
		fxElem.setAttribute("on", stmt.colInt(5));      // enabled
		fxElem.setAttribute("wet", QString::number(stmt.colDouble(6), 'g', 15)); // wet
		fxElem.setAttribute("gate", QString::number(stmt.colDouble(7), 'g', 15)); // gate

		// Plugin params from JSON
		int paramsIdx = hasFxchainEnabled ? 9 : 8;
		QString paramsJson = stmt.colText(paramsIdx);
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
	fxchain.setAttribute("enabled", fxchainEnabled);
	parent.appendChild(fxchain);
}

// Export project metadata into <head> and set root attributes
void exportProjectMetadata(sqlite3* db, QDomDocument& doc, QDomElement& root, QDomElement& head)
{
	// Check if v3 columns exist
	bool hasVersionCols = columnExists(db, "project", "lmms_version");

	SqliteStmt stmt(db, "SELECT * FROM project WHERE id = 1");
	if (!stmt.valid() || !stmt.step())
	{
		logMsg("WARNING: No project metadata found");
		return;
	}

	// Column indices: 0=id, 1=name, 2=bpm, 3=timesig_num, 4=timesig_denom,
	//                 5=master_volume, 6=master_pitch
	head.setAttribute("bpm", QString::number(stmt.colDouble(2), 'g', 15));
	head.setAttribute("timesig_numerator", stmt.colInt(3));
	head.setAttribute("timesig_denominator", stmt.colInt(4));
	head.setAttribute("mastervol", QString::number(stmt.colDouble(5), 'g', 15));
	head.setAttribute("masterpitch", QString::number(stmt.colDouble(6), 'g', 15));

	// Root element attributes from v3 columns
	if (hasVersionCols)
	{
		// 7=lmms_version, 8=project_type, 9=creator, 10=creator_version
		root.setAttribute("version", stmt.colText(7));
		root.setAttribute("type", stmt.colText(8));
		root.setAttribute("creator", stmt.colText(9));
		root.setAttribute("creatorversion", stmt.colText(10));
		// 11=created_at, 12=modified_at, 13=extra_json
		applyExtraJson(doc, head, stmt.colText(13));
	}
	else
	{
		// v2 schema: extra_json at column 9
		applyExtraJson(doc, head, stmt.colText(9));
	}

	logMsg("Project: bpm=%g, time_sig=%d/%d",
		stmt.colDouble(2), stmt.colInt(3), stmt.colInt(4));
}

// Export song UI elements (pianoroll, automationeditor, projectnotes, timeline, etc.)
void exportSongUiElements(sqlite3* db, QDomDocument& doc, QDomElement& songElem)
{
	if (!tableExists(db, "song_ui_element")) { return; }

	SqliteStmt stmt(db, "SELECT * FROM song_ui_element ORDER BY id");
	if (!stmt.valid()) { return; }

	while (stmt.step())
	{
		// 0=id, 1=element_name, 2=attributes_json, 3=children_json, 4=content_text
		QString elemName = stmt.colText(1);
		QDomElement elem = doc.createElement(elemName);

		// Apply attributes
		applyJsonColumn(doc, elem, stmt.colText(2));
		// Apply children
		applyJsonColumn(doc, elem, stmt.colText(3));

		// Text/CDATA content (e.g. project notes)
		if (!stmt.colIsNull(4))
		{
			QString contentText = stmt.colText(4);
			if (!contentText.isEmpty())
			{
				elem.appendChild(doc.createCDATASection(contentText));
			}
		}

		songElem.appendChild(elem);
	}
}

// Get trackcontainer state from database, applying to element
void applyTrackcontainerState(sqlite3* db, QDomElement& tcElem, const QString& containerType)
{
	if (!tableExists(db, "trackcontainer_state")) { return; }

	SqliteStmt stmt(db, "SELECT * FROM trackcontainer_state WHERE container_type = ?");
	if (!stmt.valid()) { return; }
	stmt.bindText(1, containerType.toUtf8().constData());

	if (stmt.step())
	{
		// 0=id, 1=container_type, 2=visible, 3=minimized, 4=maximized, 5=x, 6=y, 7=width, 8=height, 9=extra_json
		tcElem.setAttribute("visible", stmt.colInt(2));
		tcElem.setAttribute("minimized", stmt.colInt(3));
		tcElem.setAttribute("maximized", stmt.colInt(4));
		tcElem.setAttribute("x", stmt.colInt(5));
		tcElem.setAttribute("y", stmt.colInt(6));
		tcElem.setAttribute("width", stmt.colInt(7));
		tcElem.setAttribute("height", stmt.colInt(8));
		applyExtraJsonAttrsOnly(tcElem, stmt.colText(9));
	}
	else
	{
		// Defaults if no state stored
		tcElem.setAttribute("visible", 1);
		tcElem.setAttribute("minimized", 0);
		tcElem.setAttribute("maximized", containerType == "patternstore" ? 1 : 0);
		tcElem.setAttribute("x", 0);
		tcElem.setAttribute("y", 0);
		tcElem.setAttribute("width", containerType == "patternstore" ? 1527 : 1600);
		tcElem.setAttribute("height", containerType == "patternstore" ? 768 : 900);
	}
}

// Export mixer channels and routing
void exportMixer(sqlite3* db, QDomDocument& doc, QDomElement& songElem)
{
	SqliteStmt chStmt(db, "SELECT * FROM mixer_channel ORDER BY sort_order");
	if (!chStmt.valid()) { return; }

	QDomElement mixerElem = doc.createElement("mixer");

	// Restore mixer window state
	if (tableExists(db, "mixer_state"))
	{
		SqliteStmt msStmt(db, "SELECT * FROM mixer_state WHERE id = 1");
		if (msStmt.valid() && msStmt.step())
		{
			// 0=id, 1=visible, 2=minimized, 3=maximized, 4=x, 5=y, 6=width, 7=height, 8=extra_json
			mixerElem.setAttribute("visible", msStmt.colInt(1));
			mixerElem.setAttribute("minimized", msStmt.colInt(2));
			mixerElem.setAttribute("maximized", msStmt.colInt(3));
			mixerElem.setAttribute("x", msStmt.colInt(4));
			mixerElem.setAttribute("y", msStmt.colInt(5));
			mixerElem.setAttribute("width", msStmt.colInt(6));
			mixerElem.setAttribute("height", msStmt.colInt(7));
			applyExtraJsonAttrsOnly(mixerElem, msStmt.colText(8));
		}
	}

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

		// Restore extra mixer channel attributes and children
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

		// Controller connections on this mixer channel
		if (tableExists(db, "controller_connection"))
		{
			SqliteStmt ccStmt(db,
				"SELECT * FROM controller_connection WHERE owner_type = 'mixer_channel' AND owner_id = ?");
			if (ccStmt.valid())
			{
				ccStmt.bindInt(1, chId);
				// Group connections by param_name under a <connection> element
				QDomElement connElem;
				bool hasConn = false;
				while (ccStmt.step())
				{
					if (!hasConn)
					{
						connElem = doc.createElement("connection");
						hasConn = true;
					}
					// 0=id, 1=owner_type, 2=owner_id, 3=param_name, 4=controller_id, 5=connection_json
					QString paramName = ccStmt.colText(3);
					QDomElement paramElem = doc.createElement(paramName);
					// Restore controller connection from JSON
					QString connJson = ccStmt.colText(5);
					if (!connJson.isEmpty() && connJson != "{}")
					{
						QDomElement ctrlConnElem = doc.createElement("Midicontroller");
						applyJsonColumn(doc, ctrlConnElem, connJson);
						paramElem.appendChild(ctrlConnElem);
					}
					connElem.appendChild(paramElem);
				}
				if (hasConn)
				{
					chElem.appendChild(connElem);
				}
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
	// Column indices depend on whether container_type column exists (v3 vs v2 schema)
	// v3: 0=id, 1=container_type, 2=name, 3=volume, ... (all shifted +1 from v2)
	// v2: 0=id, 1=name, 2=volume, ...
	bool hasContainerType = columnExists(db, "instrument_track", "container_type");
	int off = hasContainerType ? 1 : 0;  // column offset for v3 schema

	int itId = itStmt.colInt(0);

	QDomElement trackElem = doc.createElement("track");
	trackElem.setAttribute("type", 0);
	trackElem.setAttribute("name", itStmt.colText(1 + off));
	trackElem.setAttribute("muted", itStmt.colInt(9 + off));
	trackElem.setAttribute("solo", itStmt.colInt(10 + off));
	if (!itStmt.colIsNull(11 + off))
	{
		trackElem.setAttribute("color", itStmt.colText(11 + off));
	}

	// Restore extra <track> attributes from track_extra_json
	applyExtraJsonAttrsOnly(trackElem, itStmt.colText(20 + off));

	// <instrumenttrack> settings element
	QDomElement itElem = doc.createElement("instrumenttrack");
	itElem.setAttribute("vol", QString::number(itStmt.colDouble(2 + off), 'g', 15));
	itElem.setAttribute("pan", QString::number(itStmt.colDouble(3 + off), 'g', 15));
	itElem.setAttribute("pitch", QString::number(itStmt.colDouble(4 + off), 'g', 15));
	itElem.setAttribute("pitchrange", itStmt.colInt(5 + off));
	if (!itStmt.colIsNull(6 + off))
	{
		itElem.setAttribute("mixch", itStmt.colInt(6 + off));
	}
	itElem.setAttribute("basenote", itStmt.colInt(7 + off));
	itElem.setAttribute("usemasterpitch", itStmt.colInt(8 + off));

	// Parse instrumenttrack_extra_json -- apply attrs now, children later
	QString itExtrasJson = itStmt.colText(21 + off);
	QJsonObject itExtrasChildren;
	bool hasItExtrasChildren = false;
	if (!itExtrasJson.isEmpty() && itExtrasJson != "{}")
	{
		QJsonParseError parseErr;
		QJsonDocument jdoc = QJsonDocument::fromJson(itExtrasJson.toUtf8(), &parseErr);
		if (parseErr.error == QJsonParseError::NoError && jdoc.isObject())
		{
			QJsonObject obj = jdoc.object();
			if (obj.contains("_children"))
			{
				QJsonValue childrenVal = obj.take("_children");
				if (childrenVal.isObject())
				{
					itExtrasChildren = childrenVal.toObject();
					hasItExtrasChildren = true;
				}
			}
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

	// Instrument plugin
	QString pluginName = itStmt.colText(13 + off);
	QDomElement instrumentElem = doc.createElement("instrument");
	instrumentElem.setAttribute("name", pluginName);
	QString pluginParamsJson = itStmt.colText(14 + off);
	if (!pluginParamsJson.isEmpty() && pluginParamsJson != "{}")
	{
		QJsonParseError parseErr;
		QJsonDocument jdoc = QJsonDocument::fromJson(pluginParamsJson.toUtf8(), &parseErr);
		if (parseErr.error == QJsonParseError::NoError && jdoc.isObject())
		{
			QJsonObject obj = jdoc.object();
			QJsonValue keyVal;
			if (obj.contains("_key"))
			{
				keyVal = obj.take("_key");
			}
			QDomElement pluginChild = doc.createElement(pluginName);
			jsonToXml(doc, pluginChild, obj);
			instrumentElem.appendChild(pluginChild);
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
	QString soundShapingJson = itStmt.colText(15 + off);
	if (!soundShapingJson.isEmpty() && soundShapingJson != "{}")
	{
		QDomElement eldataElem = doc.createElement("eldata");
		applyJsonColumn(doc, eldataElem, soundShapingJson);
		itElem.appendChild(eldataElem);
	}

	// Chord creator
	QString chordJson = itStmt.colText(17 + off);
	if (!chordJson.isEmpty() && chordJson != "{}")
	{
		QDomElement chordElem = doc.createElement("chordcreator");
		applyJsonColumn(doc, chordElem, chordJson);
		itElem.appendChild(chordElem);
	}

	// Arpeggiator
	QString arpJson = itStmt.colText(16 + off);
	if (!arpJson.isEmpty() && arpJson != "{}")
	{
		QDomElement arpElem = doc.createElement("arpeggiator");
		applyJsonColumn(doc, arpElem, arpJson);
		itElem.appendChild(arpElem);
	}

	// MIDI port
	QString midiJson = itStmt.colText(18 + off);
	if (!midiJson.isEmpty() && midiJson != "{}")
	{
		QDomElement midiElem = doc.createElement("midiport");
		applyJsonColumn(doc, midiElem, midiJson);
		itElem.appendChild(midiElem);
	}

	// Microtuner
	QString microtunerJson = itStmt.colText(19 + off);
	if (!microtunerJson.isEmpty() && microtunerJson != "{}")
	{
		QDomElement microtunerElem = doc.createElement("microtuner");
		applyJsonColumn(doc, microtunerElem, microtunerJson);
		itElem.appendChild(microtunerElem);
	}

	// Apply instrumenttrack extra _children after all known children
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
	// Only export patternstore instrument tracks (container_type check with fallback for v2 schema)
	bool hasContainerType = columnExists(db, "instrument_track", "container_type");
	QString itSql = hasContainerType
		? "SELECT * FROM instrument_track WHERE container_type = 'patternstore' ORDER BY sort_order"
		: "SELECT * FROM instrument_track ORDER BY sort_order";
	SqliteStmt itStmt(db, itSql.toUtf8().constData());
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

		// midi_clip columns: 0=id, 1=instrument_track_id, 2=pattern_id,
		// 3=position, 4=length, 5=clip_type, 6=steps, 7=muted, 8=name, 9=color, 10=extra_json
		// (joined column: 11=pattern_sort_order)
		bool mcHasPosition = columnExists(db, "midi_clip", "position");
		int mcTypeIdx = mcHasPosition ? 5 : 3;
		int mcStepsIdx = mcHasPosition ? 6 : 4;
		int mcMutedIdx = mcHasPosition ? 7 : 5;
		int mcNameIdx = mcHasPosition ? 8 : 6;
		int mcColorIdx = mcHasPosition ? 9 : 7;
		int mcExtraIdx = mcHasPosition ? 10 : 8;

		while (mcStmt.step())
		{
			int patternId = mcStmt.colInt(2); // pattern_id
			int patternIdx = patternIdToIndex.value(patternId, 0);
			int pos = patternIdx * DEFAULT_PATTERN_LENGTH;

			QDomElement mcElem = doc.createElement("midiclip");
			mcElem.setAttribute("type", mcStmt.colInt(mcTypeIdx));
			mcElem.setAttribute("name", mcStmt.colText(mcNameIdx));
			mcElem.setAttribute("pos", pos);
			mcElem.setAttribute("muted", mcStmt.colInt(mcMutedIdx));
			mcElem.setAttribute("steps", mcStmt.colInt(mcStepsIdx));
			if (!mcStmt.colIsNull(mcColorIdx))
			{
				mcElem.setAttribute("color", mcStmt.colText(mcColorIdx));
			}

			// Restore extra midi clip attributes
			applyExtraJson(doc, mcElem, mcStmt.colText(mcExtraIdx));

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

					// Restore extra note attributes
					applyExtraJson(doc, noteElem, noteStmt.colText(8));

					// Note detuning
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
		if (!ptStmt.colIsNull(4))
		{
			trackElem.setAttribute("color", ptStmt.colText(4));
		}

		// Restore extra pattern track attributes
		applyExtraJson(doc, trackElem, ptStmt.colText(6));

		// Ensure mutedBeforeSolo exists (LMMS expects it)
		if (!trackElem.hasAttribute("mutedBeforeSolo"))
		{
			trackElem.setAttribute("mutedBeforeSolo", 0);
		}

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

				// Restore extra pattern clip attributes
				applyExtraJson(doc, clipElem, clipStmt.colText(9));

				trackElem.appendChild(clipElem);
			}
		}

		songTc.appendChild(trackElem);
		trackCount++;
	}

	logMsg("Pattern tracks: %d tracks", trackCount);
}

// Export automation tracks filtered by container type
void exportAutomationTracks(sqlite3* db, QDomDocument& doc, QDomElement& parentElem,
	const QString& containerType = "song_tc")
{
	// Use container_type column if it exists, otherwise export all to song_tc only
	bool hasContainerType = columnExists(db, "automation_track", "container_type");
	if (!hasContainerType && containerType != "song_tc")
	{
		// Old DBs without container_type - only export for song_tc caller
		return;
	}
	QString sql = hasContainerType
		? "SELECT * FROM automation_track WHERE container_type = ? ORDER BY sort_order"
		: "SELECT * FROM automation_track ORDER BY sort_order";
	SqliteStmt atStmt(db, sql.toUtf8().constData());
	if (hasContainerType)
	{
		atStmt.bindText(1, containerType.toUtf8().constData());
	}
	if (!atStmt.valid()) { return; }

	// Column indices depend on schema version
	// New schema: id(0), name(1), muted(2), solo(3), color(4), sort_order(5),
	//             track_type(6), container_type(7), extra_json(8)
	// Old schema: id(0), name(1), muted(2), solo(3), color(4), sort_order(5), extra_json(6)
	bool hasTrackType = columnExists(db, "automation_track", "track_type");
	int extraJsonCol = hasContainerType ? (hasTrackType ? 8 : 7) : 6;
	int trackTypeCol = hasTrackType ? 6 : -1;

	int trackCount = 0;
	while (atStmt.step())
	{
		int atId = atStmt.colInt(0);

		QDomElement trackElem = doc.createElement("track");
		// Restore original track type (5=Automation, 6=HiddenAutomation)
		int trackType = (trackTypeCol >= 0) ? atStmt.colInt(trackTypeCol) : 5;
		trackElem.setAttribute("type", trackType);
		trackElem.setAttribute("name", atStmt.colText(1));
		trackElem.setAttribute("muted", atStmt.colInt(2));
		trackElem.setAttribute("solo", atStmt.colInt(3));
		if (!atStmt.colIsNull(4))
		{
			trackElem.setAttribute("color", atStmt.colText(4));
		}

		// Restore extra automation track attributes
		applyExtraJson(doc, trackElem, atStmt.colText(extraJsonCol));

		if (!trackElem.hasAttribute("mutedBeforeSolo"))
		{
			trackElem.setAttribute("mutedBeforeSolo", 0);
		}

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

				// Restore extra automation clip attributes
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

				// Automation targets
				SqliteStmt targetStmt(db,
					"SELECT * FROM automation_target WHERE automation_clip_id = ?");
				if (targetStmt.valid())
				{
					targetStmt.bindInt(1, clipId);
					while (targetStmt.step())
					{
						QDomElement objElem = doc.createElement("object");
						objElem.setAttribute("id", targetStmt.colInt(2));
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
		if (!stStmt.colIsNull(7))
		{
			trackElem.setAttribute("color", stStmt.colText(7));
		}

		// Restore extra sample track attributes
		applyExtraJson(doc, trackElem, stStmt.colText(9));

		if (!trackElem.hasAttribute("mutedBeforeSolo"))
		{
			trackElem.setAttribute("mutedBeforeSolo", 0);
		}

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

				// Restore extra sample clip attributes
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

// Export song-level instrument tracks (type=0 tracks in song trackcontainer)
void exportSongInstrumentTracks(sqlite3* db, QDomDocument& doc, QDomElement& songTc)
{
	if (!columnExists(db, "instrument_track", "container_type")) { return; }

	SqliteStmt itStmt(db,
		"SELECT * FROM instrument_track WHERE container_type = 'song' ORDER BY sort_order");
	if (!itStmt.valid()) { return; }

	int trackCount = 0;
	while (itStmt.step())
	{
		int itId = itStmt.colInt(0);
		QDomElement trackElem = buildInstrumentTrackElement(db, doc, songTc, itStmt);

		// Add midiclips (song-level clips with position/length, no pattern_id)
		SqliteStmt mcStmt(db,
			"SELECT * FROM midi_clip WHERE instrument_track_id = ? AND pattern_id IS NULL ORDER BY position");
		if (!mcStmt.valid()) { continue; }
		mcStmt.bindInt(1, itId);

		while (mcStmt.step())
		{
			// midi_clip columns: 0=id, 1=instrument_track_id, 2=pattern_id,
			// 3=position, 4=length, 5=clip_type, 6=steps, 7=muted, 8=name, 9=color, 10=extra_json
			QDomElement mcElem = doc.createElement("midiclip");
			mcElem.setAttribute("pos", mcStmt.colInt(3));
			if (mcStmt.colInt(4) > 0)
			{
				mcElem.setAttribute("len", mcStmt.colInt(4));
			}
			mcElem.setAttribute("type", mcStmt.colInt(5));
			mcElem.setAttribute("name", mcStmt.colText(8));
			mcElem.setAttribute("muted", mcStmt.colInt(7));
			mcElem.setAttribute("steps", mcStmt.colInt(6));
			if (!mcStmt.colIsNull(9))
			{
				mcElem.setAttribute("color", mcStmt.colText(9));
			}
			applyExtraJson(doc, mcElem, mcStmt.colText(10));

			// Notes
			SqliteStmt noteStmt(db,
				"SELECT * FROM note WHERE midi_clip_id = ? ORDER BY position");
			if (noteStmt.valid())
			{
				noteStmt.bindInt(1, mcStmt.colInt(0));
				while (noteStmt.step())
				{
					QDomElement noteElem = doc.createElement("note");
					noteElem.setAttribute("pos", noteStmt.colInt(2));
					noteElem.setAttribute("len", noteStmt.colInt(3));
					noteElem.setAttribute("key", noteStmt.colInt(4));
					noteElem.setAttribute("vol", noteStmt.colInt(5));
					noteElem.setAttribute("pan", noteStmt.colInt(6));
					noteElem.setAttribute("type", noteStmt.colInt(7));
					applyExtraJson(doc, noteElem, noteStmt.colText(8));
					mcElem.appendChild(noteElem);
				}
			}

			trackElem.appendChild(mcElem);
		}

		trackCount++;
	}

	if (trackCount > 0)
	{
		logMsg("Song instrument tracks: %d tracks", trackCount);
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

	// Always emit <controllers> (LMMS expects it even when empty)
	songElem.appendChild(controllersElem);
	if (count > 0)
	{
		logMsg("Controllers: %d", count);
	}
}

// Export scales and keymaps
void exportScalesAndKeymaps(sqlite3* db, QDomDocument& doc, QDomElement& songElem)
{
	// Scales
	if (tableExists(db, "scale"))
	{
		SqliteStmt stmt(db, "SELECT * FROM scale ORDER BY sort_order");
		if (stmt.valid())
		{
			QDomElement scalesElem = doc.createElement("scales");
			int count = 0;
			while (stmt.step())
			{
				// 0=id, 1=description, 2=intervals_json, 3=sort_order
				QDomElement scaleElem = doc.createElement("scale");
				scaleElem.setAttribute("description", stmt.colText(1));

				// Parse intervals array
				QString intervalsJson = stmt.colText(2);
				if (!intervalsJson.isEmpty() && intervalsJson != "[]")
				{
					QJsonParseError err;
					QJsonDocument jdoc = QJsonDocument::fromJson(intervalsJson.toUtf8(), &err);
					if (err.error == QJsonParseError::NoError && jdoc.isArray())
					{
						for (const auto& item : jdoc.array())
						{
							if (item.isObject())
							{
								QJsonObject intObj = item.toObject();
								QDomElement intElem = doc.createElement("interval");
								intElem.setAttribute("num", intObj.value("num").toString());
								intElem.setAttribute("den", intObj.value("den").toString());
								scaleElem.appendChild(intElem);
							}
						}
					}
				}

				scalesElem.appendChild(scaleElem);
				count++;
			}
			if (count > 0) { songElem.appendChild(scalesElem); }
		}
	}

	// Keymaps
	if (tableExists(db, "keymap"))
	{
		SqliteStmt stmt(db, "SELECT * FROM keymap ORDER BY sort_order");
		if (stmt.valid())
		{
			QDomElement keymapsElem = doc.createElement("keymaps");
			int count = 0;
			while (stmt.step())
			{
				// 0=id, 1=description, 2=base_key, 3=base_freq, 4=first_key, 5=last_key, 6=middle_key, 7=sort_order, 8=extra_json
				QDomElement kmElem = doc.createElement("keymap");
				kmElem.setAttribute("description", stmt.colText(1));
				kmElem.setAttribute("base_key", stmt.colInt(2));
				kmElem.setAttribute("base_freq", QString::number(stmt.colDouble(3), 'g', 15));
				kmElem.setAttribute("first_key", stmt.colInt(4));
				kmElem.setAttribute("last_key", stmt.colInt(5));
				kmElem.setAttribute("middle_key", stmt.colInt(6));
				applyExtraJsonAttrsOnly(kmElem, stmt.colText(8));
				keymapsElem.appendChild(kmElem);
				count++;
			}
			if (count > 0) { songElem.appendChild(keymapsElem); }
		}
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
	// Default root attributes (may be overridden by exportProjectMetadata)
	root.setAttribute("version", "30");
	root.setAttribute("type", "song");
	root.setAttribute("creator", "LMMS");
	root.setAttribute("creatorversion", "1.3.0-alpha");
	doc.appendChild(root);

	// <head> — also sets root attributes from v3 schema
	QDomElement head = doc.createElement("head");
	exportProjectMetadata(db, doc, root, head);
	root.appendChild(head);

	// <song>
	QDomElement song = doc.createElement("song");

	// Song trackcontainer
	QDomElement songTc = doc.createElement("trackcontainer");
	songTc.setAttribute("type", "song");
	applyTrackcontainerState(db, songTc, "song");

	// Build patternstore as a detached element
	QDomElement patternstore = doc.createElement("trackcontainer");
	patternstore.setAttribute("type", "patternstore");
	applyTrackcontainerState(db, patternstore, "patternstore");

	// Populate patternstore with instrument tracks
	exportPatternstore(db, doc, patternstore, patternIdToIndex);

	// Pattern tracks (first one gets the patternstore)
	exportPatternTracks(db, doc, songTc, patternstore, patternIdToIndex);

	// Automation tracks (trackcontainer level only)
	exportAutomationTracks(db, doc, songTc, "song_tc");

	// Sample tracks
	exportSampleTracks(db, doc, songTc);

	// Song-level instrument tracks
	exportSongInstrumentTracks(db, doc, songTc);

	song.appendChild(songTc);

	// Song-level automation tracks (outside trackcontainer)
	exportAutomationTracks(db, doc, song, "song");

	// Mixer
	exportMixer(db, doc, song);

	// Song UI elements (ControllerRackView, pianoroll, automationeditor, projectnotes, timeline)
	exportSongUiElements(db, doc, song);

	// Controllers (after UI elements to match original .mmp ordering)
	exportControllers(db, doc, song);

	// Scales and keymaps
	exportScalesAndKeymaps(db, doc, song);

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
