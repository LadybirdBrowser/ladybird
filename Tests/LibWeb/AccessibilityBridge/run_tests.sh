#!/usr/bin/env bash
#
# Runs the accessibility-bridge regression tests. Uses the current session's AT-SPI2 bus + display by default.
# For CI, set CI=1 to spin up a private Xvfb + D-Bus session + AT-SPI2 bus first (see below).
#
# Requires: python3-gi, libatspi-2.0 client (usually installed as part of at-spi2-core).
# For Layer 2 Orca tests: the "orca" Python package must be importable (usually from at-spi2/orca distro package).
# For CI: xorg-x11-server-Xvfb, dbus, at-spi2-core with at-spi-bus-launcher.
#
# By default, runs both Layer 1 (tests/) and Layer 2 (tests_orca/) suites. To run only one or the other:
#
#   ./run_tests.sh --layer=1
#   ./run_tests.sh --layer=2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# The harness reads LADYBIRD_BINARY as the path to the Ladybird executable.
# Default to Build/release/bin/Ladybird for local runs.
# ctest sets this explicitly to $<TARGET_FILE:ladybird>.
: "${LADYBIRD_BINARY:=${REPO_ROOT}/Build/release/bin/Ladybird}"
export LADYBIRD_BINARY

if [[ ! -x "${LADYBIRD_BINARY}" ]]; then
    echo "error: Ladybird binary not found at ${LADYBIRD_BINARY}" >&2
    echo "       build it with 'ninja -C Build/release ladybird'," >&2
    echo "       or set LADYBIRD_BINARY to the path of your Ladybird executable." >&2
    exit 1
fi

# The harness uses PyGObject's gi module to talk to AT-SPI2. The Linux distro packages (python3-gi, gir1.2-atspi-2.0)
# install gi into the system /usr/bin/python3 — but the actions/setup-python interpreter used by GitHub Actions doesn't
# have it. So here, we pick whichever python3 on the system has gi: Honor a user-supplied PYTHON env var first — then
# probe a short list of candidates in order.
candidates=()
if [[ -n "${PYTHON:-}" ]]; then
    candidates+=("${PYTHON}")
fi
candidates+=("python3" "/usr/bin/python3")

PYTHON_BIN=""
for cand in "${candidates[@]}"; do
    if command -v "${cand}" >/dev/null 2>&1; then
        if "${cand}" -c "import gi" 2>/dev/null; then
            PYTHON_BIN="${cand}"
            break
        fi
    fi
done

if [[ -z "${PYTHON_BIN}" ]]; then
    echo "error: no python3 on PATH has the PyGObject (gi) module." >&2
    echo "       The harness uses gi to drive the AT-SPI2 surface." >&2
    echo >&2
    echo "       Options:" >&2
    echo "         (1) Install the distro python3-gi package, which puts gi into /usr/bin/python3." >&2
    echo "             On Debian/Ubuntu: 'sudo apt install python3-gi gir1.2-atspi-2.0'." >&2
    echo "             On Fedora: 'sudo dnf install python3-gobject gir1.2-atspi-2.0'." >&2
    echo "         (2) Or set PYTHON to a python3 that has gi already." >&2
    exit 1
fi

isolated=0
# Any non-empty/non-false value for CI triggers the isolated path. GH Actions sets CI=true; our ctest wrapper sets CI=1.
case "${CI:-}" in
    ""|0|false|False|FALSE) ;;
    *) isolated=1 ;;
esac
layer=both
args=()
for arg in "$@"; do
    case "$arg" in
        --isolated) isolated=1 ;;
        --layer=1) layer=1 ;;
        --layer=2) layer=2 ;;
        --layer=both) layer=both ;;
        *) args+=("$arg") ;;
    esac
done

# Build the discovery paths once.
discover_args=()
case "$layer" in
    1) discover_args=(-s tests -t .) ;;
    2) discover_args=(-s tests_orca -t .) ;;
    both) discover_args=(-s . -t . -p 'test_*.py') ;;
