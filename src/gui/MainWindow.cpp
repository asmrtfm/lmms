/*
 * MainWindow.cpp - implementation of LMMS-main-window
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

// Own header first, per project include order convention
#include "MainWindow.h"

// System/Qt headers
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDomElement>
#include <QFileInfo>
#include <QMdiArea>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QSplitter>

// Project headers — UI dialogs and views
#include "AboutDialog.h"
#include "AutomationEditor.h"
#include "ControllerRackView.h"
#include "DataFile.h"
#include "embed.h"
#include "Engine.h"
#include "ExportProjectDialog.h"
#include "FileBrowser.h"
#include "DataFile.h"
#include "FileDialog.h"
#include "Track.h"
#include "Metronome.h"
#include "MixerView.h"
#include "GuiApplication.h"
#include "ImportFilter.h"
#include "InstrumentTrackView.h"
#include "InstrumentTrackWindow.h"
#include "MicrotunerConfig.h"
#include "PatternEditor.h"
#include "PianoRoll.h"
#include "PianoView.h"
#include "PluginBrowser.h"
#include "PluginFactory.h"
#include "PluginView.h"
#include "ProjectJournal.h"
#include "ProjectNotes.h"
#include "ProjectRenderer.h"
#include "RecentProjectsMenu.h"
#include "RemotePlugin.h"
#include "SetupDialog.h"
#include "SideBar.h"
#include "SongEditor.h"
#include "SubWindow.h"
#include "TemplatesMenu.h"
#include "TextFloat.h"
#include "TimeLineWidget.h"
#include "ToolButton.h"
#include "ToolPlugin.h"
#include "VersionedSaveDialog.h"

// Auto-generated version string header
#include "lmmsversion.h"


namespace lmms::gui
{


/**
 * @brief Constructs the MainWindow, the central application window of LMMS.
 *
 * Sets up the overall layout consisting of:
 *   - A vertical box layout with toolbar on top and content area below
 *   - A horizontal layout with a SideBar (file/plugin browsers) and a QSplitter
 *   - An MDI workspace (QMdiArea) for editor sub-windows
 *   - A global toolbar with project and editor toggle buttons
 *   - An auto-save timer and update timer for periodic UI refresh
 *   - Key modifier tracking and signal/slot connections
 *
 * The constructor does NOT create menus or finalize the toolbar — that is
 * deferred to finalize(), which is called after GuiApplication has created
 * all editor sub-windows.
 */
MainWindow::MainWindow() :
	m_workspace( nullptr ),       // MDI workspace, created below
	m_toolsMenu( nullptr ),       // Tools menu, populated in finalize()
	m_autoSaveTimer( this ),      // Timer that triggers periodic auto-save
	m_viewMenu( nullptr ),        // View menu, populated in finalize()
	m_metronomeToggle( 0 ),       // Metronome toggle button in toolbar
	m_session( SessionState::Normal ) // Normal session (not recovering)
{
	// Allow Qt to delete the window when it is closed
	setAttribute( Qt::WA_DeleteOnClose );

	// Main widget that holds the entire window content
	auto main_widget = new QWidget(this);
	// Vertical layout: toolbar on top, content area (sidebar + workspace) below
	auto vbox = new QVBoxLayout(main_widget);
	vbox->setSpacing( 0 );
	vbox->setContentsMargins(0, 0, 0, 0);

	// Horizontal container for sidebar and workspace splitter
	auto w = new QWidget(main_widget);
	auto hbox = new QHBoxLayout(w);
	hbox->setSpacing( 0 );
	hbox->setContentsMargins(0, 0, 0, 0);

	// Vertical sidebar with collapsible tabs for browsers
	auto sideBar = new SideBar(Qt::Vertical, w);

	// Splitter separates sidebar content panels from the MDI workspace
	auto splitter = new QSplitter(Qt::Horizontal, w);
	splitter->setChildrenCollapsible( false );

	// Read config to determine sidebar placement (left or right)
	ConfigManager* confMgr = ConfigManager::inst();
	bool sideBarOnRight = confMgr->value("ui", "sidebaronright").toInt();

	// --- Populate sidebar tabs with file/plugin browsers ---

	// Plugin browser tab: lists all available instrument and effect plugins
	emit initProgress(tr("Preparing plugin browser"));
	sideBar->appendTab( new PluginBrowser( splitter ) );

	// "My Projects" tab: user and factory project directories, filtered to project file types
	emit initProgress(tr("Preparing file browsers"));
	sideBar->appendTab( new FileBrowser(
				confMgr->userProjectsDir() + "*" +
				confMgr->factoryProjectsDir(),
					"*.mmp *.mmpz *.xml *.mid *.mpt",
							tr( "My Projects" ),
					embed::getIconPixmap( "project_file" ).transformed( QTransform().rotate( 90 ) ),
							splitter, false,
				confMgr->userProjectsDir(),
				confMgr->factoryProjectsDir()));

	// "My Samples" tab: user and factory sample directories
	sideBar->appendTab(
		new FileBrowser(confMgr->userSamplesDir() + "*" + confMgr->factorySamplesDir(), FileItem::defaultFilters(),
			tr("My Samples"), embed::getIconPixmap("sample_file").transformed(QTransform().rotate(90)), splitter, false,
			confMgr->userSamplesDir(), confMgr->factorySamplesDir()));

	// "My Presets" tab: user and factory preset directories (xpf, cs.xml, xiz, lv2)
	sideBar->appendTab( new FileBrowser(
				confMgr->userPresetsDir() + "*" +
				confMgr->factoryPresetsDir(),
					"*.xpf *.cs.xml *.xiz *.lv2",
					tr( "My Presets" ),
					embed::getIconPixmap( "preset_file" ).transformed( QTransform().rotate( 90 ) ),
							splitter , false,
				confMgr->userPresetsDir(),
				confMgr->factoryPresetsDir()));

	// "My Home" tab: user's home directory with default file filters
	sideBar->appendTab(new FileBrowser(QDir::homePath(), FileItem::defaultFilters(), tr("My Home"),
		embed::getIconPixmap("home").transformed(QTransform().rotate(90)), splitter, false));

	// --- Root/Volumes/My Computer tab (platform-specific) ---
	QStringList root_paths;
	QString title = tr("Root Directory");
	bool dirs_as_items = false;

#ifdef LMMS_BUILD_APPLE
	// On macOS, show /Volumes instead of root
	title = tr( "Volumes" );
	root_paths += "/Volumes";
#elif defined(LMMS_BUILD_WIN32)
	// On Windows, show drive letters as "My Computer"
	title = tr( "My Computer" );
	dirs_as_items = true;
#endif

#if ! defined(LMMS_BUILD_APPLE)
	// On Linux/Windows, enumerate all mounted drives/partitions
	QFileInfoList drives = QDir::drives();
	for( const QFileInfo & drive : drives )
	{
		root_paths += drive.absolutePath();
	}
#endif

	// Add the root filesystem browser tab
	sideBar->appendTab(new FileBrowser(root_paths.join("*"), FileItem::defaultFilters(), title,
		embed::getIconPixmap("computer").transformed(QTransform().rotate(90)), splitter, dirs_as_items));

	// --- MDI workspace ---
	// The workspace hosts all editor sub-windows (Song Editor, Piano Roll, etc.)
	m_workspace = new QMdiArea(splitter);

	// Load and apply workspace background image from user config
	emit initProgress(tr("Loading background picture"));
	QString backgroundPicFile = ConfigManager::inst()->backgroundPicFile();
	QImage backgroundPic;
	if( !backgroundPicFile.isEmpty() )
	{
		backgroundPic = QImage( backgroundPicFile );
	}
	if( !backgroundPicFile.isNull() )
	{
		m_workspace->setBackground( backgroundPic );
	}
	else
	{
		// No background configured; use a transparent/empty brush
		m_workspace->setBackground( Qt::NoBrush );
	}

	// Prevent sub-windows from auto-maximizing when activated
	m_workspace->setOption( QMdiArea::DontMaximizeSubWindowOnActivation );
	// Enable scrollbars when sub-windows extend beyond visible area
	m_workspace->setHorizontalScrollBarPolicy( Qt::ScrollBarAsNeeded );
	m_workspace->setVerticalScrollBarPolicy( Qt::ScrollBarAsNeeded );

	// Add sidebar and splitter to the horizontal layout
	hbox->addWidget(sideBar);
	hbox->addWidget(splitter);
	// If the user wants the sidebar on the right, we move the workspace and
	// the splitter to the "left" side, or the first widgets in their list
	if (sideBarOnRight)
	{
		splitter->insertWidget(0, m_workspace);
		hbox->insertWidget(0, splitter);
	}

	// --- Global toolbar at the top of the window ---
	m_toolBar = new QWidget( main_widget );
	m_toolBar->setObjectName( "mainToolbar" ); // Object name for stylesheet targeting
	m_toolBar->setFixedHeight( 64 );           // Two rows of 32px buttons
	m_toolBar->move( 0, 0 );

	// Grid layout for arranging toolbar buttons in two rows
	m_toolBarLayout = new QGridLayout( m_toolBar/*, 2, 1*/ );
	m_toolBarLayout->setContentsMargins(0, 0, 0, 0);
	m_toolBarLayout->setSpacing( 0 );

	// Place toolbar above the content area
	vbox->addWidget( m_toolBar );
	vbox->addWidget( w );
	setCentralWidget( main_widget );

	// Start the periodic UI update timer at 60 fps (drives VU meters, etc.)
	m_updateTimer.start( 1000 / 60, this );  // 60 fps

	// --- Auto-save setup ---
	if( ConfigManager::inst()->value( "ui", "enableautosave" ).toInt() )
	{
		// Connect the auto-save timer's timeout signal to the autoSave slot
		connect(&m_autoSaveTimer, SIGNAL(timeout()), this, SLOT(autoSave()));
		// Read the save interval from config; use default if less than 1 minute
		m_autoSaveInterval = ConfigManager::inst()->value(
					"ui", "saveinterval" ).toInt() < 1 ?
						DEFAULT_AUTO_SAVE_INTERVAL :
				ConfigManager::inst()->value(
					"ui", "saveinterval" ).toInt();

		// The auto save function mustn't run until there is a project
		// to save or it will run over recover.mmp if you hesitate at the
		// recover messagebox for a minute. It is now started in main.
		// See autoSaveTimerReset() in MainWindow.h
	}

	// Update play/pause icons in all editors when the song's playback state changes
	connect( Engine::getSong(), SIGNAL(playbackStateChanged()),
				this, SLOT(updatePlayPauseIcons()));

	// Update window title asterisk when the song is modified
	connect(Engine::getSong(), SIGNAL(modified()), SLOT(onSongModified()));
	// Update window title with new filename when the project filename changes
	connect(Engine::getSong(), SIGNAL(projectFileNameChanged()), SLOT(onProjectFileNameChanged()));

	// Track whether window was maximized (used by fullscreen toggle to restore state)
	maximized = isMaximized();
	// F11 toggles fullscreen mode
	new QShortcut(QKeySequence(Qt::Key_F11), this, SLOT(toggleFullscreen()));

	// If tooltips are disabled in config, install a global event filter to block them
	if (ConfigManager::inst()->value("tooltips", "disabled").toInt())
	{
		qApp->installEventFilter(this);
	}
}




