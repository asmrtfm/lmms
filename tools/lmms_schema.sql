-- LMMS SQLite Project Format Schema v3
-- Replaces the monolithic XML .mmp/.mmpz format with a relational database.
--
-- DESIGN PRINCIPLE: Every table has an extra_json column that captures ALL attributes
-- and child elements not stored in named columns. This guarantees lossless round-trips.
-- JSON columns store structured data IN the database — no external files.

PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

-- Project metadata (head element + root attributes)
CREATE TABLE project (
    id INTEGER PRIMARY KEY DEFAULT 1,
    name TEXT,
    bpm REAL NOT NULL DEFAULT 140,
    timesig_numerator INTEGER NOT NULL DEFAULT 4,
    timesig_denominator INTEGER NOT NULL DEFAULT 4,
    master_volume REAL NOT NULL DEFAULT 100,
    master_pitch REAL NOT NULL DEFAULT 0,
    -- Root element attributes (lmms-project)
    lmms_version TEXT NOT NULL DEFAULT '30',
    project_type TEXT NOT NULL DEFAULT 'song',
    creator TEXT NOT NULL DEFAULT 'LMMS',
    creator_version TEXT NOT NULL DEFAULT '1.3.0-alpha',
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    modified_at TEXT NOT NULL DEFAULT (datetime('now')),
    extra_json TEXT NOT NULL DEFAULT '{}'  -- all other <head> attributes
);

-- Song-level UI state elements (pianoroll, automationeditor, projectnotes, timeline, etc.)
-- Each row stores one top-level <song> child element that is NOT a trackcontainer/mixer
CREATE TABLE song_ui_element (
    id INTEGER PRIMARY KEY,
    element_name TEXT NOT NULL,  -- 'pianoroll', 'automationeditor', 'projectnotes', 'timeline', 'ControllerRackView'
    attributes_json TEXT NOT NULL DEFAULT '{}',  -- all attributes on the element
    children_json TEXT NOT NULL DEFAULT '{}',    -- all child elements serialized
    content_text TEXT  -- CDATA/text content (e.g. project notes HTML)
);

-- Trackcontainer window state (song and patternstore)
CREATE TABLE trackcontainer_state (
    id INTEGER PRIMARY KEY,
    container_type TEXT NOT NULL UNIQUE,  -- 'song' or 'patternstore'
    visible INTEGER NOT NULL DEFAULT 1,
    minimized INTEGER NOT NULL DEFAULT 0,
    maximized INTEGER NOT NULL DEFAULT 0,
    x INTEGER NOT NULL DEFAULT 0,
    y INTEGER NOT NULL DEFAULT 0,
    width INTEGER NOT NULL DEFAULT 1600,
    height INTEGER NOT NULL DEFAULT 900,
    extra_json TEXT NOT NULL DEFAULT '{}'
);

-- Mixer window state (the <mixer> element attributes)
CREATE TABLE mixer_state (
    id INTEGER PRIMARY KEY DEFAULT 1,
    visible INTEGER NOT NULL DEFAULT 0,
    minimized INTEGER NOT NULL DEFAULT 0,
    maximized INTEGER NOT NULL DEFAULT 0,
    x INTEGER NOT NULL DEFAULT 0,
    y INTEGER NOT NULL DEFAULT 0,
    width INTEGER NOT NULL DEFAULT 865,
    height INTEGER NOT NULL DEFAULT 278,
    extra_json TEXT NOT NULL DEFAULT '{}'
);

-- Mixer channels
CREATE TABLE mixer_channel (
    id INTEGER PRIMARY KEY,  -- the mixer channel number (num attribute)
    name TEXT NOT NULL DEFAULT '',
    volume REAL NOT NULL DEFAULT 1.0,
    muted INTEGER NOT NULL DEFAULT 0,
    soloed INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'  -- all other attributes and child elements
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
    -- fxchain-level metadata stored on the first effect per owner
    fxchain_enabled INTEGER NOT NULL DEFAULT 1,
    params_json TEXT NOT NULL DEFAULT '{}'  -- ALL other attributes and child elements
);

