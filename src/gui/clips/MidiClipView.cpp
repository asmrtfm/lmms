/*
 * MidiClipView.cpp - implementation of class MidiClipView which displays notes
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

/// @file MidiClipView.cpp
/// @brief Visual representation of a MidiClip in the track content area.
///
/// MidiClipView handles two distinct display modes:
/// - **Beat clips**: Displayed as a row of clickable step buttons in the
///   Pattern Editor (Beat+Bassline editor). Each step can be toggled on/off
///   and its volume adjusted via the mouse wheel.
/// - **Melody clips**: Displayed as a miniature piano roll preview showing
///   a scaled-down rendering of all notes in the clip.
///
/// This class inherits from ClipView, which provides the base functionality
/// for selection, drag-and-drop, resizing, and context menus.

// Own header first (per LMMS code style)
#include "MidiClipView.h"


// System/standard library headers
#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <cmath>

// Project headers
#include "AutomationEditor.h"
#include "ConfigManager.h"
#include "DeprecationHelper.h"
#include "GuiApplication.h"
#include "MidiClip.h"
#include "PianoRoll.h"
#include "RenameDialog.h"
#include "TrackView.h"

namespace lmms::gui
{

// ============================================================================
// Constants
// ============================================================================

/// Vertical pixel offset from the top of the clip widget to where
/// step buttons are drawn in beat clip mode.
constexpr int BeatStepButtonOffset = 4;

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Constructs a MidiClipView for the given MidiClip.
 *
 * Initializes the view with default note colors (white for active notes,
 * grey for muted notes), connects to the piano roll's currentMidiClipChanged
 * signal so the view repaints when the active clip changes, and reads the
 * legacy step-editor pattern display preference from the configuration.
 *
 * @param clip   The MidiClip data model this view represents.
 * @param parent The parent TrackView that owns this clip view.
 */
MidiClipView::MidiClipView( MidiClip* clip, TrackView* parent ) :
	ClipView( clip, parent ),              // Initialize the base ClipView with the clip and parent track view
	m_clip( clip ),                        // Store a direct pointer to the MidiClip for convenient access
	m_paintPixmap(),                       // Initialize the cached paint pixmap (will be created on first paint)
	m_noteFillColor(255, 255, 255, 220),   // Default fill color for active notes: semi-transparent white
	m_noteBorderColor(255, 255, 255, 220), // Default border color for active notes: semi-transparent white
	m_mutedNoteFillColor(100, 100, 100, 220),   // Default fill color for muted notes: semi-transparent grey
	m_mutedNoteBorderColor(100, 100, 100, 220), // Default border color for muted notes: semi-transparent grey
	// TODO if this option is ever added to the GUI, rename it to legacysepattern
	// Read the legacy step-editor pattern display setting from user config.
	// When enabled, beat clips show step buttons in the Song Editor if zoom >= 96 pixels/bar.
	m_legacySEPattern(ConfigManager::inst()->value("ui", "legacysebb", "0").toInt())
{
	// Repaint this clip view whenever the piano roll switches to a different clip,
	// so we can highlight/unhighlight the "current" clip border.
	connect( getGUI()->pianoRoll(), SIGNAL(currentMidiClipChanged()),
			this, SLOT(update()));

	// Trigger an initial repaint to draw the clip contents.
	update();

	// Apply the application-wide widget style (ensures themed rendering).
	setStyle( QApplication::style() );
}




// ============================================================================
// Accessors
// ============================================================================

/**
 * @brief Returns the underlying MidiClip data model for this view.
 *
 * This is a convenience accessor used by other parts of the UI
 * (e.g., the piano roll, transpose dialog) to get the data model.
 *
 * @return Pointer to the MidiClip this view represents.
 */
MidiClip* MidiClipView::getMidiClip()
{
	return m_clip;
}




// ============================================================================
// Public slots and actions
// ============================================================================

/**
 * @brief Updates the tooltip and triggers a visual repaint.
 *
 * Sets the widget tooltip to the clip's current name, then delegates
 * to ClipView::update() which marks the view as needing a repaint
 * and schedules a Qt paint event.
 */
void MidiClipView::update()
{
	// Show the clip name as a tooltip on hover
	setToolTip(m_clip->name());

	// Delegate to the base class, which sets the needsUpdate flag and triggers repaint
	ClipView::update();
}




/**
 * @brief Opens the piano roll editor focused on this clip.
 *
 * Sets this MidiClip as the current clip in the piano roll, makes the
 * piano roll window visible, brings it to the front, and gives it
 * keyboard focus. This is triggered by double-clicking a melody clip
 * or choosing "Open in piano-roll" from the context menu.
 */
void MidiClipView::openInPianoRoll()
{
	auto pRoll = getGUI()->pianoRoll();  // Get the singleton piano roll widget
	pRoll->setCurrentMidiClip(m_clip);   // Set this clip as the one being edited
	pRoll->parentWidget()->show();       // Show the piano roll's parent container (subwindow)
	pRoll->show();                       // Ensure the piano roll widget itself is visible
	pRoll->setFocus();                   // Give keyboard focus to the piano roll
}




