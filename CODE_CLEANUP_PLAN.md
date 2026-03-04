# Code Cleanup Plan: FIXMEs, DRY Violations, and Code Smells

This document catalogs every issue found in the critical serialization/import/export
code paths and specifies how to fix each one. These cleanups should happen **before**
or **alongside** the TrackBundle feature work since they affect the same files.

---

## Cleanup 1: Extract AudioTrackMixin — Eliminate InstrumentTrack/SampleTrack Duplication

**Problem:** `InstrumentTrack` and `SampleTrack` copy-paste 15+ identical patterns
for volume, panning, mixer channel, and AudioPort management.

**Duplicated code (present in both files identically):**

| Pattern | InstrumentTrack.cpp | SampleTrack.cpp |
|---------|-------------------|-----------------|
| `m_volumeModel(DefaultVolume, MinVolume, MaxVolume, 0.1f, ...)` | line 62 | line 49 |
| `m_panningModel(DefaultPanning, PanningLeft, PanningRight, 0.1f, ...)` | line 63 | line 50 |
| `m_mixerChannelModel(0, 0, 0, ...)` | line 67 | line 51 |
| `m_audioPort(name, true, &m_volumeModel, &m_panningModel, &m_mutedModel)` | line 64 | line 52 |
| `m_panningModel.setCenterValue(DefaultPanning)` | line 78 | line 56 |
| `m_mixerChannelModel.setRange(0, Engine::mixer()->numChannels()-1, 1)` | line 83 | line 57 |
| `connect(&m_mixerChannelModel, ..., this, SLOT(updateMixerChannel()))` | line 112 | line 59 |
| `m_volumeModel.saveSettings(doc, elem, "vol")` | line 826 | line 197 |
| `m_panningModel.saveSettings(doc, elem, "pan")` | line 827 | line 198 |
| `m_mixerChannelModel.saveSettings(doc, elem, "mixch")` | line 831 | line 199 |
| `m_volumeModel.loadSettings(elem, "vol")` | line 896 | line 220 |
| `m_panningModel.loadSettings(elem, "pan")` | line 897 | line 221 |
| `m_mixerChannelModel.setRange(0, Engine::mixer()->numChannels()-1)` | line 900 | line 222 |
| `m_mixerChannelModel.loadSettings(elem, "mixch")` | line 903 | line 223 |
| `void updateMixerChannel() { m_audioPort.setNextMixerChannel(m_mixerChannelModel.value()); }` | line 673-676 | line 251-254 |

**Fix: Create an intermediate base class `AudioTrackBase`**

```cpp
// include/AudioTrackBase.h
#pragma once

#include "Track.h"
#include "AudioPort.h"
#include "panning_constants.h"
#include "volume.h"

namespace lmms {

// Base class for tracks that produce audio (InstrumentTrack, SampleTrack)
// Provides shared volume, panning, mixer channel, and AudioPort management.
class AudioTrackBase : public Track {
    Q_OBJECT
public:
    AudioTrackBase(Track::Type type, TrackContainer* tc, const QString& audioPortName);

    FloatModel* volumeModel() { return &m_volumeModel; }
    FloatModel* panningModel() { return &m_panningModel; }
    IntModel* mixerChannelModel() { return &m_mixerChannelModel; }
    AudioPort* audioPort() { return &m_audioPort; }

protected:
    // Call from subclass saveTrackSpecificSettings to save shared audio properties
    void saveAudioSettings(QDomDocument& doc, QDomElement& elem);

    // Call from subclass loadTrackSpecificSettings to load shared audio properties
    void loadAudioSettings(const QDomElement& elem);

    FloatModel m_volumeModel;
    FloatModel m_panningModel;
    IntModel m_mixerChannelModel;
    AudioPort m_audioPort;

private slots:
    void updateMixerChannel();
};

} // namespace lmms
```

