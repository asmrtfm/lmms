/*
 * MidiClip.cpp - implementation of class MidiClip, which holds notes
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2005-2007 Danny McRae <khjklujn/at/yahoo.com>
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

#include "MidiClip.h"

#include <algorithm>
#include <QDomElement>

#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MidiClipView.h"
#include "PatternStore.h"
#include "PianoRoll.h"



namespace lmms
{

/**
 * @brief Constructs a new empty MidiClip for the given InstrumentTrack.
 *
 * Initializes the clip as a BeatClip with the default number of steps
 * (TimePos::stepsPerBar(), typically 32). If the clip belongs to the
 * PatternStore (i.e. the beat/bassline editor), it resizes itself to
 * match the step count of existing tracks in the same pattern.
 *
 * @param _instrument_track The InstrumentTrack that owns this clip.
 */
MidiClip::MidiClip( InstrumentTrack * _instrument_track ) :
	Clip( _instrument_track ),                   // Initialize base Clip with the parent track
	m_instrumentTrack( _instrument_track ),       // Store reference to owning instrument track
	m_clipType( Type::BeatClip ),                 // Default to beat/step-sequencer mode
	m_steps( TimePos::stepsPerBar() )             // Default step count (typically 32 steps per bar)
{
	// If this clip belongs to the PatternStore (beat/bassline editor),
	// synchronize step count with the first existing track in the pattern
	if (_instrument_track->trackContainer()	== Engine::patternStore())
	{
		resizeToFirstTrack();
	}
	// Perform common initialization (connect signals, update length)
	init();
	// Beat clips in PatternStore auto-resize when notes change
	setAutoResize( true );
}




/**
 * @brief Copy constructor: creates a deep copy of another MidiClip.
 *
 * Duplicates all notes from the source clip and copies its type and step count.
 * The auto-resize behavior depends on whether this clip is in the PatternStore
 * (auto-resize enabled) or the Song editor (auto-resize disabled, since the
 * user controls clip length manually in the song timeline).
 *
 * @param other The MidiClip to copy from.
 */
MidiClip::MidiClip( const MidiClip& other ) :
	Clip( other.m_instrumentTrack ),              // Initialize base Clip with the same parent track
	m_instrumentTrack( other.m_instrumentTrack ), // Copy the owning instrument track reference
	m_clipType( other.m_clipType ),               // Copy the clip type (BeatClip or MelodyClip)
	m_steps( other.m_steps )                      // Copy the step count
{
	// Deep-copy each note from the source clip into this clip's note vector
	for (const auto& note : other.m_notes)
	{
		m_notes.push_back(new Note(*note));
	}

	// Perform common initialization (connect signals, update length)
	init();
	// Set auto-resize based on which container this clip belongs to
	switch( getTrack()->trackContainer()->type() )
	{
		case TrackContainer::Type::Pattern:
			// PatternStore clips auto-resize to fit their note content
			setAutoResize( true );
			break;

		case TrackContainer::Type::Song:
			// fall through — Song editor clips have user-controlled length
		default:
			// Disable auto-resize so the user can freely set clip length
			setAutoResize( false );
			break;
	}
}


/**
 * @brief Destructor: cleans up all allocated notes and emits the destroyed signal.
 *
 * Emits destroyedMidiClip() before cleanup so that listeners (e.g. PianoRoll,
 * MidiClipView) can detach themselves from this clip before it becomes invalid.
 */
MidiClip::~MidiClip()
{
	// Notify listeners that this clip is being destroyed
	emit destroyedMidiClip( this );

	// Free all heap-allocated Note objects
	for (const auto& note : m_notes)
	{
		delete note;
	}

	// Clear the vector (not strictly necessary, but good practice)
	m_notes.clear();
}




/**
 * @brief Resizes this clip's step count to match the first existing instrument
 *        track in the same PatternStore container.
 *
 * When a new track is added to the PatternStore, its clips need to have the
 * same number of steps as existing tracks so all patterns stay in sync.
 * This method finds the first other InstrumentTrack in the container and
 * copies its step count for the corresponding clip index.
 */
