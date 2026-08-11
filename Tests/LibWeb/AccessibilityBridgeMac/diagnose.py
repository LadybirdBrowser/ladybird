#!/usr/bin/env python3
"""Diagnostic probe for the macOS AppKit accessibility tests.

Launches Ladybird on the roles.html fixture exactly the way the test harness does, then records, from outside the
app, everything needed to tell WHY an AXWebArea did or did not appear:

  1. whether the helper processes (WebContent, RequestServer, ImageDecoder, ...) spawn, and when;
  2. what the process family logs (captured stderr/stdout, the unified log, and crash reports);
  3. whether a real window exists at the window-server level (CGWindowList), independent of the AX bridge;
  4. whether the app activates and finishes launching (NSRunningApplication);
  5. whether the page loads at all (window screenshot analysis, plus a windowless --headless=text engine run);
  6. the real shape of the AX surface, via a cycle-safe dump with no depth cap.

The probe is diagnostic only: every phase tolerates failure, and the process always exits 0. It runs on healthy
runners too, so a failing runner's output can be diffed against a healthy runner's line by line. The one-line
"AXDIAG:" summary block at the end is the quick diff surface.

Environment:
  LADYBIRD_BINARY                   path to the Ladybird executable (same as the test harness)
  LADYBIRD_AX_DIAG_DIR              directory for bulky outputs (screenshot, samples, unified log); a temp
                                    directory is created when unset
  LADYBIRD_AX_DIAG_WAIT             seconds to watch for the AXWebArea (default 25)
  LADYBIRD_AX_DIAG_HEADLESS_TIMEOUT seconds to allow the --headless=text engine check (default 60)
"""

import datetime
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time
import traceback

try:
    from ApplicationServices import AXIsProcessTrusted
    from ApplicationServices import AXUIElementCopyAttributeValue
    from ApplicationServices import AXUIElementCreateApplication
    from ApplicationServices import AXUIElementCreateSystemWide
    from ApplicationServices import AXUIElementGetPid
    from ApplicationServices import AXUIElementSetMessagingTimeout

    HAVE_AX = True
except Exception:
    HAVE_AX = False

try:
    from CoreFoundation import CFEqual

    HAVE_CF = True
except Exception:
    HAVE_CF = False

try:
    from AppKit import NSBitmapImageRep
    from AppKit import NSRunningApplication
    from AppKit import NSScreen
    from AppKit import NSWorkspace

    HAVE_APPKIT = True
except Exception:
    HAVE_APPKIT = False

try:
    from Quartz import CGDisplayBounds
    from Quartz import CGGetActiveDisplayList
    from Quartz import CGMainDisplayID
    from Quartz import CGSessionCopyCurrentDictionary
    from Quartz import CGWindowListCopyWindowInfo
    from Quartz import kCGNullWindowID
    from Quartz import kCGWindowListOptionAll
    from Quartz import kCGWindowListOptionOnScreenOnly

    HAVE_QUARTZ = True
except Exception:
    HAVE_QUARTZ = False

try:
    from Quartz import CGPreflightScreenCaptureAccess

    HAVE_SCREEN_PREFLIGHT = True
except Exception:
    HAVE_SCREEN_PREFLIGHT = False

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_BINARY = REPO_ROOT / "Build" / "release" / "bin" / "Ladybird.app" / "Contents" / "MacOS" / "Ladybird"
FIXTURE = pathlib.Path(__file__).resolve().parents[1] / "AccessibilityBridge" / "input" / "roles.html"

HELPER_NAMES = ("WebContent", "RequestServer", "ImageDecoder", "Compositor", "WebWorker", "WebDriver")

AX_ERROR_NAMES = {
    0: "success",
    -25200: "failure",
    -25201: "illegalArgument",
    -25202: "invalidUIElement",
    -25203: "invalidUIElementObserver",
    -25204: "cannotComplete",
    -25205: "attributeUnsupported",
    -25206: "actionUnsupported",
    -25207: "notificationUnsupported",
    -25208: "notImplemented",
    -25209: "notificationAlreadyRegistered",
    -25210: "notificationNotRegistered",
    -25211: "apiDisabled",
    -25212: "noValue",
    -25213: "parameterizedAttributeUnsupported",
    -25214: "notEnoughPrecision",
}

SUMMARY = []


def say(text=""):
    print(text, flush=True)


def section(title):
    say()
    say("=" * 100)
    say("== " + title)
    say("=" * 100)


def note_summary(line):
    SUMMARY.append(line)
    say("AXDIAG: " + line)


def run_cmd(argv, timeout=30):
    """Runs a command, never raises. Returns (returncode, stdout, stderr); returncode is None on timeout."""
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        return None, out, err
    except Exception as e:
        return -1, "", "run_cmd(%r) failed: %s" % (argv, e)


def excerpt(text, head_lines=30, tail_lines=30, label="output"):
    lines = text.splitlines()
    if len(lines) <= head_lines + tail_lines:
        return text
    kept = lines[:head_lines] + ["... [%d %s lines omitted] ..." % (len(lines) - head_lines - tail_lines, label)]
    kept += lines[-tail_lines:]
    return "\n".join(kept)


def ax_err_name(err):
    return AX_ERROR_NAMES.get(err, "unknown(%s)" % err)