/**
 * @brief Destructor for MainWindow.
 *
 * Cleans up tool plugins, editor windows, and destroys the engine.
 * Editors must be destroyed before the Song is deleted in Engine::destroy()
 * due to dependency ordering (see GitHub issue #2015).
 */
MainWindow::~MainWindow()
{
	// Delete all tool plugin views and their underlying models
	for( PluginView *view : m_tools )
	{
		delete view->model();
		delete view;
	}
	// TODO: Close tools
	// dependencies are such that the editors must be destroyed BEFORE Song is deletect in Engine::destroy
	//   see issue #2015 on github
	delete getGUI()->automationEditor();
	delete getGUI()->pianoRoll();
	delete getGUI()->songEditor();
	// destroy engine which will do further cleanups etc.
	Engine::destroy();
}




/**
 * @brief Finalizes the MainWindow after all GUI components are created.
 *
 * Called by GuiApplication after creating all editor sub-windows. This method:
 *   1. Sets up the window title and icon
 *   2. Creates the File menu (New, Open, Recent, Save, Save As, Save as New Version,
 *      Save as Default Template, Save without patterns, Import, Export, Quit)
 *   3. Creates the Edit menu (Undo, Redo, Scales/Keymaps, Settings)
 *   4. Creates the View menu (editor toggles, fullscreen, display options)
 *   5. Creates the Tools menu (dynamically populated from tool plugins)
 *   6. Creates the Help menu (Online Help, About)
 *   7. Populates the toolbar with project buttons (row 0) and editor toggle buttons (row 1)
 *   8. Shows the setup dialog if this is the first run or audio device failed
 *   9. Adds editor sub-windows to the MDI workspace
 */
