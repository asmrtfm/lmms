/*
 * TrackOperationsWidget.cpp - implementation of TrackOperationsWidget class
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

// Own header first per LMMS include order convention
#include "TrackOperationsWidget.h"

// System/Qt headers
#include <cstdio>

#include <QBoxLayout>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QMenu>
#include <cstdio>

#include <QBoxLayout>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>

// Project headers (alphabetical order)
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrackView.h"
#include "Clip.h"
#include "ColorChooser.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "embed.h"
#include "Engine.h"
#include "FileDialog.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "InstrumentTrackView.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PatternTrackView.h"
#include "PixmapButton.h"
#include "Song.h"
#include "StringPairDrag.h"
#include "Track.h"
#include "TrackContainerView.h"
#include "TrackGrip.h"
#include "TrackView.h"

namespace lmms::gui
{

// =========================================================================
// Construction
// =========================================================================

/**
 * @brief Construct a TrackOperationsWidget for the given TrackView.
 *
 * This widget sits on the left side of every track and contains:
 * - A TrackGrip for drag-reordering the track
 * - A gear button (QPushButton) that opens a context menu with track-specific actions
 * - Mute and Solo toggle buttons (PixmapButtons wrapped in QWidgets for layout stability)
 *
 * The context menu is lazily populated each time it is shown, via the updateMenu() slot
 * connected to QMenu::aboutToShow.
 *
 * @param parent The TrackView that owns this widget.
 */
