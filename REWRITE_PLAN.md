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
| PortAudio / ALSA / JACK | `cpal` (cross-platform audio I/O, including input/recording) |
| fftw3 | `rustfft` |
| libsndfile | `hound` (WAV), `symphonia` (multi-format decode) |
| libsamplerate | `rubato` |
| fluidsynth | `fluidlite-rs` or FFI binding |
| libvorbis / libogg | `lewton` (Vorbis decoder), `ogg` |
| mp3lame | `mp3lame-encoder` or FFI |
| LV2 / LADSPA / VST | `nih-plug` (plugin framework), `lv2` crate, `clack-host` (CLAP) |
| SDL2 | `sdl2` crate (if needed) |
| zlib | `flate2` |
| QDomDocument (XML) | `quick-xml` (SAX parser, legacy .mmp reading only) |
| — (new: project format) | `serde` + `serde_json`, `zip`, `rusqlite` |
| — (new: checksums) | `sha2` (sample dedup in templates) |
| — (new: file watching) | `notify` (live directory watching) |
| — (new: MCP server) | `rmcp` (official Rust MCP SDK) |
| — (new: MIDI devices) | `midir` (MIDI I/O), `midly` (MIDI file parsing) |
| — (new: CLAP hosting) | `clack-host` (CLAP plugin hosting) |

## Proposed Module Structure