```cpp
// src/core/AudioTrackBase.cpp
#include "AudioTrackBase.h"
#include "Engine.h"
#include "Mixer.h"

namespace lmms {

AudioTrackBase::AudioTrackBase(Track::Type type, TrackContainer* tc,
                               const QString& audioPortName)
    : Track(type, tc)
    , m_volumeModel(DefaultVolume, MinVolume, MaxVolume, 0.1f, this, tr("Volume"))
    , m_panningModel(DefaultPanning, PanningLeft, PanningRight, 0.1f, this, tr("Panning"))
    , m_mixerChannelModel(0, 0, 0, this, tr("Mixer channel"))
    , m_audioPort(audioPortName, true, &m_volumeModel, &m_panningModel, &m_mutedModel)
{
    m_panningModel.setCenterValue(DefaultPanning);
    m_mixerChannelModel.setRange(0, Engine::mixer()->numChannels() - 1, 1);
    connect(&m_mixerChannelModel, &IntModel::dataChanged,
            this, &AudioTrackBase::updateMixerChannel);
}

void AudioTrackBase::saveAudioSettings(QDomDocument& doc, QDomElement& elem) {
    m_volumeModel.saveSettings(doc, elem, "vol");
    m_panningModel.saveSettings(doc, elem, "pan");
    m_mixerChannelModel.saveSettings(doc, elem, "mixch");
}

void AudioTrackBase::loadAudioSettings(const QDomElement& elem) {
    m_volumeModel.loadSettings(elem, "vol");
    m_panningModel.loadSettings(elem, "pan");
    m_mixerChannelModel.setRange(0, Engine::mixer()->numChannels() - 1);
    m_mixerChannelModel.loadSettings(elem, "mixch");
}

void AudioTrackBase::updateMixerChannel() {
    m_audioPort.setNextMixerChannel(m_mixerChannelModel.value());
}

} // namespace lmms
```

**Then InstrumentTrack becomes:**
```cpp
class InstrumentTrack : public AudioTrackBase {  // was: Track
    // Remove: m_volumeModel, m_panningModel, m_mixerChannelModel, m_audioPort
    // Remove: updateMixerChannel()
    // Change: constructor calls AudioTrackBase(Track::Type::Instrument, tc, tr("unnamed_track"))
    // Change: saveTrackSpecificSettings calls saveAudioSettings(doc, elem) then its own stuff
    // Change: loadTrackSpecificSettings calls loadAudioSettings(elem) then its own stuff
};
```

**And SampleTrack becomes:**
```cpp
class SampleTrack : public AudioTrackBase {  // was: Track
    // Remove: m_volumeModel, m_panningModel, m_mixerChannelModel, m_audioPort
    // Remove: updateMixerChannel()
    // Change: constructor calls AudioTrackBase(Track::Type::Sample, tc, tr("Sample track"))
    // Change: saveTrackSpecificSettings calls saveAudioSettings(doc, elem) then its own stuff
    // Change: loadTrackSpecificSettings calls loadAudioSettings(elem) then its own stuff
};
```

**Lines eliminated:** ~50 duplicated lines removed, replaced by ~40 lines in one place.
**Files changed:** 5 (2 new, 3 modified)
**Risk:** Low — pure refactor, same behavior, just moved to a base class.

---

## Cleanup 2: Fix the Automation Triple-Fallback Hack

**Problem:** `AutomationClip::resolveAllIDs()` (AutomationClip.cpp:1067-1113) has three
nested attempts to resolve each automation target ID. The comments say "FIXME: Remove
this block once the automation system gets fixed" referencing GitHub issues #3781 and #4781.

**Current code:**
```cpp
// Attempt 1: raw ID
JournallingObject* o = Engine::projectJournal()->journallingObject(id);
if (o && dynamic_cast<AutomatableModel*>(o)) {
    a->addObject(...);
} else {
    // Attempt 2: idFromSave(id)  — sets MSB
    o = Engine::projectJournal()->journallingObject(ProjectJournal::idFromSave(id));
    if (o && dynamic_cast<AutomatableModel*>(o)) {
        a->addObject(...);
    } else {
        // Attempt 3: idToSave(id)  — clears MSB
        o = Engine::projectJournal()->journallingObject(ProjectJournal::idToSave(id));
        if (o && dynamic_cast<AutomatableModel*>(o)) {
            a->addObject(...);
        }
    }
}
```