/**
 * @brief Sets this clip as a ghost overlay in the piano roll.
 *
 * Ghost notes appear as a semi-transparent overlay in the piano roll,
 * allowing the user to see notes from another clip while editing.
 * This is useful for aligning notes across different instrument tracks.
 */
void MidiClipView::setGhostInPianoRoll()
{
	auto pRoll = getGUI()->pianoRoll();  // Get the singleton piano roll widget
	pRoll->setGhostMidiClip(m_clip);    // Register this clip as the ghost note source
	pRoll->parentWidget()->show();       // Show the piano roll's parent container
	pRoll->show();                       // Ensure the piano roll widget itself is visible
	pRoll->setFocus();                   // Give keyboard focus to the piano roll
}

/**
 * @brief Sets this clip as a ghost overlay in the automation editor.
 *
 * Similar to setGhostInPianoRoll(), but displays the ghost notes in
 * the automation editor instead. This helps users align automation
 * curves with note positions from this clip.
 */
void MidiClipView::setGhostInAutomationEditor()
{
	auto aEditor = getGUI()->automationEditor(); // Get the singleton automation editor widget
	aEditor->setGhostMidiClip(m_clip);           // Register this clip as the ghost note source
	aEditor->parentWidget()->show();              // Show the automation editor's parent container
	aEditor->show();                              // Ensure the automation editor widget is visible
	aEditor->setFocus();                          // Give keyboard focus to the automation editor
}

/**
 * @brief Resets the clip name to an empty string.
 *
 * When the name is empty, the clip will display the track/instrument name
 * instead of a custom label. This is the "Reset name" context menu action.
 */
void MidiClipView::resetName() { m_clip->setName(""); }




/**
 * @brief Opens a rename dialog to let the user change the clip name.
 *
 * Displays a modal RenameDialog pre-filled with the current name.
 * The dialog modifies the string in-place, which is then applied
 * back to the clip. This is the "Change name" context menu action.
 */
void MidiClipView::changeName()
{
	QString s = m_clip->name();     // Get the current clip name
	RenameDialog rename_dlg( s );   // Create a rename dialog, passing the string by reference
	rename_dlg.exec();              // Show the dialog modally; `s` is modified in-place if user confirms
	m_clip->setName( s );           // Apply the (potentially changed) name back to the clip
}




/**
 * @brief Transposes all notes in the selected clips by a user-specified number of semitones.
 *
 * This method operates on the entire selection of clicked clips (not just this one),
 * allowing batch transposition. It first computes the overall key range across all
 * selected clips to determine safe transposition bounds (preventing notes from going
 * below key 0 or above NumKeys-1), then prompts the user for the semitone offset.
 *
 * A journal checkpoint is added per affected track for undo/redo support.
 * This action is only available for melody clips (not beat clips).
 */
void MidiClipView::transposeSelection()
{
	// Get all currently selected/clicked clip views (supports multi-selection)
	const auto selection = getClickedClips();

	// Calculate the key boundaries for all clips in the selection
	// to determine the valid transposition range
	int highest = 0;             // Highest note key found across all selected clips
	int lowest = NumKeys - 1;   // Lowest note key found across all selected clips
	for (ClipView* clipview: selection)
	{
		// Only process MidiClipViews (skip other clip types if any)
		if (auto mcv = qobject_cast<MidiClipView*>(clipview))
		{
			// boundsForNotes returns an optional struct with lowest/highest keys
			if (auto bounds = boundsForNotes(mcv->getMidiClip()->notes()))
			{
				lowest = std::min(bounds->lowest, lowest);    // Track the global minimum key
				highest = std::max(bounds->highest, highest); // Track the global maximum key
			}
		}
	}

	// Show an input dialog asking for the number of semitones to transpose.
	// The min/max bounds are computed to prevent any note from going out of range.
	int semitones = QInputDialog::getInt(this, tr("Transpose"), tr("Semitones to transpose by:"),
		/*start*/ 0, /*min*/ -lowest, /*max*/ (NumKeys - 1 - highest));

	// If the user selected 0 semitones (or cancelled), do nothing
	if (semitones == 0) { return; }

	// TODO make this not crash
	// Engine::getSong()->addJournalCheckPoint();

	// Track which tracks have already had journal checkpoints added,
	// to avoid adding duplicate checkpoints for the same track
	QSet<Track*> m_changedTracks;
	for (ClipView* clipview: selection)
	{
		// Attempt to cast each clip view to a MidiClipView
		auto mcv = qobject_cast<MidiClipView*>(clipview);
		if (!mcv) { continue; } // Skip non-MIDI clip views

		auto clip = mcv->getMidiClip();
		if (clip->notes().empty()) { continue; } // Skip empty clips

		// Add a journal checkpoint for the track (for undo/redo) if we haven't already
		auto track = clipview->getTrackView()->getTrack();
		if (!m_changedTracks.contains(track))
		{
			track->addJournalCheckPoint();     // Save state for undo
			m_changedTracks.insert(track);     // Mark this track as checkpointed
		}

		// Transpose every note in the clip by the specified number of semitones
		for (Note* note: clip->notes())
		{
			note->setKey(note->key() + semitones);
		}
		// Notify listeners (e.g., piano roll) that the clip's data has changed
		emit clip->dataChanged();
	}
	// At least one clip must have notes to show the transpose dialog, so something *has* changed
	Engine::getSong()->setModified(); // Mark the song as modified (unsaved changes)
}




