# C++ Implementation Plan: Track Template Import/Export

This plan adds 4 features to the existing LMMS C++ codebase to fulfill the modular track reuse specification. No rewrite. No new language. Extend what's already there.

---

## Overview

The existing codebase already has:
- `Track::savePreset()` / `Track::loadPreset()` — saves/loads track settings (no clips)
- `DataFile::copyResources()` — bundles external samples into a package
- `Mixer::createChannel()` — creates mixer channels with effects
- `EffectChain::saveState()` / `restoreState()` — serializes all effects
- `AutomationClip::resolveAllIDs()` — wires automation post-load
- 30+ upgrade methods for legacy formats

We add 4 things: **TrackBundle format**, **mixer channel remapping**, **automation portability**, and **SQLite project index**.

---

## Feature 1: TrackBundle Export/Import

**New files:**
- `include/TrackBundle.h`
- `src/core/TrackBundle.cpp`

**New file extension:** `.lmms-track` (ZIP containing XML + resources)

### TrackBundle::exportTrack()

```cpp
// include/TrackBundle.h
namespace lmms {

class TrackBundle {
public:
    // Export a single track (or group) to a .lmms-track bundle
    static bool exportTrack(Track* track, const QString& filePath);

    // Export multiple tracks as a group
    static bool exportGroup(const QList<Track*>& tracks, const QString& filePath);

    // Import a bundle into a project, returns the created track(s)
    static QList<Track*> importBundle(const QString& filePath, TrackContainer* tc);

private:
    // Collect the mixer channel definition for a track
    static QDomElement saveMixerChannelDef(QDomDocument& doc, int mixerChannelIndex);

    // Collect automation clips that target models owned by this track
    static QList<AutomationClip*> findAutomationForTrack(Track* track);

    // Resolve mixer channel by name on import, creating if needed
    static int resolveOrCreateMixerChannel(const QDomElement& mixerDef);
};

} // namespace lmms
```

### Export flow (builds on existing code)

```cpp
bool TrackBundle::exportTrack(Track* track, const QString& filePath) {
    // 1. Create a DataFile of type TrackBundle (new enum value)
    DataFile dataFile(DataFile::Type::TrackBundle);
    QDomDocument& doc = dataFile;
    QDomElement& content = dataFile.content();

    // 2. Save the track using EXISTING saveTrack() — includes instrument,
    //    effects, clips, notes, everything
    QDomElement trackElem = doc.createElement("track");
    content.appendChild(trackElem);
    track->saveSettings(doc, trackElem);  // <-- already saves EVERYTHING

    // 3. NEW: Also save the mixer channel definition
    //    (the channel this track is routed to, with all its effects and sends)
    int mixChIdx = getMixerChannelIndex(track);  // read from m_mixerChannelModel
    if (mixChIdx > 0) {  // 0 = Master, always exists
        QDomElement mixerDef = saveMixerChannelDef(doc, mixChIdx);
        content.appendChild(mixerDef);
    }

    // 4. NEW: Also save automation clips that target this track's models
    QList<AutomationClip*> automations = findAutomationForTrack(track);
    if (!automations.isEmpty()) {
        QDomElement autoElem = doc.createElement("bundled_automation");
        content.appendChild(autoElem);
        for (auto* ac : automations) {
            ac->saveState(doc, autoElem);  // <-- existing saveState
        }
    }

    // 5. Bundle resources (samples) using EXISTING infrastructure
    dataFile.writeFile(filePath, true);  // withResources = true

    return true;
}
```

### saveMixerChannelDef() — the new piece

