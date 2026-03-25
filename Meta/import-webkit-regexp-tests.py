#!/usr/bin/env python3
"""Import WebKit LayoutTests regex tests into Ladybird's test-js format.

Usage: python3 Meta/import-webkit-regexp-tests.py /path/to/WebKit

This script imports WebKit's LayoutTests/fast/regex/ test files, wrapping
them in Ladybird's test()/expect() harness format with a compatibility
shim for WebKit's shouldBe/shouldBeTrue/etc assertion functions.
"""

import os
import shutil
import sys

WEBKIT_COMPAT_SHIM = """\
// WebKit assertion compatibility shim for Ladybird's test-js harness

function description(msg) {
    // No-op, just used for test documentation in WebKit.
}

function shouldBe(actual_code, expected_code) {
    let actual = eval(actual_code);
    let expected = eval(expected_code);
    if (typeof actual === "string" && typeof expected === "string") {
        expect(actual).toBe(expected);
    } else if (Array.isArray(actual) && Array.isArray(expected)) {
        expect(actual).toEqual(expected);
    } else if (
        actual !== null &&
        typeof actual === "object" &&
        expected !== null &&
        typeof expected === "object"
    ) {
        expect(actual).toEqual(expected);
    } else {
        expect(actual).toBe(expected);
    }
}

function shouldBeTrue(code) {
    expect(eval(code)).toBeTrue();
}

function shouldBeFalse(code) {
    expect(eval(code)).toBeFalse();
}

function shouldBeNull(code) {
    expect(eval(code)).toBeNull();
}

function shouldBeUndefined(code) {
    expect(eval(code)).toBeUndefined();
}

function shouldThrow(code, expected_error) {
    expect(() => eval(code)).toThrow();
}

function shouldNotThrow(code) {
    eval(code);
}
"""

SRC_DIR = "LayoutTests/fast/regex/script-tests"
DEST_DIR = "Tests/LibJS/Runtime/3rdparty/webkit"

# Files to skip entirely.
SKIP_FILES = {
    "TEMPLATE.html",
}

# Tests that crash or hang -- use test.skip().
SKIP_TESTS = {
    # Crashes (SIGSEGV).
    "pcre-test-1",
}

# Tests that fail -- use test.xfail().
XFAIL_TESTS = {
    "backreferences",
    "dotstar",
    "malformed-escapes",
    "non-pattern-characters",
    "overflow",
    "quantified-assertions",
    "repeat-match-waldemar",
    "slow",
}


def find_tests(webkit_dir):
    """Find all regex test JS files in WebKit's LayoutTests."""
    src = os.path.join(webkit_dir, SRC_DIR)
    files = []
    for f in sorted(os.listdir(src)):
        if f.endswith(".js") and f not in SKIP_FILES:
            files.append(os.path.join(src, f))
    return files


def extract_copyright_header(content):
    """Extract any copyright/license header."""
    lines = content.split("\n")
    header_lines = []
    for line in lines:
        if line.startswith("//"):
            header_lines.append(line)
        elif line.strip() == "":
            if header_lines:
                header_lines.append(line)
        else:
            break
    # Remove trailing empty lines
    while header_lines and header_lines[-1].strip() == "":
        header_lines.pop()
    return "\n".join(header_lines)


def extract_body(content):
    """Extract the test body after any header."""
    lines = content.split("\n")
    body_start = 0
    in_header = True
    for i, line in enumerate(lines):
        if in_header:
            if line.startswith("//") or line.strip() == "":
                continue
            else:
                in_header = False
                body_start = i
                break
    return "\n".join(lines[body_start:])


def convert_file(src_path):
    """Convert a WebKit test file to Ladybird format."""
    with open(src_path) as f:
        content = f.read()

    filename = os.path.basename(src_path)
    name = filename.replace(".js", "")

    header = extract_copyright_header(content)
    body = extract_body(content)

    # Determine test wrapper
    if name in SKIP_TESTS:
        wrapper = "test.skip"
    elif name in XFAIL_TESTS:
        wrapper = "test.xfail"
    else:
        wrapper = "test"

    # Build the converted file.
    # The shim MUST be inside the test() callback so that eval() in
    # shouldBe/shouldBeTrue can access variables defined in the test body.
    result = ""
    if header:
        result += header + "\n\n"
    result += f'{wrapper}("{name}", () => {{\n'

    # Indent and insert shim + body inside the test callback
    for line in WEBKIT_COMPAT_SHIM.split("\n"):
        if line.strip():
            result += "    " + line + "\n"
        else:
            result += "\n"

    result += "\n"

    for line in body.split("\n"):
        if line.strip():
            result += "    " + line + "\n"
        else:
            result += "\n"

    result += "});\n"
    return result, wrapper


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/WebKit", file=sys.stderr)
        sys.exit(1)

    webkit_dir = sys.argv[1]
    src = os.path.join(webkit_dir, SRC_DIR)
    if not os.path.isdir(src):
        print(f"Error: {src} not found", file=sys.stderr)
        sys.exit(1)

    # Clean destination
    if os.path.exists(DEST_DIR):
        shutil.rmtree(DEST_DIR)

    os.makedirs(DEST_DIR, exist_ok=True)

    files = find_tests(webkit_dir)
    counts = {"test": 0, "test.skip": 0, "test.xfail": 0}

    for src_path in files:
        converted, status = convert_file(src_path)
        filename = os.path.basename(src_path)
        dest_path = os.path.join(DEST_DIR, filename)

        with open(dest_path, "w") as f:
            f.write(converted)

        counts[status] += 1
        tag = "" if status == "test" else f" [{status}]"
        rel = os.path.relpath(dest_path)
        print(f"  OK: {rel}{tag}")

    print(f"\nImported: {counts['test']} pass, {counts['test.xfail']} xfail, {counts['test.skip']} skip")


if __name__ == "__main__":
    main()
