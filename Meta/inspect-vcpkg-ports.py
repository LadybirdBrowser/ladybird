#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import os
import pathlib
import subprocess
import sys

from typing import Any
from typing import Optional

META_SOURCE_DIR = pathlib.Path(__file__).resolve().parent
LADYBIRD_SOURCE_DIR = META_SOURCE_DIR.parent


def relative_to_source_tree(path: pathlib.Path, source_root: pathlib.Path = LADYBIRD_SOURCE_DIR) -> str:
    try:
        return str(path.resolve().relative_to(source_root))
    except ValueError:
        return str(path)


def read_json(path: pathlib.Path) -> Optional[Any]:
    try:
        with open(path, "r", encoding="utf-8") as file:
            return json.load(file)
    except FileNotFoundError:
        return None


def git_output(repository: pathlib.Path, *args: str) -> Optional[str]:
    if not repository.is_dir():
        return None

    result = subprocess.run(
        ["git", "-C", str(repository), *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode != 0:
        return None

    output = result.stdout.strip()
    return output if output else None


def parse_control_paragraphs(path: pathlib.Path) -> list[dict[str, str]]:
    paragraphs: list[dict[str, str]] = []
    current_paragraph: dict[str, str] = {}
    current_field: Optional[str] = None

    with open(path, "r", encoding="utf-8") as file:
        for raw_line in file:
            line = raw_line.rstrip("\n")

            if not line:
                if current_paragraph:
                    paragraphs.append(current_paragraph)
                current_paragraph = {}
                current_field = None
                continue

            if line[0].isspace():
                if current_field is not None:
                    current_paragraph[current_field] += "\n" + line.strip()
                continue

            field, separator, value = line.partition(":")
            if not separator:
                continue

            current_field = field
            current_paragraph[current_field] = value.strip()

    if current_paragraph:
        paragraphs.append(current_paragraph)

    return paragraphs


def split_control_list(value: Optional[str]) -> list[str]:
    if not value:
        return []

    entries: list[str] = []
    current_entry: list[str] = []
    nesting_depth = 0

    for character in value:
        if character in "([":
            nesting_depth += 1
        elif character in ")]" and nesting_depth > 0:
            nesting_depth -= 1
        elif character == "," and nesting_depth == 0:
            entry = "".join(current_entry).strip()
            if entry:
                entries.append(entry)
            current_entry = []
            continue

        current_entry.append(character)

    entry = "".join(current_entry).strip()
    if entry:
        entries.append(entry)

    return entries


def dependency_name(dependency: Any) -> Optional[str]:
    if isinstance(dependency, str):
        return dependency
    if isinstance(dependency, dict):
        name = dependency.get("name")
        if isinstance(name, str):
            return name
    return None


def collect_manifest_dependency_names(manifest_path: pathlib.Path) -> tuple[set[str], set[str], Optional[str]]:
    manifest = read_json(manifest_path)
    if not isinstance(manifest, dict):
        return set(), set(), None

    direct_dependencies = {
        name
        for name in (dependency_name(dependency) for dependency in manifest.get("dependencies", []))
        if name is not None
    }

    feature_dependencies: set[str] = set()
    for feature in manifest.get("features", {}).values():
        if not isinstance(feature, dict):
            continue
        feature_dependencies.update(
            name
            for name in (dependency_name(dependency) for dependency in feature.get("dependencies", []))
            if name is not None
        )

    builtin_baseline = manifest.get("builtin-baseline")
    if not isinstance(builtin_baseline, str):
        builtin_baseline = None

    return direct_dependencies, feature_dependencies, builtin_baseline


def exact_version(version: Optional[str], port_version: Optional[str]) -> Optional[str]:
    if not version:
        return None
    if port_version and port_version != "0":
        return f"{version}#{port_version}"
    return version


def port_version_value(port_version: Optional[str]) -> int:
    if not port_version:
        return 0
    try:
        return int(port_version)
    except ValueError:
        return 0


def parse_installed_ports(installed_root: pathlib.Path) -> list[dict[str, Any]]:
    status_path = installed_root / "vcpkg" / "status"
    records: dict[tuple[str, str], dict[str, Any]] = {}

    for paragraph in parse_control_paragraphs(status_path):
        if paragraph.get("Status") != "install ok installed":
            continue

        name = paragraph.get("Package")
        triplet = paragraph.get("Architecture")
        if not name or not triplet:
            continue

        key = (name, triplet)
        record = records.setdefault(
            key,
            {
                "name": name,
                "triplet": triplet,
                "version": None,
                "port_version": 0,
                "exact_version": None,
                "abi": None,
                "features": set(),
                "dependencies": set(),
                "base_dependencies": [],
                "feature_dependencies": {},
                "description": None,
            },
        )

        dependencies = split_control_list(paragraph.get("Depends"))
        feature = paragraph.get("Feature")
        if feature:
            record["features"].add(feature)
            record["feature_dependencies"][feature] = dependencies
            record["dependencies"].update(dependencies)
            continue

        version = paragraph.get("Version")
        port_version = paragraph.get("Port-Version")
        record["version"] = version
        record["port_version"] = port_version_value(port_version)
        record["exact_version"] = exact_version(version, port_version)
        record["abi"] = paragraph.get("Abi")
        record["base_dependencies"] = dependencies
        record["dependencies"].update(dependencies)
        record["description"] = paragraph.get("Description")

    ports: list[dict[str, Any]] = []
    for record in records.values():
        record["features"] = sorted(record["features"])
        record["dependencies"] = sorted(record["dependencies"])
        record["feature_dependencies"] = {
            feature: dependencies for feature, dependencies in sorted(record["feature_dependencies"].items())
        }
        ports.append(record)

    return sorted(ports, key=lambda port: (port["triplet"], port["name"]))


def parse_abi_info_file(path: pathlib.Path) -> Optional[dict[str, str]]:
    if not path.is_file():
        return None

    entries: dict[str, str] = {}
    with open(path, "r", encoding="utf-8") as file:
        for raw_line in file:
            line = raw_line.strip()
            if not line:
                continue
            key, separator, value = line.partition(" ")
            if separator:
                entries[key] = value
            else:
                entries[key] = ""

    return entries


def abi_info_for_port(
    source_root: pathlib.Path, vcpkg_root: pathlib.Path, name: str, triplet: str
) -> Optional[dict[str, Any]]:
    path = vcpkg_root / "buildtrees" / name / f"{triplet}.vcpkg_abi_info.txt"
    entries = parse_abi_info_file(path)
    if entries is None:
        return None

    return {
        "path": relative_to_source_tree(path, source_root),
        "entries": entries,
        "features": entries.get("features", "").split(),
        "portfile_sha256": entries.get("portfile.cmake"),
        "port_manifest_sha256": entries.get("vcpkg.json"),
        "ports_cmake_sha256": entries.get("ports.cmake"),
        "triplet_abi": entries.get("triplet_abi"),
    }


def checksum_values(package: dict[str, Any]) -> list[dict[str, str]]:
    checksums = package.get("checksums", [])
    if not isinstance(checksums, list):
        return []

    result: list[dict[str, str]] = []
    for checksum in checksums:
        if not isinstance(checksum, dict):
            continue
        algorithm = checksum.get("algorithm")
        value = checksum.get("checksumValue")
        if isinstance(algorithm, str) and isinstance(value, str):
            result.append({"algorithm": algorithm, "value": value})

    return result


def spdx_info_for_port(
    source_root: pathlib.Path, installed_root: pathlib.Path, name: str, triplet: str
) -> Optional[dict[str, Any]]:
    path = installed_root / triplet / "share" / name / "vcpkg.spdx.json"
    spdx = read_json(path)
    if not isinstance(spdx, dict):
        return None

    packages = spdx.get("packages", [])
    if not isinstance(packages, list):
        packages = []

    package_by_id = {
        package.get("SPDXID"): package for package in packages if isinstance(package, dict) and package.get("SPDXID")
    }

    def package_details(spdx_id: str) -> Optional[dict[str, Any]]:
        package = package_by_id.get(spdx_id)
        if not isinstance(package, dict):
            return None

        return {
            "name": package.get("name"),
            "version": package.get("versionInfo"),
            "download_location": package.get("downloadLocation"),
            "homepage": package.get("homepage"),
            "license": package.get("licenseConcluded"),
            "checksums": checksum_values(package),
        }

    resources: list[dict[str, Any]] = []
    for package in packages:
        if not isinstance(package, dict):
            continue
        spdx_id = package.get("SPDXID")
        if not isinstance(spdx_id, str) or not spdx_id.startswith("SPDXRef-resource-"):
            continue
        resources.append(
            {
                "name": package.get("name"),
                "download_location": package.get("downloadLocation"),
                "checksums": checksum_values(package),
            }
        )

    creation_info = spdx.get("creationInfo", {})
    if not isinstance(creation_info, dict):
        creation_info = {}

    creators = creation_info.get("creators", [])
    if not isinstance(creators, list):
        creators = []

    return {
        "path": relative_to_source_tree(path, source_root),
        "name": spdx.get("name"),
        "created": creation_info.get("created"),
        "creators": creators,
        "port": package_details("SPDXRef-port"),
        "binary": package_details("SPDXRef-binary"),
        "resources": resources,
    }


def configured_overlay_ports(source_root: pathlib.Path) -> list[pathlib.Path]:
    configuration = read_json(source_root / "vcpkg-configuration.json")
    if not isinstance(configuration, dict):
        return []

    overlay_ports = configuration.get("overlay-ports", [])
    if not isinstance(overlay_ports, list):
        return []

    paths: list[pathlib.Path] = []
    for overlay_port in overlay_ports:
        if not isinstance(overlay_port, str):
            continue
        path = pathlib.Path(overlay_port)
        if not path.is_absolute():
            path = source_root / path
        paths.append(path)

    return paths


def port_source_info(source_root: pathlib.Path, vcpkg_root: pathlib.Path, name: str) -> dict[str, Optional[str]]:
    for overlay_path in configured_overlay_ports(source_root):
        if overlay_path.name == name and (overlay_path / "vcpkg.json").is_file():
            return {
                "kind": "overlay",
                "directory": relative_to_source_tree(overlay_path, source_root),
            }

        nested_overlay_path = overlay_path / name
        if (nested_overlay_path / "vcpkg.json").is_file():
            return {
                "kind": "overlay",
                "directory": relative_to_source_tree(nested_overlay_path, source_root),
            }

    builtin_path = vcpkg_root / "ports" / name
    if (builtin_path / "vcpkg.json").is_file():
        return {
            "kind": "builtin",
            "directory": relative_to_source_tree(builtin_path, source_root),
        }

    return {
        "kind": "unknown",
        "directory": None,
    }


def manifest_path_for_installed_root(source_root: pathlib.Path, installed_root: pathlib.Path) -> pathlib.Path:
    manifest_info = read_json(installed_root / "vcpkg" / "manifest-info.json")
    if isinstance(manifest_info, dict):
        manifest_path = manifest_info.get("manifest-path")
        if isinstance(manifest_path, str):
            return pathlib.Path(manifest_path)

    return source_root / "vcpkg.json"


def discover_installed_roots(
    source_root: pathlib.Path, build_dir: pathlib.Path, vcpkg_root: pathlib.Path
) -> list[pathlib.Path]:
    candidates: list[pathlib.Path] = []

    env_installed_root = os.environ.get("VCPKG_INSTALLED_DIR")
    if env_installed_root:
        candidates.append(pathlib.Path(env_installed_root))

    for status_path in build_dir.glob("*/vcpkg_installed/vcpkg/status"):
        candidates.append(status_path.parent.parent)

    manifest_installed_root = source_root / "vcpkg_installed"
    if (manifest_installed_root / "vcpkg" / "status").is_file():
        candidates.append(manifest_installed_root)

    classic_installed_root = vcpkg_root / "installed"
    if (classic_installed_root / "vcpkg" / "status").is_file():
        candidates.append(classic_installed_root)

    unique_candidates: list[pathlib.Path] = []
    seen: set[pathlib.Path] = set()
    for candidate in candidates:
        resolved_candidate = candidate.resolve()
        if resolved_candidate in seen:
            continue
        seen.add(resolved_candidate)
        if (resolved_candidate / "vcpkg" / "status").is_file():
            unique_candidates.append(resolved_candidate)

    return sorted(unique_candidates)


def build_report() -> dict[str, Any]:
    source_root = LADYBIRD_SOURCE_DIR.resolve()
    vcpkg_root = pathlib.Path(os.environ.get("VCPKG_ROOT") or source_root / "Build" / "vcpkg").resolve()
    installed_roots = discover_installed_roots(source_root, source_root / "Build", vcpkg_root)

    if not installed_roots:
        print(
            "No vcpkg install trees found. Set VCPKG_INSTALLED_DIR or run a manifest install first.",
            file=sys.stderr,
        )
        sys.exit(1)

    vcpkg_revision = git_output(vcpkg_root, "rev-parse", "HEAD")
    vcpkg_dirty_status = git_output(vcpkg_root, "status", "--porcelain")

    report = {
        "source_root": str(source_root),
        "vcpkg_root": str(vcpkg_root),
        "vcpkg_root_revision": vcpkg_revision,
        "vcpkg_root_dirty": bool(vcpkg_dirty_status),
        "installed_roots": [],
    }

    for installed_root in installed_roots:
        manifest_path = manifest_path_for_installed_root(source_root, installed_root)
        direct_dependencies, feature_dependencies, builtin_baseline = collect_manifest_dependency_names(manifest_path)

        ports = []
        for port in parse_installed_ports(installed_root):
            name = port["name"]
            triplet = port["triplet"]
            port.update(
                {
                    "declared_in_manifest_dependencies": name in direct_dependencies,
                    "declared_in_manifest_feature_dependencies": name in feature_dependencies,
                    "port_source": port_source_info(source_root, vcpkg_root, name),
                    "abi_info": abi_info_for_port(source_root, vcpkg_root, name, triplet),
                    "spdx": spdx_info_for_port(source_root, installed_root, name, triplet),
                }
            )
            ports.append(port)

        report["installed_roots"].append(
            {
                "path": str(installed_root),
                "manifest_path": str(manifest_path),
                "manifest_builtin_baseline": builtin_baseline,
                "ports": ports,
                "port_count": len(ports),
            }
        )

    return report


def report_has_duplicate_port_keys(report: dict[str, Any]) -> bool:
    keys: set[str] = set()

    for installed_root in report.get("installed_roots", []):
        for port in installed_root.get("ports", []):
            name = port.get("name")
            triplet = port.get("triplet")
            if not name or not triplet:
                continue

            key = f"{triplet}/{name}"
            if key in keys:
                return True
            keys.add(key)

    return False


def report_ports_by_key(report: dict[str, Any], root_qualified: bool) -> dict[str, dict[str, Any]]:
    entries_by_key: dict[str, list[dict[str, Any]]] = {}

    for installed_root in report.get("installed_roots", []):
        installed_root_path = installed_root.get("path", "")
        for port in installed_root.get("ports", []):
            name = port.get("name")
            triplet = port.get("triplet")
            if not name or not triplet:
                continue

            key = f"{triplet}/{name}"
            entries_by_key.setdefault(key, []).append(
                {
                    "installed_root": installed_root_path,
                    "port": port,
                }
            )

    if not root_qualified:
        return {key: entries[0] for key, entries in entries_by_key.items()}

    root_qualified_entries: dict[str, dict[str, Any]] = {}
    for key, entries in entries_by_key.items():
        for entry in entries:
            root_qualified_key = f"{entry['installed_root']}:{key}"
            root_qualified_entries[root_qualified_key] = entry

    return root_qualified_entries


def load_json_report(path: str) -> dict[str, Any]:
    try:
        with open(path, "r", encoding="utf-8") as file:
            report = json.load(file)
    except FileNotFoundError:
        print(f"{path}: file not found", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as error:
        print(f"{path}: invalid JSON: {error}", file=sys.stderr)
        sys.exit(1)

    if not isinstance(report, dict) or not isinstance(report.get("installed_roots"), list):
        print(f"{path}: not a JSON report generated by this script", file=sys.stderr)
        sys.exit(1)

    return report


def port_version(entry: Optional[dict[str, Any]]) -> Optional[str]:
    if entry is None:
        return None

    version = entry["port"].get("exact_version")
    return version if isinstance(version, str) else None


def port_identity(key: str, entry: Optional[dict[str, Any]]) -> dict[str, Optional[str]]:
    if entry is None:
        return {
            "key": key,
            "name": key.rsplit("/", 1)[-1],
            "triplet": None,
        }

    port = entry["port"]
    return {
        "key": key,
        "name": port.get("name"),
        "triplet": port.get("triplet"),
    }


def build_diff(old_path: str, new_path: str) -> dict[str, Any]:
    old_report = load_json_report(old_path)
    new_report = load_json_report(new_path)
    root_qualified = report_has_duplicate_port_keys(old_report) or report_has_duplicate_port_keys(new_report)
    old_ports = report_ports_by_key(old_report, root_qualified)
    new_ports = report_ports_by_key(new_report, root_qualified)

    version_changes = []
    for key in sorted(set(old_ports) | set(new_ports)):
        old_entry = old_ports.get(key)
        new_entry = new_ports.get(key)
        old_version = port_version(old_entry)
        new_version = port_version(new_entry)
        if old_version == new_version:
            continue

        identity = port_identity(key, new_entry or old_entry)
        version_changes.append(
            {
                **identity,
                "old_version": old_version,
                "new_version": new_version,
            }
        )

    return {
        "old_report": old_path,
        "new_report": new_path,
        "summary": {
            "version_changes": len(version_changes),
        },
        "version_changes": version_changes,
    }


def format_version(value: Any) -> str:
    return "-" if value is None else str(value)


def diff_text(diff: dict[str, Any]) -> str:
    rows = [
        [change["name"], format_version(change["old_version"]), format_version(change["new_version"])]
        for change in diff["version_changes"]
    ]
    if not rows:
        return "No version changes."

    lines = []
    headers = ["Port", "Old version", "New version"]
    widths = [len(header) for header in headers]
    for row in rows:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))

    lines.append("  ".join(header.ljust(widths[index]) for index, header in enumerate(headers)))
    lines.append("  ".join("-" * width for width in widths))
    for row in rows:
        lines.append("  ".join(value.ljust(widths[index]) for index, value in enumerate(row)))

    return "\n".join(lines)


def table_text(report: dict[str, Any]) -> str:
    lines: list[str] = []
    source_root = pathlib.Path(report["source_root"])
    vcpkg_revision = report.get("vcpkg_root_revision") or "unknown"
    dirty_suffix = " dirty" if report.get("vcpkg_root_dirty") else ""
    lines.append(
        f"VCPKG_ROOT: {relative_to_source_tree(pathlib.Path(report['vcpkg_root']), source_root)} "
        f"({vcpkg_revision}{dirty_suffix})"
    )

    installed_roots = report.get("installed_roots", [])
    for installed_root in installed_roots:
        lines.append("")
        lines.append(f"Installed root: {relative_to_source_tree(pathlib.Path(installed_root['path']), source_root)}")
        lines.append(f"Manifest: {relative_to_source_tree(pathlib.Path(installed_root['manifest_path']), source_root)}")
        if installed_root.get("manifest_builtin_baseline"):
            lines.append(f"Manifest baseline: {installed_root['manifest_builtin_baseline']}")
        lines.append(f"Installed ports: {installed_root['port_count']}")
        lines.append("")

        rows = []
        for port in installed_root["ports"]:
            spdx = port.get("spdx") or {}
            resources = spdx.get("resources") or []
            abi = port.get("abi") or ""
            abi_info = port.get("abi_info") or {}
            rows.append(
                [
                    port["name"],
                    port["triplet"],
                    port.get("exact_version") or "-",
                    ",".join(port.get("features") or []) or "-",
                    abi[:12] if abi else "-",
                    (port.get("port_source") or {}).get("kind") or "-",
                    str(len(resources)),
                    "yes" if abi_info else "no",
                ]
            )

        headers = ["Port", "Triplet", "Version", "Features", "ABI", "Source", "Resources", "ABI info"]
        widths = [len(header) for header in headers]
        for row in rows:
            for index, value in enumerate(row):
                widths[index] = max(widths[index], len(value))

        lines.append("  ".join(header.ljust(widths[index]) for index, header in enumerate(headers)))
        lines.append("  ".join("-" * width for width in widths))
        for row in rows:
            lines.append("  ".join(value.ljust(widths[index]) for index, value in enumerate(row)))

    return "\n".join(lines)


def write_json_output(path: str, payload: dict[str, Any]) -> None:
    output_path = pathlib.Path(path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_output(args: argparse.Namespace, report: dict[str, Any]) -> None:
    if args.output:
        write_json_output(args.output, report)
        return

    print(table_text(report))


def write_diff_output(args: argparse.Namespace, diff: dict[str, Any]) -> None:
    if args.output:
        write_json_output(args.output, diff)
        return

    print(diff_text(diff))


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Report exact versions of installed vcpkg ports from manifest-mode install trees, "
            "including transitive ports."
        )
    )
    parser.add_argument(
        "--diff",
        nargs=2,
        metavar=("OLD_JSON", "NEW_JSON"),
        help="Compare port versions in two JSON reports previously exported by this script.",
    )
    parser.add_argument(
        "--output",
        help="Write JSON output to this path instead of printing a table to stdout.",
    )

    args = parser.parse_args()

    if args.diff:
        diff = build_diff(args.diff[0], args.diff[1])
        write_diff_output(args, diff)
        return

    report = build_report()
    write_output(args, report)


if __name__ == "__main__":
    main()
