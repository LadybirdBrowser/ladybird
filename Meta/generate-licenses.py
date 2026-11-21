#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import functools
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request

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

# Release-tools publishes the source archives of the LGPL ports there, for every release that ships them.
THIRD_PARTY_SOURCES_URL = "https://download.ladybird.org/third-party"

# Ladybird's vcpkg asset cache, which holds the source downloads of the ports by their SHA-512.
VCPKG_ASSET_CACHE_URL = "https://vcpkg-cache.app.ladybird.org/ladybird/source-assets"

GITHUB_SOURCE_PATTERN = re.compile(r"^git\+https://github\.com/(?P<repository>[^/@]+/[^/@]+)@(?P<ref>.+)$")
UNSAFE_NAME_CHARACTERS = re.compile(r"[^A-Za-z0-9._+-]")

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
    spdx_path: pathlib.Path


@dataclass
class SourceDownload:
    file_name: str
    sha512: str
    location: str


# What builds a port: the files of its vcpkg port, by their path in it and their SHA-256, and what the port downloads.
# vcpkg records both in the SPDX document that it installs with the port.
@dataclass
class PortSource:
    port: Port
    port_files: dict[str, str]
    downloads: list[SourceDownload]

    # The name says what the archive holds, so that every build that uses the same source names the same archive.
    def archive_name(self) -> str:
        lines = [f"port {path} {digest}" for path, digest in sorted(self.port_files.items())]
        lines += sorted(f"download {download.file_name} {download.sha512}" for download in self.downloads)
        digest = hashlib.sha256("\n".join(lines).encode()).hexdigest()
        version = self.port.version.partition("#")[0]
        return UNSAFE_NAME_CHARACTERS.sub("_", f"{self.port.name}-{version}-{digest[:12]}") + ".tar"


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

        spdx_path = installed_dir / triplet / "share" / name / "vcpkg.spdx.json"
        ports.append(Port(name, version, trim_source_files(read_text(copyright_file)), spdx_path))

    return sorted(ports, key=lambda port: port.name.lower())


