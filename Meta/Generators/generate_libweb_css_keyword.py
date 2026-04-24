#!/usr/bin/env python3

# Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
# Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum


def keyword_name(dashy_name: str) -> str:
    if dashy_name == "-infinity":
        return "NegativeInfinity"
    return title_casify(dashy_name)


def write_header_file(out: TextIO, keyword_data: list) -> None:
    underlying_type = underlying_type_for_enum(len(keyword_data))
    out.write(f"""
#pragma once

#include <AK/StringView.h>
#include <AK/Traits.h>
#include <LibWeb/Export.h>

namespace Web::CSS {{

enum class Keyword : {underlying_type} {{
    Invalid,
""")

    for name in keyword_data:
        out.write(f"""
    {keyword_name(name)},
""")

    out.write("""
};

WEB_API Optional<Keyword> keyword_from_string(StringView);
StringView string_from_keyword(Keyword);

// https://www.w3.org/TR/css-values-4/#common-keywords
// https://drafts.csswg.org/css-cascade-4/#valdef-all-revert
inline bool is_css_wide_keyword(StringView name)
{
    return name.equals_ignoring_ascii_case("inherit"sv)
        || name.equals_ignoring_ascii_case("initial"sv)
        || name.equals_ignoring_ascii_case("revert"sv)
        || name.equals_ignoring_ascii_case("revert-layer"sv)
        || name.equals_ignoring_ascii_case("unset"sv);
}

}

""")


def write_implementation_file(out: TextIO, keyword_data: list) -> None:
    out.write("""
#include <AK/Assertions.h>
#include <AK/HashMap.h>
#include <LibWeb/CSS/Keyword.h>

namespace Web::CSS {

HashMap<StringView, Keyword, AK::CaseInsensitiveASCIIStringViewTraits> g_stringview_to_keyword_map {
""")

    for name in keyword_data:
        out.write(f"""
    {{"{name}"sv, Keyword::{keyword_name(name)}}},
""")

    out.write("""
};

Optional<Keyword> keyword_from_string(StringView string)
{
    return g_stringview_to_keyword_map.get(string);
}

StringView string_from_keyword(Keyword keyword) {
    switch (keyword) {
""")

    for name in keyword_data:
        out.write(f"""
    case Keyword::{keyword_name(name)}:
        return "{name}"sv;
        """)

    out.write("""
    default:
        return "(invalid CSS::Keyword)"sv;
    }
}

} // namespace Web::CSS
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS Keyword", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the Keyword header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the Keyword implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        keyword_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, keyword_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, keyword_data)


if __name__ == "__main__":
    main()
