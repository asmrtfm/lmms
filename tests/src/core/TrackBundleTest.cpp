/*
 * TrackBundleTest.cpp - tests for TrackBundle export/import functionality
 *
 * Copyright (c) 2024 LMMS Developers
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

#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QTemporaryFile>

#include "AudioTrackBase.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "DataFile.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "SampleTrack.h"
#include "Song.h"
#include "TrackBundle.h"


class TrackBundleTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		using namespace lmms;
		Engine::init(true);
	}

	void cleanupTestCase()
	{
		using namespace lmms;
		Engine::destroy();
	}

	//
	// Test 1: AudioTrackBase hierarchy — InstrumentTrack inherits correctly
	//
	void testAudioTrackBaseInheritance()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		InstrumentTrack it(song);

		// Verify that the AudioTrackBase accessors work
		QVERIFY(it.volumeModel() != nullptr);
		QVERIFY(it.panningModel() != nullptr);
		QVERIFY(it.mixerChannelModel() != nullptr);
		QVERIFY(it.audioPort() != nullptr);

		// Check default values
		QCOMPARE(it.volumeModel()->value(), 100.0f);
		QCOMPARE(it.panningModel()->value(), 0.0f);
		QCOMPARE(it.mixerChannelModel()->value(), 0);

		// Verify dynamic_cast to AudioTrackBase works
		auto* base = dynamic_cast<AudioTrackBase*>(&it);
		QVERIFY(base != nullptr);
		QCOMPARE(base->volumeModel()->value(), 100.0f);
	}

	//
	// Test 2: SampleTrack also inherits AudioTrackBase
	//
	void testSampleTrackInheritance()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		SampleTrack st(song);

		QVERIFY(st.volumeModel() != nullptr);
		QVERIFY(st.panningModel() != nullptr);
		QVERIFY(st.mixerChannelModel() != nullptr);
		QVERIFY(st.audioPort() != nullptr);

		auto* base = dynamic_cast<AudioTrackBase*>(&st);
		QVERIFY(base != nullptr);
	}

	//
	// Test 3: InstrumentTrack save/load round-trip preserves audio settings
	//
	void testInstrumentTrackSaveLoad()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		InstrumentTrack srcTrack(song);

		// Set non-default values
		srcTrack.setName("TestTrack");
		srcTrack.volumeModel()->setValue(75.0f);
		srcTrack.panningModel()->setValue(-25.0f);

		// Save to XML
		QDomDocument doc;
		QDomElement parent = doc.createElement("test");
		doc.appendChild(parent);
		srcTrack.saveState(doc, parent);

		// Create new track and load
		InstrumentTrack dstTrack(song);
		QDomElement trackElem = parent.firstChildElement("track");
		QVERIFY(!trackElem.isNull());
		dstTrack.restoreState(trackElem);

		// Verify
		QCOMPARE(dstTrack.name(), QString("TestTrack"));
		QCOMPARE(dstTrack.volumeModel()->value(), 75.0f);
		QCOMPARE(dstTrack.panningModel()->value(), -25.0f);
	}

	//
	// Test 4: SampleTrack save/load round-trip preserves audio settings
	//
	void testSampleTrackSaveLoad()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		SampleTrack srcTrack(song);

		srcTrack.setName("TestSample");
		srcTrack.volumeModel()->setValue(80.0f);
		srcTrack.panningModel()->setValue(30.0f);

		QDomDocument doc;
		QDomElement parent = doc.createElement("test");
		doc.appendChild(parent);
		srcTrack.saveState(doc, parent);

		SampleTrack dstTrack(song);
		QDomElement trackElem = parent.firstChildElement("track");
		QVERIFY(!trackElem.isNull());
		dstTrack.restoreState(trackElem);

		QCOMPARE(dstTrack.name(), QString("TestSample"));
		QCOMPARE(dstTrack.volumeModel()->value(), 80.0f);
		QCOMPARE(dstTrack.panningModel()->value(), 30.0f);
	}

	//
	// Test 5: TrackBundle export creates a valid DataFile
	//
	void testTrackBundleExport()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		InstrumentTrack track(song);
		track.setName("BundleTest");
		track.volumeModel()->setValue(42.0f);

		QTemporaryFile tmpFile;
		tmpFile.setAutoRemove(true);
		tmpFile.open();
		QString filePath = tmpFile.fileName() + ".lmms-track";
		tmpFile.close();

		bool result = TrackBundle::exportTrack(&track, filePath);
		QVERIFY(result);

		// Verify the file was created
		QVERIFY(QFile::exists(filePath));

		// Verify it's a valid DataFile
		DataFile dataFile(filePath);
		QCOMPARE(dataFile.type(), DataFile::Type::TrackBundle);

		// Verify the track element exists in the content
		QDomElement content = dataFile.content();
		QDomElement trackElem = content.firstChildElement("track");
		QVERIFY(!trackElem.isNull());

		// Clean up
		QFile::remove(filePath);
	}

	//
	// Test 6: TrackBundle import creates a new track
	//
	void testTrackBundleImport()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		InstrumentTrack srcTrack(song);
		srcTrack.setName("ImportTest");
		srcTrack.volumeModel()->setValue(55.0f);

		QTemporaryFile tmpFile;
		tmpFile.setAutoRemove(true);
		tmpFile.open();
		QString filePath = tmpFile.fileName() + ".lmms-track";
		tmpFile.close();

		// Export
		TrackBundle::exportTrack(&srcTrack, filePath);

		// Count tracks before import
		int trackCountBefore = song->tracks().size();

		// Import
		auto imported = TrackBundle::importBundle(filePath, song);
		QVERIFY(!imported.isEmpty());

		// Verify a new track was created
		QCOMPARE(song->tracks().size(), trackCountBefore + 1);

		// Clean up
		QFile::remove(filePath);
	}

	//
	// Test 7: DataFile type enum includes TrackBundle
	//
	void testDataFileTrackBundleType()
	{
		using namespace lmms;

		DataFile df(DataFile::Type::TrackBundle);
		QCOMPARE(df.type(), DataFile::Type::TrackBundle);
	}

	//
	// Test 8: Mixer channel definition round-trip
	//
	void testMixerChannelExportImport()
	{
		using namespace lmms;

		auto* mixer = Engine::mixer();
		int chIdx = mixer->createChannel();
		auto* ch = mixer->mixerChannel(chIdx);
		ch->m_name = "TestMixerChannel";
		ch->m_volumeModel.setValue(0.8f);

		auto song = Engine::getSong();
		InstrumentTrack track(song);
		track.setName("MixerTest");
		track.mixerChannelModel()->setValue(chIdx);

		QTemporaryFile tmpFile;
		tmpFile.setAutoRemove(true);
		tmpFile.open();
		QString filePath = tmpFile.fileName() + ".lmms-track";
		tmpFile.close();

		// Export
		TrackBundle::exportTrack(&track, filePath);

		// Verify the bundle contains a mixer channel definition
		DataFile dataFile(filePath);
		QDomElement content = dataFile.content();
		QDomElement mixerDef = content.firstChildElement("bundled_mixer_channel");
		QVERIFY(!mixerDef.isNull());
		QCOMPARE(mixerDef.attribute("name"), QString("TestMixerChannel"));
		QCOMPARE(mixerDef.attribute("original_index").toInt(), chIdx);

		QFile::remove(filePath);
	}

	//
	// Test 9: Automation clip detection for track
	//
	void testFindAutomationForTrack()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		InstrumentTrack track(song);
		track.setName("AutomationTarget");

		// Create an automation track that targets this track's volume
		AutomationTrack autoTrack(song);
		auto* clip = dynamic_cast<AutomationClip*>(autoTrack.createClip(0));
		QVERIFY(clip != nullptr);
		clip->addObject(track.volumeModel());
		clip->putValue(0, 50.0f, false);
		clip->putValue(100, 100.0f, false);

		// Verify the automation is found
		auto models = track.findChildren<AutomatableModel*>();
		QVERIFY(!models.isEmpty());

		// Check that clipsForModel finds the automation
		auto clips = AutomationClip::clipsForModel(track.volumeModel());
		QVERIFY(!clips.empty());
		QCOMPARE(clips.size(), static_cast<size_t>(1));
	}

	//
	// Test 10: Group export with multiple tracks
	//
	void testGroupExport()
	{
		using namespace lmms;

		auto song = Engine::getSong();
		InstrumentTrack track1(song);
		track1.setName("GroupTrack1");
		track1.volumeModel()->setValue(60.0f);

		InstrumentTrack track2(song);
		track2.setName("GroupTrack2");
		track2.volumeModel()->setValue(80.0f);

		QTemporaryFile tmpFile;
		tmpFile.setAutoRemove(true);
		tmpFile.open();
		QString filePath = tmpFile.fileName() + ".lmms-track";
		tmpFile.close();

		QList<lmms::Track*> tracks;
		tracks.append(&track1);
		tracks.append(&track2);

		bool result = TrackBundle::exportGroup(tracks, filePath);
		QVERIFY(result);

		// Verify the file contains 2 track elements
		DataFile dataFile(filePath);
		QDomElement content = dataFile.content();
		QDomNodeList trackNodes = content.elementsByTagName("track");
		QCOMPARE(trackNodes.count(), 2);

		QFile::remove(filePath);
	}
};

QTEST_GUILESS_MAIN(TrackBundleTest)
#include "TrackBundleTest.moc"