```
lmms-rs/
├── Cargo.toml                  (workspace root)
├── crates/
│   ├── lmms-core/              (audio engine, project model, transport)
│   ├── lmms-dsp/               (DSP primitives, filters, oscillators)
│   ├── lmms-midi/              (MIDI I/O and processing, MPE support)
│   ├── lmms-audio-io/          (audio backend abstraction via cpal, recording)
│   ├── lmms-plugin-host/       (LV2/VST3/CLAP/LADSPA plugin hosting)
│   ├── lmms-gui/               (UI layer)
│   ├── lmms-project/           (project file read/write, .mmp compat, templates)
│   ├── lmms-mcp/               (native MCP server for AI control)
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

Priority ordering informed by community feedback (GitHub issues, forums, Reddit 2025-2026). The overarching theme: **modernization without breaking the core workflow**.

### Phase 1 — Foundation
- Set up Cargo workspace
- Implement `lmms-core` (sample buffers, audio graph, transport)
- Implement `lmms-dsp` (oscillators, filters, envelopes)
- Implement `lmms-audio-io` with `cpal` (including **audio input/recording** — community #1 missing feature)
- MPE MIDI support from day one (added to C++ LMMS in 2025 — carry forward)

### Phase 2 — Project Format (see FORMAT_RESEARCH.md for deep analysis)
Moved ahead of plugins because it's foundational to everything else.
- Implement new hybrid project format:
  - Working format: exploded directory (per-track JSON + raw samples)
  - Save/share format: ZIP container (`.lmms`)
  - Autosave: SQLite WAL journal with snapshot versioning
- Implement **track bundle format** (`.lmms-track`, `.lmms-group`) for template portability
- Symbolic parameter paths for automation (replaces session-specific journal IDs)
- Per-pattern automation support (community request #775)
- Legacy .mmp/.mmpz reader for one-way migration
- Audio recording integration: stream-to-disk, multi-take management, non-destructive editing

### Phase 3 — Plugin System
- Define Rust plugin trait
- Port built-in plugins (start with TripleOscillator, Bitcrush)
- **LV2 hosting** (community priority — in progress in C++ LMMS 1.3)
- **CLAP hosting** (modern plugin format, gaining momentum)
- **VST3 hosting** (via FFI, stable on all platforms)
- LADSPA hosting (legacy support)
- Plugin browser with search, tags, and previews (community UX request)

### Phase 4 — GUI
- Build UI with chosen framework (iced or egui)
- Implement piano roll with ghost notes, note labels, better zoom/slicing (community UX requests)
- Pattern editor, mixer, song editor
- **Multi-monitor / detachable windows** (community request)
- MIDI controller mapping
- Audio recording UI: waveform display, punch-in/out controls, take management
- Track template drag-and-drop (import/export `.lmms-track` from song editor)
- Project versioning UI: named snapshots, diff viewer, revert

### Phase 5 — Parity & Polish
- Port remaining plugins (58 total)
- Full cross-platform testing (Linux, Windows, macOS)
- Performance benchmarking against C++ version
- Drop 32-bit support entirely (community request #7286)
- Batch export / render settings persistence
- JACK I/O improvements for Linux

### Community Alignment

| Community Priority | Phase | How Addressed |
|---|---|---|
| Track templates / reusability | Phase 2 | `.lmms-track` / `.lmms-group` ZIP bundles with symbolic refs |
| Audio recording | Phase 1+2+4 | `cpal` input, stream-to-disk, multi-take, recording UI |
| LV2/CLAP/VST3 plugins | Phase 3 | Native hosting for all modern formats |
| Per-pattern automation | Phase 2 | Embedded in clip JSON via symbolic parameter paths |
| Project versioning | Phase 2+4 | SQLite journal snapshots + UI for labeling/reverting |
| Piano roll improvements | Phase 4 | Ghost notes, labels, zoom — UI-layer work |
| Multi-monitor support | Phase 4 | Detachable windows in GUI framework |
| Non-destructive sample editing | Phase 2 | Trim/fade/pitch as clip metadata, source file untouched |
| Git-friendly collaboration | Phase 2 | Directory of JSON files, per-track isolation |
| 64-bit only | Phase 5 | Clean break — no 32-bit considerations |

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

## Native MCP Server

LMMS Studio will ship with a built-in MCP (Model Context Protocol) server, making it the first DAW that an AI assistant can directly control through a standardized protocol.

### Why This Matters

An MCP server turns LMMS Studio into an AI-controllable instrument. Claude (or any MCP-compatible AI) can:
- Create tracks, load instruments, set parameters
- Compose melodies and chord progressions by placing notes
- Build effect chains and configure automation
- Arrange full songs from natural language descriptions
- Load presets, adjust mix levels, configure routing
- Query the current project state to make informed decisions

This is a massive differentiator — no other DAW offers this capability natively.

### Architecture

```
┌──────────────────────────────────────────────┐
│  LMMS Studio (Rust)                          │
│                                              │
│  ┌──────────┐    ┌─────────────────────┐     │
│  │ Audio    │    │ MCP Server (rmcp)   │     │
│  │ Engine   │    │                     │     │
│  │          │◄───┤ Tools:              │◄────┤── stdio/HTTP ──► Claude / AI Client
│  │ Project  │    │  create_track       │     │
│  │ State    │    │  load_instrument    │     │
│  │          │    │  add_notes          │     │
│  │ Mixer    │    │  set_parameter      │     │
│  │          │    │  add_effect         │     │
│  └──────────┘    │  set_automation     │     │
│                  │  control_transport  │     │
│                  │  export_audio       │     │
│                  │                     │     │
│                  │ Resources:          │     │
│                  │  project://state    │     │
│                  │  library://presets  │     │
│                  │  library://samples  │     │
│                  └─────────────────────┘     │
└──────────────────────────────────────────────┘
```

### Transport

- **STDIO** for local integration (AI running on same machine)
- **Streamable HTTP + SSE** for remote / network access
- Runs in-process — the MCP server is a Rust module inside the DAW, not a separate process

### Rust Implementation

Using the official `rmcp` crate (v0.16+):

```toml
# In lmms-mcp/Cargo.toml
[dependencies]
rmcp = { version = "0.16", features = ["server"] }
tokio = { version = "1", features = ["full"] }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
```

### Tool Catalog

#### Track Management
| Tool | Description | Key Parameters |
|---|---|---|
| `create_track` | Create instrument, sample, or automation track | `name`, `type`, `position` |
| `delete_track` | Remove a track | `track_id` |
| `duplicate_track` | Clone a track with all settings | `track_id` |
| `rename_track` | Rename a track | `track_id`, `name` |
| `set_track_param` | Set volume, panning, mute, solo | `track_id`, `param`, `value` |
| `import_track_template` | Import a `.lmms-track` bundle | `file_path`, `position` |
| `export_track_template` | Export track as `.lmms-track` | `track_id`, `file_path` |

#### Instrument & Presets
| Tool | Description | Key Parameters |
|---|---|---|
| `load_instrument` | Load a plugin on a track | `track_id`, `plugin_name` |
| `load_preset` | Load a preset file (.xpf, .xiz) | `track_id`, `preset_path` |
| `set_instrument_param` | Set instrument-specific parameter | `track_id`, `param_path`, `value` |
| `list_presets` | List available presets for an instrument | `instrument_name`, `category` |

#### Note Entry
| Tool | Description | Key Parameters |
|---|---|---|
| `add_note` | Add a MIDI note | `track_id`, `clip_id`, `pitch`, `velocity`, `position`, `length` |
| `add_notes_batch` | Add multiple notes at once | `track_id`, `clip_id`, `notes[]` |
| `remove_notes` | Remove notes in a range | `track_id`, `clip_id`, `start`, `end` |
| `create_clip` | Create a new clip on a track | `track_id`, `position`, `length` |

#### Effects
| Tool | Description | Key Parameters |
|---|---|---|
| `add_effect` | Add effect to a track's chain | `track_id`, `effect_name`, `position` |
| `remove_effect` | Remove effect from chain | `track_id`, `effect_index` |
| `set_effect_param` | Set effect parameter | `track_id`, `effect_index`, `param`, `value` |
| `set_effect_wet_dry` | Set wet/dry mix | `track_id`, `effect_index`, `value` |

#### Automation
| Tool | Description | Key Parameters |
|---|---|---|
| `add_automation` | Create automation for a parameter | `target_path`, `keyframes[]` |
| `add_clip_automation` | Add per-clip automation | `track_id`, `clip_id`, `target_path`, `keyframes[]` |

#### Mixer
| Tool | Description | Key Parameters |
|---|---|---|
| `create_mixer_channel` | Create a mixer channel | `name` |
| `route_track` | Route track to mixer channel | `track_id`, `channel_name` |
| `set_mixer_param` | Set channel volume/pan | `channel_name`, `param`, `value` |
| `add_mixer_effect` | Add effect to mixer channel | `channel_name`, `effect_name` |

#### Transport & Export
| Tool | Description | Key Parameters |
|---|---|---|
| `set_bpm` | Set project tempo | `bpm` |
| `set_time_signature` | Set time signature | `numerator`, `denominator` |
| `play` | Start playback | `from_position` (optional) |
| `stop` | Stop playback | — |
| `export_audio` | Render to audio file | `file_path`, `format`, `sample_rate` |

### Resources (Read-Only Context)

| Resource URI | Description |
|---|---|
| `project://state` | Full project state: tracks, instruments, clips, BPM, time signature |
| `project://track/{id}` | Single track's complete state |
| `library://presets/{instrument}` | Available presets for an instrument |
| `library://samples` | Available sample files |
| `library://plugins` | Installed plugins (internal + external) |
| `library://templates` | Available track/group templates |