void MidiClip::resizeToFirstTrack()
{
	// Resize this track to be the same as existing tracks in the pattern
	// Get all tracks in the same container (PatternStore)
	const TrackContainer::TrackList & tracks =
		m_instrumentTrack->trackContainer()->tracks();
	// Iterate through tracks to find the first instrument track that is not this one
	for (const auto& track : tracks)
	{
		if (track->type() == Track::Type::Instrument)
		{
			if (track != m_instrumentTrack)
			{
				// Find the index of this clip within its parent track's clip list
				const auto& instrumentTrackClips = m_instrumentTrack->getClips();
				const auto currentClipIt = std::find(instrumentTrackClips.begin(), instrumentTrackClips.end(), this);
				// Calculate the clip index; -1 (wrapping to max unsigned) if not found
				unsigned int currentClip = currentClipIt != instrumentTrackClips.end() ?
					std::distance(instrumentTrackClips.begin(), currentClipIt) : -1;
				// Copy the step count from the corresponding clip in the reference track
				m_steps = static_cast<MidiClip*>(track->getClip(currentClip))->m_steps;
			}
			// Only check the first instrument track found, then stop
			break;
		}
	}
}




/**
 * @brief Common initialization called by all constructors.
 *
 * Connects to the Song's time signature change signal so beat clips can
 * recalculate their step layout. Temporarily disables journalling during
 * the initial length update to avoid recording it as an undoable action.
 */
void MidiClip::init()
{
	// React to time signature changes (e.g. 4/4 -> 3/4) by recalculating steps
	connect( Engine::getSong(), SIGNAL(timeSignatureChanged(int,int)),
				this, SLOT(changeTimeSignature()));
	// Disable journalling so the initial updateLength() isn't recorded as undoable
	saveJournallingState( false );

	// Calculate and set the clip's initial length based on its notes and type
	updateLength();
	// Restore the previous journalling state
	restoreJournallingState();
}




/**
 * @brief Recalculates and updates the clip's length based on its content.
 *
 * For BeatClip mode, the length is determined by beatClipLength() which
 * considers the step count and step note positions. For MelodyClip mode,
 * the length is the smallest number of full bars that encompasses all notes.
 * After updating length, notifies the PatternStore if applicable.
 */
void MidiClip::updateLength()
{
	// For beat clips, use the dedicated beat length calculation
	if( m_clipType == Type::BeatClip )
	{
		changeLength( beatClipLength() );
		// Notify PatternStore that clip dimensions may have changed
		updatePatternTrack();
		return;
	}

	// For melody clips: find the latest note endpoint
	// Start with a minimum of one bar
	tick_t max_length = TimePos::ticksPerBar();

	for (const auto& note : m_notes)
	{
		// Only consider notes with positive length (skip step-type marker notes)
		if (note->length() > 0)
		{
			// Track the furthest endpoint (note position + note length)
			max_length = std::max<tick_t>(max_length, note->endPos());
		}
	}
	// Round up to the next full bar boundary and set as clip length
	changeLength( TimePos( max_length ).nextFullBar() *
						TimePos::ticksPerBar() );
	// Notify PatternStore that clip dimensions may have changed
	updatePatternTrack();
}




/**
 * @brief Calculates the expected length of a BeatClip based on step notes and step count.
 *
 * Scans all step-type notes to find the last occupied step position, then
 * also considers the configured m_steps count (which may extend beyond the
 * last note). Returns the length rounded up to the next full bar.
 *
 * @return The beat clip length as a TimePos, always a multiple of full bars.
 */