void MainWindow::finalize()
{
	// Set the window title to the current project name (or "Untitled")
	resetWindowTitle();
	// Set the application icon in the title bar
	setWindowIcon( embed::getIconPixmap( "icon_small" ) );


	// === File Menu ("&File") ===
	auto project_menu = new QMenu(this);
	menuBar()->addMenu( project_menu )->setText( tr( "&File" ) );

	// "New" — create a blank project (Ctrl+N)
	project_menu->addAction( embed::getIconPixmap( "project_new" ),
					tr( "&New" ),
					this, SLOT(createNewProject()),
					QKeySequence::New );

	// "New from Template" submenu — lists available project templates
	auto templates_menu = new TemplatesMenu( this );
	project_menu->addMenu(templates_menu);

	// "Open..." — open an existing project file (Ctrl+O)
	project_menu->addAction( embed::getIconPixmap( "project_open" ),
					tr( "&Open..." ),
					this, SLOT(openProject()),
					QKeySequence::Open );

	// "Recent Projects" submenu — lists recently opened project files
	project_menu->addMenu(new RecentProjectsMenu(this));

	// "Save" — save the current project (Ctrl+S)
	project_menu->addAction( embed::getIconPixmap( "project_save" ),
					tr( "&Save" ),
					this, SLOT(saveProject()),
					QKeySequence::Save );

	// "Save As..." — save with a new filename (Ctrl+Shift+S)
	project_menu->addAction( embed::getIconPixmap( "project_save" ),
					tr( "Save &As..." ),
					this, SLOT(saveProjectAs()),
					Qt::CTRL + Qt::SHIFT + Qt::Key_S );

	// "Save as New Version" — auto-increment version number in filename (Ctrl+Alt+S)
	project_menu->addAction( embed::getIconPixmap( "project_save" ),
					tr( "Save as New &Version" ),
					this, SLOT(saveProjectAsNewVersion()),
					Qt::CTRL + Qt::ALT + Qt::Key_S );

	// "Save as default template" — overwrite ~/.lmms/templates/default.mpt
	project_menu->addAction( embed::getIconPixmap( "project_save" ),
					tr( "Save as default template" ),
					this, SLOT(saveProjectAsDefaultTemplate()));

	// "Save without patterns..." — save project/template with pattern data stripped out
	project_menu->addAction( embed::getIconPixmap( "project_save" ),
					tr( "Save without patterns..." ),
					this, SLOT(saveProjectAsDefaultTemplateNoPatterns()));

	// "Save as SQLite..." — save project in SQLite format (.lmms-db)
	project_menu->addAction( embed::getIconPixmap( "project_save" ),
					tr( "Save as SQLite..." ),
					this, SLOT(saveProjectAsSqlite()));

	// --- Separator between save and import/export actions ---
	project_menu->addSeparator();

	// "Import..." — import a MIDI or Hydrogen file
	project_menu->addAction( embed::getIconPixmap( "project_import" ),
					tr( "Import..." ),
					this,
					SLOT(onImportProject()));

	// "Export..." — export project as audio file (Ctrl+E)
	project_menu->addAction( embed::getIconPixmap( "project_export" ),
					tr( "E&xport..." ),
					this,
					SLOT(onExportProject()),
					Qt::CTRL + Qt::Key_E );

	// "Export Tracks..." — export each track as a separate audio file (Ctrl+Shift+E)
	project_menu->addAction( embed::getIconPixmap( "project_export" ),
					tr( "E&xport Tracks..." ),
					this,
					SLOT(onExportProjectTracks()),
					Qt::CTRL + Qt::SHIFT + Qt::Key_E );

	// "Export MIDI..." — export project as a MIDI file (Ctrl+M)
	project_menu->addAction( embed::getIconPixmap( "midi_file" ),
					tr( "Export &MIDI..." ),
					this,
					SLOT(onExportProjectMidi()),
					Qt::CTRL + Qt::Key_M );

	// --- Separator before Quit ---
	project_menu->addSeparator();

	// "Quit" — close all windows and exit (Ctrl+Q)
	project_menu->addAction( embed::getIconPixmap( "exit" ), tr( "&Quit" ),
					qApp, SLOT(closeAllWindows()),
					Qt::CTRL + Qt::Key_Q );

	// === Edit Menu ("&Edit") ===
	auto edit_menu = new QMenu(this);
	menuBar()->addMenu( edit_menu )->setText( tr( "&Edit" ) );

	// "Undo" — undo the last journalled action (Ctrl+Z)
	m_undoAction = edit_menu->addAction( embed::getIconPixmap( "edit_undo" ),
					tr( "Undo" ),
					this, SLOT(undo()),
					QKeySequence::Undo );

	// "Redo" — redo the last undone action (Ctrl+Y or Ctrl+Shift+Z)
	m_redoAction = edit_menu->addAction( embed::getIconPixmap( "edit_redo" ),
					tr( "Redo" ),
					this, SLOT(redo()),
					QKeySequence::Redo );

	// Ensure that both (Ctrl+Y) and (Ctrl+Shift+Z) activate redo shortcut regardless of OS defaults
	if (QKeySequence(QKeySequence::Redo) != QKeySequence(Qt::CTRL + Qt::Key_Y))
	{
		new QShortcut( QKeySequence( Qt::CTRL + Qt::Key_Y ), this, SLOT(redo()));
	}
	if (QKeySequence(QKeySequence::Redo) != QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z ))
	{
		new QShortcut( QKeySequence( Qt::CTRL + Qt::SHIFT + Qt::Key_Z ), this, SLOT(redo()));
	}

	edit_menu->addSeparator();

	// "Scales and keymaps" — open the microtuner configuration dialog
	edit_menu->addAction(embed::getIconPixmap("microtuner"), tr("Scales and keymaps"),
		this, SLOT(toggleMicrotunerWin()));

	// "Settings" — open the application settings dialog
	edit_menu->addAction(embed::getIconPixmap("setup_general"), tr("Settings"),
		this, SLOT(showSettingsDialog()));

	// Grey out undo/redo when the Edit menu is about to be shown, based on journal state
	connect(edit_menu, SIGNAL(aboutToShow()), this, SLOT(updateUndoRedoButtons()));

	// === View Menu ("&View") ===
	// Built dynamically each time it is opened (see updateViewMenu())
	m_viewMenu = new QMenu( this );
	menuBar()->addMenu( m_viewMenu )->setText( tr( "&View" ) );
	// Rebuild the menu contents each time it is about to be shown
	connect( m_viewMenu, SIGNAL(aboutToShow()),
		 this, SLOT(updateViewMenu()));
	// Handle toggling of checkable config items in the View menu
	connect( m_viewMenu, SIGNAL(triggered(QAction*)), this,
		SLOT(updateConfig(QAction*)));


	// === Tools Menu ("&Tools") ===
	// Dynamically populated from all registered Tool-type plugins
	m_toolsMenu = new QMenu( this );
	for( const Plugin::Descriptor* desc : getPluginFactory()->descriptors(Plugin::Type::Tool) )
	{
		// Add each tool plugin as a menu item with its logo and display name
		m_toolsMenu->addAction( desc->logo->pixmap(), desc->displayName );
		// Instantiate the tool plugin and create its view, storing for later display
		m_tools.push_back( ToolPlugin::instantiate( desc->name, /*this*/nullptr )
						   ->createView(this) );
	}
	// Only add the Tools menu to the menu bar if there are tool plugins available
	if( !m_toolsMenu->isEmpty() )
	{
		menuBar()->addMenu( m_toolsMenu )->setText( tr( "&Tools" ) );
		connect( m_toolsMenu, SIGNAL(triggered(QAction*)),
					this, SLOT(showTool(QAction*)));
	}


	// === Help Menu ("&Help") ===
	auto help_menu = new QMenu(this);
	menuBar()->addMenu( help_menu )->setText( tr( "&Help" ) );
	// May use offline help (currently always shows online help)
	if( true )
	{
		// "Online Help" — opens the LMMS documentation website in the default browser
		help_menu->addAction( embed::getIconPixmap( "help" ),
						tr( "Online Help" ),
						this, SLOT(browseHelp()));
	}
	else
	{
		// Placeholder for future offline help support
		help_menu->addAction( embed::getIconPixmap( "help" ),
							tr( "Help" ),
							this, SLOT(help()));
	}

	help_menu->addSeparator();
	// "About" — show the LMMS About dialog
	help_menu->addAction( embed::getIconPixmap( "icon_small" ), tr( "About" ),
				  this, SLOT(aboutLMMS()));

	// === Toolbar buttons — Row 0: Project actions ===

	// "New project" button
	auto project_new = new ToolButton(
		embed::getIconPixmap("project_new"), tr("Create new project"), this, SLOT(createNewProject()), m_toolBar);

	// "New from template" button with instant popup menu
	auto project_new_from_template = new ToolButton(embed::getIconPixmap("project_new_from_template"),
		tr("Create new project from template"), this, SLOT(emptySlot()), m_toolBar);
	project_new_from_template->setMenu( templates_menu );
	project_new_from_template->setPopupMode( ToolButton::InstantPopup );

	// "Open project" button
	auto project_open = new ToolButton(
		embed::getIconPixmap("project_open"), tr("Open existing project"), this, SLOT(openProject()), m_toolBar);

	// "Recent projects" button with instant popup menu
	auto project_open_recent = new ToolButton(embed::getIconPixmap("project_open_recent"),
		tr("Recently opened projects"), this, SLOT(emptySlot()), m_toolBar);
	project_open_recent->setMenu( new RecentProjectsMenu(this) );
	project_open_recent->setPopupMode( ToolButton::InstantPopup );

	// "Save project" button
	auto project_save = new ToolButton(
		embed::getIconPixmap("project_save"), tr("Save current project"), this, SLOT(saveProject()), m_toolBar);

	// "Export project" button
	auto project_export = new ToolButton(
		embed::getIconPixmap("project_export"), tr("Export current project"), this, SLOT(onExportProject()), m_toolBar);

	// Metronome toggle button (checkable on/off)
	m_metronomeToggle = new ToolButton(
				embed::getIconPixmap( "metronome" ),
				tr( "Metronome" ),
				this, SLOT(onToggleMetronome()),
							m_toolBar );
	m_metronomeToggle->setCheckable(true);
	// Initialize checked state from the song's metronome active state
	m_metronomeToggle->setChecked(Engine::getSong()->metronome().active());

	// Layout row 0: project action buttons (columns 1-7, column 0 is spacing)
	m_toolBarLayout->setColumnMinimumWidth( 0, 5 ); // Left margin spacing
	m_toolBarLayout->addWidget( project_new, 0, 1 );
	m_toolBarLayout->addWidget( project_new_from_template, 0, 2 );
	m_toolBarLayout->addWidget( project_open, 0, 3 );
	m_toolBarLayout->addWidget( project_open_recent, 0, 4 );
	m_toolBarLayout->addWidget( project_save, 0, 5 );
	m_toolBarLayout->addWidget( project_export, 0, 6 );
	m_toolBarLayout->addWidget( m_metronomeToggle, 0, 7 );


	// === Toolbar buttons — Row 1: Editor window toggles ===

	// Song Editor toggle (Ctrl+1)
	auto song_editor_window = new ToolButton(embed::getIconPixmap("songeditor"), tr("Song Editor") + " (Ctrl+1)", this,
		SLOT(toggleSongEditorWin()), m_toolBar);
	song_editor_window->setShortcut( Qt::CTRL + Qt::Key_1 );

	// Pattern Editor toggle (Ctrl+2)
	auto pattern_editor_window = new ToolButton(embed::getIconPixmap("pattern_track_btn"),
		tr("Pattern Editor") + " (Ctrl+2)", this, SLOT(togglePatternEditorWin()), m_toolBar);
	pattern_editor_window->setShortcut(Qt::CTRL + Qt::Key_2);

	// Piano Roll toggle (Ctrl+3)
	auto piano_roll_window = new ToolButton(
		embed::getIconPixmap("piano"), tr("Piano Roll") + " (Ctrl+3)", this, SLOT(togglePianoRollWin()), m_toolBar);
	piano_roll_window->setShortcut( Qt::CTRL + Qt::Key_3 );

	// Automation Editor toggle (Ctrl+4)
	auto automation_editor_window = new ToolButton(embed::getIconPixmap("automation"),
		tr("Automation Editor") + " (Ctrl+4)", this, SLOT(toggleAutomationEditorWin()), m_toolBar);
	automation_editor_window->setShortcut( Qt::CTRL + Qt::Key_4 );

	// Mixer toggle (Ctrl+5)
	auto mixer_window = new ToolButton(
		embed::getIconPixmap("mixer"), tr("Mixer") + " (Ctrl+5)", this, SLOT(toggleMixerWin()), m_toolBar);
	mixer_window->setShortcut( Qt::CTRL + Qt::Key_5 );

	// Controller Rack toggle (Ctrl+6)
	auto controllers_window = new ToolButton(embed::getIconPixmap("controller"),
		tr("Show/hide controller rack") + " (Ctrl+6)", this, SLOT(toggleControllerRack()), m_toolBar);
	controllers_window->setShortcut( Qt::CTRL + Qt::Key_6 );

	// Project Notes toggle (Ctrl+7)
	auto project_notes_window = new ToolButton(embed::getIconPixmap("project_notes"),
		tr("Show/hide project notes") + " (Ctrl+7)", this, SLOT(toggleProjectNotesWin()), m_toolBar);
	project_notes_window->setShortcut( Qt::CTRL + Qt::Key_7 );

	// Layout row 1: editor toggle buttons (columns 1-7)
	m_toolBarLayout->addWidget( song_editor_window, 1, 1 );
	m_toolBarLayout->addWidget( pattern_editor_window, 1, 2 );
	m_toolBarLayout->addWidget( piano_roll_window, 1, 3 );
	m_toolBarLayout->addWidget( automation_editor_window, 1, 4 );
	m_toolBarLayout->addWidget( mixer_window, 1, 5 );
	m_toolBarLayout->addWidget( controllers_window, 1, 6 );
	m_toolBarLayout->addWidget( project_notes_window, 1, 7 );
	// Stretch column 100 to push all toolbar buttons to the left
	m_toolBarLayout->setColumnStretch( 100, 1 );

	// === First-run or audio failure: show setup dialog ===
	if( !ConfigManager::inst()->value( "app", "configured" ).toInt() )
	{
		// First time running LMMS — mark as configured and show the setup dialog
		ConfigManager::inst()->setValue( "app", "configured", "1" );
		SetupDialog sd;
		sd.exec();
	}
	// look whether the audio engine failed to start the audio device selected by the
	// user and is using AudioDummy as a fallback
	// or the audio device is set to invalid one
	else if( Engine::audioEngine()->audioDevStartFailed() || !AudioEngine::isAudioDevNameValid(
		ConfigManager::inst()->value( "audioengine", "audiodev" ) ) )
	{
		// Audio device failed — offer the audio settings section of the setup dialog
		SetupDialog sd( SetupDialog::ConfigTab::AudioSettings );
		sd.exec();
	}

	// === Add editor sub-windows to the MDI workspace ===
	for (QWidget* widget :  std::list<QWidget*>{
			getGUI()->automationEditor(),
			getGUI()->patternEditor(),
			getGUI()->pianoRoll(),
			getGUI()->songEditor()
	})
	{
		// Wrap each editor in an MDI sub-window
		QMdiSubWindow* window = addWindowedWidget(widget);
		window->setWindowIcon(widget->windowIcon());
		// Prevent the sub-window from being deleted when closed (just hidden)
		window->setAttribute(Qt::WA_DeleteOnClose, false);
		window->resize(widget->sizeHint());
	}

	// Set initial visibility and positions of editor sub-windows
	getGUI()->automationEditor()->parentWidget()->hide();        // Hidden by default
	getGUI()->patternEditor()->parentWidget()->move(610, 5);     // Positioned to the right
	getGUI()->patternEditor()->parentWidget()->hide();            // Hidden by default
	getGUI()->pianoRoll()->parentWidget()->move(5, 5);            // Top-left
	getGUI()->pianoRoll()->parentWidget()->hide();                // Hidden by default
	getGUI()->songEditor()->parentWidget()->move(5, 5);           // Top-left
	getGUI()->songEditor()->parentWidget()->show();               // Visible by default

	// Reset window title every time we change the state of a subwindow to show the correct title
	for( const QMdiSubWindow * subWindow : workspace()->subWindowList() )
	{
		connect( subWindow, SIGNAL(windowStateChanged(Qt::WindowStates,Qt::WindowStates)), this, SLOT(resetWindowTitle()));
	}
}




/**
 * @brief Adds a widget to the global toolbar at a specific grid position.
 *
 * @param _w   The widget to add to the toolbar
 * @param _row Row index (0 = top row, 1 = bottom row). Pass -1 to span both rows.
 * @param _col Column index. Pass -1 (default) to auto-place after existing columns.
 * @return The column index where the widget was placed.
 *
 * If the widget's height exceeds 32 pixels or _row is -1, the widget spans both rows.
 */
int MainWindow::addWidgetToToolBar( QWidget * _w, int _row, int _col )
{
	// Auto-calculate column if not specified (offset by 7 to avoid collisions with built-in buttons)
	int col = ( _col == -1 ) ? m_toolBarLayout->columnCount() + 7 : _col;
	if( _w->height() > 32 || _row == -1 )
	{
		// Widget is tall or row unspecified — span both rows (row 0, col, rowSpan=2, colSpan=1)
		m_toolBarLayout->addWidget( _w, 0, col, 2, 1 );
	}
	else
	{
		// Place widget in the specified row
		m_toolBarLayout->addWidget( _w, _row, col );
	}
	return( col );
}




/**
 * @brief Adds horizontal spacing to the toolbar layout.
 *
 * @param _size The width in pixels of the spacing to add.
 *
 * Inserts an empty column with minimum width to create visual separation
 * between groups of toolbar buttons.
 */
void MainWindow::addSpacingToToolBar( int _size )
{
	m_toolBarLayout->setColumnMinimumWidth( m_toolBarLayout->columnCount() +
								7, _size );
}




/**
 * @brief Wraps a widget in a custom SubWindow and adds it to the MDI workspace.
 *
 * @param w            The widget to wrap in a sub-window.
 * @param windowFlags  Optional window flags for the sub-window.
 * @return Pointer to the created SubWindow.
 *
 * SubWindow is a custom QMdiSubWindow subclass that patches various
 * Qt bugs (e.g., geometry tracking on X11).
 */
