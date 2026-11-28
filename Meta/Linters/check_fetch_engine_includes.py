#!/usr/bin/env python3

import collections
import pathlib
import re
import sys

from typing import Optional

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
THIS_SCRIPT = pathlib.Path(__file__).resolve().relative_to(REPO_ROOT)
LIBRARIES_ROOT = pathlib.Path("Libraries")
LIBWEB_ROOT = LIBRARIES_ROOT / "LibWeb"
ENGINE_ROOT = LIBWEB_ROOT / "Fetch/Engine"
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# The source tree directories whose headers the check follows.
FOLLOWED_ROOTS = {"AK", "Libraries"}
FORBIDDEN_PREFIXES = ("LibJS/", "LibGC/")

# Defines the macros that declare a type holds no cell edges, and the trait they specialize.
ALLOWED_GC_HEADER = "LibGC/HoldsNoCellEdges.h"

# The generated headers the engine reaches, which hold debug macros, export macros, the browser version, a bindings
# enum, and IPC codec and Rust FFI declarations.
GENERATED_HEADERS = {
    "AK/Debug.h",
    "LibCore/BrowserVersion.h",
    "LibCore/Export.h",
    "LibHTTP/Cookie/CookieCodecs.h",
    "LibTextCodec/Export.h",
    "LibURL/RustFFI.h",
    "LibWeb/Export.h",
    "LibWebCommon/Bindings/SecurityPolicyViolationEvent.h",
    "LibWebCommon/Export.h",
    "LibWebCommon/MixedContent/ProhibitsMixedSecurityContextsCodecs.h",
    "LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicyCodecs.h",
}

CHECKED_SUFFIXES = {".cpp", ".h"}