esac

run_unittest() {
    cd "${SCRIPT_DIR}"
    exec "${PYTHON_BIN}" -m unittest discover "${discover_args[@]}" -v "${args[@]}"
}

if (( isolated == 0 )); then
    run_unittest
fi

# Isolated path: fresh X display + D-Bus session + AT-SPI2 bus.
: "${XVFB_DISPLAY:=:99}"
: "${XVFB_SCREEN:=1280x1024x24}"

for tool in Xvfb dbus-run-session; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "error: isolated test run requires ${tool}, which is not on PATH." >&2
        echo "       install the relevant package (xvfb + dbus on Debian/Ubuntu;" >&2
        echo "       xorg-x11-server-Xvfb + dbus-daemon on Fedora), or run the" >&2
        echo "       tests without CI=1 to use the current session's display instead." >&2
        exit 1
    fi
done

XVFB_SOCKET="/tmp/.X11-unix/X${XVFB_DISPLAY#:}"

if [[ ! -S "${XVFB_SOCKET}" ]]; then
    Xvfb "${XVFB_DISPLAY}" -screen 0 "${XVFB_SCREEN}" -nolisten tcp >/tmp/xvfb.log 2>&1 &
    XVFB_PID=$!
    trap 'kill ${XVFB_PID} 2>/dev/null || true' EXIT
    for _ in $(seq 1 30); do
        if [[ -S "${XVFB_SOCKET}" ]]; then break; fi
        sleep 0.1
    done
    if [[ ! -S "${XVFB_SOCKET}" ]]; then
        echo "error: Xvfb failed to come up on ${XVFB_DISPLAY} within 3s." >&2
        echo "       log: /tmp/xvfb.log" >&2
        exit 1
    fi
fi

export DISPLAY="${XVFB_DISPLAY}"
export QT_QPA_PLATFORM=xcb

# Ladybird's IPC sockets are bound under XDG_RUNTIME_DIR (Core::StandardPaths::runtime_directory). On a CI runner with
# no login session, XDG_RUNTIME_DIR is unset and /run/user/<uid> doesn't exist — so the socket bind() fails with ENOENT
# and Ladybird never reaches the AT-SPI2 registration we wait on. dbus-run-session would have the same problem for its
# bus socket. Fix both by pointing XDG_RUNTIME_DIR at a per-run scratch directory we own.
if [[ -z "${XDG_RUNTIME_DIR:-}" ]] || [[ ! -d "${XDG_RUNTIME_DIR}" ]]; then
    XDG_RUNTIME_DIR="$(mktemp -d -t "ladybird-a11y-runtime-XXXXXX")"
    chmod 0700 "${XDG_RUNTIME_DIR}"
    export XDG_RUNTIME_DIR
    LADYBIRD_RUNTIME_DIR_OWNED=1
fi

# Outer EXIT trap: kill anything we own — Xvfb (if we started it), and clean up the runtime dir we created. We don't
# exec into dbus-run-session below; that would wipe this trap and orphan Xvfb past the test's timeout.
cleanup_outer() {
    local rc=$?
    if [[ -n "${XVFB_PID:-}" ]]; then
        kill "${XVFB_PID}" 2>/dev/null || true
    fi
    if [[ -n "${LADYBIRD_RUNTIME_DIR_OWNED:-}" && -n "${XDG_RUNTIME_DIR:-}" ]]; then
        rm -rf "${XDG_RUNTIME_DIR}" 2>/dev/null || true
    fi
    return $rc
}
trap cleanup_outer EXIT

