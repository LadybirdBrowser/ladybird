/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <AK/String.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

namespace {
ErrorOr<void> generate_header_file(JsonObject& roles_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <LibWeb/ARIA/RoleType.h>

namespace Web::ARIA {
)~~~");

    roles_data.for_each_member([&](auto& name, auto& value) -> void {
        VERIFY(value.is_object());
        JsonObject const& value_object = value.as_object();

        auto class_definition_generator = generator.fork();
        class_definition_generator.set("spec_link"sv, value_object.get_byte_string("specLink"sv).value());
        class_definition_generator.set("description"sv, value_object.get_byte_string("description"sv).value());
        class_definition_generator.set("name"sv, name);
        class_definition_generator.append(R"~~~(
// @spec_link@
// @description@
class @name@ :
)~~~");

        JsonArray const& super_classes = value_object.get_array("superClassRoles"sv).value();
        bool first = true;
        super_classes.for_each([&](JsonValue const& value) {
            VERIFY(value.is_string());

            class_definition_generator.append(first ? " "sv : ", "sv);
            class_definition_generator.append(MUST(String::formatted("public {}", value.as_string())));
            first = false;
        });

        class_definition_generator.append(R"~~~(
{
public:
    @name@(AriaData const&);

    virtual HashTable<StateAndProperties> const& supported_states() const override;
    virtual HashTable<StateAndProperties> const& supported_properties() const override;

    virtual HashTable<StateAndProperties> const& required_states() const override;
    virtual HashTable<StateAndProperties> const& required_properties() const override;

    virtual HashTable<StateAndProperties> const& prohibited_properties() const override;
    virtual HashTable<StateAndProperties> const& prohibited_states() const override;

    virtual HashTable<Role> const& required_context_roles() const override;
    virtual HashTable<Role> const& required_owned_elements() const override;
    virtual bool accessible_name_required() const override;
    virtual bool children_are_presentational() const override;
    virtual DefaultValueType default_value_for_property_or_state(StateAndProperties) const override;
protected:
    @name@();
)~~~");

        auto name_from_source = value.as_object().get("nameFromSource"sv).value();
        if (!name_from_source.is_null())
            class_definition_generator.append(R"~~~(
public:
    virtual NameFromSource name_from_source() const override;
)~~~");
        class_definition_generator.appendln("};");
    });

    generator.appendln("}");

    TRY(file.write_until_depleted((generator.as_string_view().bytes())));
    return {};
}

String generate_hash_table_population(JsonArray const& values, StringView hash_table_name, StringView enum_class)
{
    StringBuilder builder;
    values.for_each([&](auto& value) {
        VERIFY(value.is_string());
        builder.appendff("        {}.set({}::{});\n", hash_table_name, enum_class, value.as_string());
    });

    return MUST(builder.to_string());
}

void generate_hash_table_member(SourceGenerator& generator, StringView member_name, StringView hash_table_name, StringView enum_class, JsonArray const& values)
{
    auto member_generator = generator.fork();
    member_generator.set("member_name"sv, member_name);
    member_generator.set("hash_table_name"sv, hash_table_name);
    member_generator.set("enum_class"sv, enum_class);
    member_generator.set("hash_table_size"sv, String::number(values.size()));

    if (values.size() == 0) {
        member_generator.append(R"~~~(
HashTable<@enum_class@> const& @name@::@member_name@() const
{
    static HashTable<@enum_class@> @hash_table_name@;
    return @hash_table_name@;
}
)~~~");
        return;
    }

    member_generator.append(R"~~~(
HashTable<@enum_class@> const& @name@::@member_name@() const
{
    static HashTable<@enum_class@> @hash_table_name@;
    if (@hash_table_name@.is_empty()) {
        @hash_table_name@.ensure_capacity(@hash_table_size@);
)~~~");
    member_generator.append(generate_hash_table_population(values, hash_table_name, enum_class));
    member_generator.append(R"~~~(
    }
    return @hash_table_name@;
}
)~~~");
}

StringView aria_name_to_enum_name(StringView name)
{
    if (name == "aria-activedescendant"sv) {
        return "AriaActiveDescendant"sv;
    } else if (name == "aria-atomic"sv) {
        return "AriaAtomic"sv;
    } else if (name == "aria-autocomplete"sv) {
        return "AriaAutoComplete"sv;
    } else if (name == "aria-braillelabel"sv) {
        return "AriaBrailleLabel"sv;
    } else if (name == "aria-brailleroledescription"sv) {
        return "AriaBrailleRoleDescription"sv;
    } else if (name == "aria-busy"sv) {
        return "AriaBusy"sv;
    } else if (name == "aria-checked"sv) {
        return "AriaChecked"sv;
    } else if (name == "aria-colcount"sv) {
        return "AriaColCount"sv;
    } else if (name == "aria-colindex"sv) {
        return "AriaColIndex"sv;
    } else if (name == "aria-colindextext"sv) {
        return "AriaColIndexText"sv;
    } else if (name == "aria-colspan"sv) {
        return "AriaColSpan"sv;
    } else if (name == "aria-controls"sv) {
        return "AriaControls"sv;
    } else if (name == "aria-current"sv) {
        return "AriaCurrent"sv;
    } else if (name == "aria-describedby"sv) {
        return "AriaDescribedBy"sv;
    } else if (name == "aria-description"sv) {
        return "AriaDescription"sv;
    } else if (name == "aria-details"sv) {
        return "AriaDetails"sv;
    } else if (name == "aria-disabled"sv) {
        return "AriaDisabled"sv;
    } else if (name == "aria-dropeffect"sv) {
        return "AriaDropEffect"sv;
    } else if (name == "aria-errormessage"sv) {
        return "AriaErrorMessage"sv;
    } else if (name == "aria-expanded"sv) {
        return "AriaExpanded"sv;
    } else if (name == "aria-flowto"sv) {
        return "AriaFlowTo"sv;
    } else if (name == "aria-grabbed"sv) {
        return "AriaGrabbed"sv;
    } else if (name == "aria-haspopup"sv) {
        return "AriaHasPopup"sv;
    } else if (name == "aria-hidden"sv) {
        return "AriaHidden"sv;
    } else if (name == "aria-invalid"sv) {
        return "AriaInvalid"sv;
    } else if (name == "aria-keyshortcuts"sv) {
        return "AriaKeyShortcuts"sv;
    } else if (name == "aria-label"sv) {
        return "AriaLabel"sv;
    } else if (name == "aria-labelledby"sv) {
        return "AriaLabelledBy"sv;
    } else if (name == "aria-level"sv) {
        return "AriaLevel"sv;
    } else if (name == "aria-live"sv) {
        return "AriaLive"sv;
    } else if (name == "aria-modal"sv) {
        return "AriaModal"sv;
    } else if (name == "aria-multiline"sv) {
        return "AriaMultiLine"sv;
    } else if (name == "aria-multiselectable"sv) {
        return "AriaMultiSelectable"sv;
    } else if (name == "aria-orientation"sv) {
        return "AriaOrientation"sv;
    } else if (name == "aria-owns"sv) {
        return "AriaOwns"sv;
    } else if (name == "aria-placeholder"sv) {
        return "AriaPlaceholder"sv;
    } else if (name == "aria-posinset"sv) {
        return "AriaPosInSet"sv;
    } else if (name == "aria-pressed"sv) {
        return "AriaPressed"sv;
    } else if (name == "aria-readonly"sv) {
        return "AriaReadOnly"sv;
    } else if (name == "aria-relevant"sv) {
        return "AriaRelevant"sv;
    } else if (name == "aria-required"sv) {
        return "AriaRequired"sv;
    } else if (name == "aria-roledescription"sv) {
        return "AriaRoleDescription"sv;
    } else if (name == "aria-rowcount"sv) {
        return "AriaRowCount"sv;
    } else if (name == "aria-rowindex"sv) {
        return "AriaRowIndex"sv;
    } else if (name == "aria-rowindextext"sv) {
        return "AriaRowIndexText"sv;
    } else if (name == "aria-rowspan"sv) {
        return "AriaRowSpan"sv;
    } else if (name == "aria-selected"sv) {
        return "AriaSelected"sv;
    } else if (name == "aria-setsize"sv) {
        return "AriaSetSize"sv;
    } else if (name == "aria-sort"sv) {
        return "AriaSort"sv;
    } else if (name == "aria-valuemax"sv) {
        return "AriaValueMax"sv;
    } else if (name == "aria-valuemin"sv) {
        return "AriaValueMin"sv;
    } else if (name == "aria-valuenow"sv) {
        return "AriaValueNow"sv;
    } else if (name == "aria-valuetext"sv) {
        return "AriaValueText"sv;
    } else {
        VERIFY_NOT_REACHED();
    }
}

JsonArray translate_aria_names_to_enum(JsonArray const& names)
{
    JsonArray translated_names;
    names.for_each([&](JsonValue const& value) {
        VERIFY(value.is_string());
        auto name = value.as_string();
        MUST(translated_names.append(aria_name_to_enum_name(name)));
    });
    return translated_names;
}

ErrorOr<void> generate_implementation_file(JsonObject& roles_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <LibWeb/ARIA/AriaRoles.h>

namespace Web::ARIA {
)~~~");

    roles_data.for_each_member([&](auto& name, auto& value) -> void {
        VERIFY(value.is_object());

        auto member_generator = generator.fork();
        member_generator.set("name"sv, name);

        JsonObject const& value_object = value.as_object();

        JsonArray const& supported_states = translate_aria_names_to_enum(value_object.get_array("supportedStates"sv).value());
        generate_hash_table_member(member_generator, "supported_states"sv, "states"sv, "StateAndProperties"sv, supported_states);
        JsonArray const& supported_properties = translate_aria_names_to_enum(value_object.get_array("supportedProperties"sv).value());
        generate_hash_table_member(member_generator, "supported_properties"sv, "properties"sv, "StateAndProperties"sv, supported_properties);

        JsonArray const& required_states = translate_aria_names_to_enum(value_object.get_array("requiredStates"sv).value());
        generate_hash_table_member(member_generator, "required_states"sv, "states"sv, "StateAndProperties"sv, required_states);
        JsonArray const& required_properties = translate_aria_names_to_enum(value_object.get_array("requiredProperties"sv).value());
        generate_hash_table_member(member_generator, "required_properties"sv, "properties"sv, "StateAndProperties"sv, required_properties);

        JsonArray const& prohibited_states = translate_aria_names_to_enum(value_object.get_array("prohibitedStates"sv).value());
        generate_hash_table_member(member_generator, "prohibited_states"sv, "states"sv, "StateAndProperties"sv, prohibited_states);
        JsonArray const& prohibited_properties = translate_aria_names_to_enum(value_object.get_array("prohibitedProperties"sv).value());
        generate_hash_table_member(member_generator, "prohibited_properties"sv, "properties"sv, "StateAndProperties"sv, prohibited_properties);

        JsonArray const& required_context_roles = value_object.get_array("requiredContextRoles"sv).value();
        generate_hash_table_member(member_generator, "required_context_roles"sv, "roles"sv, "Role"sv, required_context_roles);
        JsonArray const& required_owned_elements = value_object.get_array("requiredOwnedElements"sv).value();
        generate_hash_table_member(member_generator, "required_owned_elements"sv, "roles"sv, "Role"sv, required_owned_elements);

        bool accessible_name_required = value_object.get_bool("accessibleNameRequired"sv).value();
        member_generator.set("accessible_name_required"sv, accessible_name_required ? "true"sv : "false"sv);
        bool children_are_presentational = value_object.get_bool("childrenArePresentational"sv).value();
        member_generator.set("children_are_presentational", children_are_presentational ? "true"sv : "false"sv);

        JsonArray const& super_classes = value.as_object().get_array("superClassRoles"sv).value();
        member_generator.set("parent", super_classes.at(0).as_string());

        member_generator.append(R"~~~(
@name@::@name@() { }

@name@::@name@(AriaData const& data)
    : @parent@(data)
{
}

bool @name@::accessible_name_required() const
{
    return @accessible_name_required@;
}

bool @name@::children_are_presentational() const
{
    return @children_are_presentational@;
}
)~~~");

        JsonObject const& implicit_value_for_role = value_object.get_object("implicitValueForRole"sv).value();
        if (implicit_value_for_role.size() == 0) {
            member_generator.append(R"~~~(
DefaultValueType @name@::default_value_for_property_or_state(StateAndProperties) const
{
    return {};
}
)~~~");
        } else {
            member_generator.append(R"~~~(
DefaultValueType @name@::default_value_for_property_or_state(StateAndProperties state_or_property) const
{
    switch (state_or_property) {
)~~~");
            implicit_value_for_role.for_each_member([&](auto& name, auto& value) {
                auto case_generator = member_generator.fork();
                VERIFY(value.is_string());
                case_generator.set("state_or_property"sv, aria_name_to_enum_name(name));
                case_generator.set("implicit_value"sv, value.as_string());
                case_generator.append(R"~~~(
    case StateAndProperties::@state_or_property@:
        return @implicit_value@;
)~~~");
            });
            member_generator.append(R"~~~(
    default:
        return {};
    }
}
)~~~");
        }

        JsonValue const& name_from_source = value.as_object().get("nameFromSource"sv).value();
        if (!name_from_source.is_null()) {
            member_generator.set("name_from_source"sv, name_from_source.as_string());
            member_generator.append(R"~~~(
NameFromSource @name@::name_from_source() const
{
    return NameFromSource::@name_from_source@;
}
)~~~");
        }
    });

    generator.append("}");

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
    args_parser.add_option(generated_header_path, "Path to the TransformFunctions header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the TransformFunctions implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(json_path));
    VERIFY(json.is_object());
    auto roles_data = json.as_object();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(roles_data, *generated_header_file));
    TRY(generate_implementation_file(roles_data, *generated_implementation_file));

    return 0;
}
