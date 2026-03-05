/*
 * PatternEditor.cpp - basic main-window for editing patterns
 *
 * Copyright (c) 2004-2008 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "PatternEditor.h"

#include <QAction>           // For keyboard shortcut actions (pattern navigation)

#include "ClipView.h"        // For BORDER_WIDTH constant used in minimum width calculation
#include "ComboBox.h"        // Pattern selector dropdown widget
#include "DataFile.h"        // For parsing dragged-in track data
#include "embed.h"           // For loading toolbar button icons from embedded resources
#include "Engine.h"          // For Engine::patternStore() used by normalize button
#include "MainWindow.h"      // For saveWidgetState/restoreWidgetState used in settings persistence
#include "PatternStore.h"    // The TrackContainer model that holds all pattern instrument tracks
#include "PatternTrack.h"    // For PatternTrack::findPatternTrack used in cloneClip
#include "Song.h"            // For playback control and Song::addPatternTrack
#include "StringPairDrag.h"  // For decoding drag-and-drop data (track type and serialized XML)
#include "TrackView.h"       // For iterating track views during pattern removal

#include "MidiClip.h"        // For casting clips to MidiClip to call step operations
#include "Track.h"           // For Track::normalizeTrackNames used by normalize button


namespace lmms::gui
{


/**
 * @brief Construct the PatternEditor view for the given PatternStore.
 *
 * Initializes the TrackContainerView base class with the PatternStore
 * and stores a reference to it for direct access to pattern-specific methods.
 */
PatternEditor::PatternEditor(PatternStore* ps) :
	TrackContainerView(ps), // Initialize base class with the PatternStore as its model
	m_ps(ps)                // Store direct reference for pattern-specific operations
{
	setModel(ps); // Set the model on the TrackContainerView (redundant with constructor but explicit)
}




/**
 * @brief Add one bar of steps to all instrument tracks in the current pattern.
 *
 * Delegates to makeSteps(false) which iterates all instrument tracks
 * and calls addSteps() on each one's MidiClip at the current pattern index.
 */
void PatternEditor::addSteps()
{
	makeSteps( false ); // false = add new steps (don't clone existing ones)
}

/**
 * @brief Clone (duplicate) all steps for all instrument tracks in the current pattern.
 *
 * Delegates to makeSteps(true) which iterates all instrument tracks
 * and calls cloneSteps() on each one's MidiClip at the current pattern index.
 */
void PatternEditor::cloneSteps()
{
	makeSteps( true ); // true = clone existing steps (doubles length, copies note data)
}




/**
 * @brief Reset step count to default for all instrument tracks in the current pattern.
 *
 * Iterates all tracks in the PatternStore and, for each InstrumentTrack,
 * resets its MidiClip at the current pattern index to the default number
 * of steps (TimePos::stepsPerBar(), typically 32).
 */
void PatternEditor::resetSteps()
{
	// Get the list of all tracks in the PatternStore
	const TrackContainer::TrackList& tl = model()->tracks();

	for (const auto& track : tl)
	{
		// Only operate on instrument tracks (not sample or automation tracks)
		if (track->type() == Track::Type::Instrument)
		{
			// Get the MidiClip for the currently selected pattern and reset its steps
			auto p = static_cast<MidiClip*>(track->getClip(m_ps->currentPattern()));
			p->resetSteps();
		}
	}
}

/**
 * @brief Remove one bar of steps from all instrument tracks in the current pattern.
 *
 * Iterates all tracks in the PatternStore and, for each InstrumentTrack,
 * removes the last bar's worth of steps from its MidiClip at the current
 * pattern index (if more than one bar of steps remains).
 */
void PatternEditor::removeSteps()
{
	// Get the list of all tracks in the PatternStore
	const TrackContainer::TrackList& tl = model()->tracks();

	for (const auto& track : tl)
	{
		// Only operate on instrument tracks (not sample or automation tracks)
		if (track->type() == Track::Type::Instrument)
		{
			// Get the MidiClip for the currently selected pattern and remove steps
			auto p = static_cast<MidiClip*>(track->getClip(m_ps->currentPattern()));
			p->removeSteps();
		}
	}
}




/**
 * @brief Add a new sample track to the PatternStore.
 *
 * Creates an empty SampleTrack in the PatternStore. The track will
 * automatically get clips created for all existing pattern indices.
 */
void PatternEditor::addSampleTrack()
{
	(void) Track::create( Track::Type::Sample, model() ); // Cast to void: return value unused
}




