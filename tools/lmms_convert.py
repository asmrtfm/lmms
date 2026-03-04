#!/usr/bin/env python3
"""
lmms_convert.py — Convert LMMS .mmp/.mmpz XML projects to SQLite format.

Usage:
    python3 lmms_convert.py input.mmp [output.lmms-db]
    python3 lmms_convert.py input.mmpz [output.lmms-db]

If output path is omitted, uses the input filename with .lmms-db extension.

DESIGN PRINCIPLE: Every attribute and child element in the XML is stored.
Named columns exist for commonly-queried fields. Everything else goes into
extra_json columns as a catch-all. This guarantees lossless round-trips.
"""

import sys
import os
import re
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
            # .mmpz files have a 4-byte header (data length) followed by zlib data
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


def extra_attrs(elem, known_keys):
    """Capture all attributes NOT in known_keys as a dict.
    Returns {} if none extra.
    """
    return {k: v for k, v in elem.attrib.items() if k not in known_keys}


def extra_children(elem, known_tags):
    """Capture all child elements whose tags are NOT in known_tags as JSON.
    Returns a dict mapping tag -> elem_to_json(child).
    If multiple children with same unknown tag, becomes a list.
    """
    result = {}
    for child in elem:
        if child.tag in known_tags:
            continue
        child_data = elem_to_json(child)
        if child.tag in result:
            if not isinstance(result[child.tag], list):
                result[child.tag] = [result[child.tag]]
            result[child.tag].append(child_data)
        else:
            result[child.tag] = child_data
    return result


# ─── Name normalization (port of Track::normalizeTrackNames) ───

def normalize_name(name):
    """Normalize a single track name: strip clone-jank, replace whitespace with underscores."""
    # Step 1: remove "Clone of" prefixes (case-insensitive)
    name = re.sub(r'[Cc]lone[\s+\-_]+of[\s+\-_]+', '', name).strip()
    # Step 2: replace whitespace runs with underscores
    name = re.sub(r'\s+', '_', name)
    return name


def normalize_names(names):
    """Normalize a list of track names, deduplicating with 3-digit counters.
    Returns a list of normalized names in the same order.
    """
    result = []
    used = set()
    for original in names:
        candidate = normalize_name(original)
        if candidate not in used:
            result.append(candidate)
            used.add(candidate)
            if candidate != original:
                log(f"  normalize: '{original}' -> '{candidate}'")
            continue

        # Conflict — add counter
        has_counter = (len(candidate) >= 3
                       and candidate[-3] == '0'
                       and candidate[-2:].isdigit()
                       and candidate[-3:].isdigit())
        if has_counter:
            base = candidate[:-3]
            start = int(candidate[-3:]) + 1
        else:
            base = candidate + "_"
            start = 1

        for c in range(start, 100):
            numbered = f"{base}{c:03d}"
            if numbered not in used:
                candidate = numbered
                break

        result.append(candidate)
        used.add(candidate)
        log(f"  normalize: '{original}' -> '{candidate}'")

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


def convert_project_metadata(conn, root, project_name):
    """Extract <head> attributes into the project table."""
    head = root.find(".//head")
    if head is None:
        log("WARNING: No <head> element found, using defaults")
        conn.execute("INSERT INTO project (id, name) VALUES (1, ?)", (project_name,))
        return

    known = {"bpm", "timesig_numerator", "timesig_denominator", "mastervol", "masterpitch"}
    bpm = float(head.get("bpm", "140"))
    ts_num = int(head.get("timesig_numerator", "4"))
    ts_den = int(head.get("timesig_denominator", "4"))
    master_vol = float(head.get("mastervol", "100"))
    master_pitch = float(head.get("masterpitch", "0"))
    extras = extra_attrs(head, known)

    conn.execute(
        "INSERT INTO project (id, name, bpm, timesig_numerator, timesig_denominator, "
        "master_volume, master_pitch, extra_json) VALUES (1, ?, ?, ?, ?, ?, ?, ?)",
        (project_name, bpm, ts_num, ts_den, master_vol, master_pitch, json.dumps(extras)),
    )
    log(f"Project: name='{project_name}', bpm={bpm}, time_sig={ts_num}/{ts_den}, vol={master_vol}, pitch={master_pitch}")
    if extras:
        log(f"  extra head attrs: {list(extras.keys())}")


