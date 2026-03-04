"""LMMS MCP Server - SQLite-backed management for LMMS projects, presets, samples, and plugins.

Two database layers:
1. CATALOG DB (~/.lmms/catalog.db) - persistent registry of presets, samples, plugins
2. PROJECT DB (*.lmms-db files) - individual project files using the schema from
   tools/lmms_schema.sql (created by lmms_convert.py or the C++ XmlToSqlite bridge)
"""

import json
import os
import shutil
import sqlite3
import subprocess
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP

from lmms_defaults import (
    get_effect_defaults,
    get_instrument_defaults,
    get_track_subsystem_defaults,
    merge_params,
    normalize_plugin_name,
)

CATALOG_DB_PATH = os.environ.get(
    "LMMS_CATALOG_DB", str(Path.home() / ".lmms" / "catalog.db")
)
CATALOG_SCHEMA_PATH = Path(__file__).parent / "catalog_schema.sql"
# Path to lmms_convert.py and lmms_export.py (in the tools/ directory)
TOOLS_DIR = Path(__file__).parent.parent / "tools"

mcp = FastMCP(
    "lmms-sqlite",
    instructions=(
        "Manage LMMS projects (.lmms-db), presets, samples, and plugins via SQLite. "
        "Integrates with the SQLite project format from tools/lmms_convert.py."
    ),
)


# ============================================================
# Database helpers
# ============================================================


@contextmanager
def _open_db(db_path: str):
    """Yield a sqlite3 connection with row_factory set."""
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


@contextmanager
def get_catalog():
    """Get a connection to the catalog database, initializing if needed."""
    Path(CATALOG_DB_PATH).parent.mkdir(parents=True, exist_ok=True)
    with _open_db(CATALOG_DB_PATH) as conn:
        cursor = conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name='schema_version'"
        )
        if cursor.fetchone() is None:
            conn.executescript(CATALOG_SCHEMA_PATH.read_text())
        yield conn


@contextmanager
def open_project(db_path: str):
    """Open a .lmms-db project file for reading/writing.

    If given a .mmp or .mmpz file, auto-converts to .lmms-db first using
    tools/lmms_convert.py, then opens the converted database.
    """
    p = Path(db_path)
    if not p.exists():
        raise FileNotFoundError(f"Project file not found: {db_path}")

    # Auto-convert .mmp/.mmpz to .lmms-db
    if p.suffix in (".mmp", ".mmpz"):
        converted = p.with_suffix(".lmms-db")
        if not converted.exists():
            convert_script = TOOLS_DIR / "lmms_convert.py"
            if not convert_script.exists():
                raise FileNotFoundError(
                    f"lmms_convert.py not found at {convert_script}. "
                    "Cannot auto-convert .mmp files without it."
                )
            result = subprocess.run(
                ["python3", str(convert_script), db_path, str(converted)],
                capture_output=True, text=True, timeout=120,
            )
            if result.returncode != 0:
                raise ValueError(f"Auto-conversion failed: {result.stderr[:500]}")
        db_path = str(converted)
    elif p.suffix != ".lmms-db":
        raise ValueError(f"Expected .lmms-db, .mmp, or .mmpz file, got: {p.suffix}")

    with _open_db(db_path) as conn:
        yield conn


def _row_to_dict(row: sqlite3.Row | None) -> dict | None:
    if row is None:
        return None
    return dict(row)


def _rows_to_list(rows: list[sqlite3.Row]) -> list[dict]:
    return [dict(r) for r in rows]


def _build_where(filters: dict[str, Any]) -> tuple[str, list]:
    """Build a WHERE clause from a dict of column=value filters."""
    clauses = []
    values = []
    for col, val in filters.items():
        if val is not None:
            clauses.append(f"{col} = ?")
            values.append(val)
    where = " AND ".join(clauses) if clauses else "1=1"
    return where, values


def _ok(data: Any = None, **kwargs) -> str:
    if data is not None:
        return json.dumps(data)
    return json.dumps(kwargs)


def _err(msg: str) -> str:
    return json.dumps({"error": msg})


# ============================================================
# PROJECT FILE tools (.lmms-db)
# These operate on individual .lmms-db project files created by
# lmms_convert.py or the C++ XmlToSqlite bridge.
# ============================================================