**Root cause:** The journal ID system uses a most-significant-bit flag (`EO_ID_MSB`) to
distinguish between "save IDs" and "runtime IDs". When saving, `idToSave()` clears the
MSB. When loading, `idFromSave()` sets it. But somewhere in the pipeline, IDs get saved
or loaded without the correct transformation, so the triple-fallback tries all three
interpretations.

**Fix: Normalize IDs at save/load time, not at resolution time.**

```cpp
// Step 1: In AutomationClip::saveSettings(), always normalize before saving
void AutomationClip::saveSettings(QDomDocument& doc, QDomElement& parent) {
    // ... existing code ...
    for (const auto& object : m_objects) {
        if (object) {
            QDomElement element = doc.createElement("object");
            // ALWAYS use idToSave — this is the canonical save format
            element.setAttribute("id", ProjectJournal::idToSave(object->id()));
            parent.appendChild(element);
        }
    }
}

// Step 2: In AutomationClip::loadSettings(), always normalize after loading
void AutomationClip::loadSettings(const QDomElement& parent) {
    // ... existing code ...
    if (element.tagName() == "object") {
        // ALWAYS use idFromSave — convert from save format to runtime format
        jo_id_t id = ProjectJournal::idFromSave(element.attribute("id").toInt());
        m_idsToResolve.push_back(id);
    }
}

// Step 3: resolveAllIDs becomes simple — one lookup, no fallbacks
void AutomationClip::resolveAllIDs() {
    auto l = combineAllTracks();
    for (const auto& track : l) {
        if (track->type() == Track::Type::Automation
            || track->type() == Track::Type::HiddenAutomation) {
            for (const auto& clip : track->getClips()) {
                auto a = dynamic_cast<AutomationClip*>(clip);
                if (!a) continue;

                for (const auto& id : a->m_idsToResolve) {
                    auto* o = dynamic_cast<AutomatableModel*>(
                        Engine::projectJournal()->journallingObject(id));
                    if (o) {
                        a->addObject(o, false);
                    }
                    // If not found, it's genuinely missing — don't silently try
                    // other interpretations. Log a warning instead.
                    else {
                        qWarning() << "AutomationClip: Could not resolve target ID"
                                   << id << "for clip" << a->name();
                    }
                }
                a->m_idsToResolve.clear();
                a->dataChanged();
            }
        }
    }
}
```

**Risk:** Medium — must test against all 28 demo projects + legacy files.
The 30+ upgrade methods in DataFile already normalize old formats before load, so by the
time `loadSettings` runs, the XML should have consistent ID formats. The triple-fallback
was masking bugs in the upgrade pipeline. By fixing the root cause (normalize at
save/load boundaries), the resolution becomes simple and predictable.

**Backward compatibility:** Old projects with inconsistent IDs will still load correctly
because the upgrade methods run first (in `DataFile::upgrade()`). If any edge cases
remain, we add a new upgrade method rather than keeping the triple-fallback.

---

## Cleanup 3: Centralize resolveAllIDs() Calls

**Problem:** `AutomationClip::resolveAllIDs()` is called from **7 different locations:**

| File | Line | Trigger |
|------|------|---------|
| `Song.cpp` | 1185 | Project load |
| `Track.cpp` | 171 | Track clone/paste |
| `Clip.cpp` | 148 | Clip paste |
| `ProjectJournal.cpp` | 76 | Undo/redo |
| `ClipView.cpp` | 482 | Drag-and-drop |
| `TrackContentWidget.cpp` | 548 | Paste operation |
| `LadspaEffect.cpp` | 126 | Plugin sample rate change |

**Why this is bad:**
- Easy to forget to call it when adding new load/paste paths
- The function scans ALL tracks in the ENTIRE project every time — O(n*m) where n=tracks, m=clips
- Called redundantly (e.g., pasting 5 clips calls it 5 times)

**Fix: Deferred batch resolution with a dirty flag.**

