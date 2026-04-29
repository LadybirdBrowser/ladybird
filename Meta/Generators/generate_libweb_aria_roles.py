#!/usr/bin/env python3

# Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))


ARIA_NAME_TO_ENUM = {
    "aria-activedescendant": "AriaActiveDescendant",
    "aria-atomic": "AriaAtomic",
    "aria-autocomplete": "AriaAutoComplete",
    "aria-braillelabel": "AriaBrailleLabel",
    "aria-brailleroledescription": "AriaBrailleRoleDescription",
    "aria-busy": "AriaBusy",
    "aria-checked": "AriaChecked",
    "aria-colcount": "AriaColCount",
    "aria-colindex": "AriaColIndex",
    "aria-colindextext": "AriaColIndexText",
    "aria-colspan": "AriaColSpan",
    "aria-controls": "AriaControls",
    "aria-current": "AriaCurrent",
    "aria-describedby": "AriaDescribedBy",
    "aria-description": "AriaDescription",
    "aria-details": "AriaDetails",
    "aria-disabled": "AriaDisabled",
    "aria-dropeffect": "AriaDropEffect",
    "aria-errormessage": "AriaErrorMessage",
    "aria-expanded": "AriaExpanded",
    "aria-flowto": "AriaFlowTo",
    "aria-grabbed": "AriaGrabbed",
    "aria-haspopup": "AriaHasPopup",
    "aria-hidden": "AriaHidden",
    "aria-invalid": "AriaInvalid",
    "aria-keyshortcuts": "AriaKeyShortcuts",
    "aria-label": "AriaLabel",
    "aria-labelledby": "AriaLabelledBy",
    "aria-level": "AriaLevel",
    "aria-live": "AriaLive",
    "aria-modal": "AriaModal",
    "aria-multiline": "AriaMultiLine",
    "aria-multiselectable": "AriaMultiSelectable",
    "aria-orientation": "AriaOrientation",
    "aria-owns": "AriaOwns",
    "aria-placeholder": "AriaPlaceholder",
    "aria-posinset": "AriaPosInSet",
    "aria-pressed": "AriaPressed",
    "aria-readonly": "AriaReadOnly",
    "aria-relevant": "AriaRelevant",
    "aria-required": "AriaRequired",
    "aria-roledescription": "AriaRoleDescription",
    "aria-rowcount": "AriaRowCount",
    "aria-rowindex": "AriaRowIndex",
    "aria-rowindextext": "AriaRowIndexText",
    "aria-rowspan": "AriaRowSpan",
    "aria-selected": "AriaSelected",
    "aria-setsize": "AriaSetSize",
    "aria-sort": "AriaSort",
    "aria-valuemax": "AriaValueMax",
    "aria-valuemin": "AriaValueMin",
    "aria-valuenow": "AriaValueNow",
    "aria-valuetext": "AriaValueText",
}


def aria_name_to_enum_name(name: str) -> str:
    return ARIA_NAME_TO_ENUM[name]


def translate_aria_names_to_enum(names: list) -> list:
    return [aria_name_to_enum_name(name) for name in names]


def write_header_file(out: TextIO, roles_data: dict) -> None:
    out.write("""
#pragma once

#include <LibWeb/ARIA/RoleType.h>

namespace Web::ARIA {
""")

    for name, value in roles_data.items():
        spec_link = value["specLink"]
        description = value["description"]
        out.write(f"""
// {spec_link}
// {description}
class {name} :
""")
        first = True
        for super_class in value["superClassRoles"]:
            out.write(" " if first else ", ")
            out.write(f"public {super_class}")
            first = False

        out.write(f"""
{{
public:
    {name}(AriaData const&);

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
    {name}();
""")

        if value.get("nameFromSource") is not None:
            out.write("""
public:
    virtual NameFromSource name_from_source() const override;
""")
        out.write("};\n")

    out.write("}\n")