TimePos MidiClip::beatClipLength() const
{
	// Start with a minimum of one bar
	tick_t max_length = TimePos::ticksPerBar();

	// Find the latest step note position
	for (const auto& note : m_notes)
	{
		if (note->type() == Note::Type::Step)
		{
			// Step notes have zero/negative length; use position + 1 tick as the extent
			max_length = std::max<tick_t>(max_length, note->pos() + 1);
		}
	}

	// If the step count differs from the default, calculate the tick length
	// from the step count (e.g. 64 steps = 2 bars worth of ticks)
	if (m_steps != TimePos::stepsPerBar())
	{
		max_length = m_steps * TimePos::ticksPerBar() / TimePos::stepsPerBar();
	}

	// Round up to the next full bar boundary
	return TimePos{max_length}.nextFullBar() * TimePos::ticksPerBar();
}




/**
 * @brief Adds a new note to this clip, inserting it in sorted position order.
 *
 * Creates a heap-allocated copy of the provided note. If quantization is
 * requested and the PianoRoll is available, the note's position is snapped
 * to the current quantization grid. The note is inserted into m_notes
 * maintaining sorted order (by position). After insertion, the clip type
 * is re-evaluated and the length is updated.
 *
 * @param _new_note   The note to add (copied, caller retains ownership of original).
 * @param _quant_pos  If true, quantize the note position to the PianoRoll grid.
 * @return Pointer to the newly created Note in the clip's note vector.
 */
Note * MidiClip::addNote( const Note & _new_note, const bool _quant_pos )
{
	// Create a heap-allocated copy of the input note
	auto new_note = new Note(_new_note);
	// Optionally snap the note position to the PianoRoll's quantization grid
	if (_quant_pos && gui::getGUI()->pianoRoll())
	{
		new_note->quantizePos(gui::getGUI()->pianoRoll()->quantization());
	}

	// Lock the instrument track to safely modify the note vector
	// (audio engine may be reading notes concurrently)
	instrumentTrack()->lock();
	// Insert in sorted order using upper_bound to maintain position-based sorting
	m_notes.insert(std::upper_bound(m_notes.begin(), m_notes.end(), new_note, Note::lessThan), new_note);
	instrumentTrack()->unlock();

	// Re-evaluate clip type (adding a non-step note may switch from BeatClip to MelodyClip)
	checkType();
	// Recalculate clip length to accommodate the new note
	updateLength();

	// Notify views and other listeners that note data has changed
	emit dataChanged();

	return new_note;
}




/**
 * @brief Removes the note at the given iterator position from this clip.
 *
 * Deletes the Note object and erases it from the vector. The instrument
 * track is locked during modification to prevent concurrent access from
 * the audio engine. After removal, clip type and length are recalculated.
 *
 * @param it  Const iterator pointing to the note to remove.
 * @return Iterator to the next note after the removed one.
 */
NoteVector::const_iterator MidiClip::removeNote(NoteVector::const_iterator it)
{
	// Lock to prevent audio engine from reading during modification
	instrumentTrack()->lock();
	// Free the heap-allocated Note object
	delete *it;
	// Erase from the vector and get iterator to the next element
	auto new_it = m_notes.erase(it);
	instrumentTrack()->unlock();

	// Re-evaluate clip type (removing a melody note may revert to BeatClip)
	checkType();
	// Recalculate clip length since the longest note may have been removed
	updateLength();

	// Notify views and other listeners that note data has changed
	emit dataChanged();
	return new_it;
}

/**
 * @brief Removes a specific note by pointer from this clip.
 *
 * Searches for the note in the vector and removes it if found. Thread-safe
 * via instrument track locking. After removal, clip type and length are
 * recalculated.
 *
 * @param note  Pointer to the Note to remove. Must be owned by this clip.
 * @return Iterator to the next note after the removed one, or end() if not found.
 */
