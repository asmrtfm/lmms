#!/usr/bin/env python3
"""
lmms_export.py — Convert LMMS SQLite (.lmms-db) back to XML (.mmp) format.

Usage:
    python3 lmms_export.py input.lmms-db [output.mmp]

If output path is omitted, uses the input filename with .mmp extension.

DESIGN PRINCIPLE: Every extra_json column is restored exactly on export.
The round-trip must be lossless — no attributes or child elements may be dropped.
"""

import sys
import os
import json
import sqlite3
import xml.etree.ElementTree as ET
from pathlib import Path


LMMS_PROJECT_VERSION = "30"
DEFAULT_PATTERN_LENGTH = 192


def log(msg):
    print(f"[lmms_export] {msg}", file=sys.stderr)


def dict_to_xml(parent, data, skip_keys=None):
    """Convert a JSON-deserialized dict back to XML child elements/attributes.

    The inverse of elem_to_json() from lmms_convert.py.
    Simple key-value pairs become attributes on the parent.
    Dict values become child elements (recursively).
    List values become multiple child elements with the same tag.
    """
    if skip_keys is None:
        skip_keys = set()
    for key, value in data.items():
        if key in skip_keys:
            continue
        if isinstance(value, dict):
            child = ET.SubElement(parent, key)
            dict_to_xml(child, value)
        elif isinstance(value, list):
            for item in value:
                child = ET.SubElement(parent, key)
                if isinstance(item, dict):
                    dict_to_xml(child, item)
                else:
                    child.text = str(item)
        else:
            parent.set(key, str(value))


def apply_extra_json(elem, extra_json_str, skip_children_key="_children"):
    """Apply extra_json attributes and children to an element.

    extra_json may contain:
    - Simple key-value pairs → set as attributes on elem
    - "_children" key → dict of child tag -> elem_to_json data → create child elements
    - Other dict values → create child elements
    """
    if not extra_json_str or extra_json_str == "{}":
        return
    extras = json.loads(extra_json_str)
    if not extras:
        return
    children = extras.pop(skip_children_key, None)
    # Apply remaining as attributes or child elements
    dict_to_xml(elem, extras)
    # Apply unknown children
    if children:
        dict_to_xml(elem, children)


def open_database(db_path):
    """Open a SQLite database and return a connection with row_factory."""
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    return conn


def export_project_metadata(conn, head_elem):
    """Populate the <head> element with project metadata."""
    row = conn.execute("SELECT * FROM project WHERE id = 1").fetchone()
    if row is None:
        log("WARNING: No project metadata found")
        return

    head_elem.set("bpm", str(row["bpm"]))
    head_elem.set("timesig_numerator", str(row["timesig_numerator"]))
    head_elem.set("timesig_denominator", str(row["timesig_denominator"]))
    head_elem.set("mastervol", str(row["master_volume"]))
    head_elem.set("masterpitch", str(row["master_pitch"]))

    # Restore extra head attributes
    apply_extra_json(head_elem, row["extra_json"])

    log(f"Project: name='{row['name']}', bpm={row['bpm']}")


def export_effects(conn, parent_elem, owner_type, owner_id):
    """Generate <fxchain> element with effects for a given owner."""
    effects = conn.execute(
        "SELECT * FROM effect WHERE owner_type = ? AND owner_id = ? ORDER BY sort_order",
        (owner_type, owner_id),
    ).fetchall()

    if not effects:
        fxchain = ET.SubElement(parent_elem, "fxchain")
        fxchain.set("numofeffects", "0")
        fxchain.set("enabled", "0")
        return

    fxchain = ET.SubElement(parent_elem, "fxchain")
    fxchain.set("numofeffects", str(len(effects)))
    fxchain.set("enabled", "1")

    for fx in effects:
        fx_elem = ET.SubElement(fxchain, "effect")
        fx_elem.set("name", fx["plugin_name"])
        fx_elem.set("on", str(fx["enabled"]))
        fx_elem.set("wet", str(fx["wet"]))
        fx_elem.set("gate", str(fx["gate"]))

        params = json.loads(fx["params_json"])
        # Restore extra attributes on <effect> (autoquit, etc.)
        extra_attrs = params.pop("_extra_attrs", None)
        if extra_attrs:
            for k, v in extra_attrs.items():
                fx_elem.set(k, str(v))
        # Rename _key back to key
        if "_key" in params:
            params["key"] = params.pop("_key")
        dict_to_xml(fx_elem, params)

    return fxchain