def ax_read(elem, attribute):
    """Reads one AX attribute. Returns (err, value); err 0 means success. Never raises."""
    if elem is None:
        return -1, None
    try:
        err, value = AXUIElementCopyAttributeValue(elem, attribute, None)
        return err, value
    except Exception:
        return -1, None


def ax_value(elem, attribute):
    return ax_read(elem, attribute)[1]


def elem_token(elem):
    """The AXUIElement's own description: distinguishes the application token from window/content tokens."""
    if elem is None:
        return "None"
    return re.sub(r"\s+", " ", repr(elem))[:130]


def ax_geometry(elem):
    """AXPosition and AXSize as readable text: the AXValue repr buries the numbers in wrapper noise."""
    out = []
    for attribute in ("AXPosition", "AXSize"):
        match = re.search(r"value = ([^}]*?) ?type =", repr(ax_value(elem, attribute)))
        out.append(match.group(1).strip() if match else "?")
    return "pos=(%s) size=(%s)" % (out[0], out[1])


def elem_pid(elem):
    if elem is None:
        return None
    try:
        err, pid = AXUIElementGetPid(elem, None)
        return pid if err == 0 else None
    except Exception:
        return None


def cf_same(a, b):
    if a is None or b is None or not HAVE_CF:
        return False
    try:
        return bool(CFEqual(a, b))
    except Exception:
        return False


def elem_brief(elem):
    if elem is None:
        return "None"
    role_err, role = ax_read(elem, "AXRole")
    title = ax_value(elem, "AXTitle")
    parts = []
    parts.append("role=%s" % (role if role_err == 0 else "<err %s>" % ax_err_name(role_err)))
    if title:
        parts.append("title=%r" % str(title)[:40])
    pid = elem_pid(elem)
    if pid is not None:
        parts.append("pid=%s" % pid)
    return " ".join(parts)


class SeenElements:
    """Visited-set for AXUIElements, using CFEqual: AXUIElement tokens are not hashable across reads."""

    def __init__(self):
        self._items = []

    def index_of(self, elem):
        for i, item in enumerate(self._items):
            if cf_same(item, elem):
                return i
        return -1

    def add(self, elem):
        self._items.append(elem)
        return len(self._items) - 1


def find_web_area(root, max_nodes=400):
    """Cycle-safe BFS for the first AXWebArea under root. Returns (elem, visited_count, cycles_seen)."""
    if root is None:
        return None, 0, 0
    seen = SeenElements()
    queue = [root]
    cycles = 0
    visited = 0
    while queue and visited < max_nodes:
        node = queue.pop(0)
        if seen.index_of(node) != -1:
            cycles += 1
            continue
        seen.add(node)
        visited += 1
        err, role = ax_read(node, "AXRole")
        if err == 0 and role == "AXWebArea":
            return node, visited, cycles
        for child in ax_value(node, "AXChildren") or []:
            queue.append(child)
    return None, visited, cycles


def dump_ax_tree(root, max_nodes=600):
    """Cycle-safe pre-order dump of the AX tree: no depth cap, cycles are marked instead of followed."""
    if root is None:
        say("(no root element to dump)")
        return
    seen = SeenElements()
    err_histogram = {}
    lines_emitted = 0
    stack = [(root, 0)]
    while stack and lines_emitted < max_nodes:
        node, depth = stack.pop()
        indent = "  " * depth
        already = seen.index_of(node)
        if already != -1:
            say("%s(cycle: this element is #%d again)" % (indent, already))
            lines_emitted += 1
            continue
        index = seen.add(node)
        role_err, role = ax_read(node, "AXRole")
        err_histogram[role_err] = err_histogram.get(role_err, 0) + 1
        subrole = ax_value(node, "AXSubrole")
        title = ax_value(node, "AXTitle")
        desc = None
        if role_err != 0:
            desc = "<AXRole read failed: %s>" % ax_err_name(role_err)
        line = "%s#%d %s" % (indent, index, role if role_err == 0 else desc)
        if subrole:
            line += " sub=%s" % subrole
        if title:
            line += " title=%r" % str(title)[:40]
        if role == "AXWindow" or role == "AXWebArea":
            line += " " + ax_geometry(node)
        say(line)
        lines_emitted += 1
        children = ax_value(node, "AXChildren") or []
        for child in reversed(list(children)):
            stack.append((child, depth + 1))
    if stack:
        say("... [dump capped at %d nodes; %d elements still queued]" % (max_nodes, len(stack)))
    say("AX read errors during dump: %s" % {ax_err_name(k): v for k, v in sorted(err_histogram.items())})


def ps_snapshot():
    """All processes: pid -> {ppid, state, cpu, rss_kb, command}."""
    rc, out, _ = run_cmd(["ps", "-axo", "pid=,ppid=,state=,%cpu=,rss=,command="], timeout=15)
    table = {}
    if rc != 0:
        return table
    for line in out.splitlines():
        parts = line.split(None, 5)
        if len(parts) < 6:
            continue
        try:
            pid, ppid = int(parts[0]), int(parts[1])
        except ValueError:
            continue
        table[pid] = {"ppid": ppid, "state": parts[2], "cpu": parts[3], "rss": parts[4], "command": parts[5]}
    return table


