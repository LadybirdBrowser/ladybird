#!/usr/bin/env python3

# Copyright (c) 2025, The Ladybird contributors
#
# SPDX-License-Identifier: BSD-2-Clause

import glob
import json
import os
import subprocess
import sys
import urllib.request

from enum import Enum

VCPKG = "vcpkg.json"
VCPKG_OVERLAYS_PORTS = "Meta/CMake/vcpkg/overlay-ports/*"
VCPKG_REPO = "Build/vcpkg"
VCPKG_BASELINE_URL = "https://raw.githubusercontent.com/microsoft/vcpkg/{}/versions/baseline.json"
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
    "tiff",
    "vulkan",
    "vulkan-headers",
    "zlib",
]

# List of libraries that are in vcpkg but not needed for the Linux platform
vcpkg_not_linux = [
    "dirent",
    "mman",
    "pthread",
]

# List of libraries that are behind vcpkg manifest features and only
# installed for specific GUI frameworks (e.g. GTK), not the Qt flatpak.
vcpkg_feature_only = [
    "gtk",
    "libadwaita",
    "wayland",
    "wayland-protocols",
]


class DepMatch(Enum):
    Match = (0,)
    NoMatch = (1,)
    VersionMismatch = (2,)
    Excluded = (3,)


baseline_versions = {}


def get_baseline_version(baseline, name):
    if baseline not in baseline_versions:
        if os.path.isdir(VCPKG_REPO):
            # Clear GIT_DIR so git operates on the vcpkg repo, not the parent repo (pre-commit sets GIT_DIR)
            env = {k: v for k, v in os.environ.items() if k != "GIT_DIR"}
            try:
                result = subprocess.run(
                    ["git", "-C", VCPKG_REPO, "show", f"{baseline}:versions/baseline.json"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    check=True,
                    env=env,
                    text=True,
                )
                baseline_versions[baseline] = json.loads(result.stdout)
            except (subprocess.CalledProcessError, json.JSONDecodeError):
                pass

        if baseline not in baseline_versions:
            url = VCPKG_BASELINE_URL.format(baseline)

            try:
                with urllib.request.urlopen(url) as response:
                    baseline_versions[baseline] = json.load(response)
            except Exception as error:
                print(f"Failed to fetch vcpkg baseline {baseline[:7]}: {error}")
                baseline_versions[baseline] = {}

    port = baseline_versions[baseline].get("default", {}).get(name)
    if port and "baseline" in port:
        return port["baseline"]

    print(f"{name} cannot be matched, vcpkg baseline revision: {baseline[:7]}")


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
                    elif "branch" in source or "x-branch" in source:
                        # Get the branch
                        # Strip '_' postfix, replace '/' with '_': chromium/7258_13 vs chromium_7258
                        branch = str(source.get("branch", source.get("x-branch"))).split("_")[0].replace("/", "_")
                        mismatch_found |= match_and_update(name, branch)

                        break
            else:
                if not (name == SELF or name in flatpak_build_tools or name in flatpak_transitive_deps):
                    flatpak.append(name)

        # Remove excluded dependencies from the vcpkg list
        for name in flatpak_runtime_libs + vcpkg_not_linux + vcpkg_feature_only:
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
