#!/usr/bin/env python3

# Copyright (c) 2025, The Ladybird contributors
#
# SPDX-License-Identifier: BSD-2-Clause

import glob
import json
import os
import subprocess
import sys

from enum import Enum

VCPKG = "vcpkg.json"
VCPKG_OVERLAYS_PORTS = "Meta/CMake/vcpkg/overlay-ports/*"
VCPKG_URL = "https://github.com/microsoft/vcpkg.git"
VCPKG_REPO = "Build/vcpkg"
FLATPAK_MANIFEST = "Meta/CMake/flatpak/org.ladybird.Ladybird.json"
SELF = "Ladybird"

# List of build tools that are not provided by the Flatpak SDK and therefore in the manifest
# For a vcpkg build these are installed on the host system
flatpak_build_tools = [
    "gn",
]

# List of libraries that are transitive dependencies
# These are in the manifest to fetch before building, but not explicitely versioned
flatpak_transitive_deps = [
    "libyuv",
]

# List of libraries that are in the Flatpak runtime and therefore not in the manifest
# See: https://docs.flatpak.org/en/latest/available-runtimes.html#check-software-available-in-runtimes
flatpak_runtime_libs = [
    "curl",
    "dbus",
    "fontconfig",
    "harfbuzz",
    "libjpeg-turbo",
    "libproxy",
    "nghttp2",  # FIXME: This can be removed when the vcpkg overlay is no longer needed.
    "qtbase",
    "qtmultimedia",
    "sqlite3",
    "zlib",
]

# List of libraries that are in vcpkg but not needed for the Linux platform
vcpkg_not_linux = [
    "dirent",
    "mman",
]


class DepMatch(Enum):
    Match = (0,)
    NoMatch = (1,)
    VersionMismatch = (2,)
    Excluded = (3,)


def get_baseline_version(baseline, name):
    if not os.path.isdir(VCPKG_REPO):
        cmd = f"git clone --revision {baseline} --depth 1 {VCPKG_URL} {VCPKG_REPO}"
        subprocess.run(cmd, stdout=subprocess.PIPE, shell=True, check=True)

    # Query the current vcpkg baseline for its version of this package, this becomes the reference for linting
    cmd = f"cd {VCPKG_REPO} && git show {baseline}:versions/baseline.json 2> /dev/null | grep -E -A 3 -e '\"{name}\"'"
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, shell=True, check=True)

        if not result.returncode:
            json_string = result.stdout.decode("utf-8").replace("\n", "").removesuffix(",")
            result = json.loads(f"{{{json_string}}}")

            if "baseline" in result[name]:
                return result[name]["baseline"]
    except subprocess.CalledProcessError:
        print(f"{name} cannot be matched, {VCPKG_REPO} is not at baseline revision: {baseline[:7]}")


def check_for_match(vcpkg: dict, vcpkg_baseline, name: str, identifier: str) -> DepMatch:
    if name == SELF or name in flatpak_build_tools or name in flatpak_transitive_deps:
        return DepMatch.Excluded

    if name not in vcpkg:
        version = get_baseline_version(vcpkg_baseline, name)

        if version:
            vcpkg[name] = version

    if name in vcpkg:
        if vcpkg[name] not in identifier:
            return DepMatch.VersionMismatch
        else:
            return DepMatch.Match
    else:
        return DepMatch.NoMatch


def check_vcpkg_vs_flatpak_versioning():
    def match_and_update(name: str, identifier: str) -> bool:
        dep_match = check_for_match(vcpkg, vcpkg_baseline, name, identifier)

        if dep_match == DepMatch.Match or dep_match == DepMatch.Excluded:
            if name in vcpkg:
                del vcpkg[name]

            return False
        else:
            if dep_match == DepMatch.VersionMismatch:
                print(f"{name} version mismatch. vcpkg: {vcpkg[name]}, Flatpak: {source['tag']}")
                del vcpkg[name]
            elif dep_match == DepMatch.NoMatch:
                flatpak.append(name)

            return True

    vcpkg = {}
    flatpak = []
    mismatch_found = False

    with open(VCPKG) as input:
        vcpkg_json = json.load(input)
        vcpkg_baseline = vcpkg_json["builtin-baseline"]

        for package in vcpkg_json["overrides"]:
            # Add name/version pair, strip any '#' postfix from the version
            vcpkg[package["name"]] = str(package["version"]).split("#")[0]

    # Check the vcpkg overlay ports for packages not listed in overrides
    for path in glob.glob(VCPKG_OVERLAYS_PORTS):
        with open(f"{path}/vcpkg.json") as input:
            overlay = json.load(input)

            if "name" in overlay and overlay["name"] not in vcpkg and "version" in overlay:
                vcpkg[overlay["name"]] = overlay["version"]

    with open(FLATPAK_MANIFEST) as input:
        for module in json.load(input)["modules"]:
            name = module["name"]

            for source in module["sources"]:
                if "type" in source and source["type"] == "git":
                    if "tag" in source:
                        # Get the tag
                        # Replace '-' with '.': 76-1 vs 76.1
                        tag = str(source["tag"]).replace("-", ".")
                        mismatch_found |= match_and_update(name, tag)

                        break
                    elif "branch" in source:
                        # Get the branch
                        # Strip '_' postfix, replace '/' with '_': chromium/7258_13 vs chromium_7258
                        branch = str(source["branch"]).split("_")[0].replace("/", "_")
                        mismatch_found |= match_and_update(name, branch)

                        break
            else:
                if not (name == SELF or name in flatpak_build_tools or name in flatpak_transitive_deps):
                    flatpak.append(name)

        # Remove excluded dependencies from the vcpkg list
        for name in flatpak_runtime_libs + vcpkg_not_linux:
            if name in vcpkg:
                del vcpkg[name]

        if len(vcpkg):
            mismatch_found = True
            print(f"There are {len(vcpkg)} vcpkg dependencies that have no match in flatpak")

            for name in vcpkg.keys():
                print(f"- {name}")

        if len(flatpak):
            mismatch_found = True
            print(f"There are {len(flatpak)} flatpak dependencies that have no match in vcpkg")

            for name in flatpak:
                print(f"- {name}")

    return mismatch_found


def main():
    file_list = sys.argv[1:] if len(sys.argv) > 1 else [VCPKG]
    did_fail = False

    if VCPKG in file_list or FLATPAK_MANIFEST in file_list:
        did_fail = check_vcpkg_vs_flatpak_versioning()

    # TODO: Add linting of Flatpak and AppStream manifest
    # See #5594, bullet point "Missing lint job in CI to ensure Flatpak and AppStream manifests are up to snuff"

    if did_fail:
        sys.exit(1)


if __name__ == "__main__":
    main()
