#!/usr/bin/env python3
"""
lmms_convert.py — Convert LMMS .mmp/.mmpz XML projects to SQLite format.

Usage:
    python3 lmms_convert.py input.mmp [output.lmms-db]
    python3 lmms_convert.py input.mmpz [output.lmms-db]

If output path is omitted, uses the input filename with .lmms-db extension.

The converter reads the XML DOM and populates a SQLite database following
the schema in lmms_schema.sql. It handles:
- PatternStore instrument tracks and their midiclips/notes
- Pattern tracks with patternclips on the Song Editor timeline
- Song-level InstrumentTracks (type="0" in Song) → converted to new PatternTracks
- Mixer channels with effects chains and routing
- Automation tracks/clips with time nodes and object targets
- Sample tracks/clips
- Legacy tag name compatibility (bbtco→patternclip, pattern→midiclip, etc.)
"""

import sys
import os
import zlib
import json
import sqlite3
import xml.etree.ElementTree as ET
from pathlib import Path


# Legacy tag name mappings (old → new)
LEGACY_TAG_MAP = {
    "automationpattern": "automationclip",
    "bbtco": "patternclip",
    "pattern": "midiclip",
    "sampletco": "sampleclip",
}

# Default pattern length in ticks (used for pattern index calculation)
DEFAULT_PATTERN_LENGTH = 192


def log(msg):
    """Log a message to stderr."""
    print(f"[lmms_convert] {msg}", file=sys.stderr)


def read_mmp_file(path):
    """Read an .mmp or .mmpz file and return the XML root element."""
    path = Path(path)
    log(f"Reading {path}")

    if path.suffix.lower() == ".mmpz":
        with open(path, "rb") as f:
            # .mmpz files have a 4-byte header (data length) followed by gzip data
            header = f.read(4)
            compressed = f.read()
        log(f"Decompressing .mmpz ({len(compressed)} bytes compressed)")
        xml_data = zlib.decompress(compressed)
        log(f"Decompressed to {len(xml_data)} bytes")
        root = ET.fromstring(xml_data)
    else:
        tree = ET.parse(str(path))
        root = tree.getroot()

    log(f"Project version: {root.get('version', '?')}, creator: {root.get('creatorversion', '?')}")
    return root


def normalize_tag(tag):
    """Normalize a legacy XML tag name to the current name."""
    return LEGACY_TAG_MAP.get(tag, tag)


def elem_to_json(elem):
    """Convert an XML element and all its children to a JSON-serializable dict.

    Attributes become key-value pairs. Child elements become nested dicts.
    If multiple children have the same tag, they become a list.
    """
    result = dict(elem.attrib)
    for child in elem:
        child_data = elem_to_json(child)
        tag = child.tag
        if tag in result:
            # Convert to list if not already
            if not isinstance(result[tag], list):
                result[tag] = [result[tag]]
            result[tag].append(child_data)
        else:
            result[tag] = child_data
    return result


def create_database(db_path):
    """Create a new SQLite database with the schema."""
    schema_path = Path(__file__).parent / "lmms_schema.sql"
    if not schema_path.exists():
        log(f"ERROR: Schema file not found at {schema_path}")
        sys.exit(1)

    if os.path.exists(db_path):
        os.remove(db_path)
        log(f"Removed existing database at {db_path}")

    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")

    schema_sql = schema_path.read_text()
    conn.executescript(schema_sql)
    log(f"Created database at {db_path}")
    return conn


def convert_project_metadata(conn, root):
    """Extract <head> attributes into the project table."""
    head = root.find(".//head")
    if head is None:
        log("WARNING: No <head> element found, using defaults")
        conn.execute("INSERT INTO project (id) VALUES (1)")
        return

    bpm = float(head.get("bpm", "140"))
    ts_num = int(head.get("timesig_numerator", "4"))
    ts_den = int(head.get("timesig_denominator", "4"))
    master_vol = float(head.get("mastervol", "100"))
    master_pitch = float(head.get("masterpitch", "0"))

    conn.execute(
        "INSERT INTO project (id, bpm, timesig_numerator, timesig_denominator, master_volume, master_pitch) "
        "VALUES (1, ?, ?, ?, ?, ?)",
        (bpm, ts_num, ts_den, master_vol, master_pitch),
    )
    log(f"Project: bpm={bpm}, time_sig={ts_num}/{ts_den}, vol={master_vol}, pitch={master_pitch}")