def export_mixer(conn, song_elem):
    """Generate <mixer> element with channels and routing."""
    channels = conn.execute("SELECT * FROM mixer_channel ORDER BY sort_order").fetchall()
    if not channels:
        return

    mixer_elem = ET.SubElement(song_elem, "mixer")

    for ch in channels:
        ch_elem = ET.SubElement(mixer_elem, "mixerchannel")
        ch_elem.set("num", str(ch["id"]))
        ch_elem.set("name", ch["name"])
        ch_elem.set("volume", str(ch["volume"]))
        ch_elem.set("muted", str(ch["muted"]))
        ch_elem.set("soloed", str(ch["soloed"]))
        if ch["color"]:
            ch_elem.set("color", ch["color"])

        # Restore extra mixer channel attributes and children
        extras_str = ch["extra_json"]
        if extras_str and extras_str != "{}":
            extras = json.loads(extras_str)
            children = extras.pop("_children", None)
            dict_to_xml(ch_elem, extras)
            if children:
                dict_to_xml(ch_elem, children)

        export_effects(conn, ch_elem, "mixer_channel", ch["id"])

        routes = conn.execute(
            "SELECT * FROM mixer_route WHERE from_channel_id = ?",
            (ch["id"],),
        ).fetchall()
        for route in routes:
            send_elem = ET.SubElement(ch_elem, "send")
            send_elem.set("channel", str(route["to_channel_id"]))
            send_elem.set("amount", str(route["amount"]))

    log(f"Mixer: {len(channels)} channels")


def build_instrument_track_element(conn, it_row, parent_elem):
    """Build a complete <track type="0"> element for an instrument track.

    Restores ALL attributes and children from extra_json columns.
    """
    track_elem = ET.SubElement(parent_elem, "track")
    track_elem.set("type", "0")
    track_elem.set("name", it_row["name"])
    track_elem.set("muted", str(it_row["muted"]))
    track_elem.set("solo", str(it_row["solo"]))
    if it_row["color"]:
        track_elem.set("color", it_row["color"])

    # Restore extra <track> attributes (mutedBeforeSolo, etc.)
    track_extras_str = it_row["track_extra_json"]
    if track_extras_str and track_extras_str != "{}":
        track_extras = json.loads(track_extras_str)
        for k, v in track_extras.items():
            if not isinstance(v, (dict, list)):
                track_elem.set(k, str(v))

    # <instrumenttrack> settings element
    it_elem = ET.SubElement(track_elem, "instrumenttrack")
    it_elem.set("vol", str(it_row["volume"]))
    it_elem.set("pan", str(it_row["panning"]))
    it_elem.set("pitch", str(it_row["pitch"]))
    it_elem.set("pitchrange", str(it_row["pitch_range"]))
    if it_row["mixer_channel_id"] is not None:
        it_elem.set("mixch", str(it_row["mixer_channel_id"]))
    it_elem.set("basenote", str(it_row["base_note"]))
    it_elem.set("usemasterpitch", str(it_row["use_master_pitch"]))

    # Restore extra <instrumenttrack> attributes (enablecc, firstkey, lastkey, etc.)
    it_extras_str = it_row["instrumenttrack_extra_json"]
    it_extras_children = None
    if it_extras_str and it_extras_str != "{}":
        it_extras = json.loads(it_extras_str)
        it_extras_children = it_extras.pop("_children", None)
        for k, v in it_extras.items():
            if not isinstance(v, (dict, list)):
                it_elem.set(k, str(v))

    # Instrument plugin
    plugin_name = it_row["instrument_plugin"]
    instrument_elem = ET.SubElement(it_elem, "instrument")
    instrument_elem.set("name", plugin_name)
    plugin_params = json.loads(it_row["instrument_params_json"])
    if plugin_params:
        key_data = plugin_params.pop("_key", None)
        plugin_child = ET.SubElement(instrument_elem, plugin_name)
        dict_to_xml(plugin_child, plugin_params)
        if key_data:
            key_child = ET.SubElement(instrument_elem, "key")
            dict_to_xml(key_child, key_data)

    # Sound shaping (eldata)
    sound_shaping = json.loads(it_row["sound_shaping_json"])
    if sound_shaping:
        eldata_elem = ET.SubElement(it_elem, "eldata")
        dict_to_xml(eldata_elem, sound_shaping)

    # Chord creator
    chord_creator = json.loads(it_row["chord_creator_json"])
    if chord_creator:
        chord_elem = ET.SubElement(it_elem, "chordcreator")
        dict_to_xml(chord_elem, chord_creator)

    # Arpeggiator
    arpeggio = json.loads(it_row["arpeggio_json"])
    if arpeggio:
        arp_elem = ET.SubElement(it_elem, "arpeggiator")
        dict_to_xml(arp_elem, arpeggio)

    # MIDI port
    midi_port = json.loads(it_row["midi_port_json"])
    if midi_port:
        midi_elem = ET.SubElement(it_elem, "midiport")
        dict_to_xml(midi_elem, midi_port)

    # Microtuner
    microtuner = json.loads(it_row["microtuner_json"])
    if microtuner:
        micro_elem = ET.SubElement(it_elem, "microtuner")
        dict_to_xml(micro_elem, microtuner)

    # Restore unknown child elements of <instrumenttrack> (midicontrollers, etc.)
    if it_extras_children:
        dict_to_xml(it_elem, it_extras_children)

    # Effects
    export_effects(conn, it_elem, "instrument_track", it_row["id"])

    return track_elem


