/*
 * SqliteToXml.h - converts .lmms-db (SQLite) files to XML for the DataFile pipeline
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

#ifndef LMMS_SQLITE_TO_XML_H
#define LMMS_SQLITE_TO_XML_H

#include <QByteArray>
#include <QString>

#include "lmms_export.h"

namespace lmms
{

/**
 * @class SqliteToXml
 * @brief Converts an LMMS SQLite database (.lmms-db) to XML bytes
 *
 * This is the bridge for Option C of the SQLite integration:
 * read SQLite tables and generate an XML byte stream that can be
 * fed into the existing DataFile loading pipeline unchanged.
 *
 * The generated XML matches the format produced by lmms_export.py
 * and is equivalent to a standard .mmp file.
 */
class LMMS_EXPORT SqliteToXml
{
public:
	/**
	 * @brief Convert a .lmms-db file to XML bytes
	 * @param dbPath Path to the SQLite database file
	 * @return XML content as a QByteArray, or empty on error
	 */
	static QByteArray convert(const QString& dbPath);

	/**
	 * @brief Check if a file appears to be an LMMS SQLite database
	 * @param fileName File path to check (by extension)
	 * @return true if the file has the .lmms-db extension
	 */
	static bool isSqliteProject(const QString& fileName);
};

} // namespace lmms

#endif // LMMS_SQLITE_TO_XML_H
