-- LMMS SQLite Project Format Schema v1
-- Replaces the monolithic XML .mmp/.mmpz format with a relational database.
-- All entities have proper ID-based references instead of position-based identification.

PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

-- Project metadata
CREATE TABLE project (
    id INTEGER PRIMARY KEY DEFAULT 1,
    name TEXT,
    bpm REAL NOT NULL DEFAULT 140,
    timesig_numerator INTEGER NOT NULL DEFAULT 4,
    timesig_denominator INTEGER NOT NULL DEFAULT 4,
    master_volume REAL NOT NULL DEFAULT 100,
    master_pitch REAL NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    modified_at TEXT NOT NULL DEFAULT (datetime('now'))
);

-- Mixer channels
CREATE TABLE mixer_channel (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    volume REAL NOT NULL DEFAULT 1.0,
    muted INTEGER NOT NULL DEFAULT 0,
    soloed INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- Mixer routing (sends)
CREATE TABLE mixer_route (
    id INTEGER PRIMARY KEY,
    from_channel_id INTEGER NOT NULL REFERENCES mixer_channel(id),
    to_channel_id INTEGER NOT NULL REFERENCES mixer_channel(id),
    amount REAL NOT NULL DEFAULT 1.0
);

-- Effects (attached to mixer channels, instrument tracks, or sample tracks)
CREATE TABLE effect (
    id INTEGER PRIMARY KEY,
    owner_type TEXT NOT NULL,  -- 'mixer_channel', 'instrument_track', 'sample_track'
    owner_id INTEGER NOT NULL,
    plugin_name TEXT NOT NULL,
    sort_order INTEGER NOT NULL DEFAULT 0,
    enabled INTEGER NOT NULL DEFAULT 1,
    wet REAL NOT NULL DEFAULT 1.0,
    gate REAL NOT NULL DEFAULT 0.0,
    params_json TEXT NOT NULL DEFAULT '{}'
);

-- Instrument tracks (live in PatternStore only)
CREATE TABLE instrument_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    volume REAL NOT NULL DEFAULT 100,
    panning REAL NOT NULL DEFAULT 0,
    pitch REAL NOT NULL DEFAULT 0,
    pitch_range INTEGER NOT NULL DEFAULT 1,
    mixer_channel_id INTEGER REFERENCES mixer_channel(id),
    base_note INTEGER NOT NULL DEFAULT 69,
    use_master_pitch INTEGER NOT NULL DEFAULT 1,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    -- Instrument plugin
    instrument_plugin TEXT NOT NULL,
    instrument_params_json TEXT NOT NULL DEFAULT '{}',
    -- Sound shaping (envelope/LFO for vol/cut/res)
    sound_shaping_json TEXT NOT NULL DEFAULT '{}',
    -- Arpeggiator
    arpeggio_json TEXT NOT NULL DEFAULT '{}',
    -- Chord creator
    chord_creator_json TEXT NOT NULL DEFAULT '{}',
    -- MIDI port config
    midi_port_json TEXT NOT NULL DEFAULT '{}',
    -- Microtuner
    microtuner_json TEXT NOT NULL DEFAULT '{}'
);