def export_patternstore(conn, patternstore_elem, pattern_id_to_index):
    """Populate the patternstore trackcontainer with instrument tracks and their midiclips."""
    instrument_tracks = conn.execute(
        "SELECT * FROM instrument_track ORDER BY sort_order"
    ).fetchall()

    for it_row in instrument_tracks:
        track_elem = build_instrument_track_element(conn, it_row, patternstore_elem)

        midiclips = conn.execute(
            "SELECT mc.*, p.sort_order as pattern_sort_order "
            "FROM midi_clip mc "
            "JOIN pattern p ON mc.pattern_id = p.id "
            "WHERE mc.instrument_track_id = ? "
            "ORDER BY p.sort_order",
            (it_row["id"],),
        ).fetchall()

        for mc in midiclips:
            pattern_idx = pattern_id_to_index.get(mc["pattern_id"], 0)
            pos = pattern_idx * DEFAULT_PATTERN_LENGTH

            mc_elem = ET.SubElement(track_elem, "midiclip")
            mc_elem.set("type", str(mc["clip_type"]))
            mc_elem.set("name", mc["name"] or "")
            mc_elem.set("pos", str(pos))
            mc_elem.set("muted", str(mc["muted"]))
            mc_elem.set("steps", str(mc["steps"]))
            if mc["color"]:
                mc_elem.set("color", mc["color"])

            # Restore extra midiclip attributes
            apply_extra_json(mc_elem, mc["extra_json"])

            # Notes
            notes = conn.execute(
                "SELECT * FROM note WHERE midi_clip_id = ? ORDER BY position",
                (mc["id"],),
            ).fetchall()
            for note in notes:
                note_elem = ET.SubElement(mc_elem, "note")
                note_elem.set("pos", str(note["position"]))
                note_elem.set("len", str(note["length"]))
                note_elem.set("key", str(note["key"]))
                note_elem.set("vol", str(note["volume"]))
                note_elem.set("pan", str(note["panning"]))
                note_elem.set("type", str(note["note_type"]))

                # Restore extra note attributes
                apply_extra_json(note_elem, note["extra_json"])

                # Note detuning
                detunings = conn.execute(
                    "SELECT * FROM note_detuning WHERE note_id = ? ORDER BY position",
                    (note["id"],),
                ).fetchall()
                if detunings:
                    detuning_elem = ET.SubElement(note_elem, "detuning")
                    auto_elem = ET.SubElement(detuning_elem, "automationpattern")
                    for dt in detunings:
                        time_elem = ET.SubElement(auto_elem, "time")
                        time_elem.set("pos", str(dt["position"]))
                        time_elem.set("value", str(dt["value"]))
                        if dt["out_value"] is not None:
                            time_elem.set("outValue", str(dt["out_value"]))
                        time_elem.set("inTan", str(dt["in_tangent"]))
                        time_elem.set("outTan", str(dt["out_tangent"]))

    log(f"PatternStore: {len(instrument_tracks)} instrument tracks")