```cpp
QDomElement TrackBundle::saveMixerChannelDef(QDomDocument& doc, int idx) {
    // Reuse the EXACT same format Mixer::saveSettings uses,
    // but for a single channel, and add the name prominently
    MixerChannel* ch = Engine::mixer()->mixerChannel(idx);

    QDomElement mixerDef = doc.createElement("bundled_mixer_channel");

    // Store name as the primary identifier (NOT the index)
    mixerDef.setAttribute("name", ch->m_name);
    mixerDef.setAttribute("original_index", idx);  // reference only

    // Volume, mute, solo
    ch->m_volumeModel.saveSettings(doc, mixerDef, "volume");
    ch->m_muteModel.saveSettings(doc, mixerDef, "muted");
    ch->m_soloModel.saveSettings(doc, mixerDef, "soloed");

    // Color
    if (const auto& color = ch->color()) {
        mixerDef.setAttribute("color", color->name());
    }

    // Effect chain — uses EXISTING EffectChain::saveState
    ch->m_fxChain.saveState(doc, mixerDef);

    // Sends — store by NAME, not index
    for (const auto& send : ch->m_sends) {
        QDomElement sendElem = doc.createElement("send");
        MixerChannel* receiver = send->receiver();
        sendElem.setAttribute("channel_name", receiver->m_name);
        sendElem.setAttribute("original_channel_index", receiver->m_channelIndex);
        send->amount()->saveSettings(doc, sendElem, "amount");
        mixerDef.appendChild(sendElem);
    }

    return mixerDef;
}
```

### Import flow — resolveOrCreateMixerChannel()

```cpp
int TrackBundle::resolveOrCreateMixerChannel(const QDomElement& mixerDef) {
    QString channelName = mixerDef.attribute("name");

    // 1. Try to find existing channel by name
    Mixer* mixer = Engine::mixer();
    for (int i = 0; i < mixer->numChannels(); i++) {
        if (mixer->mixerChannel(i)->m_name == channelName) {
            return i;  // channel already exists, reuse it
        }
    }

    // 2. Not found — create a new channel
    int newIdx = mixer->createChannel();
    MixerChannel* ch = mixer->mixerChannel(newIdx);

    // 3. Apply ALL settings from the bundle
    ch->m_name = channelName;
    ch->m_volumeModel.loadSettings(mixerDef, "volume");
    ch->m_muteModel.loadSettings(mixerDef, "muted");
    ch->m_soloModel.loadSettings(mixerDef, "soloed");

    if (mixerDef.hasAttribute("color")) {
        ch->setColor(QColor{mixerDef.attribute("color")});
    }

    // 4. Restore effect chain
    QDomElement fxChainElem = mixerDef.firstChildElement(ch->m_fxChain.nodeName());
    if (!fxChainElem.isNull()) {
        ch->m_fxChain.restoreState(fxChainElem);
    }

    // 5. Restore sends — resolve targets by name too
    QDomNodeList sends = mixerDef.elementsByTagName("send");
    for (int i = 0; i < sends.count(); i++) {
        QDomElement sendElem = sends.at(i).toElement();
        QString targetName = sendElem.attribute("channel_name");

        // Find target channel by name (Master always exists at 0)
        int targetIdx = 0;  // default to Master
        for (int j = 0; j < mixer->numChannels(); j++) {
            if (mixer->mixerChannel(j)->m_name == targetName) {
                targetIdx = j;
                break;
            }
        }

        // Create the send route
        MixerRoute* route = mixer->createChannelSend(newIdx, targetIdx);
        if (route) {
            route->amount()->loadSettings(sendElem, "amount");
        }
    }

    return newIdx;
}

QList<Track*> TrackBundle::importBundle(const QString& filePath, TrackContainer* tc) {
    DataFile dataFile(filePath);
    QDomElement content = dataFile.content();
    QList<Track*> result;

    // 1. First, handle mixer channel creation (must exist before track loads)
    QDomElement mixerDef = content.firstChildElement("bundled_mixer_channel");
    int newMixerIdx = -1;
    if (!mixerDef.isNull()) {
        newMixerIdx = resolveOrCreateMixerChannel(mixerDef);
    }

    // 2. Load the track using EXISTING Track::create + loadSettings
    QDomElement trackElem = content.firstChildElement("track");
    while (!trackElem.isNull()) {
        auto type = static_cast<Track::Type>(trackElem.attribute("type").toInt());
        Track* track = Track::create(type, tc);

        // 3. Patch the mixer channel index BEFORE loading
        //    Replace the hardcoded index with the newly created channel
        if (newMixerIdx >= 0) {
            patchMixerChannelIndex(trackElem, newMixerIdx);
        }

        track->loadSettings(trackElem);  // <-- existing, loads everything
        result.append(track);

        trackElem = trackElem.nextSiblingElement("track");
    }

    // 4. Load bundled automation and resolve to new track's models
    QDomElement autoElem = content.firstChildElement("bundled_automation");
    if (!autoElem.isNull()) {
        loadBundledAutomation(autoElem, tc, result);
    }

    // 5. Resolve all automation IDs (existing mechanism)
    AutomationClip::resolveAllIDs();

    return result;
}
```