SubWindow* MainWindow::addWindowedWidget(QWidget *w, Qt::WindowFlags windowFlags)
{
	// wrap the widget in our own *custom* window that patches some errors in QMdiSubWindow
	auto win = new SubWindow(m_workspace->viewport(), windowFlags);
	win->setAttribute(Qt::WA_DeleteOnClose);
	win->setWidget(w);
	// Size the sub-window to fit the widget's size hint plus title bar and frame
	if (w && w->sizeHint().isValid()) {
		auto titleBarHeight = win->titleBarHeight();
		auto frameWidth = win->frameWidth();
		QSize delta(2* frameWidth, titleBarHeight + frameWidth);
		win->resize(delta + w->sizeHint());
	}
	m_workspace->addSubWindow(win);
	return win;
}


/**
 * @brief Resets the window title to reflect the current project state.
 *
 * Format: "<ProjectName>[*] - LMMS <version>"
 * - Shows "Untitled" if no project file is loaded
 * - Appends '*' if the project has unsaved modifications
 * - Appends a recovery warning if the session is in Recover state
 */
void MainWindow::resetWindowTitle()
{
	// Default to "Untitled" if no project file is loaded
	QString title(tr( "Untitled" ));

	if( Engine::getSong()->projectFileName() != "" )
	{
		// Extract just the base name (without path or extension) from the project filename
		title = QFileInfo( Engine::getSong()->projectFileName()
							).completeBaseName();
	}

	// Append asterisk to indicate unsaved modifications
	if( Engine::getSong()->isModified() )
	{
		title += '*';
	}

	// Append recovery session warning if applicable
	if( getSession() == SessionState::Recover )
	{
		title += " - " + tr( "Recover session. Please save your work!" );
	}

	// Set the final window title with LMMS version
	setWindowTitle( title + " - " + tr( "LMMS %1" ).arg( LMMS_VERSION ) );
}




/**
 * @brief Checks whether it is safe to change the current project (e.g., before opening a new one).
 *
 * @param stopPlayback If true, stops song playback before prompting.
 * @return true if the user allows the change (saved, discarded, or no changes), false if cancelled.
 *
 * If the project has unsaved changes or the session is in Recover state,
 * shows a message box asking the user to Save, Discard, or Cancel.
 */
bool MainWindow::mayChangeProject(bool stopPlayback)
{
	if( stopPlayback )
	{
		Engine::getSong()->stop();
	}

	// No unsaved changes and not recovering — safe to proceed
	if( !Engine::getSong()->isModified() && getSession() != SessionState::Recover )
	{
		return( true );
	}

	// Different message text for recovered vs. normally modified projects
	QString messageTitleRecovered = tr( "Recovered project not saved" );
	QString messageRecovered = tr( "This project was recovered from the "
					"previous session. It is currently "
					"unsaved and will be lost if you don't "
					"save it. Do you want to save it now?" );

	QString messageTitleUnsaved = tr( "Project not saved" );
	QString messageUnsaved = tr( "The current project was modified since "
					"last saving. Do you want to save it "
								"now?" );

	// Show a Save/Discard/Cancel message box
	QMessageBox mb( ( getSession() == SessionState::Recover ?
				messageTitleRecovered : messageTitleUnsaved ),
			( getSession() == SessionState::Recover ?
					messageRecovered : messageUnsaved ),
				QMessageBox::Question,
				QMessageBox::Save,
				QMessageBox::Discard,
				QMessageBox::Cancel,
				this );
	int answer = mb.exec();

	if( answer == QMessageBox::Save )
	{
		// Attempt to save; return whether save succeeded
		return( saveProject() );
	}
	else if( answer == QMessageBox::Discard )
	{
		// If recovering, clean up recovery session files
		if( getSession() == SessionState::Recover )
		{
			sessionCleanup();
		}
		return( true );
	}

	// User cancelled — do not allow the project change
	return( false );
}




/**
 * @brief Resets all tracked key modifier states to false.
 *
 * Called when the window loses focus to prevent stuck modifier keys,
 * since key release events may be missed when focus is lost.
 */
void MainWindow::clearKeyModifiers()
{
	m_keyMods.m_ctrl = false;
	m_keyMods.m_shift = false;
	m_keyMods.m_alt = false;
}




/**
 * @brief Saves the position, size, and visibility state of a widget to an XML element.
 *
 * @param _w  The widget whose state to save (may be redirected to its parent QMdiSubWindow).
 * @param _de The DOM element to write attributes into.
 *
 * Stores: visible, minimized, maximized, x, y, width, height.
 * Uses SubWindow::getTrueNormalGeometry() when available to work around
 * a Qt bug on X11 (QTBUG-256) where normalGeometry() returns incorrect values.
 */
void MainWindow::saveWidgetState( QWidget * _w, QDomElement & _de )
{
	// If our widget is the main content of a window (e.g. piano roll, Mixer, etc),
	// we really care about the position of the *window* - not the position of the widget within its window
	if( _w->parentWidget() != nullptr &&
			_w->parentWidget()->inherits( "QMdiSubWindow" ) )
	{
		_w = _w->parentWidget();
	}

	// If the widget is a SubWindow, then we can make use of the getTrueNormalGeometry() method that
	// performs the same as normalGeometry, but isn't broken on X11 ( see https://bugreports.qt.io/browse/QTBUG-256 )
	auto asSubWindow = qobject_cast<SubWindow*>(_w);
	QRect normalGeom = asSubWindow != nullptr ? asSubWindow->getTrueNormalGeometry() : _w->normalGeometry();

	// Write visibility and window state attributes
	bool visible = _w->isVisible();
	_de.setAttribute( "visible", visible );
	_de.setAttribute( "minimized", _w->isMinimized() );
	_de.setAttribute( "maximized", _w->isMaximized() );

	// Write position attributes
	_de.setAttribute( "x", normalGeom.x() );
	_de.setAttribute( "y", normalGeom.y() );

	// Write size attributes
	QSize sizeToStore = normalGeom.size();
	_de.setAttribute( "width", sizeToStore.width() );
	_de.setAttribute( "height", sizeToStore.height() );
}




/**
 * @brief Restores the position, size, and visibility state of a widget from an XML element.
 *
 * @param _w  The widget to restore (may be redirected to its parent QMdiSubWindow).
 * @param _de The DOM element containing the saved state attributes.
 *
 * Reads: visible, minimized, maximized, x, y, width, height from the element.
 * Ensures minimum size constraints are respected and handles maximized/minimized states.
 */
void MainWindow::restoreWidgetState( QWidget * _w, const QDomElement & _de )
{
	// Build a rect from saved attributes, enforcing minimum sizes
	QRect r( qMax( 1, _de.attribute( "x" ).toInt() ),
			qMax( 1, _de.attribute( "y" ).toInt() ),
			qMax( _w->sizeHint().width(), _de.attribute( "width" ).toInt() ),
			qMax( _w->minimumHeight(), _de.attribute( "height" ).toInt() ) );
	if( _de.hasAttribute( "visible" ) && !r.isNull() )
	{
		// If our widget is the main content of a window (e.g. piano roll, Mixer, etc),
		// we really care about the position of the *window* - not the position of the widget within its window
		if ( _w->parentWidget() != nullptr &&
			_w->parentWidget()->inherits( "QMdiSubWindow" ) )
		{
			_w = _w->parentWidget();
		}
		// first restore the window, as attempting to resize a maximized window causes graphics glitching
		_w->setWindowState( _w->windowState() & ~(Qt::WindowMaximized | Qt::WindowMinimized) );

		// Check isEmpty() to work around corrupt project files with empty size
		if ( ! r.size().isEmpty() ) {
			_w->resize( r.size() );
		}
		_w->move( r.topLeft() );

		// set the window to its correct minimized/maximized/restored state
		Qt::WindowStates flags = _w->windowState();
		flags = _de.attribute( "minimized" ).toInt() ?
				( flags | Qt::WindowMinimized ) :
				( flags & ~Qt::WindowMinimized );
		flags = _de.attribute( "maximized" ).toInt() ?
				( flags | Qt::WindowMaximized ) :
				( flags & ~Qt::WindowMaximized );
		_w->setWindowState( flags );

		// Restore visibility
		_w->setVisible( _de.attribute( "visible" ).toInt() );
	}
}



/**
 * @brief No-op slot used as a placeholder for buttons that only have popup menus.
 *
 * Buttons like "New from Template" and "Recent Projects" use InstantPopup mode,
 * but Qt still requires a connected slot for the button's clicked signal.
 */
void MainWindow::emptySlot()
{
}



/**
 * @brief Slot: Creates a new blank project after prompting to save unsaved changes.
 */
void MainWindow::createNewProject()
{
	if( mayChangeProject(true) )
	{
		Engine::getSong()->createNewProject();
	}
}




/**
 * @brief Slot: Opens a project file via a file dialog.
 *
 * Prompts the user to save unsaved changes (without stopping playback),
 * then shows a file dialog to select an .mmp or .mmpz file.
 */
void MainWindow::openProject()
{
	if( mayChangeProject(false) )
	{
		FileDialog ofd( this, tr( "Open Project" ), "", tr( "LMMS (*.mmp *.mmpz)" ) );

		// Default to the user's projects directory
		ofd.setDirectory( ConfigManager::inst()->userProjectsDir() );
		ofd.setFileMode( FileDialog::ExistingFiles );
		if( ofd.exec () == QDialog::Accepted &&
						!ofd.selectedFiles().isEmpty() )
		{
			Song *song = Engine::getSong();

			// Stop playback, show wait cursor while loading
			song->stop();
			setCursor( Qt::WaitCursor );
			song->loadProject( ofd.selectedFiles()[0] );
			setCursor( Qt::ArrowCursor );
		}
	}
}




/**
 * @brief Slot: Saves the current project.
 *
 * If the project has no filename yet (never been saved), delegates to saveProjectAs().
 * Otherwise saves to the current filename and cleans up recovery state if needed.
 *
 * @return true if the project was saved successfully, false otherwise.
 */
bool MainWindow::saveProject()
{
	if( Engine::getSong()->projectFileName() == "" )
	{
		// No filename yet — show Save As dialog
		return( saveProjectAs() );
	}
	else if( this->guiSaveProject() )
	{
		// Save succeeded — clean up recovery session if applicable
		if( getSession() == SessionState::Recover )
		{
			sessionCleanup();
		}
		return true;
	}
	return false;
}