TrackOperationsWidget::TrackOperationsWidget( TrackView * parent ) :
	QWidget( parent ),             /*!< Initialize QWidget with TrackView as the Qt parent */
	m_trackView( parent )          /*!< Store a reference to the owning TrackView */
{
	// Set tooltip instructing user how to initiate a drag-and-drop copy of the track
	setToolTip(tr("Press <%1> while clicking on move-grip "
				"to begin a new drag'n'drop action." ).arg(UI_CTRL_KEY) );

	// Create the context menu for the gear button; it is populated on-demand via aboutToShow
	auto toMenu = new QMenu(this);
	connect( toMenu, SIGNAL(aboutToShow()), this, SLOT(updateMenu()));


	// Set the object name for QSS (Qt Style Sheet) styling purposes
	setObjectName( "automationEnabled" );

	// Create the main horizontal layout for this widget (grip on left, buttons on right)
	auto layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0); // No margins around the layout
	layout->setSpacing(0);                   // No spacing between layout items
	layout->setAlignment(Qt::AlignTop);      // Align items to the top of the widget

	// Create and add the TrackGrip (the drag handle on the far left used to reorder tracks)
	m_trackGrip = new TrackGrip(m_trackView->getTrack(), this);
	layout->addWidget(m_trackGrip);

	// Create a sub-widget to hold the gear icon, mute button, and solo button in their own layout
	auto operationsWidget = new QWidget(this);
	auto operationsLayout = new QHBoxLayout(operationsWidget);
	operationsLayout->setContentsMargins(0, 0, 0, 0); // No margins inside the operations area
	operationsLayout->setSpacing(0);                   // No spacing between operations buttons

	// Create the gear button (QPushButton) that triggers the context menu when clicked
	m_trackOps = new QPushButton(operationsWidget);
	m_trackOps->setFocusPolicy( Qt::NoFocus );  // Prevent the button from stealing keyboard focus
	m_trackOps->setMenu( toMenu );               // Attach the context menu to this button
	m_trackOps->setToolTip(tr("Actions"));       // Tooltip for the gear button

	// This helper lambda wraps a PixmapButton in a QWidget. This is necessary due to some strange effect where the
	// PixmapButtons are resized to a size that's larger than their minimum/fixed size when the method "show" is called
	// in "TrackContainerView::realignTracks". Specifically, with the default theme the buttons are resized from
	// (16, 14) to (26, 26). This then makes them behave not as expected in layouts.
	// The resizing is not done for QWidgets. Therefore we wrap the PixmapButton in a QWidget which is set to a
	// fixed size that will be able to show the active and inactive pixmap. We can then use the QWidget in layouts
	// without any disturbances.
	//
	// The resizing only seems to affect the track view hierarchy and is triggered by Qt's internal mechanisms.
	// For example the buttons in the mixer view do not seem to be affected.
	// If you want to debug this simply override "PixmapButton::resizeEvent" and trigger a break point in there.
	auto buildPixmapButtonWrappedInWidget = [](QWidget* parent, const QString& toolTip,
		std::string_view activeGraphic, std::string_view inactiveGraphic, PixmapButton*& pixmapButton)
	{
		// Load the active (on) and inactive (off) pixmaps from embedded resources
		const auto activePixmap = embed::getIconPixmap(activeGraphic);
		const auto inactivePixmap = embed::getIconPixmap(inactiveGraphic);

		// Determine the minimum size that can contain both pixmaps
		auto necessarySize = activePixmap.size().expandedTo(inactivePixmap.size());

		// Create a fixed-size wrapper widget to prevent Qt's unwanted resize behavior
		auto wrapperWidget = new QWidget(parent);
		wrapperWidget->setFixedSize(necessarySize);

		// Create the actual PixmapButton inside the wrapper
		auto button = new PixmapButton(wrapperWidget, toolTip);
		button->setCheckable(true);                  // Make it a toggle button (on/off state)
		button->setActiveGraphic(activePixmap);      // Pixmap shown when button is active (checked)
		button->setInactiveGraphic(inactivePixmap);  // Pixmap shown when button is inactive (unchecked)
		button->setToolTip(toolTip);                 // Set the hover tooltip text

		// Return the PixmapButton pointer through the output parameter
		pixmapButton = button;

		return wrapperWidget;
	};

	// Create the mute button (green LED off = muted, green LED on = unmuted)
	auto muteWidget = buildPixmapButtonWrappedInWidget(operationsWidget, tr("Mute"), "led_off", "led_green", m_muteBtn);
	// Create the solo button (red LED on = soloed, red LED off = not soloed)
	auto soloWidget = buildPixmapButtonWrappedInWidget(operationsWidget, tr("Solo"), "led_red", "led_off", m_soloBtn);

	// Add the gear button to the operations layout, centered vertically
	operationsLayout->addWidget(m_trackOps, Qt::AlignCenter);
	// Add a small spacer between the gear button and the mute/solo buttons
	operationsLayout->addSpacing(5);

	// Check user preference: if "compact track buttons" is enabled, stack mute/solo vertically
	if( ConfigManager::inst()->value( "ui",
					  "compacttrackbuttons" ).toInt() )
	{
		// Vertical layout: mute on top, solo on bottom, with stretch above and below
		auto vlayout = new QVBoxLayout();
		vlayout->setContentsMargins(0, 0, 0, 0);
		vlayout->setSpacing(0);
		vlayout->addStretch(1);          // Push buttons toward vertical center
		vlayout->addWidget(muteWidget);  // Mute button on top
		vlayout->addWidget(soloWidget);  // Solo button below
		vlayout->addStretch(1);          // Push buttons toward vertical center
		operationsLayout->addLayout(vlayout);
	}
	else
	{
		// Default layout: mute and solo side-by-side horizontally
		operationsLayout->addWidget(muteWidget, Qt::AlignCenter);
		operationsLayout->addWidget(soloWidget, Qt::AlignCenter);
	}

	// Add a stretch to push all buttons to the left, filling remaining space on the right
	operationsLayout->addStretch(1);

	// Add the operations sub-widget to the main layout, aligned to the top
	layout->addWidget(operationsWidget, 0, Qt::AlignTop);

	// Connect the trackRemovalScheduled signal to the container's deleteTrackView slot.
	// Uses a queued connection so the removal happens after the current event processing
	// is complete, preventing issues with deleting widgets during event handling.
	connect( this, SIGNAL(trackRemovalScheduled(lmms::gui::TrackView*)),
			m_trackView->trackContainerView(),
				SLOT(deleteTrackView(lmms::gui::TrackView*)),
							Qt::QueuedConnection );

	// Repaint this widget whenever the track's muted state changes (to update LED visuals)
	connect( m_trackView->getTrack()->getMutedModel(), SIGNAL(dataChanged()),
			this, SLOT(update()));

	// Repaint this widget whenever the track's color changes (to reflect the new color)
	connect(m_trackView->getTrack(), SIGNAL(colorChanged()), this, SLOT(update()));
}







