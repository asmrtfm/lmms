# LMMS Rust Rewrite Plan

## Decision: Rewrite in Rust

After evaluating the LMMS codebase (~912 C/C++ source files, 58 plugins, real-time audio engine, Qt GUI), **Rust** was chosen as the target language.

## Why Rust

| Concern | C++ (current) | Rust |
|---|---|---|
| Memory safety | Manual, error-prone | Compile-time guarantees, no GC |
| Real-time audio | Good (no GC) | Equally good (no GC, zero-cost abstractions) |
| Concurrency | Data races possible | Fearless concurrency via ownership model |
| C/C++ interop | N/A | Excellent FFI — enables incremental migration |
| Package ecosystem | CMake + vcpkg | Cargo (unified build, test, dependency management) |
| Cross-platform | CMake configs | First-class via Cargo targets |

### Key advantages for this project

- **No garbage collector** — critical for glitch-free real-time audio processing
- **Ownership model** prevents use-after-free and data races in the audio graph
- **Incremental migration** — Rust can call C/C++ and vice versa, so the rewrite can happen module-by-module
- **Cargo** replaces the 851-line CMakeLists.txt with a simpler, more maintainable build system
- **Strong audio ecosystem** — crates like `cpal`, `rodio`, `dasp`, and `nih-plug` cover audio I/O, DSP, and plugin formats

## Rust Crate Mapping

| Current Dependency | Rust Equivalent |
|---|---|
| Qt (GUI) | `iced`, `egui`, or `slint` |
| PortAudio / ALSA / JACK | `cpal` (cross-platform audio I/O) |
| fftw3 | `rustfft` |
| libsndfile | `hound` (WAV), `symphonia` (multi-format) |
| libsamplerate | `rubato` |
| fluidsynth | `fluidlite-rs` or FFI binding |
| libvorbis / libogg | `lewton` (Vorbis decoder), `ogg` |
| mp3lame | `mp3lame-encoder` or FFI |
| LV2 / LADSPA / VST | `nih-plug` (plugin framework), `lv2` crate |
| SDL2 | `sdl2` crate (if needed) |
| zlib | `flate2` |

## Proposed Module Structure

```
lmms-rs/
├── Cargo.toml                  (workspace root)
├── crates/
│   ├── lmms-core/              (audio engine, project model, transport)
│   ├── lmms-dsp/               (DSP primitives, filters, oscillators)
│   ├── lmms-midi/              (MIDI I/O and processing)
│   ├── lmms-audio-io/          (audio backend abstraction via cpal)
│   ├── lmms-plugin-host/       (LV2/VST/LADSPA plugin hosting)
│   ├── lmms-gui/               (UI layer)
│   ├── lmms-project/           (project file read/write, .mmp compat)
│   └── lmms-app/               (application entry point)
├── plugins/
│   ├── triple-oscillator/
│   ├── bitcrush/
│   ├── compressor/
│   ├── delay/
│   ├── eq/
│   └── ...                     (58 plugins as separate crates)
└── tests/
```

## Migration Strategy

### Phase 1 — Foundation
- Set up Cargo workspace
- Implement `lmms-core` (sample buffers, audio graph, transport)
- Implement `lmms-dsp` (oscillators, filters, envelopes)
- Implement `lmms-audio-io` with `cpal`

### Phase 2 — Plugin System
- Define Rust plugin trait
- Port built-in plugins (start with TripleOscillator, Bitcrush)
- Add LV2/VST hosting via FFI

### Phase 3 — Project Format (see detailed analysis below)
- Implement legacy .mmp XML reader for backward compatibility
- Implement new hybrid project format (directory + ZIP container)
- Implement SQLite-backed autosave journal

### Phase 4 — GUI
- Build UI with chosen framework (iced or egui)
- Implement piano roll, pattern editor, mixer, song editor
- MIDI controller mapping

### Phase 5 — Parity & Polish
- Port remaining plugins
- Full cross-platform testing (Linux, Windows, macOS)
- Performance benchmarking against C++ version

---

## The XML Problem: Deep Dive

The `.mmp` format is the single biggest architectural hurdle in the rewrite. It's not just "XML vs something else" — it's a cascade of compounding problems.

### How .mmp Works Today

- **Format:** Plain XML (`.mmp`) or gzip-compressed XML (`.mmpz`)
- **Parser:** Qt's `QDomDocument` — a DOM parser that loads the **entire** tree into memory
- **Binary data:** Base64-encoded inline in XML attributes (wavetables, plugin state, sample shapes)
- **Versioning:** 30+ hand-written upgrade methods in `DataFile.cpp` that walk and mutate the DOM tree
- **Serialization pattern:** Every model class implements `saveSettings(QDomDocument&, QDomElement&)` and `loadSettings(const QDomElement&)` — XML is wired into every object in the codebase
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

### What Other DAWs Do

| DAW | Format | Notes |
|---|---|---|
| **Ableton Live** | Gzipped XML (`.als`) | Same fundamental problems as LMMS. VST state stored as hex blobs. Files reach 100k+ lines. |
| **FL Studio** | Custom binary (`.flp`) | Compact and fast, but completely opaque. Proprietary, no tooling. |
| **REAPER** | Custom text (`.rpp`) | Human-readable, git-friendly, tolerant of missing data (fills defaults). Proven durable over ~20 years. |
| **Ardour** | XML + session directory | XML metadata, audio as separate files in subdirectories. |
| **Audacity 3.0** | SQLite (`.aup3`) | Migrated FROM XML+directory TO SQLite. Audio chunks stored as BLOBs. Incremental autosave via WAL. |
| **DAWproject** | ZIP container (XML + media) | Open exchange format by Bitwig/PreSonus. ZIP with `project.xml` + separate audio/plugin entries. |

