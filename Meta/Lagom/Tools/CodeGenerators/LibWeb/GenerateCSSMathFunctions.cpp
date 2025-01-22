/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

namespace {
ErrorOr<void> generate_header_file(JsonObject& functions_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
// This file is generated by GenerateCSSMathFunctions.cpp

#pragma once

namespace Web::CSS {

enum class MathFunction {
)~~~");

    functions_data.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.appendln("    @name:titlecase@,"sv);
    });

    generator.append(R"~~~(
};

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

String generate_calculation_type_check(StringView calculation_variable_name, StringView parameter_types)
{
    StringBuilder builder;
    auto allowed_types = parameter_types.split_view('|');
    bool first_type_check = true;
    for (auto const& allowed_type_name : allowed_types) {
        if (!first_type_check)
            builder.append(" || "sv);
        first_type_check = false;

        if (allowed_type_name == "<angle>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_angle(percentages_resolve_as)"sv);
        } else if (allowed_type_name == "<dimension>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_dimension()"sv);
        } else if (allowed_type_name == "<flex>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_flex(percentages_resolve_as)"sv);
        } else if (allowed_type_name == "<frequency>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_frequency(percentages_resolve_as)"sv);
        } else if (allowed_type_name == "<length>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_length(percentages_resolve_as)"sv);
        } else if (allowed_type_name == "<number>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_number(percentages_resolve_as)"sv);
        } else if (allowed_type_name == "<percentage>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_percentage()"sv);
        } else if (allowed_type_name == "<resolution>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_resolution(percentages_resolve_as)"sv);
        } else if (allowed_type_name == "<time>"sv) {
            builder.appendff("{}.{}", calculation_variable_name, "matches_time(percentages_resolve_as)"sv);
        } else {
            dbgln("I don't know what '{}' is!", allowed_type_name);
            VERIFY_NOT_REACHED();
        }
    }
    return MUST(builder.to_string());
}

