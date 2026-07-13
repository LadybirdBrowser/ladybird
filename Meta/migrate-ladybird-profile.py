#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import shutil
import sys

from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass(frozen=True)
class Migration:
    label: str
    source: Path
    destination: Path


def environment_path(name: str) -> Optional[Path]:
    value = os.environ.get(name)
    if value is None or not value.strip():
        return None
    return Path(value).expanduser()


def platform_roots() -> tuple[Path, Path]:
    home = Path.home()
    config = environment_path("XDG_CONFIG_HOME")
    data = environment_path("XDG_DATA_HOME")

    if sys.platform == "darwin":
        return config or home / "Library" / "Preferences", data or home / "Library" / "Application Support"
    return config or home / ".config", data or home / ".local" / "share"


def migrations() -> list[Migration]:
    config_root, data_root = platform_roots()
    legacy_config = config_root / "Ladybird"
    legacy_data = data_root / "Ladybird"

    return [
        Migration("configuration", legacy_config, config_root / "Ladybird" / "Profiles" / "default"),
        Migration("data", legacy_data, data_root / "Ladybird" / "Profiles" / "default"),
    ]


def migratable_entries(source: Path) -> list[Path]:
    if not source.is_dir():
        return []
    return [entry for entry in source.iterdir() if entry.name != "Profiles"]


def directory_is_nonempty(path: Path) -> bool:
    return path.is_dir() and next(path.iterdir(), None) is not None


def copy_entry(source: Path, destination: Path) -> None:
    if source.is_dir() and not source.is_symlink():
        shutil.copytree(source, destination, symlinks=True)
    elif source.is_symlink():
        destination.symlink_to(os.readlink(source), target_is_directory=source.is_dir())
    else:
        shutil.copy2(source, destination, follow_symlinks=False)


def migrate(migration: Migration) -> None:
    migration.destination.mkdir(parents=True, exist_ok=True)
    for entry in migratable_entries(migration.source):
        copy_entry(entry, migration.destination / entry.name)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Copy Ladybird's legacy configuration and data into the default profile."
    )
    parser.add_argument("--dry-run", action="store_true", help="Show the migration without copying files")
    parser.add_argument("--yes", action="store_true", help="Migrate without asking for confirmation")
    return parser.parse_args()


def main() -> int:
    if sys.platform != "darwin" and not sys.platform.startswith("linux"):
        print("Ladybird profile migration is only supported on macOS and Linux.", file=sys.stderr)
        return 1

    arguments = parse_arguments()
    planned_migrations = migrations()

    if not any(migratable_entries(migration.source) for migration in planned_migrations):
        print("No legacy Ladybird configuration or data was found.", file=sys.stderr)
        return 1

    for migration in planned_migrations:
        if directory_is_nonempty(migration.destination):
            print(
                f"Refusing to overwrite non-empty {migration.label} profile: {migration.destination}",
                file=sys.stderr,
            )
            return 1

    print("Ladybird must be fully closed before migrating.")
    for migration in planned_migrations:
        if migratable_entries(migration.source):
            print(f"  {migration.label}: {migration.source} -> {migration.destination}")
    print("Legacy files will be left in place. Cached data will not be copied.")

    if arguments.dry_run:
        return 0

    if not arguments.yes:
        try:
            confirmation = input("Continue? [y/N] ")
        except EOFError:
            confirmation = ""
        if confirmation.lower() not in ("y", "yes"):
            print("Migration cancelled.")
            return 1

    try:
        for migration in planned_migrations:
            if migratable_entries(migration.source):
                migrate(migration)
    except OSError as error:
        print(f"Migration failed: {error}", file=sys.stderr)
        return 1

    print("Migration complete. Ladybird will now use the default profile.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
