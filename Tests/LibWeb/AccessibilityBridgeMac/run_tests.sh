#!/usr/bin/env bash
#
# Runs the macOS accessibility-bridge regression tests against NSAccessibility/AXUIElement.
#
# Requires: PyObjC frameworks (pyobjc-framework-ApplicationServices, pyobjc-framework-Cocoa) — usually pre-installed
# in macOS's system Python. The shell/IDE running this script needs Accessibility permission granted in
# System Settings → Privacy & Security → Accessibility.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# The harness reads LADYBIRD_BINARY as the path to the Ladybird executable.
# Default to Build/release/bin/Ladybird.app/Contents/MacOS/Ladybird for local runs.
# ctest sets this explicitly via $<TARGET_FILE_DIR:ladybird>.
: "${LADYBIRD_BINARY:=${REPO_ROOT}/Build/release/bin/Ladybird.app/Contents/MacOS/Ladybird}"
export LADYBIRD_BINARY

if [[ ! -x "${LADYBIRD_BINARY}" ]]; then
    echo "error: Ladybird binary not found at ${LADYBIRD_BINARY}" >&2
    echo "       build it with 'ninja -C Build/release ladybird'," >&2
    echo "       or set LADYBIRD_BINARY to the path of your Ladybird executable." >&2
    exit 1
fi

# The harness uses PyObjC's ApplicationServices module. The system /usr/bin/python3 (Apple-supplied) ships with PyObjC
# preinstalled; Homebrew's python3 typically does not. Pick whichever python3 on the system has it. Honor a
# user-supplied PYTHON env var first, then probe a short list of candidates in order.
candidates=()
if [[ -n "${PYTHON:-}" ]]; then
    candidates+=("${PYTHON}")
fi
candidates+=("python3" "/usr/bin/python3" "/Library/Developer/CommandLineTools/usr/bin/python3")

PYTHON_BIN=""
for cand in "${candidates[@]}"; do
    if command -v "${cand}" >/dev/null 2>&1; then
        if "${cand}" -c "import ApplicationServices" 2>/dev/null; then
            PYTHON_BIN="${cand}"
            break
        fi
    fi
done

if [[ -z "${PYTHON_BIN}" ]]; then
    echo "error: no python3 on PATH has the PyObjC ApplicationServices module." >&2
    echo "       The harness uses PyObjC to drive the macOS NSAccessibility / AXUIElement APIs." >&2
    echo >&2
    echo "       Options:" >&2
    echo "         (1) macOS's system Python ships with PyObjC preinstalled. If you have" >&2
    echo "             /usr/bin/python3, set PYTHON=/usr/bin/python3 and re-run." >&2
    echo "         (2) Or pip-install PyObjC into whichever python3 is on PATH:" >&2
    echo "               python3 -m pip install --user pyobjc-framework-ApplicationServices pyobjc-framework-Cocoa" >&2
    exit 1
fi

cd "${SCRIPT_DIR}"
exec "${PYTHON_BIN}" -m unittest discover -s tests -t . -v "$@"