-- Instrument tracks (patternstore or song-level)
CREATE TABLE instrument_track (
    id INTEGER PRIMARY KEY,
    container_type TEXT NOT NULL DEFAULT 'patternstore',  -- 'patternstore' or 'song'
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
    instrument_plugin TEXT NOT NULL,
    instrument_params_json TEXT NOT NULL DEFAULT '{}',
    sound_shaping_json TEXT NOT NULL DEFAULT '{}',
    arpeggio_json TEXT NOT NULL DEFAULT '{}',
    chord_creator_json TEXT NOT NULL DEFAULT '{}',
    midi_port_json TEXT NOT NULL DEFAULT '{}',
    microtuner_json TEXT NOT NULL DEFAULT '{}',
    track_extra_json TEXT NOT NULL DEFAULT '{}',
    instrumenttrack_extra_json TEXT NOT NULL DEFAULT '{}'
);

-- Pattern tracks (Song Editor timeline objects)
CREATE TABLE pattern_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
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
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
);

-- Patterns (the columns in the PatternStore grid)
CREATE TABLE pattern (
    id INTEGER PRIMARY KEY,
    name TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- MIDI clips (patternstore grid cells or song-level clips)
CREATE TABLE midi_clip (
    id INTEGER PRIMARY KEY,
    instrument_track_id INTEGER NOT NULL REFERENCES instrument_track(id),
    pattern_id INTEGER REFERENCES pattern(id),  -- NULL for song-level instrument track clips
    position INTEGER NOT NULL DEFAULT 0,  -- absolute song position for song-level clips
    length INTEGER NOT NULL DEFAULT 0,    -- clip length for song-level clips
    clip_type INTEGER NOT NULL DEFAULT 1,  -- 0=beat, 1=melody
    steps INTEGER NOT NULL DEFAULT 32,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
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
    note_type INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX idx_note_clip_pos ON note(midi_clip_id, position);

-- Per-note detuning automation
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
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
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
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
);

-- Automation tracks
CREATE TABLE automation_track (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    muted INTEGER NOT NULL DEFAULT 0,
    solo INTEGER NOT NULL DEFAULT 0,
    color TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    track_type INTEGER NOT NULL DEFAULT 5,
    container_type TEXT NOT NULL DEFAULT 'song_tc',
    extra_json TEXT NOT NULL DEFAULT '{}'
);

-- Automation clips
CREATE TABLE automation_clip (
    id INTEGER PRIMARY KEY,
    automation_track_id INTEGER NOT NULL REFERENCES automation_track(id),
    position INTEGER NOT NULL,
    length INTEGER NOT NULL,
    progression_type INTEGER NOT NULL DEFAULT 1,
    tension REAL NOT NULL DEFAULT 1.0,
    muted INTEGER NOT NULL DEFAULT 0,
    name TEXT,
    color TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}'
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
    target_object_id INTEGER NOT NULL,
    target_description TEXT
);

-- Controllers (LFO, MIDI CC, Peak)
CREATE TABLE controller (
    id INTEGER PRIMARY KEY,
    type TEXT NOT NULL,
    name TEXT NOT NULL DEFAULT '',
    params_json TEXT NOT NULL DEFAULT '{}'
);

-- Controller connections (links a controller to a model parameter)
CREATE TABLE controller_connection (
    id INTEGER PRIMARY KEY,
    owner_type TEXT NOT NULL,  -- 'mixer_channel', 'instrument_track', etc.
    owner_id INTEGER NOT NULL,
    param_name TEXT NOT NULL,
    controller_id INTEGER REFERENCES controller(id),  -- NULL for inline controllers
    connection_json TEXT NOT NULL DEFAULT '{}'  -- full connection element attributes
);

-- Scales (microtonal scale definitions)
CREATE TABLE scale (
    id INTEGER PRIMARY KEY,
    description TEXT NOT NULL DEFAULT '',
    intervals_json TEXT NOT NULL DEFAULT '[]',
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- Keymaps (microtonal keyboard mappings)
CREATE TABLE keymap (
    id INTEGER PRIMARY KEY,
    description TEXT NOT NULL DEFAULT '',
    base_key INTEGER NOT NULL DEFAULT 69,
    base_freq REAL NOT NULL DEFAULT 440.0,
    first_key INTEGER NOT NULL DEFAULT 0,
    last_key INTEGER NOT NULL DEFAULT 127,
    middle_key INTEGER NOT NULL DEFAULT 60,
    sort_order INTEGER NOT NULL DEFAULT 0,
    extra_json TEXT NOT NULL DEFAULT '{}'
);