def descendants_of(root_pid, table):
    """Transitive children of root_pid, as a sorted list of pids."""
    children = {}
    for pid, row in table.items():
        children.setdefault(row["ppid"], []).append(pid)
    result = []
    queue = [root_pid]
    while queue:
        pid = queue.pop(0)
        for child in sorted(children.get(pid, [])):
            result.append(child)
            queue.append(child)
    return result


def short_name(command):
    return os.path.basename(command.split()[0]) if command else "?"


def ladybird_family(root_pid):
    """(table, descendant pids) for the current process table."""
    table = ps_snapshot()
    return table, descendants_of(root_pid, table)


def preexisting_ladybird_processes():
    table = ps_snapshot()
    return [
        (pid, row["command"][:140])
        for pid, row in sorted(table.items())
        if "Ladybird.app/Contents/MacOS" in row["command"].split()[0]
    ]


def cg_windows_for_pids(pids, onscreen_only=False):
    if not HAVE_QUARTZ:
        return None
    option = kCGWindowListOptionOnScreenOnly if onscreen_only else kCGWindowListOptionAll
    try:
        rows = CGWindowListCopyWindowInfo(option, kCGNullWindowID) or []
    except Exception:
        return None
    pids = set(pids)
    return [row for row in rows if row.get("kCGWindowOwnerPID") in pids]


def describe_cg_window(row):
    bounds = row.get("kCGWindowBounds") or {}
    return "num=%s ownerPID=%s owner=%r layer=%s alpha=%s onscreen=%s bounds=%sx%s@(%s,%s) name=%r store=%s mem=%s" % (
        row.get("kCGWindowNumber"),
        row.get("kCGWindowOwnerPID"),
        row.get("kCGWindowOwnerName"),
        row.get("kCGWindowLayer"),
        row.get("kCGWindowAlpha"),
        bool(row.get("kCGWindowIsOnscreen")),
        bounds.get("Width"),
        bounds.get("Height"),
        bounds.get("X"),
        bounds.get("Y"),
        row.get("kCGWindowName"),
        row.get("kCGWindowStoreType"),
        row.get("kCGWindowMemoryUsage"),
    )


def running_app_state(pid):
    if not HAVE_APPKIT:
        return "AppKit bindings unavailable"
    app = NSRunningApplication.runningApplicationWithProcessIdentifier_(pid)
    if app is None:
        return "no NSRunningApplication for pid %d (the app never checked in with LaunchServices)" % pid
    policy_names = {0: "regular", 1: "accessory", 2: "prohibited"}
    return "finishedLaunching=%s active=%s policy=%s ownsMenuBar=%s bundleID=%s" % (
        bool(app.isFinishedLaunching()),
        bool(app.isActive()),
        policy_names.get(app.activationPolicy(), app.activationPolicy()),
        bool(app.ownsMenuBar()),
        app.bundleIdentifier(),
    )


def phase(title, fn, *args):
    section(title)
    started = time.monotonic()
    try:
        result = fn(*args)
        say("[phase took %.1fs]" % (time.monotonic() - started))
        return result
    except Exception:
        say("PHASE FAILED (continuing):")
        say(traceback.format_exc())
        return None


