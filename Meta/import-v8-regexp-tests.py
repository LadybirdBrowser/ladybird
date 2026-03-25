#!/usr/bin/env python3
"""Import V8 mjsunit regexp tests into Ladybird's test-js format.

Usage: python3 Meta/import-v8-regexp-tests.py /path/to/v8

This script imports V8's regexp test files that don't depend on V8-specific
native builtins, wrapping them in Ladybird's test()/expect() harness format
with a compatibility shim for V8's assertion functions.
"""

import os
import re
import shutil
import sys

V8_COMPAT_SHIM = """\
// V8 assertion compatibility shim for Ladybird's test-js harness

function assertEquals(expected, actual, msg) {
    if (Array.isArray(expected) && Array.isArray(actual)) {
        expect(actual).toEqual(expected);
    } else if (expected !== null && typeof expected === "object" && actual !== null && typeof actual === "object") {
        expect(actual).toEqual(expected);
    } else {
        expect(actual).toBe(expected);
    }
}

function assertTrue(val, msg) {
    expect(val).toBeTrue();
}

function assertFalse(val, msg) {
    expect(val).toBeFalse();
}

function assertNull(val, msg) {
    expect(val).toBeNull();
}

function assertNotNull(val, msg) {
    expect(val).not.toBeNull();
}

function assertThrows(fn, type_opt, msg_opt) {
    expect(fn).toThrow();
}

function assertDoesNotThrow(fn, msg) {
    fn();
}

function assertInstanceof(val, type, msg) {
    expect(val instanceof type).toBeTrue();
}

function assertUnreachable(msg) {
    expect().fail("unreachable" + (msg ? ": " + msg : ""));
}

function assertEarlyError(code) {
    assertThrows(() => new Function(code));
}

function assertThrowsAtRuntime(code, type_opt) {
    const f = new Function(code);
    assertThrows(f, type_opt);
}

function assertArrayEquals(expected, actual) {
    expect(actual).toEqual(expected);
}
"""

DEST_DIR = "Tests/LibJS/Runtime/3rdparty/v8"

# Files that use V8-specific native syntax (%Foo, --allow-natives-syntax)
# and cannot be imported as-is.
SKIP_PATTERNS = [
    r"allow-natives-syntax",
    r"%[A-Z]",
]

# Files to skip entirely (not importable at all).
SKIP_FILES = {
    # Performance benchmarks, not correctness tests.
    "ascii-regexp-subject.js",
    "string-slices-regexp.js",
    # Uses V8's non-standard /l (linear) flag in regex literals, which causes
    # parse errors in other engines.
    "regexp-14098.js",
    "regexp-444637793.js",
    # Uses regex literal syntax that triggers parse errors in our engine.
    "regexp-unicode-sets.js",
    # Multi-file dependency (lu-ui0..9 depend on testCodePointRange from lu-ui).
    "harmony/regexp-property-lu-ui0.js",
    "harmony/regexp-property-lu-ui1.js",
    "harmony/regexp-property-lu-ui2.js",
    "harmony/regexp-property-lu-ui3.js",
    "harmony/regexp-property-lu-ui4.js",
    "harmony/regexp-property-lu-ui5.js",
    "harmony/regexp-property-lu-ui6.js",
    "harmony/regexp-property-lu-ui7.js",
    "harmony/regexp-property-lu-ui8.js",
    "harmony/regexp-property-lu-ui9.js",
}

# Tests that crash or hang -- use test.skip() so they don't block the suite.
SKIP_TESTS = {
    # Hangs: catastrophic backtracking tests that rely on V8-specific
    # optimizations to terminate in reasonable time.
    "regexp-capture-3",
    # Crashes (SIGSEGV).
    "regexp-capture",
}

# Tests that fail but should be tracked -- use test.xfail() so we notice
# when they start passing.
XFAIL_TESTS = {
    "es6/regexp-constructor",
    "es6/regexp-tostring",
    "es6/unicode-escapes-in-regexps",
    "es6/unicode-regexp-backrefs",
    "es6/unicode-regexp-ignore-case-noi18n",
    "es6/unicode-regexp-restricted-syntax",
    "es6/unicode-regexp-zero-length",
    "regexp-duplicate-named-groups",
    "regexp-lookahead",
    "regexp-multiline",
    "regexp-sort",
    "regexp-UC16",
    "harmony/regexp-property-binary",
    "harmony/regexp-property-char-class",
    "harmony/regexp-property-enumerated",
    "harmony/regexp-property-exact-match",
    "harmony/regexp-property-general-category",
    "harmony/regexp-property-invalid",
    "harmony/regexp-property-special",
}


def should_skip(content):
    for pattern in SKIP_PATTERNS:
        if re.search(pattern, content):
            return True
    return False


