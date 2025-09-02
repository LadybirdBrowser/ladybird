/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject& units_data, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& units_data, Core::File& file);
ErrorOr<void> generate_idl_file(JsonObject& units_data, Core::File& file);

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView generated_idl_path;
    StringView units_json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the CSSNumericFactoryMethods header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the CSSNumericFactoryMethods implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(generated_idl_path, "Path to the CSSNumericFactoryMethods IDL file to generate", "generated-idl-path", 'i', "generated-idl-path");
    args_parser.add_option(units_json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(units_json_path));
    VERIFY(json.is_object());
    auto units_data = json.as_object();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));
    auto generated_idl_file = TRY(Core::File::open(generated_idl_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(units_data, *generated_header_file));
    TRY(generate_implementation_file(units_data, *generated_implementation_file));
    TRY(generate_idl_file(units_data, *generated_idl_file));

    return 0;
}

ErrorOr<void> generate_header_file(JsonObject& units_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

// https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory
namespace Web::CSS {

GC::Ref<CSSUnitValue> number(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> percent(JS::VM&, WebIDL::Double value);
)~~~");

    units_data.for_each_member([&](auto& dimension_name, JsonValue const& dimension) {
        auto dimension_generator = generator.fork();
        dimension_generator.set("dimension:acceptable_cpp", make_name_acceptable_cpp(snake_casify(dimension_name, TrimLeadingUnderscores::Yes)));
        dimension_generator.appendln("\n// <@dimension:acceptable_cpp@>");

        dimension.as_object().for_each_member([&](auto& unit_name, JsonValue const&) {
            auto unit_generator = dimension_generator.fork();
            unit_generator.set("unit:acceptable_cpp", make_name_acceptable_cpp(unit_name.to_ascii_lowercase()));
            unit_generator.appendln("GC::Ref<CSSUnitValue> @unit:acceptable_cpp@(JS::VM&, WebIDL::Double value);");
        });
    });

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject& units_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/FlyString.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/GeneratedCSSNumericFactoryMethods.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory
inline GC::Ref<CSSUnitValue> numeric_factory(JS::VM& vm, WebIDL::Double value, FlyString unit)
{
    // All of the above methods must, when called with a double value, return a new CSSUnitValue whose value internal
    // slot is set to value and whose unit internal slot is set to the name of the method as defined here.
    return CSSUnitValue::create(*vm.current_realm(), value, move(unit));
}

GC::Ref<CSSUnitValue> number(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "number"_fly_string);
}

GC::Ref<CSSUnitValue> percent(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "percent"_fly_string);
}

)~~~");

    units_data.for_each_member([&](auto& dimension_name, JsonValue const& dimension) {
        auto dimension_generator = generator.fork();
        dimension_generator.set("dimension:acceptable_cpp", make_name_acceptable_cpp(snake_casify(dimension_name, TrimLeadingUnderscores::Yes)));
        dimension_generator.appendln("\n// <@dimension:acceptable_cpp@>");

        dimension.as_object().for_each_member([&](auto& unit_name, JsonValue const&) {
            auto unit_generator = dimension_generator.fork();
            unit_generator.set("unit:name", unit_name);
            unit_generator.set("unit:acceptable_cpp", make_name_acceptable_cpp(unit_name.to_ascii_lowercase()));
            unit_generator.append(R"~~~(
GC::Ref<CSSUnitValue> @unit:acceptable_cpp@(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "@unit:name@"_fly_string);
}
)~~~");
        });
    });

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_idl_file(JsonObject& units_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
partial namespace CSS {
    CSSUnitValue number(double value);
    CSSUnitValue percent(double value);

)~~~");

    units_data.for_each_member([&](auto& dimension_name, JsonValue const& dimension) {
        auto dimension_generator = generator.fork();
        dimension_generator.set("dimension:acceptable_cpp", make_name_acceptable_cpp(snake_casify(dimension_name, TrimLeadingUnderscores::Yes)));
        dimension_generator.append(R"~~~(
    // <@dimension:acceptable_cpp@>
)~~~");

        dimension.as_object().for_each_member([&](auto& unit_name, JsonValue const&) {
            auto unit_generator = dimension_generator.fork();
            unit_generator.set("unit:name", unit_name);
            unit_generator.set("unit:acceptable_cpp", make_name_acceptable_cpp(unit_name.to_ascii_lowercase()));
            unit_generator.appendln("    [ImplementedAs=@unit:acceptable_cpp@] CSSUnitValue @unit:name@(double value);");
        });
    });

    generator.append(R"~~~(
};
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
