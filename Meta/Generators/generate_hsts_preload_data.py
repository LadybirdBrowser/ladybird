#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import ipaddress
import json
import re

from pathlib import Path

NAME_RE = re.compile(r"[a-z0-9.-]+")


def parse_entries(input_path: Path) -> list[tuple[str, bool]]:
    text = input_path.read_text(encoding="utf-8")

    # Chromium's transport_security_state_static.json uses // line comments, which are not legal JSON.
    text = re.sub(r"(?m)^\s*//.*$", "", text)

    data = json.loads(text)
    raw_entries = data["entries"]

    result: list[tuple[str, bool]] = []
    for entry in raw_entries:
        if entry.get("mode") != "force-https":
            continue

        name = entry["name"]

        try:
            ipaddress.ip_address(name)
            continue
        except ValueError:
            pass

        name = name.lower()
        if not NAME_RE.fullmatch(name):
            raise ValueError(
                f"HSTS preload entry name {name!r} contains characters outside [a-z0-9.-]; "
                f"refusing to emit it as a C++ string literal without an explicit policy."
            )

        include_subdomains = bool(entry.get("include_subdomains", False))
        result.append((name, include_subdomains))

    result.sort(key=lambda pair: pair[0])

    seen: set[str] = set()
    for name, _ in result:
        if name in seen:
            raise ValueError(f"Duplicate HSTS preload entry: {name!r}")
        seen.add(name)

    return result


def generate_header_file(output_path: Path) -> None:
    content = """#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace HTTP {

struct HSTSPreloadEntry {
    StringView name;
    bool include_subdomains;
};

class HSTSPreloadData {
    AK_MAKE_NONCOPYABLE(HSTSPreloadData);
    AK_MAKE_NONMOVABLE(HSTSPreloadData);

public:
    static HSTSPreloadData const& the();

    Optional<HSTSPreloadEntry> find_exact(StringView lowercased_domain) const;
    bool is_known_preloaded_hsts_host(StringView domain) const;

private:
    HSTSPreloadData() = default;
};

}
"""
    output_path.write_text(content, encoding="utf-8")


def generate_implementation_file(entries: list[tuple[str, bool]], output_path: Path) -> None:
    lines: list[str] = []
    lines.append("#include <AK/Array.h>")
    lines.append("#include <AK/BinarySearch.h>")
    lines.append("#include <AK/StringView.h>")
    lines.append("#include <LibHTTP/HSTSPreloadData.h>")
    lines.append("")
    lines.append("namespace HTTP {")
    lines.append("")
    lines.append(f"static constexpr Array<HSTSPreloadEntry, {len(entries)}> s_hsts_preload_entries {{ {{")
    for name, include_subdomains in entries:
        flag = "true" if include_subdomains else "false"
        lines.append(f'    HSTSPreloadEntry {{ "{name}"sv, {flag} }},')
    lines.append("} };")
    lines.append("")
    lines.append("HSTSPreloadData const& HSTSPreloadData::the()")
    lines.append("{")
    lines.append("    static HSTSPreloadData s_the;")
    lines.append("    return s_the;")
    lines.append("}")
    lines.append("")
    lines.append("Optional<HSTSPreloadEntry> HSTSPreloadData::find_exact(StringView needle) const")
    lines.append("{")
    lines.append("    auto* hit = binary_search(")
    lines.append("        s_hsts_preload_entries,")
    lines.append("        needle,")
    lines.append("        nullptr,")
    lines.append("        [](StringView lhs, HSTSPreloadEntry const& rhs) {")
    lines.append("            return lhs.compare(rhs.name);")
    lines.append("        });")
    lines.append("    if (!hit)")
    lines.append("        return {};")
    lines.append("    return *hit;")
    lines.append("}")
    lines.append("")
    lines.append("// https://www.rfc-editor.org/rfc/rfc6797#section-8.2")
    lines.append("bool HSTSPreloadData::is_known_preloaded_hsts_host(StringView domain) const")
    lines.append("{")
    lines.append("    if (find_exact(domain).has_value())")
    lines.append("        return true;")
    lines.append("    auto remaining = domain;")
    lines.append("    while (true) {")
    lines.append("        auto dot = remaining.find('.');")
    lines.append("        if (!dot.has_value())")
    lines.append("            break;")
    lines.append("        remaining = remaining.substring_view(*dot + 1);")
    lines.append("        if (auto entry = find_exact(remaining); entry.has_value() && entry->include_subdomains)")
    lines.append("            return true;")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("}")
    lines.append("")

    output_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate HSTS preload data files", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--generated-header-path", required=True, help="Path to the header file to generate")
    parser.add_argument(
        "-c", "--generated-implementation-path", required=True, help="Path to the implementation file to generate"
    )
    parser.add_argument(
        "-p",
        "--hsts-preload-list-path",
        required=True,
        help="Path to Chromium's transport_security_state_static.json",
    )
    args = parser.parse_args()

    entries = parse_entries(Path(args.hsts_preload_list_path))
    generate_header_file(Path(args.generated_header_path))
    generate_implementation_file(entries, Path(args.generated_implementation_path))


if __name__ == "__main__":
    main()
