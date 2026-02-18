#!/usr/bin/env python3
"""
test_conversion.py — Test lmms_convert.py against known project files.

Runs conversion on available .mmp/.mmpz files and validates:
- Entity counts match XML source
- Referential integrity (no orphaned rows)
- Key field values are preserved correctly
- Both old and new XML format support

Usage:
    python3 test_conversion.py [path_to_mmp_file ...]

If no paths given, tests against bundled demo projects.
"""

import sys
import os
import tempfile
import sqlite3
import zlib
import xml.etree.ElementTree as ET
from pathlib import Path

# Add tools directory to path
sys.path.insert(0, str(Path(__file__).parent))
from lmms_convert import convert, read_mmp_file, DEFAULT_PATTERN_LENGTH, LEGACY_TAG_MAP


def log(msg):
    print(f"[test] {msg}", file=sys.stderr)


def count_xml_entities(root):
    """Count key entities in the XML DOM for comparison with SQLite."""
    song = root.find("song")
    song_tc = song.find("trackcontainer") if song is not None else None

    counts = {
        "xml_notes": 0,
        "xml_midiclips": 0,
        "xml_pattern_tracks": 0,
        "xml_song_instrument_tracks": 0,
        "xml_patternstore_instrument_tracks": 0,
        "xml_patternclips": 0,
        "xml_mixer_channels": 0,
        "xml_automation_tracks": 0,
        "xml_automation_clips": 0,
        "xml_automation_nodes": 0,
    }

    if song_tc is None:
        return counts

    # Count pattern tracks (type="1") and song-level instrument tracks (type="0")
    for track in song_tc.findall("track"):
        track_type = track.get("type", "")
        if track_type == "1":
            counts["xml_pattern_tracks"] += 1
            # Count patternclips (both old and new tag names)
            counts["xml_patternclips"] += len(track.findall("patternclip"))
            counts["xml_patternclips"] += len(track.findall("bbtco"))
        elif track_type == "0":
            counts["xml_song_instrument_tracks"] += 1

    # Count items in PatternStore
    patternstore = song_tc.find('.//trackcontainer[@type="patternstore"]')
    if patternstore is None:
        patternstore = song_tc.find('.//trackcontainer[@type="bbtrackcontainer"]')

    if patternstore is not None:
        for track in patternstore.findall("track"):
            if track.get("type") == "0":
                counts["xml_patternstore_instrument_tracks"] += 1

    # Count all midiclips and notes (everywhere in the file)
    for midiclip in root.findall(".//midiclip"):
        counts["xml_midiclips"] += 1
        counts["xml_notes"] += len(midiclip.findall("note"))
    # Legacy tag name
    for midiclip in root.findall(".//pattern"):
        # Only count if it has note children (to avoid counting <pattern> elements that aren't midiclips)
        if midiclip.findall("note") or midiclip.get("pos") is not None:
            counts["xml_midiclips"] += 1
            counts["xml_notes"] += len(midiclip.findall("note"))

    # Count mixer channels
    counts["xml_mixer_channels"] += len(root.findall(".//mixerchannel"))
    counts["xml_mixer_channels"] += len(root.findall(".//fxchannel"))

    # Count automation tracks and clips (both in trackcontainer and directly in song)
    all_track_containers = [song_tc]
    if song is not None:
        all_track_containers.append(song)
    for container in all_track_containers:
        for track in container.findall("track"):
            track_type = track.get("type", "")
            if track_type in ("5", "6"):
                counts["xml_automation_tracks"] += 1
                for clip in track.findall("automationclip"):
                    counts["xml_automation_clips"] += 1
                    counts["xml_automation_nodes"] += len(clip.findall("time"))
                for clip in track.findall("automationpattern"):
                    counts["xml_automation_clips"] += 1
                    counts["xml_automation_nodes"] += len(clip.findall("time"))

    return counts


def count_db_entities(db_path):
    """Count entities in the SQLite database."""
    conn = sqlite3.connect(db_path)
    counts = {}
    for table in [
        "project", "mixer_channel", "mixer_route", "effect",
        "instrument_track", "pattern_track", "pattern_clip", "pattern",
        "midi_clip", "note", "note_detuning",
        "sample_track", "sample_clip",
        "automation_track", "automation_clip", "automation_node",
        "automation_target", "controller",
    ]:
        counts[table] = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
    conn.close()
    return counts


