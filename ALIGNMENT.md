# Design Alignment: Specification vs. Implementation

This document maps each requirement from the design specification to the current implementation plan, identifies gaps, and specifies how each gap is resolved.

---

## Requirement 1: Lossless .mmp Import

> "The database must represent a complete and exact 1:1 representation of the .mmp project file structure. No truncated schemas. No simplified representations. No discarded metadata. No lossy transformations."

### Alignment Status: GAP IDENTIFIED AND RESOLVED

**The gap:** The original FORMAT_RESEARCH.md described the .mmp reader as a "one-way migration" that converts to the new JSON format. This implies a *transformation* — which risks being lossy. The spec demands *preservation*.

**Resolution:** The import pipeline has two layers:

#### Layer 1: Raw Preservation (Database)

Every .mmp import first stores the **complete, unmodified XML** in the SQLite database alongside the parsed representation:

```sql
CREATE TABLE imported_projects (
    id INTEGER PRIMARY KEY,
    source_file TEXT NOT NULL,           -- original filename
    source_hash TEXT NOT NULL,           -- SHA-256 of original file
    raw_xml BLOB NOT NULL,              -- the COMPLETE original .mmp/.mmpz content
    import_timestamp INTEGER NOT NULL,
    lmms_version TEXT,                  -- creator version from XML header
    format_version TEXT                 -- format version from XML header
);

-- Every XML element and attribute preserved verbatim
CREATE TABLE mmp_elements (
    id INTEGER PRIMARY KEY,
    project_id INTEGER REFERENCES imported_projects(id),
    xpath TEXT NOT NULL,                -- exact path in XML tree: /lmms-project/song/trackcontainer/track[0]
    tag_name TEXT NOT NULL,             -- element tag: "instrumenttrack", "tripleoscillator", etc.
    attributes TEXT NOT NULL,           -- JSON object of ALL attributes, exactly as they appear
    text_content TEXT,                  -- any text/CDATA content
    parent_id INTEGER REFERENCES mmp_elements(id),
    child_order INTEGER NOT NULL        -- preserve element ordering
);
```

This means:
- Every attribute, even ones we don't understand, is preserved
- Element ordering is preserved
- Window geometry (`width="600" x="5" y="5"`) is preserved
- Magic integer type codes (`type="0"`) are preserved alongside human-readable translations
- Base64 binary blobs are stored both as raw binary AND as the original base64 text
- The original raw XML is stored so we can always go back to the source

#### Layer 2: Parsed Representation (Working Format)

The XML is *also* parsed into the new JSON working format for actual use. But the raw preservation in Layer 1 means we can always:
1. Reconstruct the original .mmp file byte-for-byte (minus whitespace)
2. Re-parse with updated logic if we discover we missed a field
3. Audit what changed between the raw import and the working representation

#### Reconstruction Guarantee Test

```rust
#[test]
fn test_mmp_round_trip_fidelity() {
    let original = read_file("test_project.mmp");
    let db = import_mmp(&original);
    let reconstructed = export_mmp_from_db(&db);

    // Semantic equality: same elements, same attributes, same values
    assert_xml_semantically_equal(&original, &reconstructed);

    // Every attribute from the original must exist in the database
    for (xpath, attr_name, attr_value) in all_xml_attributes(&original) {
        assert_eq!(
            db.get_attribute(xpath, attr_name),
            Some(attr_value),
            "Lost attribute {attr_name} at {xpath}"
        );
    }
}
```

This test runs against all 28 demo projects + 6 templates on every CI build.

---

## Requirement 2: Track-Level Import/Export

> "The system must allow template-like elements—such as individual tracks, groups of tracks, or structured track arrangements—to be extracted from one project and re-imported into another project."

### Alignment Status: FULLY ALIGNED

The `.lmms-track` and `.lmms-group` bundle formats (FORMAT_RESEARCH.md Part 5) are designed exactly for this.

**What can be exported:**

| Unit | Bundle Type | What's Included |
|---|---|---|
| Single instrument track | `.lmms-track` | Instrument + plugin state + effects chain + clips (notes) + automation + samples |
| Single sample track | `.lmms-track` | Sample references + effects chain + clips + automation + recordings |
| Group of tracks | `.lmms-group` | Multiple tracks + shared mixer channels + cross-track automation + samples |
| Pattern/BB track | `.lmms-track` | All instrument lines within the pattern + their clips |

