# LMMS Studio: Work Packages

This document breaks the Rust rewrite into concrete, shippable work packages. Each package is independently testable, has clear inputs/outputs, and builds on the previous ones. Packages within a phase can often be parallelized.

---

## Guiding Principles

1. **Each package produces a working artifact** — a binary that does something demonstrable, a library with passing tests, or a tool that converts data
2. **Audio-first** — get sound coming out as early as possible; everything else is wiring
3. **Format-second** — the project format is the foundation for templates, recording, automation, and MCP; it must be solid before building UI
4. **Test with real data** — use the 1,138 existing presets and 28 demo projects as test fixtures from day one
5. **Ship incrementally** — each package could be a PR, a milestone, or a sprint's worth of work

---

## Phase 1: Foundation (Audio Engine + Core)

### WP-1.1: Cargo Workspace Scaffold
**Effort:** Small (1-2 days)
**Output:** Empty workspace with all crate stubs, CI pipeline, README
- Create `lmms-rs/` workspace with `Cargo.toml`
- Stub out all crates: `lmms-core`, `lmms-dsp`, `lmms-midi`, `lmms-audio-io`, `lmms-plugin-host`, `lmms-gui`, `lmms-project`, `lmms-mcp`, `lmms-app`
- Set up CI (GitHub Actions): `cargo build`, `cargo test`, `cargo clippy`, `cargo fmt`
- Configure cross-compilation targets: Linux x86_64, Windows x86_64, macOS aarch64
- Add `rustfmt.toml`, `clippy.toml`, `.editorconfig`
- **Test:** `cargo build --workspace` succeeds

### WP-1.2: Sample Buffer & Audio Primitives
**Effort:** Small (2-3 days)
**Output:** `lmms-core` crate with `SampleFrame`, `SampleBuffer`, `AudioBuffer` types
- Define `SampleFrame` (stereo f32 pair)
- `SampleBuffer` — owned buffer with length, sample rate
- `AudioBuffer` — borrowed slice view for real-time processing (no allocation)
- Basic DSP: gain, pan, mix, fade
- **Test:** Unit tests for all buffer operations; property-based tests with `proptest`
- **Depends on:** WP-1.1

### WP-1.3: Audio I/O Backend
**Effort:** Medium (3-5 days)
**Output:** `lmms-audio-io` crate — plays audio through system speakers, records from microphone
- `cpal` integration for output (playback)
- `cpal` integration for input (recording)
- Backend abstraction trait: `AudioBackend { fn open_output(); fn open_input(); }`
- Stream-to-disk recording: write incoming audio directly to WAV file
- Sample rate conversion bridge (if device rate ≠ engine rate) via `rubato`
- **Test:** Integration test that plays a sine wave for 1 second; integration test that records 1 second of silence
- **Depends on:** WP-1.2

