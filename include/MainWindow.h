/*
 * MainWindow.h - declaration of class MainWindow, the main window of LMMS
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

#ifndef LMMS_GUI_MAIN_WINDOW_H
#define LMMS_GUI_MAIN_WINDOW_H

#include <QBasicTimer>  // Low-overhead timer for periodic UI updates (periodicUpdate signal)
#include <QTimer>       // Timer for auto-save interval scheduling
#include <QList>        // Container for loaded tool plugin views
#include <QMainWindow>  // Base class providing menu bar, toolbars, and central widget

#include "ConfigManager.h" // Access to user configuration values (e.g. auto-save interval)

class QAction;      // Forward declaration for menu/toolbar action items
class QDomElement;  // Forward declaration for XML serialization of widget state
class QGridLayout;  // Forward declaration for toolbar button layout
class QMdiArea;     // Forward declaration for the MDI workspace area

namespace lmms
{

class ConfigManager; // Forward declaration (already included but used in namespace context)

namespace gui
{

class PluginView;      // Forward declaration for tool plugin UI views
class SideBar;         // Forward declaration for the file/plugin browser sidebar panel
class SubWindow;       // Forward declaration for MDI sub-windows in the workspace
class ToolButton;      // Forward declaration for toolbar toggle buttons
class GuiApplication;  // Forward declaration for the friend class that constructs MainWindow


/**
 * @class MainWindow
 * @brief The application's main window containing the workspace, toolbar, and menus.
 *
 * MainWindow is a singleton created by GuiApplication. It manages:
 * - The MDI workspace where editor windows (Song, Pattern, Piano Roll, etc.) live
 * - The main toolbar with transport controls and editor toggle buttons
 * - File, Edit, View, and Help menus
 * - Auto-save functionality with configurable intervals
 * - Keyboard modifier tracking for global shortcuts
 * - Session state (Normal vs Recover mode after crash)
 */
class MainWindow : public QMainWindow
{
	Q_OBJECT
public:
	/// Return the MDI workspace area where editor sub-windows are displayed
	QMdiArea* workspace()
	{
		return m_workspace;
	}

	/// Return true if multi-window mode is currently active.
	bool isMultiWindowMode() const;

	/// Return the main toolbar widget at the top of the window
	QWidget* toolBar()
	{
		return m_toolBar;
	}

	/// Add a widget to the toolbar grid layout at the specified row/column (-1 for auto)
	int addWidgetToToolBar( QWidget * _w, int _row = -1, int _col = -1 );
	/// Add horizontal spacing to the toolbar layout
	void addSpacingToToolBar( int _size );

	/// Wrap a widget in an MDI sub-window with decorations and add it to the workspace
	LMMS_EXPORT SubWindow* addWindowedWidget(QWidget *w, Qt::WindowFlags windowFlags = QFlag(0));


	/// Bring the main window to focus (used after dialog dismissals)
	void refocus();

	/**
	 * @brief Prompt the user to save unsaved changes before a destructive action.
	 *
	 * Shows a dialog with Save/Discard/Cancel options. Must be called before
	 * any operation that would discard the current project (new, open, etc.).
	 *
	 * @param stopPlayback Whether to stop audio playback before showing the dialog.
	 *        If false, the caller is responsible for stopping playback.
	 * @return true if the user chose to proceed (Save or Discard), false if Cancel.
	 */
	bool mayChangeProject(bool stopPlayback);

	/// Default auto-save interval in minutes (used when no config value is set)
	static const int DEFAULT_SAVE_INTERVAL_MINUTES = 2;
	/// Default auto-save interval in milliseconds
	static const int DEFAULT_AUTO_SAVE_INTERVAL = DEFAULT_SAVE_INTERVAL_MINUTES * 60 * 1000;

	/// Minimum auto-save interval threshold in milliseconds (10 seconds)
	static const int m_autoSaveShortTime = 10 * 1000;

	/**
	 * @brief Reset the auto-save timer to the specified interval.
	 * @param msec Interval in milliseconds; defaults to the user's configured value.
	 *        Falls back to DEFAULT_AUTO_SAVE_INTERVAL if the config value is too small.
	 */
	void autoSaveTimerReset( int msec = ConfigManager::inst()->
					value( "ui", "saveinterval" ).toInt()
						* 60 * 1000 )
	{
		if( msec < m_autoSaveShortTime ) // Config value missing or too small
		{
			msec = DEFAULT_AUTO_SAVE_INTERVAL;
		}
		m_autoSaveTimer.start( msec );
	}

