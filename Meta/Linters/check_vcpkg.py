#!/usr/bin/env python3

import json
import os
import re
import sys

from typing import Optional

ROOT_VCPKG = "vcpkg.json"
VERSION_WITH_PORT_VERSION_PATTERN = re.compile(r"^.+#[0-9]+$")


def dependency_name(dependency) -> Optional[str]:
    if isinstance(dependency, str):
        return dependency
    if isinstance(dependency, dict):
        return dependency.get("name")
    return None


def collect_dependency_names(vcpkg_json) -> set[str]:
    dependency_names = set()

    for dependency in vcpkg_json.get("dependencies", []):
        name = dependency_name(dependency)
        if name is not None:
            dependency_names.add(name)

    for feature in vcpkg_json.get("features", {}).values():
        for dependency in feature.get("dependencies", []):
            name = dependency_name(dependency)
            if name is not None:
                dependency_names.add(name)

    return dependency_names


def collect_overrides(overrides) -> tuple[dict[str, dict], set[str]]:
    overrides_by_name = {}
    duplicate_override_names = set()

    for override in overrides:
        if not isinstance(override, dict):
            continue

        name = override.get("name")
        if name is None:
            continue

        if name in overrides_by_name:
            duplicate_override_names.add(name)

        overrides_by_name[name] = override

    return overrides_by_name, duplicate_override_names


def check_manifest_dependencies_have_overrides(manifest_path, vcpkg_json) -> bool:
    dependency_names = collect_dependency_names(vcpkg_json)
    overrides_by_name, duplicate_override_names = collect_overrides(vcpkg_json.get("overrides", []))

    dependencies_without_override = sorted(name for name in dependency_names if name not in overrides_by_name)
    overrides_without_version = sorted(
        name for name, override in overrides_by_name.items() if "version" not in override
    )
    overrides_with_invalid_version_format = sorted(
        name
        for name, override in overrides_by_name.items()
        if "version" in override and not VERSION_WITH_PORT_VERSION_PATTERN.match(str(override["version"]))
    )

    has_errors = False

    if duplicate_override_names:
        has_errors = True
        print(f"{manifest_path}: Duplicate override entries found for:", " ".join(sorted(duplicate_override_names)))

    if dependencies_without_override:
        has_errors = True
        print(
            f"{manifest_path}: The following dependencies are missing an override entry with a pinned version:",
            " ".join(dependencies_without_override),
        )

    if overrides_without_version:
        has_errors = True
        print(
            f"{manifest_path}: The following overrides do not define 'version':",
            " ".join(overrides_without_version),
        )

    if overrides_with_invalid_version_format:
        has_errors = True
        print(
            f"{manifest_path}: The following overrides do not use 'version#port-version':",
            " ".join(overrides_with_invalid_version_format),
        )

    return has_errors


def run():
    with open(ROOT_VCPKG, mode="r", encoding="utf-8") as f:
        root_json = json.load(f)

    has_errors = check_manifest_dependencies_have_overrides(ROOT_VCPKG, root_json)

    if has_errors:
        sys.exit(1)


def main():
    file_list = sys.argv[1:] if len(sys.argv) > 1 else [ROOT_VCPKG]

    if ROOT_VCPKG not in file_list:
        return

    run()


if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__) + "/../..")
    main()