### Format Candidates Evaluated

| Format | Structured Data | Binary Blobs | Partial Load | Autosave | Human Readable | Git-Friendly |
|---|---|---|---|---|---|---|
| XML (current) | Good | Poor (base64) | No | No (full rewrite) | Yes | Fair |
| JSON | Good | Poor (base64) | No | No (full rewrite) | Yes | Good |
| MessagePack/CBOR | Good | Good (native bytes) | No | No (full rewrite) | No | No |
| Protocol Buffers | Excellent | Good | No | No (full rewrite) | No | No |
| FlatBuffers | Good | Fair | Yes (zero-copy) | No (write-once design) | No | No |
| SQLite | Good | Good (BLOBs) | Yes (SQL queries) | Excellent (WAL mode) | No | No |
| ZIP container | Depends on inner format | Excellent (separate entries) | Yes (per-entry) | No (full rewrite) | Inspectable (unzip) | Fair |
| Custom binary | Full control | Full control | Possible | Possible | No | No |

**No single format wins on every axis.** The answer is a hybrid.

### Recommended Architecture: Hybrid Format

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
- **Git-friendly:** Each track is a separate JSON file. Diffs are meaningful. Two people can edit different tracks and merge without conflicts.
- **Human-inspectable:** `cat project.json` or open in any text editor.
- **Crash-resilient:** Losing one track file doesn't destroy the project.

#### 2. Save/Share Format — ZIP Container (`.lmms`)

When the user hits "Save As" or "Export", pack the directory into a standard ZIP:

```
MyProject.lmms
  └── (ZIP contents = the directory structure above, minus .autosave/)
```

- Users share a single `.lmms` file (small, compressed)
- Anyone can rename to `.zip` and inspect contents
- Follows the proven DAWproject / DOCX / ODT pattern
- Compression handles the "I want to share without a heavy file" use case that motivated XML originally

#### 3. Autosave — SQLite Journal

The `.autosave/journal.sqlite` database tracks incremental changes:

```sql
-- Only changed data is written on each autosave tick
CREATE TABLE changes (
    id INTEGER PRIMARY KEY,
    timestamp INTEGER NOT NULL,
    file_path TEXT NOT NULL,      -- e.g., "tracks/track_001.json"
    content BLOB NOT NULL,        -- the changed file's contents
    checksum TEXT NOT NULL
);

CREATE TABLE snapshots (
    id INTEGER PRIMARY KEY,
    timestamp INTEGER NOT NULL,
    manifest TEXT NOT NULL         -- JSON list of file_path + checksum pairs
);
```

- **Incremental:** Only modified files are written per autosave tick (not the whole project)
- **WAL mode:** SQLite's write-ahead log means autosave never blocks the audio thread
- **Crash recovery:** On startup, check for `.autosave/journal.sqlite` and offer recovery
- **Undo history:** The journal naturally provides a timeline of changes

#### 4. Legacy Compatibility — .mmp Reader

A read-only `.mmp` / `.mmpz` parser that converts old projects to the new format:

- Parse XML using a lightweight Rust XML crate (`quick-xml` — SAX-style, no DOM)
- Walk the tree once, emitting JSON files + extracting base64 blobs to raw files
- Apply the equivalent of the 30+ upgrade methods during import
- **One-way migration:** Old format in, new format out. Never write XML again.

### Why Not Just SQLite for Everything?

Audacity went all-in on SQLite and it works for them, but Audacity is fundamentally different — it stores audio waveform data (huge, append-only BLOBs). A DAW like LMMS stores structured musical data (notes, automation, routing) where:

- Human readability matters for debugging and community contributions
- Git-friendliness matters for collaborative workflows
- The data is naturally hierarchical (song > tracks > clips > notes), not relational

SQLite is the right tool for the **autosave journal** (where incremental writes and crash resilience matter) but the wrong tool for the **primary format** (where readability and diffability matter).

### Why Not Just JSON?

JSON alone (without the directory + ZIP hybrid) repeats XML's mistakes:

- Single-file JSON still requires full rewrite on save
- Binary data still needs base64 encoding
- No partial loading of a monolithic JSON file
- A 200-track project as a single JSON file is just as unwieldy as XML

JSON is the right choice **per-file within the directory structure**, not as a monolithic replacement.

### Migration Path from the C++ Codebase

The current `saveSettings`/`loadSettings` pattern touching every class is actually helpful:

1. Define a Rust `Serializable` trait with `save(&self, writer: &mut ProjectWriter)` and `load(reader: &ProjectReader) -> Self`
2. `ProjectWriter` abstracts over the output format (JSON files in the directory)
3. `ProjectReader` abstracts over the input format (JSON for new projects, XML for legacy `.mmp`)
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
}
```

### Key Rust Crates for the New Format

| Role | Crate |
|---|---|
| JSON serialization | `serde` + `serde_json` |
| XML reading (legacy .mmp) | `quick-xml` (SAX-style, fast, no DOM) |
| ZIP container | `zip` crate |
| SQLite autosave | `rusqlite` |
| Compression | `flate2` (for .mmpz decompression) |
| Binary plugin state | `serde` with `bincode` or raw `&[u8]` |
| File watching (live reload) | `notify` crate |

---

## Alternatives Considered

- **Go** — GC pauses disqualify it for real-time audio
- **Zig** — Immature ecosystem, limited GUI/audio libraries
- **C# (.NET)** — GC latency concerns, less control over memory layout
- **Staying in C++** — Viable, but misses opportunity for memory safety and modernized tooling