def convert_mixer(conn, root):
    """Extract mixer channels and routing."""
    mixer_channels = root.findall(".//mixerchannel")
    if not mixer_channels:
        mixer_channels = root.findall(".//fxchannel")
    if not mixer_channels:
        log("No mixer channels found")
        return

    channel_count = 0
    route_count = 0
    effect_count = 0
    deferred_routes = []

    known_ch_attrs = {"num", "name", "volume", "muted", "soloed"}

    for ch_elem in mixer_channels:
        ch_num = int(ch_elem.get("num", "0"))
        ch_name = ch_elem.get("name", "")
        ch_volume = float(ch_elem.get("volume", "1.0"))
        ch_muted = int(ch_elem.get("muted", "0"))
        ch_soloed = int(ch_elem.get("soloed", "0"))

        ch_extras = extra_attrs(ch_elem, known_ch_attrs)
        # Also capture unknown child elements (not send, not fxchain)
        ch_child_extras = extra_children(ch_elem, {"send", "fxchain"})
        if ch_child_extras:
            ch_extras["_children"] = ch_child_extras

        conn.execute(
            "INSERT INTO mixer_channel (id, name, volume, muted, soloed, sort_order, extra_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (ch_num, ch_name, ch_volume, ch_muted, ch_soloed, ch_num, json.dumps(ch_extras)),
        )
        channel_count += 1

        for send_elem in ch_elem.findall("send"):
            to_channel = int(send_elem.get("channel", "0"))
            amount = float(send_elem.get("amount", "1.0"))
            deferred_routes.append((ch_num, to_channel, amount))

        effect_count += convert_fxchain(conn, ch_elem, "mixer_channel", ch_num)

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
        for settings_tag in ("instrumenttrack", "sampletrack"):
            settings = parent_elem.find(settings_tag)
            if settings is not None:
                fxchain = settings.find("fxchain")
                if fxchain is not None:
                    break

    if fxchain is None:
        return 0

    # Capture ALL attributes on fxchain itself (numofeffects, enabled, etc.)
    known_fx_attrs = {"name", "on", "wet", "gate"}

    count = 0
    for idx, fx_elem in enumerate(fxchain.findall("effect")):
        plugin_name = fx_elem.get("name", "unknown")
        enabled = int(fx_elem.get("on", "1"))
        wet = float(fx_elem.get("wet", "1.0"))
        gate = float(fx_elem.get("gate", "0.0"))

        # ALL extra attributes and ALL child elements go into params_json
        params = {}
        fx_extras = extra_attrs(fx_elem, known_fx_attrs)
        if fx_extras:
            params["_extra_attrs"] = fx_extras
        for child in fx_elem:
            if child.tag == "key":
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
    Captures ALL attributes and ALL child elements — nothing is dropped.
    """
    it_elem = track_elem.find("instrumenttrack")
    if it_elem is None:
        return None

    # Known <track> attributes
    known_track_attrs = {"type", "name", "muted", "solo", "color"}
    track_extras = extra_attrs(track_elem, known_track_attrs)

    # Known <instrumenttrack> attributes
    known_it_attrs = {"vol", "pan", "pitch", "pitchrange", "mixch", "fxch",
                      "basenote", "usemasterpitch"}
    it_extras = extra_attrs(it_elem, known_it_attrs)

    # Known <instrumenttrack> child elements (captured in named columns)
    known_it_children = {"instrument", "eldata", "arpeggiator", "chordcreator",
                         "midiport", "fxchain", "microtuner"}

    # Capture ALL unknown child elements of <instrumenttrack>
    unknown_children = extra_children(it_elem, known_it_children)
    if unknown_children:
        it_extras["_children"] = unknown_children

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
        "track_extra_json": json.dumps(track_extras),
        "instrumenttrack_extra_json": json.dumps(it_extras),
    }

    # Instrument plugin
    instrument = it_elem.find("instrument")
    if instrument is not None:
        data["instrument_plugin"] = instrument.get("name", "unknown")
        plugin_elem = None
        key_elem = None
        for child in instrument:
            if child.tag == "key":
                key_elem = child
            elif plugin_elem is None:
                plugin_elem = child
        params = elem_to_json(plugin_elem) if plugin_elem is not None else {}
        if key_elem is not None:
            params["_key"] = elem_to_json(key_elem)
        data["instrument_params_json"] = json.dumps(params)
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

    # Microtuner
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
        "arpeggio_json, chord_creator_json, midi_port_json, microtuner_json, "
        "track_extra_json, instrumenttrack_extra_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
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
            data["track_extra_json"],
            data["instrumenttrack_extra_json"],
        ),
    )
    return cursor.lastrowid


def convert_notes(conn, clip_elem, midi_clip_id):
    """Extract <note> elements from a midiclip and insert into the note table.

    Returns the number of notes inserted.
    """
    known_note_attrs = {"pos", "len", "key", "vol", "pan", "type"}
    count = 0
    for note_elem in clip_elem.findall("note"):
        pos = int(note_elem.get("pos", "0"))
        length = int(note_elem.get("len", "0"))
        key = int(note_elem.get("key", "69"))
        volume = int(note_elem.get("vol", "100"))
        panning = int(note_elem.get("pan", "0"))
        note_type = int(note_elem.get("type", "0"))
        note_extras = extra_attrs(note_elem, known_note_attrs)

        note_cursor = conn.execute(
            "INSERT INTO note (midi_clip_id, position, length, key, volume, panning, note_type, extra_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (midi_clip_id, pos, length, key, volume, panning, note_type, json.dumps(note_extras)),
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
    for old_tag, new_tag in LEGACY_TAG_MAP.items():
        if new_tag == tag_name:
            results.extend(track_elem.findall(old_tag))
    return results


def convert_patternstore(conn, patternstore_elem, pattern_id_map):
    """Convert the PatternStore's instrument tracks and their midiclips.

    Returns (instrument_track_count, midi_clip_count, note_count)
    """
    it_count = 0
    mc_count = 0
    note_count = 0

    # Collect and normalize instrument track names
    track_elems = [t for t in patternstore_elem.findall("track") if t.get("type") == "0"]
    original_names = [t.get("name", "") for t in track_elems]
    normalized_names = normalize_names(original_names)

    # First pass: discover all pattern indices used by any midiclip
    all_pattern_indices = set()
    for track_elem in track_elems:
        for clip_elem in find_clip_elements(track_elem, "midiclip"):
            pos = int(clip_elem.get("pos", "0"))
            pattern_idx = pos // DEFAULT_PATTERN_LENGTH
            all_pattern_indices.add(pattern_idx)

    for idx in sorted(all_pattern_indices):
        cursor = conn.execute(
            "INSERT INTO pattern (name, sort_order) VALUES (?, ?)",
            (f"Pattern {idx}", idx),
        )
        pattern_id_map[idx] = cursor.lastrowid

    log(f"PatternStore: {len(all_pattern_indices)} patterns discovered")

    known_mc_attrs = {"type", "name", "pos", "muted", "mute", "steps", "color"}

    # Second pass: convert instrument tracks and their midiclips
    for sort_order, (track_elem, norm_name) in enumerate(zip(track_elems, normalized_names)):
        # Apply normalized name to the element before extraction
        track_elem.set("name", norm_name)

        data = extract_instrument_track_data(track_elem)
        if data is None:
            log(f"WARNING: Track '{norm_name}' has no <instrumenttrack>, skipping")
            continue

        it_id = insert_instrument_track(conn, data, sort_order)

        # Extract effects
        fx_count = convert_fxchain(conn, track_elem, "instrument_track", it_id)
        if fx_count > 0:
            log(f"  Track '{data['name']}': {fx_count} effects")

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
            mc_extras = extra_attrs(clip_elem, known_mc_attrs)

            cursor = conn.execute(
                "INSERT OR IGNORE INTO midi_clip "
                "(instrument_track_id, pattern_id, clip_type, steps, muted, name, color, extra_json) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (it_id, pattern_id, clip_type, steps, muted, clip_name, clip_color, json.dumps(mc_extras)),
            )
            mc_id = cursor.lastrowid
            if mc_id == 0:
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

    Returns (pattern_track_count, pattern_clip_count)
    """
    pt_count = 0
    pc_count = 0

    known_track_attrs = {"type", "name", "muted", "solo", "color"}
    known_pc_attrs = {"pos", "len", "off", "muted", "name", "color"}

    # Collect and normalize pattern track names
    track_elems = [t for t in song_tc.findall("track") if t.get("type") == "1"]
    original_names = [t.get("name", f"Pattern Track {i}") for i, t in enumerate(track_elems)]
    normalized_names = normalize_names(original_names)

    for pattern_track_idx, (track_elem, norm_name) in enumerate(zip(track_elems, normalized_names)):
        track_extras = extra_attrs(track_elem, known_track_attrs)

        this_pattern_id = pattern_id_map.get(pattern_track_idx)

        cursor = conn.execute(
            "INSERT INTO pattern_track (name, muted, solo, color, sort_order, extra_json) VALUES (?, ?, ?, ?, ?, ?)",
            (norm_name,
             int(track_elem.get("muted", "0")),
             int(track_elem.get("solo", "0")),
             track_elem.get("color"),
             pattern_track_idx,
             json.dumps(track_extras)),
        )
        pt_id = cursor.lastrowid
        pt_count += 1

        for clip_elem in find_clip_elements(track_elem, "patternclip"):
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_len = int(clip_elem.get("len", "0"))
            clip_off = int(clip_elem.get("off", "0"))
            clip_muted = int(clip_elem.get("muted", "0"))
            clip_name = clip_elem.get("name", "")
            clip_color = clip_elem.get("color")
            clip_extras = extra_attrs(clip_elem, known_pc_attrs)

            if this_pattern_id is None:
                pcursor = conn.execute(
                    "INSERT INTO pattern (name, sort_order) VALUES (?, ?)",
                    (f"Pattern {pattern_track_idx}", pattern_track_idx),
                )
                this_pattern_id = pcursor.lastrowid
                pattern_id_map[pattern_track_idx] = this_pattern_id

            conn.execute(
                "INSERT INTO pattern_clip "
                "(pattern_track_id, pattern_id, position, length, start_offset, muted, name, color, extra_json) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (pt_id, this_pattern_id, clip_pos, clip_len, clip_off, clip_muted, clip_name, clip_color,
                 json.dumps(clip_extras)),
            )
            pc_count += 1

    log(f"Pattern tracks: {pt_count} tracks, {pc_count} clips")
    return pt_count, pc_count


