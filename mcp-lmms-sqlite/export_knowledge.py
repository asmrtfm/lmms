#!/usr/bin/env python3
"""Export LMMS defaults as structured markdown for knowledge base ingestion.

Generates one markdown document per instrument plugin, effect plugin,
track subsystem, and a MIDI reference sheet. Output goes to the
knowledge/ subdirectory for ingestion into rubot.
"""

import json
from pathlib import Path

from lmms_defaults import (
    ARPEGGIO,
    CHORD_CREATOR,
    EFFECT_PLUGIN_DEFAULTS,
    INSTRUMENT_PLUGIN_DEFAULTS,
    INSTRUMENTTRACK_EXTRA,
    MIDI_PORT,
    SOUND_SHAPING,
    TRACK_EXTRA,
)

# Output directory relative to this script
OUTPUT_DIR = Path(__file__).parent / "knowledge"

# MIDI note names for reference doc (C-1 through G9)
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Standard LMMS ticks per beat
TICKS_PER_BEAT = 192


def _format_params(params: dict, indent: int = 0) -> str:
    """Format a parameter dict as a markdown table or nested list."""
    lines = []
    prefix = "  " * indent
    for key, value in sorted(params.items()):
        if isinstance(value, dict):
            lines.append(f"{prefix}- **{key}**:")
            lines.append(_format_params(value, indent + 1))
        else:
            display_val = value if value else '""'
            lines.append(f"{prefix}| `{key}` | `{display_val}` |")
    return "\n".join(lines)


def _write_doc(filename: str, content: str) -> Path:
    """Write a markdown document to the knowledge output directory."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUTPUT_DIR / filename
    path.write_text(content)
    return path


def export_instrument(name: str, params: dict) -> Path:
    """Export a single instrument plugin as a markdown knowledge doc."""
    table_rows = []
    for key, value in sorted(params.items()):
        if isinstance(value, dict):
            table_rows.append(f"| `{key}` | (nested object -- see below) |")
        else:
            display_val = value if value else '""'
            table_rows.append(f"| `{key}` | `{display_val}` |")

    nested_sections = []
    for key, value in sorted(params.items()):
        if isinstance(value, dict):
            nested_lines = []
            for k, v in sorted(value.items()):
                display = v if v else '""'
                nested_lines.append(f"| `{k}` | `{display}` |")
            nested_rows = "\n".join(nested_lines)
            nested_sections.append(
                f"\n### {key}\n\n"
                f"| Parameter | Default |\n|---|---|\n{nested_rows}"
            )

    rows_text = "\n".join(table_rows)
    nested_text = "".join(nested_sections)
    content = (
        f"# LMMS Instrument: {name}\n\n"
        f"Plugin name (internal): `{name}`\n\n"
        f"## Default Parameters\n\n"
        f"| Parameter | Default |\n|---|---|\n"
        f"{rows_text}"
        f"{nested_text}\n"
    )
    return _write_doc(f"instrument-{name}.md", content)


def export_effect(name: str, params: dict) -> Path:
    """Export a single effect plugin as a markdown knowledge doc."""
    table_rows = []
    for key, value in sorted(params.items()):
        display_val = value if (isinstance(value, str) and value) else str(value)
        # Truncate very long base64 values
        if len(display_val) > 80:
            display_val = display_val[:77] + "..."
        table_rows.append(f"| `{key}` | `{display_val}` |")

    content = (
        f"# LMMS Effect: {name}\n\n"
        f"Plugin name (internal): `{name}`\n\n"
        f"## Default Parameters\n\n"
        f"| Parameter | Default |\n|---|---|\n"
        f"{chr(10).join(table_rows)}\n"
    )
    return _write_doc(f"effect-{name}.md", content)


def export_track_subsystems() -> Path:
    """Export shared track subsystem defaults as a single knowledge doc."""
    sections = {
        "Sound Shaping (Filter/Envelope/LFO)": SOUND_SHAPING,
        "Arpeggio": ARPEGGIO,
        "Chord Creator": CHORD_CREATOR,
        "MIDI Port": MIDI_PORT,
        "Track Extra": TRACK_EXTRA,
        "Instrument Track Extra": INSTRUMENTTRACK_EXTRA,
    }

    parts = ["# LMMS Track Subsystems\n\nShared defaults for all instrument tracks.\n"]

    for title, params in sections.items():
        parts.append(f"\n## {title}\n")
        for key, value in sorted(params.items()):
            if isinstance(value, dict):
                parts.append(f"\n### {key}\n\n| Parameter | Default |\n|---|---|")
                for k, v in sorted(value.items()):
                    # Skip the 128 CC entries for brevity
                    if k.startswith("cc") and k[2:].isdigit():
                        continue
                    display = v if v else '""'
                    parts.append(f"| `{k}` | `{display}` |")
                if any(k.startswith("cc") for k in value):
                    parts.append("| `cc0`...`cc127` | `0` |")
            else:
                display = value if value else '""'
                parts.append(f"- `{key}`: `{display}`")

    return _write_doc("track-subsystems.md", "\n".join(parts) + "\n")


def export_midi_reference() -> Path:
    """Export a MIDI note number reference chart."""
    parts = [
        "# MIDI Note Reference for LMMS\n",
        f"LMMS uses {TICKS_PER_BEAT} ticks per beat.\n",
        "## Note Number to Name Mapping\n",
        "| Key | Note | Octave |",
        "|-----|------|--------|",
    ]

    for key_num in range(128):
        octave = (key_num // 12) - 1
        note_name = NOTE_NAMES[key_num % 12]
        parts.append(f"| {key_num} | {note_name} | {octave} |")

    parts.extend([
        "\n## Common Reference Points\n",
        "| Note | Key Number |",
        "|------|-----------|",
        "| Middle C (C4) | 60 |",
        "| A4 (concert pitch) | 69 |",
        "| C3 (bass range) | 48 |",
        "| C5 (upper range) | 72 |",
        "\n## Timing Reference\n",
        f"| Duration | Ticks |",
        "|----------|-------|",
        f"| Whole note | {TICKS_PER_BEAT * 4} |",
        f"| Half note | {TICKS_PER_BEAT * 2} |",
        f"| Quarter note | {TICKS_PER_BEAT} |",
        f"| Eighth note | {TICKS_PER_BEAT // 2} |",
        f"| Sixteenth note | {TICKS_PER_BEAT // 4} |",
        f"| Triplet eighth | {TICKS_PER_BEAT // 3} |",
    ])

    return _write_doc("midi-reference.md", "\n".join(parts) + "\n")


def export_all() -> list[Path]:
    """Export all knowledge documents. Returns list of written file paths."""
    written = []

    for name, params in INSTRUMENT_PLUGIN_DEFAULTS.items():
        written.append(export_instrument(name, params))

    for name, params in EFFECT_PLUGIN_DEFAULTS.items():
        written.append(export_effect(name, params))

    written.append(export_track_subsystems())
    written.append(export_midi_reference())

    return written


if __name__ == "__main__":
    paths = export_all()
    print(f"Exported {len(paths)} knowledge documents to {OUTPUT_DIR}/")
    for p in paths:
        print(f"  {p.name}")
