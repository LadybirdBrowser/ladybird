#!/usr/bin/env python3

# Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
# Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse

from pathlib import Path


def generate_header_file(output_path: Path) -> None:
    content = """#pragma once

#include <AK/Forward.h>
#include <AK/Trie.h>
#include <AK/Variant.h>

namespace URL {

class PublicSuffixData {
protected:
    PublicSuffixData();

public:
    PublicSuffixData(PublicSuffixData const&) = delete;
    PublicSuffixData& operator=(PublicSuffixData const&) = delete;

    static PublicSuffixData* the()
    {
        static PublicSuffixData* s_the;
        if (!s_the)
            s_the = new PublicSuffixData;
        return s_the;
    }

    bool is_public_suffix(StringView host);
    Optional<String> get_public_suffix(StringView string);

};

}

"""

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(content)


def generate_implementation_file(input_path: Path, output_path: Path) -> None:
    content = """#include <AK/String.h>
#include <AK/BinarySearch.h>
#include <AK/Vector.h>
#include <LibURL/PublicSuffixData.h>

namespace URL {

static constexpr auto s_public_suffixes = Array {"""

    reversed_lines = []
    with open(input_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()

            if line.startswith("//") or not line:
                continue

            reversed_line = ".".join(line.split(".")[::-1])
            reversed_lines.append(reversed_line)

    reversed_lines.sort()

    for item in reversed_lines:
        content += f'\n    "{item}"sv,'

    content += """
};

PublicSuffixData::PublicSuffixData()   
{    
}

bool PublicSuffixData::is_public_suffix(StringView host)
{
    return binary_search(s_public_suffixes, host);
}

Optional<String> PublicSuffixData::get_public_suffix(StringView string)
{
    auto input = string.split_view('.');
    input.reverse();

    StringBuilder overall_search_string;
    StringBuilder search_string;
    for (auto part : input) {
        search_string.clear();
        search_string.append(overall_search_string.string_view());
        search_string.append(part);

        if (is_public_suffix(search_string.string_view())) {
            overall_search_string.append(part);
            overall_search_string.append('.');
            continue;
        }

        search_string.clear();
        search_string.append(overall_search_string.string_view());
        search_string.append('.');

        if (is_public_suffix(search_string.string_view())) {
            overall_search_string.append(part);
            overall_search_string.append('.');
            continue;
        }

        break;
    }

    auto view = overall_search_string.string_view().split_view('.');
    view.reverse();

    StringBuilder return_string_builder;
    return_string_builder.join('.', view);
    if (return_string_builder.is_empty())
        return Optional<String> {};
    return MUST(return_string_builder.to_string());
}

}

"""

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(content)


def main():
    parser = argparse.ArgumentParser(description="Generate public suffix data files", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--generated-header-path", required=True, help="Path to the header file to generate")
    parser.add_argument(
        "-c", "--generated-implementation-path", required=True, help="Path to the implementation file to generate"
    )
    parser.add_argument("-p", "--public-suffix-list-path", required=True, help="Path to the public suffix list")
    args = parser.parse_args()

    generate_header_file(Path(args.generated_header_path))
    generate_implementation_file(Path(args.public_suffix_list_path), Path(args.generated_implementation_path))


if __name__ == "__main__":
    main()