# shellcheck disable=SC2016 # The single-quoted body is intentional — the
# inner bash -c expands its own $SCRIPT_DIR / $at_spi_bus_launcher etc.
dbus-run-session -- bash -c '
    set -euo pipefail

    # Inner EXIT trap: SIGTERM the AT-SPI2 daemons we backgrounded. Without this, when python exits: (1) the daemons
    # remain alive (orphaned to dbus-run-session), (2) nothing reaps them, and (3) the runner hangs until ctest hits its
    # TIMEOUT — which in CI logs, we observed as ~600s of dead time per Layer.
    AT_SPI_BUS_LAUNCHER_PID=""
    AT_SPI2_REGISTRYD_PID=""
    cleanup_inner() {
        local rc=$?
        for pid in "$AT_SPI2_REGISTRYD_PID" "$AT_SPI_BUS_LAUNCHER_PID"; do
            [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
        done
        return $rc
    }
    trap cleanup_inner EXIT

    at_spi_bus_launcher=""
    if command -v /usr/libexec/at-spi-bus-launcher >/dev/null 2>&1; then
        at_spi_bus_launcher=/usr/libexec/at-spi-bus-launcher
    elif command -v at-spi-bus-launcher >/dev/null 2>&1; then
        at_spi_bus_launcher=$(command -v at-spi-bus-launcher)
    fi
    if [[ -n "$at_spi_bus_launcher" ]]; then
        "$at_spi_bus_launcher" --launch-immediately &
        AT_SPI_BUS_LAUNCHER_PID=$!
        for _ in $(seq 1 50); do
            if dbus-send --session --dest=org.freedesktop.DBus --print-reply=literal                 /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner                 string:org.a11y.Bus 2>/dev/null | grep -q "true"; then
                break
            fi
            sleep 0.1
        done
    fi
    if [[ -x /usr/libexec/at-spi2-registryd ]]; then
        /usr/libexec/at-spi2-registryd &
        AT_SPI2_REGISTRYD_PID=$!
    fi

    # Give the registry a moment to claim org.a11y.atspi.Registry on the a11y bus, then dump a one-shot diagnostic of
    # the stack state. If a CI run later fails with "AT-SPI2 desktop children (0)", this snapshot is what tells us which
    # piece is broken: at-spi-bus-launcher missing, registryd not running, a11y bus address unreachable, etc.
    sleep 0.5
    echo "=== AT-SPI2 stack diagnostic (one-shot) ==="
    echo "DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-<unset>}"
    echo "DISPLAY=${DISPLAY:-<unset>}"
    echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<unset>}"
    echo "--- relevant processes ---"
    ps -eo pid,ppid,cmd | grep -Ei "(at-spi|atspi|dbus-daemon|dbus-launch|Xvfb)" | grep -v grep || echo "  (none)"
    echo "--- org.a11y.Bus.GetAddress on session bus ---"
    if command -v gdbus >/dev/null 2>&1; then
        gdbus call --session --dest=org.a11y.Bus --object-path=/org/a11y/bus --method=org.a11y.Bus.GetAddress 2>&1 || echo "  gdbus call failed"
    elif command -v dbus-send >/dev/null 2>&1; then
        dbus-send --session --print-reply --dest=org.a11y.Bus /org/a11y/bus org.a11y.Bus.GetAddress 2>&1 || echo "  dbus-send call failed"
    else
        echo "  (no gdbus or dbus-send on PATH)"
    fi
    echo "--- org.a11y.atspi.Registry presence ---"
    if command -v gdbus >/dev/null 2>&1; then
	# Note: this asks the *session* bus whether it knows the registry name. The registry actually lives on the a11y
	# bus (a separate D-Bus daemon). A session-bus false-result is expected; this is just a sanity print.
        gdbus call --session --dest=org.freedesktop.DBus --object-path=/org/freedesktop/DBus --method=org.freedesktop.DBus.NameHasOwner org.a11y.atspi.Registry 2>&1 || echo "  gdbus name-has-owner failed"
    fi
    echo "=== end diagnostic ==="

    cd "'"${SCRIPT_DIR}"'"
    # Run python (no exec) so the EXIT trap above fires after unittest finishes and reaps the AT-SPI2 daemons.
    '"${PYTHON_BIN}"' -m unittest discover '"${discover_args[*]}"' -v "$@"
' -- "${args[@]}"
