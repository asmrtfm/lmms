---
name: lmms-debug
description: Debug LMMS .lmms-db issues by comparing XML output against .mmp originals using lmms-mcp and rubot. Use when tracks produce no sound, effects are lost, or data is missing after SQLite round-trip.
argument-hint: <db-path> [mmp-path]
disable-model-invocation: true
allowed-tools: Bash(lmms-mcp*), Bash(rubot*), Bash(sqlite3*), Bash(python3*), Bash(diff*), Read, Grep, Glob
---

# LMMS Debug: SQLite Round-Trip Diagnostics

## Workflow

1. Query rubot for known issues: `rubot query "lmms-db round-trip <symptom>"`
2. Use lmms-mcp to inspect the database:
   - `lmms-mcp convert_db_to_mmp` to get XML output
   - `lmms-mcp project_list_tracks` to enumerate tracks
   - `lmms-mcp project_get_instrument_track` for specific track details
3. If .mmp provided, diff the XML structures
4. Check for known pitfalls:
   - `journallingObject` elements in params_json (should be filtered)
   - ZynAddSubFX nested XML corruption during JSON round-trip
   - Missing mixer channel references
   - Duplicate instrument track names
   - Empty automation clips creating phantom tracks

## Key Diagnostic Queries

```bash
# Check for journallingObject contamination
sqlite3 "$DB" "SELECT COUNT(*) FROM instrument_track WHERE params_json LIKE '%journallingObject%'"

# Check plugin params completeness
sqlite3 "$DB" "SELECT id, name, instrument_params_json FROM instrument_track WHERE instrument_params_json IS NULL OR instrument_params_json = '{}'"

# Compare track counts
sqlite3 "$DB" "SELECT container_type, COUNT(*) FROM instrument_track GROUP BY container_type"
```

## After Fix
Always ingest the fix into rubot: `rubot ingest /tmp/claude-learnings/lmms-fix-<issue>.md`