def check_referential_integrity(db_path):
    """Check for orphaned rows in the database."""
    conn = sqlite3.connect(db_path)
    issues = []

    checks = [
        ("pattern_clip → pattern_track",
         "SELECT COUNT(*) FROM pattern_clip pc LEFT JOIN pattern_track pt ON pc.pattern_track_id = pt.id WHERE pt.id IS NULL"),
        ("pattern_clip → pattern",
         "SELECT COUNT(*) FROM pattern_clip pc LEFT JOIN pattern pt ON pc.pattern_id = pt.id WHERE pt.id IS NULL"),
        ("midi_clip → instrument_track",
         "SELECT COUNT(*) FROM midi_clip mc LEFT JOIN instrument_track it ON mc.instrument_track_id = it.id WHERE it.id IS NULL"),
        ("midi_clip → pattern",
         "SELECT COUNT(*) FROM midi_clip mc LEFT JOIN pattern p ON mc.pattern_id = p.id WHERE p.id IS NULL"),
        ("note → midi_clip",
         "SELECT COUNT(*) FROM note n LEFT JOIN midi_clip mc ON n.midi_clip_id = mc.id WHERE mc.id IS NULL"),
        ("automation_clip → automation_track",
         "SELECT COUNT(*) FROM automation_clip ac LEFT JOIN automation_track at2 ON ac.automation_track_id = at2.id WHERE at2.id IS NULL"),
        ("automation_node → automation_clip",
         "SELECT COUNT(*) FROM automation_node an LEFT JOIN automation_clip ac ON an.automation_clip_id = ac.id WHERE ac.id IS NULL"),
        ("mixer_route → mixer_channel (from)",
         "SELECT COUNT(*) FROM mixer_route mr LEFT JOIN mixer_channel mc ON mr.from_channel_id = mc.id WHERE mc.id IS NULL"),
        ("mixer_route → mixer_channel (to)",
         "SELECT COUNT(*) FROM mixer_route mr LEFT JOIN mixer_channel mc ON mr.to_channel_id = mc.id WHERE mc.id IS NULL"),
    ]

    for name, query in checks:
        orphans = conn.execute(query).fetchone()[0]
        if orphans > 0:
            issues.append(f"{name}: {orphans} orphaned rows")

    conn.close()
    return issues


def check_project_metadata(db_path, root):
    """Verify project metadata matches XML."""
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    issues = []

    head = root.find(".//head")
    if head is not None:
        row = conn.execute("SELECT * FROM project WHERE id = 1").fetchone()
        if row is None:
            issues.append("No project row found")
        else:
            expected_bpm = float(head.get("bpm", "140"))
            if abs(row["bpm"] - expected_bpm) > 0.001:
                issues.append(f"BPM mismatch: expected {expected_bpm}, got {row['bpm']}")

            expected_vol = float(head.get("mastervol", "100"))
            if abs(row["master_volume"] - expected_vol) > 0.001:
                issues.append(f"Volume mismatch: expected {expected_vol}, got {row['master_volume']}")

    conn.close()
    return issues


