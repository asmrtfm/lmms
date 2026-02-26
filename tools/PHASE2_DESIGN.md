# Phase 2 Design: C++ SQLite Reader for LMMS

## Current Loading Pipeline (XML)

The project loading chain works as follows:

```
Song::loadProject(fileName)
  |
  +-> DataFile(fileName)           // Reads XML, decompresses if .mmpz
  |     |                          // Runs upgrade methods (v1.0 -> v30+)
  |     +-> QDomDocument           // Parsed into a full DOM tree
  |
  +-> Song metadata                // m_tempoModel.loadSettings(head, "bpm") etc.
  |
  +-> Mixer::loadSettings()        // Mixer channels, sends, effects
  |
  +-> TrackContainer::loadSettings()  // The song's main trackcontainer
  |     |
  |     +-> Track::create(QDomElement, TrackContainer)
  |           |
  |           +-> Track::create(Type, TrackContainer)   // Allocates the right Track subclass
  |           +-> Track::restoreState(QDomElement)
  |                 |
  |                 +-> Track::loadTrack(element)
  |                       |
  |                       +-> setName(), m_mutedModel.loadSettings() etc.
  |                       +-> loadTrackSpecificSettings(settingsNode)
  |                       |     |
  |                       |     +-> InstrumentTrack: loads vol, pan, pitch, instrument plugin,
  |                       |     |   sound shaping, arpeggio, chord creator, MIDI port, effects
  |                       |     +-> PatternTrack: loads its own patternstore trackcontainer
  |                       |     +-> SampleTrack: loads vol, pan, mixer channel
  |                       |     +-> AutomationTrack: (minimal)
  |                       |
  |                       +-> For each remaining child element (clips):
  |                             createClip(TimePos(0))
  |                             clip->restoreState(clipElement)
  |
  +-> restoreControllerStates()    // LFO, MIDI CC, Peak controllers
  +-> restoreScaleStates()         // Microtuner scales
  +-> restoreKeymapStates()        // Microtuner keymaps
  +-> GUI state (pianoroll, automationeditor, etc.)
  |
  +-> Post-load fixups:
        PatternStore::fixIncorrectPositions()
        ControllerConnection::finalizeConnections()
        AutomationClip::resolveAllIDs()
```

## Key Insight: QDomElement is Everywhere

The current system is deeply coupled to `QDomElement`. Every `loadSettings()` and
`restoreState()` method takes `const QDomElement&`. This means we **cannot** simply
replace the XML parser with a SQLite reader — we would need to either:

**Option A: Build a QDomDocument from SQLite (Adapter Pattern)**
- Read SQLite tables and construct a QDomDocument that looks exactly like the XML
- Feed this synthetic DOM into the existing loading pipeline unchanged
- Pro: Zero changes to existing load code
- Con: Defeats much of the purpose (still building full DOM in memory)

**Option B: Introduce a DataSource abstraction**
- Create an interface that both XML and SQLite can implement
- Gradually migrate `loadSettings()` methods to use the abstraction
- Pro: Clean long-term architecture
- Con: Massive refactor touching hundreds of files

**Option C: SQLite → XML stream (recommended for Phase 2)**
- Read SQLite and generate the equivalent XML string
- Pass it through the existing DataFile constructor that takes `QByteArray`
- This gives us the existing upgrade/validation pipeline for free
- Pro: Minimal code changes, leverages existing infrastructure
- Con: Still goes through XML parsing, but only as a bridge

## Recommended Approach: Option C (SQLite → XML → existing pipeline)

### Why Option C?

1. **Minimal risk**: The existing loading code is battle-tested across thousands of projects.
   Rewriting it introduces bugs. Option C reuses 100% of the load path.

2. **Incremental**: We can ship .lmms-db support without touching any existing code.
   Later phases can migrate individual subsystems to direct SQLite access.

3. **Validation**: The round-trip (XML → SQLite → XML → load) can be validated by
   comparing the regenerated XML against the original.

4. **Upgrade compatibility**: DataFile's upgrade methods handle old format quirks.
   Option C gets this for free.

### Implementation Plan

```
tools/
  lmms_convert.py          -- Phase 1: XML → SQLite (DONE)
  lmms_export.py           -- Phase 2a: SQLite → XML string
src/core/
  DataFile.cpp              -- Phase 2b: Add .lmms-db detection and SQLite-to-XML bridge
```

#### Phase 2a: `lmms_export.py` — SQLite → XML

