/*
 * DataFile.h - class for reading and writing LMMS data files
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2012-2013 Paul Giblock <p/at/pgiblock.net>
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

#ifndef LMMS_DATA_FILE_H
#define LMMS_DATA_FILE_H

#include <map>          // For std::map used by ResourcesMap type alias
#include <QDomDocument> // Base class for XML document handling
#include <vector>       // For std::vector used by upgrade method lists

#include "lmms_export.h" // LMMS_EXPORT macro for shared library symbol visibility

class QTextStream; // Forward declaration for the write() method parameter

namespace lmms
{

class ProjectVersion; // Forward declaration for version comparison in upgrade logic


/**
 * @class DataFile
 * @brief Handles reading, writing, and upgrading LMMS data files (projects, presets, etc.)
 *
 * DataFile extends QDomDocument to provide LMMS-specific file I/O functionality.
 * It supports multiple file types (song projects, templates, instrument presets,
 * pattern data, etc.), automatic format upgrading from older LMMS versions,
 * and resource bundling for portable project files.
 *
 * The XML document structure consists of a root <lmms-project> element with
 * <head> and content (type-specific) child elements.
 */
class LMMS_EXPORT DataFile : public QDomDocument
{

	/// Function pointer type for version upgrade methods (member function pointers)
	using UpgradeMethod = void(DataFile::*)();

public:
	/**
	 * @enum Type
	 * @brief Identifies the kind of data stored in this DataFile
	 *
	 * Each type corresponds to a specific XML root element name and file extension.
	 * The type determines validation rules, upgrade behavior, and serialization format.
	 */
	enum class Type
	{
		Unknown,                 ///< Unrecognized or uninitialized file type
		SongProject,             ///< Full song project (.mmp / .mmpz)
		SongProjectTemplate,     ///< Song template (.mpt) - project with preset configuration
		InstrumentTrackSettings, ///< Instrument preset (.xpf) - single instrument track settings
		DragNDropData,           ///< Transient data for drag-and-drop operations (not persisted)
		ClipboardData,           ///< Transient data for clipboard copy/paste (not persisted)
		JournalData,             ///< Undo/redo journal entries (not persisted)
		EffectSettings,          ///< Effect chain preset data
		MidiClip,                ///< MIDI clip data (piano roll pattern)
		PatternData              ///< Pattern track export data (.xppt) - instrument tracks with clips for a single pattern
	} ;

	/// Construct a DataFile by loading and parsing an existing file from disk
	DataFile( const QString& fileName );
	/// Construct a DataFile by parsing raw XML byte data (e.g. from clipboard or drag-and-drop)
	DataFile( const QByteArray& data );
	/// Construct an empty DataFile of the given type, ready to be populated with content
	DataFile( Type type );

	virtual ~DataFile() = default;

	/**
	 * @brief Validate that this DataFile's type is consistent with the given file extension
	 * @param extension The file extension to check against (e.g. "mmp", "xppt")
	 * @return true if the extension is valid for this DataFile's type
	 */
	bool validate( QString extension );

	/**
	 * @brief Append the appropriate file extension if not already present
	 * @param fn The filename to potentially modify
	 * @return The filename with the correct extension for this DataFile's type
	 */
	QString nameWithExtension( const QString& fn ) const;

	/// Serialize the XML document to a text stream (used internally by writeFile)
	void write( QTextStream& strm );
	/**
	 * @brief Write the DataFile to disk, optionally bundling external resources
	 * @param fn The target file path (compressed with gzip if extension ends in 'z')
	 * @param withResources If true, copy referenced audio files alongside the project
	 * @return true if the write succeeded
	 */
	bool writeFile(const QString& fn, bool withResources = false);
	/// Copy all referenced resources (samples, etc.) to resourcesDir, updating paths in the DOM
	bool copyResources(const QString& resourcesDir);
	/// Check whether any plugins in this DataFile reference local (non-bundled) plugin binaries
	bool hasLocalPlugins(QDomElement parent = QDomElement(), bool firstCall = true) const;

	/// Access the content element (the type-specific root, e.g. <song>, <patterndata>)
	QDomElement& content()
	{
		return m_content;
	}

	/// Access the <head> element containing project metadata (bpm, master volume, etc.)
	QDomElement& head()
	{
		return m_head;
	}

	/// Return the type of this DataFile
	Type type() const
	{
		return m_type;
	}

	/// Return the file format version as an integer index into UPGRADE_VERSIONS
	unsigned int legacyFileVersion();

private:
	/// Look up a Type enum value from its string name (e.g. "song" -> Type::SongProject)
	static Type type( const QString& typeName );
	/// Convert a Type enum value to its string name (e.g. Type::SongProject -> "song")
	static QString typeName( Type type );

	/// Recursively remove metadata nodes (nodes with metadata="1" attribute) from the DOM tree
	void cleanMetaNodes( QDomElement de );

	/// Update src attributes in resource-referencing elements using a path mapping
	void mapSrcAttributeInElementsWithResources(const QMap<QString, QString>& map);

	// Version-specific upgrade routines that transform the DOM from one format to the next.
	// Each method handles the migration from a specific older version to a newer one.
	// They are applied sequentially during upgrade() based on the file's version.
	void upgrade_0_2_1_20070501();
	void upgrade_0_2_1_20070508();
	void upgrade_0_3_0_rc2();
	void upgrade_0_3_0();
	void upgrade_0_4_0_20080104();
	void upgrade_0_4_0_20080118();
	void upgrade_0_4_0_20080129();
	void upgrade_0_4_0_20080409();
	void upgrade_0_4_0_20080607();
	void upgrade_0_4_0_20080622();
	void upgrade_0_4_0_beta1();
	void upgrade_0_4_0_rc2();
	void upgrade_1_0_99();
	void upgrade_1_1_0();
	void upgrade_1_1_91();
	void upgrade_1_2_0_rc3();
	void upgrade_1_3_0();
	void upgrade_noHiddenClipNames();
	void upgrade_automationNodes();
	void upgrade_extendedNoteRange();
	void upgrade_defaultTripleOscillatorHQ();
	void upgrade_mixerRename();
	void upgrade_bbTcoRename();
	void upgrade_sampleAndHold();
	void upgrade_midiCCIndexing();
	void upgrade_loopsRename();
	void upgrade_noteTypes();
	void upgrade_fixCMTDelays();
	void upgrade_fixBassLoopsTypo();
	void findProblematicLadspaPlugins();

	/// Ordered list of all upgrade methods, applied sequentially during format migration
	static const std::vector<UpgradeMethod> UPGRADE_METHODS;
	/// Ordered list of project versions corresponding to each upgrade method
	static const std::vector<ProjectVersion> UPGRADE_VERSIONS;

	/// Maps DOM element tag names to their resource-referencing attribute names (for bundling)
	using ResourcesMap = std::map<QString, std::vector<QString>>;
	static const ResourcesMap ELEMENTS_WITH_RESOURCES;

	/// Run all applicable upgrade methods to migrate this DataFile to the current format
	void upgrade();

	/// Parse raw XML/compressed data and populate the DOM, applying upgrades as needed
	void loadData( const QByteArray & _data, const QString & _sourceFile );

	QString m_fileName;     ///< Source file path, or empty string if not loaded from a file
	QDomElement m_content;  ///< The type-specific content root element (e.g. <song>, <patterndata>)
	QDomElement m_head;     ///< The <head> element containing project-level metadata
	Type m_type;            ///< The type of data this file represents
	unsigned int m_fileVersion; ///< The format version index for upgrade tracking
} ;


} // namespace lmms

#endif // LMMS_DATA_FILE_H