NoteVector::const_iterator MidiClip::removeNote(Note* note)
{
	// Lock to prevent audio engine from reading during modification
	instrumentTrack()->lock();

	// Search for the note pointer in the vector
	auto it = std::find(m_notes.begin(), m_notes.end(), note);
	if (it != m_notes.end())
	{
		// Free the heap-allocated Note and erase from vector
		delete *it;
		it = m_notes.erase(it);
	}

	instrumentTrack()->unlock();

	// Re-evaluate clip type and length even if note wasn't found (safe no-op)
	checkType();
	updateLength();

	// Notify views and other listeners that note data has changed
	emit dataChanged();
	return it;
}


/**
 * @brief Returns the step note at the given step index, or nullptr if the step is empty.
 *
 * Searches through all notes for one that matches the step's tick position
 * and is of type Step. Used by beat/step-sequencer operations to check
 * whether a particular step is active.
 *
 * @param step  The zero-based step index to look up.
 * @return Pointer to the Note at that step, or nullptr if no note exists there.
 */
// Returns a pointer to the note at specified step, or nullptr if note doesn't exist
Note * MidiClip::noteAtStep(int step)
{
	for (const auto& note : m_notes)
	{
		// Check if the note's position matches the expected tick position for this step
		// and that the note is a step-type note (not a melody note)
		if (note->pos() == TimePos::stepPosition(step)
			&& note->type() == Note::Type::Step)
		{
			return note;
		}
	}
	// No step note found at this position
	return nullptr;
}



/**
 * @brief Re-sorts all notes by their start position.
 *
 * Called after bulk operations that may have moved notes out of sorted order
 * (e.g. transposing, shifting). Uses Note::lessThan as the comparator.
 */
void MidiClip::rearrangeAllNotes()
{
	// sort notes by start time
	std::sort(m_notes.begin(), m_notes.end(), Note::lessThan);
}



/**
 * @brief Deletes all notes from this clip.
 *
 * Frees every heap-allocated Note and clears the vector. Thread-safe via
 * instrument track locking. After clearing, the clip type is re-evaluated
 * (an empty clip remains as whatever type it was, since all_of on empty
 * returns true, making it a BeatClip).
 */
void MidiClip::clearNotes()
{
	// Lock to prevent audio engine from reading during modification
	instrumentTrack()->lock();
	// Free all heap-allocated Note objects
	for (const auto& note : m_notes)
	{
		delete note;
	}
	// Clear the vector
	m_notes.clear();
	instrumentTrack()->unlock();

	// Re-evaluate clip type (empty clip defaults to BeatClip since all_of on empty is true)
	checkType();
	// Notify views and other listeners that note data has changed
	emit dataChanged();
}




/**
 * @brief Creates a new step note at the given step position in a beat clip.
 *
 * Step notes have a fixed short length (1/16th of a bar = DefaultTicksPerBar / 16)
 * and are placed at the tick position corresponding to the step index.
 * The note type is set to Step to distinguish it from melody notes.
 *
 * @param step  The zero-based step index where the note should be placed.
 * @return Pointer to the newly created step Note.
 */
Note * MidiClip::addStepNote( int step )
{
	// Create a step note with 1/16th bar length at the step's tick position
	Note stepNote = Note(TimePos(DefaultTicksPerBar / 16), TimePos::stepPosition(step));
	// Mark as a step note (distinguishes from freely-placed melody notes)
	stepNote.setType(Note::Type::Step);

	// Add without quantization (step positions are already on-grid)
	return addNote(stepNote, false);
}




/**
 * @brief Enables or disables a step in a beat clip.
 *
 * If enabling, creates a step note at the position (only if one doesn't
 * already exist). If disabling, removes all notes at that step position
 * (uses a while loop in case of duplicates).
 *
 * @param step     The zero-based step index to modify.
 * @param enabled  True to enable (add note), false to disable (remove note).
 */
void MidiClip::setStep( int step, bool enabled )
{
	if( enabled )
	{
		// Only add a step note if one doesn't already exist at this position
		if ( !noteAtStep( step ) )
		{
			addStepNote( step );
		}
		return;
	}

	// Remove all notes at this step position (while loop handles duplicates)
	while( Note * note = noteAtStep( step ) )
	{
		removeNote( note );
	}
}




