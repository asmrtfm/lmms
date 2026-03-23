/*
 * PatternEditor.h - basic main-window for editing patterns
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

#ifndef LMMS_GUI_PATTERN_EDITOR_H
#define LMMS_GUI_PATTERN_EDITOR_H

#include <QWidget>              // For the selection mode banner widget

#include "Editor.h"             // Base class for PatternEditorWindow (provides toolbar framework)
#include "TrackContainerView.h" // Base class for PatternEditor (provides track view management)

namespace lmms
{

class PatternStore; // Forward declaration for the track container holding pattern instrument tracks

namespace gui
{

class ComboBox; // Forward declaration for the pattern selection dropdown in the toolbar


/**
 * @class PatternEditor
 * @brief The track container view for editing patterns in the Pattern Editor.
 *
 * PatternEditor displays all instrument, sample, and automation tracks that
 * belong to the PatternStore. Unlike the Song Editor, clips are fixed in position
 * (one clip per pattern per track), so users edit note/sample/automation content
 * directly rather than arranging clips on a timeline.
 *
 * Provides toolbar actions for managing tracks (add sample/automation) and
 * step operations (reset/add/remove/clone steps) that apply to all instrument
 * tracks in the current pattern simultaneously.
 */
class PatternEditor : public TrackContainerView
{
	Q_OBJECT
public:
	/// Construct the editor view for the given PatternStore
	PatternEditor(PatternStore* ps);

	/// Return true: clips in the Pattern Editor are fixed at bar positions (not freely movable)
	bool fixedClips() const override
	{
		return true;
	}

	/// Remove track views associated with a deleted pattern index (called during pattern removal)
	void removeViewsForPattern(int pattern);

	/// Save editor state (e.g. scroll position, selected pattern) to XML
	void saveSettings(QDomDocument& doc, QDomElement& element) override;
	/// Restore editor state from XML
	void loadSettings(const QDomElement& element) override;

public slots:
	/// Reset step count to default for all instrument tracks in the current pattern
	void resetSteps();
	/// Add one bar of steps to all instrument tracks in the current pattern
	void addSteps();
	/// Double the steps (clone existing pattern) for all instrument tracks in the current pattern
	void cloneSteps();
	/// Remove one bar of steps from all instrument tracks in the current pattern
	void removeSteps();
	/// Add a new sample track to the PatternStore
	void addSampleTrack();
	/// Add a new automation track to the PatternStore
	void addAutomationTrack();
	/// Duplicate the current pattern's clips across all tracks into a new pattern
	void cloneClip();
	/// Enter selective clone mode: show checkboxes on instrument tracks and the selection banner
	void beginSelectiveClone();

protected slots:
	/// Handle drag-and-drop of instruments/presets into the editor
	void dropEvent(QDropEvent * de ) override;
	/// Scroll to and select the current pattern when the pattern selection changes
	void updatePosition();

private slots:
	/// Exit selective clone mode without performing a clone; hides checkboxes and banner
	void cancelSelectiveClone();
	/// Execute the selective clone: create a new pattern and copy notes for checked tracks only
	void executeSelectiveClone();

private:
	PatternStore* m_ps;                ///< The PatternStore model this editor displays
	bool m_inSelectiveCloneMode;       ///< True while the user is in selective clone selection mode
	QWidget* m_selectionBanner;        ///< Banner shown at the top of the editor during selective clone mode

	/// Internal helper: add or clone steps for all instrument tracks in the current pattern
	void makeSteps( bool clone );
	/// Internal helper: set selective clone checkbox visibility on all instrument track views
	void setSelectiveCloneCheckboxesVisible(bool visible);
};


/**
 * @class PatternEditorWindow
 * @brief The top-level editor window that wraps PatternEditor with a toolbar.
 *
 * PatternEditorWindow provides the window frame, toolbar (with play/stop buttons,
 * pattern selector combo box, track/step action buttons), and houses the
 * PatternEditor track container view.
 */
class PatternEditorWindow : public Editor
{
Q_OBJECT
public:
	/// Construct the window, create toolbar actions, and set up the pattern selector
	PatternEditorWindow(PatternStore* ps);
	~PatternEditorWindow() = default;

	/// Return the preferred window size
	QSize sizeHint() const override;

	PatternEditor* m_editor; ///< The embedded track container view for pattern editing

public slots:
	/// Start playback of the current pattern
	void play() override;
	/// Stop pattern playback
	void stop() override;

protected slots:
	void normalizeInstrumentTrackNames();

private:
	ComboBox* m_patternComboBox; ///< Dropdown for selecting which pattern to edit
};


} // namespace gui

} // namespace lmms

#endif // LMMS_GUI_PATTERN_EDITOR_H