def convert_mixer(conn, root):
    """Extract mixer channels and routing."""
    mixer_channels = root.findall(".//mixerchannel")
    if not mixer_channels:
        # Try legacy tag name
        mixer_channels = root.findall(".//fxchannel")
    if not mixer_channels:
        log("No mixer channels found")
        return

    channel_count = 0
    route_count = 0
    effect_count = 0
    deferred_routes = []  # (from_ch, to_ch, amount) — defer until all channels exist

    for ch_elem in mixer_channels:
        ch_num = int(ch_elem.get("num", "0"))
        ch_name = ch_elem.get("name", "")
        ch_volume = float(ch_elem.get("volume", "1.0"))
        ch_muted = int(ch_elem.get("muted", "0"))
        ch_soloed = int(ch_elem.get("soloed", "0"))

        conn.execute(
            "INSERT INTO mixer_channel (id, name, volume, muted, soloed, sort_order) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (ch_num, ch_name, ch_volume, ch_muted, ch_soloed, ch_num),
        )
        channel_count += 1

        # Collect sends for deferred insertion
        for send_elem in ch_elem.findall("send"):
            to_channel = int(send_elem.get("channel", "0"))
            amount = float(send_elem.get("amount", "1.0"))
            deferred_routes.append((ch_num, to_channel, amount))

        # Extract effects chain
        effect_count += convert_fxchain(conn, ch_elem, "mixer_channel", ch_num)

    # Insert routes now that all channels exist
    for from_ch, to_ch, amount in deferred_routes:
        conn.execute(
            "INSERT INTO mixer_route (from_channel_id, to_channel_id, amount) VALUES (?, ?, ?)",
            (from_ch, to_ch, amount),
        )
        route_count += 1

    log(f"Mixer: {channel_count} channels, {route_count} routes, {effect_count} effects")


def convert_fxchain(conn, parent_elem, owner_type, owner_id):
    """Extract effects from an <fxchain> element. Returns count of effects inserted."""
    fxchain = parent_elem.find("fxchain")
    if fxchain is None:
        # Also check inside instrumenttrack/sampletrack settings nodes
        for settings_tag in ("instrumenttrack", "sampletrack"):
            settings = parent_elem.find(settings_tag)
            if settings is not None:
                fxchain = settings.find("fxchain")
                if fxchain is not None:
                    break

    if fxchain is None:
        return 0

    count = 0
    for idx, fx_elem in enumerate(fxchain.findall("effect")):
        plugin_name = fx_elem.get("name", "unknown")
        enabled = int(fx_elem.get("on", "1"))
        wet = float(fx_elem.get("wet", "1.0"))
        gate = float(fx_elem.get("gate", "0.0"))

        # Collect all plugin-specific parameters as JSON
        params = {}
        for child in fx_elem:
            if child.tag == "key":
                # <key> contains plugin identification attributes
                params["_key"] = elem_to_json(child)
            else:
                params[child.tag] = elem_to_json(child)

        conn.execute(
            "INSERT INTO effect (owner_type, owner_id, plugin_name, sort_order, enabled, wet, gate, params_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (owner_type, owner_id, plugin_name, idx, enabled, wet, gate, json.dumps(params)),
        )
        count += 1

    return count


def extract_instrument_track_data(track_elem):
    """Extract instrument track settings from a <track type='0'> element.

    Returns a dict with all the fields needed for the instrument_track table.
    """
    it_elem = track_elem.find("instrumenttrack")
    if it_elem is None:
        return None

    data = {
        "name": track_elem.get("name", ""),
        "muted": int(track_elem.get("muted", "0")),
        "solo": int(track_elem.get("solo", "0")),
        "volume": float(it_elem.get("vol", "100")),
        "panning": float(it_elem.get("pan", "0")),
        "pitch": float(it_elem.get("pitch", "0")),
        "pitch_range": int(it_elem.get("pitchrange", "1")),
        "mixer_channel_id": int(it_elem.get("mixch", it_elem.get("fxch", "0"))) if (it_elem.get("mixch") or it_elem.get("fxch")) else None,
        "base_note": int(it_elem.get("basenote", "69")),
        "use_master_pitch": int(it_elem.get("usemasterpitch", "1")),
        "color": track_elem.get("color"),
    }

    # Instrument plugin
    instrument = it_elem.find("instrument")
    if instrument is not None:
        data["instrument_plugin"] = instrument.get("name", "unknown")
        # The plugin's own element is the first child of <instrument>
        plugin_elem = None
        for child in instrument:
            if child.tag != "key":
                plugin_elem = child
                break
        data["instrument_params_json"] = json.dumps(elem_to_json(plugin_elem)) if plugin_elem else "{}"
    else:
        data["instrument_plugin"] = "unknown"
        data["instrument_params_json"] = "{}"

    # Sound shaping (eldata)
    eldata = it_elem.find("eldata")
    data["sound_shaping_json"] = json.dumps(elem_to_json(eldata)) if eldata is not None else "{}"

    # Arpeggiator
    arp = it_elem.find("arpeggiator")
    data["arpeggio_json"] = json.dumps(elem_to_json(arp)) if arp is not None else "{}"

    # Chord creator
    chord = it_elem.find("chordcreator")
    data["chord_creator_json"] = json.dumps(elem_to_json(chord)) if chord is not None else "{}"

    # MIDI port
    midi = it_elem.find("midiport")
    data["midi_port_json"] = json.dumps(elem_to_json(midi)) if midi is not None else "{}"

    # Microtuner (scale/keymap references)
    microtuner = it_elem.find("microtuner")
    data["microtuner_json"] = json.dumps(elem_to_json(microtuner)) if microtuner is not None else "{}"

    return data