### WP-1.4: DSP Primitives
**Effort:** Medium (5-7 days)
**Output:** `lmms-dsp` crate — oscillators, filters, envelopes
- Oscillators: sine, saw, square, triangle, noise, wavetable
- Filters: lowpass, highpass, bandpass, notch, moog (matching LMMS's 8 filter types)
- ADSR envelope generator
- LFO (sine, triangle, saw, square, custom shape, random)
- **Test:** Unit tests comparing output against known reference signals; snapshot tests against C++ LMMS output for the same parameters
- **Depends on:** WP-1.2

### WP-1.5: MIDI I/O
**Effort:** Small (2-3 days)
**Output:** `lmms-midi` crate — MIDI message types, MPE support, device enumeration
- MIDI message types: NoteOn, NoteOff, CC, PitchBend, Aftertouch, SysEx
- MPE zone management (lower/upper zone, per-note channels)
- Device enumeration and connection via `midir` crate
- MIDI file reading/writing via `midly` crate
- **Test:** Parse and re-emit a standard MIDI file; round-trip test
- **Depends on:** WP-1.1

### WP-1.6: Transport & Audio Graph
**Effort:** Medium (5-7 days)
**Output:** Transport (play/stop/seek/loop) + audio graph that renders tracks to output
- `Transport` struct: play state, position (in ticks and frames), BPM, time signature, loop points
- `AudioGraph`: ordered list of tracks, each producing audio per block
- `Track` trait: `fn process(&mut self, transport: &Transport, buffer: &mut AudioBuffer)`
- Master output mixing with volume/panning
- Real-time thread: audio callback reads from graph, writes to backend
- **Test:** Render a simple arrangement (sine wave track at specific BPM) to WAV; verify timing
- **Depends on:** WP-1.2, WP-1.3, WP-1.4

### WP-1.7: First Sound — TripleOscillator
**Effort:** Medium (5-7 days)
**Output:** Working TripleOscillator plugin that produces sound through the audio graph
- Implement TripleOscillator as an `Instrument` trait implementor
- 3 oscillators with: wavetype, volume, panning, coarse/fine tuning, phase offset, stereo detuning
- Modulation modes: mix, AM, FM, PM (matching C++ LMMS's `modalgo` values)
- Sound shaping: filter + 3 envelope/LFO pairs (volume, cutoff, resonance)
- Wire up: MIDI note → TripleOscillator → audio graph → speakers
- **Test:** Load `TB303.xpf` parameters manually, render 1 bar, compare spectral characteristics against C++ LMMS render
- **Depends on:** WP-1.4, WP-1.5, WP-1.6

**Phase 1 Milestone:** Play a MIDI note through TripleOscillator → hear sound from speakers. This is the "hello world" moment.

---

## Phase 2: Project Format + Templates

### WP-2.1: Project Directory Structure
**Effort:** Small (2-3 days)
**Output:** `lmms-project` crate — create, open, save project directories
- Define the directory layout: `project.json`, `tracks/`, `automation/`, `samples/`, `plugin-state/`, `recordings/`
- `Project` struct: create new, open existing, save
- `.format-version` file for schema migration
- `project.json` schema: BPM, time signature, master volume, track list (references to track files)
- **Test:** Create project → save → reopen → verify all data round-trips
- **Depends on:** WP-1.1

### WP-2.2: Track Serialization
**Effort:** Medium (3-5 days)
**Output:** Serialize/deserialize InstrumentTrack to/from `tracks/track_NNN.json`
- Define JSON schema for track: instrument params, effect chain, clips, mixer routing
- `serde` derive macros on all model structs
- Symbolic parameter path registry: `track/volume`, `track/sound_shaping/filter_cutoff`, etc.
- Handle effect chains (ordered list of effects with params + binary state)
- **Test:** Serialize TripleOscillator track → deserialize → verify parameters match
- **Depends on:** WP-1.7, WP-2.1

### WP-2.3: Clip & Note Serialization
**Effort:** Small (2-3 days)
**Output:** Serialize/deserialize MIDI clips (notes) and sample clips
- MidiClip JSON: notes array with `key`, `velocity`, `position`, `length`, `panning`, `detuning`
- SampleClip JSON: source file path, start/end frames, fade in/out, pitch shift, time stretch
- Per-clip automation (embedded automation using symbolic paths)
- **Test:** Create clip with 100 notes → serialize → deserialize → verify all notes match
- **Depends on:** WP-2.2

### WP-2.4: Automation System
**Effort:** Medium (3-5 days)
**Output:** Automation with symbolic parameter paths (both global and per-clip)
- Symbolic path resolution: `"track/effects/0/wet_dry"` → actual parameter reference
- AutomationClip: keyframes (time/value pairs), progression type (discrete, linear, cubic hermite)
- Global automation tracks in `automation/global.json`
- Per-clip automation embedded in clip JSON
- **Test:** Create automation curve → serialize → deserialize → verify values at specific time points
- **Depends on:** WP-2.2, WP-2.3

### WP-2.5: ZIP Container (Save/Share)
**Effort:** Small (2-3 days)
**Output:** Pack/unpack project directory ↔ `.lmms` ZIP file
- `zip` crate integration
- Pack: directory → ZIP (exclude `.autosave/`)
- Unpack: ZIP → directory
- Maintain file checksums for integrity verification
- **Test:** Create project → pack to ZIP → unpack to new directory → verify byte-for-byte identical
- **Depends on:** WP-2.1

### WP-2.6: SQLite Autosave Journal
**Effort:** Medium (3-5 days)
**Output:** Incremental autosave with crash recovery and snapshot versioning
- `rusqlite` integration
- Schema: `changes` table (file_path, content, checksum), `snapshots` table (timestamp, label, manifest)
- WAL mode for non-blocking writes
- Autosave tick: diff working directory against last snapshot, write only changed files
- Crash recovery: on startup, detect journal → offer to restore
- Named snapshots (user-labeled versions)
- **Test:** Modify project → autosave → corrupt working directory → recover from journal → verify recovery
- **Depends on:** WP-2.1

### WP-2.7: Track Bundle Format (.lmms-track / .lmms-group)
**Effort:** Medium (5-7 days)
**Output:** Export/import single tracks or groups as portable bundles
- Export: collect track JSON + effects + automation + samples → ZIP bundle
- Manifest with dependency inventory, checksums, mixer routing references
- Import: unpack bundle → create track → remap automation paths → handle sample dedup → handle mixer routing
- Group bundles: multiple tracks + shared mixer channels + cross-track automation
- **Test:** Export track from project A → import into project B → verify sound is identical
- **Depends on:** WP-2.2, WP-2.3, WP-2.4, WP-2.5

### WP-2.8: Legacy .mmp/.mmpz Reader
**Effort:** Large (7-10 days)
**Output:** Read any existing LMMS project and convert to new format
- `quick-xml` SAX parser for .mmp XML
- `flate2` decompression for .mmpz
- Walk XML tree → emit JSON files + extract base64 binary blobs to raw files
- Convert journal ID-based automation references to symbolic parameter paths
- Implement equivalent of C++ LMMS's 30+ upgrade methods
- Handle all track types: InstrumentTrack, SampleTrack, PatternTrack, AutomationTrack
- **Test:** Convert all 28 demo projects + 6 templates; verify they load correctly in new format
- **Depends on:** WP-2.2, WP-2.3, WP-2.4

### WP-2.9: Preset Converter
**Effort:** Small (2-3 days)
**Output:** Tool that converts all 1,138 .xpf/.xiz presets to JSON
- Read `.xpf` XML → emit JSON preset
- Read `.xiz` (gzip XML) → decompress → emit JSON preset
- Preserve directory structure
- Add tags/categories based on directory names and preset characteristics
- Run as build step
- **Test:** Convert all 1,138 presets; verify each loads and produces the expected parameter values
- **Depends on:** WP-2.2

**Phase 2 Milestone:** Open any existing LMMS project, save in new format, export tracks as templates, import into another project, autosave works, version history works.

---

## Phase 3: Plugin System

### WP-3.1: Plugin Trait & Registry
**Effort:** Small (2-3 days)
**Output:** `lmms-plugin-host` crate with `Plugin` trait and plugin discovery
- `Instrument` trait: `fn process_note()`, `fn save_state()`, `fn load_state()`, `fn parameter_list()`
- `Effect` trait: `fn process_audio()`, `fn save_state()`, `fn load_state()`, `fn parameter_list()`
- Plugin registry: scan directories, enumerate available plugins
- **Depends on:** WP-1.2

### WP-3.2: Port Built-in Instruments
**Effort:** Large (15-25 days, parallelizable per instrument)
**Output:** All built-in instruments ported to Rust
- Priority order (by preset count / community usage):
  1. TripleOscillator (done in WP-1.7)
  2. Kicker (10 presets, simple — good second target)
  3. BitInvader (14 presets, wavetable-based)
  4. Organic (10 presets)
  5. AudioFileProcessor (6 presets, sample playback)
  6. LB302 (6 presets, 303 emulation)
  7. OpulenZ (15 presets, FM/OPL)
  8. Monstro (4 presets, complex)
  9. Xpressive (22 presets)
  10. Others: SID, Nescaline, Vibed, Watsyn, FreeBoy, PatMan, SFxR, SlicerT
- **Test per instrument:** Load each preset → render 1 bar → verify against C++ reference render
- **Depends on:** WP-3.1

### WP-3.3: Port Built-in Effects
**Effort:** Medium (7-10 days, parallelizable)
**Output:** All built-in effects ported to Rust
- Delay, Reverb, Compressor, EQ, DualFilter, Distortion, Stereo tools, etc.
- **Test per effect:** Process known input signal → verify output characteristics
- **Depends on:** WP-3.1

### WP-3.4: LV2 Plugin Hosting
**Effort:** Large (7-10 days)
**Output:** Load and run LV2 plugins
- LV2 discovery (scan standard paths)
- Instantiate plugins, connect ports, run audio
- State save/load via LV2 State extension
- UI hosting (optional, can defer to Phase 4)
- **Depends on:** WP-3.1

### WP-3.5: CLAP Plugin Hosting
**Effort:** Medium (5-7 days)
**Output:** Load and run CLAP plugins via `clack-host`
- CLAP discovery and instantiation
- Audio processing, parameter changes
- State save/load
- **Depends on:** WP-3.1

### WP-3.6: VST3 Plugin Hosting
**Effort:** Medium (5-7 days)
**Output:** Load and run VST3 plugins via FFI
- VST3 SDK FFI bindings
- Discovery, instantiation, audio processing
- State save/load
- **Depends on:** WP-3.1

### WP-3.7: LADSPA Plugin Hosting (Legacy)
**Effort:** Small (2-3 days)
**Output:** Load and run LADSPA plugins
- Simple C FFI — LADSPA is the simplest plugin API
- Discovery, instantiation, audio processing
- **Depends on:** WP-3.1

### WP-3.8: ZynAddSubFX Integration
**Effort:** Medium (5-7 days)
**Output:** ZynAddSubFX as an instrument (via FFI or embedded)
- Option A: FFI to existing C++ ZynAddSubFX library
- Option B: Use LV2 version of ZynAddSubFX (rides on WP-3.4)
- Load .xiz presets (all 954 of them)
- **Test:** Load each .xiz preset → verify it produces sound
- **Depends on:** WP-3.4 or WP-3.1

**Phase 3 Milestone:** All 58 built-in plugins ported + LV2/CLAP/VST3 external plugins load and produce audio.

---

## Phase 4: GUI

### WP-4.1: GUI Framework Setup
**Effort:** Medium (3-5 days)
**Output:** Basic window with empty song editor layout
- Choose framework: `iced`, `egui`, or `slint`
- Application shell: menu bar, toolbar, tabbed/split layout
- Theme/styling system
- **Depends on:** WP-1.1

### WP-4.2: Song Editor (Arrangement View)
**Effort:** Large (10-15 days)
**Output:** Track list with clips on a timeline
- Track list panel: name, mute/solo, volume/pan faders
- Timeline with clips displayed as blocks
- Clip drag, resize, move, duplicate
- Playhead display
- **Depends on:** WP-4.1, WP-1.6

### WP-4.3: Piano Roll
**Effort:** Large (10-15 days)
**Output:** Note editor with ghost notes, labels, zoom
- Note display and editing (click, drag, resize)
- Velocity editor
- Ghost notes from other tracks
- Note labels (pitch names)
- Zoom and scroll (horizontal and vertical)
- Snap-to-grid with configurable quantization
- **Depends on:** WP-4.1

### WP-4.4: Mixer
**Effort:** Medium (7-10 days)
**Output:** Mixer view with channel strips
- Channel strips: fader, pan knob, mute/solo, name
- Effect rack per channel
- Routing visualization
- Master channel
- **Depends on:** WP-4.1

### WP-4.5: Pattern Editor (Beat+Bassline)
**Effort:** Medium (7-10 days)
**Output:** Grid-based pattern editor
- Step sequencer grid
- Multiple instrument rows
- Pattern selector
- **Depends on:** WP-4.1, WP-4.3

### WP-4.6: Instrument Plugin UI
**Effort:** Medium (5-7 days)
**Output:** Instrument editor window
- Knobs, sliders, waveform selectors
- Envelope/LFO visualization and editing
- Filter controls
- Effect chain editor
- **Depends on:** WP-4.1, WP-3.1

### WP-4.7: Automation Editor
**Effort:** Medium (5-7 days)
**Output:** Automation curve editor
- Draw/edit keyframes
- Curve types (discrete, linear, cubic hermite)
- Parameter picker (using symbolic parameter paths)
- **Depends on:** WP-4.1, WP-2.4

### WP-4.8: Audio Recording UI
**Effort:** Medium (5-7 days)
**Output:** Waveform display, record controls, take management
- Record arm button on tracks
- Waveform display for sample clips
- Punch-in/out markers
- Take lane (show/select between takes)
- Monitoring toggle
- **Depends on:** WP-4.2, WP-1.3

### WP-4.9: Template Drag & Drop
**Effort:** Small (3-5 days)
**Output:** Import/export tracks via drag-and-drop
- Drag `.lmms-track` files into song editor
- Right-click track → "Export as Template"
- Template browser panel
- **Depends on:** WP-4.2, WP-2.7

### WP-4.10: Project Versioning UI
**Effort:** Small (3-5 days)
**Output:** Version history panel
- List of snapshots with timestamps and labels
- "Create snapshot" button with label input
- "Revert to snapshot" with confirmation
- Diff viewer (side-by-side JSON diff)
- **Depends on:** WP-4.1, WP-2.6

### WP-4.11: Multi-Monitor / Detachable Windows
**Effort:** Medium (5-7 days)
**Output:** Panels can be detached to separate windows
- Piano roll, mixer, song editor as detachable panels
- Window state persistence
- **Depends on:** WP-4.1, WP-4.2, WP-4.3, WP-4.4

**Phase 4 Milestone:** Full GUI parity with LMMS 1.x, plus recording UI, template drag-and-drop, version history, and multi-monitor support.

---

## Phase 5: MCP Server

### WP-5.1: MCP Server Core
**Effort:** Medium (3-5 days)
**Output:** `lmms-mcp` crate — MCP server with STDIO transport
- `rmcp` crate integration
- Server initialization and capability negotiation
- STDIO transport (for local AI integration)
- Tool call routing to DAW engine
- **Depends on:** WP-1.6, WP-2.1

### WP-5.2: Track Management Tools
**Effort:** Small (2-3 days)
**Output:** `create_track`, `delete_track`, `duplicate_track`, `rename_track`, `set_track_param`
- Create all track types (instrument, sample, automation)
- Parameter setting via symbolic paths
- **Test:** AI creates a track → verify it exists in project state
- **Depends on:** WP-5.1

### WP-5.3: Instrument & Preset Tools
**Effort:** Small (2-3 days)
**Output:** `load_instrument`, `load_preset`, `set_instrument_param`, `list_presets`
- Load any built-in instrument or preset by name/path
- Parameter setting using symbolic paths
- Preset listing with tags and categories
- **Test:** AI loads TB303 preset → verify parameters match
- **Depends on:** WP-5.1, WP-2.9

### WP-5.4: Note Entry Tools
**Effort:** Small (2-3 days)
**Output:** `add_note`, `add_notes_batch`, `remove_notes`, `create_clip`
- Batch note entry for efficiency (AI writes entire melodies in one call)
- Clip creation and positioning
- **Test:** AI creates a C major scale → verify notes in clip
- **Depends on:** WP-5.1, WP-2.3

### WP-5.5: Effects & Automation Tools
**Effort:** Small (2-3 days)
**Output:** `add_effect`, `set_effect_param`, `add_automation`, `add_clip_automation`
- Effect chain manipulation
- Automation curve creation via keyframes
- Per-clip automation using symbolic paths
- **Test:** AI adds reverb → sets wet/dry → adds automation → verify
- **Depends on:** WP-5.1, WP-2.4

### WP-5.6: Mixer & Transport Tools
**Effort:** Small (2-3 days)
**Output:** `create_mixer_channel`, `route_track`, `set_bpm`, `play`, `stop`, `export_audio`
- Mixer channel creation and routing
- Transport control
- Audio export (render to WAV/MP3)
- **Test:** AI sets BPM → creates arrangement → exports → verify audio file
- **Depends on:** WP-5.1

### WP-5.7: Resources & Prompts
**Effort:** Small (2-3 days)
**Output:** MCP resources (project state, library) and prompt templates
- `project://state` — full project state as JSON
- `library://presets/{instrument}` — preset listing
- `library://samples` — sample library
- Prompt templates: `compose_beat`, `compose_chord_progression`, `create_arrangement`
- **Depends on:** WP-5.1

### WP-5.8: HTTP Transport
**Effort:** Small (2-3 days)
**Output:** HTTP + SSE transport for remote MCP access
- HTTP endpoint for tool calls
- SSE stream for server-initiated notifications
- Authentication (API key or token)
- **Depends on:** WP-5.1

**Phase 5 Milestone:** Claude can create a complete song from a natural language prompt — tracks, instruments, notes, effects, automation, mix — through MCP tool calls.

---

## Phase 6: Polish & Parity

### WP-6.1: Cross-Platform Testing
**Effort:** Medium (5-7 days)
- Linux (x86_64, aarch64), Windows (x86_64), macOS (aarch64)
- CI matrix for all targets
- Platform-specific audio backend testing

### WP-6.2: Performance Benchmarking
**Effort:** Small (3-5 days)
- Benchmark audio rendering against C++ LMMS
- Profile memory usage for large projects (200+ tracks)
- Optimize hot paths in audio graph

### WP-6.3: Demo Project Parity
**Effort:** Medium (5-7 days)
- Convert all 28 demo projects via WP-2.8
- Verify each renders correctly
- Fix any conversion bugs

### WP-6.4: Documentation
**Effort:** Medium (5-7 days)
- User guide for new features (templates, versioning, recording, MCP)
- Developer guide for plugin API
- MCP tool reference for AI integrators
- Format specification for `.lmms`, `.lmms-track`, `.lmms-group`

---

## Dependency Graph (Simplified)

```
WP-1.1 (scaffold)
  ├── WP-1.2 (buffers)
  │     ├── WP-1.3 (audio I/O)
  │     ├── WP-1.4 (DSP)
  │     └── WP-1.6 (transport + graph)
  │           └── WP-1.7 (TripleOscillator ← WP-1.4, WP-1.5)
  │                 └── WP-2.2 (track serialization)
  │                       ├── WP-2.3 (clips)
  │                       ├── WP-2.4 (automation)
  │                       ├── WP-2.7 (track bundles ← WP-2.3, WP-2.4, WP-2.5)
  │                       ├── WP-2.8 (legacy reader ← WP-2.3, WP-2.4)
  │                       └── WP-2.9 (preset converter)
  ├── WP-1.5 (MIDI)
  ├── WP-2.1 (project dir)
  │     ├── WP-2.5 (ZIP container)
  │     └── WP-2.6 (autosave journal)
  ├── WP-3.1 (plugin trait)
  │     ├── WP-3.2 (built-in instruments, parallelizable)
  │     ├── WP-3.3 (built-in effects, parallelizable)
  │     ├── WP-3.4–3.7 (LV2/CLAP/VST3/LADSPA, parallelizable)
  │     └── WP-3.8 (ZynAddSubFX)
  └── WP-4.1 (GUI framework)
        ├── WP-4.2–4.7 (UI panels)
        ├── WP-4.8 (recording UI ← WP-1.3)
        ├── WP-4.9 (template UI ← WP-2.7)
        └── WP-4.10 (versioning UI ← WP-2.6)

WP-5.1 (MCP core ← WP-1.6, WP-2.1)
  └── WP-5.2–5.8 (MCP tools, parallelizable)
```

---

## Estimated Effort Summary

| Phase | Packages | Total Effort | Parallelism |
|---|---|---|---|
| Phase 1: Foundation | 7 packages | 25-35 days | WP-1.3, WP-1.4, WP-1.5 can run in parallel |
| Phase 2: Project Format | 9 packages | 30-45 days | WP-2.5, WP-2.6 can parallel with WP-2.2-2.4 |
| Phase 3: Plugin System | 8 packages | 50-75 days | WP-3.2-3.7 are all parallelizable |
| Phase 4: GUI | 11 packages | 75-110 days | WP-4.2-4.7 can largely parallel |
| Phase 5: MCP Server | 8 packages | 20-28 days | WP-5.2-5.8 are all parallelizable |
| Phase 6: Polish | 4 packages | 20-25 days | Most can parallel |
| **Total** | **47 packages** | **220-320 days** | **Significant parallelism possible** |

With a single developer, this is roughly 10-14 months of focused work. With parallelism (2-3 contributors), Phases 3 and 4 compress significantly, bringing it to 6-8 months.

---

## Suggested First Sprint (Weeks 1-4)

Start here to get the fastest path to "sound comes out":

1. **Week 1:** WP-1.1 (scaffold) + WP-1.2 (buffers) + WP-1.5 (MIDI)
2. **Week 2:** WP-1.3 (audio I/O) + WP-1.4 (DSP) — in parallel
3. **Week 3:** WP-1.6 (transport + graph)
4. **Week 4:** WP-1.7 (TripleOscillator — first sound!)

By end of week 4: play a MIDI note through TripleOscillator, hear it from speakers. Everything after this builds on a working foundation.