// ============================================================================
// Context menu
// ============================================================================

/**
 * @brief Builds the right-click context menu for this MidiClipView.
 *
 * Adds clip-type-specific actions to the context menu provided by ClipView.
 * Common actions include opening in piano roll, setting as ghost, clearing notes,
 * transposing, and renaming. Beat clips additionally get step manipulation
 * actions (reset, add, remove, clone steps).
 *
 * @param _cm The QMenu to populate with context menu actions.
 */
void MidiClipView::constructContextMenu( QMenu * _cm )
{
	// Determine if this is a beat clip (step sequencer) or melody clip
	bool isBeat = m_clip->type() == MidiClip::Type::BeatClip;

	// --- "Open in piano-roll" action (inserted at the top of the menu) ---
	auto a = new QAction(embed::getIconPixmap("piano"), tr("Open in piano-roll"), _cm);
	_cm->insertAction( _cm->actions()[0], a ); // Insert before the first existing action
	connect( a, SIGNAL(triggered(bool)),
					this, SLOT(openInPianoRoll()));

	// --- "Set as ghost in piano-roll" action ---
	auto b = new QAction(embed::getIconPixmap("ghost_note"), tr("Set as ghost in piano-roll"), _cm);
	if( m_clip->empty() ) { b->setEnabled( false ); } // Disable if the clip has no notes
	_cm->insertAction( _cm->actions()[1], b );         // Insert as second action
	connect( b, SIGNAL(triggered(bool)),
					this, SLOT(setGhostInPianoRoll()));

	// --- "Set as ghost in automation editor" action ---
	auto c = new QAction(embed::getIconPixmap("automation_ghost_note"), tr("Set as ghost in automation editor"), _cm);
	if (m_clip->empty()) { c->setEnabled(false); } // Disable if the clip has no notes
	_cm->insertAction(_cm->actions()[2], c);        // Insert as third action
	connect(c, &QAction::triggered, this, &MidiClipView::setGhostInAutomationEditor);

	// Add separators to visually group the actions
	_cm->insertSeparator(_cm->actions()[3]); // Separator after the ghost actions
	_cm->addSeparator();                     // Separator before the edit actions

	// --- "Clear all notes" action: removes every note from the clip ---
	_cm->addAction( embed::getIconPixmap( "edit_erase" ),
			tr( "Clear all notes" ), m_clip, SLOT(clear()));

	// --- "Transpose" action: only available for melody clips (not beat clips) ---
	if (!isBeat)
	{
		_cm->addAction(embed::getIconPixmap("scale"), tr("Transpose"), this, &MidiClipView::transposeSelection);
	}
	_cm->addSeparator();

	// --- "Reset name" action: clears the custom clip name ---
	_cm->addAction( embed::getIconPixmap( "reload" ), tr( "Reset name" ),
						this, SLOT(resetName()));
	// --- "Change name" action: opens a rename dialog ---
	_cm->addAction( embed::getIconPixmap( "edit_rename" ),
						tr( "Change name" ),
						this, SLOT(changeName()));

	// --- Beat clip-specific step manipulation actions ---
	if (isBeat)
	{
		_cm->addSeparator();

		// "Reset steps" resets the step count back to the default value
		_cm->addAction( embed::getIconPixmap( "step_btn_reset" ),
		  tr( "Reset steps" ), m_clip, SLOT(resetSteps()));
		// "Add steps" appends additional steps to the beat pattern
		_cm->addAction( embed::getIconPixmap( "step_btn_add" ),
			tr( "Add steps" ), m_clip, SLOT(addSteps()));
		// "Remove steps" removes steps from the end of the beat pattern
		_cm->addAction( embed::getIconPixmap( "step_btn_remove" ),
			tr( "Remove steps" ), m_clip, SLOT(removeSteps()));
		// "Clone Steps" duplicates the current step pattern (appends a copy)
		_cm->addAction( embed::getIconPixmap( "step_btn_duplicate" ),
			tr( "Clone Steps" ), m_clip, SLOT(cloneSteps()));
	}
}




// ============================================================================
// Mouse and input event handlers
// ============================================================================