```cpp
// In AutomationClip.h, add:
class AutomationClip : public Clip {
    // ...
    static void markNeedsResolution();   // call this instead of resolveAllIDs
    static void resolveIfNeeded();       // called once per event loop cycle
    static void resolveAllIDs();         // existing, now private to the batch mechanism

private:
    static bool s_needsResolution;
};

// In AutomationClip.cpp:
bool AutomationClip::s_needsResolution = false;

void AutomationClip::markNeedsResolution() {
    if (!s_needsResolution) {
        s_needsResolution = true;
        // Defer to next event loop iteration — batches multiple calls
        QMetaObject::invokeMethod(
            Engine::getSong(), [](){ AutomationClip::resolveIfNeeded(); },
            Qt::QueuedConnection);
    }
}

void AutomationClip::resolveIfNeeded() {
    if (s_needsResolution) {
        s_needsResolution = false;
        resolveAllIDs();
    }
}
```

Then replace all 7 callers with `AutomationClip::markNeedsResolution()`.

**Exception:** `Song::loadProject()` still needs synchronous resolution (the project must
be fully resolved before the UI renders). Keep the direct `resolveAllIDs()` call there,
but call `resolveIfNeeded()` instead of `resolveAllIDs()` so the flag gets cleared.

**Lines changed:** ~10 lines per caller (7 callers) + ~20 lines new mechanism = ~30 net new.
**Risk:** Low — same function gets called, just batched. Song load stays synchronous.

---

## Cleanup 4: Fix TODO in Song.cpp — Journal Batching

**Problem:** Two identical TODOs at Song.cpp:783 and Song.cpp:798:
```cpp
// FIXME journal batch of tracks instead of each track individually
```

These are in `Song::addBar()` and `Song::removeBar()`. Currently, adding/removing a bar
creates individual journal entries for every track's clip, making undo/redo slow and
producing many undo steps for a single logical operation.

**Fix:**

```cpp
void Song::addBar() {
    // Wrap the entire operation in a single journal checkpoint
    Engine::projectJournal()->checkpoint();

    TrackList tl = tracks();
    for (auto& track : tl) {
        track->addBar();
    }

    // Single undo point for the entire operation
    Engine::projectJournal()->checkpoint();
}
```

**Lines changed:** ~6 per function (2 functions) = ~12 total.
**Risk:** Low — standard journal API usage.

---

## Cleanup 5: Fix TODO in AutomationClip — Negative Length Handling

**Problem:** AutomationClip.cpp:930:
```cpp
// TODO: Handle with an upgrade method
```

When loading old projects, some automation clips can end up with negative lengths, which
gets silently clamped. This should be an explicit upgrade method.

**Fix:** Add `upgrade_fixNegativeAutomationLengths()` to DataFile.cpp:

```cpp
void DataFile::upgrade_fixNegativeAutomationLengths() {
    QDomNodeList automationClips = elementsByTagName("automationclip");
    for (int i = 0; i < automationClips.count(); i++) {
        QDomElement elem = automationClips.at(i).toElement();
        int len = elem.attribute("len", "0").toInt();
        if (len < 0) {
            // Recalculate length from the last time node
            int maxPos = 0;
            QDomNodeList timeNodes = elem.elementsByTagName("time");
            for (int j = 0; j < timeNodes.count(); j++) {
                int pos = timeNodes.at(j).toElement().attribute("pos", "0").toInt();
                if (pos > maxPos) maxPos = pos;
            }
            elem.setAttribute("len", maxPos > 0 ? maxPos + 1 : 192);  // default 1 bar
            qInfo() << "Fixed negative automation clip length:" << len << "→" << elem.attribute("len");
        }
    }
}
```

Add to `UPGRADE_METHODS` and `UPGRADE_VERSIONS` vectors.

**Lines changed:** ~25
**Risk:** Low — only affects projects with the specific bug, and the fix is conservative.

---

## Cleanup 6: Remove Dead Code and #if 0 Blocks

**Problem:** SampleTrack.cpp:194-196:
```cpp
#if 0
	_this.setAttribute( "icon", tlb->pixmapFile() );
#endif
```

Dead code that was disabled but never removed.

**Fix:** Delete the 3 lines.

---

## Cleanup 7: Modernize Signal-Slot Connections

**Problem:** InstrumentTrack and SampleTrack use the old string-based signal/slot syntax:
```cpp
connect(&m_mixerChannelModel, SIGNAL(dataChanged()), this, SLOT(updateMixerChannel()), Qt::DirectConnection);
```

