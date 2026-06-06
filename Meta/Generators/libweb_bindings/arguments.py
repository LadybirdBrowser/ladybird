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
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")

    for index, parameter in enumerate(parameters):
        if parameter.variadic:
            write_variadic_operation_parameter_conversion(out, parameter, index, includes, context)
            continue

        argument_value_name = f"arg{index}"
        parameter_name = idl_identifier_cpp_name(parameter)

        out.write(f"    auto {argument_value_name} = vm.argument({index});\n")

        if parameter.optional and parameter.default_value is None:
            out.write(
                f"""    {operation_parameter_cpp_type(parameter, context)} {parameter_name} {{}};
    if (!{argument_value_name}.is_undefined())
        {parameter_name} = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {to_idl_value(parameter, argument_value_name, includes, context)}; }}));

"""
            )
            continue

        if parameter.optional:
            out.write(
                f"""    {cpp_value_type(parameter, context)} {parameter_name} = {cpp_default_value_conversion(parameter, context)};
    if (!{argument_value_name}.is_undefined())
        {parameter_name} = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {to_idl_value(parameter, argument_value_name, includes, context)}; }}));

"""
            )
            continue

        out.write(
            f"""    auto {parameter_name} = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {to_idl_value(parameter, argument_value_name, includes, context)}; }}));

"""
        )


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
            auto argument = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {to_idl_value(parameter, "vm.argument(i)", includes, context)}; }}));
            {parameter_name}.unchecked_append(move(argument));
        }}
    }}

"""
    )