	/// Return the current auto-save timer interval in milliseconds
	int getAutoSaveTimerInterval()
	{
		return m_autoSaveTimer.interval();
	}

	/**
	 * @enum SessionState
	 * @brief Tracks whether this session started normally or is recovering from a crash
	 */
	enum class SessionState
	{
		Normal,  ///< Normal session startup
		Recover  ///< Recovering from a previous crash (auto-save recovery file exists)
	};

	/// Set the session state (Normal or Recover)
	void setSession( SessionState session )
	{
		m_session = session;
	}

	/// Return the current session state
	SessionState getSession()
	{
		return m_session;
	}

	/// Clean up recovery files after a successful session
	void sessionCleanup();

	/// Reset all tracked keyboard modifier states to false
	void clearKeyModifiers();

	/// Return whether the Shift key is currently held (may get stuck; prefer Qt key events)
	// TODO Remove this function, since m_shift can get stuck down.
	// [[deprecated]]
	bool isShiftPressed()
	{
		return m_keyMods.m_shift;
	}

	/// Save a widget's geometry and visibility state to an XML element
	static void saveWidgetState( QWidget * _w, QDomElement & _de );
	/// Restore a widget's geometry and visibility state from an XML element
	static void restoreWidgetState( QWidget * _w, const QDomElement & _de );

	/// Global event filter for handling application-wide keyboard events
	bool eventFilter(QObject* watched, QEvent* event) override;

public slots:
	/// Update the window title to reflect the current project name and modification state
	void resetWindowTitle();

	/// No-op slot used as a placeholder for unconnected menu actions
	void emptySlot();
	/// Create a new empty project (prompts to save current project first)
	void createNewProject();
	/// Open an existing project file via a file dialog
	void openProject();
	/// Save the current project to its existing filename; returns true on success
	bool saveProject();
	/// Save the current project with a new filename via a file dialog; returns true on success
	bool saveProjectAs();
	/// Save the current project as a new version (increments version suffix)
	bool saveProjectAsNewVersion();
	/// Save the current project state as the default template (~/.lmms/templates/default.mpt)
	void saveProjectAsDefaultTemplate();
	/// Save the project without pattern data — prompts for filename and format (.mpt or .mmp/.mmpz)
	void saveProjectAsDefaultTemplateNoPatterns();
	/// Save the current project as a SQLite .lmms-db file
	void saveProjectAsSqlite();
	/// Open the application settings/preferences dialog
	void showSettingsDialog();
	/// Show the About LMMS dialog with version and credits
	void aboutLMMS();
	/// Open the LMMS documentation in the default web browser
	void help();
	/// Toggle visibility of the Automation Editor window
	void toggleAutomationEditorWin();
	/// Toggle visibility of the Pattern Editor window (forceShow=true to always show)
	void togglePatternEditorWin(bool forceShow = false);
	/// Toggle visibility of the Song Editor window
	void toggleSongEditorWin();
	/// Toggle visibility of the Project Notes window
	void toggleProjectNotesWin();
	/// Toggle visibility of the Microtuner configuration window
	void toggleMicrotunerWin();
	/// Toggle visibility of the Mixer (FX mixer) window
	void toggleMixerWin();
	/// Toggle visibility of the Piano Roll window
	void togglePianoRollWin();
	/// Toggle visibility of the Controller Rack window
	void toggleControllerRack();
	/// Toggle fullscreen mode for the main window
	void toggleFullscreen();

	/// Toggle between single-window (MDI) and multi-window (OS-level) modes.
	void toggleMultiWindowMode();

	/// Update play/pause button icons based on current playback state
	void updatePlayPauseIcons();

	/// Enable/disable undo/redo toolbar buttons based on journal state
	void updateUndoRedoButtons();
	/// Undo the last action via the journalling system
	void undo();
	/// Redo the last undone action via the journalling system
	void redo();

	/// Perform an auto-save of the current project to a recovery file
	void autoSave();

private slots:
	/// Export the current project as a MIDI file
	void onExportProjectMidi();

protected:
	/// Handle window close: prompt to save, clean up, and exit
	void closeEvent( QCloseEvent * _ce ) override;
	/// Track when the window loses focus (for modifier key state management)
	void focusOutEvent( QFocusEvent * _fe ) override;
	/// Track keyboard modifier state (Ctrl, Shift, Alt) on key press
	void keyPressEvent( QKeyEvent * _ke ) override;
	/// Track keyboard modifier state on key release
	void keyReleaseEvent( QKeyEvent * _ke ) override;
	/// Handle periodic timer events (emits periodicUpdate for UI refresh)
	void timerEvent( QTimerEvent * _ev ) override;


private:
	/// Private constructor (singleton pattern, created by GuiApplication)
	MainWindow();
	/// Deleted copy constructor (singleton)
	MainWindow( const MainWindow & );
	~MainWindow() override;