/**
 * @brief Slot: Saves the current project with a new filename via a versioned save dialog.
 *
 * Shows a VersionedSaveDialog that supports .mmpz, .mmp, and .mpt (template) formats.
 * The default suffix is determined by the "app/nommpz" config setting.
 *
 * @return true if the project was saved successfully, false otherwise.
 */
bool MainWindow::saveProjectAs()
{
	// Create save options widget for the dialog (compression, etc.)
	auto optionsWidget = new SaveOptionsWidget(Engine::getSong()->getSaveOptions());
	VersionedSaveDialog sfd( this, optionsWidget, tr( "Save Project" ), "",
			tr( "LMMS Project" ) + " (*.mmpz *.mmp);;" +
				tr( "LMMS Project Template" ) + " (*.mpt)" );
	QString f = Engine::getSong()->projectFileName();
	if( f != "" )
	{
		// Pre-select the current file's directory and name
		sfd.setDirectory( QFileInfo( f ).absolutePath() );
		sfd.selectFile( QFileInfo( f ).fileName() );
	}
	else
	{
		// Default to user's projects directory
		sfd.setDirectory( ConfigManager::inst()->userProjectsDir() );
	}

	// Don't write over file with suffix if no suffix is provided.
	// Use .mmpz (compressed) unless the user has opted out via config
	QString suffix = ConfigManager::inst()->value( "app",
							"nommpz" ).toInt() == 0
						? "mmpz"
						: "mmp" ;
	sfd.setDefaultSuffix( suffix );

	if( sfd.exec () == FileDialog::Accepted &&
		!sfd.selectedFiles().isEmpty() && sfd.selectedFiles()[0] != "" )
	{
		QString fname = sfd.selectedFiles()[0] ;
		// If saving as template (.mpt), handle the extension properly
		if( sfd.selectedNameFilter().contains( "(*.mpt)" ) )
		{
			// Remove the default suffix that was auto-appended
			fname.remove( "." + suffix );
			if( !sfd.selectedFiles()[0].endsWith( ".mpt" ) )
			{
				// Check if the .mpt file already exists before appending extension
				if( VersionedSaveDialog::fileExistsQuery( fname + ".mpt",
						tr( "Save project template" ) ) )
				{
					fname += ".mpt";
				}
			}
		}
		if( this->guiSaveProjectAs( fname ) )
		{
			// Save succeeded — clean up recovery session if applicable
			if( getSession() == SessionState::Recover )
			{
				sessionCleanup();
			}
			return true;
		}
	}
	return false;
}




/**
 * @brief Slot: Saves the project with an auto-incremented version number in the filename.
 *
 * If the project has never been saved, falls back to saveProjectAs().
 * Otherwise, increments the version suffix (e.g., "song-02" -> "song-03")
 * until a non-existing filename is found, then saves to that filename.
 *
 * @return true if the project was saved successfully, false otherwise.
 */
bool MainWindow::saveProjectAsNewVersion()
{
	QString fileName = Engine::getSong()->projectFileName();
	if( fileName == "" )
	{
		return saveProjectAs();
	}
	else
	{
		// Increment version number in filename until we find one that doesn't exist
		do 		VersionedSaveDialog::changeFileNameVersion( fileName, true );
		while 	( QFile( fileName ).exists() );

		return this->guiSaveProjectAs( fileName );
	}
}




/**
 * @brief Slot: Saves the current project as the default template (~/.lmms/templates/default.mpt).
 *
 * If the default template already exists, prompts the user for confirmation before overwriting.
 */
void MainWindow::saveProjectAsDefaultTemplate()
{
	QString defaultTemplate = ConfigManager::inst()->userTemplateDir() + "default.mpt";

	QFileInfo fileInfo(defaultTemplate);
	if (fileInfo.exists())
	{
		// Warn the user before overwriting the existing default template
		if (QMessageBox::warning(this,
					 tr("Overwrite default template?"),
					 tr("This will overwrite your current default template."),
					 QMessageBox::Ok,
					 QMessageBox::Cancel) != QMessageBox::Ok)
		{
			return;
		}
	}

	Engine::getSong()->saveProjectFile( defaultTemplate );
}


void MainWindow::saveProjectAsDefaultTemplateNoPatterns()
{
	// Determine the default project suffix from config (mmpz or mmp)
	QString suffix = ConfigManager::inst()->value("app", "nommpz").toInt() == 0 ? "mmpz" : "mmp";

	// Show a save dialog defaulting to the project directory
	FileDialog sfd(this, tr("Save without patterns"),
		ConfigManager::inst()->userProjectsDir(),
		tr("LMMS Project") + " (*.mmpz *.mmp)");
	sfd.setAcceptMode(FileDialog::AcceptSave);
	sfd.setFileMode(FileDialog::AnyFile);
	sfd.setDefaultSuffix(suffix);

	if (sfd.exec() != QDialog::Accepted
		|| sfd.selectedFiles().isEmpty()
		|| sfd.selectedFiles().first().isEmpty())
	{
		return;
	}

	QString fname = sfd.selectedFiles().first();

	// Ensure a recognized project extension
	if (!fname.endsWith(".mmp") && !fname.endsWith(".mmpz"))
	{
		fname += "." + suffix;
	}

	// Check for overwrite and prompt the user
	if (QFile::exists(fname))
	{
		if (QMessageBox::warning(this,
					 tr("Overwrite file?"),
					 tr("The file \"%1\" already exists. Overwrite it?").arg(QFileInfo(fname).fileName()),
					 QMessageBox::Ok,
					 QMessageBox::Cancel) != QMessageBox::Ok)
		{
			return;
		}
	}

	// Step 1: Save the full project to the target file
	Engine::getSong()->saveProjectFile(fname);

	// Step 2: Re-read the saved file as XML DOM for manipulation
	DataFile dataFile(fname);
	QDomElement content = dataFile.content();

	// Step 3: Strip pattern data from the XML DOM
	QDomElement trackContainer = content.firstChildElement("trackcontainer");
	if (!trackContainer.isNull())
	{
		bool firstPatternTrackKept = false;
		QDomNode node = trackContainer.firstChild();
		while (!node.isNull())
		{
			QDomNode next = node.nextSibling();
			QDomElement elem = node.toElement();
			if (!elem.isNull() && elem.tagName() == "track"
				&& elem.attribute("type").toInt() == static_cast<int>(Track::Type::Pattern))
			{
				if (!firstPatternTrackKept)
				{
					firstPatternTrackKept = true;

					// Remove PatternClip elements from this track
					QDomNode trackChild = elem.firstChild();
					while (!trackChild.isNull())
					{
						QDomNode trackChildNext = trackChild.nextSibling();
						QDomElement trackChildElem = trackChild.toElement();
						if (!trackChildElem.isNull() && trackChildElem.tagName() == "patternclip")
						{
							elem.removeChild(trackChild);
						}
						trackChild = trackChildNext;
					}

					// Inside <patterntrack>, find <trackcontainer> (PatternStore)
					// and strip clip elements from each instrument track
					QDomElement patternTrackSettings = elem.firstChildElement("patterntrack");
					QDomElement patternStoreTC = patternTrackSettings.firstChildElement("trackcontainer");
					if (!patternStoreTC.isNull())
					{
						QDomNode psTrack = patternStoreTC.firstChild();
						while (!psTrack.isNull())
						{
							QDomElement psTrackElem = psTrack.toElement();
							if (!psTrackElem.isNull() && psTrackElem.tagName() == "track")
							{
								static const QStringList clipTagNames = {
									"midiclip", "sampleclip", "automationclip", "patternclip"
								};
								QDomNode clipNode = psTrackElem.firstChild();
								while (!clipNode.isNull())
								{
									QDomNode clipNext = clipNode.nextSibling();
									QDomElement clipElem = clipNode.toElement();
									if (!clipElem.isNull() && clipTagNames.contains(clipElem.tagName()))
									{
										psTrackElem.removeChild(clipNode);
									}
									clipNode = clipNext;
								}
							}
							psTrack = psTrack.nextSibling();
						}
					}
				}
				else
				{
					// Remove all additional PatternTracks beyond the first
					trackContainer.removeChild(node);
				}
			}
			node = next;
		}
	}

	// Step 4: Re-write the file without pattern data
	dataFile.writeFile(fname);
}


/**
 * @brief Slot: Saves the current project as a SQLite .lmms-db file.
 *
 * Prompts the user for a filename, then converts the current project's XML DOM
 * to SQLite format using XmlToSqlite::convert().
 */
void MainWindow::saveProjectAsSqlite()
{
	FileDialog sfd(this, tr("Save as SQLite"),
		ConfigManager::inst()->userProjectsDir(),
		tr("LMMS SQLite Project") + " (*.lmms-db)");
	sfd.setAcceptMode(FileDialog::AcceptSave);
	sfd.setFileMode(FileDialog::AnyFile);
	sfd.setDefaultSuffix("lmms-db");

	// Pre-populate with current project name but .lmms-db extension
	QString f = Engine::getSong()->projectFileName();
	if (!f.isEmpty())
	{
		sfd.setDirectory(QFileInfo(f).absolutePath());
		QString baseName = QFileInfo(f).completeBaseName();
		sfd.selectFile(baseName + ".lmms-db");
	}

	if (sfd.exec() != QDialog::Accepted
		|| sfd.selectedFiles().isEmpty()
		|| sfd.selectedFiles().first().isEmpty())
	{
		return;
	}

	QString fname = sfd.selectedFiles().first();
	if (!fname.endsWith(".lmms-db"))
	{
		fname += ".lmms-db";
	}

	// Check for overwrite
	if (QFile::exists(fname))
	{
		if (QMessageBox::warning(this,
					 tr("Overwrite file?"),
					 tr("The file \"%1\" already exists. Overwrite it?").arg(QFileInfo(fname).fileName()),
					 QMessageBox::Ok,
					 QMessageBox::Cancel) != QMessageBox::Ok)
		{
			return;
		}
	}

	fprintf(stderr, "[MainWindow] Saving project as SQLite: %s\n", qPrintable(fname));

	// Use the existing save pipeline — Song::saveProjectFile builds the full XML DOM
	// and DataFile::writeFile detects the .lmms-db extension and routes through XmlToSqlite
	bool ok = Engine::getSong()->saveProjectFile(fname);
	if (ok)
	{
		fprintf(stderr, "[MainWindow] SQLite save successful: %s\n", qPrintable(fname));
		TextFloat::displayMessage(tr("SQLite Export"),
			tr("Project saved as %1").arg(QFileInfo(fname).fileName()),
			embed::getIconPixmap("project_save"), 3000);
	}
	else
	{
		fprintf(stderr, "[MainWindow] SQLite save FAILED: %s\n", qPrintable(fname));
		QMessageBox::critical(this, tr("SQLite Export Failed"),
			tr("Failed to save project as SQLite file.\nCheck the console for details."));
	}
}



