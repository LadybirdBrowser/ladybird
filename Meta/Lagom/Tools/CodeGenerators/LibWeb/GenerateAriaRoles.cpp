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
        class_definition_generator.set("spec_link"_sv, value_object.get_string("specLink"_sv).release_value());
        class_definition_generator.set("description"_sv, value_object.get_string("description"_sv).release_value());
        class_definition_generator.set("name"_sv, name);
        class_definition_generator.append(R"~~~(
// @spec_link@
// @description@
class @name@ :
)~~~");

        JsonArray const& super_classes = value_object.get_array("superClassRoles"_sv).value();
        bool first = true;
        super_classes.for_each([&](JsonValue const& value) {
            VERIFY(value.is_string());

            class_definition_generator.append(first ? " "_sv : ", "_sv);
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

        auto name_from_source = value.as_object().get("nameFromSource"_sv).value();
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
    member_generator.set("member_name"_sv, member_name);
    member_generator.set("hash_table_name"_sv, hash_table_name);
    member_generator.set("enum_class"_sv, enum_class);
    member_generator.set("hash_table_size"_sv, String::number(values.size()));

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
    if (name == "aria-activedescendant"_sv) {
        return "AriaActiveDescendant"_sv;
    } else if (name == "aria-atomic"_sv) {
        return "AriaAtomic"_sv;
    } else if (name == "aria-autocomplete"_sv) {
        return "AriaAutoComplete"_sv;
    } else if (name == "aria-braillelabel"_sv) {
        return "AriaBrailleLabel"_sv;
    } else if (name == "aria-brailleroledescription"_sv) {
        return "AriaBrailleRoleDescription"_sv;
    } else if (name == "aria-busy"_sv) {
        return "AriaBusy"_sv;
    } else if (name == "aria-checked"_sv) {
        return "AriaChecked"_sv;
    } else if (name == "aria-colcount"_sv) {
        return "AriaColCount"_sv;
    } else if (name == "aria-colindex"_sv) {
        return "AriaColIndex"_sv;
    } else if (name == "aria-colindextext"_sv) {
        return "AriaColIndexText"_sv;
    } else if (name == "aria-colspan"_sv) {
        return "AriaColSpan"_sv;
    } else if (name == "aria-controls"_sv) {
        return "AriaControls"_sv;
    } else if (name == "aria-current"_sv) {
        return "AriaCurrent"_sv;
    } else if (name == "aria-describedby"_sv) {
        return "AriaDescribedBy"_sv;
    } else if (name == "aria-description"_sv) {
        return "AriaDescription"_sv;
    } else if (name == "aria-details"_sv) {
        return "AriaDetails"_sv;
    } else if (name == "aria-disabled"_sv) {
        return "AriaDisabled"_sv;
    } else if (name == "aria-dropeffect"_sv) {
        return "AriaDropEffect"_sv;
    } else if (name == "aria-errormessage"_sv) {
        return "AriaErrorMessage"_sv;
    } else if (name == "aria-expanded"_sv) {
        return "AriaExpanded"_sv;
    } else if (name == "aria-flowto"_sv) {
        return "AriaFlowTo"_sv;
    } else if (name == "aria-grabbed"_sv) {
        return "AriaGrabbed"_sv;
    } else if (name == "aria-haspopup"_sv) {
        return "AriaHasPopup"_sv;
    } else if (name == "aria-hidden"_sv) {
        return "AriaHidden"_sv;
    } else if (name == "aria-invalid"_sv) {
        return "AriaInvalid"_sv;
    } else if (name == "aria-keyshortcuts"_sv) {
        return "AriaKeyShortcuts"_sv;
    } else if (name == "aria-label"_sv) {
        return "AriaLabel"_sv;
    } else if (name == "aria-labelledby"_sv) {
        return "AriaLabelledBy"_sv;
    } else if (name == "aria-level"_sv) {
        return "AriaLevel"_sv;
    } else if (name == "aria-live"_sv) {
        return "AriaLive"_sv;
    } else if (name == "aria-modal"_sv) {
        return "AriaModal"_sv;
    } else if (name == "aria-multiline"_sv) {
        return "AriaMultiLine"_sv;
    } else if (name == "aria-multiselectable"_sv) {
        return "AriaMultiSelectable"_sv;
    } else if (name == "aria-orientation"_sv) {
        return "AriaOrientation"_sv;
    } else if (name == "aria-owns"_sv) {
        return "AriaOwns"_sv;
    } else if (name == "aria-placeholder"_sv) {
        return "AriaPlaceholder"_sv;
    } else if (name == "aria-posinset"_sv) {
        return "AriaPosInSet"_sv;
    } else if (name == "aria-pressed"_sv) {
        return "AriaPressed"_sv;
    } else if (name == "aria-readonly"_sv) {
        return "AriaReadOnly"_sv;
    } else if (name == "aria-relevant"_sv) {
        return "AriaRelevant"_sv;
    } else if (name == "aria-required"_sv) {
        return "AriaRequired"_sv;
    } else if (name == "aria-roledescription"_sv) {
        return "AriaRoleDescription"_sv;
    } else if (name == "aria-rowcount"_sv) {
        return "AriaRowCount"_sv;
    } else if (name == "aria-rowindex"_sv) {
        return "AriaRowIndex"_sv;
    } else if (name == "aria-rowindextext"_sv) {
        return "AriaRowIndexText"_sv;
    } else if (name == "aria-rowspan"_sv) {
        return "AriaRowSpan"_sv;
    } else if (name == "aria-selected"_sv) {
        return "AriaSelected"_sv;
    } else if (name == "aria-setsize"_sv) {
        return "AriaSetSize"_sv;
    } else if (name == "aria-sort"_sv) {
        return "AriaSort"_sv;
    } else if (name == "aria-valuemax"_sv) {
        return "AriaValueMax"_sv;
    } else if (name == "aria-valuemin"_sv) {
        return "AriaValueMin"_sv;
    } else if (name == "aria-valuenow"_sv) {
        return "AriaValueNow"_sv;
    } else if (name == "aria-valuetext"_sv) {
        return "AriaValueText"_sv;
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
        member_generator.set("name"_sv, name);

        JsonObject const& value_object = value.as_object();

        JsonArray const& supported_states = translate_aria_names_to_enum(value_object.get_array("supportedStates"_sv).value());
        generate_hash_table_member(member_generator, "supported_states"_sv, "states"_sv, "StateAndProperties"_sv, supported_states);
        JsonArray const& supported_properties = translate_aria_names_to_enum(value_object.get_array("supportedProperties"_sv).value());
        generate_hash_table_member(member_generator, "supported_properties"_sv, "properties"_sv, "StateAndProperties"_sv, supported_properties);

        JsonArray const& required_states = translate_aria_names_to_enum(value_object.get_array("requiredStates"_sv).value());
        generate_hash_table_member(member_generator, "required_states"_sv, "states"_sv, "StateAndProperties"_sv, required_states);
        JsonArray const& required_properties = translate_aria_names_to_enum(value_object.get_array("requiredProperties"_sv).value());
        generate_hash_table_member(member_generator, "required_properties"_sv, "properties"_sv, "StateAndProperties"_sv, required_properties);

        JsonArray const& prohibited_states = translate_aria_names_to_enum(value_object.get_array("prohibitedStates"_sv).value());
        generate_hash_table_member(member_generator, "prohibited_states"_sv, "states"_sv, "StateAndProperties"_sv, prohibited_states);
        JsonArray const& prohibited_properties = translate_aria_names_to_enum(value_object.get_array("prohibitedProperties"_sv).value());
        generate_hash_table_member(member_generator, "prohibited_properties"_sv, "properties"_sv, "StateAndProperties"_sv, prohibited_properties);

        JsonArray const& required_context_roles = value_object.get_array("requiredContextRoles"_sv).value();
        generate_hash_table_member(member_generator, "required_context_roles"_sv, "roles"_sv, "Role"_sv, required_context_roles);
        JsonArray const& required_owned_elements = value_object.get_array("requiredOwnedElements"_sv).value();
        generate_hash_table_member(member_generator, "required_owned_elements"_sv, "roles"_sv, "Role"_sv, required_owned_elements);

        bool accessible_name_required = value_object.get_bool("accessibleNameRequired"_sv).value();
        member_generator.set("accessible_name_required"_sv, accessible_name_required ? "true"_sv : "false"_sv);
        bool children_are_presentational = value_object.get_bool("childrenArePresentational"_sv).value();
        member_generator.set("children_are_presentational", children_are_presentational ? "true"_sv : "false"_sv);

        JsonArray const& super_classes = value.as_object().get_array("superClassRoles"_sv).value();
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

        JsonObject const& implicit_value_for_role = value_object.get_object("implicitValueForRole"_sv).value();
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
                case_generator.set("state_or_property"_sv, aria_name_to_enum_name(name));
                case_generator.set("implicit_value"_sv, value.as_string());
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

        JsonValue const& name_from_source = value.as_object().get("nameFromSource"_sv).value();
        if (!name_from_source.is_null()) {
            member_generator.set("name_from_source"_sv, name_from_source.as_string());
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
