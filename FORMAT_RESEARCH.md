# Project Format Research: Problems, Alternatives, and Template Portability

This document captures the deep analysis of LMMS's `.mmp` project format, evaluates alternatives used across the DAW industry, and proposes a hybrid architecture that solves the template/track portability problem.

---

## Part 1: The XML Problem

### How .mmp Works Today

- **Format:** Plain XML (`.mmp`) or gzip-compressed XML (`.mmpz`)
- **Parser:** Qt's `QDomDocument` — a DOM parser that loads the **entire** tree into memory before any data can be accessed
- **Binary data:** Base64-encoded inline in XML attributes (wavetables, plugin state, sample shapes)
- **Versioning:** 30+ hand-written upgrade methods in `DataFile.cpp` that walk and mutate the DOM tree
- **Serialization pattern:** Every model class implements `saveSettings(QDomDocument&, QDomElement&)` and `loadSettings(const QDomElement&)` — XML is wired into every object in the codebase
- **`DataFile` inherits from `QDomDocument`** — the XML DOM is literally the data model, not just a serialization format
- **Resource bundling:** Optional directory-based bundles where the `.mmp` becomes a folder with a `resources/` subdirectory

### What's Wrong With It

| Problem | Impact |
|---|---|
| **DOM parsing is all-or-nothing** | Opening a 200-track project loads every note, every automation point, every plugin state into memory before a single sound plays. No partial loading possible. |
| **Base64 bloat** | Binary data inflated by 33%. A 1MB wavetable becomes 1.33MB of ASCII in the XML. Parsing that ASCII is slower than reading raw bytes. |
| **Autosave rewrites everything** | Saving means serializing the entire DOM tree to text. For large projects this blocks the UI thread or requires a deep copy of the full tree for background saving. |
| **No crash resilience** | If the write is interrupted (crash, power loss), the file is corrupt. There's no journaling or atomic write. |
| **XML is deeply coupled** | `DataFile` inherits from `QDomDocument`. Every model class takes `QDomElement&` parameters. This isn't a format — it's the data model itself. Changing it means touching every class. |
| **Schema evolution is painful** | 30+ sequential upgrade functions (`upgrade_0_2_1_20070501` through `upgrade_fixBassLoopsTypo`) that manually traverse and rewrite XML nodes. Each format change requires a new hand-written migration. |
| **Poor diff/merge** | Attribute-heavy single-line elements diff badly. Base64 blobs are opaque. Reordering of semantically-identical elements creates spurious conflicts. |
| **No track portability** | A track's data is embedded in the monolithic XML tree. There is no way to extract a track with its instrument, effects, automation, and samples as a portable unit. Presets only save instrument settings, not the full track graph. |

### What Other DAWs Do

| DAW | Format | Notes |
|---|---|---|
| **Ableton Live** | Gzipped XML (`.als`) | Same fundamental problems as LMMS. VST state stored as hex blobs. Files reach 100k+ lines uncompressed. |
| **FL Studio** | Custom binary (`.flp`) | Compact and fast, but completely opaque. Proprietary, no tooling. |
| **REAPER** | Custom text (`.rpp`) | Human-readable, git-friendly, tolerant of missing data (fills defaults). Proven durable over ~20 years. Track templates are just `.rpp` fragments. |
| **Ardour** | XML + session directory | XML metadata, audio as separate files in subdirectories. |
| **Audacity 3.0** | SQLite (`.aup3`) | Migrated FROM XML+directory TO SQLite. Audio chunks stored as BLOBs. Incremental autosave via WAL mode. |
| **DAWproject** | ZIP container (XML + media) | Open exchange format by Bitwig/PreSonus. ZIP with `project.xml` + separate audio/plugin entries. |

---

## Part 2: Format Candidates Evaluated

### Summary Table

