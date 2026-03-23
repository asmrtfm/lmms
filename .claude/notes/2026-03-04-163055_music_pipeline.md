# Music Pipeline Implementation WIP

## Status: All phases complete (1-5). Phase 6 (make the beat) ready to execute.

## Key paths
- rubot: /home/me/.src/github/asmrtfm/rubot/20260301/063437/rubot/
- lmms-mcp: /home/me/.src/github/asmrtfm/lmms/mcp-lmms-sqlite/
- skills: ~/.claude/skills/

## Ollama music models installed
- tristanbehrens/musicllm:latest
- ALIENTELLIGENCE/musicprompts:latest
- ALIENTELLIGENCE/musicindustry:latest
- AeroCorp/afm-african_music:latest
- llamusic/llamusic:latest
- rndmcnlly/strudel-edit:latest

## User requirements
- All models must be configurable where used
- Configured/specified models must be honored throughout
- DB was down, now started via docker compose

## Phases
1. Music constants + config + generation/music.py + MCP tool + CLI command
2. LMMS knowledge export + rubot ingest command
3. Claude Code skills
4. Scheduling in rubot CLI
5. Music knowledge enrichment pipeline
6. Make the beat
