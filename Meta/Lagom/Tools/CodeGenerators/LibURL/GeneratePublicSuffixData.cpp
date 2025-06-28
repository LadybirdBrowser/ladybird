/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(Core::InputBufferedFile&, Core::File&);
ErrorOr<void> generate_implementation_file(Core::InputBufferedFile&, Core::File&);

static ErrorOr<NonnullOwnPtr<Core::InputBufferedFile>> open_file(StringView path, Core::File::OpenMode mode)
{
    if (path.is_empty())
        return Error::from_string_literal("Provided path is empty, please provide all command line options");

    auto file = TRY(Core::File::open(path, mode));
    return Core::InputBufferedFile::create(move(file));
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView public_suffix_list_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(public_suffix_list_path, "Path to the public suffix list", "public-suffix-list-path", 'p', "public-suffix-list-path");
    args_parser.parse(arguments);

    auto identifier_data = TRY(open_file(public_suffix_list_path, Core::File::OpenMode::Read));

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(*identifier_data, *generated_header_file));
    TRY(generate_implementation_file(*identifier_data, *generated_implementation_file));

    return 0;
}

ErrorOr<void> generate_header_file(Core::InputBufferedFile&, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.append(R"~~~(
#pragma once

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

private:
    Trie<char, Empty> m_dictionary;
};

}

)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(Core::InputBufferedFile& input, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.append(R"~~~(
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/PublicSuffixData.h>

namespace URL {

static constexpr auto s_public_suffixes = Array {)~~~");

    Array<u8, 1024> buffer {};

    while (TRY(input.can_read_line())) {
        auto line = TRY(input.read_line(buffer));

        if (line.starts_with("//"sv) || line.is_empty())
            continue;

        auto view = line.split_view("."sv);
        view.reverse();

        auto val = MUST(String::join("."sv, view));

        generator.set("line", val);
        generator.append(R"~~~(
    "@line@"sv,)~~~");
    }

    generator.append(R"~~~(
};

PublicSuffixData::PublicSuffixData()
    : m_dictionary('/')
{
    // FIXME: Reduce the depth of this trie
    for (auto str : s_public_suffixes) {
        MUST(m_dictionary.insert(str.begin(), str.end(), Empty {}, [](auto const&, auto const&) -> Optional<Empty> { return {}; }));
    }
}

bool PublicSuffixData::is_public_suffix(StringView host)
{
    auto it = host.begin();
    auto& node = m_dictionary.traverse_until_last_accessible_node(it, host.end());
    return it.is_end() && node.has_metadata();
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

)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
