# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import TypeOptionality
from Generators.libweb_bindings.cpp_types import add_include_for_contained_storage_type
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type_details
from Generators.libweb_bindings.cpp_types import cpp_value_type
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.default_values import cpp_default_value_conversion
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.to_idl_value import argument_with_string_to_idl_value
from Generators.libweb_bindings.to_idl_value import string_argument_to_idl_value
from Generators.libweb_bindings.to_idl_value import to_idl_value
from Utils.webidl_parser import OperationParameter


def operation_parameter_cpp_type(parameter: OperationParameter, context: GenerationContext) -> str:
    if parameter.variadic:
        cpp_type = cpp_type_for_idl_type_details(
            parameter.type,
            context,
            extended_attributes=parameter.extended_attributes,
        )
        storage_type_name = cpp_type.contained_storage_type.value
        return f"{storage_type_name}<{cpp_type.name}>"

    if parameter.optional:
        return cpp_type_for_idl_type(
            parameter.type,
            context,
            optionality=TypeOptionality.OptionalArgument,
            extended_attributes=parameter.extended_attributes,
        )

    return cpp_value_type(parameter, context)


def write_operation_parameter_conversions(
    out: TextIO,
    parameters: list[OperationParameter],
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> None:
    includes.add("LibWeb/WebIDL/ExceptionOrUtils.h")

    for index, parameter in enumerate(parameters):
        if parameter.variadic:
            write_variadic_operation_parameter_conversion(out, parameter, index, includes, context)
            continue

        argument_value_name = f"arg{index}"
        parameter_name = idl_identifier_cpp_name(parameter)

        out.write(f"    auto {argument_value_name} = vm.argument({index});\n")

        if write_string_lending_parameter_conversion(out, parameter, argument_value_name, includes, context):
            continue

        if parameter.optional and parameter.default_value is None:
            out.write(
                f"""    {operation_parameter_cpp_type(parameter, context)} {parameter_name} {{}};
    if (!{argument_value_name}.is_undefined())
        {parameter_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {to_idl_value(parameter, argument_value_name, includes, context)}; }}));

"""
            )
            continue

        if parameter.optional:
            out.write(
                f"""    {cpp_value_type(parameter, context)} {parameter_name} = {cpp_default_value_conversion(parameter, context)};
    if (!{argument_value_name}.is_undefined())
        {parameter_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {to_idl_value(parameter, argument_value_name, includes, context)}; }}));

"""
            )
            continue

        out.write(
            f"""    auto {parameter_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {to_idl_value(parameter, argument_value_name, includes, context)}; }}));

"""
        )


def write_string_lending_parameter_conversion(
    out: TextIO,
    parameter: OperationParameter,
    argument_value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> bool:
    """Writes the conversion of a parameter that reads the string of a String value where the value holds it, and
    returns whether the parameter is one: a string, a nullable string or a union with a string member."""
    parameter_name = idl_identifier_cpp_name(parameter)
    is_undefined = f"{argument_value_name}.is_undefined()"
    has_default = parameter.optional and parameter.default_value is not None
    is_optional_without_default = parameter.optional and parameter.default_value is None

    string_argument = string_argument_to_idl_value(parameter, argument_value_name, includes)
    if string_argument is not None and not is_optional_without_default:
        if has_default:
            default_value = cpp_default_value_conversion(parameter, context)
            string_argument = f"{is_undefined} ? WebIDL::StringArgument::owned({default_value}) : {string_argument}"
        out.write(
            f"""    auto {argument_value_name}_string = {string_argument};
    auto const& {parameter_name} = {argument_value_name}_string.string();

"""
        )
        return True

    if is_optional_without_default:
        cpp_type_name = operation_parameter_cpp_type(parameter, context)
    else:
        cpp_type_name = cpp_value_type(parameter, context)

    def owned_value() -> str:
        value = f"TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {to_idl_value(parameter, argument_value_name, includes, context)}; }}))"
        if is_optional_without_default:
            return f"{is_undefined} ? {cpp_type_name} {{}} : {cpp_type_name} {{ {value} }}"
        if has_default:
            default_value = cpp_default_value_conversion(parameter, context)
            return f"{is_undefined} ? {cpp_type_name} {{ {default_value} }} : {cpp_type_name} {{ {value} }}"
        return value

    argument = argument_with_string_to_idl_value(
        parameter, cpp_type_name, argument_value_name, owned_value, includes, context
    )
    if argument is None:
        return False
    out.write(
        f"""    auto {argument_value_name}_argument = {argument};
    auto const& {parameter_name} = {argument_value_name}_argument.value();

"""
    )
    return True


def write_variadic_operation_parameter_conversion(
    out: TextIO,
    parameter: OperationParameter,
    index: int,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> None:
    cpp_type = cpp_type_for_idl_type_details(
        parameter.type,
        context,
        extended_attributes=parameter.extended_attributes,
    )
    add_include_for_contained_storage_type(cpp_type.contained_storage_type, includes)

    parameter_name = idl_identifier_cpp_name(parameter)
    parameter_cpp_type = operation_parameter_cpp_type(parameter, context)

    out.write(
        f"""    {parameter_cpp_type} {parameter_name};
    if (vm.argument_count() > {index}) {{
        {parameter_name}.ensure_capacity(vm.argument_count() - {index});
        for (size_t i = {index}; i < vm.argument_count(); ++i) {{
            auto argument = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {to_idl_value(parameter, "vm.argument(i)", includes, context)}; }}));
            {parameter_name}.unchecked_append(move(argument));
        }}
    }}

"""
    )