| Format | Structured Data | Binary Blobs | Partial Load | Autosave | Human Readable | Git-Friendly | Track Portability |
|---|---|---|---|---|---|---|---|
| XML (current) | Good | Poor (base64) | No | No (full rewrite) | Yes | Fair | No |
| JSON | Good | Poor (base64) | No | No (full rewrite) | Yes | Good | No (monolithic) |
| MessagePack/CBOR | Good | Good (native bytes) | No | No (full rewrite) | No | No | No |
| Protocol Buffers | Excellent | Good | No | No (full rewrite) | No | No | No |
| FlatBuffers | Good | Fair | Yes (zero-copy) | No (write-once) | No | No | No |
| SQLite | Good | Good (BLOBs) | Yes (SQL) | Excellent (WAL) | No | No | Possible |
| ZIP container | Depends | Excellent (separate entries) | Yes (per-entry) | No (full rewrite) | Inspectable | Fair | Yes |
| Custom binary | Full control | Full control | Possible | Possible | No | No | Must implement |

### Detailed Evaluations

#### FlatBuffers / Cap'n Proto (Zero-Copy Deserialization)
- Zero deserialization overhead — access fields via pointer offsets directly
- **Arena allocation model** — long-lived mutable documents leak memory; can't free individual objects
- Write-once design conflicts with DAW's continuous mutation pattern
- Binary, git-hostile, no human readability
- **Verdict:** Poor fit as primary format. Good for IPC or undo snapshots.

#### MessagePack / CBOR (Binary JSON)
- Drop-in replacement for JSON with native binary type (no base64)
- 40-60% smaller than JSON/XML
- No random access, no partial loading, no schema evolution
- CBOR is IETF-standardized; MessagePack has broader adoption
- **Verdict:** Good as a component inside a container. Not standalone.

#### Protocol Buffers
- Excellent schema evolution via field numbers
- Very compact (varint encoding)
- Must parse entire message — no random access
- Default 64MB message size limit
- **Verdict:** Strong for structured metadata inside a container. Not for monolithic files with embedded samples.

#### SQLite
- True random access via SQL, incremental autosave via WAL mode
- Crash-resilient (ACID transactions), single-file packaging
- Completely binary, git-hostile, relational model awkward for hierarchical DAW data
- **Verdict:** Best for autosave journal. Not ideal as primary format due to readability and diffability loss.

#### ZIP Container
- Separation of concerns — metadata in one file, binary assets as separate entries
- Partial loading via central directory random access
- Standard tooling, human-inspectable (unzip and read)
- No incremental autosave — modifying requires rewriting
- **Verdict:** Excellent for save/share format. Poor for live editing.

#### JSON
- Simpler than XML (no closing tags, no namespaces, no DTDs)
- ~30-50% smaller, 2-5x faster to parse
- Better git diffs (shorter lines, less redundancy)
- Still no binary data type (base64 still needed for monolithic files)
- **Verdict:** Best choice per-file in a directory structure. Not as a monolithic replacement.

**No single format satisfies all requirements. The answer is a hybrid.**

---

## Part 3: The Track Dependency Graph (The Portability Problem)

### What "a track" actually contains

This is the core problem. A track is not a flat object — it's a deep dependency graph. Here's what an InstrumentTrack owns:

```
InstrumentTrack
├── Track parameters (volume, panning, pitch, pitch range, mute, solo, color)
├── Instrument (plugin)
│   ├── Plugin descriptor (name, type)
│   ├── Plugin-specific state (binary blob — wavetable data, oscillator config, etc.)
│   └── SubPluginFeatures::Key (for multi-preset plugins)
├── InstrumentSoundShaping
│   ├── Filter settings (enabled, type, cutoff, resonance)
│   └── EnvelopeAndLfoParameters[3] (Volume, Cut, Resonance)
│       ├── Envelope (predelay, attack, hold, decay, sustain, release, amount)
│       └── LFO (speed, amount, shape, user-defined wave)
├── InstrumentFunctionNoteStacking (chord settings)
├── InstrumentFunctionArpeggio (arpeggiator settings)
├── EffectChain
│   └── Effect[] (ordered list)
│       ├── Plugin descriptor
│       ├── Plugin-specific state (binary blob)
│       ├── Wet/dry, gate, decay settings
│       └── Effect's own EffectChain (effects can be nested!)
├── MidiPort (MIDI channel, input/output device assignments)
├── Piano (key states for piano roll display)
├── Microtuner (custom tuning settings)
├── MIDI CC models[128] (controller assignments)
├── Clips[] (the actual musical content)
│   └── MidiClip (for InstrumentTrack)
│       ├── Notes[] (key, velocity, position, length, panning, detuning)
│       └── Position, length, name, color, mute state
├── Mixer channel assignment (send routing)
└── AutomationClips[] (on a SEPARATE AutomationTrack)
    ├── Target object reference (by journal ID — session-specific!)
    ├── Time/value keyframe pairs
    └── Progression type (discrete, linear, cubic hermite)
```