ErrorOr<void> generate_implementation_file(JsonObject& functions_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
// This file is generated by GenerateCSSMathFunctions.cpp

#include <AK/Debug.h>
#include <LibWeb/CSS/MathFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS::Parser {

static Optional<RoundingStrategy> parse_rounding_strategy(Vector<ComponentValue> const& tokens)
{
    auto stream = TokenStream { tokens };
    stream.discard_whitespace();
    if (!stream.has_next_token())
        return {};

    auto& ident = stream.consume_a_token();
    if (!ident.is(Token::Type::Ident))
        return {};

    stream.discard_whitespace();
    if (stream.has_next_token())
        return {};

    auto maybe_keyword = keyword_from_string(ident.token().ident());
    if (!maybe_keyword.has_value())
        return {};

    return keyword_to_rounding_strategy(maybe_keyword.value());
}

RefPtr<CalculationNode> Parser::parse_math_function(Function const& function, CalculationContext const& context)
{
    TokenStream stream { function.value };
    auto arguments = parse_a_comma_separated_list_of_component_values(stream);
    auto const& percentages_resolve_as = context.percentages_resolve_as;
)~~~");

    functions_data.for_each_member([&](auto& name, JsonValue const& value) -> void {
        auto& function_data = value.as_object();
        auto& parameters = function_data.get_array("parameters"sv).value();
        auto parameter_validation_rule = function_data.get_byte_string("parameter-validation"sv);
        bool requires_same_parameters = parameter_validation_rule.has_value() ? (parameter_validation_rule == "same"sv) : true;

        auto function_generator = generator.fork();
        function_generator.set("name:lowercase", name);
        function_generator.set("name:titlecase", title_casify(name));
        function_generator.appendln("    if (function.name.equals_ignoring_ascii_case(\"@name:lowercase@\"sv)) {");
        if (function_data.get_bool("is-variadic"sv).value_or(false)) {
            // Variadic function
            function_generator.append(R"~~~(
        Optional<CSSNumericType> determined_argument_type;
        Vector<NonnullRefPtr<CalculationNode>> parsed_arguments;
        parsed_arguments.ensure_capacity(arguments.size());

        for (auto& argument : arguments) {
            auto calculation_node = parse_a_calculation(argument, context);
            if (!calculation_node) {
                dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument #{} is not a valid calculation", parsed_arguments.size());
                return nullptr;
            }

            auto maybe_argument_type = calculation_node->numeric_type();
            if (!maybe_argument_type.has_value()) {
                dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument #{} couldn't determine its type", parsed_arguments.size());
                return nullptr;
            }
            auto argument_type = maybe_argument_type.release_value();

)~~~");
            // Generate some type checks
            VERIFY(parameters.size() == 1);
            auto& parameter_data = parameters[0].as_object();
            auto parameter_type_string = parameter_data.get_byte_string("type"sv).value();
            function_generator.set("type_check", generate_calculation_type_check("argument_type"sv, parameter_type_string));
            function_generator.append(R"~~~(
            if (!(@type_check@)) {
                dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument #{} type ({}) is not an accepted type", parsed_arguments.size(), argument_type.dump());
                return nullptr;
            }

            if (!determined_argument_type.has_value()) {
                determined_argument_type = move(argument_type);
            } else {
)~~~");
            if (requires_same_parameters) {
                function_generator.append(R"~~~(
                if (determined_argument_type != argument_type) {
                    dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument #{} type ({}) doesn't match type of previous arguments ({})", parsed_arguments.size(), argument_type.dump(), determined_argument_type->dump());
                    return nullptr;
                }
)~~~");
            } else {
                function_generator.append(R"~~~(
                if (auto consistent_type = determined_argument_type->consistent_type(argument_type); consistent_type.has_value()) {
                    determined_argument_type = consistent_type.release_value();
                } else {
                    dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument #{} type ({}) is not consistent with type of previous arguments ({})", parsed_arguments.size(), argument_type.dump(), determined_argument_type->dump());
                    return nullptr;
                }
)~~~");
            }
            function_generator.append(R"~~~(
            }

            parsed_arguments.append(calculation_node.release_nonnull());
        }

        return @name:titlecase@CalculationNode::create(move(parsed_arguments));
    }
)~~~");

        } else {
            // Function with specified parameters.
            size_t min_argument_count = 0;
            size_t max_argument_count = parameters.size();
            parameters.for_each([&](JsonValue const& parameter_value) {
                auto& parameter = parameter_value.as_object();
                if (parameter.get_bool("required"sv) == true)
                    min_argument_count++;
            });
            function_generator.set("min_argument_count", String::number(min_argument_count));
            function_generator.set("max_argument_count", String::number(max_argument_count));

            function_generator.append(R"~~~(
        if (arguments.size() < @min_argument_count@ || arguments.size() > @max_argument_count@) {
            dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() has wrong number of arguments {}, expected between @min_argument_count@ and @max_argument_count@ inclusive", arguments.size());
            return nullptr;
        }
        size_t argument_index = 0;
        Optional<CSSNumericType> determined_argument_type;
)~~~");

            size_t parameter_index = 0;
            parameters.for_each([&](JsonValue const& parameter_value) {
                auto& parameter = parameter_value.as_object();
                auto parameter_type_string = parameter.get_byte_string("type"sv).value();
                auto parameter_required = parameter.get_bool("required"sv).value();

                auto parameter_generator = function_generator.fork();
                parameter_generator.set("parameter_name", parameter.get_byte_string("name"sv).value());
                parameter_generator.set("parameter_index", String::number(parameter_index));

                bool parameter_is_calculation;
                if (parameter_type_string == "<rounding-strategy>") {
                    parameter_is_calculation = false;
                    parameter_generator.set("parameter_type", "RoundingStrategy"_string);
                    parameter_generator.set("parse_function", "parse_rounding_strategy(arguments[argument_index])"_string);
                    parameter_generator.set("check_function", ".has_value()"_string);
                    parameter_generator.set("release_function", ".release_value()"_string);
                    if (auto default_value = parameter.get_byte_string("default"sv); default_value.has_value()) {
                        parameter_generator.set("parameter_default", MUST(String::formatted(" = RoundingStrategy::{}", title_casify(default_value.value()))));
                    } else {
                        parameter_generator.set("parameter_default", ""_string);
                    }
                } else {
                    // NOTE: This assumes everything not handled above is a calculation node of some kind.
                    parameter_is_calculation = true;
                    parameter_generator.set("parameter_type", "RefPtr<CalculationNode>"_string);
                    parameter_generator.set("parse_function", "parse_a_calculation(arguments[argument_index], context)"_string);
                    parameter_generator.set("check_function", " != nullptr"_string);
                    parameter_generator.set("release_function", ".release_nonnull()"_string);

                    // NOTE: We have exactly one default value in the data right now, and it's a `<calc-constant>`,
                    //       so that's all we handle.
                    if (auto default_value = parameter.get_byte_string("default"sv); default_value.has_value()) {
                        parameter_generator.set("parameter_default", MUST(String::formatted(" = ConstantCalculationNode::create(CalculationNode::constant_type_from_string(\"{}\"sv).value())", default_value.value())));
                    } else {
                        parameter_generator.set("parameter_default", ""_string);
                    }
                }

                parameter_generator.append(R"~~~(
        @parameter_type@ parameter_@parameter_index@@parameter_default@;
)~~~");

                if (parameter_required) {
                    parameter_generator.append(R"~~~(
        if (argument_index >= arguments.size()) {
            dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() missing required argument '@parameter_name@'");
            return nullptr;
        } else {
)~~~");
                } else {
                    parameter_generator.append(R"~~~(
        if (argument_index < arguments.size()) {
)~~~");
                }

                parameter_generator.append(R"~~~(
            auto maybe_parsed_argument_@parameter_index@ = @parse_function@;
            if (maybe_parsed_argument_@parameter_index@@check_function@) {
                parameter_@parameter_index@ = maybe_parsed_argument_@parameter_index@@release_function@;
                argument_index++;
)~~~");
                if (parameter_required) {
                    parameter_generator.append(R"~~~(
            } else {
                dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() required argument '@parameter_name@' failed to parse");
                return nullptr;
)~~~");
                }
                parameter_generator.append(R"~~~(
            }
        }
)~~~");

                if (parameter_is_calculation) {
                    auto parameter_type_variable = MUST(String::formatted("argument_type_{}", parameter_index));
                    parameter_generator.set("type_check", generate_calculation_type_check(parameter_type_variable, parameter_type_string));
                    parameter_generator.append(R"~~~(
        auto maybe_argument_type_@parameter_index@ = parameter_@parameter_index@->numeric_type();
        if (!maybe_argument_type_@parameter_index@.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument '@parameter_name@' couldn't determine its type");
            return nullptr;
        }
        auto argument_type_@parameter_index@ = maybe_argument_type_@parameter_index@.release_value();

        if (!(@type_check@)) {
            dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument '@parameter_name@' type ({}) is not an accepted type", argument_type_@parameter_index@.dump());
            return nullptr;
        }

        if (!determined_argument_type.has_value()) {
            determined_argument_type = argument_type_@parameter_index@;
        } else {
)~~~");

                    if (requires_same_parameters) {
                        parameter_generator.append(R"~~~(
            if (determined_argument_type != argument_type_@parameter_index@) {
                dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument '@parameter_name@' type ({}) doesn't match type of previous arguments ({})", argument_type_@parameter_index@.dump(), determined_argument_type->dump());
                return nullptr;
            }
)~~~");
                    } else {
                        parameter_generator.append(R"~~~(
            if (auto consistent_type = determined_argument_type->consistent_type(argument_type_@parameter_index@); consistent_type.has_value()) {
                determined_argument_type = consistent_type.release_value();
            } else {
                dbgln_if(CSS_PARSER_DEBUG, "@name:lowercase@() argument '@parameter_name@' type ({}) is not consistent with type of previous arguments ({})", argument_type_@parameter_index@.dump(), determined_argument_type->dump());
                return nullptr;
            }
)~~~");
                    }
                    parameter_generator.append(R"~~~(
        }
)~~~");
                }

                parameter_index++;
            });

            // Generate the call to the constructor
            function_generator.append("        return @name:titlecase@CalculationNode::create("sv);
            parameter_index = 0;
            parameters.for_each([&](JsonValue const& parameter_value) {
                auto& parameter = parameter_value.as_object();
                auto parameter_type_string = parameter.get_byte_string("type"sv).value();

                auto parameter_generator = function_generator.fork();
                parameter_generator.set("parameter_index"sv, String::number(parameter_index));

                if (parameter_type_string == "<rounding-strategy>"sv) {
                    parameter_generator.set("release_value"sv, ""_string);
                } else {
                    // NOTE: This assumes everything not handled above is a calculation node of some kind.
                    parameter_generator.set("release_value"sv, ".release_nonnull()"_string);
                }

                if (parameter_index == 0) {
                    parameter_generator.append("parameter_@parameter_index@@release_value@"sv);
                } else {
                    parameter_generator.append(", parameter_@parameter_index@@release_value@"sv);
                }
                parameter_index++;
            });
            function_generator.append(R"~~~();
    }
)~~~");
        }
    });

    generator.append(R"~~~(
    return nullptr;
}

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
} // end anonymous namespace

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the MathFunctions header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the MathFunctions implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(json_path));
    VERIFY(json.is_object());
    auto math_functions_data = json.as_object();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(math_functions_data, *generated_header_file));
    TRY(generate_implementation_file(math_functions_data, *generated_implementation_file));

    return 0;
}