**What travels with the export:**

```
MyBassTrack.lmms-track (ZIP)
├── manifest.json           # format version, contents inventory, checksums
├── track.json              # ALL track settings (see completeness below)
├── instrument/
│   ├── instrument.json     # plugin name, ALL parameters
│   └── state.bin           # binary plugin state (exact reproduction)
├── effects/
│   ├── fx_0.json           # effect plugin name, ALL parameters
│   ├── fx_0_state.bin      # binary effect state
│   └── ...
├── clips/
│   ├── clip_0.json         # ALL notes with position, length, velocity, panning, detuning
│   └── ...
├── automation/
│   ├── volume.json         # keyframes with symbolic target path
│   ├── filter_cutoff.json
│   └── ...
├── samples/                # all referenced audio files
│   └── ...
└── mixer_channel/          # the mixer channel this track was routed to
    ├── channel.json        # name, volume, panning, solo, mute
    ├── fx_0.json           # mixer channel effects
    ├── fx_0_state.bin
    └── sends.json          # send routing (by channel NAME, not index)
```

---

## Requirement 3: Import Preserves All Configuration

> "Tracks and groups of tracks must be transferable between projects in a way that maintains: routing, automation, plugin configuration, track hierarchy, timing and arrangement."

### Alignment Status: FULLY ALIGNED

Each element and how it's preserved:

### Routing

**Current .mmp:** `mixch="2"` — hardcoded integer index.

**In the bundle:**
```json
// track.json
{
  "mixer_channel": {
    "name": "Bass Bus",
    "original_index": 2,
    "definition": "mixer_channel/channel.json"
  }
}

// mixer_channel/channel.json
{
  "name": "Bass Bus",
  "volume": 1.0,
  "muted": false,
  "soloed": false,
  "effects": [
    {
      "plugin": "Compressor",
      "params": { "threshold": -12.0, "ratio": 4.0, "attack": 10.0, "release": 100.0 },
      "state_file": "fx_0_state.bin",
      "wet_dry": 1.0,
      "enabled": true
    }
  ],
  "sends": [
    { "target": "Master", "amount": 1.0 }
  ]
}
```

**On import to a blank project:**
1. System checks: does a channel named "Bass Bus" exist? No.
2. System creates mixer channel "Bass Bus" with ALL effects and settings from `channel.json`
3. System creates ALL send targets that don't exist (if "Master" doesn't exist, it does in every project)
4. System routes the track to the new channel
5. **The channel number is NEVER used** — only the name

**On import to a project that already has a "Bass Bus" channel:**
1. System detects name collision
2. User chooses: merge with existing channel, create "Bass Bus (2)", or pick a different channel
3. Either way, all effects and settings from the bundle are available for comparison

### Automation

**Current .mmp:** `<object id="4975896"/>` — runtime-assigned integer, meaningless outside the session.

**In the bundle:**
```json
// automation/filter_cutoff.json
{
  "target": "track/sound_shaping/filter_cutoff",
  "progression": "cubic_hermite",
  "tension": 0.0,
  "keyframes": [
    { "pos": 0, "value": 5000.0, "in_tan": 0.0, "out_tan": -0.5 },
    { "pos": 384, "value": 800.0, "in_tan": -0.5, "out_tan": 0.0 }
  ]
}
```

**On import:** The symbolic path `track/sound_shaping/filter_cutoff` is resolved against the newly-created track's parameter tree. The automation reconnects automatically — no ID mapping needed.

### Plugin Configuration

Every plugin parameter is stored in two forms:
1. **JSON params** — human-readable, inspectable, diffable
2. **Binary state blob** — exact plugin state for bit-perfect reproduction