class Probe:
    def __init__(self):
        self.binary = pathlib.Path(os.environ.get("LADYBIRD_BINARY") or DEFAULT_BINARY)
        self.diag_dir = pathlib.Path(os.environ.get("LADYBIRD_AX_DIAG_DIR") or tempfile.mkdtemp(prefix="ax-diag-"))
        self.wait_seconds = float(os.environ.get("LADYBIRD_AX_DIAG_WAIT", "25"))
        self.headless_timeout = float(os.environ.get("LADYBIRD_AX_DIAG_HEADLESS_TIMEOUT", "60"))
        self.proc = None
        self.app_elem = None
        self.launch_wall_time = None
        self.stdout_path = self.diag_dir / "ladybird-stdout.log"
        self.stderr_path = self.diag_dir / "ladybird-stderr.log"
        self.family_pids = []
        self.largest_window_number = None
        self.web_area_seen_at = None
        self.preexisting = []

    # -- Phase 0 ------------------------------------------------------------------------------------------------

    def report_environment(self):
        say("time                  : %s" % datetime.datetime.now().isoformat())
        say(
            "runner                : RUNNER_NAME=%r ImageOS=%r ImageVersion=%r"
            % (os.environ.get("RUNNER_NAME"), os.environ.get("ImageOS"), os.environ.get("ImageVersion"))
        )
        rc, out, _ = run_cmd(["sw_vers"])
        say("sw_vers               : %s" % " / ".join(out.split()))
        rc, out, _ = run_cmd(["sysctl", "-n", "hw.model", "hw.ncpu", "hw.memsize"])
        say("hardware              : %s" % " / ".join(out.split()))
        say("python                : %s (%s)" % (sys.executable, sys.version.split()[0]))
        say(
            "PyObjC modules        : ApplicationServices=%s CoreFoundation=%s AppKit=%s Quartz=%s"
            % (HAVE_AX, HAVE_CF, HAVE_APPKIT, HAVE_QUARTZ)
        )
        rc, out, _ = run_cmd(["launchctl", "managername"])
        say("launchctl managername : %s" % out.strip())
        rc, out, _ = run_cmd(["stat", "-f%Su", "/dev/console"])
        say("console user          : %s (this process runs as %s)" % (out.strip(), os.environ.get("USER")))
        rc, out, _ = run_cmd(["pgrep", "-x", "WindowServer"])
        say("WindowServer pid      : %s" % (out.strip() or "NOT RUNNING"))
        if HAVE_AX:
            say("AXIsProcessTrusted    : %s" % bool(AXIsProcessTrusted()))
        if HAVE_QUARTZ:
            try:
                err, displays, count = CGGetActiveDisplayList(16, None, None)
                say("active displays       : err=%s count=%s" % (err, count))
                if count:
                    say("main display bounds   : %s" % re.sub(r"\s+", " ", repr(CGDisplayBounds(CGMainDisplayID()))))
            except Exception as e:
                say("active displays       : query failed: %s" % e)
            try:
                session = CGSessionCopyCurrentDictionary()
                say("CGSession dictionary  : %s" % re.sub(r"\s+", " ", repr(session))[:400])
            except Exception as e:
                say("CGSession dictionary  : query failed: %s" % e)
        if HAVE_APPKIT:
            screens = NSScreen.screens() or []
            say(
                "NSScreen.screens      : %d %s"
                % (len(screens), [re.sub(r"\s+", " ", repr(s.frame())) for s in screens])
            )
        if HAVE_SCREEN_PREFLIGHT:
            say("screen capture access : %s" % bool(CGPreflightScreenCaptureAccess()))
        else:
            say("screen capture access : CGPreflightScreenCaptureAccess unavailable in these bindings")
        say("binary                : %s (exists=%s)" % (self.binary, self.binary.exists()))
        say("fixture               : %s (exists=%s)" % (FIXTURE, FIXTURE.exists()))
        say("diagnostics directory : %s" % self.diag_dir)
        leftovers = preexisting_ladybird_processes()
        if leftovers:
            say("PRE-EXISTING Ladybird processes (evidence of an earlier run that did not shut down):")
            for pid, command in leftovers:
                say("  pid %-7d %s" % (pid, command))
        else:
            say("pre-existing Ladybird processes: none")
        self.preexisting = [pid for pid, _ in leftovers]

    # -- Phase 1+2: launch and watch ----------------------------------------------------------------------------

    def launch(self):
        # --temporary-profile, as the harness passes it: a plain launch would hand its URL to a browser already running
        # on the default profile and exit, and the probe has to launch exactly the way the tests do.
        argv = [str(self.binary), "--force-cpu-painting", "--temporary-profile", FIXTURE.resolve().as_uri()]
        say("launching: %s" % " ".join(argv))
        self.launch_wall_time = datetime.datetime.now()
        stdout_file = open(self.stdout_path, "wb")
        stderr_file = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(argv, stdout=stdout_file, stderr=stderr_file)
        say("pid: %d" % self.proc.pid)

    def watch(self):
        """One line per second: process family, launch state, window count, and AX surface, until the AXWebArea
        populates (healthy) or the wait budget runs out (the failure under investigation)."""
        if self.proc is None:
            say("no process to watch")
            return
        pid = self.proc.pid
        if HAVE_AX:
            self.app_elem = AXUIElementCreateApplication(pid)
            try:
                AXUIElementSetMessagingTimeout(self.app_elem, 2.0)
            except Exception:
                pass
        t0 = time.monotonic()
        deadline = t0 + self.wait_seconds
        while True:
            elapsed = time.monotonic() - t0
            exit_code = self.proc.poll()
            if exit_code is not None:
                note_summary("ui-process: EXITED code=%s at t=%.1fs (never stayed up)" % (exit_code, elapsed))
                return
            table, family = ladybird_family(pid)
            self.family_pids = [pid] + family
            me = table.get(pid, {})
            helper_counts = {}
            for child in family:
                name = short_name(table.get(child, {}).get("command", ""))
                helper_counts[name] = helper_counts.get(name, 0) + 1
            helpers = ",".join("%s=%d" % (k, v) for k, v in sorted(helper_counts.items())) or "none"
            cg_all = cg_windows_for_pids(self.family_pids)
            cg_on = cg_windows_for_pids(self.family_pids, onscreen_only=True)
            cg_text = "all=%s on=%s" % (
                len(cg_all) if cg_all is not None else "?",
                len(cg_on) if cg_on is not None else "?",
            )
            app_state = "?"
            if HAVE_APPKIT:
                app = NSRunningApplication.runningApplicationWithProcessIdentifier_(pid)
                if app is None:
                    app_state = "unregistered"
                else:
                    app_state = "finLaunch=%s active=%s" % (
                        "y" if app.isFinishedLaunching() else "n",
                        "y" if app.isActive() else "n",
                    )
            ax_text = "unavailable"
            web_status = "-"
            if self.app_elem is not None:
                win_err, windows = ax_read(self.app_elem, "AXWindows")
                focused_err, focused = ax_read(self.app_elem, "AXFocusedWindow")
                if focused_err != 0:
                    focused_text = "<%s>" % ax_err_name(focused_err)
                else:
                    focused_role = ax_value(focused, "AXRole")
                    focused_text = str(focused_role)
                    if cf_same(focused, self.app_elem):
                        focused_text += "(IS-THE-APP-ELEMENT)"
                ax_text = "windows=%s focused=%s" % (
                    len(windows) if win_err == 0 and windows is not None else "<%s>" % ax_err_name(win_err),
                    focused_text,
                )
                entry = (focused if focused_err == 0 else None) or ax_value(self.app_elem, "AXMainWindow")
                web_via_window, _, _ = find_web_area(entry)
                web_via_app = None
                if web_via_window is None:
                    web_via_app, _, _ = find_web_area(self.app_elem)
                web = web_via_window or web_via_app
                if web is None:
                    web_status = "none"
                else:
                    children = ax_value(web, "AXChildren") or []
                    path = "window-path" if web_via_window is not None else "APP-PATH-ONLY"
                    web_status = "%s children=%d" % (path, len(children))
                    if children:
                        if self.web_area_seen_at is None:
                            self.web_area_seen_at = elapsed
            say(
                "t=%5.1fs  ui: state=%s cpu=%s%%  helpers: %s  app: %s  cgwin: %s  ax: %s  webArea: %s"
                % (elapsed, me.get("state", "?"), me.get("cpu", "?"), helpers, app_state, cg_text, ax_text, web_status)
            )
            if self.web_area_seen_at is not None:
                note_summary(
                    "web-area: POPULATED at t=%.1fs (via %s)"
                    % (self.web_area_seen_at, "window path" if "window-path" in web_status else "app element only")
                )
                return
            if time.monotonic() >= deadline:
                note_summary("web-area: NOT POPULATED within %.0fs (matches the CI failure)" % self.wait_seconds)
                return
            time.sleep(max(0.0, 1.0 - (time.monotonic() - t0 - elapsed)))

    def report_helpers(self):
        if self.proc is None:
            return
        table, family = ladybird_family(self.proc.pid)
        self.family_pids = [self.proc.pid] + family
        say("process family (pid, ppid, state, cpu, rss KiB, command):")
        for pid in self.family_pids:
            row = table.get(pid)
            if row is None:
                say("  pid %-7d (gone)" % pid)
                continue
            say(
                "  pid %-7d ppid %-7d %-4s %5s%% %8s  %s"
                % (pid, row["ppid"], row["state"], row["cpu"], row["rss"], row["command"][:120])
            )
        names = sorted({short_name(table[p]["command"]) for p in family if p in table})
        for expected in ("WebContent", "RequestServer", "ImageDecoder"):
            note_summary("helper %-13s: %s" % (expected, "spawned" if expected in names else "NEVER SPAWNED"))
        extras = [n for n in names if n not in ("WebContent", "RequestServer", "ImageDecoder")]
        if extras:
            note_summary("other helpers      : %s" % ",".join(extras))

    # -- Phase 3: AX deep dive ----------------------------------------------------------------------------------

    def ax_deep_dive(self):
        if self.app_elem is None:
            say("AX bindings or app element unavailable")
            return
        app = self.app_elem
        say("app element token     : %s" % elem_token(app))
        for attribute in (
            "AXRole",
            "AXTitle",
            "AXChildren",
            "AXWindows",
            "AXFocusedWindow",
            "AXMainWindow",
            "AXMenuBar",
            "AXFocusedUIElement",
            "AXFrontmost",
            "AXHidden",
        ):
            err, value = ax_read(app, attribute)
            if err != 0:
                say("app.%-20s: <error %s>" % (attribute, ax_err_name(err)))
            elif attribute in ("AXChildren", "AXWindows"):
                items = list(value or [])
                say("app.%-20s: %d item(s)" % (attribute, len(items)))
                for i, item in enumerate(items[:8]):
                    say("    [%d] %s | %s" % (i, elem_brief(item), elem_token(item)))
            elif attribute in ("AXFocusedWindow", "AXMainWindow", "AXMenuBar", "AXFocusedUIElement"):
                say("app.%-20s: %s | %s" % (attribute, elem_brief(value), elem_token(value)))
            else:
                say("app.%-20s: %r" % (attribute, value))
        focused = ax_value(app, "AXFocusedWindow")
        main = ax_value(app, "AXMainWindow")
        windows = list(ax_value(app, "AXWindows") or [])
        say("identity checks (CFEqual on the AX tokens):")
        say("  focusedWindow is the app element : %s" % cf_same(focused, app))
        say("  mainWindow is the app element    : %s" % cf_same(main, app))
        say("  focusedWindow == mainWindow      : %s" % cf_same(focused, main))
        if windows:
            say("  focusedWindow in AXWindows       : %s" % any(cf_same(focused, w) for w in windows))
        if focused is not None:
            parent = ax_value(focused, "AXParent")
            say("  focusedWindow.AXParent           : %s | %s" % (elem_brief(parent), elem_token(parent)))
            say("  focusedWindow.AXParent is app    : %s" % cf_same(parent, app))
            say("  focusedWindow.AXSubrole          : %r" % ax_value(focused, "AXSubrole"))
        note_summary("ax-focused-window-is-app-element: %s" % cf_same(focused, app))
        try:
            system_wide = AXUIElementCreateSystemWide()
            AXUIElementSetMessagingTimeout(system_wide, 2.0)
            err, focused_app = ax_read(system_wide, "AXFocusedApplication")
            if err == 0:
                say("systemwide focused app: %s | %s" % (elem_brief(focused_app), elem_token(focused_app)))
                say("  it is our Ladybird               : %s" % (elem_pid(focused_app) == self.proc.pid))
            else:
                say("systemwide focused app: <error %s>" % ax_err_name(err))
        except Exception as e:
            say("systemwide focused app: failed: %s" % e)
        say()
        say("full cycle-safe AX tree from the app element (roles; cycles marked, not followed):")
        dump_ax_tree(app)

    # -- Phase 4: window server ---------------------------------------------------------------------------------

    def report_cg_windows(self):
        if not HAVE_QUARTZ:
            say("Quartz bindings unavailable: cannot query the window server")
            return
        rows = cg_windows_for_pids(self.family_pids)
        say("windows owned by the Ladybird process family, per the window server (option: all):")
        if not rows:
            say("  (none)")
        for row in rows or []:
            say("  " + describe_cg_window(row))
        on_rows = cg_windows_for_pids(self.family_pids, onscreen_only=True)
        say("same, on-screen only:")
        if not on_rows:
            say("  (none)")
        best_area = -1
        for row in on_rows or []:
            say("  " + describe_cg_window(row))
            bounds = row.get("kCGWindowBounds") or {}
            area = (bounds.get("Width") or 0) * (bounds.get("Height") or 0)
            layer = row.get("kCGWindowLayer") or 0
            if area > best_area and layer == 0:
                best_area = area
                self.largest_window_number = row.get("kCGWindowNumber")
        try:
            all_rows = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID) or []
            by_name = [r for r in all_rows if "adybird" in str(r.get("kCGWindowOwnerName", ""))]
            other = [r for r in by_name if r.get("kCGWindowOwnerPID") not in set(self.family_pids)]
            if other:
                say("windows owned by OTHER Ladybird processes (not this probe's family):")
                for row in other:
                    say("  " + describe_cg_window(row))
        except Exception:
            pass
        note_summary(
            "cg-windows: family owns %s window(s), %s on-screen, largest-layer0=%s"
            % (
                len(rows) if rows is not None else "?",
                len(on_rows) if on_rows is not None else "?",
                self.largest_window_number,
            )
        )

    # -- Phase 5: NSRunningApplication and frontmost ------------------------------------------------------------

    def report_app_activation(self):
        if self.proc is None:
            return
        state = running_app_state(self.proc.pid)
        say("NSRunningApplication  : %s" % state)
        note_summary("nsrunningapplication: %s" % state)
        if HAVE_APPKIT:
            front = NSWorkspace.sharedWorkspace().frontmostApplication()
            if front is None:
                say("frontmost application : none")
            else:
                say("frontmost application : %s (pid %s)" % (front.localizedName(), front.processIdentifier()))

    # -- Phase 6: native stacks ---------------------------------------------------------------------------------

    def sample_processes(self):
        """/usr/bin/sample: with an empty stderr, the native stacks are the only view of what the family is doing."""
        targets = []
        if self.proc is not None and self.proc.poll() is None:
            targets.append(("Ladybird", self.proc.pid))
        table, family = ladybird_family(self.proc.pid) if self.proc else ({}, [])
        for child in family:
            name = short_name(table.get(child, {}).get("command", ""))
            if name == "WebContent":
                targets.append((name, child))
                break
        if not targets:
            say("nothing alive to sample")
            return
        for name, pid in targets:
            out_path = self.diag_dir / ("sample-%s-%d.txt" % (name, pid))
            say("sampling %s (pid %d) for 2s -> %s" % (name, pid, out_path))
            rc, out, err = run_cmd(["/usr/bin/sample", str(pid), "2", "-file", str(out_path)], timeout=60)
            if rc != 0:
                say("  sample failed rc=%s: %s %s" % (rc, out.strip()[:300], err.strip()[:300]))
                continue
            try:
                text = out_path.read_text(errors="replace")
            except Exception as e:
                say("  could not read sample output: %s" % e)
                continue
            marker = text.find("Call graph:")
            block = text[marker:] if marker != -1 else text
            lines = block.splitlines()[:45]
            say("  ---- first thread(s) of the %s call graph ----" % name)
            for line in lines:
                say("  " + line)
            say("  ---- (full call graph in %s) ----" % out_path.name)

    # -- Phase 7: screenshot ------------------------------------------------------------------------------------

    def screenshot_window(self):
        if self.largest_window_number is None:
            say("no on-screen layer-0 window to capture (see the window-server phase)")
            note_summary("screenshot: SKIPPED, no on-screen window")
            return
        if HAVE_SCREEN_PREFLIGHT and not CGPreflightScreenCaptureAccess():
            say("this process lacks the Screen Recording permission; trying anyway (screencapture will say no)")
        png_path = self.diag_dir / "ladybird-window.png"
        rc, out, err = run_cmd(
            ["/usr/sbin/screencapture", "-x", "-o", "-l", str(self.largest_window_number), str(png_path)],
            timeout=30,
        )
        if rc != 0 or not png_path.exists():
            say("screencapture failed rc=%s stdout=%r stderr=%r" % (rc, out.strip(), err.strip()))
            note_summary("screenshot: FAILED (rc=%s; likely no Screen Recording permission)" % rc)
            return
        say("captured %s (%d bytes)" % (png_path, png_path.stat().st_size))
        verdict = "unanalyzed"
        if HAVE_APPKIT:
            rep = NSBitmapImageRep.imageRepWithContentsOfFile_(str(png_path))
            if rep is None:
                say("NSBitmapImageRep could not read the capture")
            else:
                width, height = int(rep.pixelsWide()), int(rep.pixelsHigh())
                colors = set()
                samples = 24
                for iy in range(samples):
                    for ix in range(samples):
                        x = int((ix + 0.5) * width / samples)
                        y = int((iy + 0.5) * height / samples)
                        color = rep.colorAtX_y_(x, y)
                        if color is None:
                            continue
                        try:
                            r, g, b = color.redComponent(), color.greenComponent(), color.blueComponent()
                        except Exception:
                            value = color.brightnessComponent()
                            r = g = b = value
                        colors.add((round(r * 15), round(g * 15), round(b * 15)))
                whiteish = (15, 15, 15) in colors and len(colors) <= 2
                verdict = "BLANK-ish" if len(colors) <= 2 else "has content"
                say(
                    "pixels: %dx%d, %d distinct colors in a %dx%d sample grid -> %s%s"
                    % (width, height, len(colors), samples, samples, verdict, " (all white)" if whiteish else "")
                )
        note_summary("screenshot: %s (%s)" % (verdict, png_path.name))

    # -- Phase 8: shutdown + captured stdio ---------------------------------------------------------------------

    def shutdown_and_report_stdio(self):
        if self.proc is not None:
            code = self.proc.poll()
            if code is None:
                say("terminating pid %d" % self.proc.pid)
                self.proc.terminate()
                try:
                    code = self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    say("SIGTERM ignored for 5s; killing")
                    self.proc.kill()
                    try:
                        code = self.proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        code = "unkillable"
            say("ui process exit status: %s" % code)
            time.sleep(1.0)
            leftovers = [p for p in self.family_pids[1:] if p in ps_snapshot()]
            if leftovers:
                say("helpers still alive after the UI process died (killing): %s" % leftovers)
                for pid in leftovers:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except OSError:
                        pass
        for label, path in (("stdout", self.stdout_path), ("stderr", self.stderr_path)):
            try:
                data = path.read_bytes()
            except Exception:
                data = b""
            note_summary("captured %s bytes: %d" % (label, len(data)))
            if data:
                text = data.decode(errors="replace")
                say("---- Ladybird family %s (%d bytes) ----" % (label, len(data)))
                say(excerpt(text, 60, 60, label))
                say("---- end %s ----" % label)

    # -- Phase 9: windowless engine check -----------------------------------------------------------------------

    def headless_engine_check(self):
        """Runs the same binary with --headless=text: the whole engine minus AppKit windows. If this passes while
        the windowed run never surfaced an AXWebArea, the failure is in the window/AX layer, not the engine."""
        argv = [str(self.binary), "--headless=text", FIXTURE.resolve().as_uri()]
        say("running: %s (timeout %.0fs)" % (" ".join(argv), self.headless_timeout))
        started = time.monotonic()
        try:
            proc = subprocess.run(argv, capture_output=True, text=True, timeout=self.headless_timeout)
            rc, out, err = proc.returncode, proc.stdout, proc.stderr
        except subprocess.TimeoutExpired as e:
            rc = None
            out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
            run_cmd(["pkill", "-9", "-f", "Ladybird.app/Contents/MacOS"], timeout=10)
        elapsed = time.monotonic() - started
        say("rc=%s in %.1fs; stdout %d bytes, stderr %d bytes" % (rc, elapsed, len(out), len(err)))
        if out:
            say("---- headless stdout ----")
            say(excerpt(out, 40, 20))
        if err:
            say("---- headless stderr ----")
            say(excerpt(err, 40, 40))
        if rc is None:
            note_summary(
                "headless-text: TIMED OUT after %.0fs (engine problem, not a window/AX problem)" % self.headless_timeout
            )
        elif rc == 0 and "First heading" in out:
            note_summary(
                "headless-text: PASS in %.1fs (engine loads the page; any failure is window/AX-side)" % elapsed
            )
        else:
            note_summary("headless-text: FAILED rc=%s (expected the fixture text; see output above)" % rc)

    # -- Phase 10: unified log ----------------------------------------------------------------------------------

    def unified_log(self):
        """The stderr file catching nothing does not mean the family logged nothing: NSLog and os_log go to the
        unified log only. Pull everything the Ladybird.app family said since launch, plus what WindowServer and
        tccd said about it."""
        if self.launch_wall_time is None:
            say("no launch time recorded")
            return
        start = (self.launch_wall_time - datetime.timedelta(seconds=5)).strftime("%Y-%m-%d %H:%M:%S")
        log_path = self.diag_dir / "unified-log-ladybird.txt"
        rc, out, err = run_cmd(
            [
                "log",
                "show",
                "--start",
                start,
                "--info",
                "--debug",
                "--style",
                "compact",
                "--predicate",
                'processImagePath CONTAINS[c] "Ladybird.app"',
            ],
            timeout=180,
        )
        if rc != 0:
            say("log show failed rc=%s: %s" % (rc, err.strip()[:500]))
            note_summary("unified-log: query FAILED rc=%s" % rc)
        else:
            log_path.write_text(out)
            lines = out.splitlines()
            say(
                "unified log since %s for the Ladybird.app family: %d lines (full copy: %s)"
                % (start, len(lines), log_path.name)
            )
            interesting = re.compile(
                r"error|fail|denied|deny|sandbox|violat|crash|abort|assert|accessib|AX", re.IGNORECASE
            )
            hits = [line for line in lines if interesting.search(line)]
            say("%d lines match the interesting-words filter; first 40:" % len(hits))
            for line in hits[:40]:
                say("  " + line[:220])
            say("first and last of the whole stream:")
            say(excerpt(out, 15, 25))
            note_summary(
                "unified-log: %d lines from the family since launch (%d interesting)" % (len(lines), len(hits))
            )
        rc, out, err = run_cmd(
            [
                "log",
                "show",
                "--start",
                start,
                "--style",
                "compact",
                "--predicate",
                '(process == "WindowServer" OR process == "tccd" OR process == "launchservicesd")'
                ' AND eventMessage CONTAINS[c] "ladybird"',
            ],
            timeout=120,
        )
        if rc == 0:
            lines = out.splitlines()
            say("WindowServer/tccd/launchservicesd lines mentioning Ladybird: %d" % len(lines))
            for line in lines[:40]:
                say("  " + line[:220])
        else:
            say("WindowServer/tccd query failed rc=%s: %s" % (rc, err.strip()[:300]))

    # -- Phase 11: crash reports --------------------------------------------------------------------------------

    def crash_reports(self):
        cutoff = time.time() - 60 * 60
        found = []
        for directory in (
            pathlib.Path.home() / "Library/Logs/DiagnosticReports",
            pathlib.Path("/Library/Logs/DiagnosticReports"),
        ):
            try:
                entries = list(directory.iterdir())
            except Exception:
                continue
            for entry in entries:
                if not re.match(r"(Ladybird|WebContent|RequestServer|ImageDecoder|Compositor|WebWorker)", entry.name):
                    continue
                try:
                    if entry.stat().st_mtime < cutoff:
                        continue
                except OSError:
                    continue
                found.append(entry)
        if not found:
            say("no recent crash reports for the Ladybird family")
            note_summary("crash-reports: none in the last 60 minutes")
            return
        note_summary("crash-reports: %d recent file(s): %s" % (len(found), ", ".join(e.name for e in found)))
        for entry in sorted(found):
            say("---- %s ----" % entry)
            try:
                text = entry.read_text(errors="replace")
            except Exception as e:
                say("  unreadable: %s" % e)
                continue
            if entry.suffix == ".ips":
                body_start = text.find("\n")
                try:
                    body = json.loads(text[body_start:])
                    for key in ("procName", "termination", "exception", "faultingThread", "asi"):
                        if key in body:
                            say("  %s: %s" % (key, json.dumps(body[key])[:400]))
                    continue
                except Exception:
                    pass
            say(excerpt(text, 25, 5))

    # -- Phase 12: summary --------------------------------------------------------------------------------------

    def print_summary(self):
        say("Everything below also appeared inline above; this block is the quick healthy-vs-failing diff surface.")
        for line in SUMMARY:
            say("AXDIAG: " + line)