This is not type-safe and won't catch errors at compile time.

**Fix:** Use the modern syntax (already used elsewhere in the codebase):
```cpp
connect(&m_mixerChannelModel, &IntModel::dataChanged, this, &AudioTrackBase::updateMixerChannel);
```

This is handled automatically by Cleanup 1 (AudioTrackBase extraction).

---

## Cleanup 8: FileBrowser.cpp Threading TODO

**Problem:** FileBrowser.cpp lines 726 and 744:
```cpp
// TODO: We should do this work outside the event thread
// TODO: this can be removed once we do this outside the event thread
```

Preset preview loading blocks the UI thread.

**Fix (minimal):** Move the DataFile loading to a QThread worker:

```cpp
// Create a small worker for async preset loading
class PresetLoader : public QRunnable {
    QString m_path;
    std::function<void(DataFile*)> m_callback;
public:
    PresetLoader(const QString& path, std::function<void(DataFile*)> cb)
        : m_path(path), m_callback(std::move(cb)) { setAutoDelete(true); }
    void run() override {
        auto* df = new DataFile(m_path);
        QMetaObject::invokeMethod(qApp, [this, df]() { m_callback(df); });
    }
};
```

**Risk:** Medium — need to ensure the callback handles the case where the widget was
destroyed while loading. Use a QPointer guard.
**Priority:** Low — this is a UX improvement, not a correctness issue.

---

## Implementation Priority

| Priority | Cleanup | Effort | Risk | Why |
|----------|---------|--------|------|-----|
| **P0** | #1 AudioTrackBase | 1 day | Low | Directly needed for TrackBundle — `saveAudioSettings`/`loadAudioSettings` is the interface TrackBundle calls |
| **P0** | #6 Dead code removal | 5 min | None | Trivial |
| **P1** | #2 Fix automation triple-fallback | 1 day | Medium | Makes automation portable across projects (critical for import/export) |
| **P1** | #3 Centralize resolveAllIDs | 0.5 day | Low | Needed so TrackBundle import has a clean resolution path |
| **P2** | #4 Journal batching | 0.5 day | Low | Existing FIXME, quick win |
| **P2** | #5 Negative automation length | 0.5 day | Low | Existing TODO, proper upgrade method |
| **P2** | #7 Modernize signals | Done in #1 | None | Comes free with AudioTrackBase |
| **P2** | #9 Rename INVAL/OUTVAL macros | 0.5 day | Low | Convention violation |
| **P2** | #10 Fix objectDestroyed TODO | 0.5 day | Medium | Unimplemented logic for permanent vs temporary removal |
| **P2** | #11 Add error logging for failed resolution | 0.5 day | Low | Silent failures are dangerous |
| **P3** | #8 Async preset loading | 1 day | Medium | UX improvement, not blocking |

**Total effort for P0+P1:** ~2.5 days
**Total effort for all:** ~7 days

---

## Cleanup 9: Rename INVAL/OUTVAL/INTAN/OUTTAN/POS Macros

**Problem:** AutomationClip.h:259-295 defines inline functions with UPPERCASE names:
```cpp
inline float INVAL(AutomationClip::TimemapIterator it)
inline float OUTVAL(AutomationClip::TimemapIterator it)
inline float OFFSET(AutomationClip::TimemapIterator it)
inline float INTAN(AutomationClip::TimemapIterator it)
inline float OUTTAN(AutomationClip::TimemapIterator it)
inline float LOCKEDTAN(AutomationClip::TimemapIterator it)
inline int POS(AutomationClip::TimemapIterator it)
```

These look like preprocessor macros but are functions. Violates C++ naming conventions.

**Fix:** Rename to camelCase:
```cpp
inline float inValue(AutomationClip::TimemapIterator it)
inline float outValue(AutomationClip::TimemapIterator it)
inline float offset(AutomationClip::TimemapIterator it)
inline float inTangent(AutomationClip::TimemapIterator it)
inline float outTangent(AutomationClip::TimemapIterator it)
inline bool lockedTangent(AutomationClip::TimemapIterator it)
inline int position(AutomationClip::TimemapIterator it)
```