/**
 * @brief Slot: Opens the application settings dialog (SetupDialog).
 */
void MainWindow::showSettingsDialog()
{
	SetupDialog sd;
	sd.exec();
}




/**
 * @brief Slot: Shows the LMMS About dialog with version and credits.
 */
void MainWindow::aboutLMMS()
{
	AboutDialog(this).exec();
}




/**
 * @brief Slot: Shows a placeholder help message (currently unused; online help is preferred).
 */
void MainWindow::help()
{
	QMessageBox::information( this, tr( "Help not available" ),
				  tr( "Currently there's no help "
						  "available in LMMS.\n"
						  "Please visit "
						  "http://lmms.sf.net/wiki "
						  "for documentation on LMMS." ),
				  QMessageBox::Ok );
}




/**
 * @brief Toggles the visibility of an editor sub-window in the MDI workspace.
 *
 * @param window    The editor widget to toggle (e.g., Song Editor, Piano Roll).
 * @param forceShow If true, always show the window regardless of current state.
 *
 * If the window is already the active sub-window and visible, it is hidden.
 * Otherwise, it is shown and given focus.
 *
 * Includes a workaround for Qt Bug #260116 by resetting scrollbar policies.
 */
void MainWindow::toggleWindow( QWidget *window, bool forceShow )
{
	// Get the MDI sub-window parent
	QWidget *parent = window->parentWidget();

	if( forceShow ||
		m_workspace->activeSubWindow() != parent ||
		parent->isHidden() )
	{
		// Show the window and give it focus
		parent->show();
		window->show();
		window->setFocus();
	}
	else
	{
		// Hide the window and try to refocus another visible editor
		parent->hide();
		refocus();
	}

	// Workaround for Qt Bug #260116: toggle scrollbar policies to force refresh
	m_workspace->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
	m_workspace->setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
	m_workspace->setHorizontalScrollBarPolicy( Qt::ScrollBarAsNeeded );
	m_workspace->setVerticalScrollBarPolicy( Qt::ScrollBarAsNeeded );
}



/**
 * @brief Slot: Toggles between fullscreen and normal/maximized window state (F11).
 *
 * Remembers whether the window was maximized before entering fullscreen,
 * so that exiting fullscreen restores the correct previous state.
 */
void MainWindow::toggleFullscreen()
{
	if ( !isFullScreen() )
	{
		// Remember if we were maximized before going fullscreen
		maximized = isMaximized();
		showFullScreen();
	}
	else
	{
		// Restore to maximized or normal, depending on previous state
		maximized ? showMaximized() : showNormal();
	}
}



/*
 * When an editor window with focus is toggled off, attempt to set focus
 * to the next visible editor window, or if none are visible, set focus
 * to the parent window.
 */
/**
 * @brief Sets focus to the first visible editor sub-window, or to MainWindow if none are visible.
 *
 * Called after hiding an editor window to ensure keyboard focus is not lost.
 * Checks editors in priority order: Song Editor, Pattern Editor, Piano Roll, Automation Editor.
 */
void MainWindow::refocus()
{
	const auto gui = getGUI();

	// Attempt to set the focus on the first of these editors that is not hidden...
	for (auto editorParent : { gui->songEditor()->parentWidget(), gui->patternEditor()->parentWidget(),
		gui->pianoRoll()->parentWidget(), gui->automationEditor()->parentWidget() })
	{
		if (!editorParent->isHidden())
		{
			editorParent->setFocus();
			return;
		}
	}

	// ... otherwise set the focus on the main window.
	this->setFocus();
}




/**
 * @brief Slot: Toggles visibility of the Pattern Editor sub-window.
 *
 * @param forceShow If true, the window is always shown (never hidden).
 */
void MainWindow::togglePatternEditorWin( bool forceShow )
{
	toggleWindow( getGUI()->patternEditor(), forceShow );
}




/**
 * @brief Slot: Toggles visibility of the Song Editor sub-window.
 */
void MainWindow::toggleSongEditorWin()
{
	toggleWindow( getGUI()->songEditor() );
}




/**
 * @brief Slot: Toggles visibility of the Project Notes sub-window.
 */
void MainWindow::toggleProjectNotesWin()
{
	toggleWindow( getGUI()->getProjectNotes() );
}




/**
 * @brief Slot: Toggles visibility of the Piano Roll sub-window.
 */
void MainWindow::togglePianoRollWin()
{
	toggleWindow( getGUI()->pianoRoll() );
}




/**
 * @brief Slot: Toggles visibility of the Automation Editor sub-window.
 */
void MainWindow::toggleAutomationEditorWin()
{
	toggleWindow( getGUI()->automationEditor() );
}




/**
 * @brief Slot: Toggles visibility of the Mixer sub-window.
 */
void MainWindow::toggleMixerWin()
{
	toggleWindow( getGUI()->mixerView() );
}



/**
 * @brief Slot: Toggles visibility of the Microtuner configuration sub-window.
 */
void MainWindow::toggleMicrotunerWin()
{
	toggleWindow( getGUI()->getMicrotunerConfig() );
}




/**
 * @brief Slot: Rebuilds the View menu contents when it is about to be shown.
 *
 * Populates the menu with:
 *   - Editor window toggle actions (Song Editor through Project Notes, with shortcuts)
 *   - Fullscreen toggle (F11)
 *   - Checkable display options (dBFS display, smooth scroll, note labels)
 *
 * These checkable items persist their state via ConfigManager and are
 * handled by updateConfig() when triggered.
 */
void MainWindow::updateViewMenu()
{
	// Clear the menu to rebuild it fresh each time
	m_viewMenu->clear();
	// TODO: get current visibility for these and indicate in menu?
	// Not that it's straight visible <-> invisible, more like
	// not on top -> top <-> invisible

	// Editor toggle actions with keyboard shortcut hints
	m_viewMenu->addAction(embed::getIconPixmap( "songeditor" ),
			      tr( "Song Editor" ) + "\tCtrl+1",
			      this, SLOT(toggleSongEditorWin())
		);
	m_viewMenu->addAction(embed::getIconPixmap("pattern_track"),
					tr("Pattern Editor") + "\tCtrl+2",
					this, SLOT(togglePatternEditorWin())
		);
	m_viewMenu->addAction(embed::getIconPixmap( "piano" ),
			      tr( "Piano Roll" ) + "\tCtrl+3",
			      this, SLOT(togglePianoRollWin())
		);
	m_viewMenu->addAction(embed::getIconPixmap( "automation" ),
			      tr( "Automation Editor" ) + "\tCtrl+4",
			      this,
			      SLOT(toggleAutomationEditorWin())
		);
	m_viewMenu->addAction(embed::getIconPixmap( "mixer" ),
			      tr( "Mixer" ) + "\tCtrl+5",
			      this, SLOT(toggleMixerWin())
		);
	m_viewMenu->addAction(embed::getIconPixmap( "controller" ),
			      tr( "Controller Rack" ) + "\tCtrl+6",
			      this, SLOT(toggleControllerRack())
		);
	m_viewMenu->addAction(embed::getIconPixmap( "project_notes" ),
			      tr( "Project Notes" ) + "\tCtrl+7",
			      this, SLOT(toggleProjectNotesWin())
		);

	m_viewMenu->addSeparator();

	// Fullscreen toggle
	m_viewMenu->addAction(embed::getIconPixmap( "fullscreen" ),
				tr( "Fullscreen" ) + "\tF11",
				this, SLOT(toggleFullscreen())
		);

	m_viewMenu->addSeparator();

	// --- Checkable display/UI options ---
	// Here we should put all look&feel -stuff from configmanager
	// that is safe to change on the fly. There is probably some
	// more elegant way to do this.

	// Volume display mode: dBFS vs linear percentage
	auto qa = new QAction(tr("Volume as dBFS"), this);
	qa->setData("displaydbfs");             // Tag used by updateConfig() to identify this option
	qa->setCheckable( true );
	qa->setChecked( ConfigManager::inst()->value( "app", "displaydbfs" ).toInt() );
	m_viewMenu->addAction(qa);

	// Smooth scrolling in timeline views
	qa = new QAction(tr( "Smooth scroll" ), this);
	qa->setData("smoothscroll");
	qa->setCheckable( true );
	qa->setChecked( ConfigManager::inst()->value( "ui", "smoothscroll" ).toInt() );
	m_viewMenu->addAction(qa);

	// Not yet.
	/* qa = new QAction(tr( "One instrument track window" ), this);
	qa->setData("oneinstrument");
	qa->setCheckable( true );
	qa->setChecked( ConfigManager::inst()->value( "ui", "oneinstrumenttrackwindow" ).toInt() );
	m_viewMenu->addAction(qa);
	*/

	// Show note name labels on piano roll keys
	qa = new QAction(tr( "Enable note labels in piano roll" ), this);
	qa->setData("printnotelabels");
	qa->setCheckable( true );
	qa->setChecked( ConfigManager::inst()->value( "ui", "printnotelabels" ).toInt() );
	m_viewMenu->addAction(qa);

}




/**
 * @brief Slot: Handles toggling of checkable config items in the View menu.
 *
 * @param _who The QAction that was triggered, containing a data tag identifying the config option.
 *
 * Reads the action's data tag and checked state, then writes the corresponding
 * value to ConfigManager. Supported tags: "displaydbfs", "tooltips",
 * "smoothscroll", "oneinstrument", "printnotelabels".
 */
void MainWindow::updateConfig( QAction * _who )
{
	QString tag = _who->data().toString(); // Config option identifier
	bool checked = _who->isChecked();      // New checked state

	if( tag == "displaydbfs" )
	{
		// Toggle between dBFS and linear volume display
		ConfigManager::inst()->setValue( "app", "displaydbfs",
						 QString::number(checked) );
	}
	else if ( tag == "tooltips" )
	{
		// Toggle tooltip visibility (note: stored as "disabled", so inverted)
		ConfigManager::inst()->setValue( "tooltips", "disabled",
						 QString::number(!checked) );

		// Install or remove the global tooltip-blocking event filter
		if (checked) { qApp->removeEventFilter(this); }
		else { qApp->installEventFilter(this); }

	}
	else if ( tag == "smoothscroll" )
	{
		// Toggle smooth scrolling in timeline views
		ConfigManager::inst()->setValue( "ui", "smoothscroll",
						 QString::number(checked) );
	}
	else if ( tag == "oneinstrument" )
	{
		// Toggle single instrument track window mode (not yet exposed in UI)
		ConfigManager::inst()->setValue( "ui", "oneinstrumenttrackwindow",
						 QString::number(checked) );
	}
	else if ( tag == "printnotelabels" )
	{
		// Toggle note name labels on piano roll keys
		ConfigManager::inst()->setValue( "ui", "printnotelabels",
						 QString::number(checked) );
	}
}



