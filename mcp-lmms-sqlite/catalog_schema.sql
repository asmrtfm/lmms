-- LMMS Asset Catalog Schema
-- Manages: Presets, Samples, Plugins (cross-project asset registry)
-- Separate from the .lmms-db project file format (see tools/lmms_schema.sql)
-- Version: 1

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

-- Schema version tracking for migrations
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL DEFAULT (datetime('now')),
    description TEXT
);

INSERT INTO schema_version (version, description) VALUES (1, 'Initial catalog schema');

-- ============================================================
-- PLUGINS (registry of available instruments and effects)
-- ============================================================

CREATE TABLE IF NOT EXISTS plugins (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    display_name TEXT,
    plugin_type TEXT NOT NULL CHECK (plugin_type IN ('instrument', 'effect', 'tool', 'other')),
    plugin_format TEXT NOT NULL CHECK (plugin_format IN ('native', 'ladspa', 'lv2', 'vst2', 'vst3', 'other')),
    file_path TEXT,
    version TEXT,
    author TEXT,
    description TEXT,
    enabled INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX idx_plugins_type ON plugins(plugin_type);
CREATE INDEX idx_plugins_format ON plugins(plugin_format);

CREATE TABLE IF NOT EXISTS plugin_parameters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    plugin_id INTEGER NOT NULL REFERENCES plugins(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    display_name TEXT,
    param_type TEXT NOT NULL CHECK (param_type IN ('float', 'int', 'bool', 'enum', 'string')),
    default_value TEXT,
    min_value TEXT,
    max_value TEXT,
    description TEXT,
    UNIQUE(plugin_id, name)
);

CREATE INDEX idx_plugin_params_plugin ON plugin_parameters(plugin_id);

-- ============================================================
-- PRESETS (instrument/effect preset library)
-- ============================================================

CREATE TABLE IF NOT EXISTS presets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    plugin_name TEXT,
    category TEXT,
    tags TEXT DEFAULT '[]',  -- JSON array of strings
    description TEXT,
    preset_data TEXT,        -- XML or JSON preset content
    file_path TEXT,
    author TEXT,
    is_factory INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at TEXT NOT NULL DEFAULT (datetime('now')),
    FOREIGN KEY (plugin_name) REFERENCES plugins(name) ON DELETE SET NULL
);

CREATE INDEX idx_presets_plugin ON presets(plugin_name);
CREATE INDEX idx_presets_category ON presets(category);

-- Full-text search for presets
CREATE VIRTUAL TABLE IF NOT EXISTS presets_fts USING fts5(
    name, category, tags, description, author,
    content='presets',
    content_rowid='id'
);

CREATE TRIGGER presets_ai AFTER INSERT ON presets BEGIN
    INSERT INTO presets_fts(rowid, name, category, tags, description, author)
    VALUES (new.id, new.name, new.category, new.tags, new.description, new.author);
END;
CREATE TRIGGER presets_ad AFTER DELETE ON presets BEGIN
    INSERT INTO presets_fts(presets_fts, rowid, name, category, tags, description, author)
    VALUES ('delete', old.id, old.name, old.category, old.tags, old.description, old.author);
END;
CREATE TRIGGER presets_au AFTER UPDATE ON presets BEGIN
    INSERT INTO presets_fts(presets_fts, rowid, name, category, tags, description, author)
    VALUES ('delete', old.id, old.name, old.category, old.tags, old.description, old.author);
    INSERT INTO presets_fts(rowid, name, category, tags, description, author)
    VALUES (new.id, new.name, new.category, new.tags, new.description, new.author);
END;

-- ============================================================
-- SAMPLES (audio sample catalog)
-- ============================================================

CREATE TABLE IF NOT EXISTS samples (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    file_path TEXT UNIQUE NOT NULL,
    format TEXT CHECK (format IN ('wav', 'flac', 'ogg', 'mp3', 'aiff', 'raw', 'other')),
    sample_rate INTEGER,
    channels INTEGER,
    bit_depth INTEGER,
    duration_ms INTEGER,
    file_size INTEGER,
    tags TEXT DEFAULT '[]',  -- JSON array of strings
    category TEXT,
    description TEXT,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX idx_samples_format ON samples(format);
CREATE INDEX idx_samples_category ON samples(category);

-- Full-text search for samples
CREATE VIRTUAL TABLE IF NOT EXISTS samples_fts USING fts5(
    name, category, tags, description,
    content='samples',
    content_rowid='id'
);

CREATE TRIGGER samples_ai AFTER INSERT ON samples BEGIN
    INSERT INTO samples_fts(rowid, name, category, tags, description)
    VALUES (new.id, new.name, new.category, new.tags, new.description);
END;
CREATE TRIGGER samples_ad AFTER DELETE ON samples BEGIN
    INSERT INTO samples_fts(samples_fts, rowid, name, category, tags, description)
    VALUES ('delete', old.id, old.name, old.category, old.tags, old.description);
END;
CREATE TRIGGER samples_au AFTER UPDATE ON samples BEGIN
    INSERT INTO samples_fts(samples_fts, rowid, name, category, tags, description)
    VALUES ('delete', old.id, old.name, old.category, old.tags, old.description);
    INSERT INTO samples_fts(rowid, name, category, tags, description)
    VALUES (new.id, new.name, new.category, new.tags, new.description);
END;

-- ============================================================
-- PROJECT INDEX (lightweight index of known .lmms-db / .mmp files)
-- ============================================================

CREATE TABLE IF NOT EXISTS project_index (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    file_path TEXT UNIQUE NOT NULL,
    file_format TEXT CHECK (file_format IN ('lmms-db', 'mmp', 'mmpz')),
    description TEXT,
    tags TEXT DEFAULT '[]',
    bpm REAL,
    time_sig TEXT,
    file_size INTEGER,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at TEXT NOT NULL DEFAULT (datetime('now')),
    last_opened_at TEXT
);

CREATE VIRTUAL TABLE IF NOT EXISTS project_index_fts USING fts5(
    name, description, tags,
    content='project_index',
    content_rowid='id'
);

CREATE TRIGGER project_index_ai AFTER INSERT ON project_index BEGIN
    INSERT INTO project_index_fts(rowid, name, description, tags)
    VALUES (new.id, new.name, new.description, new.tags);
END;
CREATE TRIGGER project_index_ad AFTER DELETE ON project_index BEGIN
    INSERT INTO project_index_fts(project_index_fts, rowid, name, description, tags)
    VALUES ('delete', old.id, old.name, old.description, old.tags);
END;
CREATE TRIGGER project_index_au AFTER UPDATE ON project_index BEGIN
    INSERT INTO project_index_fts(project_index_fts, rowid, name, description, tags)
    VALUES ('delete', old.id, old.name, old.description, old.tags);
    INSERT INTO project_index_fts(rowid, name, description, tags)
    VALUES (new.id, new.name, new.description, new.tags);
END;