def insert_instrument_track(conn, data, sort_order):
    """Insert an instrument track row and return its ID."""
    cursor = conn.execute(
        "INSERT INTO instrument_track "
        "(name, volume, panning, pitch, pitch_range, mixer_channel_id, base_note, "
        "use_master_pitch, muted, solo, color, sort_order, "
        "instrument_plugin, instrument_params_json, sound_shaping_json, "
        "arpeggio_json, chord_creator_json, midi_port_json, microtuner_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            data["name"],
            data["volume"],
            data["panning"],
            data["pitch"],
            data["pitch_range"],
            data["mixer_channel_id"],
            data["base_note"],
            data["use_master_pitch"],
            data["muted"],
            data["solo"],
            data["color"],
            sort_order,
            data["instrument_plugin"],
            data["instrument_params_json"],
            data["sound_shaping_json"],
            data["arpeggio_json"],
            data["chord_creator_json"],
            data["midi_port_json"],
            data["microtuner_json"],
        ),
    )
    return cursor.lastrowid


def convert_notes(conn, clip_elem, midi_clip_id):
    """Extract <note> elements from a midiclip and insert into the note table.

    Returns the number of notes inserted.
    """
    count = 0
    for note_elem in clip_elem.findall("note"):
        pos = int(note_elem.get("pos", "0"))
        length = int(note_elem.get("len", "0"))
        key = int(note_elem.get("key", "69"))
        volume = int(note_elem.get("vol", "100"))
        panning = int(note_elem.get("pan", "0"))
        note_type = int(note_elem.get("type", "0"))

        note_cursor = conn.execute(
            "INSERT INTO note (midi_clip_id, position, length, key, volume, panning, note_type) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (midi_clip_id, pos, length, key, volume, panning, note_type),
        )

        # Check for detuning automation on this note
        detuning = note_elem.find("detuning")
        if detuning is not None:
            note_id = note_cursor.lastrowid
            for auto_elem in detuning.findall("automationpattern"):
                for time_elem in auto_elem.findall("time"):
                    t_pos = int(time_elem.get("pos", "0"))
                    t_value = float(time_elem.get("value", "0"))
                    t_out_value = time_elem.get("outValue")
                    t_in_tan = float(time_elem.get("inTan", "0"))
                    t_out_tan = float(time_elem.get("outTan", "0"))
                    conn.execute(
                        "INSERT INTO note_detuning (note_id, position, value, out_value, in_tangent, out_tangent) "
                        "VALUES (?, ?, ?, ?, ?, ?)",
                        (note_id, t_pos, t_value,
                         float(t_out_value) if t_out_value is not None else None,
                         t_in_tan, t_out_tan),
                    )

        count += 1
    return count


def find_clip_elements(track_elem, tag_name):
    """Find clip elements by tag name, handling legacy names."""
    results = list(track_elem.findall(tag_name))
    # Also check legacy tag names
    for old_tag, new_tag in LEGACY_TAG_MAP.items():
        if new_tag == tag_name:
            results.extend(track_elem.findall(old_tag))
    return results