### Why this is hard to make portable today

1. **Automation is stored separately.** Automation clips live on AutomationTrack objects, not on the InstrumentTrack they control. An automation clip targeting "Track 3's filter cutoff" references it by a `ProjectJournal` ID — an integer assigned at runtime. If you extract Track 3 from the project, the automation clips on the AutomationTrack still point to the old ID, which won't exist in the new project.

2. **Mixer channel routing is absolute.** Track 3 might route to Mixer Channel 7. But the destination project might not have a Channel 7, or it might be a completely different channel with different effects.

3. **Sample paths are filesystem-dependent.** Samples are referenced by file path. Moving a track to another machine means the paths are broken.

4. **Plugin state is opaque binary.** Each plugin serializes its own state as a binary blob. There's no way to inspect or remap references inside plugin state (e.g., a sampler plugin that references files by path internally).

5. **Effects have their own dependency graphs.** Each effect in the chain has its own plugin state, its own automatable parameters, and potentially its own automation clips on separate AutomationTracks.

6. **Presets only save instrument settings.** The existing `savePreset`/`loadPreset` mechanism (`Track::saveTrack(doc, element, true)`) saves the instrument plugin state and track parameters, but explicitly **skips clips** (the actual musical content). It also skips MIDI port settings. It does NOT collect associated automation clips from other tracks.

### What "portable" means concretely

A user should be able to:
- **Export** a track (or group of tracks) from Project A as a self-contained package
- **Import** that package into Project B with all child relationships intact
- The imported track should sound identical — same instrument, same effects, same automation, same samples
- Automation should reconnect to the correct parameters on the imported track
- Samples should be bundled, not referenced by absolute path

---

## Part 4: Recommended Architecture

### The Hybrid Format

Three formats for three roles:

#### 1. Working Format — Exploded Directory

While a project is open, it lives as a directory on disk:

```
MyProject.lmms-project/
├── project.json              # song structure, mixer routing, global settings
├── .format-version           # format version for migration
├── tracks/
│   ├── track_001.json        # per-track: instrument config, clip data, notes
│   ├── track_002.json
│   └── track_003.json
├── automation/
│   ├── track_001.json        # automation curves (time/value pairs)
│   └── global.json           # tempo, master vol automation
├── samples/
│   ├── kick.wav              # raw audio files — no base64, no encoding
│   ├── snare.wav
│   └── pad_loop.ogg
├── plugin-state/
│   ├── track_001_inst.bin    # plugin state as raw binary
│   └── track_002_fx_0.bin
└── .autosave/
    └── journal.sqlite        # autosave journal (see below)
```

**Why this solves the core problems:**

- **Partial loading:** Read only `track_001.json` when the user opens that track. Samples loaded on demand.
- **No base64:** Samples and plugin state are raw files. Zero encoding overhead.
- **Git-friendly:** Each track is a separate JSON file. Diffs are meaningful. Two people can edit different tracks and merge.
- **Human-inspectable:** Open any file in a text editor.
- **Crash-resilient:** Losing one track file doesn't destroy the project.
- **Track portability:** Each track is already isolated — extracting it is a matter of collecting its files.

#### 2. Save/Share Format — ZIP Container (`.lmms`)

When the user saves or exports, the directory is packed into a standard ZIP:

```
MyProject.lmms
  └── (ZIP contents = the directory structure above, minus .autosave/)
```

- Single file for sharing
- Anyone can rename to `.zip` and inspect contents
- Follows the proven DAWproject / DOCX / ODT pattern

#### 3. Autosave — SQLite Journal

The `.autosave/journal.sqlite` tracks incremental changes:

- **Incremental:** Only modified files are written per autosave tick
- **WAL mode:** SQLite's write-ahead log means autosave never blocks the audio thread
- **Crash recovery:** On startup, check for journal and offer recovery
- **Undo history:** The journal naturally provides a timeline of changes

---

## Part 5: Solving Template & Track Portability

### The Key Insight

The directory-based working format doesn't *inhibit* portability — it *enables* it. Because each track's data is already in separate files, a "track template" is just a **subset ZIP** containing only the files that belong to that track and its children.