// =========================================================================
// Event Handlers
// =========================================================================

/**
 * @brief Handle mouse press events on the TrackOperationsWidget.
 *
 * If the user clicks with the left mouse button while holding Ctrl, and this
 * track is not a Pattern Editor track, a drag-and-drop operation is initiated
 * to copy (clone) the track. The track state is serialized into a DataFile
 * and wrapped in a StringPairDrag.
 *
 * If the left button is pressed without Ctrl, the event is ignored so that
 * the parent TrackView can handle it as a track-move (reorder) operation.
 *
 * @param me The mouse event to handle.
 */
void TrackOperationsWidget::mousePressEvent( QMouseEvent * me )
{
	// Left-click + Ctrl on a non-Pattern track: start drag-and-drop copy
	if( me->button() == Qt::LeftButton &&
		me->modifiers() & Qt::ControlModifier &&
			m_trackView->getTrack()->type() != Track::Type::Pattern)
	{
		// Create a DataFile to serialize the track's state for drag-and-drop
		DataFile dataFile( DataFile::Type::DragNDropData );
		// Save the entire track state (settings, clips, etc.) into the DataFile
		m_trackView->getTrack()->saveState( dataFile, dataFile.content() );
		// Initiate the drag operation with the track type as the key, serialized XML as data,
		// and a screenshot of the track settings widget as the drag pixmap
		new StringPairDrag( QString( "track_%1" ).arg(
					static_cast<int>(m_trackView->getTrack()->type()) ),
			dataFile.toString(), m_trackView->getTrackSettingsWidget()->grab(),
									this );
	}
	else if( me->button() == Qt::LeftButton )
	{
		// Plain left-click without Ctrl: ignore the event so the parent TrackView
		// can handle it as a track-move (reorder) drag operation
		me->ignore();
	}
}




/**
 * @brief Paint the widget background.
 *
 * Fills the entire widget rectangle with the window background brush
 * from the current palette/theme.
 *
 * @param pe The paint event (unused).
 */
void TrackOperationsWidget::paintEvent(QPaintEvent*)
{
	QPainter p( this );

	// Fill the widget background with the standard window brush from the palette
	p.fillRect(rect(), palette().brush(QPalette::Window));
}


// =========================================================================
// Track Operations (Clone, Clear, Remove)
// =========================================================================

/**
 * @brief Show a confirmation dialog before removing a track.
 *
 * Checks the user's preference in ConfigManager for whether to show the
 * deletion warning dialog. If the preference is set to skip, returns true
 * immediately. Otherwise, displays a QMessageBox with a "Don't ask again"
 * checkbox. If the user checks that box, future removals will skip the dialog.
 *
 * @return true if the user confirms removal (or if the warning is disabled), false otherwise.
 */
bool TrackOperationsWidget::confirmRemoval()
{
	// Check user preference: "1" means show warning, "0" means skip
	bool needConfirm = ConfigManager::inst()->value("ui", "trackdeletionwarning", "1").toInt();
	if (!needConfirm){ return true; } // User has opted out of the confirmation dialog

	// Build the warning message with the track's display name
	QString messageRemoveTrack = tr("After removing a track, it can not "
					"be recovered. Are you sure you want to remove track \"%1\"?")
					.arg(m_trackView->getTrack()->name());
	QString messageTitleRemoveTrack = tr("Confirm removal");
	QString askAgainText = tr("Don't ask again");

	// Create a checkbox that lets the user suppress future removal warnings
	auto askAgainCheckBox = new QCheckBox(askAgainText, nullptr);
	connect(askAgainCheckBox, &QCheckBox::stateChanged, [](int state){
		// Invert button state: if checkbox is checked, set preference to "0" (don't warn)
		ConfigManager::inst()->setValue("ui", "trackdeletionwarning", state ? "0" : "1");
	});

	// Build and configure the message box with warning icon and OK/Cancel buttons
	QMessageBox mb(this);
	mb.setText(messageRemoveTrack);
	mb.setWindowTitle(messageTitleRemoveTrack);
	mb.setIcon(QMessageBox::Warning);
	mb.addButton(QMessageBox::Cancel);
	mb.addButton(QMessageBox::Ok);
	mb.setCheckBox(askAgainCheckBox);       // Attach the "Don't ask again" checkbox
	mb.setDefaultButton(QMessageBox::Cancel); // Default to Cancel to prevent accidental deletion

	// Execute the dialog and check the result
	int answer = mb.exec();

	if( answer == QMessageBox::Ok )
	{
		return true; // User confirmed the removal
	}
	return false; // User cancelled the removal
}