def convert_song_level_instrument_tracks(conn, song_tc, pattern_id_map):
    """Convert Song-level InstrumentTracks (type="0" directly in Song) to PatternTracks.

    Returns (converted_count)
    """
    converted = 0

    max_pattern_idx = max(pattern_id_map.keys()) if pattern_id_map else -1
    existing_pt_count = conn.execute("SELECT COUNT(*) FROM pattern_track").fetchone()[0]
    existing_it_count = conn.execute("SELECT COUNT(*) FROM instrument_track").fetchone()[0]

    for track_elem in song_tc.findall("track"):
        if track_elem.get("type") != "0":
            continue

        track_name = normalize_name(track_elem.get("name", "Converted Track"))
        log(f"Converting song-level InstrumentTrack '{track_name}' to PatternTrack")

        # Apply normalized name
        track_elem.set("name", track_name)

        max_pattern_idx += 1
        new_pattern_idx = max_pattern_idx
        pcursor = conn.execute(
            "INSERT INTO pattern (name, sort_order) VALUES (?, ?)",
            (f"Pattern {new_pattern_idx} (from {track_name})", new_pattern_idx),
        )
        new_pattern_id = pcursor.lastrowid
        pattern_id_map[new_pattern_idx] = new_pattern_id

        data = extract_instrument_track_data(track_elem)
        if data is None:
            log(f"WARNING: Song-level track '{track_name}' has no <instrumenttrack>, skipping")
            continue

        it_id = insert_instrument_track(conn, data, existing_it_count + converted)

        convert_fxchain(conn, track_elem, "instrument_track", it_id)

        ptcursor = conn.execute(
            "INSERT INTO pattern_track (name, muted, solo, color, sort_order, extra_json) VALUES (?, ?, ?, ?, ?, ?)",
            (track_name, data["muted"], data["solo"], data["color"], existing_pt_count + converted, "{}"),
        )
        pt_id = ptcursor.lastrowid

        known_mc_attrs = {"type", "name", "pos", "muted", "mute", "steps", "color"}
        midiclips = find_clip_elements(track_elem, "midiclip")
        note_count = 0
        for clip_idx, clip_elem in enumerate(midiclips):
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_type = int(clip_elem.get("type", "1"))
            steps = int(clip_elem.get("steps", "32"))
            muted = int(clip_elem.get("muted", clip_elem.get("mute", "0")))
            clip_name = clip_elem.get("name", "")
            mc_extras = extra_attrs(clip_elem, known_mc_attrs)

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

            mc_cursor = conn.execute(
                "INSERT INTO midi_clip "
                "(instrument_track_id, pattern_id, clip_type, steps, muted, name, extra_json) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (it_id, clip_pattern_id, clip_type, steps, muted, clip_name, json.dumps(mc_extras)),
            )
            mc_id = mc_cursor.lastrowid

            note_count += convert_notes(conn, clip_elem, mc_id)

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

            conn.execute(
                "INSERT INTO pattern_clip "
                "(pattern_track_id, pattern_id, position, length, start_offset, muted, name, extra_json) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (pt_id, clip_pattern_id, clip_pos, clip_len, 0, muted, clip_name, "{}"),
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

    known_track_attrs = {"type", "name", "muted", "solo", "color"}
    known_ac_attrs = {"pos", "len", "prog", "tens", "mute", "muted", "name", "color"}

    for track_elem in song_tc.findall("track"):
        track_type = track_elem.get("type", "")
        if track_type not in ("5", "6"):
            continue

        name = track_elem.get("name", f"Automation Track {at_count}")
        muted = int(track_elem.get("muted", "0"))
        solo = int(track_elem.get("solo", "0"))
        color = track_elem.get("color")
        track_extras = extra_attrs(track_elem, known_track_attrs)

        cursor = conn.execute(
            "INSERT INTO automation_track (name, muted, solo, color, sort_order, extra_json) VALUES (?, ?, ?, ?, ?, ?)",
            (name, muted, solo, color, at_count, json.dumps(track_extras)),
        )
        at_id = cursor.lastrowid
        at_count += 1

        clip_elems = find_clip_elements(track_elem, "automationclip")

        for clip_elem in clip_elems:
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_len = int(clip_elem.get("len", "0"))
            progression = int(clip_elem.get("prog", "1"))
            tension = float(clip_elem.get("tens", "1.0"))
            clip_muted = int(clip_elem.get("mute", clip_elem.get("muted", "0")))
            clip_name = clip_elem.get("name", "")
            clip_color = clip_elem.get("color")
            clip_extras = extra_attrs(clip_elem, known_ac_attrs)
            # Capture unknown children (not time, not object)
            clip_child_extras = extra_children(clip_elem, {"time", "object"})
            if clip_child_extras:
                clip_extras["_children"] = clip_child_extras

            ac_cursor = conn.execute(
                "INSERT INTO automation_clip "
                "(automation_track_id, position, length, progression_type, tension, muted, name, color, extra_json) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (at_id, clip_pos, clip_len, progression, tension, clip_muted, clip_name, clip_color,
                 json.dumps(clip_extras)),
            )
            ac_id = ac_cursor.lastrowid
            ac_count += 1

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

    known_track_attrs = {"type", "name", "muted", "solo", "color"}
    known_st_attrs = {"vol", "pan", "mixch", "fxch"}
    known_sc_attrs = {"pos", "len", "src", "muted", "name", "color"}

    for track_elem in song_tc.findall("track"):
        if track_elem.get("type") != "2":
            continue

        name = track_elem.get("name", f"Sample Track {st_count}")
        muted = int(track_elem.get("muted", "0"))
        solo = int(track_elem.get("solo", "0"))
        color = track_elem.get("color")
        track_extras = extra_attrs(track_elem, known_track_attrs)

        st_elem = track_elem.find("sampletrack")
        volume = 100.0
        panning = 0.0
        mixer_ch = None
        st_extras = {}
        if st_elem is not None:
            volume = float(st_elem.get("vol", "100"))
            panning = float(st_elem.get("pan", "0"))
            mixer_ch = int(st_elem.get("mixch", st_elem.get("fxch", "0"))) if (st_elem.get("mixch") or st_elem.get("fxch")) else None
            st_extras = extra_attrs(st_elem, known_st_attrs)
            # Capture unknown children of sampletrack (not fxchain)
            st_child_extras = extra_children(st_elem, {"fxchain"})
            if st_child_extras:
                st_extras["_children"] = st_child_extras

        # Merge track-level extras
        all_extras = {**track_extras}
        if st_extras:
            all_extras["_sampletrack"] = st_extras

        cursor = conn.execute(
            "INSERT INTO sample_track (name, volume, panning, mixer_channel_id, muted, solo, color, sort_order, extra_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (name, volume, panning, mixer_ch, muted, solo, color, st_count, json.dumps(all_extras)),
        )
        st_id = cursor.lastrowid
        st_count += 1

        convert_fxchain(conn, track_elem, "sample_track", st_id)

        for clip_elem in find_clip_elements(track_elem, "sampleclip"):
            clip_pos = int(clip_elem.get("pos", "0"))
            clip_len = int(clip_elem.get("len", "0"))
            clip_src = clip_elem.get("src", "")
            clip_muted = int(clip_elem.get("muted", "0"))
            clip_name = clip_elem.get("name", "")
            clip_color = clip_elem.get("color")
            clip_extras = extra_attrs(clip_elem, known_sc_attrs)

            conn.execute(
                "INSERT INTO sample_clip "
                "(sample_track_id, position, length, source_path, muted, name, color, extra_json) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (st_id, clip_pos, clip_len, clip_src, clip_muted, clip_name, clip_color,
                 json.dumps(clip_extras)),
            )
            sc_count += 1

    if st_count > 0:
        log(f"Sample tracks: {st_count} tracks, {sc_count} clips")
    return st_count, sc_count