@mcp.tool()
def project_info(db_path: str) -> str:
    """Get project metadata and summary from a .lmms-db file.

    Args:
        db_path: Path to the .lmms-db project file.
    """
    try:
        with open_project(db_path) as conn:
            project = conn.execute("SELECT * FROM project").fetchone()
            tables = {}
            for table in (
                "instrument_track", "pattern_track", "sample_track",
                "automation_track", "pattern", "midi_clip", "note",
                "mixer_channel", "effect", "controller",
            ):
                try:
                    row = conn.execute(f"SELECT COUNT(*) as cnt FROM {table}").fetchone()  # noqa: S608
                    tables[table] = row["cnt"]
                except sqlite3.OperationalError:
                    tables[table] = 0
            return _ok(project=_row_to_dict(project), entity_counts=tables)
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_list_tracks(db_path: str) -> str:
    """List all tracks in a .lmms-db project, grouped by type.

    Args:
        db_path: Path to the .lmms-db project file.
    """
    try:
        with open_project(db_path) as conn:
            result: dict[str, list] = {}
            for table, label in [
                ("instrument_track", "instrument_tracks"),
                ("pattern_track", "pattern_tracks"),
                ("sample_track", "sample_tracks"),
                ("automation_track", "automation_tracks"),
            ]:
                try:
                    rows = conn.execute(
                        f"SELECT * FROM {table} ORDER BY sort_order"  # noqa: S608
                    ).fetchall()
                    result[label] = _rows_to_list(rows)
                except sqlite3.OperationalError:
                    result[label] = []
            return _ok(result)
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_get_instrument_track(db_path: str, track_id: int) -> str:
    """Get detailed info about an instrument track including its MIDI clips.

    Args:
        db_path: Path to the .lmms-db project file.
        track_id: ID of the instrument track.
    """
    try:
        with open_project(db_path) as conn:
            track = conn.execute(
                "SELECT * FROM instrument_track WHERE id = ?", (track_id,)
            ).fetchone()
            if track is None:
                return _err(f"Instrument track {track_id} not found")
            clips = conn.execute(
                "SELECT * FROM midi_clip WHERE instrument_track_id = ?", (track_id,)
            ).fetchall()
            effects = conn.execute(
                "SELECT * FROM effect WHERE owner_type = 'instrument_track' AND owner_id = ? ORDER BY sort_order",
                (track_id,),
            ).fetchall()
            return _ok(
                track=_row_to_dict(track),
                midi_clips=_rows_to_list(clips),
                effects=_rows_to_list(effects),
            )
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_get_notes(db_path: str, midi_clip_id: int) -> str:
    """Get all notes in a MIDI clip.

    Args:
        db_path: Path to the .lmms-db project file.
        midi_clip_id: ID of the MIDI clip.
    """
    try:
        with open_project(db_path) as conn:
            clip = conn.execute(
                "SELECT * FROM midi_clip WHERE id = ?", (midi_clip_id,)
            ).fetchone()
            if clip is None:
                return _err(f"MIDI clip {midi_clip_id} not found")
            notes = conn.execute(
                "SELECT * FROM note WHERE midi_clip_id = ? ORDER BY position, key",
                (midi_clip_id,),
            ).fetchall()
            return _ok(clip=_row_to_dict(clip), notes=_rows_to_list(notes))
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_get_mixer(db_path: str) -> str:
    """Get all mixer channels and their routing from a project.

    Args:
        db_path: Path to the .lmms-db project file.
    """
    try:
        with open_project(db_path) as conn:
            channels = conn.execute(
                "SELECT * FROM mixer_channel ORDER BY sort_order"
            ).fetchall()
            routes = conn.execute("SELECT * FROM mixer_route").fetchall()
            effects = conn.execute(
                "SELECT * FROM effect WHERE owner_type = 'mixer_channel' ORDER BY owner_id, sort_order"
            ).fetchall()
            return _ok(
                channels=_rows_to_list(channels),
                routes=_rows_to_list(routes),
                effects=_rows_to_list(effects),
            )
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_get_automation(db_path: str, track_id: int = 0) -> str:
    """Get automation tracks, clips, and nodes from a project.

    Args:
        db_path: Path to the .lmms-db project file.
        track_id: Optional automation track ID to filter by. 0 = all tracks.
    """
    try:
        with open_project(db_path) as conn:
            if track_id:
                tracks = conn.execute(
                    "SELECT * FROM automation_track WHERE id = ?", (track_id,)
                ).fetchall()
            else:
                tracks = conn.execute(
                    "SELECT * FROM automation_track ORDER BY sort_order"
                ).fetchall()
            track_ids = [t["id"] for t in tracks]
            clips = []
            nodes = []
            targets = []
            if track_ids:
                placeholders = ",".join("?" * len(track_ids))
                clips = conn.execute(
                    f"SELECT * FROM automation_clip WHERE automation_track_id IN ({placeholders}) ORDER BY position",  # noqa: S608
                    track_ids,
                ).fetchall()
                clip_ids = [c["id"] for c in clips]
                if clip_ids:
                    cp = ",".join("?" * len(clip_ids))
                    nodes = conn.execute(
                        f"SELECT * FROM automation_node WHERE automation_clip_id IN ({cp}) ORDER BY automation_clip_id, position",  # noqa: S608
                        clip_ids,
                    ).fetchall()
                    targets = conn.execute(
                        f"SELECT * FROM automation_target WHERE automation_clip_id IN ({cp})",  # noqa: S608
                        clip_ids,
                    ).fetchall()
            return _ok(
                tracks=_rows_to_list(tracks),
                clips=_rows_to_list(clips),
                nodes=_rows_to_list(nodes),
                targets=_rows_to_list(targets),
            )
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_get_patterns(db_path: str) -> str:
    """Get all patterns (PatternStore columns) and pattern clips from a project.

    Args:
        db_path: Path to the .lmms-db project file.
    """
    try:
        with open_project(db_path) as conn:
            patterns = conn.execute(
                "SELECT * FROM pattern ORDER BY sort_order"
            ).fetchall()
            clips = conn.execute(
                "SELECT * FROM pattern_clip ORDER BY pattern_track_id, position"
            ).fetchall()
            return _ok(
                patterns=_rows_to_list(patterns),
                pattern_clips=_rows_to_list(clips),
            )
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_get_sample_clips(db_path: str, track_id: int = 0) -> str:
    """Get sample tracks and their clips from a project.

    Args:
        db_path: Path to the .lmms-db project file.
        track_id: Optional sample track ID to filter by. 0 = all tracks.
    """
    try:
        with open_project(db_path) as conn:
            if track_id:
                tracks = conn.execute(
                    "SELECT * FROM sample_track WHERE id = ?", (track_id,)
                ).fetchall()
            else:
                tracks = conn.execute(
                    "SELECT * FROM sample_track ORDER BY sort_order"
                ).fetchall()
            track_ids = [t["id"] for t in tracks]
            clips = []
            if track_ids:
                placeholders = ",".join("?" * len(track_ids))
                clips = conn.execute(
                    f"SELECT * FROM sample_clip WHERE sample_track_id IN ({placeholders}) ORDER BY position",  # noqa: S608
                    track_ids,
                ).fetchall()
            return _ok(
                tracks=_rows_to_list(tracks),
                clips=_rows_to_list(clips),
            )
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def project_query(db_path: str, query: str, params: str = "[]") -> str:
    """Execute a raw SQL query against a .lmms-db project file.

    Args:
        db_path: Path to the .lmms-db project file.
        query: SQL query string. Use ? for parameter placeholders.
        params: JSON array of parameter values.
    """
    try:
        param_list = json.loads(params)
        with open_project(db_path) as conn:
            cursor = conn.execute(query, param_list)
            if cursor.description:
                rows = cursor.fetchall()
                return _ok(rows=_rows_to_list(rows), count=len(rows))
            return _ok(rows_affected=cursor.rowcount)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def project_add_note(
    db_path: str,
    midi_clip_id: int,
    position: int,
    length: int,
    key: int,
    volume: int = 100,
    panning: int = 0,
) -> str:
    """Add a note to a MIDI clip in a .lmms-db project.

    Args:
        db_path: Path to the .lmms-db project file.
        midi_clip_id: ID of the MIDI clip to add the note to.
        position: Note position in ticks.
        length: Note length in ticks.
        key: MIDI key number (0-127, 69=A4).
        volume: Note velocity (0-127).
        panning: Note panning (-100 to 100).
    """
    try:
        with open_project(db_path) as conn:
            clip = conn.execute(
                "SELECT id FROM midi_clip WHERE id = ?", (midi_clip_id,)
            ).fetchone()
            if clip is None:
                return _err(f"MIDI clip {midi_clip_id} not found")
            cursor = conn.execute(
                """INSERT INTO note (midi_clip_id, position, length, key, volume, panning)
                   VALUES (?, ?, ?, ?, ?, ?)""",
                (midi_clip_id, position, length, key, volume, panning),
            )
            return _ok(id=cursor.lastrowid, midi_clip_id=midi_clip_id)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def project_update_note(
    db_path: str, note_id: int, updates: str
) -> str:
    """Update a note in a .lmms-db project.

    Args:
        db_path: Path to the .lmms-db project file.
        note_id: ID of the note to update.
        updates: JSON object of field:value pairs (position, length, key, volume, panning).
    """
    try:
        data = json.loads(updates)
        allowed = {"position", "length", "key", "volume", "panning"}
        data = {k: v for k, v in data.items() if k in allowed}
        if not data:
            return _err("No valid updates provided")
        set_clause = ", ".join(f"{k} = ?" for k in data)
        values = list(data.values())
        with open_project(db_path) as conn:
            conn.execute(
                f"UPDATE note SET {set_clause} WHERE id = ?",  # noqa: S608
                values + [note_id],
            )
            return _ok(status="updated", note_id=note_id)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def project_delete_note(db_path: str, note_id: int) -> str:
    """Delete a note from a .lmms-db project.

    Args:
        db_path: Path to the .lmms-db project file.
        note_id: ID of the note to delete.
    """
    try:
        with open_project(db_path) as conn:
            conn.execute("DELETE FROM note WHERE id = ?", (note_id,))
            return _ok(status="deleted", note_id=note_id)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def project_update_track(
    db_path: str, track_type: str, track_id: int, updates: str
) -> str:
    """Update a track's properties in a .lmms-db project.

    Args:
        db_path: Path to the .lmms-db project file.
        track_type: One of: instrument_track, pattern_track, sample_track, automation_track.
        track_id: ID of the track to update.
        updates: JSON object of field:value pairs to update.
    """
    valid_tables = {"instrument_track", "pattern_track", "sample_track", "automation_track"}
    if track_type not in valid_tables:
        return _err(f"track_type must be one of: {', '.join(sorted(valid_tables))}")
    try:
        data = json.loads(updates)
        if not data:
            return _err("No updates provided")
        set_clause = ", ".join(f"{k} = ?" for k in data)
        values = list(data.values())
        with open_project(db_path) as conn:
            conn.execute(
                f"UPDATE {track_type} SET {set_clause} WHERE id = ?",  # noqa: S608
                values + [track_id],
            )
            return _ok(status="updated", track_type=track_type, track_id=track_id)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def project_update_mixer_channel(
    db_path: str, channel_id: int, updates: str
) -> str:
    """Update a mixer channel in a .lmms-db project.

    Args:
        db_path: Path to the .lmms-db project file.
        channel_id: ID of the mixer channel.
        updates: JSON object of field:value pairs (name, volume, muted, soloed, color).
    """
    try:
        data = json.loads(updates)
        allowed = {"name", "volume", "muted", "soloed", "color"}
        data = {k: v for k, v in data.items() if k in allowed}
        if not data:
            return _err("No valid updates provided")
        set_clause = ", ".join(f"{k} = ?" for k in data)
        values = list(data.values())
        with open_project(db_path) as conn:
            conn.execute(
                f"UPDATE mixer_channel SET {set_clause} WHERE id = ?",  # noqa: S608
                values + [channel_id],
            )
            return _ok(status="updated", channel_id=channel_id)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


# ============================================================
# CATALOG tools (persistent asset registry)
# Operates on ~/.lmms/catalog.db
# ============================================================


# --- Catalog: Schema / Stats ---

@mcp.tool()
def catalog_init() -> str:
    """Initialize the LMMS asset catalog database."""
    with get_catalog() as conn:
        _ = conn  # connection opened = schema applied
    return _ok(status="ok", db_path=CATALOG_DB_PATH)


@mcp.tool()
def catalog_stats() -> str:
    """Get catalog database statistics."""
    with get_catalog() as conn:
        stats: dict[str, Any] = {}
        for table in ("plugins", "presets", "samples", "project_index", "plugin_parameters"):
            row = conn.execute(f"SELECT COUNT(*) as cnt FROM {table}").fetchone()  # noqa: S608
            stats[f"{table}_count"] = row["cnt"]

        rows = conn.execute(
            "SELECT plugin_type, COUNT(*) as cnt FROM plugins GROUP BY plugin_type"
        ).fetchall()
        stats["plugins_by_type"] = {r["plugin_type"]: r["cnt"] for r in rows}

        rows = conn.execute(
            "SELECT format, COUNT(*) as cnt FROM samples WHERE format IS NOT NULL GROUP BY format"
        ).fetchall()
        stats["samples_by_format"] = {r["format"]: r["cnt"] for r in rows}

        rows = conn.execute(
            "SELECT category, COUNT(*) as cnt FROM presets WHERE category IS NOT NULL GROUP BY category ORDER BY cnt DESC LIMIT 20"
        ).fetchall()
        stats["presets_by_category"] = {r["category"]: r["cnt"] for r in rows}

        try:
            stats["db_size_bytes"] = Path(CATALOG_DB_PATH).stat().st_size
        except FileNotFoundError:
            stats["db_size_bytes"] = 0
        return _ok(stats)


@mcp.tool()
def catalog_query(query: str, params: str = "[]") -> str:
    """Execute a raw SQL query against the catalog database.

    Args:
        query: SQL query. Use ? for parameters.
        params: JSON array of parameter values.
    """
    param_list = json.loads(params)
    with get_catalog() as conn:
        cursor = conn.execute(query, param_list)
        if cursor.description:
            rows = cursor.fetchall()
            return _ok(rows=_rows_to_list(rows), count=len(rows))
        return _ok(rows_affected=cursor.rowcount)


# --- Catalog: Plugins ---