### Track Bundle Format (`.lmms-track`)

A `.lmms-track` file is a ZIP containing a self-contained track with all its dependencies:

```
MyBassTrack.lmms-track
├── manifest.json             # metadata, format version, contents inventory
├── track.json                # the track definition (instrument, params, clips)
├── effects/
│   ├── fx_0.json             # effect chain entries with settings
│   ├── fx_0_state.bin        # effect plugin binary state
│   ├── fx_1.json
│   └── fx_1_state.bin
├── automation/
│   ├── volume.json           # automation curves with SYMBOLIC references
│   ├── filter_cutoff.json
│   └── fx_0_wetdry.json
├── instrument/
│   ├── instrument.json       # instrument plugin config
│   └── state.bin             # instrument plugin binary state
├── samples/                  # all referenced samples, bundled
│   ├── bass_loop.wav
│   └── sub_hit.ogg
└── clips/
    ├── clip_0.json           # MIDI clip data (notes, position)
    └── clip_1.json
```

### Solving the Hard Problems

#### Problem 1: Automation References

**Current approach (broken for portability):** AutomationClip stores `object.id()` — a `ProjectJournal` integer assigned at runtime. This ID means nothing outside the current session.

**Solution: Symbolic parameter paths.** Instead of referencing by journal ID, automation targets are identified by a human-readable path:

```json
// automation/filter_cutoff.json
{
  "target": "track/sound_shaping/filter_cutoff",
  "progression": "cubic_hermite",
  "tension": 0.0,
  "keyframes": [
    { "pos": 0, "value": 5000.0 },
    { "pos": 192, "value": 800.0 },
    { "pos": 384, "value": 5000.0 }
  ]
}
```

```json
// automation/fx_0_wetdry.json
{
  "target": "track/effects/0/wet_dry",
  "progression": "linear",
  "keyframes": [
    { "pos": 0, "value": 0.5 },
    { "pos": 384, "value": 1.0 }
  ]
}
```

The `target` field is a **path relative to the track**, not a global ID. When the track is imported into a new project, the importer walks the path to find the actual parameter object. This is:
- **Self-contained:** No external ID registry needed
- **Human-readable:** You can see what parameter is being automated
- **Relocatable:** Works regardless of which project the track is imported into
- **Debuggable:** If a reference breaks, the path tells you exactly what's missing

The path scheme:
```
track/volume                          → track volume knob
track/panning                         → track panning knob
track/pitch                           → track pitch
track/sound_shaping/filter_cutoff     → instrument filter cutoff
track/sound_shaping/filter_resonance  → instrument filter resonance
track/sound_shaping/env_volume/attack → volume envelope attack
track/sound_shaping/lfo_cut/speed     → cutoff LFO speed
track/instrument/<param_name>         → instrument-specific parameter
track/effects/0/wet_dry               → first effect's wet/dry
track/effects/0/<param_name>          → first effect's parameter
track/effects/1/wet_dry               → second effect's wet/dry
track/arpeggio/speed                  → arpeggiator speed
track/note_stacking/range             → chord range
```

#### Problem 2: Mixer Channel Routing

**Current approach:** Track stores an absolute mixer channel index (e.g., "route to channel 7").

**Solution: Symbolic mixer references + import-time remapping.**

In the track bundle:
```json
// track.json
{
  "mixer_channel": {
    "label": "Bass Bus",
    "original_index": 7
  }
}
```

On import, the system:
1. Looks for a mixer channel named "Bass Bus" in the target project
2. If found, routes to it
3. If not found, offers to: (a) create a new channel with that name, (b) route to master, or (c) let the user pick

#### Problem 3: Sample References

**Current approach:** Samples referenced by filesystem path (absolute or relative to LMMS data directory).

**Solution: All referenced samples are bundled in the `.lmms-track` ZIP.**

```json
// track.json (instrument section)
{
  "instrument": {
    "name": "AudioFileProcessor",
    "sample": "samples/bass_loop.wav"   // path relative to bundle root
  }
}
```

On import, samples are extracted to the target project's `samples/` directory. If a sample with the same name already exists (and has the same checksum), reuse it. If name collision with different content, rename with a suffix.

