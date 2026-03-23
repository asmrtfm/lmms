# PatternCopyTest — 14 test cases:                                                                                                                                            

  ┌───────────────────────────────────────┬─────────────────────────────────────────────────────────────────────────────┐
  │                 Test                  │                              What it verifies                               │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testLengthOfPatternReturnsMax         │ PatternStore::lengthOfPattern() returns the max across all InstrumentTracks │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testShortClipNoGhostNotes             │ Core bug fix: short MidiClip + shared period = no ghost notes               │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testVisibleNotesStillCopied           │ Notes within the visible window still pass the filter                       │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testNegativeOffsetFiltering           │ Negative startTimeOffset selects later portion of pattern                   │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testNoteTruncationAtClipBoundary      │ Notes extending past clip end are truncated                                 │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testRepetitionCountWithSharedLength   │ Correct repetition count with shared period                                 │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testShortClipDoesNotRepeatAtOwnPeriod │ 1-bar clip in 3-bar pattern: 2 reps not 4                                   │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testNoteCopyPreservesType             │ Note::Type::Step preserved through copy construction                        │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testCheckTypeClassification           │ checkType() correctly classifies BeatClip vs MelodyClip                     │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testAddStepsOnlyAffectsBeatClips      │ updateLength() on MelodyClip is idempotent                                  │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testUserScenarioPatternCopy           │ Full integration reproducing user's exact bug scenario                      │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testNegativeOffsetWithSharedLength    │ User's negative-offset clips with shared period                             │
  ├───────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
  │ testMultipleRepetitionsSharedLength   │ 7-bar clip with 3-bar pattern = 3 correct repetitions                       │
  └───────────────────────────────────────┴─────────────────────────────────────────────────────────────────────────────┘

