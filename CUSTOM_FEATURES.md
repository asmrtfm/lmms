# Custom Features & Architecture Guide

This document explains the conceptual model of LMMS's editor architecture and provides detailed overviews of all custom features added to this branch.

---

## Part 1: Understanding LMMS's Editor Architecture

### The Three Editors

LMMS has three primary editing windows. Understanding the relationship between them is essential for working on this codebase.

#### Song Editor (Timeline)
The Song Editor is a **free-form timeline** where the user arranges clips horizontally across time. Tracks run vertically down the left side; the horizontal axis is time measured in bars. The user can place, move, resize, truncate, split, and copy clips on this timeline.

The Song Editor can contain several track types:
- **PatternTracks** — references to patterns in the PatternStore
- **InstrumentTracks** — standalone instruments placed directly in the song
- **SampleTracks** — audio samples
- **AutomationTracks** — automation curves

#### Piano Roll (Instrument-Track Editor)
The Piano Roll is also a **free-form timeline**, but for individual notes within an instrument. The vertical axis is pitch (MIDI note number); the horizontal axis is time. The user can place, move, resize, truncate, and copy notes.

#### The Critical Insight: Song Editor ≈ Piano Roll

**The Song Editor and the Piano Roll are the two most similar editors in LMMS.** They are both free-form timeline editors where the user manipulates rectangular objects:

| Concept | Song Editor | Piano Roll |
|---------|-------------|------------|
| **Container** | Song (TrackContainer) | MidiClip |
| **Objects on timeline** | Clips (PatternClip, SampleClip, etc.) | Notes |
| **Operations** | Move, resize, truncate, copy, delete | Move, resize, truncate, copy, delete |
| **Horizontal axis** | Time (bars) | Time (bars/beats) |
| **Vertical axis** | Tracks | Pitch |

**PatternClips in the Song Editor are the conceptual equivalent of Notes in the Piano Roll.** The same manipulation operations apply to both. When you shrink a PatternClip in the Song Editor, the underlying pattern data in the PatternStore is unchanged — but only the portion within the clip's boundaries plays back. The clip's `length` and `startTimeOffset` act as a **playback window** into the pattern data, just like a note's position and length define when and how long it sounds.

This means that when a PatternClip is resized in the Song Editor, the changes **propagate down into the respective slices of the InstrumentTracks** — not by modifying the InstrumentTrack data, but by controlling which portion of each InstrumentTrack's MidiClip is audible during playback.

#### Pattern Editor (Beat/Step Sequencer) — The Odd One Out

The Pattern Editor is fundamentally different from both the Song Editor and Piano Roll. It is a **fixed grid of steps**, not a free-form timeline. It displays the InstrumentTracks belonging to the PatternStore and can only contain InstrumentTracks. Users toggle steps on/off or edit note content per-step, but they cannot freely move or resize clips — clip positions are fixed at one-per-pattern-index.

### The PatternStore Data Model

The PatternStore is a TrackContainer that holds InstrumentTracks in a grid structure:

```
                Pattern 0    Pattern 1    Pattern 2    ...
              ┌────────────┬────────────┬────────────┐
  Kick        │ MidiClip 0 │ MidiClip 1 │ MidiClip 2 │  ← InstrumentTrack
              ├────────────┼────────────┼────────────┤
  Snare       │ MidiClip 0 │ MidiClip 1 │ MidiClip 2 │  ← InstrumentTrack
              ├────────────┼────────────┼────────────┤
  Bass        │ MidiClip 0 │ MidiClip 1 │ MidiClip 2 │  ← InstrumentTrack
              └────────────┴────────────┴────────────┘
```

- **Rows** = InstrumentTracks (each with a full instrument: plugin, effects, mixer channel)
- **Columns** = Pattern indices (one per PatternTrack in the Song)
- **Cells** = MidiClips containing note data (or step-sequencer data for beat clips)

Each PatternTrack in the Song Editor maps to a column index via `PatternTrack::s_infoMap`. The `patternIndex()` method returns which column this track references. `track->getClip(patternIndex)` returns the MidiClip at that cell.

### Key Code Relationships

- `Engine::getSong()` → Song (TrackContainer holding PatternTracks and Song-level tracks)
- `Engine::patternStore()` → PatternStore (TrackContainer holding InstrumentTracks)
- `PatternTrack::patternIndex()` → int column index into the PatternStore grid
- `track->getClip(n)` → Clip at column n for that track (auto-creates if missing)
- Song-level InstrumentTracks (type=0 directly in the Song) can coexist with PatternTracks

---

## Part 2: Custom Features

### Feature 1: Copy Pattern Clips to New Track

