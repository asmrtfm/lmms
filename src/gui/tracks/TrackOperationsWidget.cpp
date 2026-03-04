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

#include "TrackOperationsWidget.h"

#include <QFileDialog>
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
#include "TrackBundle.h"
#include "TrackContainerView.h"
#include "TrackView.h"

namespace lmms::gui
{

/*! \brief Create a new trackOperationsWidget
 *
 * The trackOperationsWidget is the grip and the mute button of a track.
 *
 * \param parent the trackView to contain this widget
 */
TrackOperationsWidget::TrackOperationsWidget( TrackView * parent ) :
	QWidget( parent ),             /*!< The parent widget */
	m_trackView( parent )          /*!< The parent track view */
{
	setToolTip(tr("Press <%1> while clicking on move-grip "
				"to begin a new drag'n'drop action." ).arg(UI_CTRL_KEY) );

	auto toMenu = new QMenu(this);
	connect( toMenu, SIGNAL(aboutToShow()), this, SLOT(updateMenu()));


	setObjectName( "automationEnabled" );


	m_trackOps = new QPushButton( this );
	m_trackOps->move( 12, 1 );
	m_trackOps->setFocusPolicy( Qt::NoFocus );
	m_trackOps->setMenu( toMenu );
	m_trackOps->setToolTip(tr("Actions"));


	m_muteBtn = new PixmapButton( this, tr( "Mute" ) );
	m_muteBtn->setActiveGraphic( embed::getIconPixmap( "led_off" ) );
	m_muteBtn->setInactiveGraphic( embed::getIconPixmap( "led_green" ) );
	m_muteBtn->setCheckable( true );

	m_soloBtn = new PixmapButton( this, tr( "Solo" ) );
	m_soloBtn->setActiveGraphic( embed::getIconPixmap( "led_red" ) );
	m_soloBtn->setInactiveGraphic( embed::getIconPixmap( "led_off" ) );
	m_soloBtn->setCheckable( true );

	if( ConfigManager::inst()->value( "ui",
					  "compacttrackbuttons" ).toInt() )
	{
		m_muteBtn->move( 46, 0 );
		m_soloBtn->move( 46, 16 );
	}
	else
	{
		m_muteBtn->move( 46, 8 );
		m_soloBtn->move( 62, 8 );
	}

	m_muteBtn->show();
	m_muteBtn->setToolTip(tr("Mute"));

	m_soloBtn->show();
	m_soloBtn->setToolTip(tr("Solo"));

	connect( this, SIGNAL(trackRemovalScheduled(lmms::gui::TrackView*)),
			m_trackView->trackContainerView(),
				SLOT(deleteTrackView(lmms::gui::TrackView*)),
							Qt::QueuedConnection );

	connect( m_trackView->getTrack()->getMutedModel(), SIGNAL(dataChanged()),
			this, SLOT(update()));

	connect(m_trackView->getTrack(), SIGNAL(colorChanged()), this, SLOT(update()));
}







/*! \brief Respond to trackOperationsWidget mouse events
 *
 *  If it's the left mouse button, and Ctrl is held down, and we're
 *  not a Pattern Editor track, then start a new drag event to
 *  copy this track.
 *
 *  Otherwise, ignore all other events.
 *
 *  \param me The mouse event to respond to.
 */
void TrackOperationsWidget::mousePressEvent( QMouseEvent * me )
{
	if( me->button() == Qt::LeftButton &&
		me->modifiers() & Qt::ControlModifier &&
			m_trackView->getTrack()->type() != Track::Type::Pattern)
	{
		DataFile dataFile( DataFile::Type::DragNDropData );
		m_trackView->getTrack()->saveState( dataFile, dataFile.content() );
		new StringPairDrag( QString( "track_%1" ).arg(
					static_cast<int>(m_trackView->getTrack()->type()) ),
			dataFile.toString(), m_trackView->getTrackSettingsWidget()->grab(),
									this );
	}
	else if( me->button() == Qt::LeftButton )
	{
		// track-widget (parent-widget) initiates track-move
		me->ignore();
	}
}




/*! \brief Repaint the trackOperationsWidget
 *
 *  If we're not moving, and in the Pattern Editor, then turn
 *  automation on or off depending on its previous state and show
 *  ourselves.
 *
 *  Otherwise, hide ourselves.
 *
 *  \todo Flesh this out a bit - is it correct?
 *  \param pe The paint event to respond to
 */
void TrackOperationsWidget::paintEvent( QPaintEvent * pe )
{
	QPainter p( this );

	p.fillRect(rect(), palette().brush(QPalette::Window));

	if (m_trackView->getTrack()->color().has_value() && !m_trackView->getTrack()->getMutedModel()->value()) 
	{
		QRect coloredRect( 0, 0, 10, m_trackView->getTrack()->getHeight() );

		p.fillRect(coloredRect, m_trackView->getTrack()->color().value());
	}

	p.drawPixmap(2, 2, embed::getIconPixmap(m_trackView->isMovingTrack() ? "track_op_grip_c" : "track_op_grip"));
}


/*! \brief Show a message box warning the user that this track is about to be closed */
bool TrackOperationsWidget::confirmRemoval()
{
	bool needConfirm = ConfigManager::inst()->value("ui", "trackdeletionwarning", "1").toInt();
	if (!needConfirm){ return true; }
	
	QString messageRemoveTrack = tr("After removing a track, it can not "
					"be recovered. Are you sure you want to remove track \"%1\"?")
					.arg(m_trackView->getTrack()->name());
	QString messageTitleRemoveTrack = tr("Confirm removal");
	QString askAgainText = tr("Don't ask again");
	auto askAgainCheckBox = new QCheckBox(askAgainText, nullptr);
	connect(askAgainCheckBox, &QCheckBox::stateChanged, [](int state){
		// Invert button state, if it's checked we *shouldn't* ask again
		ConfigManager::inst()->setValue("ui", "trackdeletionwarning", state ? "0" : "1");
	});

	QMessageBox mb(this);
	mb.setText(messageRemoveTrack);
	mb.setWindowTitle(messageTitleRemoveTrack);
	mb.setIcon(QMessageBox::Warning);
	mb.addButton(QMessageBox::Cancel);
	mb.addButton(QMessageBox::Ok);
	mb.setCheckBox(askAgainCheckBox);
	mb.setDefaultButton(QMessageBox::Cancel);

	int answer = mb.exec();

	if( answer == QMessageBox::Ok )
	{
		return true;
	}
	return false;
}



/*! \brief Clone this track
 *
 */
void TrackOperationsWidget::cloneTrack()
{
	TrackContainerView *tcView = m_trackView->trackContainerView();

	Track *newTrack = m_trackView->getTrack()->clone();
	TrackView *newTrackView = tcView->createTrackView( newTrack );

	int index = tcView->trackViews().indexOf( m_trackView );
	int i = tcView->trackViews().size();
	while ( i != index + 1 )
	{
		tcView->moveTrackView( newTrackView, i - 1 );
		i--;
	}

	if (m_soloBtn->model()->value())
	{
		// if this track was solo, make the new track the new solo
		newTrack->toggleSolo();
	}
}


/*! \brief Clear this track - clears all Clips from the track */
void TrackOperationsWidget::clearTrack()
{
	Track * t = m_trackView->getTrack();
	t->addJournalCheckPoint();
	t->lock();
	t->deleteClips();
	t->unlock();
}



/*! \brief Remove this track from the track list
 *
 */
void TrackOperationsWidget::removeTrack()
{
	if (confirmRemoval())
	{
		emit trackRemovalScheduled(m_trackView);
	}
}

void TrackOperationsWidget::selectTrackColor()
{
	const auto newColor = ColorChooser{this}
		.withPalette(ColorChooser::Palette::Track)
		->getColor(m_trackView->getTrack()->color().value_or(Qt::white));

	if (!newColor.isValid()) { return; }

	const auto track = m_trackView->getTrack();
	track->addJournalCheckPoint();
	track->setColor(newColor);
	Engine::getSong()->setModified();
}

void TrackOperationsWidget::resetTrackColor()
{
	auto track = m_trackView->getTrack();
	track->addJournalCheckPoint();
	track->setColor(std::nullopt);
	Engine::getSong()->setModified();
}

void TrackOperationsWidget::randomizeTrackColor()
{
	QColor buffer = ColorChooser::getPalette( ColorChooser::Palette::Track )[ rand() % 48 ];
	auto track = m_trackView->getTrack();
	track->addJournalCheckPoint();
	track->setColor(buffer);
	Engine::getSong()->setModified();
}

void TrackOperationsWidget::resetClipColors()
{
	auto track = m_trackView->getTrack();
	track->addJournalCheckPoint();
	for (auto clip : track->getClips())
	{
		clip->setColor(std::nullopt);
	}
	Engine::getSong()->setModified();
}

/*! \brief Update the trackOperationsWidget context menu
 *
 *  For all track types, we have the Clone and Remove options.
 *  For instrument-tracks we also offer the MIDI-control-menu
 *  For automation tracks, extra options: turn on/off recording
 *  on all Clips (same should be added for sample tracks when
 *  sampletrack recording is implemented)
 */
void TrackOperationsWidget::updateMenu()
{
	QMenu * toMenu = m_trackOps->menu();
	toMenu->clear();
	toMenu->addAction( embed::getIconPixmap( "edit_copy", 16, 16 ),
						tr( "Clone this track" ),
						this, SLOT(cloneTrack()));
	toMenu->addAction( embed::getIconPixmap( "cancel", 16, 16 ),
						tr( "Remove this track" ),
						this, SLOT(removeTrack()));

	if( ! m_trackView->trackContainerView()->fixedClips() )
	{
		toMenu->addAction( tr( "Clear this track" ), this, SLOT(clearTrack()));
	}
	if (QMenu *mixerMenu = m_trackView->createMixerMenu(tr("Channel %1: %2"), tr("Assign to new Mixer Channel")))
	{
		toMenu->addMenu(mixerMenu);
	}

	if (auto trackView = dynamic_cast<InstrumentTrackView*>(m_trackView))
	{
		toMenu->addSeparator();
		toMenu->addMenu(trackView->midiMenu());
	}
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

	// Track template export/import
	if (m_trackView->getTrack()->type() == Track::Type::Instrument
		|| m_trackView->getTrack()->type() == Track::Type::Sample)
	{
		toMenu->addAction(tr("Export track as template..."),
		                  this, SLOT(exportTrackAsTemplate()));
	}
	toMenu->addAction(tr("Import track template..."),
	                  this, SLOT(importTrackTemplate()));

	toMenu->addSeparator();

	QMenu* colorMenu = toMenu->addMenu(tr("Track color"));
	colorMenu->setIcon(embed::getIconPixmap("colorize"));
	colorMenu->addAction(tr("Change"), this, SLOT(selectTrackColor()));
	colorMenu->addAction(tr("Reset"), this, SLOT(resetTrackColor()));
	colorMenu->addAction(tr("Pick random"), this, SLOT(randomizeTrackColor()));
	colorMenu->addSeparator();
	colorMenu->addAction(tr("Reset clip colors"), this, SLOT(resetClipColors()));
}


void TrackOperationsWidget::toggleRecording( bool on )
{
	auto atv = dynamic_cast<AutomationTrackView*>(m_trackView);
	if( atv )
	{
		for( Clip * clip : atv->getTrack()->getClips() )
		{
			auto ap = dynamic_cast<AutomationClip*>(clip);
			if( ap ) { ap->setRecording( on ); }
		}
		atv->update();
	}
}



void TrackOperationsWidget::recordingOn()
{
	toggleRecording( true );
}


void TrackOperationsWidget::recordingOff()
{
	toggleRecording( false );
}


void TrackOperationsWidget::exportTrackAsTemplate()
{
	QString fileName = QFileDialog::getSaveFileName(this,
		tr("Export Track Template"),
		QString(),
		tr("LMMS Track Bundle (*.lmms-track)"));

	if (fileName.isEmpty()) { return; }

	if (!fileName.endsWith(".lmms-track"))
	{
		fileName += ".lmms-track";
	}

	if (!TrackBundle::exportTrack(m_trackView->getTrack(), fileName))
	{
		QMessageBox::warning(this, tr("Export Failed"),
			tr("Could not export track template."));
	}
}


void TrackOperationsWidget::importTrackTemplate()
{
	QString fileName = QFileDialog::getOpenFileName(this,
		tr("Import Track Template"),
		QString(),
		tr("LMMS Track Bundle (*.lmms-track)"));

	if (fileName.isEmpty()) { return; }

	Engine::audioEngine()->requestChangeInModel();
	auto tracks = TrackBundle::importBundle(fileName,
		m_trackView->getTrack()->trackContainer());
	Engine::audioEngine()->doneChangeInModel();

	if (tracks.isEmpty())
	{
		QMessageBox::warning(this, tr("Import Failed"),
			tr("Could not import track template."));
	}
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
