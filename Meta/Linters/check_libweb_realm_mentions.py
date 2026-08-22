#!/usr/bin/env python3

import collections
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
LIBWEB_ROOT = pathlib.Path("Libraries/LibWeb")

REALM_PATTERN = re.compile(r"JS::Realm|LibJS/Runtime/Realm\.h")
PLATFORM_OBJECT_IMPLEMENTATION_PATTERN = re.compile(r"public\s+Bindings::PlatformObject")

# This check is intentionally coarse: component-level counts cannot see an
# intra-component swap, and it counts JS::Realm spellings rather than realm
# reasoning moved into function bodies. The latter is covered by the separate
# conservative lexical audit metric in Documentation/Wrappers/28-...md.

EXCLUDED_PATH_PARTS = {
    "Bindings",
    "WebIDL",
}

EXCLUDED_PREFIXES = (pathlib.Path("Libraries/LibWeb/HTML/Scripting"),)

CHECKED_SUFFIXES = {
    ".cpp",
    ".h",
    ".mm",
}

# Each entry is:
#   component: (allowed file count, allowed mention count, reason)
#
# This is intentionally exact. If a component goes down, tighten the baseline in
# the same change. If it goes up, justify the architecture change here.
ALLOWED_REALM_MENTIONS = {
    "Animations": (5, 14, "animation callbacks/events and CSS value materialization still pass spec-selected realms"),
    "CSS": (7, 21, "CSS Typed OM, parser-facing reification, and style-value JS materialization"),
    "Clipboard": (4, 16, "clipboard item/data callbacks still materialize JS values in caller-selected realms"),
    "Compression": (4, 8, "compression streams still use realms for stream/chunk conversion plumbing"),
    "ContentSecurityPolicy": (4, 14, "CSP violation/report objects and callbacks still materialize JS-facing values"),
    "CookieStore": (2, 19, "cookie-store async callbacks and promise/value materialization still thread realms"),
    "Crypto": (10, 428, "WebCrypto algorithms still use realms for buffer/key/promise materialization"),
    "DOM": (10, 33, "DOM abort plumbing and node helpers still have callback/materialization realm use"),
    "DOMURL": (2, 8, "URLSearchParams iterator objects still materialize JS iterator results in selected realms"),
    "Fetch": (
        22,
        62,
        "Fetch bodies, headers, requests, responses, and controllers still materialize JS values/streams",
    ),
    "FileAPI": (6, 11, "File/Blob/FileReader algorithms still create streams, buffers, and events in selected realms"),
    "Geometry": (8, 8, "geometry constructors and structured clone still materialize JS-facing geometry objects"),
    "HTML": (
        64,
        201,
        "HTML algorithms still contain structured serialization, canvas, navigation, worker, and event realm use",
    ),
    "IndexedDB": (
        14,
        100,
        "IndexedDB requests/cursors/records still materialize structured-clone values in selected realms",
    ),
    "Infra": (2, 13, "JSON parse/serialization helpers still expose JS values in selected realms"),
    "Internals": (3, 3, "test-only internals still expose JS-facing helper objects and promises"),
    "SVG": (2, 8, "SVG length objects still store/use realms for detached/reflected JS/CSS materialization"),
    "ServiceWorker": (
        6,
        41,
        "service worker jobs, caches, and container promises still materialize results in selected realms",
    ),
    "Streams": (
        34,
        214,
        "Streams algorithms still use realms for controller/read/write operations and chunk conversion",
    ),
    "TrustedTypes": (2, 9, "Trusted Types policy factory operations still use selected realms"),
    "WebAssembly": (
        12,
        102,
        "WebAssembly constructors/exports instantiate JS objects/functions in spec-selected realms",
    ),
    "WebAudio": (2, 7, "WebAudio buffers still materialize JS buffers and callback/promise values in selected realms"),
    "WebDriver": (2, 7, "WebDriver execute/JSON conversion still materializes JS values for automation"),
    "WebGL": (72, 120, "WebGL APIs still materialize buffers, typed arrays, extensions, and wrapper objects"),
    "WebLocks": (6, 7, "Web Locks queue/callback algorithms still use callback and promise realms"),
    "XHR": (4, 11, "XHR/FormData/FileReader-style paths still materialize JS values and events"),
}


def is_excluded(path):
    if path.name.endswith("BindingsGlue.h"):
        return True
    if any(path.is_relative_to(prefix) for prefix in EXCLUDED_PREFIXES):
        return True
    relative_parts = path.relative_to(LIBWEB_ROOT).parts
    return any(part in EXCLUDED_PATH_PARTS for part in relative_parts)


def tracked_libweb_paths():
    output = subprocess.check_output(
        ["git", "ls-files", str(LIBWEB_ROOT)],
        cwd=REPO_ROOT,
        text=True,
    )
    for line in output.splitlines():
        path = pathlib.Path(line)
        if path.suffix in CHECKED_SUFFIXES:
            yield path


def main():
    actual = collections.defaultdict(lambda: [0, 0])
    platform_object_implementations = []

    for path in tracked_libweb_paths():
        text = (REPO_ROOT / path).read_text(encoding="utf-8", errors="ignore")
        if path != pathlib.Path(
            "Libraries/LibWeb/HTML/WindowProxy.h"
        ) and PLATFORM_OBJECT_IMPLEMENTATION_PATTERN.search(text):
            platform_object_implementations.append(path)

        if is_excluded(path):
            continue
        mention_count = len(REALM_PATTERN.findall(text))
        if mention_count == 0:
            continue

        component = path.relative_to(LIBWEB_ROOT).parts[0]
        actual[component][0] += 1
        actual[component][1] += mention_count

    failures = []

    if platform_object_implementations:
        print("LibWeb IDL implementations must derive from Bindings::Wrappable, not Bindings::PlatformObject.")
        print("WindowProxy is the sole exception because it is itself a spec-defined exotic JavaScript object.")
        for path in platform_object_implementations:
            print(f"  {path}")
        print()

    for component in sorted(set(actual) | set(ALLOWED_REALM_MENTIONS)):
        actual_counts = tuple(actual.get(component, (0, 0)))
        expected_files, expected_mentions, reason = ALLOWED_REALM_MENTIONS.get(component, (0, 0, "not allowlisted"))
        expected_counts = (expected_files, expected_mentions)
        if actual_counts == expected_counts:
            continue
        failures.append((component, actual_counts, expected_counts, reason))

    if failures:
        print("LibWeb implementation-side realm mention baseline changed.")
        print("Excluded: Bindings/, HTML/Scripting/, WebIDL/, and *BindingsGlue.h.")
        print("Checked pattern: JS::Realm or LibJS/Runtime/Realm.h")
        print()
        for component, actual_counts, expected_counts, reason in failures:
            actual_files, actual_mentions = actual_counts
            expected_files, expected_mentions = expected_counts
            print(
                f"{component}: actual {actual_files} files/{actual_mentions} mentions; "
                f"expected {expected_files} files/{expected_mentions} mentions"
            )
            print(f"  allowlist reason: {reason}")
        print()
        print("If counts decreased, tighten ALLOWED_REALM_MENTIONS in this script.")
        print("If counts increased, justify the new realm use in the allowlist reason.")
    if failures or platform_object_implementations:
        return 1

    print("LibWeb implementation-side realm mention baseline is unchanged.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