# The LibWeb files outside Fetch/Engine that hold the engine's code, which the check follows from as well.
ENGINE_FILES_OUTSIDE_ENGINE_ROOT = {
    "ContentSecurityPolicy/Directives/Directive.cpp",
    "ContentSecurityPolicy/Directives/Directive.h",
    "ContentSecurityPolicy/Directives/DirectiveOperations.cpp",
    "ContentSecurityPolicy/Directives/DirectiveOperations.h",
    "ContentSecurityPolicy/Directives/FetchIntegration.cpp",
    "ContentSecurityPolicy/Directives/FetchIntegration.h",
    "ContentSecurityPolicy/Directives/KeywordSources.cpp",
    "ContentSecurityPolicy/Directives/KeywordSources.h",
    "ContentSecurityPolicy/Directives/Names.cpp",
    "ContentSecurityPolicy/Directives/Names.h",
    "ContentSecurityPolicy/Directives/SourceExpression.cpp",
    "ContentSecurityPolicy/Directives/SourceExpression.h",
    "ContentSecurityPolicy/FetchIntegration.cpp",
    "ContentSecurityPolicy/FetchIntegration.h",
    "ContentSecurityPolicy/Policy.cpp",
    "ContentSecurityPolicy/Policy.h",
    "ContentSecurityPolicy/PolicyList.cpp",
    "ContentSecurityPolicy/PolicyList.h",
    "ContentSecurityPolicy/RequestViolation.cpp",
    "ContentSecurityPolicy/RequestViolation.h",
    "Fetch/Fetching/Checks.cpp",
    "Fetch/Fetching/Checks.h",
    "Fetch/Fetching/EngineFetching.h",
    "Fetch/Fetching/FetchEngine.cpp",
    "Fetch/Fetching/FetchEngine.h",
    "Fetch/Fetching/Fetching.cpp",
    "Fetch/Fetching/NetworkBodyPump.cpp",
    "Fetch/Fetching/NetworkBodyPump.h",
    "Fetch/Fetching/NetworkObserverHooks.cpp",
    "Fetch/Fetching/NetworkObserverHooks.h",
    "Fetch/Fetching/PendingResponse.cpp",
    "Fetch/Fetching/PendingResponse.h",
    "Fetch/Fetching/RefCountedFlag.cpp",
    "Fetch/Fetching/RefCountedFlag.h",
    "Fetch/Infrastructure/ClientContextSnapshot.h",
    "Fetch/Infrastructure/FetchControlBlock.h",
    "Fetch/Infrastructure/FetchControllerToken.h",
    "Fetch/Infrastructure/FetchParams.cpp",
    "Fetch/Infrastructure/FetchParams.h",
    "Fetch/Infrastructure/FetchTimingInfo.cpp",
    "Fetch/Infrastructure/FetchTimingInfo.h",
    "Fetch/Infrastructure/HTTP.cpp",
    "Fetch/Infrastructure/HTTP.h",
    "Fetch/Infrastructure/HTTP/CORS.cpp",
    "Fetch/Infrastructure/HTTP/CORS.h",
    "Fetch/Infrastructure/HTTP/FetchBody.cpp",
    "Fetch/Infrastructure/HTTP/FetchBody.h",
    "Fetch/Infrastructure/HTTP/MIME.cpp",
    "Fetch/Infrastructure/HTTP/MIME.h",
    "Fetch/Infrastructure/HTTP/Requests.cpp",
    "Fetch/Infrastructure/HTTP/Requests.h",
    "Fetch/Infrastructure/HTTP/Responses.cpp",
    "Fetch/Infrastructure/HTTP/Responses.h",
    "Fetch/Infrastructure/KeepaliveQuotaAccountant.cpp",
    "Fetch/Infrastructure/KeepaliveQuotaAccountant.h",
    "Fetch/Infrastructure/MimeTypeBlocking.cpp",
    "Fetch/Infrastructure/MimeTypeBlocking.h",
    "Fetch/Infrastructure/NetworkObservation.h",
    "Fetch/Infrastructure/NetworkPartitionKey.cpp",
    "Fetch/Infrastructure/NetworkPartitionKey.h",
    "Fetch/Infrastructure/NoSniffBlocking.cpp",
    "Fetch/Infrastructure/NoSniffBlocking.h",
    "Fetch/Infrastructure/PortBlocking.cpp",
    "Fetch/Infrastructure/PortBlocking.h",
    "Fetch/Infrastructure/PreflightCache.cpp",
    "Fetch/Infrastructure/PreflightCache.h",
    "Fetch/Infrastructure/RequestOrResponseBlocking.h",
    "HTML/PolicyContainers.cpp",
    "HTML/PolicyContainers.h",
    "HTML/Scripting/EnvironmentSettingsObjectAddress.h",
    "HTML/Scripting/RemoteClientNavigation.h",
    "HTML/Scripting/RemoteEnvironmentSettings.h",
    "HighResolutionTime/Clocks.cpp",
    "HighResolutionTime/Clocks.h",
    "Loader/LoaderConfig.cpp",
    "Loader/LoaderConfig.h",
    "MixedContent/AbstractOperations.cpp",
    "MixedContent/AbstractOperations.h",
    "ReferrerPolicy/AbstractOperations.cpp",
    "ReferrerPolicy/AbstractOperations.h",
    "SRI/IntegrityPolicy.cpp",
    "SRI/IntegrityPolicy.h",
    "SRI/SRI.cpp",
    "SRI/SRI.h",
    "UpgradeInsecureRequests/AbstractOperations.cpp",
    "UpgradeInsecureRequests/AbstractOperations.h",
}


def includes_of(path):
    for line_number, line in enumerate((REPO_ROOT / path).read_text(encoding="utf-8").splitlines(), start=1):
        match = INCLUDE.match(line)
        if match:
            yield line_number, line.strip(), match.group(1) == '"', match.group(2)


def resolve(path, is_quoted, included_path):
    candidates = [(REPO_ROOT / path).parent / included_path] if is_quoted else []
    candidates.append(REPO_ROOT / LIBRARIES_ROOT / included_path)
    candidates.append(REPO_ROOT / included_path)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve().relative_to(REPO_ROOT)
    return None


