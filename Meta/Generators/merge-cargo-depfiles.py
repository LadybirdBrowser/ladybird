#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import subprocess

from pathlib import Path

LADYBIRD_SOURCE_DIR = Path(__file__).resolve().parent.parent.parent
FLAP_SOURCE_DIR = LADYBIRD_SOURCE_DIR / "Libraries" / "LibJS" / "Flap"


def dependencies_from(depfile):
    for rule in depfile.read_text().replace("\\\n", "").splitlines():
        dependencies = split_rule(rule)
        if dependencies is not None:
            yield from split_words(dependencies)


def split_rule(rule):
    escaped = False
    for index, character in enumerate(rule):
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == ":":
            return rule[index + 1 :]
    return None


def split_words(words):
    word = []
    escaped = False
    for character in words:
        if escaped:
            word.append(character)
            escaped = False
        elif character == "\\":
            escaped = True
        elif character.isspace():
            if word:
                yield "".join(word)
                word = []
        else:
            word.append(character)
    if escaped:
        word.append("\\")
    if word:
        yield "".join(word)


def is_within(directory, path):
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def escape_depfile_path(path):
    return (
        str(path).replace("$", "$$").replace("\\", "\\\\").replace(" ", "\\ ").replace("#", "\\#").replace(":", "\\:")
    )


def cargo_manifests():
    manifests = subprocess.check_output(
        ["git", "-C", str(LADYBIRD_SOURCE_DIR), "ls-files", "-z", "--", "Cargo.toml", "**/Cargo.toml"]
    ).split(b"\0")

    return {(LADYBIRD_SOURCE_DIR / Path(manifest.decode())).resolve() for manifest in manifests if manifest}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--stamp", type=Path)
    parser.add_argument("--target", type=Path, action="append", default=[])
    args = parser.parse_args()

    sources = [LADYBIRD_SOURCE_DIR, FLAP_SOURCE_DIR]
    dependencies = cargo_manifests()

    for target in args.target:
        for depfile in target.rglob("*.d"):
            for dependency in dependencies_from(depfile):
                path = Path(dependency)
                candidates = [path] if path.is_absolute() else [directory / path for directory in sources]

                for candidate in candidates:
                    candidate = candidate.resolve()

                    if candidate.exists() and is_within(LADYBIRD_SOURCE_DIR, candidate):
                        dependencies.add(candidate)
                        break

    escaped_dependencies = " ".join(escape_depfile_path(path) for path in sorted(dependencies))
    args.output.write_text(f"{escape_depfile_path(args.stamp)}: {escaped_dependencies}\n")


if __name__ == "__main__":
    main()