Update all call sites in AutomationClip.cpp (saveSettings, valueAt, etc.).

---

## Cleanup 10: Fix objectDestroyed TODO

**Problem:** AutomationClip.cpp:1130-1152:
```cpp
void AutomationClip::objectDestroyed(jo_id_t _id) {
    // TODO: distict between temporary removal (e.g. LADSPA controls
    // when switching samplerate) and real deletions because in the latter
    // case we had to remove ourselves if we're the global automation
    // clip of the destroyed object
    m_idsToResolve.push_back(_id);  // adds to "resolve later" — wrong for permanent deletions
```

When a model is permanently destroyed (track deleted), the automation clip adds the dead
ID to `m_idsToResolve` hoping it'll come back. It won't. The clip is now silently broken.

**Fix:** Add a `permanent` flag:
```cpp
void AutomationClip::objectDestroyed(jo_id_t id, bool permanent) {
    QMutexLocker m(&m_clipMutex);

    if (permanent) {
        // Object is gone forever — remove from our targets
        // If we're a global automation clip for this object, remove ourselves
        m_objects.erase(
            std::remove_if(m_objects.begin(), m_objects.end(),
                [id](const QPointer<AutomatableModel>& obj) {
                    return !obj || obj->id() == id;
                }),
            m_objects.end()
        );
    } else {
        // Temporary removal (sample rate change, effect reload)
        // Save ID for re-resolution
        m_idsToResolve.push_back(id);
    }
}
```

---

## Cleanup 11: Add Error Logging for Failed Automation Resolution

**Problem:** When `resolveAllIDs()` fails to find a target for an automation clip,
it silently drops the connection. The user never knows their automation is broken.

**Fix:** Already handled in Cleanup #2 (the simplified `resolveAllIDs`), but also add
a post-resolution audit:

```cpp
// At the end of resolveAllIDs():
for (const auto& track : l) {
    if (track->type() == Track::Type::Automation) {
        for (const auto& clip : track->getClips()) {
            auto a = dynamic_cast<AutomationClip*>(clip);
            if (a && a->m_objects.empty() && !a->m_idsToResolve.empty()) {
                qWarning() << "AutomationClip" << a->name()
                           << "has no resolved targets — automation will not function."
                           << "Unresolved IDs:" << a->m_idsToResolve;
            }
        }
    }
}
```

---

## Files Modified Summary

| File | Cleanups | Nature of Change |
|------|----------|-----------------|
| **NEW** `include/AudioTrackBase.h` | #1 | New base class |
| **NEW** `src/core/AudioTrackBase.cpp` | #1 | New base class implementation |
| `include/InstrumentTrack.h` | #1 | Change parent class, remove duplicated members |
| `src/tracks/InstrumentTrack.cpp` | #1, #7 | Remove duplicated code, call base class |
| `include/SampleTrack.h` | #1 | Change parent class, remove duplicated members |
| `src/tracks/SampleTrack.cpp` | #1, #6, #7 | Remove duplicated code, call base class, remove dead code |
| `src/core/AutomationClip.cpp` | #2, #3, #5 | Fix triple-fallback, add deferred resolution, fix TODO |
| `include/AutomationClip.h` | #3 | Add markNeedsResolution/resolveIfNeeded |
| `src/core/Song.cpp` | #3, #4 | Replace resolveAllIDs calls, add journal batching |
| `src/core/Track.cpp` | #3 | Replace resolveAllIDs call |
| `src/core/Clip.cpp` | #3 | Replace resolveAllIDs call |
| `src/core/ProjectJournal.cpp` | #3 | Replace resolveAllIDs call |
| `src/gui/clips/ClipView.cpp` | #3 | Replace resolveAllIDs call |
| `src/gui/tracks/TrackContentWidget.cpp` | #3 | Replace resolveAllIDs call |
| `plugins/LadspaEffect/LadspaEffect.cpp` | #3 | Replace resolveAllIDs call |
| `src/core/DataFile.cpp` | #5 | Add upgrade method |
| `src/gui/FileBrowser.cpp` | #8 (P3) | Async loading |
| `src/CMakeLists.txt` | #1 | Add AudioTrackBase.cpp |
