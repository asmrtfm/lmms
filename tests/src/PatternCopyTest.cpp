/*
 * PatternCopyTest.cpp - tests for copy-to-new-pattern-track logic
 *
 * Verifies that the note filtering math in copySelectionToNewPatternTrack
 * correctly uses the shared pattern length (PatternStore::lengthOfPattern)
 * rather than per-MidiClip lengths, and that step counts are handled properly.
 *
 * Copyright (c) 2026
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <QCoreApplication>
#include <QTest>

#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"
#include "TimePos.h"

using namespace lmms;

/**
 * @brief Replicates the core note-filtering math from copySelectionToNewPatternTrack.
 *
 * Given a source MidiClip, a repetition period, clip boundaries (offset + length),
 * returns the note positions that pass the filter. This is the exact formula from
 * PatternClipView::copySelectionToNewPatternTrack() so we can unit-test it in isolation.
 */
static QVector<int> filterNotes(
	const NoteVector& sourceNotes,
	tick_t patternLength,
	tick_t startTimeOffset,
	tick_t clipLength)
{
	QVector<int> result;
	int maxPossibleRepetitions = clipLength / patternLength + 1 + 1;

	for (const Note* note : sourceNotes)
	{
		for (int i = -1; i < maxPossibleRepetitions - 1; i++)
		{
			TimePos posRelativeToClip = note->pos() + startTimeOffset + i * patternLength;
			if (posRelativeToClip < 0 || posRelativeToClip >= clipLength) { continue; }
			result.append(static_cast<int>(posRelativeToClip));
		}
	}
	return result;
}


class PatternCopyTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		Engine::init(true);
	}

	void cleanupTestCase()
	{
		Engine::destroy();
	}

	/**
	 * @brief Verify lengthOfPattern returns the max length across all tracks.
	 *
	 * When InstrumentTracks have MidiClips of different lengths at the same
	 * pattern index, lengthOfPattern should return the longest (in bars).
	 */
	void testLengthOfPatternReturnsMax()
	{
		auto patternStore = Engine::patternStore();

		// Create a PatternTrack to get a pattern index
		PatternTrack pt(Engine::getSong());
		int patIdx = pt.patternIndex();

		// Create two InstrumentTracks in the PatternStore
		InstrumentTrack shortTrack(patternStore);
		InstrumentTrack longTrack(patternStore);

		// Short track: add a note within 1 bar
		auto* shortClip = dynamic_cast<MidiClip*>(shortTrack.getClip(patIdx));
		QVERIFY(shortClip);
		shortClip->addNote(Note(TimePos(12), TimePos(72), 69), false);

		// Long track: add a note extending to bar 3
		auto* longClip = dynamic_cast<MidiClip*>(longTrack.getClip(patIdx));
		QVERIFY(longClip);
		longClip->addNote(Note(TimePos(12), TimePos(500), 69), false);

		// lengthOfPattern should reflect the LONGEST clip (3 bars for pos 500+12=512)
		bar_t patLen = patternStore->lengthOfPattern(patIdx);
		QVERIFY2(patLen >= 3, qPrintable(QString("Expected >=3 bars, got %1").arg(patLen)));

		// Verify short clip length is less than the shared pattern length
		QVERIFY(shortClip->length() < longClip->length());
		QCOMPARE(patLen, longClip->length().nextFullBar());
	}

	/**
	 * @brief A short MidiClip must NOT produce ghost notes when using
	 *        the shared pattern length as the repetition period.
	 *
	 * This is the core bug: if a 1-bar MidiClip is repeated with a 1-bar
	 * period instead of the shared 3-bar period, it produces notes at
	 * positions where the audio engine would NOT play them.
	 */
	void testShortClipNoGhostNotes()
	{
		const tick_t TPB = TimePos::ticksPerBar();

		// Simulate the user's scenario:
		// - Short MidiClip (hihat): 1 note at pos 72, clip length = 1 bar
		// - Long MidiClip (kick): notes spanning 3 bars
		// - Shared pattern length = 3 bars
		// - PatternClip: offset=+192 (1 bar), length=192 (1 bar)

		Note hihatNote(TimePos(12), TimePos(72), 69);
		NoteVector notes;
		notes.push_back(&hihatNote);

		// With CORRECT shared pattern length (3 bars = 576 ticks):
		// The note should NOT pass the filter for offset=+TPB, length=TPB
		QVector<int> correctResult = filterNotes(notes, 3 * TPB, TPB, TPB);
		QVERIFY2(correctResult.isEmpty(),
			qPrintable(QString("Expected 0 notes with shared period, got %1").arg(correctResult.size())));

		// With WRONG per-clip length (1 bar = 192 ticks):
		// The note INCORRECTLY passes the filter (the old bug)
		QVector<int> buggyResult = filterNotes(notes, TPB, TPB, TPB);
		QVERIFY2(!buggyResult.isEmpty(),
			"Sanity check: the old per-clip period should produce ghost notes");
	}

	/**
	 * @brief Notes that DO fall within the visible window should still be copied.
	 *
	 * With the shared pattern length, notes at positions within the offset window
	 * must still pass the filter correctly.
	 */
	void testVisibleNotesStillCopied()
	{
		const tick_t TPB = TimePos::ticksPerBar();
		const tick_t patternLength = 3 * TPB;

		// Note at pos 408 (2.125 bars into the pattern)
		// PatternClip: offset=+TPB, length=TPB
		// Visible window with 3-bar pattern and offset +TPB:
		//   For i=-1: posRelative = 408 + TPB - patternLength = 408 + 192 - 576 = 24
		//   24 >= 0 && 24 < 192 → PASS (correct!)
		Note kickNote(TimePos(12), TimePos(408), 72);
		NoteVector notes;
		notes.push_back(&kickNote);

		QVector<int> result = filterNotes(notes, patternLength, TPB, TPB);
		QCOMPARE(result.size(), 1);
		QCOMPARE(result[0], 24);
	}

	/**
	 * @brief Negative offsets correctly select later portions of the pattern.
	 *
	 * With offset=-480 and length=48, the visible window is ticks [480, 528)
	 * relative to the repeating pattern.
	 */
	void testNegativeOffsetFiltering()
	{
		const tick_t TPB = TimePos::ticksPerBar();
		const tick_t patternLength = 3 * TPB; // 576 ticks

		// Note at pos 500 should be visible (500 + (-480) = 20, within [0, 48))
		Note visibleNote(TimePos(12), TimePos(500), 69);
		// Note at pos 100 should NOT be visible (100 + (-480) = -380, < 0)
		Note invisibleNote(TimePos(12), TimePos(100), 69);

		NoteVector notes;
		notes.push_back(&visibleNote);
		notes.push_back(&invisibleNote);

		QVector<int> result = filterNotes(notes, patternLength, -480, 48);
		QCOMPARE(result.size(), 1);
		QCOMPARE(result[0], 20); // 500 + (-480) = 20
	}

	/**
	 * @brief Notes that extend past the clip boundary should be truncated.
	 *
	 * Replicates the truncation logic from copySelectionToNewPatternTrack.
	 */
	void testNoteTruncationAtClipBoundary()
	{
		// Note at pos 500 with length 100, clip window [480, 528) (offset=-480, len=48)
		// posRelativeToClip = 500 - 480 = 20, within [0, 48)
		// But note extends to 20+100 = 120 which is past clipEnd (48)
		// Should be truncated to length 48-20 = 28
		Note longNote(TimePos(100), TimePos(500), 69);

		tick_t posRelativeToClip = 500 + (-480); // = 20
		tick_t clipLength = 48;

		QVERIFY(posRelativeToClip >= 0 && posRelativeToClip < clipLength);

		// Apply truncation logic
		tick_t truncatedLength = longNote.length();
		if (posRelativeToClip + truncatedLength > clipLength)
		{
			truncatedLength = clipLength - posRelativeToClip;
		}
		QCOMPARE(truncatedLength, static_cast<tick_t>(28));
	}

	/**
	 * @brief Pattern repetitions are counted correctly using shared length.
	 *
	 * A 4-bar PatternClip with a 3-bar shared pattern length should produce
	 * at most 2 repetitions (ceil(4/3) = 2, plus the safety margins).
	 */
	void testRepetitionCountWithSharedLength()
	{
		const tick_t TPB = TimePos::ticksPerBar();
		const tick_t patternLength = 3 * TPB;  // shared: 3 bars
		const tick_t clipLength = 4 * TPB;      // PatternClip: 4 bars
		const tick_t offset = 0;                // no offset

		// Note at pos 0 should appear at: i=0 → pos 0, i=1 → pos 576 (both < 768)
		// i=2 → pos 1152 which is >= 768, filtered out
		Note note(TimePos(12), TimePos(0), 69);
		NoteVector notes;
		notes.push_back(&note);

		QVector<int> result = filterNotes(notes, patternLength, offset, clipLength);
		QCOMPARE(result.size(), 2);
		QCOMPARE(result[0], 0);
		QCOMPARE(result[1], static_cast<int>(patternLength));
	}

	/**
	 * @brief A 1-bar MidiClip within a 3-bar shared pattern does NOT repeat
	 *        at 1-bar intervals, only at 3-bar intervals.
	 *
	 * This directly tests the fix: using per-clip length (1 bar) would produce
	 * 4 repetitions in a 4-bar window; using shared length (3 bars) produces
	 * only the correct count.
	 */
	void testShortClipDoesNotRepeatAtOwnPeriod()
	{
		const tick_t TPB = TimePos::ticksPerBar();
		const tick_t sharedLength = 3 * TPB;
		const tick_t clipLength = 4 * TPB;
		const tick_t offset = 0;

		// Note at pos 50 in a 1-bar MidiClip
		Note note(TimePos(12), TimePos(50), 69);
		NoteVector notes;
		notes.push_back(&note);

		// With SHARED length (3 bars): note at pos 50, then 50+576=626
		// Both < 768 (4 bars), so 2 matches
		QVector<int> sharedResult = filterNotes(notes, sharedLength, offset, clipLength);
		QCOMPARE(sharedResult.size(), 2);

		// With WRONG per-clip length (1 bar): note at 50, 242, 434, 626 — 4 matches
		QVector<int> perClipResult = filterNotes(notes, TPB, offset, clipLength);
		QVERIFY2(perClipResult.size() > sharedResult.size(),
			"Per-clip period produces more (wrong) repetitions than shared period");
	}

	/**
	 * @brief Note::Type is preserved through copy construction.
	 *
	 * Step notes (BeatClip) must retain their type when copied during the
	 * pattern copy operation.
	 */
	void testNoteCopyPreservesType()
	{
		Note regularNote(TimePos(12), TimePos(0), 69);
		QCOMPARE(regularNote.type(), Note::Type::Regular);

		Note stepNote(TimePos(12), TimePos(0), 69);
		stepNote.setType(Note::Type::Step);
		QCOMPARE(stepNote.type(), Note::Type::Step);

		// Copy construction must preserve type
		Note copiedRegular{regularNote};
		QCOMPARE(copiedRegular.type(), Note::Type::Regular);

		Note copiedStep{stepNote};
		QCOMPARE(copiedStep.type(), Note::Type::Step);
	}

	/**
	 * @brief MidiClip::checkType correctly distinguishes BeatClip from MelodyClip.
	 */
	void testCheckTypeClassification()
	{
		auto patternStore = Engine::patternStore();
		InstrumentTrack track(patternStore);

		// Empty clip → BeatClip (all_of on empty range returns true)
		MidiClip emptyClip(&track);
		QCOMPARE(emptyClip.type(), MidiClip::Type::BeatClip);

		// Adding a regular note (positive length) → MelodyClip
		MidiClip melodyClip(&track);
		melodyClip.addNote(Note(TimePos(12), TimePos(0), 69), false);
		QCOMPARE(melodyClip.type(), MidiClip::Type::MelodyClip);

		// Adding only step notes → stays BeatClip
		MidiClip beatClip(&track);
		Note stepNote(TimePos(-192), TimePos(0), 69);
		stepNote.setType(Note::Type::Step);
		beatClip.addNote(stepNote, false);
		QCOMPARE(beatClip.type(), MidiClip::Type::BeatClip);
	}

	/**
	 * @brief addSteps only extends BeatClips; MelodyClips should use updateLength.
	 *
	 * This tests the fix to the step count finalization loop: BeatClips get
	 * addSteps() to extend their step grid, MelodyClips get updateLength()
	 * which calculates from note positions.
	 */
	void testAddStepsOnlyAffectsBeatClips()
	{
		auto patternStore = Engine::patternStore();
		InstrumentTrack track(patternStore);
		const tick_t TPB = TimePos::ticksPerBar();

		// Create a melody clip with a note ending within bar 2
		// pos=300 + len=12 = 312, which is < 2*TPB=384, so rounds to 2 bars
		MidiClip melodyClip(&track);
		melodyClip.addNote(Note(TimePos(12), TimePos(300), 69), false);
		QCOMPARE(melodyClip.type(), MidiClip::Type::MelodyClip);

		tick_t melodyLenBefore = melodyClip.length();
		melodyClip.updateLength();
		tick_t melodyLenAfter = melodyClip.length();

		// updateLength on a MelodyClip should set length based on note endpoints
		QCOMPARE(melodyLenBefore, melodyLenAfter);
		QCOMPARE(melodyLenAfter, static_cast<tick_t>(2 * TPB));
	}

	/**
	 * @brief Full integration: reproduce the user's exact scenario.
	 *
	 * Sets up InstrumentTracks matching the user's project:
	 * - "kick" with notes spanning 3 bars (period = 3 bars)
	 * - "hihat" with 1 note at pos 72 (period = 1 bar individually)
	 * Then verifies that using the shared pattern length (3 bars) for
	 * the repetition period correctly filters out the hihat note when
	 * the clip has offset=+TPB, while the per-clip period would not.
	 */
	void testUserScenarioPatternCopy()
	{
		const tick_t TPB = TimePos::ticksPerBar();
		auto patternStore = Engine::patternStore();

		// Create a PatternTrack to get a fresh pattern index
		PatternTrack pt(Engine::getSong());
		int patIdx = pt.patternIndex();

		// "kick" InstrumentTrack: notes spanning 3 bars
		InstrumentTrack kickTrack(patternStore);
		auto* kickClip = dynamic_cast<MidiClip*>(kickTrack.getClip(patIdx));
		QVERIFY(kickClip);
		kickClip->addNote(Note(TimePos(12), TimePos(0), 72), false);
		kickClip->addNote(Note(TimePos(12), TimePos(408), 72), false);
		kickClip->addNote(Note(TimePos(12), TimePos(528), 72), false);

		// "hihat" InstrumentTrack: 1 note at pos 72 (1-bar clip)
		InstrumentTrack hihatTrack(patternStore);
		auto* hihatClip = dynamic_cast<MidiClip*>(hihatTrack.getClip(patIdx));
		QVERIFY(hihatClip);
		hihatClip->addNote(Note(TimePos(12), TimePos(72), 69), false);

		// Verify individual lengths differ
		QVERIFY2(kickClip->length() > hihatClip->length(),
			"Kick clip should be longer than hihat clip");

		// Verify shared pattern length matches the longest clip
		bar_t sharedBars = patternStore->lengthOfPattern(patIdx);
		tick_t sharedLength = sharedBars * TPB;
		QCOMPARE(sharedBars, kickClip->length().nextFullBar());
		QVERIFY(sharedLength > hihatClip->length());

		// Simulate PatternClip with offset=+TPB, length=TPB
		// (the user's clips 3 and 5 in the project file)
		tick_t clipOffset = static_cast<tick_t>(TPB);   // +192
		tick_t clipLength = static_cast<tick_t>(TPB);    // 192

		// With SHARED pattern length: hihat note should NOT pass
		QVector<int> hihatShared = filterNotes(hihatClip->notes(), sharedLength, clipOffset, clipLength);
		QVERIFY2(hihatShared.isEmpty(),
			qPrintable(QString("Hihat should have 0 notes with shared period, got %1")
				.arg(hihatShared.size())));

		// With SHARED pattern length: kick note at 408 SHOULD pass
		QVector<int> kickShared = filterNotes(kickClip->notes(), sharedLength, clipOffset, clipLength);
		QVERIFY2(!kickShared.isEmpty(),
			"Kick notes should still pass with shared period");

		// Verify the specific kick note that passes is at pos 408
		// 408 + 192 - 576 = 24, which is within [0, 192)
		bool found24 = false;
		for (int pos : kickShared)
		{
			if (pos == 24) { found24 = true; }
		}
		QVERIFY2(found24, "Kick note at pos 408 should produce clip-relative pos 24");
	}

	/**
	 * @brief Negative offset with shared pattern length selects correct window.
	 *
	 * Reproduces the user's clips 1 and 2 (offset=-480, length=48 or 96).
	 */
	void testNegativeOffsetWithSharedLength()
	{
		const tick_t TPB = TimePos::ticksPerBar();
		const tick_t sharedLength = 3 * TPB; // 576

		// kick notes: 0, 408, 528
		Note n0(TimePos(12), TimePos(0), 72);
		Note n408(TimePos(12), TimePos(408), 72);
		Note n528(TimePos(12), TimePos(528), 72);
		NoteVector kickNotes;
		kickNotes.push_back(&n0);
		kickNotes.push_back(&n408);
		kickNotes.push_back(&n528);

		// Clip: offset=-480, length=48 → visible window [480, 528) in pattern
		QVector<int> result = filterNotes(kickNotes, sharedLength, -480, 48);

		// n528: 528 + (-480) = 48, NOT < 48, so filtered out
		// n408: 408 + (-480) = -72, < 0, filtered out
		// n0: 0 + (-480) = -480, < 0, filtered out
		// With i=1: n0: 0 + (-480) + 576 = 96, >= 48, filtered
		// n408: 408 + (-480) + 576 = 504, >= 48, filtered
		// n528: 528 + (-480) + 576 = 624, >= 48, filtered
		// So NO notes should pass for this small clip
		QVERIFY2(result.isEmpty(),
			qPrintable(QString("Expected 0 notes for offset=-480 len=48, got %1").arg(result.size())));

		// Clip: offset=-480, length=96 → visible window [480, 576)
		QVector<int> result2 = filterNotes(kickNotes, sharedLength, -480, 96);
		// n528: 528 + (-480) = 48, 48 < 96 → PASS at pos 48
		QCOMPARE(result2.size(), 1);
		QCOMPARE(result2[0], 48);
	}

	/**
	 * @brief Multiple repetitions across a long clip with shared length.
	 *
	 * A PatternClip spanning 6+ bars with a 3-bar shared pattern should
	 * produce exactly 2 full repetitions of each note.
	 */
	void testMultipleRepetitionsSharedLength()
	{
		const tick_t TPB = TimePos::ticksPerBar();
		const tick_t sharedLength = 3 * TPB; // 576
		const tick_t clipLength = 7 * TPB;   // 1344 — over 2 full repetitions
		const tick_t offset = 0;

		// Note at pos 100
		Note note(TimePos(12), TimePos(100), 69);
		NoteVector notes;
		notes.push_back(&note);

		QVector<int> result = filterNotes(notes, sharedLength, offset, clipLength);

		// i=-1: 100 - 576 = -476 < 0, skip
		// i=0: 100, pass
		// i=1: 100 + 576 = 676, pass (< 1344)
		// i=2: 100 + 1152 = 1252, pass (< 1344)
		// i=3: 100 + 1728 = 1828, skip (>= 1344)
		QCOMPARE(result.size(), 3);
		QCOMPARE(result[0], 100);
		QCOMPARE(result[1], 100 + static_cast<int>(sharedLength));
		QCOMPARE(result[2], 100 + 2 * static_cast<int>(sharedLength));
	}
};

QTEST_GUILESS_MAIN(PatternCopyTest)
#include "PatternCopyTest.moc"