def convert_patternstore(conn, patternstore_elem, pattern_id_map):
    """Convert the PatternStore's instrument tracks and their midiclips.

    Args:
        conn: SQLite connection
        patternstore_elem: The <trackcontainer type="patternstore"> element
        pattern_id_map: Dict mapping old pattern index → new pattern.id (populated here)

    Returns:
        (instrument_track_count, midi_clip_count, note_count)
    """
    it_count = 0
    mc_count = 0
    note_count = 0

    # First pass: discover all pattern indices used by any midiclip
    all_pattern_indices = set()
    for track_elem in patternstore_elem.findall("track"):
        for clip_elem in find_clip_elements(track_elem, "midiclip"):
            pos = int(clip_elem.get("pos", "0"))
            pattern_idx = pos // DEFAULT_PATTERN_LENGTH
            all_pattern_indices.add(pattern_idx)

    # Create pattern rows for all discovered indices
    for idx in sorted(all_pattern_indices):
        cursor = conn.execute(
            "INSERT INTO pattern (name, sort_order) VALUES (?, ?)",
            (f"Pattern {idx}", idx),
        )
        pattern_id_map[idx] = cursor.lastrowid

    log(f"PatternStore: {len(all_pattern_indices)} patterns discovered")

    # Second pass: convert instrument tracks and their midiclips
    for sort_order, track_elem in enumerate(patternstore_elem.findall("track")):
        if track_elem.get("type") != "0":
            log(f"WARNING: Non-instrument track type={track_elem.get('type')} in patternstore, skipping")
            continue

        data = extract_instrument_track_data(track_elem)
        if data is None:
            log(f"WARNING: Track '{track_elem.get('name', '?')}' has no <instrumenttrack>, skipping")
            continue

        it_id = insert_instrument_track(conn, data, sort_order)

        # Also extract effects from the instrumenttrack element
        it_elem = track_elem.find("instrumenttrack")
        if it_elem is not None:
            fxchain = it_elem.find("fxchain")
            if fxchain is not None:
                fx_count = 0
                for fx_idx, fx_elem in enumerate(fxchain.findall("effect")):
                    plugin_name = fx_elem.get("name", "unknown")
                    enabled = int(fx_elem.get("on", "1"))
                    wet = float(fx_elem.get("wet", "1.0"))
                    gate = float(fx_elem.get("gate", "0.0"))
                    params = {}
                    for child in fx_elem:
                        if child.tag == "key":
                            params["_key"] = elem_to_json(child)
                        else:
                            params[child.tag] = elem_to_json(child)
                    conn.execute(
                        "INSERT INTO effect (owner_type, owner_id, plugin_name, sort_order, enabled, wet, gate, params_json) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                        ("instrument_track", it_id, plugin_name, fx_idx, enabled, wet, gate, json.dumps(params)),
                    )
                    fx_count += 1

        it_count += 1

        # Convert midiclips for this instrument track
        for clip_elem in find_clip_elements(track_elem, "midiclip"):
            pos = int(clip_elem.get("pos", "0"))
            pattern_idx = pos // DEFAULT_PATTERN_LENGTH
            pattern_id = pattern_id_map.get(pattern_idx)
            if pattern_id is None:
                log(f"WARNING: midiclip at pos={pos} maps to unknown pattern index {pattern_idx}")
                continue

            clip_type = int(clip_elem.get("type", "1"))
            steps = int(clip_elem.get("steps", "32"))
            muted = int(clip_elem.get("muted", clip_elem.get("mute", "0")))
            clip_name = clip_elem.get("name", "")
            clip_color = clip_elem.get("color")

            cursor = conn.execute(
                "INSERT OR IGNORE INTO midi_clip "
                "(instrument_track_id, pattern_id, clip_type, steps, muted, name, color) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (it_id, pattern_id, clip_type, steps, muted, clip_name, clip_color),
            )
            mc_id = cursor.lastrowid
            if mc_id == 0:
                # UNIQUE constraint hit — shouldn't happen with proper data
                log(f"WARNING: Duplicate midi_clip for track {it_id}, pattern {pattern_id}")
                row = conn.execute(
                    "SELECT id FROM midi_clip WHERE instrument_track_id=? AND pattern_id=?",
                    (it_id, pattern_id),
                ).fetchone()
                mc_id = row[0] if row else None
                if mc_id is None:
                    continue

            mc_count += 1
            note_count += convert_notes(conn, clip_elem, mc_id)

    log(f"PatternStore: {it_count} instrument tracks, {mc_count} midi clips, {note_count} notes")
    return it_count, mc_count, note_count