def names_a_source_tree_library(included_path):
    first_component = pathlib.PurePosixPath(included_path).parts[0]
    return (REPO_ROOT / LIBRARIES_ROOT / first_component).is_dir() or (REPO_ROOT / first_component).is_dir()


def main() -> int:
    engine_files = [
        path.relative_to(REPO_ROOT)
        for path in sorted((REPO_ROOT / ENGINE_ROOT).rglob("*"))
        if path.suffix in CHECKED_SUFFIXES
    ]
    missing_engine_files = []
    for engine_file in sorted(ENGINE_FILES_OUTSIDE_ENGINE_ROOT):
        path = LIBWEB_ROOT / engine_file
        if (REPO_ROOT / path).is_file():
            engine_files.append(path)
        else:
            missing_engine_files.append(engine_file)
    engine_file_set = set(engine_files)

    reached_by: dict[pathlib.Path, Optional[tuple[pathlib.Path, int, str]]] = dict.fromkeys(engine_files)
    queue = collections.deque(engine_files)
    forbidden = []
    outside_engine = []
    unresolved = []
    reached_generated_headers = set()

    while queue:
        path = queue.popleft()
        for line_number, line, is_quoted, included_path in includes_of(path):
            include = (path, line_number, line)
            resolved = resolve(path, is_quoted, included_path)
            if resolved is not None and resolved.is_relative_to(LIBRARIES_ROOT):
                included_path = resolved.relative_to(LIBRARIES_ROOT).as_posix()

            if included_path.startswith(FORBIDDEN_PREFIXES) and included_path != ALLOWED_GC_HEADER:
                forbidden.append((included_path, include))
            elif included_path in GENERATED_HEADERS:
                reached_generated_headers.add(included_path)
            elif resolved is None:
                if names_a_source_tree_library(included_path):
                    unresolved.append((included_path, include))
            elif resolved.is_relative_to(LIBWEB_ROOT) and resolved not in engine_file_set:
                outside_engine.append((included_path, include))
            elif resolved.parts[0] in FOLLOWED_ROOTS and resolved not in reached_by:
                reached_by[resolved] = include
                queue.append(resolved)

    def print_chains(offenders):
        for included_path, include in offenders:
            chain = [include]
            while reached_by[chain[-1][0]] is not None:
                chain.append(reached_by[chain[-1][0]])
            print(f"  {included_path}, through:", file=sys.stderr)
            for path, line_number, line in reversed(chain):
                print(f"    {path}:{line_number}: {line}", file=sys.stderr)

    if forbidden:
        print(
            f"The fetch engine must not include LibJS or LibGC, directly or through other headers, except "
            f"{ALLOWED_GC_HEADER}:",
            file=sys.stderr,
        )
        print_chains(forbidden)
    if outside_engine:
        print(
            f"The fetch engine must not include a LibWeb header outside {ENGINE_ROOT} and "
            f"ENGINE_FILES_OUTSIDE_ENGINE_ROOT in {THIS_SCRIPT}:",
            file=sys.stderr,
        )
        print_chains(outside_engine)
    if unresolved:
        print(
            f"The fetch engine must not include a library header that is not in the source tree, such as a generated "
            f"one outside GENERATED_HEADERS in {THIS_SCRIPT}:",
            file=sys.stderr,
        )
        print_chains(unresolved)
    for engine_file in missing_engine_files:
        print(
            f"{LIBWEB_ROOT / engine_file} does not exist; remove it from ENGINE_FILES_OUTSIDE_ENGINE_ROOT in "
            f"{THIS_SCRIPT}.",
            file=sys.stderr,
        )

    unreached_generated_headers = sorted(GENERATED_HEADERS - reached_generated_headers)
    for header in unreached_generated_headers:
        print(
            f"{header} is no longer reached from the fetch engine; remove it from GENERATED_HEADERS in {THIS_SCRIPT}.",
            file=sys.stderr,
        )

    return 1 if forbidden or outside_engine or unresolved or missing_engine_files or unreached_generated_headers else 0


if __name__ == "__main__":
    sys.exit(main())