### Prompts (User-Invoked Templates)

| Prompt | Description |
|---|---|
| `compose_beat` | "Create a drum pattern with kick, snare, hihat" |
| `compose_chord_progression` | "Write a chord progression in the key of..." |
| `create_arrangement` | "Arrange an intro, verse, chorus structure" |
| `mix_project` | "Set levels and panning for a balanced mix" |
| `sound_design` | "Create a sound matching this description..." |

### Integration With Template System

The MCP server uses the same symbolic parameter paths as the template format:

```json
// AI calls set_instrument_param
{
  "track_id": "track_001",
  "param_path": "sound_shaping/filter_cutoff",
  "value": 2000.0
}

// AI calls add_clip_automation
{
  "track_id": "track_001",
  "clip_id": "clip_0",
  "target_path": "track/sound_shaping/filter_cutoff",
  "keyframes": [
    { "pos": 0, "value": 5000.0 },
    { "pos": 384, "value": 800.0 }
  ]
}
```

Same path scheme everywhere: MCP tools, automation, templates. One concept, three use cases.

---

## Factory Content: Instrument Templates & Presets

### Current Inventory

| Category | Count | Format |
|---|---|---|
| TripleOscillator presets | 72 | `.xpf` (XML) |
| ZynAddSubFX presets | 954 | `.xiz` (gzipped XML) |
| Other instruments (BitInvader, Organic, Kicker, etc.) | 103 | `.xpf` (XML) |
| Audio samples | 1,005 | `.wav`, `.ogg`, `.flac` |
| Wavetables | 4 | binary |
| Project templates | 6 | `.mpt` (XML) |
| Demo projects | 28 | `.mmp` (XML) |
| **Total** | **2,172 files** | |

