#!/usr/bin/env bash
# Regenerates the platform projects (Builds/*, JuceLibraryCode/) from NOVA.jucer.
# Run this after a fresh clone, or after adding/removing source files in Projucer.
#
#   ./scripts/resave-nova.sh                 # uses ~/JUCE or $JUCE_PATH
#   JUCE_PATH=/path/to/JUCE ./scripts/resave-nova.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JUCE_PATH="${JUCE_PATH:-$HOME/JUCE}"
PROJUCER="$JUCE_PATH/Projucer.app/Contents/MacOS/Projucer"

if [[ ! -x "$PROJUCER" ]]; then
    echo "Projucer not found at $PROJUCER" >&2
    echo "Install JUCE 8.0.12 (https://juce.com/get-juce) or set JUCE_PATH." >&2
    exit 1
fi

# The project's modules use 'useGlobalPath', so Projucer resolves them from this
# machine-local setting rather than from anything committed to the repo.
"$PROJUCER" --set-global-search-path osx defaultJuceModulePath "$JUCE_PATH/modules"
"$PROJUCER" --resave "$REPO_ROOT/NOVA.jucer"

echo "Projects regenerated. Build with: ./scripts/build-nova.sh"