/**
 * @brief Clone (duplicate) the track associated with this widget.
 *
 * Creates a deep copy of the track (including all clips, settings, effects),
 * inserts the new track view immediately after the original in the track
 * container, and transfers the solo state if the original was soloed.
 */
void TrackOperationsWidget::cloneTrack()
{
	// Get the TrackContainerView that manages all track views
	TrackContainerView *tcView = m_trackView->trackContainerView();

	// Deep-clone the track (creates a new Track with identical state)
	Track *newTrack = m_trackView->getTrack()->clone();
	// Create a corresponding TrackView for the cloned track in the container
	TrackView *newTrackView = tcView->createTrackView( newTrack );

	// Move the new track view to be directly after the original track view.
	// The clone is initially added at the end, so we repeatedly move it up
	// until it sits at position (index + 1).
	int index = tcView->trackViews().indexOf( m_trackView );
	int i = tcView->trackViews().size();
	while ( i != index + 1 )
	{
		tcView->moveTrackView( newTrackView, i - 1 );
		i--;
	}

	// If the original track was in solo mode, toggle solo on the new track
	// so the clone becomes the new solo track instead
	if (m_soloBtn->model()->value())
	{
		// if this track was solo, make the new track the new solo
		newTrack->toggleSolo();
	}
}


/**
 * @brief Clear all clips from this track.
 *
 * Adds a journal checkpoint (for undo support), locks the track to
 * prevent concurrent audio engine access, deletes all clips,
 * then unlocks the track.
 */
void TrackOperationsWidget::clearTrack()
{
	Track * t = m_trackView->getTrack();
	t->addJournalCheckPoint(); // Save state for undo before clearing
	t->lock();                 // Lock the track to prevent audio engine from accessing it
	t->deleteClips();          // Remove all clips from the track
	t->unlock();               // Release the lock after modification is complete
}



/**
 * @brief Remove this track from the track container.
 *
 * Prompts the user for confirmation (unless they have disabled the warning),
 * then emits trackRemovalScheduled which is connected via a queued connection
 * to TrackContainerView::deleteTrackView.
 */
void TrackOperationsWidget::removeTrack()
{
	if (confirmRemoval())
	{
		// Emit the signal; the queued connection ensures the track is deleted
		// after the current event processing completes
		emit trackRemovalScheduled(m_trackView);
	}
}

// =========================================================================
// Track Color Management
// =========================================================================

/**
 * @brief Open a color chooser dialog and set the track's color.
 *
 * Uses the Track color palette and initializes the dialog with the
 * track's current color (defaulting to white if no custom color is set).
 * Adds a journal checkpoint for undo support and marks the song as modified.
 */
void TrackOperationsWidget::selectTrackColor()
{
	// Open a color chooser with the Track palette, initialized to the current track color
	const auto newColor = ColorChooser{this}
		.withPalette(ColorChooser::Palette::Track)
		->getColor(m_trackView->getTrack()->color().value_or(Qt::white));

	if (!newColor.isValid()) { return; } // User cancelled the color dialog

	const auto track = m_trackView->getTrack();
	track->addJournalCheckPoint(); // Save state for undo
	track->setColor(newColor);     // Apply the new color
	Engine::getSong()->setModified(); // Mark the song/project as having unsaved changes
}

/**
 * @brief Reset the track's color to the default (no custom color).
 *
 * Removes any custom color assignment, reverting to the theme's default
 * track color. Adds a journal checkpoint for undo support.
 */