/**
 * @brief Handles mouse press events for step toggling and delegation.
 *
 * In beat clip mode (when step buttons are displayed), a left-click on a step
 * button toggles that step on or off. If the step has no note, one is created;
 * if a note exists, it is deactivated. The piano roll is also updated if it
 * is currently showing this clip.
 *
 * For melody clips or clicks outside the step button area, the event is
 * delegated to ClipView::mousePressEvent() for standard clip interactions
 * (selection, dragging, resizing, etc.).
 *
 * @param _me The mouse press event.
 */
void MidiClipView::mousePressEvent( QMouseEvent * _me )
{
	// Determine whether step buttons should be displayed:
	// - fixedClips() is true in the Pattern Editor (BB editor), always show steps
	// - In the Song Editor, only show steps if zoom >= 96 px/bar AND legacy mode is on
	bool displayPattern = fixedClips() || (pixelsPerBar() >= 96 && m_legacySEPattern);

	// Check if this is a left-click on a step button in a beat clip
	if (_me->button() == Qt::LeftButton && m_clip->m_clipType == MidiClip::Type::BeatClip && displayPattern
		&& _me->y() > BeatStepButtonOffset && _me->y() < BeatStepButtonOffset + m_stepBtnOff.height())

	// when mouse button is pressed in pattern mode

	{
//	get the step number that was clicked on and
//	do calculations in floats to prevent rounding errors...
		// Convert the x-coordinate of the click to a step index.
		// The clickable area spans from BORDER_WIDTH to (width - BORDER_WIDTH),
		// divided evenly among m_steps steps.
		float tmp = ( ( float(_me->x()) - BORDER_WIDTH ) *
				float( m_clip -> m_steps ) ) / float(width() - BORDER_WIDTH*2);

		int step = int( tmp ); // Truncate to get the integer step index

//	debugging to ensure we get the correct step...
//		qDebug( "Step (%f) %d", tmp, step );

		// Safety check: ensure the computed step index is within valid range
		if( step >= m_clip->m_steps )
		{
			qDebug( "Something went wrong in clip.cpp: step %d doesn't exist in clip!", step );
			return;
		}

		// Look up whether a note already exists at this step
		Note * n = m_clip->noteAtStep( step );

		if( n == nullptr )
		{
			// No note at this step: create a new active step note
			m_clip->addStepNote( step );
		}
		else // note at step found
		{
			// Note exists: save state for undo and deactivate the step
			m_clip->addJournalCheckPoint();
			m_clip->setStep( step, false );
		}

		Engine::getSong()->setModified(); // Mark the song as having unsaved changes
		update();                         // Repaint this clip view to reflect the change

		// If the piano roll is currently displaying this clip, update it too
		if( getGUI()->pianoRoll()->currentMidiClip() == m_clip )
		{
			getGUI()->pianoRoll()->update();
		}
	}
	else

	// if not in pattern mode, let parent class handle the event

	{
		// Delegate to ClipView for standard clip interactions
		// (selection, dragging, context menu, etc.)
		ClipView::mousePressEvent( _me );
	}
}

/**
 * @brief Handles double-click events to open the piano roll.
 *
 * A left double-click on a melody clip (or a beat clip when step buttons
 * are not displayed) opens the piano roll editor for this clip.
 * Non-left-button double-clicks are ignored.
 *
 * @param _me The mouse double-click event.
 */
void MidiClipView::mouseDoubleClickEvent(QMouseEvent *_me)
{
	// Only respond to left-button double-clicks
	if( _me->button() != Qt::LeftButton )
	{
		_me->ignore(); // Let the event propagate to parent widgets
		return;
	}
	// Open piano roll for melody clips, or for beat clips when not in fixed/step mode
	if( m_clip->m_clipType == MidiClip::Type::MelodyClip || !fixedClips() )
	{
		openInPianoRoll();
	}
}




/**
 * @brief Handles mouse wheel events for adjusting step note volume.
 *
 * In beat clip mode when step buttons are displayed, scrolling the mouse
 * wheel over a step adjusts its volume by +/- 5 per wheel tick. If the
 * step has no note and the wheel scrolls up, a new note is created at
 * volume 0 and then incremented. Volume is clamped to the [0, 100] range.
 *
 * For clicks outside the step button area or non-beat clips, the event
 * is delegated to ClipView::wheelEvent().
 *
 * @param we The mouse wheel event.
 */