-- Pattern tracks (Song Editor timeline objects)
CREATE TABLE pattern_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- Pattern clips (the rectangular objects on the Song Editor timeline)
CREATE TABLE pattern_clip (
    id INTEGER PRIMARY KEY,
    pattern_track_id INTEGER NOT NULL REFERENCES pattern_track(id),
    pattern_id INTEGER NOT NULL REFERENCES pattern(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    start_offset INTEGER NOT NULL DEFAULT 0,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT
);

-- Patterns (the columns in the PatternStore grid)
CREATE TABLE pattern (
    id INTEGER PRIMARY KEY,
    name TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- MIDI clips (cells in the PatternStore grid)
CREATE TABLE midi_clip (
    id INTEGER PRIMARY KEY,
    instrument_track_id INTEGER NOT NULL REFERENCES instrument_track(id),
    pattern_id INTEGER NOT NULL REFERENCES pattern(id),
    clip_type INTEGER NOT NULL DEFAULT 1,  -- 0=beat, 1=melody
    steps INTEGER NOT NULL DEFAULT 32,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT,
    UNIQUE(instrument_track_id, pattern_id)
);

-- Notes (atomic musical events)
CREATE TABLE note (
    id INTEGER PRIMARY KEY,
    midi_clip_id INTEGER NOT NULL REFERENCES midi_clip(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    key INTEGER NOT NULL,
    volume INTEGER NOT NULL DEFAULT 100,
    panning INTEGER NOT NULL DEFAULT 0,
    note_type INTEGER NOT NULL DEFAULT 0  -- 0=regular, 1=step
);
CREATE INDEX idx_note_clip_pos ON note(midi_clip_id, position);

-- Per-note detuning automation (rare, optional)
CREATE TABLE note_detuning (
    id INTEGER PRIMARY KEY,
    note_id INTEGER NOT NULL REFERENCES note(id),
    position INTEGER NOT NULL,
    value REAL NOT NULL,
    out_value REAL,
    in_tangent REAL NOT NULL DEFAULT 0,
    out_tangent REAL NOT NULL DEFAULT 0
);

-- Sample tracks
CREATE TABLE sample_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    volume REAL NOT NULL DEFAULT 100,
    panning REAL NOT NULL DEFAULT 0,
    mixer_channel_id INTEGER REFERENCES mixer_channel(id),
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- Sample clips
CREATE TABLE sample_clip (
    id INTEGER PRIMARY KEY,
    sample_track_id INTEGER NOT NULL REFERENCES sample_track(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    source_path TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT
);

-- Automation tracks
CREATE TABLE automation_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- Automation clips
CREATE TABLE automation_clip (
    id INTEGER PRIMARY KEY,
    automation_track_id INTEGER NOT NULL REFERENCES automation_track(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    progression_type INTEGER NOT NULL DEFAULT 1,  -- 0=discrete, 1=linear, 2=cubic
    tension REAL NOT NULL DEFAULT 1.0,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT
);

-- Automation nodes (time-value pairs within an automation clip)
CREATE TABLE automation_node (
    id INTEGER PRIMARY KEY,
    automation_clip_id INTEGER NOT NULL REFERENCES automation_clip(id),
    position INTEGER NOT NULL,
    in_value REAL NOT NULL,
    out_value REAL NOT NULL,
    in_tangent REAL NOT NULL DEFAULT 0,
    out_tangent REAL NOT NULL DEFAULT 0,
    locked_tangents INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_auto_node_clip_pos ON automation_node(automation_clip_id, position);

-- Automation targets (which model parameter an automation clip controls)
CREATE TABLE automation_target (
    id INTEGER PRIMARY KEY,
    automation_clip_id INTEGER NOT NULL REFERENCES automation_clip(id),
    target_object_id INTEGER NOT NULL,  -- JournallingObject ID from the XML
    target_description TEXT  -- human-readable description if available
);

-- Controllers (LFO, MIDI CC, Peak)
CREATE TABLE controller (
    id INTEGER PRIMARY KEY,
    type TEXT NOT NULL,  -- 'lfo', 'midi', 'peak'
    name TEXT NOT NULL DEFAULT '',
    params_json TEXT NOT NULL DEFAULT '{}'
);

-- Controller connections (links a controller to a model parameter)
CREATE TABLE controller_connection (
    id INTEGER PRIMARY KEY,
    owner_type TEXT NOT NULL,  -- entity type that owns the connected parameter
    owner_id INTEGER NOT NULL, -- entity ID
    param_name TEXT NOT NULL,  -- which parameter
    controller_id INTEGER NOT NULL REFERENCES controller(id)
);