/**
 * @brief Splits a set of notes at a given position into two separate notes each.
 *
 * For each note in the input vector, if the split position falls within the
 * note's duration, the original note is shortened to end at the split point,
 * and a new note is created starting at the split point with the remaining
 * length. Notes where the split position is outside their range are skipped.
 *
 * @param notes  The notes to split (typically the current selection in PianoRoll).
 * @param pos    The tick position at which to split the notes.
 */
void MidiClip::splitNotes(const NoteVector& notes, TimePos pos)
{
	// Nothing to do if no notes are selected
	if (notes.empty()) { return; }

	// Record an undo checkpoint before modifying notes
	addJournalCheckPoint();

	for (const auto& note : notes)
	{
		// Calculate the length of the left (before split) and right (after split) portions
		int leftLength = pos.getTicks() - note->pos();
		int rightLength = note->length() - leftLength;

		// Split out of bounds — skip notes where the split position doesn't
		// fall within the note's duration
		if (leftLength <= 0 || rightLength <= 0)
		{
			continue;
		}

		// Reduce the original note's length to end at the split point
		note->setLength(leftLength);

		// Create a new note with the remaining length, starting at the split point
		Note newNote = Note(*note);
		newNote.setLength(rightLength);
		newNote.setPos(note->pos() + leftLength);

		// Add the right half as a new note (no quantization needed)
		addNote(newNote, false);
	}
}




/**
 * @brief Explicitly sets the clip type to BeatClip or MelodyClip.
 *
 * Only accepts valid type values; any other value is silently ignored.
 * This is called by checkType() after auto-detecting the appropriate type.
 *
 * @param _new_clip_type  The new clip type to set.
 */
void MidiClip::setType( Type _new_clip_type )
{
	// Only accept valid clip type values
	if( _new_clip_type == Type::BeatClip ||
				_new_clip_type == Type::MelodyClip )
	{
		m_clipType = _new_clip_type;
	}
}




/**
 * @brief Auto-detects whether this clip should be a BeatClip or MelodyClip.
 *
 * Examines all notes: if every note is of type Step (or the clip is empty),
 * the clip is classified as BeatClip. If any note is not a step note (i.e.
 * a freely-placed melody note), the clip becomes a MelodyClip.
 *
 * Note: std::all_of returns true for an empty range, so an empty clip
 * defaults to BeatClip.
 */
void MidiClip::checkType()
{
	// If all notes are StepNotes, we have a BeatClip
	// (std::all_of returns true for empty containers, so empty clips are BeatClips)
	const auto beatClip = std::all_of(m_notes.begin(), m_notes.end(), [](auto note) { return note->type() == Note::Type::Step; });

	// Set the type based on the detection result
	setType(beatClip ? Type::BeatClip : Type::MelodyClip);
}




/**
 * @brief Serializes this clip's data to an XML DOM element.
 *
 * Saves the clip type, name, color, position, mute state, step count, and
 * all notes. For clipboard/drag-and-drop operations, the position is stored
 * as -1 to indicate that loadSettings() should not override the target
 * clip's existing position.
 *
 * @param _doc   The XML document used to create new elements.
 * @param _this  The XML element to write attributes and child elements to.
 */