/**
 * @brief Add a new automation track to the PatternStore.
 *
 * Creates an empty AutomationTrack in the PatternStore. The track will
 * automatically get clips created for all existing pattern indices.
 */
void PatternEditor::addAutomationTrack()
{
	(void) Track::create( Track::Type::Automation, model() ); // Cast to void: return value unused
}




/**
 * @brief Remove clip views for a specific pattern index from all track views.
 *
 * Called when a pattern is deleted. Iterates all TrackViews in the editor
 * and removes the ClipView at the given pattern index from each track's
 * content widget.
 *
 * @param pattern The pattern index whose clip views should be removed
 */
void PatternEditor::removeViewsForPattern(int pattern)
{
	for( TrackView* view : trackViews() )
	{
		view->getTrackContentWidget()->removeClipView(pattern); // Remove the clip view at this index
	}
}



/**
 * @brief Save the Pattern Editor window state (position, size) to XML.
 *
 * Delegates to MainWindow::saveWidgetState to persist the parent window's
 * geometry and visibility state.
 */
void PatternEditor::saveSettings(QDomDocument& doc, QDomElement& element)
{
	MainWindow::saveWidgetState( parentWidget(), element ); // Save parent window geometry
}

/**
 * @brief Restore the Pattern Editor window state from XML.
 *
 * Delegates to MainWindow::restoreWidgetState to restore the parent window's
 * previously saved geometry and visibility state.
 */
void PatternEditor::loadSettings(const QDomElement& element)
{
	MainWindow::restoreWidgetState(parentWidget(), element); // Restore parent window geometry
}




/**
 * @brief Handle drag-and-drop events for tracks being dropped into the editor.
 *
 * When a track is dragged from another editor (e.g. from a preset browser),
 * this creates a new track from the serialized XML data. After creation,
 * it validates that the track has the correct number of clips at the right
 * positions. If not, it recreates them to match the PatternStore's pattern count.
 *
 * Non-track drops are delegated to the base class handler.
 */
void PatternEditor::dropEvent(QDropEvent* de)
{
	// Decode the drag data type and value from the StringPairDrag format
	QString type = StringPairDrag::decodeKey( de );
	QString value = StringPairDrag::decodeValue( de );

	if( type.left( 6 ) == "track_" ) // Check if this is a track drag (prefix "track_")
	{
		// Parse the serialized track XML data
		DataFile dataFile( value.toUtf8() );
		// Create a new track from the XML element in this PatternStore
		Track * t = Track::create( dataFile.content().firstChild().toElement(), model() );

		// Validate that the track has clips at correct positions for all patterns.
		// A valid track should have exactly numOfPatterns clips, each at position (i, 0).
		bool hasValidPatternClips = false;
		if (t->getClips().size() == static_cast<std::size_t>(m_ps->numOfPatterns()))
		{
			hasValidPatternClips = true;
			// Verify each clip is at the expected bar position
			for (auto i = std::size_t{0}; i < t->getClips().size(); ++i)
			{
				if (t->getClips()[i]->startPosition() != TimePos(i, 0))
				{
					hasValidPatternClips = false; // Clip at wrong position
					break;
				}
			}
		}
		if (!hasValidPatternClips)
		{
			// Clips are invalid: delete them all and recreate properly
			t->deleteClips();
			t->createClipsForPattern(m_ps->numOfPatterns() - 1); // Create clips for all patterns
		}
		m_ps->updateAfterTrackAdd(); // Refresh the PatternStore UI

		de->accept(); // Mark the drop event as handled
	}
	else
	{
		TrackContainerView::dropEvent( de ); // Delegate non-track drops to base class
	}
}




/**
 * @brief Handle pattern selection changes by emitting a position update signal.
 *
 * Called when the user selects a different pattern from the combo box.
 * Emits positionChanged to notify track views to scroll to the correct position.
 */
void PatternEditor::updatePosition()
{
	//realignTracks(); // Disabled: tracks are already aligned in fixed-clip mode
	emit positionChanged( m_currentPosition ); // Notify views of the new position
}




/**
 * @brief Internal helper to add or clone steps for all instrument tracks.
 *
 * Iterates all tracks in the PatternStore and, for each InstrumentTrack,
 * either adds new blank steps or clones existing steps on its MidiClip
 * at the current pattern index.
 *
 * @param clone If true, clone existing steps (doubling length and copying data).
 *              If false, add blank steps (extending length by one bar).
 */