@mcp.tool()
def catalog_create_plugin(
    name: str,
    plugin_type: str,
    plugin_format: str,
    display_name: str = "",
    file_path: str = "",
    version: str = "",
    author: str = "",
    description: str = "",
    enabled: bool = True,
) -> str:
    """Register a plugin in the catalog.

    Args:
        name: Unique internal plugin name.
        plugin_type: One of: instrument, effect, tool, other.
        plugin_format: One of: native, ladspa, lv2, vst2, vst3, other.
        display_name: Human-readable name.
        file_path: Path to plugin file/library.
        version: Plugin version string.
        author: Plugin author.
        description: Plugin description.
        enabled: Whether the plugin is enabled.
    """
    with get_catalog() as conn:
        cursor = conn.execute(
            """INSERT INTO plugins (name, display_name, plugin_type, plugin_format,
               file_path, version, author, description, enabled)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (name, display_name or name, plugin_type, plugin_format,
             file_path, version, author, description, int(enabled)),
        )
        return _ok(id=cursor.lastrowid, name=name)


@mcp.tool()
def catalog_get_plugin(plugin_id: int = 0, name: str = "") -> str:
    """Get a plugin from the catalog by ID or name.

    Args:
        plugin_id: Plugin ID.
        name: Plugin name (alternative lookup).
    """
    with get_catalog() as conn:
        if plugin_id:
            row = conn.execute("SELECT * FROM plugins WHERE id = ?", (plugin_id,)).fetchone()
        elif name:
            row = conn.execute("SELECT * FROM plugins WHERE name = ?", (name,)).fetchone()
        else:
            return _err("Provide plugin_id or name")
        if row is None:
            return _err("Plugin not found")
        params = conn.execute(
            "SELECT * FROM plugin_parameters WHERE plugin_id = ?", (row["id"],)
        ).fetchall()
        result = _row_to_dict(row)
        result["parameters"] = _rows_to_list(params)
        return _ok(result)


@mcp.tool()
def catalog_list_plugins(
    plugin_type: str = "",
    plugin_format: str = "",
    enabled: str = "",
    limit: int = 50,
    offset: int = 0,
) -> str:
    """List plugins in the catalog with optional filters.

    Args:
        plugin_type: Filter by type (instrument, effect, tool, other).
        plugin_format: Filter by format (native, ladspa, lv2, vst2, vst3, other).
        enabled: Filter by enabled ("true" or "false").
        limit: Max results.
        offset: Results to skip.
    """
    filters: dict[str, Any] = {}
    if plugin_type:
        filters["plugin_type"] = plugin_type
    if plugin_format:
        filters["plugin_format"] = plugin_format
    if enabled:
        filters["enabled"] = 1 if enabled.lower() == "true" else 0
    where, values = _build_where(filters)
    with get_catalog() as conn:
        rows = conn.execute(
            f"SELECT * FROM plugins WHERE {where} ORDER BY name LIMIT ? OFFSET ?",  # noqa: S608
            values + [limit, offset],
        ).fetchall()
        total = conn.execute(
            f"SELECT COUNT(*) as cnt FROM plugins WHERE {where}", values  # noqa: S608
        ).fetchone()
        return _ok(plugins=_rows_to_list(rows), total=total["cnt"])


@mcp.tool()
def catalog_update_plugin(plugin_id: int, updates: str) -> str:
    """Update a plugin in the catalog.

    Args:
        plugin_id: ID of the plugin.
        updates: JSON object of field:value pairs to update.
    """
    data = json.loads(updates)
    allowed = {"name", "display_name", "plugin_type", "plugin_format",
               "file_path", "version", "author", "description", "enabled"}
    data = {k: v for k, v in data.items() if k in allowed}
    if not data:
        return _err("No valid updates provided")
    set_clause = ", ".join(f"{k} = ?" for k in data)
    set_clause += ", updated_at = datetime('now')"
    values = list(data.values())
    with get_catalog() as conn:
        conn.execute(
            f"UPDATE plugins SET {set_clause} WHERE id = ?",  # noqa: S608
            values + [plugin_id],
        )
        return _ok(status="updated", plugin_id=plugin_id)


@mcp.tool()
def catalog_delete_plugin(plugin_id: int) -> str:
    """Delete a plugin from the catalog.

    Args:
        plugin_id: ID of the plugin.
    """
    with get_catalog() as conn:
        conn.execute("DELETE FROM plugins WHERE id = ?", (plugin_id,))
        return _ok(status="deleted", plugin_id=plugin_id)


@mcp.tool()
def catalog_add_plugin_parameter(
    plugin_id: int,
    name: str,
    param_type: str,
    display_name: str = "",
    default_value: str = "",
    min_value: str = "",
    max_value: str = "",
    description: str = "",
) -> str:
    """Add a parameter definition to a plugin in the catalog.

    Args:
        plugin_id: ID of the parent plugin.
        name: Parameter name.
        param_type: One of: float, int, bool, enum, string.
        display_name: Human-readable name.
        default_value: Default value.
        min_value: Minimum value.
        max_value: Maximum value.
        description: Parameter description.
    """
    with get_catalog() as conn:
        cursor = conn.execute(
            """INSERT INTO plugin_parameters
               (plugin_id, name, display_name, param_type, default_value, min_value, max_value, description)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
            (plugin_id, name, display_name or name, param_type,
             default_value, min_value, max_value, description),
        )
        return _ok(id=cursor.lastrowid, plugin_id=plugin_id, name=name)


# --- Catalog: Presets ---

@mcp.tool()
def catalog_create_preset(
    name: str,
    plugin_name: str = "",
    category: str = "",
    tags: str = "[]",
    description: str = "",
    preset_data: str = "",
    file_path: str = "",
    author: str = "",
    is_factory: bool = False,
) -> str:
    """Add a preset to the catalog.

    Args:
        name: Preset name.
        plugin_name: Associated plugin name.
        category: Category (e.g., "Bass", "Lead", "Pad").
        tags: JSON array of tag strings.
        description: Preset description.
        preset_data: XML or JSON preset content.
        file_path: Path to preset file.
        author: Preset author.
        is_factory: Whether this is a factory preset.
    """
    with get_catalog() as conn:
        cursor = conn.execute(
            """INSERT INTO presets (name, plugin_name, category, tags, description,
               preset_data, file_path, author, is_factory)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (name, plugin_name or None, category, tags, description,
             preset_data, file_path, author, int(is_factory)),
        )
        return _ok(id=cursor.lastrowid, name=name)


@mcp.tool()
def catalog_get_preset(preset_id: int) -> str:
    """Get a preset from the catalog.

    Args:
        preset_id: Preset ID.
    """
    with get_catalog() as conn:
        row = conn.execute("SELECT * FROM presets WHERE id = ?", (preset_id,)).fetchone()
        if row is None:
            return _err("Preset not found")
        return _ok(_row_to_dict(row))


@mcp.tool()
def catalog_list_presets(
    plugin_name: str = "",
    category: str = "",
    author: str = "",
    is_factory: str = "",
    limit: int = 50,
    offset: int = 0,
) -> str:
    """List presets in the catalog with optional filters.

    Args:
        plugin_name: Filter by plugin.
        category: Filter by category.
        author: Filter by author.
        is_factory: Filter by factory status ("true"/"false").
        limit: Max results.
        offset: Results to skip.
    """
    filters: dict[str, Any] = {}
    if plugin_name:
        filters["plugin_name"] = plugin_name
    if category:
        filters["category"] = category
    if author:
        filters["author"] = author
    if is_factory:
        filters["is_factory"] = 1 if is_factory.lower() == "true" else 0
    where, values = _build_where(filters)
    with get_catalog() as conn:
        rows = conn.execute(
            f"SELECT * FROM presets WHERE {where} ORDER BY name LIMIT ? OFFSET ?",  # noqa: S608
            values + [limit, offset],
        ).fetchall()
        total = conn.execute(
            f"SELECT COUNT(*) as cnt FROM presets WHERE {where}", values  # noqa: S608
        ).fetchone()
        return _ok(presets=_rows_to_list(rows), total=total["cnt"])


@mcp.tool()
def catalog_search_presets(query: str, limit: int = 20) -> str:
    """Full-text search across presets in the catalog.

    Args:
        query: Search query (FTS5 syntax: AND, OR, NOT, quotes for phrases).
        limit: Max results.
    """
    with get_catalog() as conn:
        rows = conn.execute(
            """SELECT p.*, rank FROM presets p
               JOIN presets_fts ON p.id = presets_fts.rowid
               WHERE presets_fts MATCH ?
               ORDER BY rank LIMIT ?""",
            (query, limit),
        ).fetchall()
        return _ok(results=_rows_to_list(rows), count=len(rows))


@mcp.tool()
def catalog_update_preset(preset_id: int, updates: str) -> str:
    """Update a preset in the catalog.

    Args:
        preset_id: Preset ID.
        updates: JSON object of field:value pairs.
    """
    data = json.loads(updates)
    allowed = {"name", "plugin_name", "category", "tags", "description",
               "preset_data", "file_path", "author", "is_factory"}
    data = {k: v for k, v in data.items() if k in allowed}
    if not data:
        return _err("No valid updates provided")
    set_clause = ", ".join(f"{k} = ?" for k in data)
    set_clause += ", updated_at = datetime('now')"
    values = list(data.values())
    with get_catalog() as conn:
        conn.execute(
            f"UPDATE presets SET {set_clause} WHERE id = ?",  # noqa: S608
            values + [preset_id],
        )
        return _ok(status="updated", preset_id=preset_id)


@mcp.tool()
def catalog_delete_preset(preset_id: int) -> str:
    """Delete a preset from the catalog.

    Args:
        preset_id: Preset ID.
    """
    with get_catalog() as conn:
        conn.execute("DELETE FROM presets WHERE id = ?", (preset_id,))
        return _ok(status="deleted", preset_id=preset_id)


# --- Catalog: Samples ---

@mcp.tool()
def catalog_create_sample(
    name: str,
    file_path: str,
    format: str = "",
    sample_rate: int = 0,
    channels: int = 0,
    bit_depth: int = 0,
    duration_ms: int = 0,
    file_size: int = 0,
    tags: str = "[]",
    category: str = "",
    description: str = "",
) -> str:
    """Register a sample in the catalog.

    Args:
        name: Sample name.
        file_path: Path to the audio file.
        format: Audio format (wav, flac, ogg, mp3, aiff, raw, other).
        sample_rate: Sample rate in Hz.
        channels: Number of channels.
        bit_depth: Bit depth.
        duration_ms: Duration in milliseconds.
        file_size: File size in bytes.
        tags: JSON array of tag strings.
        category: Category (e.g., "Kick", "Snare", "FX").
        description: Sample description.
    """
    with get_catalog() as conn:
        cursor = conn.execute(
            """INSERT INTO samples (name, file_path, format, sample_rate, channels,
               bit_depth, duration_ms, file_size, tags, category, description)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (name, file_path, format or None, sample_rate or None,
             channels or None, bit_depth or None, duration_ms or None,
             file_size or None, tags, category, description),
        )
        return _ok(id=cursor.lastrowid, name=name)