void MidiClipView::wheelEvent(QWheelEvent * we)
{
	// Check if we are in beat clip mode with step buttons displayed,
	// and the wheel event is in the lower portion (step button area)
	if(m_clip->m_clipType == MidiClip::Type::BeatClip &&
				(fixedClips() || pixelsPerBar() >= 96) &&
				position(we).y() > height() - m_stepBtnOff.height())
	{
//	get the step number that was wheeled on and
//	do calculations in floats to prevent rounding errors...
		// Convert x-coordinate to a step index (same calculation as in mousePressEvent)
		float tmp = ((float(position(we).x()) - BORDER_WIDTH) *
				float(m_clip -> m_steps)) / float(width() - BORDER_WIDTH*2);

		int step = int( tmp ); // Truncate to integer step index

		// Bail out if the step is out of range
		if( step >= m_clip->m_steps )
		{
			return;
		}

		// Get the note at this step (may be nullptr if the step is inactive)
		Note * n = m_clip->noteAtStep( step );

		// Determine scroll direction, accounting for inverted scroll (e.g., natural scrolling)
		const int direction = (we->angleDelta().y() > 0 ? 1 : -1) * (we->inverted() ? -1 : 1);

		// If scrolling up on an empty step, create a new note starting at volume 0
		if(!n && direction > 0)
		{
			n = m_clip->addStepNote( step );
			n->setVolume( 0 ); // Start at 0 so the first increment brings it to 5
		}

		// Adjust the volume of the note if one exists
		if( n != nullptr )
		{
			int vol = n->getVolume(); // Get the current volume
			if(direction > 0)
			{
				// Scroll up: increase volume by 5, capped at 100
				n->setVolume( qMin( 100, vol + 5 ) );
			}
			else
			{
				// Scroll down: decrease volume by 5, floored at 0
				n->setVolume( qMax( 0, vol - 5 ) );
			}

			Engine::getSong()->setModified(); // Mark song as modified
			update();                         // Repaint this clip view

			// Update the piano roll if it's displaying this clip
			if( getGUI()->pianoRoll()->currentMidiClip() == m_clip )
			{
				getGUI()->pianoRoll()->update();
			}
		}
		we->accept(); // Mark the event as handled so it doesn't propagate
	}
	else
	{
		// Not in the step button area or not a beat clip: delegate to base class
		ClipView::wheelEvent(we);
	}
}


// ============================================================================
// Painting
// ============================================================================

/**
 * @brief Computes the inclusive range of note keys.
 *
 * Given the minimum and maximum MIDI key values, returns how many
 * distinct keys are spanned (inclusive of both endpoints).
 *
 * @param minKey The lowest MIDI key value.
 * @param maxKey The highest MIDI key value.
 * @return The number of keys in the range [minKey, maxKey].
 */
static int computeNoteRange(int minKey, int maxKey)
{
	return (maxKey - minKey) + 1;
}

/**
 * @brief Paints the visual representation of the MidiClip.
 *
 * This is the main rendering method. It uses a cached pixmap (m_paintPixmap)
 * to avoid redundant repainting. The rendering has three distinct paths:
 *
 * 1. **Beat clip with step buttons**: Draws a row of step button pixmaps,
 *    each showing on/off state and volume (via opacity blending).
 * 2. **Melody clip / Beat clip in Song Editor**: Draws a miniature piano roll
 *    visualization with each note rendered as a small rectangle or line.
 * 3. **Common elements**: Bar lines, clip name text label, inner/outer borders,
 *    and the muted icon overlay.
 *
 * @param pe The paint event (unused; the entire widget is always repainted).
 */