A Python script that reads a .lmms-db file and generates a valid .mmp XML string.
This serves as:
- A validation tool (round-trip test)
- A reference implementation for the C++ version
- A way to convert back to .mmp for users who need it

#### Phase 2b: C++ Integration

Add to `DataFile::DataFile(const QString& fileName)`:

```cpp
// Pseudocode
if (fileName.endsWith(".lmms-db")) {
    // Open SQLite database
    // Generate XML string from tables
    // Parse the XML string as a QDomDocument
    // Continue with normal upgrade/load path
}
```

This requires:
1. Adding SQLite3 as a dependency (it's likely already available via Qt)
2. A `SqliteToXml` helper class that reads tables and builds XML
3. A few lines in DataFile's constructor to detect .lmms-db files

## Entity Loading Order (critical)

The XML loading order matters because of cross-references:

1. **Mixer channels** — Must exist before InstrumentTracks reference them via `mixch`
2. **PatternStore InstrumentTracks** — Must exist before PatternClips reference patterns
3. **Pattern tracks** — Reference patterns by index (in XML) or by ID (in SQLite)
4. **Song-level tracks** — AutomationTracks, SampleTracks
5. **Controllers** — Must exist before ControllerConnections reference them by index
6. **Post-load**: `AutomationClip::resolveAllIDs()` resolves JournallingObject IDs

The SQLite→XML generator must emit entities in this order.

## JournallingObject ID Challenge

The biggest obstacle to direct SQLite loading (Options A/B) is the JournallingObject ID system:

- Every `AutomatableModel` gets a unique `jo_id_t` assigned at construction time
- Automation clips reference models by these IDs (stored as `<object id="..."/>`)
- IDs are **not stable** — they depend on construction order
- During XML load, `idToSave()`/`idFromSave()` maps between saved and runtime IDs
- `AutomationClip::resolveAllIDs()` resolves saved IDs to runtime objects

In the SQLite format, we store the saved IDs in `automation_target.target_object_id`.
When generating XML for the bridge, these IDs must be emitted in the `<object id="..."/>`
elements exactly as they were in the original XML. The existing resolution code handles
the rest.

For future direct-SQLite loading (Phase 4+), we would need to either:
- Assign JournallingObject IDs deterministically based on SQLite primary keys
- Or build a mapping table during load

## Effect Chain / Plugin Parameter Challenge

Plugin parameters are stored as JSON in SQLite but need to be XML for the bridge.
The `params_json` column contains nested structures that map directly to XML elements.
The SQLite→XML generator must reverse the `elem_to_json()` conversion:

```python
def json_to_xml(parent_elem, data):
    for key, value in data.items():
        if isinstance(value, dict):
            child = ET.SubElement(parent_elem, key)
            json_to_xml(child, value)
        elif isinstance(value, list):
            for item in value:
                child = ET.SubElement(parent_elem, key)
                json_to_xml(child, item)
        else:
            parent_elem.set(key, str(value))
```

## File Format Detection

Detect format by file extension and magic bytes:

| Extension | Magic | Format |
|-----------|-------|--------|
| `.mmp`    | `<?xml` | Uncompressed XML |
| `.mmpz`   | 4-byte header + `x\x9c` | zlib-compressed XML |
| `.lmms-db`| `SQLite format 3` | SQLite database |

## Future: Direct SQLite Access (Phase 4+)

Eventually, individual subsystems can be migrated to read directly from SQLite:

1. **Note queries**: `SELECT * FROM note WHERE midi_clip_id=? ORDER BY position`
   replaces walking `<note>` child elements
2. **Automation queries**: `SELECT * FROM automation_node WHERE automation_clip_id=?`
   replaces walking `<time>` elements
3. **Incremental save**: Only UPDATE/INSERT changed rows instead of rewriting entire XML
4. **Undo/redo**: SQLite transactions instead of serializing entire state to XML

Each migration replaces a `loadSettings(QDomElement)` with a `loadFromDatabase(sqlite3*)`,
and can be done one subsystem at a time without affecting others.

## Summary of Work for Phase 2

| Task | Effort | Risk |
|------|--------|------|
| `lmms_export.py` (SQLite → XML) | Medium | Low |
| Round-trip validation test | Small | Low |
| C++ SQLite detection in DataFile | Small | Low |
| C++ `SqliteToXml` helper class | Medium | Medium |
| CMake: add SQLite3 dependency | Small | Low |
| Integration testing | Medium | Medium |