**Purpose:** Select one or more PatternClips in the Song Editor and copy their note data into a brand new PatternTrack, respecting clip boundaries (position, length, offset, looping).

**How to use:**
1. In the Song Editor, select one or more PatternClips (they must all be PatternClips)
2. Right-click → "Copy to New Pattern Track"
3. A new PatternTrack is created with the combined note data from all selected clips

**Behavior details:**
- Notes are copied from the source pattern's InstrumentTracks into the new pattern's InstrumentTracks
- Clip boundaries are respected: only notes whose start position falls within the visible portion of the clip are copied
- Notes that start within the clip boundary but extend beyond it are **truncated** at the boundary
- Looping/repetition is handled: if a clip is longer than the underlying pattern, the notes repeat and are copied for each repetition
- Start time offsets are accounted for
- Multiple selected clips from different PatternTracks are merged into the new pattern

**Files:**
- `src/gui/clips/PatternClipView.cpp` — `copySelectionToNewPatternTrack()`, `canCopySelectionToNewTrack()`, context menu integration
- `include/PatternClipView.h` — method declarations (added via `MidiClipView.h` modifications)

---

### Feature 2: Pattern Track Export (.xppt files)

**Purpose:** Export one or more patterns to portable `.xppt` files that contain the complete instrument configuration and note data for a single pattern column.

**How to use:**
1. **From Song Editor toolbar:** Click "Export patterns..." button (right side of toolbar)
2. **From PatternTrack context menu:** Right-click a PatternTrack → gear menu → "Export patterns..."
3. A checkbox dialog appears listing all PatternTracks — select which ones to export
4. Choose a destination directory
5. Each selected pattern is saved as a `.xppt` file named after the PatternTrack

**What's in an .xppt file:**
Each file contains the complete state of every InstrumentTrack in the PatternStore, but with only the single MidiClip at the target pattern index. This means each .xppt file is self-contained — it includes instrument plugins, effects, mixer routing, and the note/step data for that one pattern.

**Normalization:** Track names are automatically normalized before export (clone-jank stripped, whitespace replaced with underscores, duplicates resolved with counters).

**Files:**
- `src/gui/tracks/TrackOperationsWidget.cpp` — `exportPattern()` (context menu version)
- `src/gui/editors/SongEditor.cpp` — `SongEditorWindow::exportPatterns()` (toolbar version)
- `include/DataFile.h` — `PatternData` type enum
- `src/core/DataFile.cpp` — `.xppt` extension registration

---

### Feature 3: Pattern Track Import (.xppt files)

**Purpose:** Import `.xppt` pattern files into the current project, restoring instrument settings and note data.

**How to use:**
1. **From Song Editor toolbar:** Click "Import patterns..." button
2. **From PatternTrack context menu:** Right-click a PatternTrack → gear menu → "Import patterns..."
3. Select one or more `.xppt` files
4. Context menu version: first file imports into the right-clicked PatternTrack's pattern index; additional files create new PatternTracks
5. Toolbar version: each file creates a new PatternTrack

**Import behavior:**
- For each InstrumentTrack in the file, matches by position order against existing InstrumentTracks in the PatternStore
- If the file has more tracks than the PatternStore, new InstrumentTracks are created with full settings loaded from the file
- JournallingObject metadata is stripped from imported XML to prevent ID collisions and crashes
- The entire operation is wrapped in `requestChangeInModel()`/`doneChangeInModel()` for audio engine thread safety

**Files:**
- `src/gui/tracks/TrackOperationsWidget.cpp` — `importPattern()` (context menu version)
- `src/gui/editors/SongEditor.cpp` — `SongEditorWindow::importPatterns()` (toolbar version)

---

### Feature 4: Save Without Patterns

**Purpose:** Save a copy of the current project with all pattern arrangement and note data stripped out, preserving instrument configurations, effects, and mixer setup. Useful for creating reusable project skeletons.

**How to use:**
1. File menu → "Save without patterns..."
2. Choose a filename and location (defaults to user projects directory, .mmpz/.mmp format)
3. The saved file contains all instruments, effects, and mixer channels but no note/step data and no song arrangement

**How it works internally:**
1. Saves the full project via `Song::saveProjectFile()`
2. Re-reads the saved file as XML DOM
3. Keeps the first PatternTrack (which holds the PatternStore with all InstrumentTrack settings)
4. Strips all `<patternclip>` elements (song arrangement)
5. Inside the PatternStore, strips all clip elements (`midiclip`, `sampleclip`, `automationclip`) from each track while preserving instrument/effect settings
6. Removes all additional PatternTracks beyond the first
7. Re-writes the cleaned file

**Files:**
- `src/gui/MainWindow.cpp` — `saveProjectAsDefaultTemplateNoPatterns()`, File menu integration
- `include/MainWindow.h` — slot declaration