/**
 * @brief Slot: Toggles the metronome on or off based on the toolbar button's checked state.
 */
void MainWindow::onToggleMetronome()
{
	Engine::getSong()->metronome().setActive(m_metronomeToggle->isChecked());
}




/**
 * @brief Slot: Toggles visibility of the Controller Rack sub-window.
 */
void MainWindow::toggleControllerRack()
{
	toggleWindow( getGUI()->getControllerRackView() );
}




/**
 * @brief Slot: Updates play/pause icons across all editor windows based on current playback state.
 *
 * Resets all editors' pause icons to false, then sets the pause icon to true
 * on whichever editor corresponds to the current play mode (Song, AutomationClip,
 * Pattern, or MidiClip).
 */
void MainWindow::updatePlayPauseIcons()
{
	// Reset all editors to "not playing" state
	getGUI()->songEditor()->setPauseIcon( false );
	getGUI()->automationEditor()->setPauseIcon( false );
	getGUI()->patternEditor()->setPauseIcon( false );
	getGUI()->pianoRoll()->setPauseIcon( false );

	// Set pause icon on the editor that is currently playing
	if( Engine::getSong()->isPlaying() )
	{
		switch( Engine::getSong()->playMode() )
		{
			case Song::PlayMode::Song:
				getGUI()->songEditor()->setPauseIcon( true );
				break;

			case Song::PlayMode::AutomationClip:
				getGUI()->automationEditor()->setPauseIcon( true );
				break;

			case Song::PlayMode::Pattern:
				getGUI()->patternEditor()->setPauseIcon( true );
				break;

			case Song::PlayMode::MidiClip:
				getGUI()->pianoRoll()->setPauseIcon( true );
				break;

			default:
				break;
		}
	}
}


/**
 * @brief Slot: Updates the enabled state of Undo/Redo actions based on journal availability.
 *
 * Called just before the Edit menu is shown (via aboutToShow signal).
 * Greys out Undo if there is nothing to undo, and Redo if nothing to redo.
 */
void MainWindow::updateUndoRedoButtons()
{
	// when the edit menu is shown, grey out the undo/redo buttons if there's nothing to undo/redo
	// else, un-grey them
	m_undoAction->setEnabled(Engine::projectJournal()->canUndo());
	m_redoAction->setEnabled(Engine::projectJournal()->canRedo());
}



/**
 * @brief Slot: Performs an undo operation via the project journal.
 */
void MainWindow::undo()
{
	Engine::projectJournal()->undo();
}




/**
 * @brief Slot: Performs a redo operation via the project journal.
 */
void MainWindow::redo()
{
	Engine::projectJournal()->redo();
}




/**
 * @brief Handles the window close event.
 *
 * @param _ce The close event to accept or ignore.
 *
 * Prompts the user to save unsaved changes via mayChangeProject().
 * If allowed, deletes the recovery file and accepts the close event.
 * Otherwise, ignores the event to keep the window open.
 */
void MainWindow::closeEvent( QCloseEvent * _ce )
{
	if( mayChangeProject(true) )
	{
		// delete recovery file
		if( ConfigManager::inst()->
				value( "ui", "enableautosave" ).toInt() )
		{
			sessionCleanup();
			_ce->accept();
		}
	}
	else
	{
		// User cancelled — keep the window open
		_ce->ignore();
	}
}




/**
 * @brief Cleans up recovery session state.
 *
 * Deletes the auto-save recovery file and resets the session state to Normal.
 * Called after a successful save or when the user discards a recovered project.
 */
void MainWindow::sessionCleanup()
{
	// delete recover session files
	QFile::remove( ConfigManager::inst()->recoveryFile() );
	setSession( SessionState::Normal );
}




/**
 * @brief Global event filter to block tooltip events when tooltips are disabled.
 *
 * @param watched The object that the event was sent to.
 * @param event   The event to filter.
 * @return true if the event should be filtered out (blocked), false to pass it through.
 *
 * Installed on QApplication when the "tooltips/disabled" config is set to 1.
 * Intercepts QEvent::ToolTip events and suppresses them.
 */
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
	// For now this function is only used to globally block tooltips
	// It must be installed to QApplication through installEventFilter
	if (event->type() == QEvent::ToolTip) { return true; }

	// Pass all other events through to the default handler
	return QObject::eventFilter(watched, event);
}




/**
 * @brief Handles focus-out events by clearing tracked key modifier states.
 *
 * @param _fe The focus event (passed to parent class handler).
 *
 * When the window loses focus, key release events are no longer received,
 * so modifier key states (Ctrl, Shift, Alt) could become stuck. This
 * clears them preemptively.
 *
 * @note TODO: This function is apparently never actually called — investigate removal.
 */
void MainWindow::focusOutEvent( QFocusEvent * _fe )
{
	// TODO Remove this function, since it is apparently never actually called!
	// when loosing focus we do not receive key-(release!)-events anymore,
	// so we might miss release-events of one the modifiers we're watching!
	clearKeyModifiers();
	QMainWindow::leaveEvent( _fe );
}




/**
 * @brief Handles key press events for modifier tracking and piano input.
 *
 * @param _ke The key event.
 *
 * Tracks Ctrl, Shift, and Alt modifier keys in the m_keyMods struct.
 * For all other keys, forwards the event to the top-level instrument track
 * window's piano view for live keyboard playing. If the piano view does
 * not accept the event, passes it to the QMainWindow base class.
 */
void MainWindow::keyPressEvent( QKeyEvent * _ke )
{
	switch( _ke->key() )
	{
		case Qt::Key_Control: m_keyMods.m_ctrl = true; break;   // Track Ctrl state
		case Qt::Key_Shift: m_keyMods.m_shift = true; break;    // Track Shift state
		case Qt::Key_Alt: m_keyMods.m_alt = true; break;        // Track Alt state
		default:
		{
			// Forward non-modifier keys to the topmost instrument's piano view
			InstrumentTrackWindow * w =
						InstrumentTrackView::topLevelInstrumentTrackWindow();
			if( w )
			{
				w->pianoView()->keyPressEvent( _ke );
			}
			// If the piano view didn't accept the key, let the base class handle it
			if( !_ke->isAccepted() )
			{
				QMainWindow::keyPressEvent( _ke );
			}
		}
	}
}




/**
 * @brief Handles key release events for modifier tracking and piano input.
 *
 * @param _ke The key event.
 *
 * Clears Ctrl, Shift, and Alt modifier states on release.
 * For other keys, forwards to the piano view for note-off handling.
 */
void MainWindow::keyReleaseEvent( QKeyEvent * _ke )
{
	switch( _ke->key() )
	{
		case Qt::Key_Control: m_keyMods.m_ctrl = false; break;  // Clear Ctrl state
		case Qt::Key_Shift: m_keyMods.m_shift = false; break;   // Clear Shift state
		case Qt::Key_Alt: m_keyMods.m_alt = false; break;       // Clear Alt state
		default:
			// Forward non-modifier key releases to the piano view for note-off
			if( InstrumentTrackView::topLevelInstrumentTrackWindow() )
			{
				InstrumentTrackView::topLevelInstrumentTrackWindow()->
					pianoView()->keyReleaseEvent( _ke );
			}
			// If the piano view didn't accept the key, let the base class handle it
			if( !_ke->isAccepted() )
			{
				QMainWindow::keyReleaseEvent( _ke );
			}
	}
}




/**
 * @brief Handles timer events by emitting the periodicUpdate signal.
 *
 * @param _te The timer event (from m_updateTimer, firing at ~60 fps).
 *
 * The periodicUpdate signal drives periodic UI refreshes such as VU meter
 * displays, playback position indicators, and other animated widgets.
 */
void MainWindow::timerEvent( QTimerEvent * _te)
{
	emit periodicUpdate();
}





/**
 * @brief Slot: Shows a tool plugin's view when its menu item is clicked.
 *
 * @param _idx The QAction from the Tools menu that was triggered.
 *
 * Looks up the corresponding PluginView by index in the m_tools list,
 * shows it, shows its parent widget, and gives it focus.
 */
void MainWindow::showTool( QAction * _idx )
{
	// Map the action to its index in the tools menu, then look up the corresponding view
	PluginView * p = m_tools[m_toolsMenu->actions().indexOf( _idx )];
	p->show();
	p->parentWidget()->show();
	p->setFocus();
}




/**
 * @brief Slot: Opens the LMMS online documentation in the user's default web browser.
 */
void MainWindow::browseHelp()
{
	// file:// alternative for offline help
	QString url = "https://lmms.io/documentation/";
	QDesktopServices::openUrl( url );
	// TODO: Handle error
}




/**
 * @brief Slot: Performs an auto-save of the current project to the recovery file.
 *
 * Auto-save is skipped if any of the following conditions are true:
 *   - The song is currently being exported
 *   - A project is being loaded
 *   - The main thread is waiting on a remote plugin
 *   - A mouse button is being held down (user is in the middle of a drag)
 *   - The song is playing and "enablerunningautosave" config is not set
 *
 * If auto-save is skipped, the timer is shortened to retry in 10 seconds.
 * On successful save, the timer is reset to the full configured interval.
 */
void MainWindow::autoSave()
{
	if( !Engine::getSong()->isExporting() &&
		!Engine::getSong()->isLoadingProject() &&
		!RemotePluginBase::isMainThreadWaiting() &&
		!QApplication::mouseButtons() &&
		( ConfigManager::inst()->value( "ui",
				"enablerunningautosave" ).toInt() ||
			! Engine::getSong()->isPlaying() ) )
	{
		// Save to the recovery file and reset timer to full interval
		Engine::getSong()->saveProjectFile(ConfigManager::inst()->recoveryFile());
		autoSaveTimerReset();  // Reset timer
	}
	else
	{
		// Conditions not met — try again in 10 seconds instead of the full interval
		if( getAutoSaveTimerInterval() != m_autoSaveShortTime )
		{
			autoSaveTimerReset( m_autoSaveShortTime );
		}
	}
}