void TrackOperationsWidget::resetTrackColor()
{
	auto track = m_trackView->getTrack();
	track->addJournalCheckPoint();  // Save state for undo
	track->setColor(std::nullopt);  // Clear the custom color (revert to default)
	Engine::getSong()->setModified();
}

/**
 * @brief Assign a random color from the Track palette to this track.
 *
 * Picks a random color from the 48-color Track palette and applies it.
 * Adds a journal checkpoint for undo support.
 */
void TrackOperationsWidget::randomizeTrackColor()
{
	// Pick a random color from the 48-entry Track color palette
	QColor buffer = ColorChooser::getPalette( ColorChooser::Palette::Track )[ rand() % 48 ];
	auto track = m_trackView->getTrack();
	track->addJournalCheckPoint();    // Save state for undo
	track->setColor(buffer);          // Apply the randomly chosen color
	Engine::getSong()->setModified(); // Mark the song as modified
}

/**
 * @brief Reset all clip colors in this track to inherit from the track color.
 *
 * Iterates over every clip in the track and clears its individual color,
 * so clips will inherit the track's color (or the default if no track color is set).
 */
void TrackOperationsWidget::resetClipColors()
{
	auto track = m_trackView->getTrack();
	track->addJournalCheckPoint(); // Save state for undo
	// Clear the custom color on each clip so they inherit the track color
	for (auto clip : track->getClips())
	{
		clip->setColor(std::nullopt);
	}
	Engine::getSong()->setModified();
}

// =========================================================================
// Context Menu
// =========================================================================

/**
 * @brief Populate the context menu for this track's gear button.
 *
 * Called each time the menu is about to be shown (via QMenu::aboutToShow signal).
 * Builds a menu that adapts to the track type:
 *
 * - All tracks: Clone, Remove, Clear (if clips are not fixed), Mixer channel assignment,
 *   and a Color submenu (change, reset, randomize, reset clip colors).
 * - InstrumentTrackView: Adds a MIDI input/output submenu.
 * - AutomationTrackView: Adds "Turn all recording on/off" actions.
 * - PatternTrackView: Adds "Export patterns..." and "Import patterns..." actions
 *   for the .xppt pattern file format.
 */
void TrackOperationsWidget::updateMenu()
{
	// Get the menu attached to the gear button and clear any previous entries
	QMenu * toMenu = m_trackOps->menu();
	toMenu->clear();

	// Add the "Clone this track" action (available for all track types)
	toMenu->addAction( embed::getIconPixmap( "edit_copy", 16, 16 ),
						tr( "Clone this track" ),
						this, SLOT(cloneTrack()));
	// Add the "Remove this track" action (available for all track types)
	toMenu->addAction( embed::getIconPixmap( "cancel", 16, 16 ),
						tr( "Remove this track" ),
						this, SLOT(removeTrack()));

	// "Clear this track" is only available when clips are not fixed
	// (fixed clips are used in the Pattern Editor where clip count is managed automatically)
	if( ! m_trackView->trackContainerView()->fixedClips() )
	{
		toMenu->addAction( tr( "Clear this track" ), this, SLOT(clearTrack()));
	}

	// Add a Mixer channel assignment submenu if the track supports mixer routing
	if (QMenu *mixerMenu = m_trackView->createMixerMenu(tr("Channel %1: %2"), tr("Assign to new Mixer Channel")))
	{
		toMenu->addMenu(mixerMenu);
	}

	// For InstrumentTrackViews: add the MIDI input/output configuration submenu
	if (auto trackView = dynamic_cast<InstrumentTrackView*>(m_trackView))
	{
		toMenu->addSeparator();
		toMenu->addMenu(trackView->midiMenu()); // MIDI input/output channel configuration
	}

	// For AutomationTrackViews: add recording toggle actions for all automation clips
	if( dynamic_cast<AutomationTrackView *>( m_trackView ) )
	{
		toMenu->addAction( tr( "Turn all recording on" ), this, SLOT(recordingOn()));
		toMenu->addAction( tr( "Turn all recording off" ), this, SLOT(recordingOff()));
	}
	if (dynamic_cast<PatternTrackView*>(m_trackView))
	{
		toMenu->addSeparator();
		toMenu->addAction(tr("Export patterns..."), this, SLOT(exportPattern()));
		toMenu->addAction(tr("Import patterns..."), this, SLOT(importPattern()));
	}

	toMenu->addSeparator();

	// Build the "Track color" submenu with change, reset, randomize, and clip color reset options
	QMenu* colorMenu = toMenu->addMenu(tr("Track color"));
	colorMenu->setIcon(embed::getIconPixmap("colorize"));
	colorMenu->addAction(tr("Change"), this, SLOT(selectTrackColor()));       // Open color picker
	colorMenu->addAction(tr("Reset"), this, SLOT(resetTrackColor()));         // Revert to default
	colorMenu->addAction(tr("Pick random"), this, SLOT(randomizeTrackColor())); // Random from palette
	colorMenu->addSeparator();
	colorMenu->addAction(tr("Reset clip colors"), this, SLOT(resetClipColors())); // Clear per-clip colors
}