def convert_controllers(conn, root):
    """Convert <controller> elements from the ControllerRack."""
    count = 0
    for ctrl_elem in root.findall(".//controller"):
        ctrl_type_num = ctrl_elem.get("type", "")
        ctrl_name = ctrl_elem.get("name", "")

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

    # Project name = input file basename (without extension)
    project_name = input_path.stem

    root = read_mmp_file(input_path)

    conn = create_database(output_path)

    try:
        # 1. Project metadata (with project name from filename)
        convert_project_metadata(conn, root, project_name)

        # 2. Mixer channels
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

        patternstore = song_tc.find('.//trackcontainer[@type="patternstore"]')
        if patternstore is None:
            patternstore = song_tc.find('.//trackcontainer[@type="bbtrackcontainer"]')
        pattern_id_map = {}

        if patternstore is not None:
            convert_patternstore(conn, patternstore, pattern_id_map)
        else:
            log("WARNING: No PatternStore found")

        # 4. Pattern tracks with patternclips
        convert_pattern_tracks(conn, song_tc, pattern_id_map)

        # 5. Song-level InstrumentTracks → PatternTracks
        convert_song_level_instrument_tracks(conn, song_tc, pattern_id_map)

        # 6. Automation tracks
        convert_automation_tracks(conn, song_tc)
        convert_automation_tracks(conn, song)

        # 7. Sample tracks
        convert_sample_tracks(conn, song_tc)
        convert_sample_tracks(conn, song)

        # 8. Controllers
        convert_controllers(conn, root)

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