def convert_pattern_tracks(conn, song_tc, pattern_id_map):
    """Convert Pattern tracks (type="1") from the Song trackcontainer.

    Each pattern track gets:
    - A pattern_track row
    - pattern_clip rows for any patternclips it has

    The pattern_id for each patternclip is determined by the track's position
    (sort order) among pattern tracks, since in the XML format, the Nth pattern
    track corresponds to pattern index N.

    Returns (pattern_track_count, pattern_clip_count)
    """
    pt_count = 0
    pc_count = 0

    pattern_track_idx = 0
    for track_elem in song_tc.findall("track"):
        if track_elem.get("type") != "1":
            continue

        name = track_elem.get("name", f"Pattern Track {pattern_track_idx}")
        muted = int(track_elem.get("muted", "0"))
        solo = int(track_elem.get("solo", "0"))
        color = track_elem.get("color")

        # The pattern_id for this track's clips is its index among pattern tracks
        # This needs to map to the same pattern IDs created in convert_patternstore
        this_pattern_id = pattern_id_map.get(pattern_track_idx)

        cursor = conn.execute(
            "INSERT INTO pattern_track (name, muted, solo, color, sort_order) VALUES (?, ?, ?, ?, ?)",
            (name, muted, solo, color, pattern_track_idx),
        )
        pt_id = cursor.lastrowid
        pt_count += 1

        # Convert patternclips
        for clip_elem in find_clip_elements(track_elem, "patternclip"):
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_len = int(clip_elem.get("len", "0"))
            clip_off = int(clip_elem.get("off", "0"))
            clip_muted = int(clip_elem.get("muted", "0"))
            clip_name = clip_elem.get("name", "")
            clip_color = clip_elem.get("color")

            if this_pattern_id is None:
                # Pattern index not found — create one
                pcursor = conn.execute(
                    "INSERT INTO pattern (name, sort_order) VALUES (?, ?)",
                    (f"Pattern {pattern_track_idx}", pattern_track_idx),
                )
                this_pattern_id = pcursor.lastrowid
                pattern_id_map[pattern_track_idx] = this_pattern_id

            conn.execute(
                "INSERT INTO pattern_clip "
                "(pattern_track_id, pattern_id, position, length, start_offset, muted, name, color) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (pt_id, this_pattern_id, clip_pos, clip_len, clip_off, clip_muted, clip_name, clip_color),
            )
            pc_count += 1

        pattern_track_idx += 1

    log(f"Pattern tracks: {pt_count} tracks, {pc_count} clips")
    return pt_count, pc_count