	/// Complete initialization after construction (creates menus, toolbar, workspace)
	void finalize();

	/// Toggle a sub-window's visibility; forceShow=true ensures it becomes visible
	void toggleWindow( QWidget *window, bool forceShow = false );

	/// Enter or exit multi-window mode. Detaches/attaches all 8 main editors
	/// and the SideBar; collapses or restores the workspace container.
	void setMultiWindowMode(bool enabled);

	/// Export the project as audio; multiExport=true exports each track separately
	void exportProject(bool multiExport = false);
	/// Display success/error feedback after a save operation
	void handleSaveResult(QString const & filename, bool songSavedSuccessfully);
	/// Internal: save project with UI feedback (progress bar, error dialogs)
	bool guiSaveProject();
	/// Internal: save project to a specific filename with UI feedback
	bool guiSaveProjectAs( const QString & filename );

	QMdiArea * m_workspace;      ///< MDI area where editor sub-windows are displayed

	QWidget * m_toolBar;         ///< The main toolbar container widget
	QGridLayout * m_toolBarLayout; ///< Grid layout managing toolbar button positions

	/**
	 * @struct keyModifiers
	 * @brief Tracks the current state of keyboard modifier keys
	 * @note This tracking can get stuck if key-up events are missed (e.g. alt-tab)
	 */
	struct keyModifiers
	{
		keyModifiers() :
			m_ctrl( false ),
			m_shift( false ),
			m_alt( false )
		{
		}
		bool m_ctrl;   ///< Whether Ctrl is currently held
		bool m_shift;  ///< Whether Shift is currently held
		bool m_alt;    ///< Whether Alt is currently held
	} m_keyMods;

	QMenu * m_toolsMenu;         ///< The Tools menu for launching tool plugins
	QAction * m_undoAction;      ///< Toolbar action for undo
	QAction * m_redoAction;      ///< Toolbar action for redo
	QList<PluginView *> m_tools; ///< List of loaded tool plugin views

	QBasicTimer m_updateTimer;   ///< Low-overhead timer driving periodic UI updates
	QTimer m_autoSaveTimer;      ///< Timer for scheduling auto-save operations
	int m_autoSaveInterval;      ///< Configured auto-save interval in milliseconds

	friend class GuiApplication; ///< GuiApplication creates and initializes MainWindow

	QMenu * m_viewMenu;          ///< The View menu (toggles for editor windows)

	ToolButton * m_metronomeToggle; ///< Toolbar button to toggle the metronome click

	SessionState m_session;      ///< Current session state (Normal or Recover)

	bool maximized;              ///< Tracks whether the window was maximized before fullscreen

	bool m_multiWindowMode;          ///< True when editors are free-floating OS windows
	SideBar* m_sideBar;              ///< The file/plugin browser sidebar panel
	QWidget* m_workspaceContainer;  ///< The widget containing the sidebar + MDI area
	bool m_sideBarOnRight;           ///< True if sidebar is configured on the right side

private slots:
	/// Open the LMMS help/documentation URL in the default browser
	void browseHelp();
	/// Handle selection of a tool plugin from the Tools menu
	void showTool( QAction * _idx );
	/// Update View menu check states to reflect current window visibility
	void updateViewMenu();
	/// Handle configuration changes from the View menu (e.g. toolbar visibility)
	void updateConfig( QAction * _who );
	/// Handle metronome toggle button clicks
	void onToggleMetronome();
	/// Initiate project audio export (single file)
	void onExportProject();
	/// Initiate project audio export (one file per track)
	void onExportProjectTracks();
	/// Initiate project import from another DAW format
	void onImportProject();
	/// Handle song modification state changes (update title bar asterisk)
	void onSongModified();
	/// Handle project filename changes (update title bar)
	void onProjectFileNameChanged();

signals:
	/// Emitted periodically for UI components that need regular refresh (VU meters, etc.)
	void periodicUpdate();
	/// Emitted during startup to update the splash screen progress message
	void initProgress(const QString &msg);

} ;


} // namespace gui

} // namespace lmms

#endif // LMMS_GUI_MAIN_WINDOW_H