### Helper: patch mixer channel index in XML before load

```cpp
void TrackBundle::patchMixerChannelIndex(QDomElement& trackElem, int newIdx) {
    // Find the instrumenttrack or sampletrack child element
    // and replace the mixch value
    QDomNodeList children = trackElem.childNodes();
    for (int i = 0; i < children.count(); i++) {
        QDomElement child = children.at(i).toElement();
        if (child.isNull()) continue;

        QString tag = child.tagName();
        if (tag == "instrumenttrack" || tag == "sampletrack") {
            // Find the mixch attribute (stored as a value child element by IntModel)
            QDomNodeList mixchNodes = child.elementsByTagName("mixch");
            for (int j = 0; j < mixchNodes.count(); j++) {
                mixchNodes.at(j).toElement().setAttribute("value", newIdx);
            }
            // Also check direct attribute (older format)
            if (child.hasAttribute("mixch")) {
                child.setAttribute("mixch", newIdx);
            }
        }
    }
}
```

**Estimated new code:** ~400-500 lines in `TrackBundle.cpp`

**Existing code reused:**
- `Track::saveSettings()` / `loadSettings()` — all track data
- `EffectChain::saveState()` / `restoreState()` — all effects
- `Mixer::createChannel()` — channel creation
- `MixerRoute` / `createChannelSend()` — send routing
- `DataFile::writeFile(path, true)` — resource bundling
- `AutomationClip::resolveAllIDs()` — automation wiring

---

## Feature 2: Find Automation for a Track

The tricky part. Automation clips reference their targets by runtime journal ID. We need to find which automation clips target models owned by a specific track.

**Where to add:** `src/core/TrackBundle.cpp` (or as a utility in `AutomationClip`)

```cpp
QList<AutomationClip*> TrackBundle::findAutomationForTrack(Track* track) {
    QList<AutomationClip*> result;

    // Get all AutomatableModels owned by this track
    // InstrumentTrack owns: m_volumeModel, m_panningModel, m_pitchModel,
    //   m_mixerChannelModel, plus all instrument parameters, effect params, etc.

    // Use the existing static method:
    //   AutomationClip::clipsForModel(const AutomatableModel* m)
    // This searches ALL automation clips in the project for ones targeting a model.

    // Collect all AutomatableModel children of the track
    auto models = track->findChildren<AutomatableModel*>();

    for (auto* model : models) {
        auto clips = AutomationClip::clipsForModel(model);
        for (auto* clip : clips) {
            if (!result.contains(clip)) {
                result.append(clip);
            }
        }
    }

    return result;
}
```

This uses `QObject::findChildren<T>()` (Qt built-in) to discover all AutomatableModel instances in the track's object tree, then `AutomationClip::clipsForModel()` (already exists at `AutomationClip.cpp:1044`) to find their automation.

**Estimated new code:** ~30 lines

---

## Feature 3: UI Integration

### 3a. Right-click menu on tracks

**File to modify:** `src/gui/tracks/TrackView.cpp`

Add to the existing right-click context menu:

```cpp
// In TrackView::createContextMenu() or similar
contextMenu->addSeparator();
contextMenu->addAction(
    QPixmap(),  // icon
    tr("Export Track as Template..."),
    [this]() {
        FileDialog sfd(this, tr("Save Track Template"),
            "", tr("LMMS Track Bundle (*.lmms-track)"));
        sfd.setAcceptMode(FileDialog::AcceptSave);
        sfd.setDefaultSuffix("lmms-track");
        if (sfd.exec() == FileDialog::Accepted && !sfd.selectedFiles().isEmpty()) {
            TrackBundle::exportTrack(getTrack(), sfd.selectedFiles().first());
        }
    }
);
```

**Estimated new code:** ~15 lines added to existing function

### 3b. Import track from Song Editor

**File to modify:** `src/gui/editors/SongEditor.cpp`

Add to existing menu or toolbar:

```cpp
// In SongEditor context menu or File menu
action = new QAction(tr("Import Track Template..."), this);
connect(action, &QAction::triggered, [this]() {
    FileDialog ofd(this, tr("Open Track Template"),
        "", tr("LMMS Track Bundle (*.lmms-track)"));
    ofd.setAcceptMode(FileDialog::AcceptOpen);
    if (ofd.exec() == FileDialog::Accepted && !ofd.selectedFiles().isEmpty()) {
        Engine::audioEngine()->requestChangeInModel();
        auto tracks = TrackBundle::importBundle(
            ofd.selectedFiles().first(), Engine::getSong());
        Engine::audioEngine()->doneChangeInModel();

        if (tracks.isEmpty()) {
            QMessageBox::warning(this, tr("Import Failed"),
                tr("Could not import track template."));
        }
    }
});
```

**Estimated new code:** ~20 lines added to existing function

### 3c. Register .lmms-track in FileBrowser

**File to modify:** `src/gui/FileBrowser.cpp`

Add to the file type detection in `FileItem::determineFileType()`:

```cpp
else if (ext == "lmms-track") {
    m_type = FileType::Preset;        // or add a new FileType::TrackBundle
    m_handling = FileHandling::LoadAsTrackBundle;  // new enum value
}
```

And in the drop handler, add the new handling type:

```cpp
case FileItem::FileHandling::LoadAsTrackBundle: {
    Engine::audioEngine()->requestChangeInModel();
    TrackBundle::importBundle(f->fullName(), Engine::getSong());
    Engine::audioEngine()->doneChangeInModel();
    break;
}
```

**Estimated new code:** ~15 lines added to existing functions

---

## Feature 4: SQLite Project Index (Optional Enhancement)

This enables cross-project queries like "find all tracks using TripleOscillator" or "find all tracks at 140 BPM". It's an index, not a replacement for the XML.

**New files:**
- `include/ProjectIndex.h`
- `src/core/ProjectIndex.cpp`

**Dependency:** Qt's built-in `QSqlDatabase` + `QSqlQuery` (SQLite driver, already shipped with Qt)

```cpp
// include/ProjectIndex.h
namespace lmms {

class ProjectIndex {
public:
    ProjectIndex(const QString& dbPath);
    ~ProjectIndex();

    // Index a project file (called on save or on first scan)
    void indexProject(const QString& projectPath);

    // Index a track bundle
    void indexBundle(const QString& bundlePath);

    // Query tracks
    struct TrackInfo {
        QString projectPath;
        QString trackName;
        QString instrumentName;
        int bpm;
        int mixerChannel;
        QString mixerChannelName;
    };

    QList<TrackInfo> findTracksByInstrument(const QString& instrumentName);
    QList<TrackInfo> findTracksByName(const QString& namePattern);
    QList<TrackInfo> findTracksByBPM(int minBPM, int maxBPM);

    // Store raw XML for 1:1 reconstruction guarantee
    void storeRawProject(const QString& projectPath, const QByteArray& rawXml);
    QByteArray getRawProject(const QString& projectPath);

private:
    QSqlDatabase m_db;
    void createTables();
};

} // namespace lmms
```

