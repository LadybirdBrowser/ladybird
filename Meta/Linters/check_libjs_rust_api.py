#!/usr/bin/env python3
"""Keeps Rust code outside LibJS on the embedding API of the Rust LibJS runtime.

The libjs_rust crate is the runtime itself, so nearly all of it is pub for the sake of its own modules. The part that
embedders use is the api module, which lists the types, traits and functions the runtime supports an embedder with.
Every path into the crate from outside Libraries/LibJS has to go through the api module, which the generated bindings
of LibWeb follow as well, so the generators are checked for the Rust they write. The crate cannot be given another name
either, as a path through that name would escape the check. Comments and the string literals of Rust code are left
out; the strings of the generators are not, as that is where their Rust is.
"""

import io
import pathlib
import re
import subprocess
import sys
import tokenize

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

CRATE = "libjs_rust"
API_MODULE = "api"

EXEMPT_PATHS = ("Libraries/LibJS/",)

CHECKED_SUFFIXES = {".rs", ".py"}

# The crate's name as a whole word.
CRATE_NAME = re.compile(rf"(?<![A-Za-z0-9_]){CRATE}\b")

# A `::` right before the name that follows a name of its own, which makes the crate's name a segment of a longer
# path, such as a module called libjs_rust in some other crate, rather than the crate. A `::` after a space, as in
# `use ::libjs_rust`, starts an absolute path, whose first segment is the crate.
SEGMENT_OF_A_LONGER_PATH = re.compile(r"[A-Za-z0-9_>)\]]::\s*$")

PATH_SEPARATOR = re.compile(r"\s*::\s*")

RENAME = re.compile(r"\s+as\b")

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

RUST_RAW_STRING_START = re.compile(r"b?r(#*)\"")

RUST_CHARACTER_LITERAL = re.compile(r"'(?:[^'\\\n]|\\(?:[^u]|u\{[0-9a-fA-F]+\}))'")


def tracked_paths():
    output = subprocess.check_output(["git", "ls-files"], cwd=REPO_ROOT, text=True)
    for line in output.splitlines():
        path = pathlib.PurePosixPath(line)
        if not line.startswith(EXEMPT_PATHS) and path.suffix in CHECKED_SUFFIXES:
            yield path


def blanked(text, start, end):
    """The text with the characters of [start, end) other than newlines replaced by spaces, so that every offset and
    line number of what remains is unchanged."""
    return text[:start] + re.sub(r"[^\n]", " ", text[start:end]) + text[end:]


def rust_code_only(text):
    """The Rust source without its comments and its string and character literals."""
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end == -1 else end
        elif text.startswith("/*", index):
            depth = 0
            end = index
            while end < len(text):
                if text.startswith("/*", end):
                    depth += 1
                    end += 2
                elif text.startswith("*/", end):
                    depth -= 1
                    end += 2
                    if depth == 0:
                        break
                else:
                    end += 1
        elif (raw_string := RUST_RAW_STRING_START.match(text, index)) is not None:
            closing = '"' + raw_string.group(1)
            end = text.find(closing, raw_string.end())
            end = len(text) if end == -1 else end + len(closing)
        elif text.startswith('"', index) or text.startswith('b"', index):
            end = text.index('"', index) + 1
            while end < len(text) and text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            end += 1
        elif (character := RUST_CHARACTER_LITERAL.match(text, index)) is not None:
            end = character.end()
        else:
            index += 1
            continue
        text = blanked(text, index, end)
        index = end
    return text


def python_code_only(text):
    """The Python source without its comments. Its strings stay, as the generators write Rust in them."""
    line_offsets = [0]
    for line in text.splitlines(keepends=True):
        line_offsets.append(line_offsets[-1] + len(line))
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, SyntaxError):
        return text
    for token in tokens:
        if token.type == tokenize.COMMENT:
            start = line_offsets[token.start[0] - 1] + token.start[1]
            end = line_offsets[token.end[0] - 1] + token.end[1]
            text = blanked(text, start, end)
    return text


def brace_group(text, start):
    """The text of the balanced `{...}` group that starts at `start`, without the braces, or None if it never closes."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1 : index]
    return None


def split_at_top_level_commas(group):
    items = []
    depth = 0
    item_start = 0
    for index, character in enumerate(group):
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
        elif character == "," and depth == 0:
            items.append(group[item_start:index])
            item_start = index + 1
    items.append(group[item_start:])
    return [item.strip() for item in items if item.strip()]


def names_the_api_module(path):
    """Whether a path relative to the crate is the api module or something in it: `api`, `api::Vm`, `api as js`."""
    match = IDENTIFIER.match(path)
    return match is not None and match.group(0) == API_MODULE


def disallowed_uses(code):
    """Yields `(offset, use)` for every path into the crate that does not go through the api module, and for every
    use of the crate under another name."""
    for match in CRATE_NAME.finditer(code):
        if SEGMENT_OF_A_LONGER_PATH.search(code, 0, match.start()):
            continue
        if (separator := PATH_SEPARATOR.match(code, match.end())) is not None:
            path_start = separator.end()
            if code.startswith("{", path_start):
                group = brace_group(code, path_start)
                if group is None:
                    yield match.start(), code[match.start() : path_start + 1]
                    continue
                for item in split_at_top_level_commas(group):
                    if not names_the_api_module(item):
                        yield match.start(), f"{CRATE}::{{{item}}}"
            elif (segment := IDENTIFIER.match(code, path_start)) is not None:
                if segment.group(0) != API_MODULE:
                    yield match.start(), f"{CRATE}::{segment.group(0)}"
            elif code.startswith("*", path_start):
                yield match.start(), f"{CRATE}::*"
        elif RENAME.match(code, match.end()) is not None:
            yield match.start(), f"{CRATE} as ..."


def main():
    failures = []
    for path in tracked_paths():
        text = (REPO_ROOT / path).read_text(encoding="utf-8", errors="ignore")
        if CRATE not in text:
            continue
        code = rust_code_only(text) if path.suffix == ".rs" else python_code_only(text)
        for offset, use in sorted(disallowed_uses(code)):
            line_number = code.count("\n", 0, offset) + 1
            failures.append(f"{path}:{line_number}: {use}")

    for failure in failures:
        print(failure)
    if failures:
        print()
        print(
            f"Code outside LibJS reaches the Rust runtime through {CRATE}::{API_MODULE}. If the API lacks something, "
            "add it to Libraries/LibJS/Rust/src/api.rs instead of naming the runtime's own modules."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