def convert_song_level_instrument_tracks(conn, song_tc, pattern_id_map):
    """Convert Song-level InstrumentTracks (type="0" directly in Song) to PatternTracks.

    These tracks don't belong to the PatternStore — they're a legacy/advanced feature.
    We convert them by:
    1. Creating a new instrument_track in the PatternStore
    2. Creating a new pattern for each such track
    3. Creating a new pattern_track in the Song
    4. Moving any midiclips into the new pattern
    5. Creating patternclips that reference the new pattern

    Returns (converted_count)
    """
    converted = 0

    # Count existing patterns to know where to start new ones
    max_pattern_idx = max(pattern_id_map.keys()) if pattern_id_map else -1
    # Count existing pattern tracks for sort_order
    existing_pt_count = conn.execute("SELECT COUNT(*) FROM pattern_track").fetchone()[0]
    # Count existing instrument tracks for sort_order
    existing_it_count = conn.execute("SELECT COUNT(*) FROM instrument_track").fetchone()[0]

    for track_elem in song_tc.findall("track"):
        if track_elem.get("type") != "0":
            continue

        track_name = track_elem.get("name", "Converted Track")
        log(f"Converting song-level InstrumentTrack '{track_name}' to PatternTrack")

        # 1. Create new pattern
        max_pattern_idx += 1
        new_pattern_idx = max_pattern_idx
        pcursor = conn.execute(
            "INSERT INTO pattern (name, sort_order) VALUES (?, ?)",
            (f"Pattern {new_pattern_idx} (from {track_name})", new_pattern_idx),
        )
        new_pattern_id = pcursor.lastrowid
        pattern_id_map[new_pattern_idx] = new_pattern_id

        # 2. Create new instrument track
        data = extract_instrument_track_data(track_elem)
        if data is None:
            log(f"WARNING: Song-level track '{track_name}' has no <instrumenttrack>, skipping")
            continue

        it_id = insert_instrument_track(conn, data, existing_it_count + converted)

        # Also convert effects
        it_elem = track_elem.find("instrumenttrack")
        if it_elem is not None:
            fxchain = it_elem.find("fxchain")
            if fxchain is not None:
                for fx_idx, fx_elem in enumerate(fxchain.findall("effect")):
                    plugin_name = fx_elem.get("name", "unknown")
                    enabled = int(fx_elem.get("on", "1"))
                    wet = float(fx_elem.get("wet", "1.0"))
                    gate = float(fx_elem.get("gate", "0.0"))
                    params = {}
                    for child in fx_elem:
                        if child.tag == "key":
                            params["_key"] = elem_to_json(child)
                        else:
                            params[child.tag] = elem_to_json(child)
                    conn.execute(
                        "INSERT INTO effect (owner_type, owner_id, plugin_name, sort_order, enabled, wet, gate, params_json) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                        ("instrument_track", it_id, plugin_name, fx_idx, enabled, wet, gate, json.dumps(params)),
                    )

        # 3. Create new pattern track
        ptcursor = conn.execute(
            "INSERT INTO pattern_track (name, muted, solo, color, sort_order) VALUES (?, ?, ?, ?, ?)",
            (track_name, data["muted"], data["solo"], data["color"], existing_pt_count + converted),
        )
        pt_id = ptcursor.lastrowid

        # 4. Convert midiclips — each gets its own pattern since song-level tracks
        #    can have multiple independent clips at different timeline positions
        midiclips = find_clip_elements(track_elem, "midiclip")
        note_count = 0
        for clip_idx, clip_elem in enumerate(midiclips):
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_type = int(clip_elem.get("type", "1"))
            steps = int(clip_elem.get("steps", "32"))
            muted = int(clip_elem.get("muted", clip_elem.get("mute", "0")))
            clip_name = clip_elem.get("name", "")

            # Each midiclip in a song-level track gets its own pattern
            if clip_idx == 0:
                clip_pattern_id = new_pattern_id
            else:
                max_pattern_idx += 1
                pcur = conn.execute(
                    "INSERT INTO pattern (name, sort_order) VALUES (?, ?)",
                    (f"Pattern {max_pattern_idx} (from {track_name} clip {clip_idx})", max_pattern_idx),
                )
                clip_pattern_id = pcur.lastrowid
                pattern_id_map[max_pattern_idx] = clip_pattern_id

            # Create midi_clip
            mc_cursor = conn.execute(
                "INSERT INTO midi_clip "
                "(instrument_track_id, pattern_id, clip_type, steps, muted, name) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (it_id, clip_pattern_id, clip_type, steps, muted, clip_name),
            )
            mc_id = mc_cursor.lastrowid

            note_count += convert_notes(conn, clip_elem, mc_id)

            # Calculate clip length from notes if not explicitly set
            notes_in_clip = clip_elem.findall("note")
            if notes_in_clip:
                max_end = 0
                for n in notes_in_clip:
                    n_pos = int(n.get("pos", "0"))
                    n_len = abs(int(n.get("len", "0")))
                    max_end = max(max_end, n_pos + n_len)
                clip_len = max_end if max_end > 0 else steps * 12
            else:
                clip_len = steps * 12

            # Create a patternclip referencing this clip's pattern
            conn.execute(
                "INSERT INTO pattern_clip "
                "(pattern_track_id, pattern_id, position, length, start_offset, muted, name) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (pt_id, clip_pattern_id, clip_pos, clip_len, 0, muted, clip_name),
            )

        log(f"  -> Created pattern track '{track_name}' with {len(midiclips)} clips, {note_count} notes")
        converted += 1

    if converted > 0:
        log(f"Converted {converted} song-level InstrumentTracks to PatternTracks")
    return converted


def convert_automation_tracks(conn, song_tc):
    """Convert automation tracks (type="5" or type="6") and their clips.

    Returns (track_count, clip_count, node_count)
    """
    at_count = 0
    ac_count = 0
    an_count = 0

    for track_elem in song_tc.findall("track"):
        track_type = track_elem.get("type", "")
        # type 5 = Automation, type 6 = HiddenAutomation
        if track_type not in ("5", "6"):
            continue

        name = track_elem.get("name", f"Automation Track {at_count}")
        muted = int(track_elem.get("muted", "0"))
        solo = int(track_elem.get("solo", "0"))
        color = track_elem.get("color")

        cursor = conn.execute(
            "INSERT INTO automation_track (name, muted, solo, color, sort_order) VALUES (?, ?, ?, ?, ?)",
            (name, muted, solo, color, at_count),
        )
        at_id = cursor.lastrowid
        at_count += 1

        # Find automation clips (both current and legacy tag names)
        clip_elems = find_clip_elements(track_elem, "automationclip")

        for clip_elem in clip_elems:
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_len = int(clip_elem.get("len", "0"))
            progression = int(clip_elem.get("prog", "1"))
            tension = float(clip_elem.get("tens", "1.0"))
            clip_muted = int(clip_elem.get("mute", clip_elem.get("muted", "0")))
            clip_name = clip_elem.get("name", "")
            clip_color = clip_elem.get("color")

            ac_cursor = conn.execute(
                "INSERT INTO automation_clip "
                "(automation_track_id, position, length, progression_type, tension, muted, name, color) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (at_id, clip_pos, clip_len, progression, tension, clip_muted, clip_name, clip_color),
            )
            ac_id = ac_cursor.lastrowid
            ac_count += 1

            # Extract time nodes
            for time_elem in clip_elem.findall("time"):
                t_pos = int(time_elem.get("pos", "0"))
                t_value = float(time_elem.get("value", "0"))
                t_out_value = float(time_elem.get("outValue", time_elem.get("value", "0")))
                t_in_tan = float(time_elem.get("inTan", "0"))
                t_out_tan = float(time_elem.get("outTan", "0"))
                t_locked = int(time_elem.get("lockedTan", "0"))

                conn.execute(
                    "INSERT INTO automation_node "
                    "(automation_clip_id, position, in_value, out_value, in_tangent, out_tangent, locked_tangents) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)",
                    (ac_id, t_pos, t_value, t_out_value, t_in_tan, t_out_tan, t_locked),
                )
                an_count += 1

            # Extract automation targets (object references)
            for obj_elem in clip_elem.findall("object"):
                obj_id = int(obj_elem.get("id", "0"))
                conn.execute(
                    "INSERT INTO automation_target (automation_clip_id, target_object_id) VALUES (?, ?)",
                    (ac_id, obj_id),
                )

    if at_count > 0:
        log(f"Automation: {at_count} tracks, {ac_count} clips, {an_count} nodes")
    return at_count, ac_count, an_count