void MidiClipView::paintEvent( QPaintEvent * )
{
	QPainter painter( this ); // Painter for the actual widget surface

	// If no repaint is needed, draw the cached pixmap and return early
	if( !needsUpdate() )
	{
		painter.drawPixmap( 0, 0, m_paintPixmap );
		return;
	}

	// Clear the update flag since we are about to repaint
	setNeedsUpdate( false );

	// Allocate or resize the cached pixmap if necessary
	if (m_paintPixmap.isNull() || m_paintPixmap.size() != size())
	{
		m_paintPixmap = QPixmap(size());
	}

	// Paint to the offscreen pixmap for caching
	QPainter p( &m_paintPixmap );

	QColor c; // The base color for the clip background
	bool const muted = m_clip->getTrack()->isMuted() || m_clip->isMuted(); // True if track or clip is muted
	bool current = getGUI()->pianoRoll()->currentMidiClip() == m_clip;     // True if this clip is active in piano roll
	bool beatClip = m_clip->m_clipType == MidiClip::Type::BeatClip;        // True if this is a beat/step-sequencer clip

	if( beatClip )
	{
		// Beat clips use a dedicated theme color from the patternClipBackground QProperty
		// Do not paint PatternClips how we paint MidiClips
		c = patternClipBackground();
	}
	else
	{
		// Melody clips use the clip's custom color or a default derived from the background
		c = getColorForDisplay( painter.background().color() );
	}

	// Create a vertical gradient for the clip background.
	// Beat clips have the darker end at top; melody clips have it at bottom.
	// invert the gradient for the background in the B&B editor
	QLinearGradient lingrad( 0, 0, 0, height() );
	lingrad.setColorAt( beatClip ? 0 : 1, c.darker( 300 ) ); // Darker end
	lingrad.setColorAt( beatClip ? 1 : 0, c );                // Lighter end

	// Paint a solid black rectangle first to prevent visual glitches
	// when the clip background color has transparency
	// paint a black rectangle under the clip to prevent glitches with transparent backgrounds
	p.fillRect( rect(), QColor( 0, 0, 0 ) );

	// Fill with either the gradient or the flat color, depending on theme setting
	if( gradient() )
	{
		p.fillRect( rect(), lingrad );
	}
	else
	{
		p.fillRect( rect(), c );
	}

	// --- Text box height computation ---
	// We need to know the text label height ahead of time so notes can be
	// painted underneath it (offset downward by this amount).
	// Check whether we will paint a text box and compute its potential height
	// This is needed so we can paint the notes underneath it.
	bool const drawName = !m_clip->name().isEmpty();            // True if the clip has a custom name
	bool const drawTextBox = !beatClip && drawName;             // Only melody clips show a text label

	// TODO Warning! This might cause problems if ClipView::paintTextLabel changes
	int textBoxHeight = 0;
	const int textTop = BORDER_WIDTH + 1; // Top padding for the text label
	if (drawTextBox)
	{
		// Measure the font to determine the text label height
		QFont labelFont = this->font();
		labelFont.setHintingPreference( QFont::PreferFullHinting );

		QFontMetrics fontMetrics(labelFont);
		textBoxHeight = fontMetrics.height() + 2 * textTop; // Font height + vertical padding
	}

	// --- Compute scaling factors for note positioning ---

	// Compute pixels per bar for this clip view
	const int baseWidth = fixedClips() ? parentWidget()->width() - 2 * BORDER_WIDTH
						: width() - BORDER_WIDTH;
	const float pixelsPerBar = baseWidth / (float) m_clip->length().getBar();

	// Length of one bar and one tick in the normalized [0,1] x [0,1] coordinate system
	// used for drawing notes in the melody clip renderer
	// Length of one bar/beat in the [0,1] x [0,1] coordinate system
	const float barLength = 1. / m_clip->length().getBar();
	const float tickLength = barLength / TimePos::ticksPerBar();

	// X origin for drawing (accounts for the left border)
	const int x_base = BORDER_WIDTH;

	// Determine whether to display step buttons (beat clip mode)
	// fixedClips() = true in the Pattern Editor; legacy mode requires sufficient zoom
	bool displayPattern = fixedClips() || (pixelsPerBar >= 96 && m_legacySEPattern);

	// Reference to the note collection for convenient access
	NoteVector const & noteCollection = m_clip->m_notes;

	// ========================================================================
	// PATH 1: Beat clip with step button display (Pattern Editor view)
	// ========================================================================
	// Beat clip paint event (on BB Editor)
	if (beatClip && displayPattern)
	{
		// Step button pixmaps to be scaled to fit the clip width
		QPixmap stepon0;      // Step-on base layer (volume = 0)
		QPixmap stepon200;    // Step-on overlay layer (volume = 200, blended by opacity)
		QPixmap stepoff;      // Step-off for even groups of 4
		QPixmap stepoffl;     // Step-off (lighter) for odd groups of 4

		const int steps = std::max(1, m_clip->m_steps); // Total number of steps (at least 1)
		const int w = width() - 2 * BORDER_WIDTH;       // Available width for step buttons

		// Scale each step button pixmap to fit evenly within the clip width.
		// Height is preserved; width is divided equally among all steps.
		// scale step graphics to fit the beat clip length
		stepon0
			= m_stepBtnOn0.scaled(w / steps, m_stepBtnOn0.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		stepon200 = m_stepBtnOn200.scaled(
			w / steps, m_stepBtnOn200.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		stepoff
			= m_stepBtnOff.scaled(w / steps, m_stepBtnOff.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		stepoffl = m_stepBtnOffLight.scaled(
			w / steps, m_stepBtnOffLight.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

		// Iterate over every step and draw the appropriate button graphic
		for (int it = 0; it < steps; it++)	// go through all the steps in the beat clip
		{
			// Check if there is an active note at this step
			Note* n = m_clip->noteAtStep(it);

			// figure out x and y coordinates for step graphic
			const int x = BORDER_WIDTH + static_cast<int>(it * w / steps); // Horizontal position
			const int y = BeatStepButtonOffset;                             // Vertical position (constant offset)

			if (n)
			{
				// Active step: draw the light background, then the base "on" graphic,
				// then overlay the volume-dependent "on" graphic with opacity proportional
				// to the square root of (volume / 200). This gives a visual volume indicator.
				const int vol = n->getVolume();
				p.drawPixmap(x, y, stepoffl);                   // Light background layer
				p.drawPixmap(x, y, stepon0);                    // Base "on" layer (always fully opaque)
				p.setOpacity(std::sqrt(vol / 200.0));           // Scale opacity by sqrt(volume/200)
				p.drawPixmap(x, y, stepon200);                  // Volume-tinted overlay
				p.setOpacity(1);                                // Reset opacity for subsequent drawing
			}
			else if ((it / 4) % 2)
			{
				// Inactive step in an odd group of 4: use the lighter "off" pixmap
				// This creates a visual grouping pattern (groups of 4 steps alternate shading)
				p.drawPixmap(x, y, stepoffl);
			}
			else
			{
				// Inactive step in an even group of 4: use the standard "off" pixmap
				p.drawPixmap(x, y, stepoff);
			}
		} // end for loop

		// If the clip or its track is muted, draw a semi-transparent overlay
		// draw a transparent rectangle over muted clips
		if (muted)
		{
			p.setBrush(mutedBackgroundColor());
			p.setOpacity(0.5);
			p.drawRect(0, 0, width(), height());
		}
	}
	// ========================================================================
	// PATH 2: Melody clip or beat clip shown in the Song Editor
	// Renders a miniature piano roll preview of all notes
	// ========================================================================
	// Melody clip and Beat clip (on Song Editor) paint event
	else if
	(
		!noteCollection.empty() &&
			(m_clip->m_clipType == MidiClip::Type::MelodyClip ||
			m_clip->m_clipType == MidiClip::Type::BeatClip)
	)
	{
		// --- Determine the vertical range of notes to display ---
		// Compute the minimum and maximum key in the clip
		// so that we know how much there is to draw.
		int maxKey = std::numeric_limits<int>::min(); // Will hold the highest note key
		int minKey = std::numeric_limits<int>::max(); // Will hold the lowest note key

		// Scan all notes to find the key range
		for (Note const * note : noteCollection)
		{
			int const key = note->key();
			maxKey = qMax( maxKey, key ); // Track the highest key
			minKey = qMin( minKey, key ); // Track the lowest key
		}

		// Ensure a minimum display range of one octave (12 semitones)
		// so that single-note clips don't look squished or invisible
		// If needed adjust the note range so that we always have paint a certain interval
		int const minimalNoteRange = 12; // Always paint at least one octave
		int const actualNoteRange = computeNoteRange(minKey, maxKey);

		if (actualNoteRange < minimalNoteRange)
		{
			// Distribute the extra range evenly above and below the actual notes
			int missingNumberOfNotes = minimalNoteRange - actualNoteRange;
			minKey = std::max(0, minKey - missingNumberOfNotes / 2);  // Expand downward (clamped at 0)
			maxKey = maxKey + missingNumberOfNotes / 2;                // Expand upward
			if (missingNumberOfNotes % 2 == 1)
			{
				// If odd number of missing notes, add the extra one at the top
				// Put more range at the top to bias drawing towards the bottom
				++maxKey;
			}
		}

		// Recompute the range after adjustment
		int const adjustedNoteRange = computeNoteRange(minKey, maxKey);

		// --- Compute vertical offset to position notes below the text label ---

		// Start with the text box height as the distance from the top
		// Transform such that [0, 1] x [0, 1] paints in the correct area
		float distanceToTop = textBoxHeight;

		// Smoothly transition the text offset based on widget height.
		// When the track is very short, notes are drawn from the very top (no offset).
		// When the track is tall enough, notes start below the text label.
		// In between, the offset is interpolated linearly.
		// This moves the notes smoothly under the text
		int widgetHeight = height();
		int fullyAtTopAtLimit = MINIMAL_TRACK_HEIGHT;       // Below this height, no text offset
		int fullyBelowAtLimit = 4 * fullyAtTopAtLimit;      // Above this height, full text offset
		if (widgetHeight <= fullyBelowAtLimit)
		{
			if (widgetHeight <= fullyAtTopAtLimit)
			{
				// Very short track: draw notes from the very top
				distanceToTop = 0;
			}
			else
			{
				// Interpolate between 0 and textBoxHeight based on widget height
				float const a = 1. / (fullyAtTopAtLimit - fullyBelowAtLimit);
				float const b = - float(fullyBelowAtLimit) / (fullyAtTopAtLimit - fullyBelowAtLimit);
				float const scale = a * widgetHeight + b;          // Linear interpolation factor [0, 1]
				distanceToTop = (1. - scale) * textBoxHeight;      // Scale the text offset
			}
		}

		int const notesBorder = 4; // Border padding (in pixels) at top and bottom of note area

		// --- Begin drawing notes using a normalized coordinate system ---
		// The relavant painting code starts here
		p.save(); // Save the painter state so we can restore it after the transform

		// Transform the painter so that coordinates [0,1] x [0,1] map to the note area.
		// Translate down by the text offset + border, then scale to fill the remaining space.
		p.translate(0., distanceToTop + notesBorder);
		p.scale(width(), height() - distanceToTop - 2 * notesBorder);

		// Choose note colors based on mute state and clip color brightness
		// set colour based on mute status
		QColor noteFillColor = muted ? getMutedNoteFillColor().lighter(200)
									 : (c.lightness() > 175 ? getNoteFillColor().darker(400) : getNoteFillColor());
		QColor noteBorderColor = muted ? getMutedNoteBorderColor()
									   : (hasCustomColor() ? c.lighter(200) : getNoteBorderColor());

		// For very short tracks (< 64px tall), draw notes as horizontal lines
		// instead of filled rectangles for better visibility at small sizes
		bool const drawAsLines = height() < 64;
		if (drawAsLines)
		{
			p.setPen(noteFillColor); // Lines use the fill color
		}
		else
		{
			p.setPen(noteBorderColor);                   // Rectangles use the border color for outlines
			p.setRenderHint(QPainter::Antialiasing);     // Enable antialiasing for smoother rectangles
		}

		// Explicitly set pen width to 0 (cosmetic pen: always 1 pixel regardless of transform).
		// Needed for Qt5 although the documentation for QPainter::setPen(QColor) as it's used above
		// states that it should already set a width of 0.
		QPen pen = p.pen();
		pen.setWidth(0);
		p.setPen(pen);

		// Height of one note in the normalized [0, 1] vertical space
		float const noteHeight = 1. / adjustedNoteRange;

		// --- Draw each note as a rectangle or line ---
		// scan through all the notes and draw them on the clip
		for (Note const * currentNote : noteCollection)
		{
			// Map the note's MIDI key to a 0-based index within the displayed range
			// Map to 0, 1, 2, ...
			int mappedNoteKey = currentNote->key() - minKey;
			// Invert so that higher notes appear at the top (y=0) of the widget
			int invertedMappedNoteKey = adjustedNoteRange - mappedNoteKey - 1;

			// Compute horizontal position and width in normalized coordinates
			float const noteStartX = currentNote->pos() * tickLength;    // Start position (0 to 1)
			float const noteLength = currentNote->length() * tickLength; // Duration (in normalized units)

			// Compute vertical position in normalized coordinates
			float const noteStartY = invertedMappedNoteKey * noteHeight;

			// Create the note rectangle in normalized coordinates
			QRectF noteRectF( noteStartX, noteStartY, noteLength, noteHeight);
			if (drawAsLines)
			{
				// Draw as a horizontal line through the center of the note's vertical slot
				p.drawLine(QPointF(noteStartX, noteStartY + 0.5 * noteHeight),
					   QPointF(noteStartX + noteLength, noteStartY + 0.5 * noteHeight));
			}
			else
			{
				// Draw as a filled rectangle with a border
				p.fillRect( noteRectF, noteFillColor );
				p.drawRect( noteRectF );
			}
		}

		p.restore(); // Restore the painter state (undo the translate/scale transform)
	}

	// ========================================================================
	// Common drawing: bar lines, text label, borders, muted icon
	// ========================================================================

	// --- Draw bar separator lines ---
	// Short tick marks at the top and bottom edges of the clip to indicate bar boundaries
	// bar lines
	const int lineSize = 3; // Length of bar tick marks in pixels
	p.setPen( c.darker( 200 ) ); // Use a darker shade of the clip color

	for( bar_t t = 1; t < m_clip->length().getBar(); ++t )
	{
		// Draw a tick mark at the top edge
		p.drawLine( x_base + static_cast<int>( pixelsPerBar * t ) - 1,
				BORDER_WIDTH, x_base + static_cast<int>(
						pixelsPerBar * t ) - 1, BORDER_WIDTH + lineSize );
		// Draw a tick mark at the bottom edge
		p.drawLine( x_base + static_cast<int>( pixelsPerBar * t ) - 1,
				rect().bottom() - ( lineSize + BORDER_WIDTH ),
				x_base + static_cast<int>( pixelsPerBar * t ) - 1,
				rect().bottom() - BORDER_WIDTH );
	}

	// --- Draw the clip name text label (melody clips only) ---
	// clip name
	if (drawTextBox)
	{
		paintTextLabel(m_clip->name(), p); // Delegates to ClipView::paintTextLabel()
	}

	// --- Draw inner and outer borders (not for beat clips in the Pattern Editor) ---
	if( !( fixedClips() && beatClip ) )
	{
		// Inner border: lighter shade, even brighter if this is the "current" clip in piano roll
		// inner border
		p.setPen( c.lighter( current ? 160 : 130 ) );
		p.drawRect( 1, 1, rect().right() - BORDER_WIDTH,
			rect().bottom() - BORDER_WIDTH );

		// Outer border: brighter if current, darker otherwise
		// outer border
		p.setPen( current ? c.lighter( 130 ) : c.darker( 300 ) );
		p.drawRect( 0, 0, rect().right(), rect().bottom() );
	}

	// --- Draw the muted icon if the clip itself is muted ---
	// (Only shows if the clip was manually muted, not just the track)
	// draw the 'muted' pixmap only if the clip was manually muted
	if( m_clip->isMuted() )
	{
		const int spacing = BORDER_WIDTH;  // Offset from the left/bottom edges
		const int size = 14;               // Icon dimensions (14x14 pixels)
		// Draw the muted speaker icon in the bottom-left corner
		p.drawPixmap( spacing, height() - ( size + spacing ),
			embed::getIconPixmap( "muted", size, size ) );
	}

	// Finally, blit the completed offscreen pixmap to the actual widget surface
	painter.drawPixmap( 0, 0, m_paintPixmap );
}


} // namespace lmms::gui