void PatternEditor::makeSteps( bool clone )
{
	// Get the list of all tracks in the PatternStore
	const TrackContainer::TrackList& tl = model()->tracks();

	for (const auto& track : tl)
	{
		// Only operate on instrument tracks (sample and automation tracks don't have steps)
		if (track->type() == Track::Type::Instrument)
		{
			// Get the MidiClip at the current pattern index
			auto p = static_cast<MidiClip*>(track->getClip(m_ps->currentPattern()));
			if( clone )
			{
				p->cloneSteps(); // Double the steps and copy note data
			} else
			{
				p->addSteps(); // Add one bar of blank steps
			}
		}
	}
}

/**
 * @brief Clone the current pattern into a new PatternTrack.
 *
 * Creates a duplicate of the current PatternTrack (including all its instrument
 * tracks and clips), then clears the Song Editor clips from the new track since
 * they represent song arrangement data rather than pattern content.
 *
 * After cloning, the editor switches to display the newly created pattern.
 */
// TODO: Avoid repeated code from cloneTrack and clearTrack in TrackOperationsWidget somehow
void PatternEditor::cloneClip()
{
	// Get the PatternStore and current pattern index
	auto ps = static_cast<PatternStore*>(model());
	const int currentPattern = ps->currentPattern();

	// Find the PatternTrack in the Song that corresponds to the current pattern index
	PatternTrack* pt = PatternTrack::findPatternTrack(currentPattern);

	if (pt)
	{
		// Clone the entire PatternTrack (creates a copy with all settings and clips)
		Track* newTrack = pt->clone();
		// Switch the editor to display the newly cloned pattern
		ps->setCurrentPattern(static_cast<PatternTrack*>(newTrack)->patternIndex());

		// Remove the Song Editor clips from the clone. The clone inherits clips that
		// represent the original track's Song arrangement, which is not desired.
		newTrack->lock();       // Lock to prevent concurrent access during modification
		newTrack->deleteClips(); // Remove all Song Editor arrangement clips
		newTrack->unlock();     // Release the lock
	}
}




/**
 * @brief Construct the Pattern Editor window with toolbar and all controls.
 *
 * Sets up:
 * - Window icon and title
 * - Central widget (the PatternEditor track container view)
 * - Drag-and-drop forwarding from toolbar to editor
 * - Pattern selector combo box
 * - Track action buttons (new pattern, clone, add sample/automation tracks)
 * - Step action buttons (reset, remove, add, clone steps)
 * - Keyboard shortcuts (+ and - for pattern navigation)
 *
 * @param ps The PatternStore model to edit
 */