### Preset Schema (Current XML → New JSON)

Current `.xpf` preset (e.g., TB303):
```xml
<instrumenttracksettings muted="0" type="0" name="TB303">
  <instrumenttrack pan="0" mixch="0" pitch="0" basenote="81" vol="59">
    <instrument name="tripleoscillator">
      <tripleoscillator wavetype0="2" vol0="100" coarse0="0" finer0="0"
                        wavetype1="2" vol1="0"   coarse1="0" finer1="0"
                        wavetype2="2" vol2="0"   coarse2="0" finer2="0"
                        modalgo1="2" modalgo2="2" modalgo3="0" ... />
    </instrument>
    <eldata fres="0.76" ftype="6" fcut="1" fwet="1">
      <elvol att="0.038" dec="0.279" sus="0" rel="0.112" amt="1" ... />
      <elcut att="0.062" dec="0.426" sus="0.999" rel="0" amt="1" ... />
    </eldata>
    <fxchain numofeffects="0" enabled="0"/>
  </instrumenttrack>
</instrumenttracksettings>
```

New JSON preset equivalent:
```json
{
  "name": "TB303",
  "instrument": {
    "plugin": "triple_oscillator",
    "params": {
      "osc1": { "wavetype": "saw", "volume": 100, "coarse": 0, "fine": 0, "pan": 0 },
      "osc2": { "wavetype": "saw", "volume": 0, "coarse": 0, "fine": 0, "pan": 0 },
      "osc3": { "wavetype": "saw", "volume": 0, "coarse": 0, "fine": 0, "pan": 0 },
      "modulation": { "osc2_mode": "pm", "osc3_mode": "pm" }
    }
  },
  "sound_shaping": {
    "filter": { "type": "moog_double_lowpass", "cutoff": 1, "resonance": 0.76, "wet": 1.0 },
    "envelopes": {
      "volume": { "attack": 0.038, "decay": 0.279, "sustain": 0, "release": 0.112, "amount": 1.0 },
      "cutoff": { "attack": 0.062, "decay": 0.426, "sustain": 0.999, "release": 0, "amount": 1.0 }
    }
  },
  "track": { "volume": 59, "panning": 0, "pitch": 0, "base_note": 81 }
}
```

### Conversion Strategy

All 1,138 presets will be converted during the build process:

1. **Write a Rust conversion tool** (`lmms-preset-converter`) that reads `.xpf`/`.xiz` XML and emits JSON presets
2. **Run at build time** — source presets stay as XML in the repo for history; built JSON presets ship with the binary
3. **Preserve the directory structure** — `data/presets/TripleOscillator/TB303.xpf` → `presets/triple_oscillator/TB303.json`
4. **Expose via MCP** — the `library://presets/{instrument}` resource lists all available presets; `load_preset` tool applies them

### Presets as MCP-Loadable Resources

When Claude calls `list_presets`:
```json
// Request
{ "instrument_name": "triple_oscillator" }

// Response
{
  "presets": [
    { "name": "TB303", "path": "presets/triple_oscillator/TB303.json", "tags": ["bass", "acid"] },
    { "name": "SuperSawLead", "path": "presets/triple_oscillator/SuperSawLead.json", "tags": ["lead", "supersaw"] },
    ...
  ]
}
```

When Claude calls `load_preset`:
```json
{ "track_id": "track_001", "preset_path": "presets/triple_oscillator/TB303.json" }
```

The preset is loaded with all parameters, effect chains, envelope settings — everything. This is the same `.lmms-track` template system, just for presets that come bundled with the DAW.

---

## Alternatives Considered

- **Go** — GC pauses disqualify it for real-time audio
- **Zig** — Immature ecosystem, limited GUI/audio libraries
- **C# (.NET)** — GC latency concerns, less control over memory layout
- **Staying in C++** — Viable, but misses opportunity for memory safety and modernized tooling