void MidiClip::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	// Save the clip type as an integer attribute (0 = BeatClip, 1 = MelodyClip)
	_this.setAttribute( "type", static_cast<int>(m_clipType) );
	// Save the user-assigned clip name
	_this.setAttribute( "name", name() );

	// Save the custom color if one has been set
	if (const auto& c = color())
	{
		_this.setAttribute("color", c->name());
	}
	// as the target of copied/dragged MIDI clip is always an existing
	// MIDI clip, we must not store actual position, instead we store -1
	// which tells loadSettings() not to mess around with position
	if( _this.parentNode().nodeName() == "clipboard" ||
			_this.parentNode().nodeName() == "dnddata" )
	{
		// Position -1 signals "don't change position on load" for paste/drop
		_this.setAttribute( "pos", -1 );
	}
	else
	{
		// Normal save: store the actual start position in ticks
		_this.setAttribute( "pos", startPosition() );
	}
	// Save mute state as integer (0 = unmuted, 1 = muted)
	_this.setAttribute( "muted", isMuted() );
	// Save the number of steps (for beat clips; preserved even for melody clips)
	_this.setAttribute( "steps", m_steps );

	// now save settings of all notes as child XML elements
	for (auto& note : m_notes)
	{
		note->saveState(_doc, _this);
	}
}




/**
 * @brief Deserializes this clip's data from an XML DOM element.
 *
 * Restores the clip type, name, color, position, mute state, step count,
 * and all notes from the XML. A position of -1 (from clipboard/DnD)
 * means the existing position is preserved. After loading, the clip type
 * is re-validated and the length is recalculated.
 *
 * @param _this  The XML element containing the serialized clip data.
 */
void MidiClip::loadSettings( const QDomElement & _this )
{
	// Restore the clip type from the "type" attribute
	m_clipType = static_cast<Type>( _this.attribute( "type"
								).toInt() );
	// Restore the user-assigned name
	setName( _this.attribute( "name" ) );

	// Restore custom color if the attribute exists
	if (_this.hasAttribute("color"))
	{
		setColor(QColor{_this.attribute("color")});
	}

	// Only update position if it's >= 0 (position -1 means "keep current",
	// used for clipboard paste and drag-and-drop operations)
	if( _this.attribute( "pos" ).toInt() >= 0 )
	{
		movePosition( _this.attribute( "pos" ).toInt() );
	}
	// Restore mute state: toggle if the saved state differs from current
	if (static_cast<bool>(_this.attribute("muted").toInt()) != isMuted())
	{
		toggleMute();
	}

	// Clear existing notes before loading new ones
	clearNotes();

	// Iterate over child elements to load individual notes
	QDomNode node = _this.firstChild();
	while( !node.isNull() )
	{
		// Only process element nodes, and skip metadata-only notes
		if( node.isElement() &&
			!node.toElement().attribute( "metadata" ).toInt() )
		{
			// Create a new Note and restore its state from the XML element
			auto n = new Note;
			n->restoreState( node.toElement() );
			// Append directly (notes are expected to be saved in order)
			m_notes.push_back( n );
		}
		node = node.nextSibling();
        }

	// Restore the step count from the "steps" attribute
	m_steps = _this.attribute( "steps" ).toInt();
	// If steps was missing or zero, use the default step count
	if( m_steps == 0 )
	{
		m_steps = TimePos::stepsPerBar();
	}

	// Re-validate the clip type based on the loaded notes
	checkType();
	// Recalculate clip length based on loaded content
	updateLength();

	// Notify views and other listeners that all data has been replaced
	emit dataChanged();
}




/**
 * @brief Returns the MidiClip at the previous pattern index, or nullptr if at the start.
 *
 * Used for navigation in the PianoRoll (e.g. Ctrl+Left to go to previous pattern).
 *
 * @return Pointer to the previous MidiClip, or nullptr if this is the first clip.
 */
MidiClip *  MidiClip::previousMidiClip() const
{
	return adjacentMidiClipByOffset(-1);
}




/**
 * @brief Returns the MidiClip at the next pattern index, or nullptr if at the end.
 *
 * Used for navigation in the PianoRoll (e.g. Ctrl+Right to go to next pattern).
 *
 * @return Pointer to the next MidiClip, or nullptr if this is the last clip.
 */
MidiClip *  MidiClip::nextMidiClip() const
{
	return adjacentMidiClipByOffset(1);
}