---

### Feature 5: Reset Steps

**Purpose:** Reset the step count of beat clips back to the default (one bar, typically 32 steps).

**How to use:**
- **Per-clip:** Right-click a beat clip in the Pattern Editor → "Reset steps"
- **All tracks:** Click "Reset steps" button in the Pattern Editor toolbar (resets all InstrumentTracks in the current pattern)

**Files:**
- `src/tracks/MidiClip.cpp` — `MidiClip::resetSteps()` implementation
- `include/MidiClip.h` — slot declaration
- `src/gui/clips/MidiClipView.cpp` — context menu integration
- `src/gui/editors/PatternEditor.cpp` — toolbar button, `PatternEditor::resetSteps()` slot

---

### Feature 6: Track Name Normalization

**Purpose:** Clean up track names by stripping "Clone of" junk and replacing whitespace with underscores, with automatic deduplication via counter suffixes.

**How to use:**
- **Song Editor:** Click "Normalize names" button in the toolbar (normalizes PatternTrack names)
- **Pattern Editor:** Click "Normalize names" button in the toolbar (normalizes InstrumentTrack names)
- **Automatic:** Names are normalized before pattern export

**Normalization algorithm (per track):**
1. Capture the track's current name
2. Remove all "clone-jank": any instance of "Clone"/"clone" followed by a word-break character (space, +, -, _) followed by "of", anywhere in the name, repeated any number of times
3. Replace all whitespace with underscores
4. If the name hasn't changed, skip (already normalized or doesn't need it)
5. If the new name is unique among all current track names, rename directly
6. If it conflicts, apply counter logic:
   - Counter suffixes are exactly 3 digits beginning with 0 (e.g., `_001` through `_099`)
   - If the name already ends with a counter, increment it
   - Otherwise, append a new counter starting at `_001`
   - Each conflict check reads fresh track names (never stale cached names)
7. Log the rename to stderr, then set the new name

**Files:**
- `src/core/Track.cpp` — `Track::normalizeTrackNames()`, `logNormalize()`, `renameTrack()`, `trackTypeName()`
- `include/Track.h` — static method declaration
- `src/gui/editors/SongEditor.cpp` — `SongEditorWindow::normalizePatternTrackNames()` slot, toolbar button
- `src/gui/editors/PatternEditor.cpp` — `PatternEditorWindow::normalizeInstrumentTrackNames()` slot, toolbar button
- `include/SongEditor.h` — slot declaration
- `include/PatternEditor.h` — slot declaration

---

### Feature 7: Song Editor Toolbar Buttons

**Purpose:** Provide quick access to pattern management actions from the Song Editor toolbar without needing to right-click a specific track.

**Buttons (right side of toolbar):**
- **Normalize names** — normalizes all PatternTrack names in the Song
- **Export patterns...** — opens the pattern export dialog
- **Import patterns...** — opens the pattern import file picker

**Files:**
- `src/gui/editors/SongEditor.cpp` — toolbar setup in `SongEditorWindow` constructor
- `include/SongEditor.h` — slot declarations

---

## Part 3: Files Modified (Summary)

| File | Changes |
|------|---------|
| `include/DataFile.h` | Added `PatternData` to `DataFile::Type` enum |
| `include/MainWindow.h` | Added `saveProjectAsDefaultTemplateNoPatterns()` slot |
| `include/MidiClip.h` | Added `resetSteps()` slot declaration |
| `include/PatternEditor.h` | Added `normalizeInstrumentTrackNames()` slot |
| `include/SongEditor.h` | Added `normalizePatternTrackNames()`, `exportPatterns()`, `importPatterns()` slots |
| `include/Track.h` | Added `normalizeTrackNames()` static method |
| `include/TrackOperationsWidget.h` | Added `exportPattern()`, `importPattern()` slots |
| `src/core/DataFile.cpp` | Registered `patterndata` type, `.xppt` extension |
| `src/core/Track.cpp` | Implemented `normalizeTrackNames()` with helpers |
| `src/gui/MainWindow.cpp` | Implemented save-without-patterns, File menu item |
| `src/gui/clips/MidiClipView.cpp` | Added "Reset steps" to beat clip context menu |
| `src/gui/clips/PatternClipView.cpp` | Implemented copy-to-new-pattern-track with truncation |
| `src/gui/editors/PatternEditor.cpp` | Added normalize button, resetSteps toolbar button |
| `src/gui/editors/SongEditor.cpp` | Added toolbar buttons, export/import/normalize implementations |
| `src/gui/tracks/TrackOperationsWidget.cpp` | Export/import implementations, context menu |
| `src/tracks/MidiClip.cpp` | Implemented `resetSteps()` |