@mcp.tool()
def catalog_get_sample(sample_id: int) -> str:
    """Get a sample from the catalog.

    Args:
        sample_id: Sample ID.
    """
    with get_catalog() as conn:
        row = conn.execute("SELECT * FROM samples WHERE id = ?", (sample_id,)).fetchone()
        if row is None:
            return _err("Sample not found")
        return _ok(_row_to_dict(row))


@mcp.tool()
def catalog_list_samples(
    format: str = "",
    category: str = "",
    limit: int = 50,
    offset: int = 0,
) -> str:
    """List samples in the catalog with optional filters.

    Args:
        format: Filter by audio format.
        category: Filter by category.
        limit: Max results.
        offset: Results to skip.
    """
    filters: dict[str, Any] = {}
    if format:
        filters["format"] = format
    if category:
        filters["category"] = category
    where, values = _build_where(filters)
    with get_catalog() as conn:
        rows = conn.execute(
            f"SELECT * FROM samples WHERE {where} ORDER BY name LIMIT ? OFFSET ?",  # noqa: S608
            values + [limit, offset],
        ).fetchall()
        total = conn.execute(
            f"SELECT COUNT(*) as cnt FROM samples WHERE {where}", values  # noqa: S608
        ).fetchone()
        return _ok(samples=_rows_to_list(rows), total=total["cnt"])


@mcp.tool()
def catalog_search_samples(query: str, limit: int = 20) -> str:
    """Full-text search across samples in the catalog.

    Args:
        query: Search query (FTS5 syntax).
        limit: Max results.
    """
    with get_catalog() as conn:
        rows = conn.execute(
            """SELECT s.*, rank FROM samples s
               JOIN samples_fts ON s.id = samples_fts.rowid
               WHERE samples_fts MATCH ?
               ORDER BY rank LIMIT ?""",
            (query, limit),
        ).fetchall()
        return _ok(results=_rows_to_list(rows), count=len(rows))


@mcp.tool()
def catalog_update_sample(sample_id: int, updates: str) -> str:
    """Update a sample in the catalog.

    Args:
        sample_id: Sample ID.
        updates: JSON object of field:value pairs.
    """
    data = json.loads(updates)
    allowed = {"name", "file_path", "format", "sample_rate", "channels",
               "bit_depth", "duration_ms", "file_size", "tags", "category", "description"}
    data = {k: v for k, v in data.items() if k in allowed}
    if not data:
        return _err("No valid updates provided")
    set_clause = ", ".join(f"{k} = ?" for k in data)
    set_clause += ", updated_at = datetime('now')"
    values = list(data.values())
    with get_catalog() as conn:
        conn.execute(
            f"UPDATE samples SET {set_clause} WHERE id = ?",  # noqa: S608
            values + [sample_id],
        )
        return _ok(status="updated", sample_id=sample_id)


@mcp.tool()
def catalog_delete_sample(sample_id: int) -> str:
    """Delete a sample from the catalog.

    Args:
        sample_id: Sample ID.
    """
    with get_catalog() as conn:
        conn.execute("DELETE FROM samples WHERE id = ?", (sample_id,))
        return _ok(status="deleted", sample_id=sample_id)


# --- Catalog: Project Index ---

