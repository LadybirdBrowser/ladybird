#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# Generate cargo-sources.json for Flatpak builds from Cargo.lock.
# Run this script whenever Rust dependencies change.

import argparse
import json
import re
import sys

from pathlib import Path
from typing import Any

script_dir = Path(__file__).parent
repo_root = script_dir / ".." / ".." / ".."
lock_path = repo_root / "Cargo.lock"
out_path = script_dir / "cargo-sources.json"


def generate(lock: str) -> str:
    crates = []
    for m in re.finditer(
        r'name = "(.+?)"\nversion = "(.+?)"\nsource = "registry.+?"\nchecksum = "(.+?)"',
        lock,
    ):
        crates.append((m.group(1), m.group(2), m.group(3)))

    sources: list[dict[str, Any]] = [
        {
            "type": "git",
            "url": "https://github.com/corrosion-rs/corrosion.git",
            "tag": "v0.5.1",
            "dest": "corrosion",
        },
    ]

    for name, version, checksum in crates:
        sources.append(
            {
                "type": "archive",
                "archive-type": "tar-gzip",
                "url": f"https://static.crates.io/crates/{name}/{name}-{version}.crate",
                "sha256": checksum,
                "dest": f"cargo/vendor/{name}-{version}",
            }
        )

    checksum_commands = []
    for name, version, checksum in crates:
        d = f"cargo/vendor/{name}-{version}"
        checksum_json = json.dumps({"files": {}, "package": checksum})
        checksum_commands.append(f"echo '{checksum_json}' > {d}/.cargo-checksum.json")

    cargo_config = '[source.crates-io]\\nreplace-with = "vendored-sources"\\n\\n[source.vendored-sources]\\ndirectory = "cargo/vendor"\\n'

    sources.append(
        {
            "type": "shell",
            "commands": [
                *checksum_commands,
                "mkdir -p .cargo",
                f"printf '{cargo_config}' > .cargo/config.toml",
            ],
        }
    )

    return json.dumps(sources, indent=4) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Generate cargo-sources.json for Flatpak builds from Cargo.lock.")
    parser.add_argument(
        "--check", action="store_true", help="only check that cargo-sources.json matches Cargo.lock, without writing it"
    )
    args = parser.parse_args()

    sources = generate(lock_path.read_text())
    if args.check:
        if not out_path.exists() or out_path.read_text() != sources:
            script = Path(__file__).resolve().relative_to(repo_root.resolve())
            print(f"{out_path.name} does not match Cargo.lock; run {script} to update it.")
            sys.exit(1)
        return

    out_path.write_text(sources)
    print(f"Generated {out_path}")


if __name__ == "__main__":
    main()