/**
 * @brief Returns an adjacent MidiClip at the given offset from this clip's index.
 *
 * Looks up this clip's index within its parent track's clip list, applies
 * the offset, and returns the clip at the resulting index. Returns nullptr
 * if the resulting index is out of bounds.
 *
 * @param offset  The offset from the current clip index (-1 for previous, +1 for next).
 * @return Pointer to the adjacent MidiClip, or nullptr if out of bounds.
 */
MidiClip * MidiClip::adjacentMidiClipByOffset(int offset) const
{
	// Get all clips belonging to this clip's parent instrument track
	auto& clips = m_instrumentTrack->getClips();
	// Calculate the target clip index by adding the offset to the current index
	int clipNum = m_instrumentTrack->getClipNum(this) + offset;
	// Bounds check: return nullptr if the index is negative or past the end
	if (clipNum < 0 || static_cast<size_t>(clipNum) >= clips.size()) { return nullptr; }
	// Cast the generic Clip pointer to MidiClip (safe because InstrumentTrack only has MidiClips)
	return dynamic_cast<MidiClip*>(clips[clipNum]);
}




/**
 * @brief Clears all notes with an undo checkpoint.
 *
 * Records a journal checkpoint so the operation can be undone, then
 * delegates to clearNotes() which handles the actual note deletion.
 */
void MidiClip::clear()
{
	// Record an undo checkpoint before clearing
	addJournalCheckPoint();
	// Delete all notes
	clearNotes();
}




/**
 * @brief Resets the step count to the default value (TimePos::stepsPerBar()).
 *
 * Restores the step count to the default number of steps per bar (typically 32).
 * This does not remove any notes — it only changes the number of visible steps
 * in the beat editor. Notes beyond the new step count will still exist but won't
 * be visible or playable until the step count is increased again.
 *
 * This was added as a feature to complement addSteps/removeSteps/cloneSteps,
 * providing a quick way to return to the default beat pattern length.
 */
void MidiClip::resetSteps()
{
	// Reset to the default number of steps per bar (typically 32)
	m_steps = TimePos::stepsPerBar();
	// Recalculate clip length to reflect the new step count
	updateLength();
	// Notify views to redraw with the updated step count
	emit dataChanged();
}

/**
 * @brief Adds one bar's worth of steps to the beat clip.
 *
 * Increases m_steps by TimePos::stepsPerBar() (typically 32), extending
 * the beat pattern by one bar. The new steps are initially empty.
 */
void MidiClip::addSteps()
{
	// Add one bar's worth of steps (typically 32)
	m_steps += TimePos::stepsPerBar();
	// Recalculate clip length to accommodate the additional steps
	updateLength();
	// Notify views to redraw with the extended step grid
	emit dataChanged();
}

/**
 * @brief Duplicates all existing steps by doubling the step count.
 *
 * Doubles m_steps, then copies each active step note from the first half
 * into the corresponding position in the second half. The cloned notes
 * preserve the original's key, length, panning, and volume.
 */
void MidiClip::cloneSteps()
{
	// Remember the current step count before doubling
	int oldLength = m_steps;
	m_steps *= 2; // cloning doubles the track
	// Copy each active step from the first half to the corresponding position in the second half
	for(int i = 0; i < oldLength; ++i )
	{
		// Check if there's an active step note at this position
		Note *toCopy = noteAtStep( i );
		if( toCopy )
		{
			// Enable the corresponding step in the cloned region
			setStep( oldLength + i, true );
			// Get the newly created step note
			Note *newNote = noteAtStep( oldLength + i );
			// Copy all note properties from the original
			newNote->setKey( toCopy->key() );
			newNote->setLength( toCopy->length() );
			newNote->setPanning( toCopy->getPanning() );
			newNote->setVolume( toCopy->getVolume() );
		}
	}
	// Recalculate clip length to reflect the doubled step count
	updateLength();
	// Notify views to redraw with the cloned pattern
	emit dataChanged();
}




