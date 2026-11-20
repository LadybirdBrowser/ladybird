#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

from dataclasses import dataclass
from dataclasses import field
from typing import Any
from typing import Optional

META_SOURCE_DIR = pathlib.Path(__file__).resolve().parent
LADYBIRD_SOURCE_DIR = META_SOURCE_DIR.parent

SEPARATOR = "=" * 80

# Ports that install libraries or headers, yet only run while building.
BUILD_ONLY_PORTS = {"pkgconf"}

# Ports that install neither libraries nor headers, yet end up in what we ship.
DATA_PORTS = {"pdfjs"}

SHIPPED_PORT_DIRECTORIES = {"bin", "Frameworks", "include", "lib"}

LGPL_MARKER = "GNU LESSER GENERAL PUBLIC LICENSE"

DYNAMIC_PORTS_PATH = pathlib.PurePosixPath("Meta/CMake/vcpkg/dynamic-ports.txt")

CRATE_LICENSE_FILE_PATTERN = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense|patents)", re.IGNORECASE)

C_FAMILY_COMMENT_PATTERN = re.compile(r"/\*.*?\*/|(?:^[ \t]*//[^\n]*\n?)+", re.DOTALL | re.MULTILINE)
ASSEMBLY_COMMENT_PATTERN = re.compile(r"(?:^[ \t]*;[^\n]*\n?)+", re.MULTILINE)
SOURCE_COMMENT_PATTERNS = {
    **dict.fromkeys(["c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inc", "m", "mm"], C_FAMILY_COMMENT_PATTERN),
    "asm": ASSEMBLY_COMMENT_PATTERN,
}
LICENSE_NOTICE_PATTERN = re.compile(
    r"copyright|public domain|licensed under|permission is hereby granted|spdx-license-identifier", re.IGNORECASE
)


@dataclass
class Port:
    name: str
    version: str
    copyright: str


@dataclass
class CrateLicense:
    text: str
    crates: list[str] = field(default_factory=list)


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").strip("\n")


def parse_control_paragraphs(path: pathlib.Path) -> list[dict[str, str]]:
    paragraphs = []
    for block in read_text(path).split("\n\n"):
        paragraph = dict(re.findall(r"^([\w-]+): (.*)$", block, re.MULTILINE))
        if paragraph:
            paragraphs.append(paragraph)
    return paragraphs


# NOTE: Whole source files get cut down to their license notices here.
#
# A port's copyright file is whatever its portfile hands to vcpkg_install_copyright(), which joins the files it is given
# under a "<file name>:" heading each. Some ports hand it an entire source file, since the only copy of a notice is in
# a comment of that file: libspng passes spng.c, sdl3 passes stb_image.h and dav1d passes x86inc.asm, which is over
# half a megabyte of code between the three of them.
#
# So of every part whose heading names a C, C++, Objective-C or NASM file, only the comments that carry a copyright or
# license notice are kept, and "(license notices only)" is added to its heading. Everything else is copied as it is:
# parts under any other heading, copyright files without headings, and a source file in which no such comment is found.
# The headings are told apart from the text by their looks alone: a line of its own between blank lines, ending in a
# colon, without spaces, and with a dot or a capital in it, which rules out the labels of C. A line in a source file
# that still looks like one ends the part early, which leaves the rest of that file in place.
def trim_source_files(copyright_text: str) -> str:
    lines = copyright_text.split("\n")

    def is_heading(index: int) -> bool:
        line = lines[index]
        if not line.endswith(":") or " " in line or "/" in line:
            return False
        if "." not in line and line == line.lower():
            return False
        if index > 0 and lines[index - 1] != "":
            return False
        return index + 1 < len(lines) and lines[index + 1] == ""

    headings = [index for index in range(len(lines)) if is_heading(index)]
    if not headings:
        return copyright_text

    parts = []
    if preamble := "\n".join(lines[: headings[0]]).strip("\n"):
        parts.append(preamble)
    for heading, next_heading in zip(headings, headings[1:] + [len(lines)]):
        name = lines[heading][:-1]
        body = "\n".join(lines[heading + 1 : next_heading]).strip("\n")

        if comment_pattern := SOURCE_COMMENT_PATTERNS.get(name.rpartition(".")[2]):
            comments = [match.group(0).strip("\n") for match in comment_pattern.finditer(body)]
            notices = [comment for comment in comments if LICENSE_NOTICE_PATTERN.search(comment)]
            if notices:
                name = f"{name} (license notices only)"
                body = "\n\n".join(notices)

        parts.append(f"{name}:\n\n{body}")

    return "\n\n".join(parts)