def convert_sample_tracks(conn, song_tc):
    """Convert sample tracks (type="2") and their clips.

    Returns (track_count, clip_count)
    """
    st_count = 0
    sc_count = 0

    for track_elem in song_tc.findall("track"):
        if track_elem.get("type") != "2":
            continue

        name = track_elem.get("name", f"Sample Track {st_count}")
        muted = int(track_elem.get("muted", "0"))
        solo = int(track_elem.get("solo", "0"))
        color = track_elem.get("color")

        # Extract sample track settings
        st_elem = track_elem.find("sampletrack")
        volume = 100.0
        panning = 0.0
        mixer_ch = None
        if st_elem is not None:
            volume = float(st_elem.get("vol", "100"))
            panning = float(st_elem.get("pan", "0"))
            mixer_ch = int(st_elem.get("mixch", "0")) if st_elem.get("mixch") else None

        cursor = conn.execute(
            "INSERT INTO sample_track (name, volume, panning, mixer_channel_id, muted, solo, color, sort_order) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (name, volume, panning, mixer_ch, muted, solo, color, st_count),
        )
        st_id = cursor.lastrowid
        st_count += 1

        # Extract effects from sample track
        if st_elem is not None:
            fxchain = st_elem.find("fxchain")
            if fxchain is not None:
                for fx_idx, fx_elem in enumerate(fxchain.findall("effect")):
                    plugin_name = fx_elem.get("name", "unknown")
                    enabled = int(fx_elem.get("on", "1"))
                    wet = float(fx_elem.get("wet", "1.0"))
                    gate = float(fx_elem.get("gate", "0.0"))
                    params = {}
                    for child in fx_elem:
                        if child.tag == "key":
                            params["_key"] = elem_to_json(child)
                        else:
                            params[child.tag] = elem_to_json(child)
                    conn.execute(
                        "INSERT INTO effect (owner_type, owner_id, plugin_name, sort_order, enabled, wet, gate, params_json) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                        ("sample_track", st_id, plugin_name, fx_idx, enabled, wet, gate, json.dumps(params)),
                    )

        # Convert sample clips
        for clip_elem in find_clip_elements(track_elem, "sampleclip"):
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_len = int(clip_elem.get("len", "0"))
            clip_src = clip_elem.get("src", "")
            clip_muted = int(clip_elem.get("muted", "0"))
            clip_name = clip_elem.get("name", "")
            clip_color = clip_elem.get("color")

            conn.execute(
                "INSERT INTO sample_clip "
                "(sample_track_id, position, length, source_path, muted, name, color) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (st_id, clip_pos, clip_len, clip_src, clip_muted, clip_name, clip_color),
            )
            sc_count += 1

    if st_count > 0:
        log(f"Sample tracks: {st_count} tracks, {sc_count} clips")
    return st_count, sc_count


def convert_controllers(conn, root):
    """Convert <controller> elements from the ControllerRack.

    Note: Controllers are stored as children of the song element in some formats,
    or within track settings as controller connections.
    """
    # Look for controllers in various locations
    count = 0
    for ctrl_elem in root.findall(".//controller"):
        ctrl_type_num = ctrl_elem.get("type", "")
        ctrl_name = ctrl_elem.get("name", "")

        # Map type numbers to names
        type_map = {"0": "lfo", "1": "midi", "2": "peak"}
        ctrl_type = type_map.get(ctrl_type_num, f"type_{ctrl_type_num}")

        params = elem_to_json(ctrl_elem)

        conn.execute(
            "INSERT INTO controller (type, name, params_json) VALUES (?, ?, ?)",
            (ctrl_type, ctrl_name, json.dumps(params)),
        )
        count += 1

    if count > 0:
        log(f"Controllers: {count}")
    return count