def export_pattern_tracks(conn, song_tc, patternstore_elem, pattern_id_to_index):
    """Generate Pattern track elements (type='1') in the song trackcontainer."""
    pattern_tracks = conn.execute(
        "SELECT * FROM pattern_track ORDER BY sort_order"
    ).fetchall()

    first_pattern_track = True
    for pt in pattern_tracks:
        track_elem = ET.SubElement(song_tc, "track")
        track_elem.set("type", "1")
        track_elem.set("name", pt["name"])
        track_elem.set("muted", str(pt["muted"]))
        track_elem.set("solo", str(pt["solo"]))
        if pt["color"]:
            track_elem.set("color", pt["color"])

        # Restore extra track attributes
        apply_extra_json(track_elem, pt["extra_json"])

        pt_settings = ET.SubElement(track_elem, "patterntrack")

        if first_pattern_track and patternstore_elem is not None:
            pt_settings.append(patternstore_elem)
            first_pattern_track = False

        clips = conn.execute(
            "SELECT * FROM pattern_clip WHERE pattern_track_id = ? ORDER BY position",
            (pt["id"],),
        ).fetchall()
        for clip in clips:
            clip_elem = ET.SubElement(track_elem, "patternclip")
            clip_elem.set("pos", str(clip["position"]))
            clip_elem.set("len", str(clip["length"]))
            clip_elem.set("off", str(clip["start_offset"]))
            clip_elem.set("muted", str(clip["muted"]))
            clip_elem.set("name", clip["name"] or "")
            if clip["color"]:
                clip_elem.set("color", clip["color"])

            # Restore extra clip attributes
            apply_extra_json(clip_elem, clip["extra_json"])

    log(f"Pattern tracks: {len(pattern_tracks)} tracks")


def export_automation_tracks(conn, parent_elem):
    """Generate automation track elements."""
    auto_tracks = conn.execute(
        "SELECT * FROM automation_track ORDER BY sort_order"
    ).fetchall()

    for at in auto_tracks:
        track_elem = ET.SubElement(parent_elem, "track")
        track_elem.set("type", "5")
        track_elem.set("name", at["name"])
        track_elem.set("muted", str(at["muted"]))
        track_elem.set("solo", str(at["solo"]))
        if at["color"]:
            track_elem.set("color", at["color"])

        # Restore extra track attributes
        apply_extra_json(track_elem, at["extra_json"])

        ET.SubElement(track_elem, "automationtrack")

        clips = conn.execute(
            "SELECT * FROM automation_clip WHERE automation_track_id = ? ORDER BY position",
            (at["id"],),
        ).fetchall()

        for clip in clips:
            clip_elem = ET.SubElement(track_elem, "automationclip")
            clip_elem.set("pos", str(clip["position"]))
            clip_elem.set("len", str(clip["length"]))
            clip_elem.set("prog", str(clip["progression_type"]))
            clip_elem.set("tens", str(clip["tension"]))
            clip_elem.set("mute", str(clip["muted"]))
            clip_elem.set("name", clip["name"] or "")
            if clip["color"]:
                clip_elem.set("color", clip["color"])

            # Restore extra clip attributes and children
            extras_str = clip["extra_json"]
            if extras_str and extras_str != "{}":
                extras = json.loads(extras_str)
                children = extras.pop("_children", None)
                dict_to_xml(clip_elem, extras)
                if children:
                    dict_to_xml(clip_elem, children)

            nodes = conn.execute(
                "SELECT * FROM automation_node WHERE automation_clip_id = ? ORDER BY position",
                (clip["id"],),
            ).fetchall()
            for node in nodes:
                time_elem = ET.SubElement(clip_elem, "time")
                time_elem.set("pos", str(node["position"]))
                time_elem.set("value", str(node["in_value"]))
                time_elem.set("outValue", str(node["out_value"]))
                time_elem.set("inTan", str(node["in_tangent"]))
                time_elem.set("outTan", str(node["out_tangent"]))
                time_elem.set("lockedTan", str(node["locked_tangents"]))

            targets = conn.execute(
                "SELECT * FROM automation_target WHERE automation_clip_id = ?",
                (clip["id"],),
            ).fetchall()
            for target in targets:
                obj_elem = ET.SubElement(clip_elem, "object")
                obj_elem.set("id", str(target["target_object_id"]))

    if auto_tracks:
        log(f"Automation: {len(auto_tracks)} tracks")