// =========================================================================
// Automation Recording
// =========================================================================

/**
 * @brief Toggle recording state on all automation clips in this track.
 *
 * Only applicable to AutomationTrackViews. Iterates over all clips in the track,
 * and for each AutomationClip, enables or disables recording mode.
 *
 * @param on true to enable recording, false to disable recording.
 */
void TrackOperationsWidget::toggleRecording( bool on )
{
	// Dynamic cast to check if this is an AutomationTrackView
	auto atv = dynamic_cast<AutomationTrackView*>(m_trackView);
	if( atv )
	{
		// Iterate over all clips in the automation track
		for( Clip * clip : atv->getTrack()->getClips() )
		{
			// Only AutomationClips support recording; skip other clip types
			auto ap = dynamic_cast<AutomationClip*>(clip);
			if( ap ) { ap->setRecording( on ); }
		}
		// Refresh the track view to reflect the updated recording state visually
		atv->update();
	}
}



/**
 * @brief Enable recording on all automation clips in this track.
 *
 * Convenience slot that calls toggleRecording(true).
 */
void TrackOperationsWidget::recordingOn()
{
	toggleRecording( true );
}


/**
 * @brief Disable recording on all automation clips in this track.
 *
 * Convenience slot that calls toggleRecording(false).
 */
void TrackOperationsWidget::recordingOff()
{
	toggleRecording( false );
}


// =========================================================================
// Pattern Export / Import (.xppt files)
// =========================================================================


