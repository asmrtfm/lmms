/*
 * MidiClip.h - declaration of class MidiClip, which contains all information
 *              about a clip
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_MIDI_CLIP_H
#define LMMS_MIDI_CLIP_H

#include "Clip.h" // Base class providing clip positioning, muting, and serialization
#include "Note.h" // Note class and NoteVector type for storing MIDI note data


namespace lmms
{


class InstrumentTrack; // Forward declaration for the track that owns this clip

namespace gui
{
class MidiClipView; // Forward declaration for the visual representation of this clip
}


/**
 * @class MidiClip
 * @brief A clip containing MIDI note data, used both for piano roll melodies
 *        and beat/step-sequencer patterns.
 *
 * MidiClip stores a collection of Notes and supports two modes:
 * - BeatClip: Fixed-length step sequencer pattern (used in the Pattern Editor)
 * - MelodyClip: Variable-length piano roll pattern (used in the Song Editor)
 *
 * The clip type is automatically determined by checkType() based on whether
 * the notes conform to a step grid pattern. Beat clips have a configurable
 * number of steps (default: TimePos::stepsPerBar(), typically 32).
 */
class LMMS_EXPORT MidiClip : public Clip
{
	Q_OBJECT
public:
	/**
	 * @enum Type
	 * @brief Distinguishes between step-sequencer and piano roll clip modes
	 */
	enum class Type
	{
		BeatClip,  ///< Step sequencer pattern with fixed step grid (Pattern Editor)
		MelodyClip ///< Free-form piano roll pattern with arbitrary note placement
	} ;

	/// Construct a new empty MidiClip owned by the given InstrumentTrack
	MidiClip( InstrumentTrack* instrumentTrack );
	/// Copy constructor: duplicate all notes and settings from another MidiClip
	MidiClip( const MidiClip& other );
	~MidiClip() override;

	/// Initialize the clip after construction (sets up time signature connections)
	void init();

	/// Recalculate the clip's length based on its notes and type
	void updateLength();

	// -- Note management --

	/// Add a note to this clip, optionally quantizing its position to the grid
	Note * addNote( const Note & _new_note, const bool _quant_pos = true );

	/// Remove the note at the given iterator position; returns iterator to next note
	NoteVector::const_iterator removeNote(NoteVector::const_iterator it);
	/// Remove a specific note by pointer; returns iterator to next note
	NoteVector::const_iterator removeNote(Note* note);

	/// Return the note at the given step index (for beat clips), or nullptr if step is empty
	Note * noteAtStep( int _step );

	/// Sort all notes by position (used after bulk modifications)
	void rearrangeAllNotes();
	/// Delete all notes from this clip
	void clearNotes();

	/// Return a const reference to the internal note collection
	inline const NoteVector & notes() const
	{
		return m_notes;
	}

	/// Create and return a new note at the given step position (for beat clips)
	Note * addStepNote( int step );
	/// Enable or disable a step in a beat clip (creates/removes the note at that step)
	void setStep( int step, bool enabled );

	/// Split the given notes at the specified position into two halves
	void splitNotes(const NoteVector& notes, TimePos pos);

	// -- Clip type --

	/// Return the current clip type (BeatClip or MelodyClip)
	inline Type type() const
	{
		return m_clipType;
	}


	// -- Navigation --

	/// Return the MidiClip in the previous pattern index position, or nullptr
	MidiClip * previousMidiClip() const;
	/// Return the MidiClip in the next pattern index position, or nullptr
	MidiClip * nextMidiClip() const;

	// -- Serialization --

	/// Save this clip's notes, type, and step count to XML
	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	/// Load this clip's notes, type, and step count from XML
	void loadSettings( const QDomElement & _this ) override;
	/// Return the XML element tag name for this clip type
	inline QString nodeName() const override
	{
		return "midiclip";
	}

	/// Return the InstrumentTrack that owns this clip
	inline InstrumentTrack * instrumentTrack() const
	{
		return m_instrumentTrack;
	}

	/// Return true if this clip contains no notes
	bool empty();


	/// Create and return a new MidiClipView for this clip within the given TrackView
	gui::ClipView * createView( gui::TrackView * _tv ) override;


	/// Make the dataChanged signal accessible (inherited from Model but needed publicly)
	using Model::dataChanged;

public slots:
	/// Reset step count to default value (TimePos::stepsPerBar(), typically 32)
	void resetSteps();
	/// Add one bar's worth of steps (TimePos::stepsPerBar()) to the step count
	void addSteps();
	/// Duplicate all existing steps by doubling the step count and copying note data
	void cloneSteps();
	/// Remove one bar's worth of steps from the end (if more than one bar remains)
	void removeSteps();
	/// Clear all notes with an undo checkpoint
	void clear();

protected:
	/// Notify the PatternStore that this clip's content has changed
	void updatePatternTrack();

protected slots:
	/// Handle time signature changes by recalculating beat clip length
	void changeTimeSignature();


private:
	/// Calculate the expected length of this clip when in BeatClip mode
	TimePos beatClipLength() const;

	/// Set the clip type and emit signals if changed
	void setType( Type _new_clip_type );
	/// Auto-detect whether this clip should be BeatClip or MelodyClip based on its notes
	void checkType();

	/// Resize this clip to match the first track in its container (for pattern sync)
	void resizeToFirstTrack();

	InstrumentTrack * m_instrumentTrack; ///< The instrument track that owns this clip

	Type m_clipType; ///< Current clip mode (BeatClip or MelodyClip)

	NoteVector m_notes; ///< All MIDI notes in this clip, sorted by position
	int m_steps;        ///< Number of steps in beat mode (multiple of stepsPerBar)

	/// Find an adjacent MidiClip by offset (-1 for previous, +1 for next)
	MidiClip * adjacentMidiClipByOffset(int offset) const;

	friend class gui::MidiClipView; ///< MidiClipView needs direct access to notes and steps


signals:
	/// Emitted when this MidiClip is about to be destroyed (for cleanup in listeners)
	void destroyedMidiClip( lmms::MidiClip* );
} ;


} // namespace lmms

#endif // LMMS_MIDI_CLIP_H
