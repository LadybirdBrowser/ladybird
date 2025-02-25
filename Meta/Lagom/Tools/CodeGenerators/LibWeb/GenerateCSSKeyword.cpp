/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonArray& keyword_data, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonArray& keyword_data, Core::File& file);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Keyword header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Keyword implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(json_path));
    VERIFY(json.is_array());
    auto keyword_data = json.as_array();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(keyword_data, *generated_header_file));
    TRY(generate_implementation_file(keyword_data, *generated_implementation_file));

    return 0;
}

static String keyword_name(String const& dashy_name)
{
    if (dashy_name == "-infinity"sv)
        return "NegativeInfinity"_string;
    return title_casify(dashy_name);
}

ErrorOr<void> generate_header_file(JsonArray& keyword_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.append(R"~~~(
#pragma once

#include <AK/StringView.h>
#include <AK/Traits.h>

namespace Web::CSS {

enum class Keyword {
    Invalid,
)~~~");

    keyword_data.for_each([&](auto& name) {
        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", keyword_name(name.as_string()));

        member_generator.append(R"~~~(
    @name:titlecase@,
)~~~");
    });

    generator.append(R"~~~(
};

Optional<Keyword> keyword_from_string(StringView);
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

)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonArray& keyword_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/Assertions.h>
#include <AK/HashMap.h>
#include <LibWeb/CSS/Keyword.h>

namespace Web::CSS {

HashMap<StringView, Keyword, AK::CaseInsensitiveASCIIStringViewTraits> g_stringview_to_keyword_map {
)~~~");

    keyword_data.for_each([&](auto& name) {
        auto member_generator = generator.fork();
        member_generator.set("name", name.as_string());
        member_generator.set("name:titlecase", keyword_name(name.as_string()));
        member_generator.append(R"~~~(
    {"@name@"sv, Keyword::@name:titlecase@},
)~~~");
    });

    generator.append(R"~~~(
};

Optional<Keyword> keyword_from_string(StringView string)
{
    return g_stringview_to_keyword_map.get(string);
}

StringView string_from_keyword(Keyword keyword) {
    switch (keyword) {
)~~~");

    keyword_data.for_each([&](auto& name) {
        auto member_generator = generator.fork();
        member_generator.set("name", name.as_string());
        member_generator.set("name:titlecase", keyword_name(name.as_string()));
        member_generator.append(R"~~~(
    case Keyword::@name:titlecase@:
        return "@name@"sv;
        )~~~");
    });

    generator.append(R"~~~(
    default:
        return "(invalid CSS::Keyword)"sv;
    }
}

} // namespace Web::CSS
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