@mcp.tool()
def catalog_index_project(
    name: str,
    file_path: str,
    file_format: str = "lmms-db",
    description: str = "",
    tags: str = "[]",
    bpm: float = 0.0,
    time_sig: str = "4/4",
) -> str:
    """Add a project to the catalog index.

    Args:
        name: Project name.
        file_path: Path to the project file (.lmms-db, .mmp, .mmpz).
        file_format: One of: lmms-db, mmp, mmpz.
        description: Project description.
        tags: JSON array of tag strings.
        bpm: BPM of the project.
        time_sig: Time signature (e.g., "4/4").
    """
    p = Path(file_path)
    file_size = p.stat().st_size if p.exists() else 0
    with get_catalog() as conn:
        cursor = conn.execute(
            """INSERT OR REPLACE INTO project_index
               (name, file_path, file_format, description, tags, bpm, time_sig, file_size)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
            (name, str(p.resolve()), file_format, description, tags, bpm, time_sig, file_size),
        )
        return _ok(id=cursor.lastrowid, name=name)


@mcp.tool()
def catalog_list_projects(limit: int = 50, offset: int = 0) -> str:
    """List projects in the catalog index.

    Args:
        limit: Max results.
        offset: Results to skip.
    """
    with get_catalog() as conn:
        rows = conn.execute(
            "SELECT * FROM project_index ORDER BY updated_at DESC LIMIT ? OFFSET ?",
            (limit, offset),
        ).fetchall()
        total = conn.execute("SELECT COUNT(*) as cnt FROM project_index").fetchone()
        return _ok(projects=_rows_to_list(rows), total=total["cnt"])


@mcp.tool()
def catalog_search_projects(query: str, limit: int = 20) -> str:
    """Full-text search across projects in the catalog index.

    Args:
        query: Search query (FTS5 syntax).
        limit: Max results.
    """
    with get_catalog() as conn:
        rows = conn.execute(
            """SELECT p.*, rank FROM project_index p
               JOIN project_index_fts ON p.id = project_index_fts.rowid
               WHERE project_index_fts MATCH ?
               ORDER BY rank LIMIT ?""",
            (query, limit),
        ).fetchall()
        return _ok(results=_rows_to_list(rows), count=len(rows))


# --- Catalog: Bulk Operations ---

@mcp.tool()
def catalog_scan_plugins() -> str:
    """Register all known LMMS native instruments and effects in the catalog."""
    native_instruments = [
        "AudioFileProcessor", "BitInvader", "CarlaInstrument", "Kicker",
        "LB302", "Mallets", "Monstro", "NES", "OpulenZ", "Organic",
        "FreeBoy", "PatMan", "SF2Player", "SID", "TripleOscillator",
        "VSTInstrument", "Watsyn", "ZynAddSubFX",
    ]
    native_effects = [
        "Amplifier", "BassBooster", "BitcrushEffect", "CrossoverEQ",
        "Delay", "DualFilter", "Dynamics", "Eq", "Flanger", "HydrogenImport",
        "LOMMEffect", "MultitapEcho", "PeakControllerEffect", "ReverbSC",
        "SpectrumAnalyzer", "StereoEnhancer", "StereoMatrix", "VSTEffect",
        "Waveshaper",
    ]
    registered = []
    with get_catalog() as conn:
        for name in native_instruments:
            conn.execute(
                """INSERT OR IGNORE INTO plugins (name, display_name, plugin_type, plugin_format)
                   VALUES (?, ?, 'instrument', 'native')""",
                (name, name),
            )
            registered.append(name)
        for name in native_effects:
            conn.execute(
                """INSERT OR IGNORE INTO plugins (name, display_name, plugin_type, plugin_format)
                   VALUES (?, ?, 'effect', 'native')""",
                (name, name),
            )
            registered.append(name)
    return _ok(registered=registered, count=len(registered))


@mcp.tool()
def catalog_import_samples(
    directory: str,
    category: str = "",
    recursive: bool = True,
) -> str:
    """Scan a directory for audio files and add them to the catalog.

    Args:
        directory: Path to directory to scan.
        category: Category for all found samples.
        recursive: Whether to scan subdirectories.
    """
    audio_extensions = {".wav", ".flac", ".ogg", ".mp3", ".aiff", ".aif", ".raw"}
    ext_to_format = {
        ".wav": "wav", ".flac": "flac", ".ogg": "ogg",
        ".mp3": "mp3", ".aiff": "aiff", ".aif": "aiff", ".raw": "raw",
    }
    base = Path(directory)
    if not base.is_dir():
        return _err(f"Directory not found: {directory}")

    pattern = "**/*" if recursive else "*"
    imported = []
    with get_catalog() as conn:
        for path in base.glob(pattern):
            if path.suffix.lower() not in audio_extensions:
                continue
            fp = str(path.resolve())
            fmt = ext_to_format.get(path.suffix.lower(), "other")
            try:
                file_size = path.stat().st_size
                conn.execute(
                    """INSERT OR IGNORE INTO samples (name, file_path, format, file_size, category)
                       VALUES (?, ?, ?, ?, ?)""",
                    (path.stem, fp, fmt, file_size, category or path.parent.name),
                )
                imported.append(fp)
            except sqlite3.IntegrityError:
                pass
    return _ok(imported=len(imported), files=imported[:100])


@mcp.tool()
def catalog_import_projects(
    directory: str,
    recursive: bool = True,
) -> str:
    """Scan a directory for .mmp/.mmpz/.lmms-db files and index them in the catalog.

    Args:
        directory: Path to directory to scan.
        recursive: Whether to scan subdirectories.
    """
    base = Path(directory)
    if not base.is_dir():
        return _err(f"Directory not found: {directory}")

    ext_to_format = {".mmp": "mmp", ".mmpz": "mmpz", ".lmms-db": "lmms-db"}
    pattern = "**/*" if recursive else "*"
    imported = []
    with get_catalog() as conn:
        for path in base.glob(pattern):
            fmt = ext_to_format.get(path.suffix.lower())
            if fmt is None:
                continue
            fp = str(path.resolve())
            try:
                file_size = path.stat().st_size
                conn.execute(
                    """INSERT OR IGNORE INTO project_index (name, file_path, file_format, file_size)
                       VALUES (?, ?, ?, ?)""",
                    (path.stem, fp, fmt, file_size),
                )
                imported.append(fp)
            except sqlite3.IntegrityError:
                pass
    return _ok(imported=len(imported), files=imported[:100])


@mcp.tool()
def catalog_export(output_path: str = "") -> str:
    """Export the entire catalog as JSON.

    Args:
        output_path: Path to write JSON. If empty, returns data inline.
    """
    with get_catalog() as conn:
        data = {
            "plugins": _rows_to_list(conn.execute("SELECT * FROM plugins").fetchall()),
            "plugin_parameters": _rows_to_list(conn.execute("SELECT * FROM plugin_parameters").fetchall()),
            "presets": _rows_to_list(conn.execute("SELECT * FROM presets").fetchall()),
            "samples": _rows_to_list(conn.execute("SELECT * FROM samples").fetchall()),
            "project_index": _rows_to_list(conn.execute("SELECT * FROM project_index").fetchall()),
            "schema_version": _rows_to_list(conn.execute("SELECT * FROM schema_version").fetchall()),
        }
    if output_path:
        Path(output_path).write_text(json.dumps(data, indent=2))
        return _ok(status="exported", path=output_path)
    return _ok(data)


@mcp.tool()
def catalog_import(input_path: str) -> str:
    """Import data from a JSON export into the catalog.

    Args:
        input_path: Path to the JSON file.
    """
    data = json.loads(Path(input_path).read_text())
    counts: dict[str, int] = {}
    with get_catalog() as conn:
        for table in ("plugins", "plugin_parameters", "presets", "samples", "project_index"):
            rows = data.get(table, [])
            if not rows:
                counts[table] = 0
                continue
            cols = list(rows[0].keys())
            placeholders = ", ".join("?" * len(cols))
            col_names = ", ".join(cols)
            inserted = 0
            for row in rows:
                try:
                    conn.execute(
                        f"INSERT OR IGNORE INTO {table} ({col_names}) VALUES ({placeholders})",  # noqa: S608
                        [row.get(c) for c in cols],
                    )
                    inserted += 1
                except sqlite3.Error:
                    pass
            counts[table] = inserted
    return _ok(status="imported", counts=counts)


@mcp.tool()
def catalog_bulk_tag(
    entity_type: str,
    entity_ids: str,
    tags_to_add: str = "[]",
    tags_to_remove: str = "[]",
) -> str:
    """Add or remove tags from multiple catalog entities at once.

    Args:
        entity_type: One of: presets, samples, project_index.
        entity_ids: JSON array of entity IDs.
        tags_to_add: JSON array of tag strings to add.
        tags_to_remove: JSON array of tag strings to remove.
    """
    valid = {"presets", "samples", "project_index"}
    if entity_type not in valid:
        return _err(f"entity_type must be one of: {', '.join(sorted(valid))}")

    ids = json.loads(entity_ids)
    add_tags = set(json.loads(tags_to_add))
    remove_tags = set(json.loads(tags_to_remove))
    updated = 0
    with get_catalog() as conn:
        for eid in ids:
            row = conn.execute(
                f"SELECT tags FROM {entity_type} WHERE id = ?", (eid,)  # noqa: S608
            ).fetchone()
            if row is None:
                continue
            current = set(json.loads(row["tags"] or "[]"))
            new_tags = (current | add_tags) - remove_tags
            conn.execute(
                f"UPDATE {entity_type} SET tags = ?, updated_at = datetime('now') WHERE id = ?",  # noqa: S608
                (json.dumps(sorted(new_tags)), eid),
            )
            updated += 1
    return _ok(updated=updated)


# ============================================================
# CONVERSION tools (bridge to lmms_convert.py / lmms_export.py)
# ============================================================


@mcp.tool()
def convert_mmp_to_db(mmp_path: str, db_path: str = "") -> str:
    """Convert an .mmp/.mmpz XML project to .lmms-db SQLite format.

    Uses tools/lmms_convert.py for lossless 1:1 conversion. Every attribute
    and child element is preserved via extra_json columns.

    Args:
        mmp_path: Path to the .mmp or .mmpz file.
        db_path: Output .lmms-db path. Defaults to same name with .lmms-db extension.
    """
    convert_script = TOOLS_DIR / "lmms_convert.py"
    if not convert_script.exists():
        return _err(f"lmms_convert.py not found at {convert_script}")
    p = Path(mmp_path)
    if not p.exists():
        return _err(f"File not found: {mmp_path}")
    if not db_path:
        db_path = str(p.with_suffix(".lmms-db"))
    try:
        result = subprocess.run(
            ["python3", str(convert_script), mmp_path, db_path],
            capture_output=True, text=True, timeout=120,
        )
        if result.returncode != 0:
            return _err(f"Conversion failed: {result.stderr}")
        return _ok(
            status="converted",
            input=mmp_path,
            output=db_path,
            stderr=result.stderr[:2000],
        )
    except subprocess.TimeoutExpired:
        return _err("Conversion timed out after 120 seconds")


@mcp.tool()
def convert_db_to_mmp(db_path: str, mmp_path: str = "") -> str:
    """Convert a .lmms-db SQLite project back to .mmp XML format.

    Uses tools/lmms_export.py for lossless round-trip conversion.

    Args:
        db_path: Path to the .lmms-db file.
        mmp_path: Output .mmp path. Defaults to same name with .mmp extension.
    """
    export_script = TOOLS_DIR / "lmms_export.py"
    if not export_script.exists():
        return _err(f"lmms_export.py not found at {export_script}")
    p = Path(db_path)
    if not p.exists():
        return _err(f"File not found: {db_path}")
    if not mmp_path:
        mmp_path = str(p.with_suffix(".mmp"))
    try:
        result = subprocess.run(
            ["python3", str(export_script), db_path, mmp_path],
            capture_output=True, text=True, timeout=120,
        )
        if result.returncode != 0:
            return _err(f"Export failed: {result.stderr}")
        return _ok(
            status="exported",
            input=db_path,
            output=mmp_path,
            stderr=result.stderr[:2000],
        )
    except subprocess.TimeoutExpired:
        return _err("Export timed out after 120 seconds")


# ============================================================
# TRACK TEMPLATE tools
# Extract tracks (with all data) from one project and insert
# into another. Enables "template" workflow: save a track setup
# once, reuse it across projects.
# ============================================================

# The project schema tables associated with each track type
_INSTRUMENT_TRACK_DEPS = [
    # (table, fk_column, parent_table)
    ("midi_clip", "instrument_track_id", "instrument_track"),
    ("effect", None, None),  # special: owner_type='instrument_track'
]


def _extract_mixer_channel(conn: sqlite3.Connection, channel_id: int) -> dict | None:
    """Extract a mixer channel with its effects and routes."""
    channel = conn.execute(
        "SELECT * FROM mixer_channel WHERE id = ?", (channel_id,)
    ).fetchone()
    if channel is None:
        return None
    channel_data = _row_to_dict(channel)
    # Effects on this mixer channel
    effects = conn.execute(
        "SELECT * FROM effect WHERE owner_type = 'mixer_channel' AND owner_id = ? ORDER BY sort_order",
        (channel_id,),
    ).fetchall()
    channel_data["effects"] = _rows_to_list(effects)
    # Outgoing routes (sends from this channel)
    routes = conn.execute(
        "SELECT * FROM mixer_route WHERE from_channel_id = ?", (channel_id,)
    ).fetchall()
    channel_data["routes_out"] = _rows_to_list(routes)
    return channel_data


def _extract_controllers_for_track(
    conn: sqlite3.Connection, owner_type: str, owner_id: int
) -> list[dict]:
    """Extract controller connections and their controllers for a track."""
    connections = conn.execute(
        "SELECT * FROM controller_connection WHERE owner_type = ? AND owner_id = ?",
        (owner_type, owner_id),
    ).fetchall()
    result = []
    for cc in connections:
        cc_dict = dict(cc)
        controller = conn.execute(
            "SELECT * FROM controller WHERE id = ?", (cc["controller_id"],)
        ).fetchone()
        cc_dict["controller"] = _row_to_dict(controller)
        result.append(cc_dict)
    return result


def _extract_instrument_track(conn: sqlite3.Connection, track_id: int) -> dict:
    """Extract a complete instrument track with all associated data."""
    track = conn.execute(
        "SELECT * FROM instrument_track WHERE id = ?", (track_id,)
    ).fetchone()
    if track is None:
        raise ValueError(f"Instrument track {track_id} not found")

    track_data = _row_to_dict(track)

    # MIDI clips and their notes
    clips = conn.execute(
        "SELECT * FROM midi_clip WHERE instrument_track_id = ?", (track_id,)
    ).fetchall()
    clips_data = []
    for clip in clips:
        clip_dict = dict(clip)
        notes = conn.execute(
            "SELECT * FROM note WHERE midi_clip_id = ?", (clip["id"],)
        ).fetchall()
        clip_dict["notes"] = _rows_to_list(notes)
        # Note detuning
        note_ids = [n["id"] for n in notes]
        detunings = []
        if note_ids:
            placeholders = ",".join("?" * len(note_ids))
            detunings = conn.execute(
                f"SELECT * FROM note_detuning WHERE note_id IN ({placeholders})",  # noqa: S608
                note_ids,
            ).fetchall()
        clip_dict["note_detunings"] = _rows_to_list(detunings)
        clips_data.append(clip_dict)

    # Effects on this track
    effects = conn.execute(
        "SELECT * FROM effect WHERE owner_type = 'instrument_track' AND owner_id = ? ORDER BY sort_order",
        (track_id,),
    ).fetchall()

    # Mixer channel (preserves routing)
    mixer_channel = None
    if track_data.get("mixer_channel_id") is not None:
        mixer_channel = _extract_mixer_channel(conn, track_data["mixer_channel_id"])

    # Controller connections
    controllers = _extract_controllers_for_track(conn, "instrument_track", track_id)

    return {
        "type": "instrument_track",
        "track": track_data,
        "midi_clips": clips_data,
        "effects": _rows_to_list(effects),
        "mixer_channel": mixer_channel,
        "controller_connections": controllers,
    }


def _extract_sample_track(conn: sqlite3.Connection, track_id: int) -> dict:
    """Extract a complete sample track with all clips."""
    track = conn.execute(
        "SELECT * FROM sample_track WHERE id = ?", (track_id,)
    ).fetchone()
    if track is None:
        raise ValueError(f"Sample track {track_id} not found")

    track_data = _row_to_dict(track)

    clips = conn.execute(
        "SELECT * FROM sample_clip WHERE sample_track_id = ?", (track_id,)
    ).fetchall()

    effects = conn.execute(
        "SELECT * FROM effect WHERE owner_type = 'sample_track' AND owner_id = ? ORDER BY sort_order",
        (track_id,),
    ).fetchall()

    # Mixer channel (preserves routing)
    mixer_channel = None
    if track_data.get("mixer_channel_id") is not None:
        mixer_channel = _extract_mixer_channel(conn, track_data["mixer_channel_id"])

    return {
        "type": "sample_track",
        "track": track_data,
        "sample_clips": _rows_to_list(clips),
        "effects": _rows_to_list(effects),
        "mixer_channel": mixer_channel,
    }


def _extract_pattern_track(conn: sqlite3.Connection, track_id: int) -> dict:
    """Extract a complete pattern track with pattern clips and referenced patterns."""
    track = conn.execute(
        "SELECT * FROM pattern_track WHERE id = ?", (track_id,)
    ).fetchone()
    if track is None:
        raise ValueError(f"Pattern track {track_id} not found")

    clips = conn.execute(
        "SELECT * FROM pattern_clip WHERE pattern_track_id = ?", (track_id,)
    ).fetchall()

    # Collect referenced patterns
    pattern_ids = list({c["pattern_id"] for c in clips})
    patterns = []
    if pattern_ids:
        placeholders = ",".join("?" * len(pattern_ids))
        patterns = conn.execute(
            f"SELECT * FROM pattern WHERE id IN ({placeholders})",  # noqa: S608
            pattern_ids,
        ).fetchall()

    return {
        "type": "pattern_track",
        "track": _row_to_dict(track),
        "pattern_clips": _rows_to_list(clips),
        "patterns": _rows_to_list(patterns),
    }


def _extract_automation_track(conn: sqlite3.Connection, track_id: int) -> dict:
    """Extract a complete automation track with clips and nodes."""
    track = conn.execute(
        "SELECT * FROM automation_track WHERE id = ?", (track_id,)
    ).fetchone()
    if track is None:
        raise ValueError(f"Automation track {track_id} not found")

    clips = conn.execute(
        "SELECT * FROM automation_clip WHERE automation_track_id = ?", (track_id,)
    ).fetchall()

    clips_data = []
    for clip in clips:
        clip_dict = dict(clip)
        nodes = conn.execute(
            "SELECT * FROM automation_node WHERE automation_clip_id = ?", (clip["id"],)
        ).fetchall()
        targets = conn.execute(
            "SELECT * FROM automation_target WHERE automation_clip_id = ?", (clip["id"],)
        ).fetchall()
        clip_dict["nodes"] = _rows_to_list(nodes)
        clip_dict["targets"] = _rows_to_list(targets)
        clips_data.append(clip_dict)

    return {
        "type": "automation_track",
        "track": _row_to_dict(track),
        "automation_clips": clips_data,
    }


def _next_id(conn: sqlite3.Connection, table: str) -> int:
    """Get the next available ID for a table."""
    row = conn.execute(f"SELECT COALESCE(MAX(id), 0) + 1 as next_id FROM {table}").fetchone()  # noqa: S608
    return row["next_id"]


def _next_sort_order(conn: sqlite3.Connection, table: str) -> int:
    """Get the next sort_order for a table."""
    row = conn.execute(
        f"SELECT COALESCE(MAX(sort_order), -1) + 1 as next_order FROM {table}"  # noqa: S608
    ).fetchone()
    return row["next_order"]


def _insert_row(conn: sqlite3.Connection, table: str, data: dict) -> int:
    """Insert a row into a table, returning the new rowid."""
    cols = list(data.keys())
    placeholders = ", ".join("?" * len(cols))
    col_names = ", ".join(cols)
    cursor = conn.execute(
        f"INSERT INTO {table} ({col_names}) VALUES ({placeholders})",  # noqa: S608
        [data[c] for c in cols],
    )
    return cursor.lastrowid


def _insert_mixer_channel(conn: sqlite3.Connection, channel_data: dict | None) -> int | None:
    """Insert a mixer channel from template, returning new channel ID.

    If the channel already exists (by ID), returns the existing ID.
    """
    if channel_data is None:
        return None
    data = dict(channel_data)
    effects = data.pop("effects", [])
    routes = data.pop("routes_out", [])
    old_id = data.get("id")

    # Check if this channel ID already exists in the target
    existing = conn.execute(
        "SELECT id FROM mixer_channel WHERE id = ?", (old_id,)
    ).fetchone()
    if existing:
        return old_id  # reuse existing channel

    # Create the mixer channel
    new_id = _insert_row(conn, "mixer_channel", data)

    # Insert effects
    for effect in effects:
        effect_data = dict(effect)
        effect_data.pop("id", None)
        effect_data["owner_type"] = "mixer_channel"
        effect_data["owner_id"] = new_id
        effect_data["id"] = _next_id(conn, "effect")
        _insert_row(conn, "effect", effect_data)

    # Insert routes
    for route in routes:
        route_data = dict(route)
        route_data.pop("id", None)
        route_data["from_channel_id"] = new_id
        route_data["id"] = _next_id(conn, "mixer_route")
        _insert_row(conn, "mixer_route", route_data)

    return new_id


def _insert_controllers(
    conn: sqlite3.Connection, connections: list[dict], owner_type: str, owner_id: int
) -> int:
    """Insert controller connections and their controllers. Returns count inserted."""
    count = 0
    for cc in connections:
        cc_data = dict(cc)
        cc_data.pop("id", None)
        controller_data = cc_data.pop("controller", None)

        # Insert the controller if it doesn't exist
        controller_id = cc_data.get("controller_id")
        if controller_data:
            ctrl = dict(controller_data)
            old_ctrl_id = ctrl.pop("id", None)
            # Check if controller already exists
            existing = conn.execute(
                "SELECT id FROM controller WHERE id = ?", (old_ctrl_id,)
            ).fetchone()
            if existing:
                controller_id = old_ctrl_id
            else:
                ctrl["id"] = _next_id(conn, "controller")
                controller_id = _insert_row(conn, "controller", ctrl)

        cc_data["owner_type"] = owner_type
        cc_data["owner_id"] = owner_id
        cc_data["controller_id"] = controller_id
        cc_data["id"] = _next_id(conn, "controller_connection")
        _insert_row(conn, "controller_connection", cc_data)
        count += 1
    return count


def _insert_instrument_track(conn: sqlite3.Connection, template: dict) -> dict:
    """Insert an instrument track template into a project. Returns ID mapping."""
    track_data = dict(template["track"])
    old_track_id = track_data.pop("id", None)

    # Handle mixer channel - insert if provided, remap the FK
    mixer_channel = template.get("mixer_channel")
    if mixer_channel:
        new_mixer_id = _insert_mixer_channel(conn, mixer_channel)
        if new_mixer_id is not None:
            track_data["mixer_channel_id"] = new_mixer_id

    # Assign new ID and sort_order
    track_data["id"] = _next_id(conn, "instrument_track")
    track_data["sort_order"] = _next_sort_order(conn, "instrument_track")
    new_track_id = _insert_row(conn, "instrument_track", track_data)

    clip_id_map = {}
    note_id_map = {}

    for clip in template.get("midi_clips", []):
        clip_data = dict(clip)
        old_clip_id = clip_data.pop("id", None)
        notes = clip_data.pop("notes", [])
        detunings = clip_data.pop("note_detunings", [])

        clip_data["instrument_track_id"] = new_track_id
        clip_data["id"] = _next_id(conn, "midi_clip")
        new_clip_id = _insert_row(conn, "midi_clip", clip_data)
        clip_id_map[old_clip_id] = new_clip_id

        for note in notes:
            note_data = dict(note)
            old_note_id = note_data.pop("id", None)
            note_data["midi_clip_id"] = new_clip_id
            note_data["id"] = _next_id(conn, "note")
            new_note_id = _insert_row(conn, "note", note_data)
            note_id_map[old_note_id] = new_note_id

        for det in detunings:
            det_data = dict(det)
            det_data.pop("id", None)
            old_note_id = det_data["note_id"]
            if old_note_id in note_id_map:
                det_data["note_id"] = note_id_map[old_note_id]
                det_data["id"] = _next_id(conn, "note_detuning")
                _insert_row(conn, "note_detuning", det_data)

    for effect in template.get("effects", []):
        effect_data = dict(effect)
        effect_data.pop("id", None)
        effect_data["owner_id"] = new_track_id
        effect_data["id"] = _next_id(conn, "effect")
        _insert_row(conn, "effect", effect_data)

    # Controller connections
    _insert_controllers(
        conn, template.get("controller_connections", []),
        "instrument_track", new_track_id,
    )

    return {
        "new_track_id": new_track_id,
        "old_track_id": old_track_id,
        "clips_inserted": len(clip_id_map),
        "notes_inserted": len(note_id_map),
    }


def _insert_sample_track(conn: sqlite3.Connection, template: dict) -> dict:
    """Insert a sample track template into a project."""
    track_data = dict(template["track"])
    old_track_id = track_data.pop("id", None)

    # Handle mixer channel
    mixer_channel = template.get("mixer_channel")
    if mixer_channel:
        new_mixer_id = _insert_mixer_channel(conn, mixer_channel)
        if new_mixer_id is not None:
            track_data["mixer_channel_id"] = new_mixer_id

    track_data["id"] = _next_id(conn, "sample_track")
    track_data["sort_order"] = _next_sort_order(conn, "sample_track")
    new_track_id = _insert_row(conn, "sample_track", track_data)

    clips_inserted = 0
    for clip in template.get("sample_clips", []):
        clip_data = dict(clip)
        clip_data.pop("id", None)
        clip_data["sample_track_id"] = new_track_id
        clip_data["id"] = _next_id(conn, "sample_clip")
        _insert_row(conn, "sample_clip", clip_data)
        clips_inserted += 1

    for effect in template.get("effects", []):
        effect_data = dict(effect)
        effect_data.pop("id", None)
        effect_data["owner_id"] = new_track_id
        effect_data["id"] = _next_id(conn, "effect")
        _insert_row(conn, "effect", effect_data)

    return {
        "new_track_id": new_track_id,
        "old_track_id": old_track_id,
        "clips_inserted": clips_inserted,
    }


def _insert_pattern_track(conn: sqlite3.Connection, template: dict) -> dict:
    """Insert a pattern track template into a project."""
    track_data = dict(template["track"])
    old_track_id = track_data.pop("id", None)

    track_data["id"] = _next_id(conn, "pattern_track")
    track_data["sort_order"] = _next_sort_order(conn, "pattern_track")
    new_track_id = _insert_row(conn, "pattern_track", track_data)

    # Insert patterns (if not already present)
    pattern_id_map = {}
    for pattern in template.get("patterns", []):
        pat_data = dict(pattern)
        old_pat_id = pat_data.pop("id", None)
        existing = conn.execute(
            "SELECT id FROM pattern WHERE id = ?", (old_pat_id,)
        ).fetchone()
        if existing:
            pattern_id_map[old_pat_id] = old_pat_id
        else:
            pat_data["id"] = _next_id(conn, "pattern")
            new_pat_id = _insert_row(conn, "pattern", pat_data)
            pattern_id_map[old_pat_id] = new_pat_id

    # Insert pattern clips with remapped IDs
    clips_inserted = 0
    for clip in template.get("pattern_clips", []):
        clip_data = dict(clip)
        clip_data.pop("id", None)
        clip_data["pattern_track_id"] = new_track_id
        old_pat_id = clip_data.get("pattern_id")
        if old_pat_id in pattern_id_map:
            clip_data["pattern_id"] = pattern_id_map[old_pat_id]
        clip_data["id"] = _next_id(conn, "pattern_clip")
        _insert_row(conn, "pattern_clip", clip_data)
        clips_inserted += 1

    return {
        "new_track_id": new_track_id,
        "old_track_id": old_track_id,
        "clips_inserted": clips_inserted,
    }


def _insert_automation_track(conn: sqlite3.Connection, template: dict) -> dict:
    """Insert an automation track template into a project."""
    track_data = dict(template["track"])
    old_track_id = track_data.pop("id", None)

    track_data["id"] = _next_id(conn, "automation_track")
    track_data["sort_order"] = _next_sort_order(conn, "automation_track")
    new_track_id = _insert_row(conn, "automation_track", track_data)

    clips_inserted = 0
    for clip in template.get("automation_clips", []):
        clip_data = dict(clip)
        old_clip_id = clip_data.pop("id", None)
        nodes = clip_data.pop("nodes", [])
        targets = clip_data.pop("targets", [])

        clip_data["automation_track_id"] = new_track_id
        clip_data["id"] = _next_id(conn, "automation_clip")
        new_clip_id = _insert_row(conn, "automation_clip", clip_data)
        clips_inserted += 1

        for node in nodes:
            node_data = dict(node)
            node_data.pop("id", None)
            node_data["automation_clip_id"] = new_clip_id
            node_data["id"] = _next_id(conn, "automation_node")
            _insert_row(conn, "automation_node", node_data)

        for target in targets:
            target_data = dict(target)
            target_data.pop("id", None)
            target_data["automation_clip_id"] = new_clip_id
            target_data["id"] = _next_id(conn, "automation_target")
            _insert_row(conn, "automation_target", target_data)

    return {
        "new_track_id": new_track_id,
        "old_track_id": old_track_id,
        "clips_inserted": clips_inserted,
    }


@mcp.tool()
def extract_track_template(
    db_path: str,
    track_type: str,
    track_id: int,
    output_path: str = "",
) -> str:
    """Extract a complete track from a .lmms-db project as a reusable JSON template.

    Captures everything: track settings, all clips, notes, effects, automation
    nodes, extra_json data - a complete 1:1 copy of the track.

    Args:
        db_path: Path to the source .lmms-db project file.
        track_type: One of: instrument_track, sample_track, automation_track, pattern_track.
        track_id: ID of the track to extract.
        output_path: Optional path to save the template JSON. If empty, returns inline.
    """
    extractors = {
        "instrument_track": _extract_instrument_track,
        "sample_track": _extract_sample_track,
        "automation_track": _extract_automation_track,
        "pattern_track": _extract_pattern_track,
    }
    if track_type not in extractors:
        return _err(f"track_type must be one of: {', '.join(sorted(extractors))}")

    try:
        with open_project(db_path) as conn:
            template = extractors[track_type](conn, track_id)
            template["source_project"] = db_path

        if output_path:
            Path(output_path).write_text(json.dumps(template, indent=2))
            return _ok(status="extracted", path=output_path, track_type=track_type)
        return _ok(template)
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def extract_tracks_bulk(
    db_path: str,
    track_specs: str,
    output_path: str = "",
) -> str:
    """Extract multiple tracks from a project as a single reusable template bundle.

    Args:
        db_path: Path to the source .lmms-db project file.
        track_specs: JSON array of {"type": "instrument_track", "id": 1} objects.
        output_path: Optional path to save the template JSON.
    """
    specs = json.loads(track_specs)
    extractors = {
        "instrument_track": _extract_instrument_track,
        "sample_track": _extract_sample_track,
        "automation_track": _extract_automation_track,
        "pattern_track": _extract_pattern_track,
    }

    try:
        templates = []
        with open_project(db_path) as conn:
            for spec in specs:
                track_type = spec["type"]
                track_id = spec["id"]
                if track_type not in extractors:
                    return _err(f"Unknown track type: {track_type}")
                template = extractors[track_type](conn, track_id)
                templates.append(template)

        bundle = {
            "source_project": db_path,
            "tracks": templates,
            "track_count": len(templates),
        }

        if output_path:
            Path(output_path).write_text(json.dumps(bundle, indent=2))
            return _ok(status="extracted", path=output_path, track_count=len(templates))
        return _ok(bundle)
    except (FileNotFoundError, ValueError) as e:
        return _err(str(e))


@mcp.tool()
def insert_track_template(
    db_path: str,
    template_path: str = "",
    template_json: str = "",
) -> str:
    """Insert a track template into a .lmms-db project.

    Accepts either a file path to a template JSON or inline JSON. Handles both
    single-track templates and multi-track bundles. All IDs are remapped to
    avoid conflicts. All data (notes, effects, clips, extra_json) is preserved.

    Args:
        db_path: Path to the target .lmms-db project file.
        template_path: Path to a template JSON file (from extract_track_template).
        template_json: Inline JSON template string (alternative to template_path).
    """
    if template_path:
        template = json.loads(Path(template_path).read_text())
    elif template_json:
        template = json.loads(template_json)
    else:
        return _err("Provide template_path or template_json")

    inserters = {
        "instrument_track": _insert_instrument_track,
        "sample_track": _insert_sample_track,
        "automation_track": _insert_automation_track,
        "pattern_track": _insert_pattern_track,
    }

    try:
        results = []
        with open_project(db_path) as conn:
            # Handle both single templates and bundles
            if "tracks" in template:
                # Multi-track bundle
                for track_template in template["tracks"]:
                    track_type = track_template["type"]
                    inserter = inserters.get(track_type)
                    if inserter is None:
                        return _err(f"Unknown track type: {track_type}")
                    result = inserter(conn, track_template)
                    result["type"] = track_type
                    results.append(result)
            else:
                # Single track template
                track_type = template["type"]
                inserter = inserters.get(track_type)
                if inserter is None:
                    return _err(f"Unknown track type: {track_type}")
                result = inserter(conn, template)
                result["type"] = track_type
                results.append(result)

        return _ok(status="inserted", tracks=results)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def create_blank_project(db_path: str, name: str = "New Project", bpm: float = 140.0) -> str:
    """Create a new blank .lmms-db project file.

    Creates a minimal project with the standard schema (from tools/lmms_schema.sql)
    that is ready to receive track templates.

    Args:
        db_path: Path for the new .lmms-db file.
        name: Project name.
        bpm: Initial BPM.
    """
    schema_path = TOOLS_DIR / "lmms_schema.sql"
    if not schema_path.exists():
        return _err(f"Project schema not found at {schema_path}")

    p = Path(db_path)
    if p.exists():
        return _err(f"File already exists: {db_path}")
    p.parent.mkdir(parents=True, exist_ok=True)

    try:
        conn = sqlite3.connect(db_path)
        conn.executescript(schema_path.read_text())
        conn.execute(
            """INSERT INTO project (id, name, bpm, timesig_numerator, timesig_denominator,
               master_volume, master_pitch)
               VALUES (1, ?, ?, 4, 4, 100, 0)""",
            (name, bpm),
        )
        # Create default master mixer channel
        conn.execute(
            """INSERT INTO mixer_channel (id, name, volume, sort_order)
               VALUES (0, 'Master', 1.0, 0)"""
        )
        conn.commit()
        conn.close()
        return _ok(status="created", db_path=db_path, name=name, bpm=bpm)
    except sqlite3.Error as e:
        return _err(str(e))


@mcp.tool()
def project_create_instrument_track(
    db_path: str,
    name: str,
    instrument_plugin: str,
    instrument_params_json: str = "{}",
    volume: float = 100.0,
    panning: float = 0.0,
    pitch: float = 0.0,
    mixer_channel_id: int = 0,
    notes: str = "[]",
    effects: str = "[]",
) -> str:
    """Create a new instrument track from scratch in a .lmms-db project.

    This is the high-level tool for "I want to add this instrument with
    these notes and these effects."

    Args:
        db_path: Path to the .lmms-db project file.
        name: Track name.
        instrument_plugin: Plugin name (e.g., "TripleOscillator", "ZynAddSubFX", "Kicker").
        instrument_params_json: JSON object of plugin parameters.
        volume: Track volume (0-200, default 100).
        panning: Track panning (-100 to 100, default 0).
        pitch: Track pitch offset.
        mixer_channel_id: Mixer channel to route to (0 = master).
        notes: JSON array of note objects. Each note: {"position": int (ticks),
               "length": int (ticks), "key": int (0-127, 69=A4),
               "volume": int (0-127, default 100), "panning": int (-100 to 100, default 0)}.
               LMMS uses 192 ticks per beat.
        effects: JSON array of effect objects. Each effect:
                {"plugin_name": str, "params_json": str (JSON), "enabled": bool (default true),
                 "wet": float (0-1, default 1), "gate": float (default 0)}.
    """
    try:
        note_list = json.loads(notes)
        effect_list = json.loads(effects)

        # Normalize plugin name to lowercase internal form
        instrument_plugin = normalize_plugin_name(instrument_plugin)

        # Merge user-provided instrument params over plugin defaults
        user_params = json.loads(instrument_params_json)
        plugin_defaults = get_instrument_defaults(instrument_plugin)
        merged_instrument_params = json.dumps(
            merge_params(plugin_defaults, user_params)
        )

        # Get track subsystem defaults (sound shaping, arpeggio, etc.)
        subsystems = get_track_subsystem_defaults()

        with open_project(db_path) as conn:
            track_id = _next_id(conn, "instrument_track")
            sort_order = _next_sort_order(conn, "instrument_track")

            # Create the instrument track with all subsystem defaults
            conn.execute(
                """INSERT INTO instrument_track
                   (id, name, volume, panning, pitch, mixer_channel_id,
                    instrument_plugin, instrument_params_json, sort_order,
                    sound_shaping_json, arpeggio_json, chord_creator_json,
                    midi_port_json, track_extra_json,
                    instrumenttrack_extra_json)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (track_id, name, volume, panning, pitch,
                 mixer_channel_id or None, instrument_plugin,
                 merged_instrument_params, sort_order,
                 subsystems["sound_shaping_json"],
                 subsystems["arpeggio_json"],
                 subsystems["chord_creator_json"],
                 subsystems["midi_port_json"],
                 subsystems["track_extra_json"],
                 subsystems["instrumenttrack_extra_json"]),
            )

            # Create a MIDI clip for the notes (if any)
            notes_inserted = 0
            if note_list:
                clip_id = _next_id(conn, "midi_clip")
                # Get project's first pattern ID (or create one)
                pattern = conn.execute(
                    "SELECT id FROM pattern ORDER BY sort_order LIMIT 1"
                ).fetchone()
                if pattern is None:
                    pattern_id = _next_id(conn, "pattern")
                    conn.execute(
                        "INSERT INTO pattern (id, name, sort_order) VALUES (?, 'Pattern 0', 0)",
                        (pattern_id,),
                    )
                else:
                    pattern_id = pattern["id"]

                conn.execute(
                    """INSERT INTO midi_clip (id, instrument_track_id, pattern_id,
                       clip_type, steps)
                       VALUES (?, ?, ?, 1, 32)""",
                    (clip_id, track_id, pattern_id),
                )

                for note in note_list:
                    note_id = _next_id(conn, "note")
                    conn.execute(
                        """INSERT INTO note (id, midi_clip_id, position, length, key, volume, panning)
                           VALUES (?, ?, ?, ?, ?, ?, ?)""",
                        (note_id, clip_id, note["position"], note["length"],
                         note["key"], note.get("volume", 100), note.get("panning", 0)),
                    )
                    notes_inserted += 1

            # Add effects with proper defaults
            effects_inserted = 0
            for i, eff in enumerate(effect_list):
                effect_id = _next_id(conn, "effect")
                eff_plugin = normalize_plugin_name(eff["plugin_name"])
                eff_user_params = json.loads(eff.get("params_json", "{}"))
                eff_defaults = get_effect_defaults(eff_plugin)
                eff_merged = json.dumps(merge_params(eff_defaults, eff_user_params))
                conn.execute(
                    """INSERT INTO effect (id, owner_type, owner_id, plugin_name,
                       sort_order, enabled, wet, gate, params_json)
                       VALUES (?, 'instrument_track', ?, ?, ?, ?, ?, ?, ?)""",
                    (effect_id, track_id, eff_plugin, i,
                     1 if eff.get("enabled", True) else 0,
                     eff.get("wet", 1.0), eff.get("gate", 0.0),
                     eff_merged),
                )
                effects_inserted += 1

            return _ok(
                track_id=track_id,
                name=name,
                instrument=instrument_plugin,
                notes_inserted=notes_inserted,
                effects_inserted=effects_inserted,
            )
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def project_add_effect(
    db_path: str,
    owner_type: str,
    owner_id: int,
    plugin_name: str,
    params_json: str = "{}",
    enabled: bool = True,
    wet: float = 1.0,
    gate: float = 0.0,
) -> str:
    """Add an effect to a track or mixer channel in a .lmms-db project.

    Args:
        db_path: Path to the .lmms-db project file.
        owner_type: What to attach the effect to: instrument_track, sample_track, mixer_channel.
        owner_id: ID of the track or mixer channel.
        plugin_name: Effect plugin name (e.g., "ReverbSC", "Delay", "Eq").
        params_json: JSON object of effect parameters.
        enabled: Whether the effect is enabled.
        wet: Wet/dry mix (0.0 to 1.0).
        gate: Gate threshold.
    """
    valid_owners = {"instrument_track", "sample_track", "mixer_channel"}
    if owner_type not in valid_owners:
        return _err(f"owner_type must be one of: {', '.join(sorted(valid_owners))}")
    try:
        # Normalize plugin name and merge params over defaults
        plugin_name = normalize_plugin_name(plugin_name)
        user_params = json.loads(params_json)
        eff_defaults = get_effect_defaults(plugin_name)
        merged_params = json.dumps(merge_params(eff_defaults, user_params))

        with open_project(db_path) as conn:
            effect_id = _next_id(conn, "effect")
            sort_order = conn.execute(
                "SELECT COALESCE(MAX(sort_order), -1) + 1 as n FROM effect WHERE owner_type = ? AND owner_id = ?",
                (owner_type, owner_id),
            ).fetchone()["n"]
            conn.execute(
                """INSERT INTO effect (id, owner_type, owner_id, plugin_name,
                   sort_order, enabled, wet, gate, params_json)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (effect_id, owner_type, owner_id, plugin_name,
                 sort_order, int(enabled), wet, gate, merged_params),
            )
            return _ok(effect_id=effect_id, plugin_name=plugin_name, owner_type=owner_type)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def project_set_bpm(db_path: str, bpm: float) -> str:
    """Set the BPM of a .lmms-db project.

    Args:
        db_path: Path to the .lmms-db project file.
        bpm: New BPM value.
    """
    try:
        with open_project(db_path) as conn:
            conn.execute("UPDATE project SET bpm = ?, modified_at = datetime('now') WHERE id = 1", (bpm,))
            return _ok(status="updated", bpm=bpm)
    except (FileNotFoundError, ValueError, sqlite3.Error) as e:
        return _err(str(e))


@mcp.tool()
def import_project_file(
    file_path: str,
    target_db_path: str = "",
) -> str:
    """Import any LMMS project file (.mmp, .mmpz, or .lmms-db) into a .lmms-db.

    Handles legacy/old .mmp files gracefully by auto-converting through
    lmms_convert.py. If the file is already .lmms-db, copies it to the target path.

    Args:
        file_path: Path to the source project file (.mmp, .mmpz, or .lmms-db).
        target_db_path: Optional target .lmms-db path. Defaults to same name with .lmms-db extension.
    """
    p = Path(file_path)
    if not p.exists():
        return _err(f"File not found: {file_path}")

    if not target_db_path:
        target_db_path = str(p.with_suffix(".lmms-db"))

    target = Path(target_db_path)

    if p.suffix == ".lmms-db":
        if str(p.resolve()) != str(target.resolve()):
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(p), str(target))
        return _ok(status="ready", db_path=target_db_path, format="lmms-db")

    if p.suffix in (".mmp", ".mmpz"):
        convert_script = TOOLS_DIR / "lmms_convert.py"
        if not convert_script.exists():
            return _err(f"lmms_convert.py not found at {convert_script}")
        try:
            result = subprocess.run(
                ["python3", str(convert_script), file_path, target_db_path],
                capture_output=True, text=True, timeout=120,
            )
            if result.returncode != 0:
                return _err(f"Conversion failed: {result.stderr[:500]}")
            return _ok(
                status="converted",
                source=file_path,
                db_path=target_db_path,
                format=p.suffix.lstrip("."),
            )
        except subprocess.TimeoutExpired:
            return _err("Conversion timed out")

    return _err(f"Unsupported file format: {p.suffix}")


@mcp.tool()
def clone_project(source_path: str, dest_path: str) -> str:
    """Clone a .lmms-db project file.

    Creates an exact copy of the project database.

    Args:
        source_path: Path to the source .lmms-db file.
        dest_path: Path for the cloned .lmms-db file.
    """
    src = Path(source_path)
    dst = Path(dest_path)
    if not src.exists():
        return _err(f"Source not found: {source_path}")
    if dst.exists():
        return _err(f"Destination already exists: {dest_path}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(src), str(dst))
    return _ok(status="cloned", source=source_path, dest=dest_path)


# ============================================================
# Entry point
# ============================================================

if __name__ == "__main__":
    mcp.run()