void TrackOperationsWidget::exportPattern()
{
	auto patternTrack = dynamic_cast<PatternTrack*>(m_trackView->getTrack());
	if (!patternTrack) { return; }

	// --- Step 1: Build a checkbox dialog listing all PatternTracks ---
	QDialog selectionDialog(window());
	selectionDialog.setWindowTitle(tr("Export patterns"));
	selectionDialog.setSizeGripEnabled(true);
	auto* layout = new QVBoxLayout(&selectionDialog);
	layout->addWidget(new QLabel(tr("Select patterns to export:")));

	auto* selectButtonLayout = new QHBoxLayout();
	auto* selectAllBtn = new QPushButton(tr("Select all"), &selectionDialog);
	auto* selectNoneBtn = new QPushButton(tr("Select none"), &selectionDialog);
	selectButtonLayout->addWidget(selectAllBtn);
	selectButtonLayout->addWidget(selectNoneBtn);
	selectButtonLayout->addStretch();
	layout->addLayout(selectButtonLayout);

	auto* scrollArea = new QScrollArea(&selectionDialog);
	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);
	auto* scrollWidget = new QWidget();
	auto* scrollLayout = new QVBoxLayout(scrollWidget);

	QVector<QPair<PatternTrack*, QCheckBox*>> checkboxes;
	for (const auto& track : Engine::getSong()->tracks())
	{
		if (track->type() != Track::Type::Pattern) { continue; }
		auto* pt = dynamic_cast<PatternTrack*>(track);
		if (!pt) { continue; }

		auto* cb = new QCheckBox(pt->name(), scrollWidget);
		if (pt == patternTrack) { cb->setChecked(true); }
		scrollLayout->addWidget(cb);
		checkboxes.append({pt, cb});
	}
	scrollLayout->addStretch();
	scrollArea->setWidget(scrollWidget);
	layout->addWidget(scrollArea, 1);

	connect(selectAllBtn, &QPushButton::clicked, [&checkboxes]() {
		for (const auto& pair : checkboxes) { pair.second->setChecked(true); }
	});
	connect(selectNoneBtn, &QPushButton::clicked, [&checkboxes]() {
		for (const auto& pair : checkboxes) { pair.second->setChecked(false); }
	});

	auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &selectionDialog);
	connect(buttonBox, &QDialogButtonBox::accepted, &selectionDialog, &QDialog::accept);
	connect(buttonBox, &QDialogButtonBox::rejected, &selectionDialog, &QDialog::reject);
	layout->addWidget(buttonBox);

	if (auto* screen = selectionDialog.screen())
	{
		const int maxHeight = screen->availableGeometry().height() * 2 / 3;
		selectionDialog.setMaximumHeight(maxHeight);
	}
	selectionDialog.resize(selectionDialog.sizeHint().width(), qMin(selectionDialog.sizeHint().height(), selectionDialog.maximumHeight()));

	if (selectionDialog.exec() != QDialog::Accepted) { return; }

	// --- Collect the user's selection ---
	QVector<PatternTrack*> selectedTracks;
	for (const auto& pair : checkboxes)
	{
		if (pair.second->isChecked()) { selectedTracks.append(pair.first); }
	}
	if (selectedTracks.isEmpty()) { return; }

	// --- Step 2: Show a directory picker dialog ---
	const QString directory = FileDialog::getExistingDirectory(this, tr("Export patterns to directory"), "");
	if (directory.isEmpty()) { return; }

	// --- Step 3: Normalize names and export ---
	Track::normalizeTrackNames(Engine::getSong());
	Track::normalizeTrackNames(Engine::patternStore());

	auto exportSinglePattern = [](int patternIndex, const QString& filePath)
	{
		DataFile dataFile(DataFile::Type::PatternData);
		QDomDocument& doc = dataFile;
		QDomElement& content = dataFile.content();

		for (const auto& track : Engine::patternStore()->tracks())
		{
			QDomElement trackElement = doc.createElement("track");
			trackElement.setAttribute("type", static_cast<int>(track->type()));
			trackElement.setAttribute("name", track->name());
			if (track->color().has_value())
			{
				trackElement.setAttribute("color", track->color()->name());
			}

			QDomElement settingsElement = doc.createElement(track->nodeName());
			trackElement.appendChild(settingsElement);
			track->saveTrackSpecificSettings(doc, settingsElement, false);

			Clip* clip = track->getClip(static_cast<std::size_t>(patternIndex));
			if (clip)
			{
				clip->saveState(doc, trackElement);
			}

			content.appendChild(trackElement);
		}

		dataFile.writeFile(filePath);
	};

	static const QRegularExpression unsafeChars(R"([\x00-\x1f"*/:<>?\\|\x7f])");

	for (auto* pt : selectedTracks)
	{
		QString filename = pt->name();
		filename.replace(' ', '_');
		filename.remove(unsafeChars);
		filename += ".xppt";
		const QString filePath = QDir(directory).filePath(filename);
		fprintf(stderr, "Exporting Pattern-track \"%s\" to %s\n",
			pt->name().toUtf8().constData(),
			filePath.toUtf8().constData());
		exportSinglePattern(pt->patternIndex(), filePath);
	}

	fprintf(stderr, "Export complete.\n");
}