PatternEditorWindow::PatternEditorWindow(PatternStore* ps) :
	Editor(false),                     // false = don't create default record button
	m_editor(new PatternEditor(ps))    // Create the track container view
{
	// Set window appearance
	setWindowIcon(embed::getIconPixmap("pattern_track_btn")); // Icon shown in title bar and taskbar
	setWindowTitle(tr("Pattern Editor"));                      // Window title text
	setCentralWidget(m_editor);                                // Set the editor as the central widget

	// Enable drag-and-drop on both the window and toolbar, forwarding drops to the editor
	setAcceptDrops(true);
	m_toolBar->setAcceptDrops(true);
	connect(m_toolBar, SIGNAL(dragEntered(QDragEnterEvent*)), m_editor, SLOT(dragEnterEvent(QDragEnterEvent*)));
	connect(m_toolBar, SIGNAL(dropped(QDropEvent*)), m_editor, SLOT(dropEvent(QDropEvent*)));

	// Calculate minimum window width based on compact/normal track button mode
	// TODO: Use style sheet instead of hardcoded width calculations
	if (ConfigManager::inst()->value("ui", "compacttrackbuttons").toInt())
	{
		// Compact mode: narrower track operations + settings widgets
		setMinimumWidth(TRACK_OP_WIDTH_COMPACT + DEFAULT_SETTINGS_WIDGET_WIDTH_COMPACT + 2 * ClipView::BORDER_WIDTH + 384);
	}
	else
	{
		// Normal mode: full-width track operations + settings widgets
		setMinimumWidth(TRACK_OP_WIDTH + DEFAULT_SETTINGS_WIDGET_WIDTH + 2 * ClipView::BORDER_WIDTH + 384);
	}

	// Set tooltips for play/stop buttons (inherited from Editor base class)
	m_playAction->setToolTip(tr("Play/pause current pattern (Space)"));
	m_stopAction->setToolTip(tr("Stop playback of current pattern (Space)"));


	// -- Pattern selector toolbar section --
	DropToolBar* patternSelectionToolBar = addDropToolBarToTop(tr("Pattern selector"));

	// Create the pattern selector combo box with a fixed size
	m_patternComboBox = new ComboBox(m_toolBar);
	m_patternComboBox->setFixedSize(200, ComboBox::DEFAULT_HEIGHT);
	m_patternComboBox->setModel(&ps->m_patternComboBoxModel); // Bind to PatternStore's combo model

	patternSelectionToolBar->addWidget(m_patternComboBox);


	// -- Track and step actions toolbar section --
	DropToolBar *trackAndStepActionsToolBar = addDropToolBarToTop(tr("Track and step actions"));


	// Track management actions
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("add_pattern_track"), tr("New pattern"),
						Engine::getSong(), SLOT(addPatternTrack()));       // Create new PatternTrack in Song
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("clone_pattern_track_clip"), tr("Clone pattern"),
						m_editor, SLOT(cloneClip()));                      // Duplicate current pattern
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("add_sample_track"),	tr("Add sample-track"),
						m_editor, SLOT(addSampleTrack()));                 // Add sample track to PatternStore
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("add_automation"), tr("Add automation-track"),
						m_editor, SLOT(addAutomationTrack()));             // Add automation track to PatternStore

	// Spacer widget pushes step actions to the right side of the toolbar
	auto stretch = new QWidget(m_toolBar);
	stretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	trackAndStepActionsToolBar->addWidget(stretch);


	// Normalize instrument track names
	trackAndStepActionsToolBar->addAction(tr("Normalize names"), this, SLOT(normalizeInstrumentTrackNames()));

	// Step management actions (apply to all instrument tracks in the current pattern)
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("step_btn_reset"), tr("Reset steps"), //
						m_editor, SLOT(resetSteps()));                     // Reset all tracks to default step count
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("step_btn_remove"), tr("Remove steps"),
						m_editor, SLOT(removeSteps()));                    // Remove one bar of steps from all tracks
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("step_btn_add"), tr("Add steps"),
						m_editor, SLOT(addSteps()));                       // Add one bar of steps to all tracks
	trackAndStepActionsToolBar->addAction(embed::getIconPixmap("step_btn_duplicate"), tr("Clone Steps"),
						m_editor, SLOT(cloneSteps()));                     // Clone steps for all tracks

	// Connect pattern combo box changes to editor position update
	connect(&ps->m_patternComboBoxModel, SIGNAL(dataChanged()),
			m_editor, SLOT(updatePosition()));

	// Keyboard shortcut: '+' key navigates to next pattern
	auto viewNext = new QAction(this);
	connect(viewNext, SIGNAL(triggered()), m_patternComboBox, SLOT(selectNext()));
	viewNext->setShortcut(Qt::Key_Plus);
	addAction(viewNext);

	// Keyboard shortcut: '-' key navigates to previous pattern
	auto viewPrevious = new QAction(this);
	connect(viewPrevious, SIGNAL(triggered()), m_patternComboBox, SLOT(selectPrevious()));
	viewPrevious->setShortcut(Qt::Key_Minus);
	addAction(viewPrevious);
}


/**
 * @brief Return the preferred size for the Pattern Editor window.
 * @return A QSize slightly wider than the minimum width, with a default height of 300.
 */
QSize PatternEditorWindow::sizeHint() const
{
	return {minimumWidth() + 10, 300};
}


/**
 * @brief Start or toggle playback of the current pattern.
 *
 * If not already in pattern playback mode, starts playing the current pattern.
 * If already playing, toggles pause/resume.
 */
void PatternEditorWindow::play()
{
	if (Engine::getSong()->playMode() != Song::PlayMode::Pattern)
	{
		Engine::getSong()->playPattern(); // Start pattern playback
	}
	else
	{
		Engine::getSong()->togglePause(); // Toggle pause if already playing
	}
}


/**
 * @brief Stop pattern playback.
 */
void PatternEditorWindow::stop()
{
	Engine::getSong()->stop(); // Stop all playback
}


void PatternEditorWindow::normalizeInstrumentTrackNames()
{
	Track::normalizeTrackNames(Engine::patternStore());
}


} // namespace lmms::gui
