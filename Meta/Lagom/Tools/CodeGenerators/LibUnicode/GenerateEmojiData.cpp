/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/ByteString.h>
#include <AK/QuickSort.h>
#include <AK/SourceGenerator.h>
#include <AK/StringUtils.h>
#include <AK/Types.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibFileSystem/FileSystem.h>

struct Emoji {
    size_t image_path { 0 };
    Vector<u32> code_points;
    size_t code_point_array_index { 0 };
};

struct EmojiData {
    UniqueStringStorage unique_strings;
    Vector<Emoji> emojis;
    Vector<ByteString> emoji_file_list;
};

static ErrorOr<void> parse_emoji_file_list(Core::InputBufferedFile& file, EmojiData& emoji_data)
{
    HashTable<ByteString> seen_emojis;
    Array<u8, 1024> buffer;

    while (TRY(file.can_read_line())) {
        auto line = TRY(file.read_line(buffer));
        if (line.is_empty())
            continue;

        if (seen_emojis.contains(line)) {
            warnln("\x1b[1;31mError!\x1b[0m Duplicate emoji \x1b[35m{}\x1b[0m listed in emoji-file-list.txt.", line);
            return Error::from_errno(EEXIST);
        }

        ByteString emoji_file { line.trim_whitespace() };
        emoji_data.emoji_file_list.append(emoji_file);
        seen_emojis.set(emoji_file);

        Emoji emoji;
        emoji.image_path = emoji_data.unique_strings.ensure(emoji_file);

        auto emoji_basename = LexicalPath::basename(emoji_file, LexicalPath::StripExtension::Yes);

        emoji_basename.view().for_each_split_view('_', SplitBehavior::Nothing, [&](StringView code_point) {
            static constexpr auto code_point_header = "U+"sv;

            VERIFY(code_point.starts_with(code_point_header));
            code_point = code_point.substring_view(code_point_header.length());

            auto code_point_value = AK::StringUtils::convert_to_uint_from_hex<u32>(code_point);
            VERIFY(code_point_value.has_value());

            emoji.code_points.append(*code_point_value);
        });

        emoji_data.emojis.append(move(emoji));
    }

    return {};
}

static ErrorOr<void> validate_emoji(StringView emoji_resource_path, EmojiData& emoji_data)
{
    TRY(Core::Directory::for_each_entry(emoji_resource_path, Core::DirIterator::SkipDots, [&](auto& entry, auto&) -> ErrorOr<IterationDecision> {
        auto lexical_path = LexicalPath(entry.name);
        if (lexical_path.extension() != "png")
            return IterationDecision::Continue;

        auto title = lexical_path.title();
        if (!title.starts_with("U+"sv))
            return IterationDecision::Continue;

        Vector<u32> code_points;
        TRY(title.for_each_split_view('_', SplitBehavior::Nothing, [&](auto segment) -> ErrorOr<void> {
            auto code_point = AK::StringUtils::convert_to_uint_from_hex<u32>(segment.substring_view(2));
            VERIFY(code_point.has_value());

            TRY(code_points.try_append(*code_point));
            return {};
        }));

        auto it = emoji_data.emojis.find_if([&](auto const& emoji) {
            return emoji.code_points == code_points;
        });

        if (it == emoji_data.emojis.end()) {
            warnln("\x1b[1;31mError!\x1b[0m Emoji data for \x1b[35m{}\x1b[0m not found. Please check emoji-test.txt and emoji-serenity.txt.", entry.name);
            return Error::from_errno(ENOENT);
        }

        if (!emoji_data.emoji_file_list.contains_slow(lexical_path.string())) {
            warnln("\x1b[1;31mError!\x1b[0m Emoji entry for \x1b[35m{}\x1b[0m not found. Please check emoji-file-list.txt.", lexical_path);
            return Error::from_errno(ENOENT);
        }

        return IterationDecision::Continue;
    }));

    return {};
}

static ErrorOr<void> generate_emoji_data_header(Core::InputBufferedFile& file, EmojiData const&)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

