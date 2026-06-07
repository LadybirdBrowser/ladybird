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
#include <LibURL/Forward.h>

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

    bool is_matching_public_suffix(StringView host);
    bool is_matching_public_suffix(Host const& host);
    Optional<String> find_matching_public_suffix(StringView string);
    Optional<String> find_matching_public_suffix(Host const& host);
    Optional<String> find_matching_registrable_domain(StringView string);
    Optional<String> find_matching_registrable_domain(Host const& host);

};

}

"""

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(content)


def canonicalize_public_suffix_rule(rule: str) -> str:
    def canonicalize_label(label: str) -> str:
        if label == "*":
            return label

        prefix = ""
        if label.startswith("!"):
            prefix = "!"
            label = label[1:]

        return prefix + label.encode("idna").decode("ascii").lower()

    return ".".join(canonicalize_label(label) for label in rule.split("."))


def generate_implementation_file(input_path: Path, output_path: Path) -> None:
    content = """#include <AK/String.h>
#include <AK/BinarySearch.h>
#include <AK/Vector.h>
#include <LibURL/Host.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>

namespace URL {

static constexpr auto s_public_suffixes = Array {"""

    reversed_lines = []
    with open(input_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()

            if line.startswith("//") or not line:
                continue

            line = canonicalize_public_suffix_rule(line)
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

static bool is_reversed_public_suffix(StringView reversed_host)
{
    return binary_search(s_public_suffixes, reversed_host);
}

static Optional<StringView> serialized_domain(Host const& host)
{
    if (!host.is_domain())
        return OptionalNone {};

    return host.get<String>().bytes_as_string_view();
}

struct NormalizedDomain {
    StringView host;
    StringView trailing_dot;
};

static Optional<NormalizedDomain> normalize_domain_for_matching(StringView host)
{
    host = host.trim("."sv, TrimMode::Left);
    if (host.is_empty())
        return OptionalNone {};

    auto trailing_dot = ""sv;
    if (host.ends_with('.')) {
        trailing_dot = "."sv;
        host = host.substring_view(0, host.length() - 1);
        if (host.is_empty())
            return OptionalNone {};
    }

    return NormalizedDomain { host, trailing_dot };
}

static Optional<NormalizedDomain> normalized_domain_for_host(Host const& host)
{
    auto domain = serialized_domain(host);
    if (!domain.has_value())
        return OptionalNone {};

    return normalize_domain_for_matching(*domain);
}

static bool is_matching_public_suffix_impl(StringView host)
{
    // Empty labels are kept so that inputs such as "com." do not match the bare "com" entry.
    auto labels = host.split_view('.', SplitBehavior::KeepEmpty);
    labels.reverse();

    StringBuilder reversed_host;
    reversed_host.join('.', labels);

    return is_reversed_public_suffix(reversed_host.string_view());
}

bool PublicSuffixData::is_matching_public_suffix(StringView host)
{
    if (host.is_empty())
        return false;

    auto parsed_host = Parser::parse_host(host);
    if (!parsed_host.has_value())
        return false;

    return is_matching_public_suffix(*parsed_host);
}

bool PublicSuffixData::is_matching_public_suffix(Host const& host)
{
    auto normalized_domain = normalized_domain_for_host(host);
    if (!normalized_domain.has_value())
        return false;

    return is_matching_public_suffix_impl(normalized_domain->host);
}

static Optional<String> find_matching_public_suffix_impl(StringView host)
{
    auto input = host.split_view('.');
    input.reverse();

    StringBuilder overall_search_string;
    StringBuilder search_string;
    for (auto part : input) {
        search_string.clear();
        search_string.append(overall_search_string.string_view());
        search_string.append(part);

        if (is_reversed_public_suffix(search_string.string_view())) {
            overall_search_string.append(part);
            overall_search_string.append('.');
            continue;
        }

        search_string.clear();
        search_string.append(overall_search_string.string_view());
        search_string.append('.');

        if (is_reversed_public_suffix(search_string.string_view())) {
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

Optional<String> PublicSuffixData::find_matching_public_suffix(StringView string)
{
    if (string.is_empty())
        return {};

    auto parsed_host = Parser::parse_host(string);
    if (!parsed_host.has_value())
        return {};

    return find_matching_public_suffix(*parsed_host);
}

Optional<String> PublicSuffixData::find_matching_public_suffix(Host const& host)
{
    auto normalized_domain = normalized_domain_for_host(host);
    if (!normalized_domain.has_value())
        return {};

    auto public_suffix = find_matching_public_suffix_impl(normalized_domain->host);
    if (!public_suffix.has_value())
        return {};

    return MUST(String::formatted("{}{}", public_suffix.value(), normalized_domain->trailing_dot));
}

// https://github.com/publicsuffix/list/wiki/Format#algorithm
static Optional<String> find_matching_registrable_domain_impl(StringView host)
{
    // The registered or registrable domain is the public suffix plus one additional label.
    auto public_suffix = find_matching_public_suffix_impl(host);
    if (!public_suffix.has_value() || !host.ends_with(*public_suffix))
        return {};

    if (host == *public_suffix)
        return {};

    auto subhost = host.substring_view(0, host.length() - public_suffix->bytes_as_string_view().length());
    subhost = subhost.trim("."sv, TrimMode::Right);

    if (subhost.is_empty())
        return {};

    size_t start_index = 0;
    if (auto index = subhost.find_last('.'); index.has_value())
        start_index = *index + 1;

    return MUST(String::from_utf8(host.substring_view(start_index)));
}

Optional<String> PublicSuffixData::find_matching_registrable_domain(StringView string)
{
    if (string.is_empty())
        return {};

    auto parsed_host = Parser::parse_host(string);
    if (!parsed_host.has_value())
        return {};

    return find_matching_registrable_domain(*parsed_host);
}

Optional<String> PublicSuffixData::find_matching_registrable_domain(Host const& host)
{
    auto normalized_domain = normalized_domain_for_host(host);
    if (!normalized_domain.has_value())
        return {};

    auto registrable_domain = find_matching_registrable_domain_impl(normalized_domain->host);
    if (!registrable_domain.has_value())
        return {};

    return MUST(String::formatted("{}{}", registrable_domain.value(), normalized_domain->trailing_dot));
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
