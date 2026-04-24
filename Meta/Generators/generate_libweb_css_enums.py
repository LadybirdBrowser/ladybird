#!/usr/bin/env python3

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

from Utils.utils import snake_casify
from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum


def write_header_file(out: TextIO, enums_data: dict) -> None:
    out.write("""
#pragma once

#include <AK/Optional.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {
""")

    for name, value in enums_data.items():
        out.write(f"enum class {title_casify(name)} : {underlying_type_for_enum(len(value))} {{\n")
        for member_name in value:
            # Don't include aliases in the enum.
            if "=" in member_name:
                continue
            out.write(f"    {title_casify(member_name)},\n")

        out.write(f"""}};
Optional<{title_casify(name)}> keyword_to_{snake_casify(name)}(Keyword);
Keyword to_keyword({title_casify(name)});
StringView to_string({title_casify(name)});

""")

    out.write("}\n")


def write_implementation_file(out: TextIO, enums_data: dict) -> None:
    out.write("""
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Keyword.h>

namespace Web::CSS {
""")

    for name, value in enums_data.items():
        name_titlecase = title_casify(name)
        name_snakecase = snake_casify(name)

        out.write(f"""
Optional<{name_titlecase}> keyword_to_{name_snakecase}(Keyword keyword)
{{
    switch (keyword) {{
""")
        for member_name in value:
            if "=" in member_name:
                parts = member_name.split("=")
                valueid_titlecase = title_casify(parts[0])
                member_titlecase = title_casify(parts[1])
            else:
                valueid_titlecase = title_casify(member_name)
                member_titlecase = title_casify(member_name)
            out.write(f"""    case Keyword::{valueid_titlecase}:
        return {name_titlecase}::{member_titlecase};
""")
        out.write("""    default:
        return {};
    }
}
""")

        out.write(f"""
Keyword to_keyword({name_titlecase} {name_snakecase}_value)
{{
    switch ({name_snakecase}_value) {{
""")
        for member_name in value:
            if "=" in member_name:
                continue
            member_titlecase = title_casify(member_name)
            out.write(f"""    case {name_titlecase}::{member_titlecase}:
        return Keyword::{member_titlecase};
""")
        out.write("""    default:
        VERIFY_NOT_REACHED();
    }
}
""")

        out.write(f"""
StringView to_string({name_titlecase} value)
{{
    switch (value) {{
""")
        for member_name in value:
            if "=" in member_name:
                continue
            member_titlecase = title_casify(member_name)
            out.write(f"""    case {name_titlecase}::{member_titlecase}:
        return "{member_name}"sv;
""")
        out.write("""    default:
        VERIFY_NOT_REACHED();
    }
}
""")

    out.write("}\n")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS Enums", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the Enums header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the Enums implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        enums_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, enums_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, enums_data)


if __name__ == "__main__":
    main()