def port_is_shipped(name: str, installed_files: list[str], triplet: str) -> bool:
    if name in BUILD_ONLY_PORTS:
        return False
    if name in DATA_PORTS:
        return True

    for installed_file in installed_files:
        parts = pathlib.PurePosixPath(installed_file).parts
        if len(parts) > 2 and parts[0] == triplet and parts[1] in SHIPPED_PORT_DIRECTORIES:
            return True
    return False


def read_dynamic_ports(source_dir: pathlib.Path) -> set[str]:
    lines = read_text(source_dir / DYNAMIC_PORTS_PATH).splitlines()
    return {line.strip() for line in lines if line.strip() and not line.startswith("#")}


def collect_vcpkg_ports(installed_dir: pathlib.Path, triplet: str) -> list[Port]:
    ports = []

    for paragraph in parse_control_paragraphs(installed_dir / "vcpkg" / "status"):
        if "Feature" in paragraph or paragraph.get("Architecture") != triplet:
            continue
        if paragraph.get("Status") != "install ok installed":
            continue

        name = paragraph["Package"]
        version = paragraph["Version"]
        if port_version := paragraph.get("Port-Version"):
            version = f"{version}#{port_version}"

        list_files = list((installed_dir / "vcpkg" / "info").glob(f"{name}_*_{triplet}.list"))
        if len(list_files) != 1:
            raise RuntimeError(f"Expected one file list for the vcpkg port {name}, found {len(list_files)}")
        if not port_is_shipped(name, read_text(list_files[0]).splitlines(), triplet):
            continue

        copyright_file = installed_dir / triplet / "share" / name / "copyright"
        if not copyright_file.is_file():
            raise RuntimeError(f"The vcpkg port {name} installs files we ship, but no copyright file")

        ports.append(Port(name, version, trim_source_files(read_text(copyright_file))))

    return sorted(ports, key=lambda port: port.name.lower())


def cargo_metadata(cargo: str, source_dir: pathlib.Path, rust_target: Optional[str], offline: bool) -> dict[str, Any]:
    command = [cargo, "metadata", "--format-version", "1", "--locked", "--all-features"]
    if rust_target:
        command += ["--filter-platform", rust_target]
    if offline:
        command.append("--offline")

    result = subprocess.run(command, cwd=source_dir, check=True, stdout=subprocess.PIPE)
    return json.loads(result.stdout)


def linked_third_party_crates(metadata: dict[str, Any]) -> list[dict[str, Any]]:
    packages = {package["id"]: package for package in metadata["packages"]}
    nodes = {node["id"]: node for node in metadata["resolve"]["nodes"]}

    visited = set()
    pending = list(metadata["workspace_members"])
    while pending:
        package_id = pending.pop()
        if package_id in visited:
            continue
        visited.add(package_id)

        for dependency in nodes[package_id]["deps"]:
            if any(dep_kind["kind"] is None for dep_kind in dependency["dep_kinds"]):
                pending.append(dependency["pkg"])

    crates = [packages[package_id] for package_id in visited if packages[package_id]["source"] is not None]
    return sorted(crates, key=lambda crate: (crate["name"].lower(), crate["version"]))


def crate_license_files(crate: dict[str, Any]) -> list[pathlib.Path]:
    crate_dir = pathlib.Path(crate["manifest_path"]).parent

    license_files = []
    for entry in sorted(crate_dir.iterdir(), key=lambda entry: entry.name.lower()):
        if not CRATE_LICENSE_FILE_PATTERN.match(entry.name):
            continue
        if entry.is_dir():
            license_files += sorted(path for path in entry.rglob("*") if path.is_file())
        else:
            license_files.append(entry)

    if declared_file := crate.get("license_file"):
        declared_path = (crate_dir / declared_file).resolve()
        if declared_path.is_file() and declared_path not in [path.resolve() for path in license_files]:
            license_files.append(declared_path)

    return license_files