def export_sample_tracks(conn, parent_elem):
    """Generate sample track elements."""
    sample_tracks = conn.execute(
        "SELECT * FROM sample_track ORDER BY sort_order"
    ).fetchall()

    for st in sample_tracks:
        track_elem = ET.SubElement(parent_elem, "track")
        track_elem.set("type", "2")
        track_elem.set("name", st["name"])
        track_elem.set("muted", str(st["muted"]))
        track_elem.set("solo", str(st["solo"]))
        if st["color"]:
            track_elem.set("color", st["color"])

        st_elem = ET.SubElement(track_elem, "sampletrack")
        st_elem.set("vol", str(st["volume"]))
        st_elem.set("pan", str(st["panning"]))
        if st["mixer_channel_id"] is not None:
            st_elem.set("mixch", str(st["mixer_channel_id"]))

        # Restore extra attrs from extra_json
        extras_str = st["extra_json"]
        if extras_str and extras_str != "{}":
            extras = json.loads(extras_str)
            # Track-level extras (non-dict, non-_sampletrack)
            sampletrack_extras = extras.pop("_sampletrack", None)
            for k, v in extras.items():
                if not isinstance(v, (dict, list)):
                    track_elem.set(k, str(v))
            # Sampletrack element extras
            if sampletrack_extras:
                children = sampletrack_extras.pop("_children", None)
                for k, v in sampletrack_extras.items():
                    if not isinstance(v, (dict, list)):
                        st_elem.set(k, str(v))
                if children:
                    dict_to_xml(st_elem, children)

        export_effects(conn, st_elem, "sample_track", st["id"])

        clips = conn.execute(
            "SELECT * FROM sample_clip WHERE sample_track_id = ? ORDER BY position",
            (st["id"],),
        ).fetchall()
        for clip in clips:
            clip_elem = ET.SubElement(track_elem, "sampleclip")
            clip_elem.set("pos", str(clip["position"]))
            clip_elem.set("len", str(clip["length"]))
            clip_elem.set("src", clip["source_path"])
            clip_elem.set("muted", str(clip["muted"]))
            clip_elem.set("name", clip["name"] or "")
            if clip["color"]:
                clip_elem.set("color", clip["color"])

            # Restore extra clip attributes
            apply_extra_json(clip_elem, clip["extra_json"])

    if sample_tracks:
        log(f"Sample tracks: {len(sample_tracks)} tracks")


def export_controllers(conn, song_elem):
    """Generate <controllers> element."""
    controllers = conn.execute("SELECT * FROM controller ORDER BY id").fetchall()
    if not controllers:
        return

    controllers_elem = ET.SubElement(song_elem, "controllers")
    for ctrl in controllers:
        ctrl_elem = ET.SubElement(controllers_elem, "controller")
        params = json.loads(ctrl["params_json"])
        dict_to_xml(ctrl_elem, params)

    log(f"Controllers: {len(controllers)}")


def export_to_xml(db_path, output_path=None):
    """Main export function: read SQLite, generate XML."""
    db_path = Path(db_path)
    if output_path is None:
        output_path = db_path.with_suffix(".mmp")
    else:
        output_path = Path(output_path)

    log(f"Reading {db_path}")
    conn = open_database(db_path)

    # Build the pattern_id → index mapping
    patterns = conn.execute("SELECT id, sort_order FROM pattern ORDER BY sort_order").fetchall()
    pattern_id_to_index = {p["id"]: p["sort_order"] for p in patterns}

    # Build XML document
    root = ET.Element("lmms-project")
    root.set("version", LMMS_PROJECT_VERSION)
    root.set("type", "song")
    root.set("creator", "LMMS")
    root.set("creatorversion", "1.3.0-alpha")

    # <head>
    head = ET.SubElement(root, "head")
    export_project_metadata(conn, head)

    # <song>
    song = ET.SubElement(root, "song")

    # Song trackcontainer
    song_tc = ET.SubElement(song, "trackcontainer")
    song_tc.set("type", "song")
    song_tc.set("visible", "1")
    song_tc.set("minimized", "0")
    song_tc.set("maximized", "0")
    song_tc.set("x", "0")
    song_tc.set("y", "0")
    song_tc.set("width", "1600")
    song_tc.set("height", "900")

    # Build patternstore as a detached element first
    patternstore = ET.Element("trackcontainer")
    patternstore.set("type", "patternstore")
    patternstore.set("visible", "1")
    patternstore.set("minimized", "0")
    patternstore.set("maximized", "1")
    patternstore.set("x", "0")
    patternstore.set("y", "0")
    patternstore.set("width", "1527")
    patternstore.set("height", "768")

    export_patternstore(conn, patternstore, pattern_id_to_index)
    export_pattern_tracks(conn, song_tc, patternstore, pattern_id_to_index)
    export_automation_tracks(conn, song_tc)
    export_sample_tracks(conn, song_tc)
    export_mixer(conn, song)
    export_controllers(conn, song)

    conn.close()

    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0"?>\n')
        f.write('<!DOCTYPE lmms-project>\n')
        tree.write(f, encoding="unicode", xml_declaration=False)

    file_size = os.path.getsize(output_path)
    log(f"Done! Output: {output_path} ({file_size:,} bytes)")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input.lmms-db [output.mmp]", file=sys.stderr)
        sys.exit(1)

    db_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else None
    export_to_xml(db_path, output_path)


if __name__ == "__main__":
    main()