void TrackOperationsWidget::importPattern()
{
	auto patternTrack = dynamic_cast<PatternTrack*>(m_trackView->getTrack());
	if (!patternTrack) { return; }

	FileDialog ofd(this, tr("Import patterns"), "", tr("LMMS pattern file (*.xppt)"));
	ofd.setAcceptMode(FileDialog::AcceptOpen);
	ofd.setFileMode(FileDialog::ExistingFiles);

	if (ofd.exec() != QDialog::Accepted
		|| ofd.selectedFiles().isEmpty()
		|| ofd.selectedFiles().first().isEmpty())
	{
		return;
	}

	const QStringList files = ofd.selectedFiles();

	std::function<void(QDomElement&)> stripJournallingIDs = [&](QDomElement& parent)
	{
		QDomNode child = parent.firstChild();
		while (!child.isNull())
		{
			QDomNode next = child.nextSibling();
			QDomElement elem = child.toElement();
			if (!elem.isNull())
			{
				if (elem.tagName() == "journallingObject")
				{
					parent.removeChild(child);
				}
				else
				{
					stripJournallingIDs(elem);
				}
			}
			child = next;
		}
	};

	auto importSingleFile = [&stripJournallingIDs](int patternIndex, const QString& filePath)
	{
		DataFile dataFile(filePath);
		QDomElement content = dataFile.content();

		stripJournallingIDs(content);

		static const QStringList clipTagNames = {"midiclip", "sampleclip", "automationclip", "patternclip"};

		QVector<QDomElement> fileTrackElements;
		QDomNode node = content.firstChild();
		while (!node.isNull())
		{
			QDomElement elem = node.toElement();
			node = node.nextSibling();
			if (!elem.isNull() && elem.tagName() == "track"
				&& static_cast<Track::Type>(elem.attribute("type").toInt()) == Track::Type::Instrument)
			{
				fileTrackElements.append(elem);
			}
		}

		const auto& existingTracks = Engine::patternStore()->tracks();

		for (int i = 0; i < fileTrackElements.size(); ++i)
		{
			const QDomElement& trackElement = fileTrackElements[i];

			Track* destTrack = nullptr;
			if (i < static_cast<int>(existingTracks.size()))
			{
				destTrack = existingTracks[i];
			}
			else
			{
				destTrack = Track::create(Track::Type::Instrument, Engine::patternStore());
				destTrack->setName(trackElement.attribute("name"));

				QDomElement itElement = trackElement.firstChildElement("instrumenttrack");
				if (!itElement.isNull())
				{
					auto instTrack = dynamic_cast<InstrumentTrack*>(destTrack);
					if (instTrack)
					{
						instTrack->loadTrackSpecificSettings(itElement);
					}
				}
			}

			QDomElement clipElement;
			QDomNode childNode = trackElement.firstChild();
			while (!childNode.isNull())
			{
				QDomElement childElem = childNode.toElement();
				if (!childElem.isNull() && clipTagNames.contains(childElem.tagName()))
				{
					clipElement = childElem;
					break;
				}
				childNode = childNode.nextSibling();
			}

			if (!clipElement.isNull())
			{
				destTrack->createClipsForPattern(patternIndex);
				Clip* destClip = destTrack->getClip(patternIndex);
				if (destClip)
				{
					const TimePos savedPos = destClip->startPosition();
					destClip->restoreState(clipElement);
					destClip->movePosition(savedPos);
				}
			}
		}
	};

	Engine::audioEngine()->requestChangeInModel();

	const QString firstName = QFileInfo(files[0]).baseName();
	fprintf(stderr, "[importPattern] Renaming PatternTrack to '%s'\n", firstName.toUtf8().constData());
	patternTrack->setName(firstName);
	importSingleFile(patternTrack->patternIndex(), files[0]);

	for (int i = 1; i < files.size(); ++i)
	{
		auto* newTrack = Track::create(Track::Type::Pattern, Engine::getSong());
		auto* newPatternTrack = dynamic_cast<PatternTrack*>(newTrack);
		if (!newPatternTrack) { continue; }
		const QString baseName = QFileInfo(files[i]).baseName();
		fprintf(stderr, "[importPattern] Setting PatternTrack name to '%s'\n", baseName.toUtf8().constData());
		newPatternTrack->setName(baseName);
		importSingleFile(newPatternTrack->patternIndex(), files[i]);
	}

	Engine::audioEngine()->doneChangeInModel();
	Engine::patternStore()->updateComboBox();
	Engine::getSong()->setModified();
}


} // namespace lmms::gui