def describe_crate_without_license_file(crate: dict[str, Any]) -> str:
    lines = [
        "This crate does not ship a license file.",
        f"License: {crate['license']}",
    ]
    if authors := crate.get("authors"):
        lines.append(f"Authors: {', '.join(authors)}")
    if repository := crate.get("repository") or crate.get("homepage"):
        lines.append(f"Source: {repository}")
    return "\n".join(lines)


def collect_crate_licenses(crates: list[dict[str, Any]]) -> list[CrateLicense]:
    licenses_by_digest: dict[str, CrateLicense] = {}

    def add(crate: dict[str, Any], text: str) -> None:
        normalized = "\n".join(line.rstrip() for line in text.splitlines()).strip()
        digest = hashlib.sha256(normalized.encode()).hexdigest()
        crate_license = licenses_by_digest.setdefault(digest, CrateLicense(normalized))

        label = f"{crate['name']} {crate['version']}"
        if label not in crate_license.crates:
            crate_license.crates.append(label)

    for crate in crates:
        if crate.get("license") is None and crate.get("license_file") is None:
            raise RuntimeError(f"The crate {crate['name']} {crate['version']} declares no license")

        license_files = crate_license_files(crate)
        if not license_files:
            add(crate, describe_crate_without_license_file(crate))
        for license_file in license_files:
            add(crate, read_text(license_file))

    return sorted(licenses_by_digest.values(), key=lambda crate_license: crate_license.crates[0].lower())


def section(title: str, body: str) -> str:
    return f"{SEPARATOR}\n{title}\n{SEPARATOR}\n\n{body}\n"


def generate(args: argparse.Namespace) -> str:
    ports = []
    if args.vcpkg_installed_dir and args.vcpkg_triplet:
        ports = collect_vcpkg_ports(args.vcpkg_installed_dir, args.vcpkg_triplet)

    metadata = cargo_metadata(args.cargo, args.source_dir, args.rust_target, args.offline)
    crates = linked_third_party_crates(metadata)
    crate_licenses = collect_crate_licenses(crates)

    lgpl_ports = [port for port in ports if LGPL_MARKER in port.copyright]

    dynamic_ports = read_dynamic_ports(args.source_dir)
    if statically_linked_lgpl_ports := [port.name for port in lgpl_ports if port.name not in dynamic_ports]:
        raise RuntimeError(f"LGPL ports missing from {DYNAMIC_PORTS_PATH}: {', '.join(statically_linked_lgpl_ports)}")

    introduction = [
        "Ladybird is distributed under the BSD 2-Clause License below. It includes the third-party software listed",
        "after that, under the licenses reproduced with each of them.",
    ]
    if lgpl_ports:
        introduction += [
            "",
            "This build of Ladybird uses the following libraries under the GNU Lesser General Public License:",
            "",
            *[f"  {port.name} {port.version}" for port in lgpl_ports],
            "",
            "The source code of Ladybird at https://github.com/LadybirdBrowser/ladybird contains everything needed to",
            "build it against modified versions of these libraries, and the build downloads the source code of each",
            "of them in the version listed here.",
        ]

    output = ["\n".join(introduction) + "\n"]
    output.append(section("Ladybird", read_text(args.source_dir / "LICENSE")))

    for port in ports:
        output.append(section(f"{port.name} {port.version}", port.copyright))

    for crate_license in crate_licenses:
        title = "\n".join(f"{crate} (Rust crate)" for crate in crate_license.crates)
        output.append(section(title, crate_license.text))

    return "\n".join(output)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Collects the licenses of Ladybird and of the third-party software a build of it includes"
    )
    parser.add_argument("--output", "-o", required=True, type=pathlib.Path)
    parser.add_argument("--source-dir", type=pathlib.Path, default=LADYBIRD_SOURCE_DIR)
    parser.add_argument("--vcpkg-installed-dir", type=pathlib.Path)
    parser.add_argument("--vcpkg-triplet")
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--rust-target")
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()

    try:
        contents = generate(args)
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(contents.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