def test_file(input_path):
    """Run conversion and validation on a single file."""
    input_path = Path(input_path)
    log(f"=== Testing {input_path.name} ===")

    # Read XML for comparison
    root = read_mmp_file(input_path)
    xml_counts = count_xml_entities(root)

    # Convert to SQLite
    with tempfile.NamedTemporaryFile(suffix=".lmms-db", delete=False) as f:
        db_path = f.name

    try:
        convert(str(input_path), db_path)
        db_counts = count_db_entities(db_path)

        passed = 0
        failed = 0

        # Test 1: Note count must match exactly
        expected_notes = xml_counts["xml_notes"]
        actual_notes = db_counts["note"]
        if expected_notes == actual_notes:
            log(f"  PASS: Note count matches ({expected_notes})")
            passed += 1
        else:
            log(f"  FAIL: Note count mismatch: XML={expected_notes}, DB={actual_notes}")
            failed += 1

        # Test 2: Instrument track count must match
        expected_it = xml_counts["xml_patternstore_instrument_tracks"] + xml_counts["xml_song_instrument_tracks"]
        actual_it = db_counts["instrument_track"]
        if expected_it == actual_it:
            log(f"  PASS: Instrument track count matches ({expected_it})")
            passed += 1
        else:
            log(f"  FAIL: Instrument track count mismatch: XML={expected_it}, DB={actual_it}")
            failed += 1

        # Test 3: Pattern track count (original + converted song-level)
        expected_pt = xml_counts["xml_pattern_tracks"] + xml_counts["xml_song_instrument_tracks"]
        actual_pt = db_counts["pattern_track"]
        if expected_pt == actual_pt:
            log(f"  PASS: Pattern track count matches ({expected_pt})")
            passed += 1
        else:
            log(f"  FAIL: Pattern track count mismatch: expected={expected_pt}, DB={actual_pt}")
            failed += 1

        # Test 4: Mixer channel count
        expected_mc = xml_counts["xml_mixer_channels"]
        actual_mc = db_counts["mixer_channel"]
        if expected_mc == actual_mc:
            log(f"  PASS: Mixer channel count matches ({expected_mc})")
            passed += 1
        else:
            log(f"  FAIL: Mixer channel count mismatch: XML={expected_mc}, DB={actual_mc}")
            failed += 1

        # Test 5: Automation track count
        expected_at = xml_counts["xml_automation_tracks"]
        actual_at = db_counts["automation_track"]
        if expected_at == actual_at:
            log(f"  PASS: Automation track count matches ({expected_at})")
            passed += 1
        else:
            log(f"  FAIL: Automation track count mismatch: XML={expected_at}, DB={actual_at}")
            failed += 1

        # Test 6: Automation node count
        expected_an = xml_counts["xml_automation_nodes"]
        actual_an = db_counts["automation_node"]
        if expected_an == actual_an:
            log(f"  PASS: Automation node count matches ({expected_an})")
            passed += 1
        else:
            log(f"  FAIL: Automation node count mismatch: XML={expected_an}, DB={actual_an}")
            failed += 1

        # Test 7: Referential integrity
        integrity_issues = check_referential_integrity(db_path)
        if not integrity_issues:
            log(f"  PASS: Referential integrity OK")
            passed += 1
        else:
            for issue in integrity_issues:
                log(f"  FAIL: {issue}")
            failed += 1

        # Test 8: Project metadata
        metadata_issues = check_project_metadata(db_path, root)
        if not metadata_issues:
            log(f"  PASS: Project metadata OK")
            passed += 1
        else:
            for issue in metadata_issues:
                log(f"  FAIL: {issue}")
            failed += 1

        # Test 9: At least one project row exists
        if db_counts["project"] == 1:
            log(f"  PASS: Exactly one project row")
            passed += 1
        else:
            log(f"  FAIL: Expected 1 project row, got {db_counts['project']}")
            failed += 1

        log(f"  Result: {passed} passed, {failed} failed")
        return passed, failed

    except Exception as e:
        log(f"  FAIL: Conversion error: {e}")
        import traceback
        traceback.print_exc(file=sys.stderr)
        return 0, 1
    finally:
        if os.path.exists(db_path):
            os.unlink(db_path)


def main():
    repo_root = Path(__file__).parent.parent

    if len(sys.argv) > 1:
        test_files = [Path(p) for p in sys.argv[1:]]
    else:
        # Auto-discover test files
        test_files = []

        # Check for the main project file
        main_project = repo_root / "Southendopus-hen.mmp"
        if main_project.exists():
            test_files.append(main_project)

        # Add demo projects
        demos_dir = repo_root / "data" / "projects" / "demos"
        if demos_dir.exists():
            for f in sorted(demos_dir.iterdir()):
                if f.suffix.lower() in (".mmp", ".mmpz"):
                    test_files.append(f)

        shorties_dir = repo_root / "data" / "projects" / "shorties"
        if shorties_dir.exists():
            for f in sorted(shorties_dir.iterdir()):
                if f.suffix.lower() in (".mmp", ".mmpz"):
                    test_files.append(f)

    if not test_files:
        log("No test files found!")
        sys.exit(1)

    total_passed = 0
    total_failed = 0
    file_results = []

    for f in test_files:
        passed, failed = test_file(f)
        total_passed += passed
        total_failed += failed
        file_results.append((f.name, passed, failed))

    log("")
    log("=" * 60)
    log(f"TOTAL: {total_passed} passed, {total_failed} failed across {len(test_files)} files")
    log("=" * 60)

    if total_failed > 0:
        log("Failed files:")
        for name, p, f in file_results:
            if f > 0:
                log(f"  {name}: {f} failures")
        sys.exit(1)
    else:
        log("All tests passed!")


if __name__ == "__main__":
    main()