/**
 * @brief Removes one bar's worth of steps from the end of the beat clip.
 *
 * Decreases m_steps by TimePos::stepsPerBar() (typically 32), but only
 * if the result would still leave at least one bar of steps. Any active
 * step notes in the removed range are deleted.
 */
void MidiClip::removeSteps()
{
	// Number of steps in one bar (typically 32)
	int n = TimePos::stepsPerBar();
	// Only remove if there would still be steps remaining
	if( n < m_steps )
	{
		// Disable (remove notes from) the steps that are being removed
		for( int i = m_steps - n; i < m_steps; ++i )
		{
			setStep( i, false );
		}
		// Reduce the step count by one bar
		m_steps -= n;
		// Recalculate clip length to reflect the reduced step count
		updateLength();
		// Notify views to redraw with the shorter step grid
		emit dataChanged();
	}
}




/**
 * @brief Creates and returns a new MidiClipView for this clip.
 *
 * Factory method called by the track view system to create the visual
 * representation of this clip in the timeline.
 *
 * @param _tv  The parent TrackView that will contain this clip's view.
 * @return A new MidiClipView instance for this clip.
 */
gui::ClipView * MidiClip::createView( gui::TrackView * _tv )
{
	return new gui::MidiClipView( this, _tv );
}




/**
 * @brief Notifies the PatternStore and PianoRoll that this clip's content changed.
 *
 * If this clip belongs to the PatternStore (beat/bassline editor), tells
 * the PatternStore to update the corresponding pattern track (which may
 * affect playback scheduling). Also refreshes the PianoRoll view if this
 * clip is currently being edited there.
 */
void MidiClip::updatePatternTrack()
{
	// If this clip is in the PatternStore, notify it of the content change
	if (getTrack()->trackContainer() == Engine::patternStore())
	{
		Engine::patternStore()->updatePatternTrack(this);
	}

	// If the PianoRoll is open and displaying this clip, refresh its display
	if (gui::getGUI() != nullptr
		&& gui::getGUI()->pianoRoll()
		&& gui::getGUI()->pianoRoll()->currentMidiClip() == this)
	{
		gui::getGUI()->pianoRoll()->update();
	}
}




/**
 * @brief Returns true if this clip contains no notes with non-zero length.
 *
 * A clip is considered "empty" if all its notes have zero length (which
 * means they are effectively inactive or placeholder notes). Step notes
 * with non-zero length are considered content.
 *
 * @return True if no notes have a non-zero length, false otherwise.
 */
bool MidiClip::empty()
{
	for (const auto& note : m_notes)
	{
		// A note with non-zero length counts as real content
		if (note->length() != 0)
		{
			return false;
		}
	}
	// No notes with non-zero length found — clip is empty
	return true;
}




/**
 * @brief Handles time signature changes by recalculating the beat clip's step count.
 *
 * When the song's time signature changes (e.g. from 4/4 to 3/4), this slot
 * recalculates m_steps to ensure the beat clip covers all existing step notes.
 * It finds the last step note position, rounds up to the next full bar, and
 * converts that to a step count. The result is at least one bar's worth of steps.
 */
void MidiClip::changeTimeSignature()
{
	// Start at the last tick of the first bar
	TimePos last_pos = TimePos::ticksPerBar() - 1;
	for (const auto& note : m_notes)
	{
		// Only consider step notes (negative length indicates a step-type note)
		if (note->length() < 0 && note->pos() > last_pos)
		{
			// Extend last_pos to cover this step note plus one step's width
			last_pos = note->pos() + TimePos::ticksPerBar() / TimePos::stepsPerBar();
		}
	}
	// Round up to the next full bar boundary
	last_pos = last_pos.nextFullBar() * TimePos::ticksPerBar();
	// Convert from bar count to step count, ensuring at least one bar of steps
	m_steps = std::max<tick_t>(TimePos::stepsPerBar(),
				last_pos.getBar() * TimePos::stepsPerBar());
	// Recalculate clip length with the new step count
	updateLength();
}


} // namespace lmms
