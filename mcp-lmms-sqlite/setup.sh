#!/usr/bin/env bash
set -euo pipefail

# LMMS MCP Server - Install & Setup Script
# Installs the MCP server and configures it for Claude Code.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors (disabled if not a terminal)
if [ -t 1 ]; then
    BOLD='\033[1m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RED='\033[0;31m'
    RESET='\033[0m'
else
    BOLD='' GREEN='' YELLOW='' RED='' RESET=''
fi

info()  { echo -e "${GREEN}[+]${RESET} $*"; }
warn()  { echo -e "${YELLOW}[!]${RESET} $*"; }
error() { echo -e "${RED}[x]${RESET} $*" >&2; }

# ── Pre-flight checks ──────────────────────────────────────

check_python() {
    for cmd in python3 python; do
        if command -v "$cmd" &>/dev/null; then
            version=$("$cmd" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
            major=$("$cmd" -c 'import sys; print(sys.version_info.major)')
            minor=$("$cmd" -c 'import sys; print(sys.version_info.minor)')
            if [ "$major" -ge 3 ] && [ "$minor" -ge 10 ]; then
                PYTHON="$cmd"
                info "Found $PYTHON ($version)"
                return 0
            fi
        fi
    done
    error "Python 3.10+ is required but not found."
    exit 1
}

check_tools() {
    local missing=0
    if [ ! -f "$REPO_ROOT/tools/lmms_convert.py" ]; then
        warn "tools/lmms_convert.py not found - .mmp conversion will not work"
        warn "Fetch it: git remote add asmrtfm https://github.com/asmrtfm/lmms.git"
        warn "          git fetch asmrtfm feature/sqlite-conversion"
        warn "          git checkout asmrtfm/feature/sqlite-conversion -- tools/lmms_convert.py tools/lmms_export.py tools/lmms_schema.sql"
        missing=1
    fi
    if [ ! -f "$REPO_ROOT/tools/lmms_schema.sql" ]; then
        warn "tools/lmms_schema.sql not found - blank project creation will not work"
        missing=1
    fi
    if [ "$missing" -eq 0 ]; then
        info "Conversion tools found (lmms_convert.py, lmms_schema.sql)"
    fi
}

# ── Install ─────────────────────────────────────────────────

install_server() {
    info "Installing lmms-mcp-sqlite..."
    "$PYTHON" -m pip install -e "$SCRIPT_DIR" --quiet 2>&1 | tail -3
    info "Installed successfully"
}

# ── Claude Code configuration ──────────────────────────────

configure_claude() {
    local settings_dir="$HOME/.claude"
    local settings_file="$settings_dir/settings.json"
    local server_path="$SCRIPT_DIR/server.py"

    echo ""
    echo -e "${BOLD}Claude Code MCP Configuration${RESET}"
    echo ""

    # Build the JSON snippet
    local snippet
    snippet=$(cat <<ENDJSON
{
  "mcpServers": {
    "lmms-sqlite": {
      "command": "$PYTHON",
      "args": ["$server_path"]
    }
  }
}
ENDJSON
)

    if [ -f "$settings_file" ]; then
        # Check if already configured
        if grep -q "lmms-sqlite" "$settings_file" 2>/dev/null; then
            info "Claude Code already configured with lmms-sqlite"
            return 0
        fi
        warn "Existing $settings_file found."
        echo "  Add this to your mcpServers block:"
        echo ""
        echo "    \"lmms-sqlite\": {"
        echo "      \"command\": \"$PYTHON\","
        echo "      \"args\": [\"$server_path\"]"
        echo "    }"
        echo ""
    else
        echo "  To connect to Claude Code, create $settings_file with:"
        echo ""
        echo "$snippet"
        echo ""
        read -rp "  Create this file now? [Y/n] " answer
        if [[ "${answer:-y}" =~ ^[Yy]$ ]]; then
            mkdir -p "$settings_dir"
            echo "$snippet" > "$settings_file"
            info "Created $settings_file"
        fi
    fi
}

# ── Verify ──────────────────────────────────────────────────

verify_install() {
    echo ""
    info "Verifying installation..."
    if "$PYTHON" -c "from mcp.server.fastmcp import FastMCP; print('  mcp SDK: OK')" 2>/dev/null; then
        true
    else
        error "mcp SDK import failed"
        return 1
    fi
    if "$PYTHON" -c "import ast; ast.parse(open('$SCRIPT_DIR/server.py').read()); print('  server.py: OK')"; then
        true
    else
        error "server.py has syntax errors"
        return 1
    fi
    info "All checks passed"
}

# ── Quick-start summary ────────────────────────────────────

print_summary() {
    echo ""
    echo -e "${BOLD}━━━ Quick Start ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
    echo ""
    echo "  Run the server standalone:"
    echo "    $PYTHON $SCRIPT_DIR/server.py"
    echo ""
    echo "  Or via mcp CLI:"
    echo "    mcp run $SCRIPT_DIR/server.py"
    echo ""
    echo "  Convert an existing .mmp project:"
    echo "    Use the convert_mmp_to_db tool, or:"
    echo "    $PYTHON $REPO_ROOT/tools/lmms_convert.py my_song.mmp my_song.lmms-db"
    echo ""
    echo "  Environment variables:"
    echo "    LMMS_CATALOG_DB  Catalog DB path (default: ~/.lmms/catalog.db)"
    echo ""
    echo -e "${BOLD}━━━ Example: Create a track from scratch ━━━━━━━━━━━━━━━${RESET}"
    echo ""
    echo '  Once connected to Claude, ask:'
    echo '    "Create a blank project at /tmp/test.lmms-db, add a'
    echo '     TripleOscillator track with a C major chord at 120 BPM,'
    echo '     and put a ReverbSC on it."'
    echo ""
    echo '  Claude will call: create_blank_project, project_set_bpm,'
    echo '  project_create_instrument_track, and project_add_effect.'
    echo ""
    echo -e "${BOLD}━━━ Example: Reuse tracks across projects ━━━━━━━━━━━━━━${RESET}"
    echo ""
    echo '    "Extract the bass track from song_a.lmms-db and insert'
    echo '     it into song_b.lmms-db"'
    echo ""
    echo '  Claude will call: project_list_tracks, extract_track_template,'
    echo '  and insert_track_template.'
    echo ""
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
}

# ── Main ────────────────────────────────────────────────────

main() {
    echo ""
    echo -e "${BOLD}LMMS MCP Server Setup${RESET}"
    echo ""
    check_python
    check_tools
    install_server
    verify_install
    configure_claude
    print_summary
}

main "$@"
