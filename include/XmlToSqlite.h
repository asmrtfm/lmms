/*
 * XmlToSqlite.h - converts LMMS XML (QDomDocument) to SQLite .lmms-db format
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

#ifndef LMMS_XML_TO_SQLITE_H
#define LMMS_XML_TO_SQLITE_H

#include <QDomDocument>
#include <QString>

#include "lmms_export.h"

namespace lmms
{

/**
 * @class XmlToSqlite
 * @brief Converts an LMMS QDomDocument to a SQLite database (.lmms-db)
 *
 * This is the save-path counterpart of SqliteToXml. It takes the
 * QDomDocument that DataFile already has (since DataFile extends
 * QDomDocument) and writes all entities to a SQLite database file.
 *
 * The conversion logic is a C++ port of tools/lmms_convert.py.
 */
class LMMS_EXPORT XmlToSqlite
{
public:
	/**
	 * @brief Convert a QDomDocument to a .lmms-db file
	 * @param doc The XML document to convert (typically the DataFile itself)
	 * @param dbPath Path for the output SQLite database file
	 * @return true on success, false on error
	 */
	static bool convert(const QDomDocument& doc, const QString& dbPath);
};

} // namespace lmms

#endif // LMMS_XML_TO_SQLITE_H