For plugin state (opaque binary blobs), samples referenced *inside* plugin state can't be remapped — but the bundle includes all samples the track used, and the plugin state references them by the same relative path. This works as long as the extracted samples land in the expected location.

#### Problem 4: Effect Chain Portability

Effects and their ordering must travel with the track:

```json
// effects/fx_0.json
{
  "index": 0,
  "plugin": "ReverbSC",
  "enabled": true,
  "wet_dry": 0.65,
  "gate": -60.0,
  "decay": 500,
  "plugin_params": {
    "room_size": 0.8,
    "damping": 0.5,
    "bandwidth": 0.9
  }
}
```

The binary state file (`fx_0_state.bin`) is the plugin's opaque state for exact reproduction. The JSON params are the human-readable fallback and serve as documentation.

#### Problem 5: Group/Multi-Track Templates

A user might want to export a *group* of tracks — e.g., a drum kit with 8 instrument tracks routed to a drum bus, with automation across all of them.

**Solution: Track Group Bundle (`.lmms-group`)**

Same ZIP container concept, but the manifest declares multiple tracks and their inter-relationships:

```json
// manifest.json
{
  "format_version": 1,
  "type": "track_group",
  "name": "80s Drum Kit",
  "description": "8-track drum kit with bus compression",
  "tracks": [
    {
      "id": "kick",
      "file": "tracks/kick.json",
      "mixer_channel": { "label": "Drum Bus" }
    },
    {
      "id": "snare",
      "file": "tracks/snare.json",
      "mixer_channel": { "label": "Drum Bus" }
    },
    {
      "id": "hihat",
      "file": "tracks/hihat.json",
      "mixer_channel": { "label": "Drum Bus" }
    }
  ],
  "mixer_channels": [
    {
      "label": "Drum Bus",
      "effects": [
        {
          "plugin": "Compressor",
          "state": "mixer/drum_bus_compressor.bin",
          "params": { "threshold": -12.0, "ratio": 4.0, "attack": 10.0 }
        }
      ],
      "route_to": { "label": "Master" }
    }
  ],
  "automation": [
    {
      "target": "tracks/kick/volume",
      "file": "automation/kick_volume.json"
    },
    {
      "target": "mixer/Drum Bus/effects/0/threshold",
      "file": "automation/bus_comp_threshold.json"
    }
  ],
  "samples": {
    "kick.wav": { "checksum": "sha256:abc123..." },
    "snare.wav": { "checksum": "sha256:def456..." }
  }
}
```

**Cross-track automation** uses the same symbolic path scheme, but now paths are relative to the group:
- `tracks/kick/volume` → the kick track's volume
- `mixer/Drum Bus/effects/0/threshold` → the drum bus compressor's threshold

On import, the entire group is inserted atomically — all tracks, mixer channels, automation, and samples together.

### The Import/Export Flow

#### Export (Project → Template)

