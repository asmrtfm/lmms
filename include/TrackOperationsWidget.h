/*
 * TrackOperationsWidget.h - declaration of TrackOperationsWidget class
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

#ifndef LMMS_GUI_TRACK_OPERATIONS_WIDGET_H
#define LMMS_GUI_TRACK_OPERATIONS_WIDGET_H

#include <QWidget> // Base class for the widget

class QCheckBox;   // Forward declaration for the selective clone selection checkbox
class QPushButton; // Forward declaration for the track operations gear button

namespace lmms::gui
{

class PixmapButton; // Forward declaration for mute/solo toggle buttons with pixmap icons
class TrackGrip;    // Forward declaration for the drag handle used to reorder tracks
class TrackView;    // Forward declaration for the parent track view that owns this widget


/**
 * @class TrackOperationsWidget
 * @brief Widget displayed at the left side of each track providing mute, solo,
 *        and track operations (gear menu) controls.
 *
 * This widget is embedded within a TrackView and provides:
 * - A gear button that opens a context menu with track-specific operations
 *   (clone, remove, color, recording, export/import patterns, etc.)
 * - Mute and solo toggle buttons
 * - A TrackGrip handle for drag-reordering tracks
 *
 * The context menu adapts based on the track type (InstrumentTrack, AutomationTrack,
 * PatternTrack, etc.) by using dynamic_cast checks in updateMenu().
 */
class TrackOperationsWidget : public QWidget
{
	Q_OBJECT
public:
	/// Construct the widget with the given TrackView as parent; sets up buttons and layout
	TrackOperationsWidget( TrackView * parent );
	~TrackOperationsWidget() override = default;

	/// Return the TrackGrip handle widget used for drag-reordering this track
	TrackGrip* getTrackGrip() const { return m_trackGrip; }

	/// Show or hide the selective clone checkbox (used by PatternEditor in selection mode)
	void setSelectiveCloneCheckboxVisible(bool visible);
	/// Return whether this track is checked for inclusion in a selective clone operation
	bool isSelectedForSelectiveClone() const;


protected:
	/// Handle right-click to open the context menu (delegates to updateMenu)
	void mousePressEvent( QMouseEvent * me ) override;
	/// Paint the widget background using the current style
	void paintEvent( QPaintEvent * pe ) override;
	/// Show a confirmation dialog before removing a track; returns true if user confirms
	bool confirmRemoval();


private slots:
	/// Create a duplicate of this track and all its clips
	void cloneTrack();
	/// Remove this track from its container (after user confirmation)
	void removeTrack();
	/// Populate and display the context menu with track-type-specific actions
	void updateMenu();
	/// Open a color picker dialog to set a custom track color
	void selectTrackColor();
	/// Assign a random color to this track
	void randomizeTrackColor();
	/// Remove custom color and revert to the default track color
	void resetTrackColor();
	/// Reset all clip colors in this track to inherit the track's color
	void resetClipColors();
	/// Toggle MIDI recording on/off for this track (InstrumentTrack only)
	void toggleRecording(bool on);
	/// Enable MIDI recording for all automation tracks
	void recordingOn();
	/// Disable MIDI recording for all automation tracks
	void recordingOff();
	/// Remove all clips and automation data from this track
	void clearTrack();
	/// Export selected PatternTracks to .xppt files
	void exportPattern();
	/// Import .xppt pattern files into PatternTracks
	void importPattern();

private:
	TrackView * m_trackView;    ///< The parent TrackView that owns this widget

	TrackGrip* m_trackGrip;                  ///< Drag handle for reordering tracks by dragging
	QPushButton * m_trackOps;               ///< The gear button that opens the context menu
	PixmapButton * m_muteBtn;               ///< Toggle button to mute/unmute this track
	PixmapButton * m_soloBtn;               ///< Toggle button to solo/unsolo this track
	QCheckBox * m_selectiveCloneCheckbox;   ///< Checkbox shown during Pattern Editor selective clone mode


	friend class TrackView; ///< TrackView needs access to internal layout and buttons

signals:
	/// Emitted when the user confirms removal of this track (consumed by the track container)
	void trackRemovalScheduled( lmms::gui::TrackView * t );

} ;


} // namespace lmms::gui

#endif // LMMS_GUI_TRACK_OPERATIONS_WIDGET_H