If the plugin is available on the target machine: load binary state (exact reproduction).
If the plugin is NOT available: show user the JSON params (they can see what's missing).

### Track Hierarchy

Pattern/BB tracks contain sub-tracks:

```json
// track.json for a Pattern track
{
  "type": "pattern",
  "name": "Drums",
  "sub_tracks": [
    {
      "name": "Kick",
      "file": "sub_tracks/kick.json"
    },
    {
      "name": "Snare",
      "file": "sub_tracks/snare.json"
    },
    {
      "name": "HiHat",
      "file": "sub_tracks/hihat.json"
    }
  ],
  "patterns": [
    {
      "name": "Pattern 0",
      "steps": 16,
      "clips": "patterns/pattern_0/"
    }
  ]
}
```

Each sub-track has its own instrument, effects, automation. The entire hierarchy is preserved.

### Timing and Arrangement

Clips carry absolute position (in ticks) and length:

```json
{
  "clips": [
    {
      "position": 0,       // tick 0
      "length": 768,       // 4 bars at 192 ticks/bar
      "notes": [
        { "key": 60, "pos": 0, "len": 96, "vol": 100, "pan": 0 }
      ]
    },
    {
      "position": 768,     // bar 5
      "length": 384,       // 2 bars
      "notes": [ ... ]
    }
  ]
}
```

On import, the user chooses where to place the clips:
- **At original positions** — exact timing preserved
- **At cursor position** — clips offset relative to insertion point
- **At bar 0** — clips normalized to start at the beginning

---

## Requirement 4: Reconstruction Guarantee

> "Take stored project data from the database. Insert selected tracks or track groups into a completely blank project. Produce an exported result that is functionally identical to the original tracks."

### Alignment Status: FULLY ALIGNED

This is the core test case. Here's the exact flow:

```
[Original Project A]
  │
  ├── Track "Bass" (TripleOscillator, TB303 preset)
  │   ├── volume=59, panning=0, pitch=0, base_note=81
  │   ├── instrument: tripleoscillator with all osc params
  │   ├── filter: moog double lowpass, cut=1, res=0.76, wet=1.0
  │   ├── envelopes: volume (A=0.038, D=0.279, S=0, R=0.112)
  │   ├── effects: [reverb (wet=0.3), delay (wet=0.5)]
  │   ├── clips: [8 bars of notes at specific positions]
  │   ├── automation: filter cutoff sweep over 16 bars
  │   └── mixer: routed to channel "Bass Bus" (vol=1.0, comp+EQ)
  │
  ▼ Export as .lmms-track
  │
  [BassBounce.lmms-track]  (ZIP file)
  │
  ▼ Import into blank project
  │
[New Blank Project B]
  │
  ├── Track "Bass" (TripleOscillator, TB303 preset)
  │   ├── volume=59, panning=0, pitch=0, base_note=81  ✓ IDENTICAL
  │   ├── instrument: tripleoscillator with all osc params  ✓ IDENTICAL
  │   ├── filter: moog double lowpass, cut=1, res=0.76  ✓ IDENTICAL
  │   ├── envelopes: all values preserved  ✓ IDENTICAL
  │   ├── effects: [reverb (wet=0.3), delay (wet=0.5)]  ✓ IDENTICAL
  │   ├── clips: [8 bars of notes]  ✓ IDENTICAL
  │   ├── automation: filter cutoff sweep  ✓ IDENTICAL
  │   └── mixer: routed to NEW channel "Bass Bus" (vol=1.0, comp+EQ)  ✓ CREATED
  │
  └── Mixer Channel "Bass Bus" (NEWLY CREATED)
      ├── volume=1.0  ✓ FROM BUNDLE
      ├── effects: [compressor, EQ]  ✓ FROM BUNDLE
      └── send: Master (amount=1.0)  ✓ FROM BUNDLE
```

**Verification test:**

```rust
#[test]
fn test_track_export_import_reconstruction() {
    // Load original project
    let project_a = Project::open("test_fixtures/complex_project.lmms");
    let bass_track = project_a.track_by_name("Bass").unwrap();

    // Export the track
    let bundle = TrackBundleExporter::export(bass_track, &project_a).unwrap();
    bundle.save("test_output/bass.lmms-track").unwrap();

    // Import into blank project
    let mut project_b = Project::new_blank();
    let imported = TrackBundleImporter::import(
        &mut project_b,
        "test_output/bass.lmms-track"
    ).unwrap();

    // Verify functional identity
    let imported_track = &project_b.tracks()[0];

    // All instrument parameters match
    assert_eq!(bass_track.instrument_params(), imported_track.instrument_params());

    // All effect parameters match
    assert_eq!(bass_track.effect_chain(), imported_track.effect_chain());

    // All notes match
    assert_eq!(bass_track.all_notes(), imported_track.all_notes());

    // All automation matches
    assert_eq!(bass_track.automation_curves(), imported_track.automation_curves());

    // Mixer channel was created with correct settings
    let mixer_ch = project_b.mixer().channel_by_name("Bass Bus").unwrap();
    let original_ch = project_a.mixer().channel(2); // was channel index 2
    assert_eq!(mixer_ch.volume(), original_ch.volume());
    assert_eq!(mixer_ch.effects(), original_ch.effects());

    // RENDER TEST: both produce identical audio
    let render_a = render_track(&project_a, bass_track, 16 /* bars */);
    let render_b = render_track(&project_b, imported_track, 16);
    assert_audio_equal(&render_a, &render_b, 0.001 /* tolerance */);
}
```

The ultimate test: render both tracks and compare audio output. If they sound the same, the reconstruction is complete.

---

## Requirement 5: Mixer Channel Remapping

> "It is critical that any assigned mixer channels are not literally hardcoded, that way when imported to blank project it will create the missing mixer channel as a new channel with all of the previous effects and settings but not the literal channel number."

### Alignment Status: FULLY ALIGNED

This is addressed at three levels:

### Level 1: Storage (Never Store Indices)

In the new format, tracks reference mixer channels by **name**, never by index:

```json
// track.json — NEW format
{
  "mixer_channel": {
    "name": "Drum Bus"
  }
}
```

Compare to the old format:
```xml
<!-- .mmp — OLD format -->
<instrumenttrack mixch="2" ... />
```

### Level 2: Import from .mmp (Index → Name Translation)

When importing a legacy .mmp file, the importer:

1. Reads all `<mixerchannel>` definitions to build an index-to-name map:
   ```
   0 → "Master"
   1 → "Drum"
   2 → "Bass"
   3 → "Lead"
   ```
2. For each track with `mixch="2"`, resolves to name "Bass"
3. Stores the channel name in the track definition, never the index
4. Stores the complete mixer channel definition (effects, volume, sends) alongside

```rust
fn import_mixer_routing(track_xml: &XmlElement, mixer_channels: &[MixerChannelDef]) -> MixerRef {
    let index = track_xml.attr("mixch").parse::<usize>().unwrap_or(0);
    let channel = &mixer_channels[index];

    MixerRef {
        name: channel.name.clone(),          // "Bass" — this is what we store
        original_index: Some(index),         // 2 — kept for reference only, never used for routing
        channel_definition: channel.clone(), // complete definition including effects, sends
    }
}
```

### Level 3: Import to New Project (Create Missing Channels)

When importing a track bundle into a project:

```rust
fn resolve_mixer_channel(project: &mut Project, mixer_ref: &MixerRef) -> MixerChannelId {
    // Try to find by name
    if let Some(existing) = project.mixer().channel_by_name(&mixer_ref.name) {
        return existing.id();
    }

    // Channel doesn't exist — create it with full settings from the bundle
    let new_channel = project.mixer_mut().create_channel(CreateChannelRequest {
        name: mixer_ref.name.clone(),
        volume: mixer_ref.channel_definition.volume,
        panning: mixer_ref.channel_definition.panning,
        muted: mixer_ref.channel_definition.muted,
        effects: mixer_ref.channel_definition.effects.clone(),
        // Sends also resolved by name, not index
        sends: mixer_ref.channel_definition.sends.iter().map(|send| {
            SendRoute {
                target_name: send.target_name.clone(),  // "Master", not "0"
                amount: send.amount,
            }
        }).collect(),
    });

    new_channel.id()
}
```

The key guarantee: **mixer channel index never appears in routing logic.** It exists only as an internal implementation detail for ordering channels in the mixer UI.

---

## Requirement 6: Individual Track Selection

> "I want to be able to select an individual track."

### Alignment Status: FULLY ALIGNED

The MCP server and UI both support individual track operations:

**Via MCP (AI-driven):**
```json
// Export one track
{ "tool": "export_track_template", "track_id": "track_003", "file_path": "my_bass.lmms-track" }

// Import one track
{ "tool": "import_track_template", "file_path": "my_bass.lmms-track", "position": 0 }
```

**Via UI:**
- Right-click any track → "Export as Template..."
- Drag `.lmms-track` file into song editor
- File → Import Track Template...

**Via the directory structure:**
Each track is already an individual file (`tracks/track_003.json`). Selecting and extracting it is a file operation, not a tree surgery.

---

## Requirement 7: Composable Track Creation

> "I want to be able to add this instrument with this tempo and this note and this effect."

### Alignment Status: FULLY ALIGNED

The MCP server provides granular composition tools:

```json
// Step 1: Create a track
{ "tool": "create_track", "name": "Acid Bass", "type": "instrument" }
// Returns: { "track_id": "track_001" }

// Step 2: Load an instrument preset
{ "tool": "load_preset", "track_id": "track_001", "preset_path": "presets/triple_oscillator/TB303.json" }

// Step 3: Set the tempo
{ "tool": "set_bpm", "bpm": 140 }

// Step 4: Add notes
{ "tool": "add_notes_batch", "track_id": "track_001", "clip_id": "clip_0", "notes": [
    { "pitch": 36, "velocity": 100, "position": 0, "length": 48 },
    { "pitch": 36, "velocity": 90, "position": 96, "length": 48 },
    { "pitch": 39, "velocity": 100, "position": 192, "length": 96 }
]}

// Step 5: Add an effect
{ "tool": "add_effect", "track_id": "track_001", "effect_name": "Delay" }
{ "tool": "set_effect_param", "track_id": "track_001", "effect_index": 0, "param": "feedback", "value": 0.4 }

// Step 6: Route to a new mixer channel
{ "tool": "create_mixer_channel", "name": "Bass Bus" }
{ "tool": "route_track", "track_id": "track_001", "channel_name": "Bass Bus" }
{ "tool": "add_mixer_effect", "channel_name": "Bass Bus", "effect_name": "Compressor" }
```

Each step is atomic and composable. The AI (or user via UI) builds up a track piece by piece, and the result is a fully-formed track that can be exported as a `.lmms-track` template and reused.

---

## Requirement 8: Complete Replication on Import

> "When an instrument-track or a pattern-track are imported to a totally blank project, the database should contain everything necessary to fully replicate the effects and all settings from the original project including adding any missing instruments and mixer channels."

### Alignment Status: FULLY ALIGNED

The `.lmms-track` bundle is **self-contained**. It includes:

| Component | Included? | How |
|---|---|---|
| Instrument plugin + all params | Yes | `instrument/instrument.json` + `state.bin` |
| Sound shaping (filter, envelopes, LFOs) | Yes | Part of `track.json` |
| Effect chain (all effects, ordered) | Yes | `effects/fx_N.json` + `fx_N_state.bin` |
| Arpeggiator settings | Yes | Part of `track.json` |
| Chord/note stacking settings | Yes | Part of `track.json` |
| MIDI CC mappings | Yes | Part of `track.json` |
| Microtuning | Yes | Part of `track.json` |
| All clips with all notes | Yes | `clips/clip_N.json` |
| All automation curves | Yes | `automation/*.json` with symbolic paths |
| All referenced samples | Yes | `samples/` directory |
| Mixer channel definition | Yes | `mixer_channel/channel.json` |
| Mixer channel effects | Yes | `mixer_channel/fx_N.json` + `fx_N_state.bin` |
| Mixer send routing | Yes | `mixer_channel/sends.json` (by name) |
| Track metadata (name, color, height) | Yes | Part of `track.json` |

**Nothing is left behind.** A blank project + one `.lmms-track` bundle = a fully functional single-track project with working instrument, effects, automation, mixer routing, and audio.

**For pattern tracks**, the bundle additionally includes:
- All sub-tracks (each with their own instrument, effects, clips)
- Pattern definitions (steps, clip assignments)
- The pattern track's own mixer routing

---

## Gap Summary

| Requirement | Original Status | Resolution |
|---|---|---|
| 1:1 .mmp fidelity | GAP: described as "one-way migration" | RESOLVED: two-layer import (raw XML preservation + parsed working format) |
| Track-level export/import | Aligned | `.lmms-track` bundles |
| Preserve routing | Aligned | Named mixer channels, never indices |
| Preserve automation | Aligned | Symbolic parameter paths |
| Preserve plugin config | Aligned | JSON params + binary state blobs |
| Preserve hierarchy | Aligned | Sub-tracks in pattern bundles |
| Preserve timing | Aligned | Absolute tick positions in clips |
| Mixer channel remapping | Aligned | Create-on-import by name |
| Individual track selection | Aligned | Per-track files + MCP tools + UI |
| Composable creation | Aligned | MCP tool catalog (30+ tools) |
| Full replication from blank | Aligned | Self-contained bundles with all dependencies |
| Reconstruction guarantee | Aligned | Audio render comparison test |

**One gap was found and resolved:** The .mmp import was described as a lossy transformation. It is now a two-layer process: lossless raw preservation (for the guarantee) + parsed working format (for actual use).