def file_key(subdir, filename):
    """Get the key used in SKIP_FILES."""
    if subdir:
        return f"{subdir}/{filename}"
    return filename


def test_name(subdir, filename):
    """Get the test name (no .js extension)."""
    name = filename.replace(".js", "")
    if subdir:
        return f"{subdir}/{name}"
    return name


def find_regexp_tests(v8_dir):
    """Find all regexp-related test files in V8's mjsunit directory."""
    mjsunit = os.path.join(v8_dir, "test", "mjsunit")
    files = []

    # Top-level regexp files
    for f in sorted(os.listdir(mjsunit)):
        if f.endswith(".js") and "regexp" in f.lower():
            files.append(("", os.path.join(mjsunit, f)))

    # es6/ subdirectory
    es6_dir = os.path.join(mjsunit, "es6")
    if os.path.isdir(es6_dir):
        for f in sorted(os.listdir(es6_dir)):
            if f.endswith(".js") and ("regexp" in f.lower() or "unicode-regexp" in f.lower()):
                files.append(("es6", os.path.join(es6_dir, f)))

    # harmony/ subdirectory
    harmony_dir = os.path.join(mjsunit, "harmony")
    if os.path.isdir(harmony_dir):
        for f in sorted(os.listdir(harmony_dir)):
            if f.endswith(".js") and "regexp" in f.lower():
                files.append(("harmony", os.path.join(harmony_dir, f)))

    return files


def extract_copyright_header(content):
    """Extract the BSD copyright header from the file."""
    lines = content.split("\n")
    header_lines = []
    for line in lines:
        if line.startswith("//"):
            header_lines.append(line)
        else:
            break
    return "\n".join(header_lines)


def extract_body(content):
    """Extract the test body (everything after the copyright header and flags comment)."""
    lines = content.split("\n")
    body_start = 0
    for i, line in enumerate(lines):
        if line.startswith("//"):
            continue
        if line.strip() == "":
            body_start = i + 1
            continue
        body_start = i
        break
    return "\n".join(lines[body_start:])


def convert_file(subdir, src_path):
    """Convert a V8 test file to Ladybird format."""
    with open(src_path) as f:
        content = f.read()

    filename = os.path.basename(src_path)

    if file_key(subdir, filename) in SKIP_FILES:
        return None, "skip-file"

    if should_skip(content):
        return None, "v8-natives"

    name = test_name(subdir, filename)
    header = extract_copyright_header(content)
    body = extract_body(content)

    # Determine test wrapper
    if name in SKIP_TESTS:
        wrapper = "test.skip"
    elif name in XFAIL_TESTS:
        wrapper = "test.xfail"
    else:
        wrapper = "test"

    # Build the converted file
    result = header + "\n\n"
    result += V8_COMPAT_SHIM + "\n"
    result += f'{wrapper}("{name}", () => {{\n'

    # Indent the body
    for line in body.split("\n"):
        if line.strip():
            result += "    " + line + "\n"
        else:
            result += "\n"

    result += "});\n"
    return result, wrapper


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/v8", file=sys.stderr)
        sys.exit(1)

    v8_dir = sys.argv[1]
    if not os.path.isdir(os.path.join(v8_dir, "test", "mjsunit")):
        print(f"Error: {v8_dir}/test/mjsunit not found", file=sys.stderr)
        sys.exit(1)

    # Clean destination
    if os.path.exists(DEST_DIR):
        shutil.rmtree(DEST_DIR)

    os.makedirs(DEST_DIR, exist_ok=True)
    os.makedirs(os.path.join(DEST_DIR, "es6"), exist_ok=True)
    os.makedirs(os.path.join(DEST_DIR, "harmony"), exist_ok=True)

    files = find_regexp_tests(v8_dir)
    counts = {"test": 0, "test.skip": 0, "test.xfail": 0, "skipped": 0}

    for subdir, src_path in files:
        converted, status = convert_file(subdir, src_path)
        filename = os.path.basename(src_path)
        key = file_key(subdir, filename)

        if converted is None:
            counts["skipped"] += 1
            print(f"  SKIP ({status}): {key}")
            continue

        if subdir:
            dest_path = os.path.join(DEST_DIR, subdir, filename)
        else:
            dest_path = os.path.join(DEST_DIR, filename)

        with open(dest_path, "w") as f:
            f.write(converted)

        counts[status] += 1
        tag = "" if status == "test" else f" [{status}]"
        rel = os.path.relpath(dest_path)
        print(f"  OK: {rel}{tag}")

    print(
        f"\nImported: {counts['test']} pass, {counts['test.xfail']} xfail, "
        f"{counts['test.skip']} skip, {counts['skipped']} not imported"
    )


if __name__ == "__main__":
    main()