def generate_hash_table_member(
    out: TextIO,
    name: str,
    member_name: str,
    hash_table_name: str,
    enum_class: str,
    values: list,
) -> None:
    if len(values) == 0:
        out.write(f"""
HashTable<{enum_class}> const& {name}::{member_name}() const
{{
    static HashTable<{enum_class}> {hash_table_name};
    return {hash_table_name};
}}
""")
        return

    out.write(f"""
HashTable<{enum_class}> const& {name}::{member_name}() const
{{
    static HashTable<{enum_class}> {hash_table_name};
    if ({hash_table_name}.is_empty()) {{
        {hash_table_name}.ensure_capacity({len(values)});
""")
    for v in values:
        out.write(f"        {hash_table_name}.set({enum_class}::{v});\n")
    out.write(f"""
    }}
    return {hash_table_name};
}}
""")


def write_implementation_file(out: TextIO, roles_data: dict) -> None:
    out.write("""
#include <LibWeb/ARIA/AriaRoles.h>

namespace Web::ARIA {
""")

    for name, value in roles_data.items():
        supported_states = translate_aria_names_to_enum(value["supportedStates"])
        generate_hash_table_member(out, name, "supported_states", "states", "StateAndProperties", supported_states)
        supported_properties = translate_aria_names_to_enum(value["supportedProperties"])
        generate_hash_table_member(
            out, name, "supported_properties", "properties", "StateAndProperties", supported_properties
        )

        required_states = translate_aria_names_to_enum(value["requiredStates"])
        generate_hash_table_member(out, name, "required_states", "states", "StateAndProperties", required_states)
        required_properties = translate_aria_names_to_enum(value["requiredProperties"])
        generate_hash_table_member(
            out, name, "required_properties", "properties", "StateAndProperties", required_properties
        )

        prohibited_states = translate_aria_names_to_enum(value["prohibitedStates"])
        generate_hash_table_member(out, name, "prohibited_states", "states", "StateAndProperties", prohibited_states)
        prohibited_properties = translate_aria_names_to_enum(value["prohibitedProperties"])
        generate_hash_table_member(
            out, name, "prohibited_properties", "properties", "StateAndProperties", prohibited_properties
        )

        required_context_roles = value["requiredContextRoles"]
        generate_hash_table_member(out, name, "required_context_roles", "roles", "Role", required_context_roles)
        required_owned_elements = value["requiredOwnedElements"]
        generate_hash_table_member(out, name, "required_owned_elements", "roles", "Role", required_owned_elements)

        accessible_name_required = "true" if value["accessibleNameRequired"] else "false"
        children_are_presentational = "true" if value["childrenArePresentational"] else "false"
        parent = value["superClassRoles"][0]

        out.write(f"""
{name}::{name}() {{ }}

{name}::{name}(AriaData const& data)
    : {parent}(data)
{{
}}

bool {name}::accessible_name_required() const
{{
    return {accessible_name_required};
}}

bool {name}::children_are_presentational() const
{{
    return {children_are_presentational};
}}
""")

        implicit_value_for_role = value["implicitValueForRole"]
        if len(implicit_value_for_role) == 0:
            out.write(f"""
DefaultValueType {name}::default_value_for_property_or_state(StateAndProperties) const
{{
    return {{}};
}}
""")
        else:
            out.write(f"""
DefaultValueType {name}::default_value_for_property_or_state(StateAndProperties state_or_property) const
{{
    switch (state_or_property) {{
""")
            for state_name, implicit_value in implicit_value_for_role.items():
                out.write(f"""
    case StateAndProperties::{aria_name_to_enum_name(state_name)}:
        return {implicit_value};
""")
            out.write("""
    default:
        return {};
    }
}
""")

        name_from_source = value.get("nameFromSource")
        if name_from_source is not None:
            out.write(f"""
NameFromSource {name}::name_from_source() const
{{
    return NameFromSource::{name_from_source};
}}
""")

    out.write("}")


def main():
    parser = argparse.ArgumentParser(description="Generate ARIA Roles", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the AriaRoles header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the AriaRoles implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        roles_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, roles_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, roles_data)


if __name__ == "__main__":
    main()