def validate_database(conn):
    """Run validation queries on the converted database."""
    log("--- Validation ---")

    tables = [
        "project", "mixer_channel", "mixer_route", "effect",
        "instrument_track", "pattern_track", "pattern_clip", "pattern",
        "midi_clip", "note", "note_detuning",
        "sample_track", "sample_clip",
        "automation_track", "automation_clip", "automation_node", "automation_target",
        "controller",
    ]

    for table in tables:
        count = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        if count > 0:
            log(f"  {table}: {count} rows")

    # Check referential integrity
    orphan_clips = conn.execute(
        "SELECT COUNT(*) FROM pattern_clip pc "
        "LEFT JOIN pattern_track pt ON pc.pattern_track_id = pt.id "
        "WHERE pt.id IS NULL"
    ).fetchone()[0]
    if orphan_clips > 0:
        log(f"  WARNING: {orphan_clips} orphaned pattern clips")

    orphan_midi = conn.execute(
        "SELECT COUNT(*) FROM midi_clip mc "
        "LEFT JOIN instrument_track it ON mc.instrument_track_id = it.id "
        "WHERE it.id IS NULL"
    ).fetchone()[0]
    if orphan_midi > 0:
        log(f"  WARNING: {orphan_midi} orphaned midi clips")

    orphan_notes = conn.execute(
        "SELECT COUNT(*) FROM note n "
        "LEFT JOIN midi_clip mc ON n.midi_clip_id = mc.id "
        "WHERE mc.id IS NULL"
    ).fetchone()[0]
    if orphan_notes > 0:
        log(f"  WARNING: {orphan_notes} orphaned notes")

    log("--- Validation complete ---")


def convert(input_path, output_path=None):
    """Main conversion function."""
    input_path = Path(input_path)
    if output_path is None:
        output_path = input_path.with_suffix(".lmms-db")
    else:
        output_path = Path(output_path)

    # Read and parse the XML
    root = read_mmp_file(input_path)

    # Create the SQLite database
    conn = create_database(output_path)

    try:
        # Convert in order of dependency

        # 1. Project metadata
        convert_project_metadata(conn, root)

        # 2. Mixer channels (needed before tracks that reference them)
        convert_mixer(conn, root)

        # 3. PatternStore instrument tracks and midiclips
        song = root.find("song")
        if song is None:
            log("ERROR: No <song> element found")
            sys.exit(1)

        song_tc = song.find("trackcontainer")
        if song_tc is None:
            log("ERROR: No <trackcontainer> in <song>")
            sys.exit(1)

        # Find the PatternStore (new format: type="patternstore", old format: type="bbtrackcontainer")
        patternstore = song_tc.find('.//trackcontainer[@type="patternstore"]')
        if patternstore is None:
            patternstore = song_tc.find('.//trackcontainer[@type="bbtrackcontainer"]')
        pattern_id_map = {}  # old_index -> new_pattern_id

        if patternstore is not None:
            convert_patternstore(conn, patternstore, pattern_id_map)
        else:
            log("WARNING: No PatternStore found")

        # 4. Pattern tracks with patternclips
        convert_pattern_tracks(conn, song_tc, pattern_id_map)

        # 5. Song-level InstrumentTracks → PatternTracks
        convert_song_level_instrument_tracks(conn, song_tc, pattern_id_map)

        # 6. Automation tracks (check both trackcontainer and direct song children)
        convert_automation_tracks(conn, song_tc)
        convert_automation_tracks(conn, song)

        # 7. Sample tracks (check both trackcontainer and direct song children)
        convert_sample_tracks(conn, song_tc)
        convert_sample_tracks(conn, song)

        # 8. Controllers
        convert_controllers(conn, root)

        # Commit and validate
        conn.commit()
        validate_database(conn)

        db_size = os.path.getsize(output_path)
        log(f"Done! Output: {output_path} ({db_size:,} bytes)")

    except Exception as e:
        conn.rollback()
        log(f"ERROR: Conversion failed: {e}")
        raise
    finally:
        conn.close()


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input.mmp [output.lmms-db]", file=sys.stderr)
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else None
    convert(input_path, output_path)


if __name__ == "__main__":
    main()