1. User selects track(s) in the song editor
2. System walks the dependency graph:
   - Collect the track's JSON (instrument, params, clips)
   - Collect effect chain (each effect's JSON + binary state)
   - Collect all automation clips that target any parameter on this track
   - Collect all referenced samples
   - Record mixer channel assignment (by name, not index)
3. Write the manifest
4. Pack into ZIP → `.lmms-track` or `.lmms-group`

#### Import (Template → Project)

1. User drags a `.lmms-track` file into the song editor (or uses File → Import Track)
2. System reads the manifest
3. Extract samples to project's `samples/` directory (dedup by checksum)
4. Create new track with instrument and effects
5. Remap automation targets using symbolic paths → resolve to new journal IDs
6. Handle mixer routing (find or create named channel)
7. Insert clips at the user's chosen position

### Why This Works With the ZIP Container (Not Against It)

The concern was that a ZIP container inhibits portability. Actually the opposite is true:

| Format | Extract a single track? | Bundle samples? | Relocatable automation? |
|---|---|---|---|
| **Monolithic XML** (current `.mmp`) | Must parse entire DOM, extract subtree, manually collect automation from separate tracks, manually collect sample paths | Base64-encoded inline — must decode | No — uses session-specific integer IDs |
| **Monolithic JSON** | Same problems as XML | Same problems as XML | Same problems |
| **Directory + ZIP** (proposed) | Each track is already a separate file — just copy it | Samples are already separate files — just include them | Symbolic paths are self-contained and relocatable |

The directory structure makes tracks *independently addressable*. The ZIP is just packaging. A `.lmms-track` is structurally identical to a subset of a `.lmms` project — same file layout, same JSON schema, just fewer files.

### Comparison With Other DAWs' Template Systems

| DAW | Track Template Support | How It Works |
|---|---|---|
| **REAPER** | Track templates (`.RTrackTemplate`) | A snippet of the `.rpp` text format. Because `.rpp` is text and tracks are self-contained sections, a template is literally a copy-paste of the text block. Samples referenced by path (not bundled). |
| **Ableton Live** | Drag tracks to browser | Saves as `.adv` (device preset) or `.alc` (clip with device). Samples are collected into a "Collected" folder. Limited — can't export automation easily. |
| **FL Studio** | Channel presets | Saves instrument + effects as a preset. No clip/pattern data. Very limited. |
| **Logic Pro** | Track stacks + patches | ".patch" files bundle channel strip settings (instrument + effects). Can include MIDI regions. |
| **Bitwig** | Preset system | Can save entire track presets including device chains. Modulation (automation) is preserved because it's stored on the device, not on a separate track. |

LMMS's proposed system would be **more capable than any of these** because:
1. It bundles samples (unlike REAPER)
2. It preserves automation (unlike FL Studio, Ableton)
3. It supports multi-track groups with mixer routing (unlike any of them)
4. The format is an open, inspectable ZIP (unlike all of them)

---

## Part 6: Key Rust Crates for Implementation

| Role | Crate |
|---|---|
| JSON serialization | `serde` + `serde_json` |
| XML reading (legacy .mmp) | `quick-xml` (SAX-style, fast, no DOM) |
| ZIP container | `zip` crate |
| SQLite autosave | `rusqlite` |
| Compression | `flate2` (for .mmpz decompression) |
| Binary plugin state | `serde` with `bincode` or raw `&[u8]` |
| File watching (live reload) | `notify` crate |
| Checksumming (sample dedup) | `sha2` crate |
| Path handling | `std::path` + `pathdiff` crate |

---

## Part 7: Migration Path

### From C++ to Rust

The current `saveSettings`/`loadSettings` pattern is actually a useful guide:

1. Define a Rust `Serializable` trait with `save(&self, writer: &mut ProjectWriter)` and `load(reader: &ProjectReader) -> Self`
2. `ProjectWriter` abstracts over the output format (JSON files in the directory)
3. `ProjectReader` abstracts over the input (JSON for new projects, XML for legacy `.mmp`)
4. Each model struct implements the trait — same pattern, cleaner execution
5. The XML-specific DOM coupling is replaced with format-agnostic serialization

```rust
trait Serializable {
    fn save(&self, writer: &mut ProjectWriter) -> Result<()>;
    fn load(reader: &ProjectReader) -> Result<Self> where Self: Sized;
}

// Format-agnostic writer
struct ProjectWriter {
    project_dir: PathBuf,
}

impl ProjectWriter {
    fn write_track(&mut self, id: TrackId, data: &serde_json::Value) -> Result<()>;
    fn write_sample(&mut self, name: &str, data: &[u8]) -> Result<()>;
    fn write_plugin_state(&mut self, id: &str, data: &[u8]) -> Result<()>;
    fn write_automation(&mut self, target_path: &str, data: &AutomationData) -> Result<()>;
}

// Track bundle exporter
struct TrackBundleExporter {
    zip_writer: ZipWriter<File>,
}

impl TrackBundleExporter {
    fn export_track(&mut self, track: &Track, project: &Project) -> Result<()> {
        // 1. Serialize track definition
        // 2. Collect effect chain + plugin states
        // 3. Find all automation clips targeting this track's parameters
        // 4. Collect referenced samples
        // 5. Build manifest
        // 6. Write all to ZIP
    }
}
```

### Legacy .mmp Compatibility

A read-only `.mmp`/`.mmpz` parser:
- Parse XML using `quick-xml` (SAX-style, no DOM)
- Walk the tree once, emitting JSON files + extracting base64 blobs to raw files
- Convert journal ID-based automation references to symbolic paths during import
- Apply the equivalent of the 30+ upgrade methods during import
- **One-way migration:** Old format in, new format out. Never write XML again.