**Schema:**

```sql
-- Projects we've indexed
CREATE TABLE projects (
    id INTEGER PRIMARY KEY,
    file_path TEXT UNIQUE NOT NULL,
    file_hash TEXT NOT NULL,
    raw_xml BLOB,                    -- optional: full 1:1 copy
    bpm INTEGER,
    time_sig TEXT,
    master_volume INTEGER,
    indexed_at INTEGER NOT NULL
);

-- Tracks within projects
CREATE TABLE tracks (
    id INTEGER PRIMARY KEY,
    project_id INTEGER REFERENCES projects(id),
    track_name TEXT NOT NULL,
    track_type INTEGER NOT NULL,     -- 0=instrument, 1=pattern, 2=sample, 5=automation
    instrument_name TEXT,            -- "tripleoscillator", "zynaddsubfx", etc.
    mixer_channel_name TEXT,
    volume REAL,
    panning REAL,
    has_effects INTEGER,
    clip_count INTEGER
);

-- Track bundles we've exported
CREATE TABLE bundles (
    id INTEGER PRIMARY KEY,
    file_path TEXT UNIQUE NOT NULL,
    source_project TEXT,
    source_track_name TEXT,
    instrument_name TEXT,
    created_at INTEGER NOT NULL
);

-- Full text search on track names
CREATE VIRTUAL TABLE tracks_fts USING fts5(track_name, instrument_name);
```

**Estimated new code:** ~300-400 lines

---

## Implementation Order

### Phase 1: Core TrackBundle (1 week)

| Step | What | Files | Lines |
|------|------|-------|-------|
| 1.1 | Add `DataFile::Type::TrackBundle` | `include/DataFile.h`, `src/core/DataFile.cpp` | ~5 |
| 1.2 | Create `TrackBundle` class with `exportTrack()` | `include/TrackBundle.h`, `src/core/TrackBundle.cpp` | ~200 |
| 1.3 | Add `saveMixerChannelDef()` | `src/core/TrackBundle.cpp` | ~60 |
| 1.4 | Add `findAutomationForTrack()` | `src/core/TrackBundle.cpp` | ~30 |
| 1.5 | Add `importBundle()` with `resolveOrCreateMixerChannel()` | `src/core/TrackBundle.cpp` | ~150 |
| 1.6 | Add `patchMixerChannelIndex()` | `src/core/TrackBundle.cpp` | ~30 |
| 1.7 | Add to CMakeLists.txt | `src/CMakeLists.txt` | ~2 |

**Test:** Export a track from Crunk(Demo).mmp (has mixer routing), import into a blank project, verify:
- Instrument loads with correct settings
- Mixer channel "Drum" is created with correct volume
- Send to Master exists
- All clips are present

### Phase 2: UI Integration (3 days)

| Step | What | Files | Lines |
|------|------|-------|-------|
| 2.1 | Right-click "Export Track as Template..." | `src/gui/tracks/TrackView.cpp` | ~15 |
| 2.2 | Song Editor "Import Track Template..." | `src/gui/editors/SongEditor.cpp` | ~20 |
| 2.3 | FileBrowser .lmms-track support | `src/gui/FileBrowser.cpp` | ~15 |
| 2.4 | Drag-and-drop from FileBrowser | `src/gui/FileBrowser.cpp` | ~10 |

**Test:** Full round-trip via UI — right-click export, then drag-drop import.

### Phase 3: Group Export (3 days)

| Step | What | Files | Lines |
|------|------|-------|-------|
| 3.1 | Multi-track selection in Song Editor | `src/gui/editors/SongEditor.cpp` | ~40 |
| 3.2 | `exportGroup()` — multiple tracks + shared mixer channels | `src/core/TrackBundle.cpp` | ~100 |
| 3.3 | `importBundle()` handles multiple tracks | Already done in Phase 1 | ~0 |