def read_port_source(port: Port) -> PortSource:
    try:
        spdx = json.loads(port.spdx_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError(f"The vcpkg port {port.name} has no readable SPDX document: {error}") from error

    files = {entry["SPDXID"]: entry for entry in spdx.get("files", [])}
    port_files = {}
    for relationship in spdx.get("relationships", []):
        if relationship["spdxElementId"] != "SPDXRef-port" or relationship["relationshipType"] != "CONTAINS":
            continue
        entry = files[relationship["relatedSpdxElement"]]
        digests = {checksum["algorithm"]: checksum["checksumValue"] for checksum in entry.get("checksums", [])}
        path = pathlib.PurePosixPath(entry["fileName"])
        if path.is_absolute() or ".." in path.parts or "SHA256" not in digests:
            raise RuntimeError(f"The SPDX document of {port.name} lists the port file {entry['fileName']}")
        port_files[path.as_posix()] = digests["SHA256"].lower()
    if not port_files:
        raise RuntimeError(f"The SPDX document of {port.name} lists no files of its port")

    downloads = []
    for package in spdx.get("packages", []):
        if not package["SPDXID"].startswith("SPDXRef-resource-"):
            continue
        digests = {checksum["algorithm"]: checksum["checksumValue"] for checksum in package.get("checksums", [])}
        location = package.get("downloadLocation", "")
        if "SHA512" not in digests:
            raise RuntimeError(f"The SPDX document of {port.name} gives no SHA-512 of {location}")
        if github := GITHUB_SOURCE_PATTERN.match(location):
            # The file name that vcpkg_from_github() gives the archive that it downloads.
            file_name = f"{github['repository'].replace('/', '-')}-{github['ref'].replace('/', '_-')}.tar.gz"
        else:
            file_name = package.get("name") or urllib.parse.urlparse(location).path.rpartition("/")[2]
        if not file_name or UNSAFE_NAME_CHARACTERS.search(file_name) or file_name.startswith("."):
            raise RuntimeError(f"The SPDX document of {port.name} names the download {location} as {file_name!r}")
        sha512 = digests["SHA512"].lower()
        if same_name := next((download for download in downloads if download.file_name == file_name), None):
            if same_name.sha512 != sha512:
                raise RuntimeError(f"The SPDX document of {port.name} names two different downloads {file_name}")
            continue
        downloads.append(SourceDownload(file_name, sha512, location))

    return PortSource(port, port_files, downloads)


def configured_overlay_ports(source_dir: pathlib.Path) -> list[pathlib.Path]:
    configuration = json.loads(read_text(source_dir / "vcpkg-configuration.json"))
    return [source_dir / path for path in configuration.get("overlay-ports", [])]


def read_directory_files(directory: pathlib.Path) -> dict[str, bytes]:
    return {
        path.relative_to(directory).as_posix(): path.read_bytes()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def read_git_tree(repository: pathlib.Path, tree: str) -> dict[str, bytes]:
    archive = subprocess.run(
        ["git", "-C", str(repository), "archive", "--format=tar", tree], check=True, stdout=subprocess.PIPE
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        return {
            member.name: file.read()
            for member in tar.getmembers()
            if member.isfile() and (file := tar.extractfile(member))
        }


def versioned_port_trees(vcpkg_root: pathlib.Path, source: PortSource) -> list[str]:
    name = source.port.name
    versions_file = vcpkg_root / "versions" / f"{name[0]}-" / f"{name}.json"
    if not versions_file.is_file():
        return []
    version, _, port_version = source.port.version.partition("#")
    trees = []
    for entry in json.loads(read_text(versions_file))["versions"]:
        names = [entry.get(key) for key in ("version", "version-semver", "version-date", "version-string")]
        if version in names and str(entry.get("port-version", 0)) == (port_version or "0"):
            trees.append(entry["git-tree"])
    return trees


# vcpkg takes an overlay port before the port of its registry, and the port of a version that vcpkg.json pins from
# the history of the registry. Of these, the one whose files are those that the SPDX document lists is the one that
# built the port.
def read_port_files(source_dir: pathlib.Path, vcpkg_root: pathlib.Path, source: PortSource) -> dict[str, bytes]:
    name = source.port.name
    candidates = [
        functools.partial(read_directory_files, overlay / name)
        for overlay in configured_overlay_ports(source_dir)
        if (overlay / name / "vcpkg.json").is_file()
    ]
    candidates += [
        functools.partial(read_git_tree, vcpkg_root, tree) for tree in versioned_port_trees(vcpkg_root, source)
    ]
    if (vcpkg_root / "ports" / name / "vcpkg.json").is_file():
        candidates.append(functools.partial(read_directory_files, vcpkg_root / "ports" / name))

    for candidate in candidates:
        files = candidate()
        digests = {path: hashlib.sha256(data).hexdigest() for path, data in files.items()}
        if digests == source.port_files:
            return files
    raise RuntimeError(f"Found no port of {name} {source.port.version} with the files that its SPDX document lists")


def download_source(download: SourceDownload, downloads_dir: Optional[pathlib.Path], destination: pathlib.Path) -> None:
    def matches(path: pathlib.Path) -> bool:
        digest = hashlib.sha512()
        with open(path, "rb") as file:
            while chunk := file.read(1 << 20):
                digest.update(chunk)
        return digest.hexdigest() == download.sha512

    if downloads_dir and (downloads_dir / download.file_name).is_file() and matches(downloads_dir / download.file_name):
        shutil.copyfile(downloads_dir / download.file_name, destination)
        return

    urls = [f"{VCPKG_ASSET_CACHE_URL}/{download.sha512}"]
    if github := GITHUB_SOURCE_PATTERN.match(download.location):
        urls.append(f"https://github.com/{github['repository']}/archive/{github['ref']}.tar.gz")
    elif download.location.startswith("https://"):
        urls.append(download.location)

    failures = []
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=60) as response, open(destination, "wb") as file:
                shutil.copyfileobj(response, file)
        except (OSError, urllib.error.URLError) as error:
            failures.append(f"{url}: {error}")
            continue
        if matches(destination):
            return
        failures.append(f"{url}: another SHA-512")
    raise RuntimeError(f"Could not download {download.file_name}:\n  " + "\n  ".join(failures))


# The archive holds nothing that differs between the machines that make it, so that every build of the same source
# makes the very same archive.
def add_to_archive(archive: tarfile.TarFile, name: str, data: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    info.mtime = 0
    archive.addfile(info, io.BytesIO(data))


def write_port_archive(
    source: PortSource, port_files: dict[str, bytes], work_dir: pathlib.Path, destination: pathlib.Path
) -> None:
    root = source.archive_name().removesuffix(".tar")
    readme = "\n".join(
        [
            f"The source code of {source.port.name} {source.port.version}, as Ladybird builds it with vcpkg.",
            "",
            "port/ holds the vcpkg port of the library: its build scripts and our patches. The other files are the",
            "source archives that the port downloads, unchanged:",
            "",
            *[f"  {download.file_name}: {download.location}" for download in source.downloads],
            "",
        ]
    )
    with tarfile.open(destination, "w", format=tarfile.USTAR_FORMAT) as archive:
        add_to_archive(archive, f"{root}/README", readme.encode())
        for path, data in sorted(port_files.items()):
            add_to_archive(archive, f"{root}/port/{path}", data)
        for download in sorted(source.downloads, key=lambda download: download.file_name):
            add_to_archive(archive, f"{root}/{download.file_name}", (work_dir / download.file_name).read_bytes())


def package_third_party_sources(args: argparse.Namespace) -> None:
    if not args.vcpkg_installed_dir or not args.vcpkg_triplet or not args.vcpkg_root:
        raise RuntimeError("Packaging the sources of the LGPL ports needs the vcpkg root, installed tree and triplet")

    ports = collect_vcpkg_ports(args.vcpkg_installed_dir, args.vcpkg_triplet)
    sources = [read_port_source(port) for port in ports if LGPL_MARKER in port.copyright]
    downloads_dir = args.vcpkg_root / "downloads"

    with tempfile.TemporaryDirectory() as work:
        work_dir = pathlib.Path(work)
        archives = []
        for source in sources:
            port_files = read_port_files(args.source_dir, args.vcpkg_root, source)
            source_dir = work_dir / source.port.name
            source_dir.mkdir()
            for download in source.downloads:
                download_source(
                    download, downloads_dir if downloads_dir.is_dir() else None, source_dir / download.file_name
                )
            write_port_archive(source, port_files, source_dir, work_dir / source.archive_name())
            archives.append(work_dir / source.archive_name())
            print(f"Packaged the source of {source.port.name} {source.port.version} as {source.archive_name()}")

        args.third_party_sources.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.third_party_sources, "w", format=tarfile.USTAR_FORMAT) as bundle:
            for archive in archives:
                bundle.add(archive, arcname=archive.name)


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
    if lgpl_ports and args.release:
        introduction += [
            "",
            "This release of Ladybird uses the following libraries under the GNU Lesser General Public License. The",
            "source code of each, with our patches and build scripts, is at the address below it:",
            "",
        ]
        for port in lgpl_ports:
            introduction += [
                f"  {port.name} {port.version}",
                f"    {THIRD_PARTY_SOURCES_URL}/{read_port_source(port).archive_name()}",
            ]
        introduction += [
            "",
            "The source code of Ladybird at https://github.com/LadybirdBrowser/ladybird contains everything needed to",
            "build it against modified versions of these libraries.",
        ]
    elif lgpl_ports:
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
    parser.add_argument("--output", "-o", type=pathlib.Path)
    parser.add_argument("--source-dir", type=pathlib.Path, default=LADYBIRD_SOURCE_DIR)
    parser.add_argument("--vcpkg-installed-dir", type=pathlib.Path)
    parser.add_argument("--vcpkg-triplet")
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--rust-target")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument(
        "--release", action="store_true", help="name where release-tools publishes the source of each LGPL port"
    )
    parser.add_argument(
        "--third-party-sources",
        type=pathlib.Path,
        help="instead of the licenses, write an archive with the source archive of each LGPL port to this path",
    )
    parser.add_argument("--vcpkg-root", type=pathlib.Path, default=os.environ.get("VCPKG_ROOT"))
    args = parser.parse_args()

    if args.third_party_sources:
        try:
            package_third_party_sources(args)
        except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        return 0

    if not args.output:
        parser.error("the following arguments are required: --output/-o")

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