def main():
    say("Ladybird macOS accessibility diagnostic probe")
    try:
        probe = Probe()
        probe.diag_dir.mkdir(parents=True, exist_ok=True)
    except Exception:
        say(traceback.format_exc())
        say("PROBE COMPLETE (setup failed; always exits 0)")
        return 0
    phase("phase 0: environment", probe.report_environment)
    if not probe.binary.exists() or not FIXTURE.exists():
        say("binary or fixture missing; nothing further to probe")
        return 0
    if not HAVE_AX:
        say("PyObjC ApplicationServices unavailable; nothing further to probe")
        return 0
    phase("phase 1+2: launch Ladybird and watch it come up (Q1, Q4 timeline)", probe.launch)
    phase("watch", probe.watch)
    phase("phase 2b: helper process inventory (Q1)", probe.report_helpers)
    phase("phase 3: AX deep dive, cycle-safe full dump (Q6)", probe.ax_deep_dive)
    phase("phase 4: window server truth (Q3)", probe.report_cg_windows)
    phase("phase 5: app activation state (Q4)", probe.report_app_activation)
    phase("phase 6: native thread stacks via sample (Q2)", probe.sample_processes)
    phase("phase 7: window screenshot (Q5)", probe.screenshot_window)
    phase("phase 8: shutdown + captured stdout/stderr (Q2)", probe.shutdown_and_report_stdio)
    phase("phase 9: windowless engine check via --headless=text (Q5)", probe.headless_engine_check)
    phase("phase 10: unified log (Q2)", probe.unified_log)
    phase("phase 11: crash reports (Q2)", probe.crash_reports)
    phase("summary", probe.print_summary)
    say("PROBE COMPLETE (always exits 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