**Test:** Select 3 tracks in Crunk(Demo).mmp (Drum, Bass, Lead), export as group, import into blank project, verify all 3 mixer channels are created.

### Phase 4: SQLite Index (1 week, optional)

| Step | What | Files | Lines |
|------|------|-------|-------|
| 4.1 | Create `ProjectIndex` class | `include/ProjectIndex.h`, `src/core/ProjectIndex.cpp` | ~300 |
| 4.2 | Index on project save | `src/core/Song.cpp` | ~5 |
| 4.3 | Index on bundle export | `src/core/TrackBundle.cpp` | ~5 |
| 4.4 | Search UI (optional) | `src/gui/FileBrowser.cpp` | ~50 |

---

## What We're NOT Changing

- **The .mmp format itself** — fully backward compatible, existing projects load as before
- **The serialization framework** — `saveSettings()`/`loadSettings()` stays exactly as-is
- **The 30+ upgrade methods** — untouched
- **Plugin interfaces** — untouched
- **The Mixer class** — only calling existing public methods
- **The audio engine** — untouched

## Total New Code Estimate

| Component | Lines |
|---|---|
| TrackBundle.h | ~50 |
| TrackBundle.cpp | ~470 |
| UI additions (across 3 files) | ~100 |
| ProjectIndex.h + .cpp (optional) | ~350 |
| CMakeLists.txt changes | ~5 |
| **Total** | **~625 lines** (975 with SQLite index) |

For context, the existing codebase is ~83,000 lines in `src/` and ~54,000 in `plugins/`. This adds less than 1%.

---

## Risk Assessment

| Risk | Likelihood | Mitigation |
|---|---|---|
| Automation IDs don't resolve after import | Medium | Use existing `resolveAllIDs()` + test against all demos |
| Mixer channel name collision on import | Low | Prompt user: merge, rename, or pick existing |
| Binary plugin state not portable across platforms | Already exists | Same risk as current preset system — no change |
| Large sample files make bundles huge | Low | Same as existing `withResources` — user expects it |
| Pattern tracks have complex sub-track hierarchy | Medium | Test with CR8000.mpt and TR808.mpt templates |

---

## Automation ID Resolution: The Known Problem

The current `AutomationClip::resolveAllIDs()` has three nested fallback attempts:

```cpp
// Attempt 1: direct ID
JournallingObject* o = Engine::projectJournal()->journallingObject(id);
// Attempt 2: idFromSave(id)
o = Engine::projectJournal()->journallingObject(ProjectJournal::idFromSave(id));
// Attempt 3: idToSave(id)
o = Engine::projectJournal()->journallingObject(ProjectJournal::idToSave(id));
```

For track bundle import, this works because:
1. When we call `track->loadSettings(trackElem)`, the track's models register with the ProjectJournal and get IDs
2. When we then load the bundled automation clips, their `m_idsToResolve` get populated
3. When we call `resolveAllIDs()`, the models are already registered

The only risk is if the IDs collide with existing models in the destination project. To handle this, we can:
- Clear and re-assign IDs for the imported automation clips before resolution
- Or use the existing `ProjectJournal::idFromSave()` / `idToSave()` mapping

This is the same mechanism that already works for project load, copy-paste, and drag-and-drop. If it breaks, it would break those too.

---

## File Listing: What Gets Modified

**New files (3):**
```
include/TrackBundle.h
src/core/TrackBundle.cpp
src/CMakeLists.txt  (add TrackBundle.cpp to build)
```

**Modified files (5, minor additions):**
```
include/DataFile.h          — add Type::TrackBundle enum value
src/core/DataFile.cpp       — add "trackbundle" to type name map
src/gui/tracks/TrackView.cpp       — add right-click menu item
src/gui/editors/SongEditor.cpp     — add import menu item
src/gui/FileBrowser.cpp            — add .lmms-track handling
```

**Optional new files (2, for SQLite index):**
```
include/ProjectIndex.h
src/core/ProjectIndex.cpp
```