/**
 * @brief Slot: Exports the current project as a MIDI file via a save dialog.
 *
 * Shows a FileDialog for selecting the output .mid file, defaulting to the
 * current project's directory and base filename. Delegates the actual export
 * to Song::exportProjectMidi().
 */
void MainWindow::onExportProjectMidi()
{
	FileDialog efd( this );

	efd.setFileMode( FileDialog::AnyFile );

	// Only MIDI file type is supported
	QStringList types;
	types << tr("MIDI File (*.mid)");
	efd.setNameFilters( types );

	// Determine base filename from current project or use "untitled"
	QString base_filename;
	QString const & projectFileName = Engine::getSong()->projectFileName();
	if( !projectFileName.isEmpty() )
	{
		efd.setDirectory( QFileInfo( projectFileName ).absolutePath() );
		base_filename = QFileInfo( projectFileName ).completeBaseName();
	}
	else
	{
		efd.setDirectory( ConfigManager::inst()->userProjectsDir() );
		base_filename = tr( "untitled" );
	}
	efd.selectFile( base_filename + ".mid" );
	efd.setDefaultSuffix( "mid");
	efd.setWindowTitle( tr( "Select file for project-export..." ) );

	efd.setAcceptMode( FileDialog::AcceptSave );


	if( efd.exec() == QDialog::Accepted && !efd.selectedFiles().isEmpty() && !efd.selectedFiles()[0].isEmpty() )
	{
		const QString suffix = ".mid";

		QString export_filename = efd.selectedFiles()[0];
		// Ensure the .mid extension is present
		if (!export_filename.endsWith(suffix)) export_filename += suffix;

		// Perform the MIDI export
		Engine::getSong()->exportProjectMidi(export_filename);
	}
}

/**
 * @brief Exports the current project as audio file(s).
 *
 * @param multiExport If true, exports each track as a separate audio file
 *                    into a user-selected directory. If false (default),
 *                    exports the full mix as a single audio file.
 *
 * For single export: shows available audio formats (WAV, OGG, etc.) from
 * ProjectRenderer::fileEncodeDevices. For multi-export: shows a directory
 * selection dialog. In both cases, opens an ExportProjectDialog to perform
 * the actual rendering.
 */
void MainWindow::exportProject(bool multiExport)
{
	QString const & projectFileName = Engine::getSong()->projectFileName();

	FileDialog efd( getGUI()->mainWindow() );

	if ( multiExport )
	{
		// Multi-export: select a directory for per-track export
		efd.setFileMode( FileDialog::Directory);
		efd.setWindowTitle( tr( "Select directory for writing exported tracks..." ) );
		if( !projectFileName.isEmpty() )
		{
			efd.setDirectory( QFileInfo( projectFileName ).absolutePath() );
		}
	}
	else
	{
		// Single export: select a file with audio format filter
		efd.setFileMode( FileDialog::AnyFile );
		int idx = 0;
		QStringList types;
		// Build list of available audio export formats
		while( ProjectRenderer::fileEncodeDevices[idx].m_fileFormat != ProjectRenderer::ExportFileFormat::Count)
		{
			if(ProjectRenderer::fileEncodeDevices[idx].isAvailable()) {
				types << tr(ProjectRenderer::fileEncodeDevices[idx].m_description);
			}
			++idx;
		}
		efd.setNameFilters( types );

		// Set default filename based on current project or "untitled"
		QString baseFilename;
		if( !projectFileName.isEmpty() )
		{
			efd.setDirectory( QFileInfo( projectFileName ).absolutePath() );
			baseFilename = QFileInfo( projectFileName ).completeBaseName();
		}
		else
		{
			efd.setDirectory( ConfigManager::inst()->userProjectsDir() );
			baseFilename = tr( "untitled" );
		}
		// Pre-select filename with the first available format's extension
		efd.selectFile( baseFilename + ProjectRenderer::fileEncodeDevices[0].m_extension );
		efd.setWindowTitle( tr( "Select file for project-export..." ) );
	}

	// Default to WAV format
	QString suffix = "wav";
	efd.setDefaultSuffix( suffix );
	efd.setAcceptMode( FileDialog::AcceptSave );

	if( efd.exec() == QDialog::Accepted && !efd.selectedFiles().isEmpty() &&
					 !efd.selectedFiles()[0].isEmpty() )
	{

		QString exportFileName = efd.selectedFiles()[0];
		if ( !multiExport )
		{
			// Extract the file extension from the selected name filter
			int stx = efd.selectedNameFilter().indexOf( "(*." );
			int etx = efd.selectedNameFilter().indexOf( ")" );

			if ( stx > 0 && etx > stx )
			{
				// Get first extension from selected dropdown.
				// i.e. ".wav" from "WAV-File (*.wav), Dummy-File (*.dum)"
				suffix = efd.selectedNameFilter().mid( stx + 2, etx - stx - 2 ).split( " " )[0].trimmed();

				// Case sensitivity depends on platform (macOS/Windows are case-insensitive)
				Qt::CaseSensitivity cs = Qt::CaseSensitive;
#if defined(LMMS_BUILD_APPLE) || defined(LMMS_BUILD_WIN32)
				cs = Qt::CaseInsensitive;
#endif
				// Remove the suffix if it was auto-appended, to handle overwrite check
				exportFileName.remove( "." + suffix, cs );
				if ( efd.selectedFiles()[0].endsWith( suffix ) )
				{
					// Check for existing file before re-appending suffix
					if( VersionedSaveDialog::fileExistsQuery( exportFileName + suffix,
							tr( "Save project" ) ) )
					{
						exportFileName += suffix;
					}
				}
			}
		}

		// Open the export dialog which handles rendering in a background thread
		ExportProjectDialog epd( exportFileName, getGUI()->mainWindow(), multiExport );
		epd.exec();
	}
}

/**
 * @brief Displays a success or failure notification after saving a project.
 *
 * @param filename                The path of the file that was saved (or attempted).
 * @param songSavedSuccessfully   Whether the save operation succeeded.
 *
 * On success: shows a TextFloat notification, adds the file to recent projects,
 * and resets the window title. On failure: shows an error notification.
 */
void MainWindow::handleSaveResult(QString const & filename, bool songSavedSuccessfully)
{
	if (songSavedSuccessfully)
	{
		// Show a brief "Project saved" floating notification
		TextFloat::displayMessage( tr( "Project saved" ), tr( "The project %1 is now saved.").arg( filename ),
				embed::getIconPixmap( "project_save", 24, 24 ), 2000 );
		// Add to the recently opened projects list for the File menu
		ConfigManager::inst()->addRecentlyOpenedProject(filename);
		// Update window title to remove the '*' modified indicator
		resetWindowTitle();
	}
	else
	{
		// Show an error floating notification
		TextFloat::displayMessage( tr( "Project NOT saved." ), tr( "The project %1 was not saved!" ).arg(filename),
				embed::getIconPixmap( "error" ), 4000 );
	}
}

/**
 * @brief Saves the project to its current filename with GUI feedback.
 *
 * Delegates to Song::guiSaveProject() and displays the result via handleSaveResult().
 *
 * @return true if the save succeeded, false otherwise.
 */
bool MainWindow::guiSaveProject()
{
	Song * song = Engine::getSong();
	bool const songSaveResult = song->guiSaveProject();
	handleSaveResult(song->projectFileName(), songSaveResult);

	return songSaveResult;
}

/**
 * @brief Saves the project to a new filename with GUI feedback.
 *
 * @param filename The new file path to save to.
 * @return true if the save succeeded, false otherwise.
 *
 * Delegates to Song::guiSaveProjectAs() and displays the result via handleSaveResult().
 */
bool MainWindow::guiSaveProjectAs( const QString & filename )
{
	Song * song = Engine::getSong();
	bool const songSaveResult = song->guiSaveProjectAs(filename);
	handleSaveResult(filename, songSaveResult);

	return songSaveResult;
}

/**
 * @brief Slot: Initiates a single-file audio export.
 *
 * Wrapper that calls exportProject() with multiExport=false (default).
 */
void MainWindow::onExportProject()
{
	this->exportProject();
}

/**
 * @brief Slot: Initiates a multi-track audio export (one file per track).
 *
 * Wrapper that calls exportProject() with multiExport=true.
 */
void MainWindow::onExportProjectTracks()
{
	this->exportProject(true);
}

/**
 * @brief Slot: Imports an external file (MIDI or Hydrogen) into the current song.
 *
 * Shows a file dialog for selecting .mid, .midi, .rmi (MIDI), or .h2song (Hydrogen) files.
 * Delegates the actual import to ImportFilter::import(). After import, disables the
 * "load on launch" flag so the imported project is not automatically reloaded.
 */
void MainWindow::onImportProject()
{
	Song * song = Engine::getSong();

	if (song)
	{
		FileDialog ofd( nullptr, tr( "Import file" ),
				ConfigManager::inst()->userProjectsDir(),
				tr("MIDI sequences") +
				" (*.mid *.midi *.rmi);;" +
				tr("Hydrogen projects") +
				" (*.h2song);;" +
				tr("All file types") +
				" (*.*)");

		ofd.setFileMode( FileDialog::ExistingFiles );
		if( ofd.exec () == QDialog::Accepted && !ofd.selectedFiles().isEmpty() )
		{
			// Perform the import using the appropriate filter
			ImportFilter::import( ofd.selectedFiles()[0], song );
		}

		// Prevent automatic reload of this imported file on next launch
		song->setLoadOnLaunch(false);
	}
}

/**
 * @brief Slot: Updates the window title when the song's modified state changes.
 *
 * Only performs the update if called from the GUI main thread, since the Song
 * can be marked as modified from audio/worker threads. This is a design
 * limitation noted in the original implementation.
 */
void MainWindow::onSongModified()
{
	// Only update the window title if the code is executed from the GUI main thread.
	// The assumption seems to be that the Song can also be set as modified from other
	// threads. This is not a good design! Copied from the original implementation of
	// Song::setModified.
	if(QThread::currentThread() == this->thread())
	{
		this->resetWindowTitle();
	}
}

/**
 * @brief Slot: Updates the window title when the project filename changes.
 *
 * Connected to Song::projectFileNameChanged, which fires after Save As
 * or when a new project is loaded.
 */
void MainWindow::onProjectFileNameChanged()
{
	this->resetWindowTitle();
}


} // namespace lmms::gui