static ErrorOr<void> generate_emoji_data_implementation(Core::InputBufferedFile& file, EmojiData const& emoji_data)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.set("string_index_type"sv, emoji_data.unique_strings.type_that_fits());
    generator.set("emojis_size"sv, ByteString::number(emoji_data.emojis.size()));

    generator.append(R"~~~(
#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibUnicode/Emoji.h>
#include <LibUnicode/EmojiData.h>

namespace Unicode {
)~~~");

    emoji_data.unique_strings.generate(generator);

    size_t total_code_point_count { 0 };
    for (auto const& emoji : emoji_data.emojis)
        total_code_point_count += emoji.code_points.size();
    generator.set("total_code_point_count", ByteString::number(total_code_point_count));

    generator.append(R"~~~(
static constexpr Array<u32, @total_code_point_count@> s_emoji_code_points { {)~~~");

    bool first = true;
    for (auto const& emoji : emoji_data.emojis) {
        for (auto code_point : emoji.code_points) {
            generator.append(first ? " "sv : ", "sv);
            generator.append(ByteString::formatted("{:#x}", code_point));
            first = false;
        }
    }

    generator.append(" } };\n"sv);

    generator.append(R"~~~(
struct EmojiData {
    constexpr ReadonlySpan<u32> code_points() const
    {
        return ReadonlySpan<u32>(s_emoji_code_points.data() + code_point_start, code_point_count);
    }

    @string_index_type@ image_path { 0 };
    size_t code_point_start { 0 };
    size_t code_point_count { 0 };
};
)~~~");

    generator.append(R"~~~(

static constexpr Array<EmojiData, @emojis_size@> s_emojis { {)~~~");

    for (auto const& emoji : emoji_data.emojis) {
        generator.set("image_path"sv, ByteString::number(emoji.image_path));
        generator.set("code_point_start"sv, ByteString::number(emoji.code_point_array_index));
        generator.set("code_point_count"sv, ByteString::number(emoji.code_points.size()));

        generator.append(R"~~~(
    { @image_path@, @code_point_start@, @code_point_count@ },)~~~");
    }

    generator.append(R"~~~(
} };

struct EmojiCodePointComparator {
    constexpr int operator()(ReadonlySpan<u32> code_points, EmojiData const& emoji)
    {
        auto emoji_code_points = emoji.code_points();

        if (code_points.size() != emoji_code_points.size())
            return static_cast<int>(code_points.size()) - static_cast<int>(emoji_code_points.size());

        for (size_t i = 0; i < code_points.size(); ++i) {
            if (code_points[i] != emoji_code_points[i])
                return static_cast<int>(code_points[i]) - static_cast<int>(emoji_code_points[i]);
        }

        return 0;
    }
};

Optional<StringView> emoji_image_for_code_points(ReadonlySpan<u32> code_points)
{
    if (auto const* emoji = binary_search(s_emojis, code_points, nullptr, EmojiCodePointComparator {}))
        return decode_string(emoji->image_path);
    return {};
}

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView emoji_file_list_path;
    StringView emoji_resource_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Unicode Data header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Unicode Data implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(emoji_file_list_path, "Path to the emoji-file-list.txt file", "emoji-file-list-path", 'f', "emoji-file-list-path");
    args_parser.add_option(emoji_resource_path, "Path to the /res/emoji directory", "emoji-resource-path", 'r', "emoji-resource-path");
    args_parser.parse(arguments);

    VERIFY(!emoji_resource_path.is_empty() && FileSystem::exists(emoji_resource_path));
    VERIFY(!emoji_file_list_path.is_empty() && FileSystem::exists(emoji_file_list_path));

    EmojiData emoji_data {};

    auto emoji_file_list_file = TRY(open_file(emoji_file_list_path, Core::File::OpenMode::Read));
    TRY(parse_emoji_file_list(*emoji_file_list_file, emoji_data));

    TRY(validate_emoji(emoji_resource_path, emoji_data));

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    TRY(generate_emoji_data_header(*generated_header_file, emoji_data));

    quick_sort(emoji_data.emojis, [](auto const& lhs, auto const& rhs) {
        if (lhs.code_points.size() != rhs.code_points.size())
            return lhs.code_points.size() < rhs.code_points.size();

        for (size_t i = 0; i < lhs.code_points.size(); ++i) {
            if (lhs.code_points[i] < rhs.code_points[i])
                return true;
            if (lhs.code_points[i] > rhs.code_points[i])
                return false;
        }

        return false;
    });

    size_t code_point_array_index { 0 };
    for (auto& emoji : emoji_data.emojis) {
        emoji.code_point_array_index = code_point_array_index;
        code_point_array_index += emoji.code_points.size();
    }

    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));
    TRY(generate_emoji_data_implementation(*generated_implementation_file, emoji_data));

    return 0;
}
