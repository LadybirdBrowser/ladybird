# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Optional
from typing import TextIO

from Generators.libweb_bindings import overload_resolution
from Generators.libweb_bindings.arguments import write_operation_parameter_conversions
from Generators.libweb_bindings.attributes import reflected_attribute_name
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import idl_implementation_cpp_name
from Generators.libweb_bindings.cpp_types import is_numeric_type
from Generators.libweb_bindings.cpp_types import is_string_type
from Generators.libweb_bindings.extended_attributes import wrap_with_ce_reactions
from Generators.libweb_bindings.extended_attributes import wrap_with_extended_attribute_exposure_checks
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Utils.webidl_parser import Attribute
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Operation


# https://webidl.spec.whatwg.org/#js-default-operations
def operation_is_default_to_json(operation: Operation) -> bool:
    return (
        operation.name == "toJSON"
        and "Default" in operation.extended_attributes
        and operation.return_type.name == "object"
    )


# https://webidl.spec.whatwg.org/#dfn-json-types
def idl_type_is_json_type(idl_type: IDLType, context: GenerationContext) -> bool:
    # The JSON types are:
    # * nullable types whose inner type is a JSON type,
    if idl_type.nullable:
        return idl_type_is_json_type(idl_type.without_nullable(), context)

    if isinstance(idl_type, IDLParameterizedType):
        # * sequence types whose parameterized type is a JSON type,
        # * frozen array types whose parameterized type is a JSON type,
        if idl_type.name in ("sequence", "FrozenArray"):
            return idl_type_is_json_type(idl_type.parameters[0], context)
        # * records where all of their values are JSON types,
        if idl_type.name == "record":
            return idl_type_is_json_type(idl_type.parameters[1], context)

    # * interface types that have a toJSON operation declared on themselves or one of their inherited interfaces.
    interface = context.interface(idl_type)
    if interface is not None:
        return any(
            operation.name == "toJSON" and "Default" in operation.extended_attributes
            for interface_in_chain in context.inheritance_stack(interface)
            for operation in interface_in_chain.regular_operations
        )

    # FIXME: * dictionary types where the types of all members declared on the dictionary and all its inherited dictionaries are JSON types,

    # * numeric types,
    # * boolean,
    # * string types,
    # * object,
    return (
        is_numeric_type(idl_type.name)
        or idl_type.name == "boolean"
        or is_string_type(idl_type.name)
        or idl_type.name == "object"
        or context.enumeration(idl_type) is not None
    )


def define_the_regular_operations(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
    unforgeable: bool = False,
) -> None:
    for name, operations in overload_resolution.operation_overload_sets(interface).items():
        if any("LegacyUnforgeable" in operation.extended_attributes for operation in operations) != unforgeable:
            continue
        operation = operations[0]
        out.write(
            wrap_with_extended_attribute_exposure_checks(
                includes,
                operation.extended_attributes,
                f"""    object.define_native_function(realm, "{name}"_utf16_fly_string, {idl_identifier_cpp_name(operation)}, {overload_resolution.operation_overload_set_length(operations)}, default_attributes);

""",
            )
        )

    if unforgeable:
        return

    implemented_operation_names = overload_resolution.operation_overload_sets(interface).keys()
    for operation in interface.regular_operations:
        if "FIXME" not in operation.extended_attributes:
            continue
        if operation.name in implemented_operation_names:
            continue
        out.write(
            f"""    object.define_direct_property("{operation.name}"_utf16_fly_string, JS::js_undefined(), default_attributes | JS::Attribute::Unimplemented);

"""
        )


def define_the_static_operations(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    for name, operations in overload_resolution.operation_overload_sets(interface, static=True).items():
        operation = operations[0]
        out.write(
            wrap_with_extended_attribute_exposure_checks(
                includes,
                operation.extended_attributes,
                f"""    object.define_native_function(realm, "{name}"_utf16_fly_string, {idl_identifier_cpp_name(operation)}, {overload_resolution.operation_overload_set_length(operations)}, JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable);

""",
            )
        )


def define_unscopable_members(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    unscopable_names = []
    for attribute in interface.regular_attributes:
        if "FIXME" not in attribute.extended_attributes and "Unscopable" in attribute.extended_attributes:
            unscopable_names.append(attribute.name)
    for name, operations in overload_resolution.operation_overload_sets(interface).items():
        if all("Unscopable" in operation.extended_attributes for operation in operations):
            unscopable_names.append(name)
    if not unscopable_names:
        return

    includes.add("LibJS/Runtime/Object.h")
    out.write(
        """    auto unscopable_object = JS::Object::create(realm, nullptr);
"""
    )
    for name in unscopable_names:
        out.write(
            f"""    MUST(unscopable_object->create_data_property("{name}"_utf16_fly_string, JS::Value(true)));
"""
        )
    out.write(
        """    object.define_direct_property(vm.well_known_symbol_unscopables(), unscopable_object, JS::Attribute::Configurable);

"""
    )


def define_the_stringifier(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
    unforgeable: bool = False,
) -> None:
    if interface.stringifier is None:
        return

    extended_attributes = interface.stringifier.extended_attributes
    if ("LegacyUnforgeable" in extended_attributes) != unforgeable:
        return

    out.write(
        wrap_with_extended_attribute_exposure_checks(
            includes,
            extended_attributes,
            """    object.define_native_function(realm, "toString"_utf16_fly_string, to_string, 0, default_attributes);
""",
        )
    )
    out.write(
        """

"""
    )


def write_regular_operations(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    for operations in overload_resolution.operation_overload_sets(interface).values():
        if len(operations) > 1:
            receiver_class = interface.namespace_class if interface.is_namespace else interface.prototype_class
            overload_resolution.write_overload_arbiter(
                out, context, includes, interface, operations, receiver_class=receiver_class
            )
            for overload_index, operation in enumerate(operations):
                write_operation(out, context, includes, interface, operation, overload_index)
        else:
            write_operation(out, context, includes, interface, operations[0])


def write_regular_operations_for_receiver(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    receiver_class: str,
    defined_callbacks: Optional[set[str]] = None,
) -> None:
    for operations in overload_resolution.operation_overload_sets(interface).values():
        callbacks = [
            idl_identifier_cpp_name(operation, suffix=overload_index if len(operations) > 1 else None)
            for overload_index, operation in enumerate(operations)
        ]
        callbacks.append(idl_identifier_cpp_name(operations[0]))
        if defined_callbacks is not None and all(callback in defined_callbacks for callback in callbacks):
            continue
        if defined_callbacks is not None:
            defined_callbacks.update(callbacks)
        if len(operations) > 1:
            overload_resolution.write_overload_arbiter(
                out, context, includes, interface, operations, receiver_class=receiver_class
            )
            for overload_index, operation in enumerate(operations):
                write_operation(out, context, includes, interface, operation, overload_index, receiver_class)
        else:
            write_operation(out, context, includes, interface, operations[0], receiver_class=receiver_class)


def write_static_operations(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    for operations in overload_resolution.operation_overload_sets(interface, static=True).values():
        if len(operations) > 1:
            overload_resolution.write_overload_arbiter(
                out,
                context,
                includes,
                interface,
                operations,
                receiver_class=interface.constructor_class,
            )
            for overload_index, operation in enumerate(operations):
                write_operation(out, context, includes, interface, operation, overload_index, emit_as_static=True)
        else:
            write_operation(out, context, includes, interface, operations[0], emit_as_static=True)


def write_stringifier(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    receiver_class: Optional[str] = None,
) -> None:
    if receiver_class is None:
        receiver_class = interface.prototype_class

    if interface.stringifier is None:
        return

    attribute = interface.stringifier.attribute
    stringifier_type = attribute.type if attribute is not None else IDLType("DOMString")
    stringifier_cpp_name = idl_implementation_cpp_name(attribute) if attribute is not None else "to_string"
    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({receiver_class}::to_string)
{{
    WebIDL::log_trace(vm, "{receiver_class}::to_string");
    auto* idl_object = TRY(impl_from(vm));
    auto R = TRY(throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{stringifier_cpp_name}(); }}));
    return {to_javascript_value(stringifier_type, "R", includes, context)};
}}

"""
    )


# https://webidl.spec.whatwg.org/#dfn-create-operation-function
def write_operation(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    operation: Operation,
    overload_index: Optional[int] = None,
    receiver_class: Optional[str] = None,
    emit_as_static: bool = False,
) -> None:
    if operation_is_default_to_json(operation):
        write_default_to_json_operation(out, context, includes, interface, operation)
        return

    return_type_is_promise = operation.return_type.name == "Promise"

    arguments = ", ".join(idl_identifier_cpp_name(parameter) for parameter in operation.parameters)
    callee_arguments = arguments
    operation_invokes_as_static = emit_as_static or interface.is_namespace
    if operation_invokes_as_static:
        callee_arguments = f"vm, {arguments}" if arguments else "vm"
    callback_name = idl_identifier_cpp_name(operation, suffix=overload_index)
    if interface.is_namespace:
        receiver_class = receiver_class or interface.namespace_class
    else:
        receiver_class = receiver_class or (
            interface.constructor_class if emit_as_static else interface.prototype_class
        )
    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({receiver_class}::{callback_name})
{{
    WebIDL::log_trace(vm, "{receiver_class}::{callback_name}");
    [[maybe_unused]] auto& realm = *vm.current_realm();
"""
    )
    if return_type_is_promise:
        includes.add("LibWeb/WebIDL/Promise.h")
        out.write(
            """    auto steps = [&realm, &vm]() -> JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> {
        (void)realm;
"""
        )
    if not operation_invokes_as_static:
        out.write(
            f"""    [[maybe_unused]] {fully_qualified_name_for_interface(interface)}* idl_object = TRY(impl_from(vm));

"""
        )
    required_argument_count = overload_resolution.operation_length(operation)
    if required_argument_count == 1:
        out.write(
            f"""    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

"""
        )
    elif required_argument_count > 1:
        out.write(
            f"""    if (vm.argument_count() < {required_argument_count})
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountMany, "{operation.name}", "{required_argument_count}");

"""
        )

    write_operation_parameter_conversions(out, operation.parameters, includes, context)

    if operation_invokes_as_static:
        callee = fully_qualified_name_for_interface(interface)
        if interface.is_namespace:
            callee = fully_qualified_name_for_interface(interface).partition("::")[0]
        out.write(
            f"""    [[maybe_unused]] auto R = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {callee}::{idl_implementation_cpp_name(operation)}({callee_arguments}); }}));
"""
        )
        if return_type_is_promise:
            out.write(
                f"""        return R;
    }};

    auto maybe_R = steps();

    // And then, if an exception E was thrown:
    // 1. If op has a return type that is a promise type, then return ! Call(%Promise.reject%, %Promise%, «E»).
    // 2. Otherwise, end these steps and allow the exception to propagate.
    if (maybe_R.is_throw_completion())
        return WebIDL::create_rejected_promise(realm, maybe_R.error_value())->promise();

    auto R = maybe_R.release_value();
    return {to_javascript_value(operation.return_type, "R", includes, context)};
}}

"""
            )
        else:
            out.write(
                f"""    return {to_javascript_value(operation.return_type, "R", includes, context)};
}}

"""
            )
        return
    if "CEReactions" in operation.extended_attributes:
        if return_type_is_promise:
            raise RuntimeError(
                f"Unsupported promise-returning [CEReactions] operation '{operation.name}' on '{interface.name}'"
            )
        ce_reactions_steps = wrap_with_ce_reactions(includes, "original_steps()")
        out.write(
            f"""    auto original_steps = [&] {{
        return throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }});
    }};

    [[maybe_unused]] auto R = TRY({ce_reactions_steps});
    return {to_javascript_value(operation.return_type, "R", includes, context)};
}}

"""
        )
        return
    if return_type_is_promise:
        out.write(
            f"""    [[maybe_unused]] auto R = TRY(throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }}));
        return R;
    }};

    auto maybe_R = steps();

    // And then, if an exception E was thrown:
    // 1. If op has a return type that is a promise type, then return ! Call(%Promise.reject%, %Promise%, «E»).
    // 2. Otherwise, end these steps and allow the exception to propagate.
    if (maybe_R.is_throw_completion())
        return WebIDL::create_rejected_promise(realm, maybe_R.error_value())->promise();

    return {to_javascript_value(operation.return_type, "maybe_R.release_value()", includes, context)};
}}

"""
        )
        return
    out.write(
        f"""    [[maybe_unused]] auto R = TRY(throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }}));
    return {to_javascript_value(operation.return_type, "R", includes, context)};
}}

"""
    )


# FIXME: This belongs is an attribute getter helper somewhere.
def default_to_json_getter_steps(
    attribute: Attribute,
    value_name: str,
) -> str:
    is_reflected = "Reflect" in attribute.extended_attributes
    if is_reflected and attribute.type.name == "boolean":
        return f'auto {value_name} = idl_object->has_attribute("{reflected_attribute_name(attribute)}"_fly_string);'
    if is_reflected:
        return (
            f'auto {value_name} = idl_object->get_attribute_value("{reflected_attribute_name(attribute)}"_fly_string);'
        )
    return f"auto {value_name} = TRY(throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(attribute)}(); }}));"


# https://webidl.spec.whatwg.org/#collect-attribute-values
def collect_attribute_values(
    interface: Interface,
    context: GenerationContext,
    includes: GeneratedIncludes,
    attribute_values: list[str],
) -> None:
    # 1. If a toJSON operation with a [Default] extended attribute is declared on I
    #    then for each exposed regular attribute attr that is an interface member of I, in order:
    if not any(operation_is_default_to_json(operation) for operation in interface.regular_operations):
        return

    for attribute in interface.regular_attributes:
        # 1. Let id be the identifier of attr.
        # 2. Let value be the result of running the getter steps of attr with object as this.
        # 3. If value is a JSON type, then set map[id] to value.
        if not idl_type_is_json_type(attribute.type, context):
            continue

        value_name = f"{idl_identifier_cpp_name(attribute)}_{len(attribute_values)}_value"
        key_name = f"{idl_identifier_cpp_name(attribute)}_{len(attribute_values)}_key"
        js_value_name = f"{value_name}_js"
        getter_steps = default_to_json_getter_steps(attribute, value_name)

        attribute_values.append(
            wrap_with_extended_attribute_exposure_checks(
                includes,
                attribute.extended_attributes,
                f"""    {getter_steps}

    // 1. Let k be key converted to a JavaScript value.
    auto {key_name} = "{attribute.name}"_utf16_fly_string;

    // 2. Let v be value converted to a JavaScript value.
    auto {js_value_name} = {to_javascript_value(attribute.type, value_name, includes, context)};

    // 3. Perform ! CreateDataPropertyOrThrow(result, k, v).
    MUST(result->create_data_property({key_name}, {js_value_name}));
""",
            )
        )


# https://webidl.spec.whatwg.org/#collect-attribute-values-of-an-inheritance-stack
def collect_attribute_values_of_an_inheritance_stack(
    stack: list[Interface],
    context: GenerationContext,
    includes: GeneratedIncludes,
    attribute_values: list[str],
) -> None:
    # 1. Let I be the result of popping from stack.
    interface = stack.pop()

    # 2. Invoke collect attribute values given object, I, and map.
    collect_attribute_values(interface, context, includes, attribute_values)

    # 3. If stack is not empty, then invoke collect attribute values of an inheritance stack given object, stack, and map.
    if stack:
        collect_attribute_values_of_an_inheritance_stack(stack, context, includes, attribute_values)


# https://webidl.spec.whatwg.org/#js-default-tojson
def write_default_to_json_operation(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    operation: Operation,
) -> None:
    includes.add("LibJS/Runtime/Object.h")
    includes.add("LibWeb/WebIDL/Tracing.h")

    # 1. Let map be a new ordered map.

    # 2. Let stack be the result of creating an inheritance stack for interface I.
    stack = context.inheritance_stack(interface)

    # 3. Invoke collect attribute values of an inheritance stack given this, stack, and map.
    attribute_values: list[str] = []
    collect_attribute_values_of_an_inheritance_stack(stack, context, includes, attribute_values)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::{idl_identifier_cpp_name(operation)}");
    auto& realm = *vm.current_realm();

    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm));

    // 4. Let result be OrdinaryObjectCreate(%Object.prototype%).
    auto result = JS::Object::create(realm, realm.intrinsics().object_prototype());

    // 5. For each key → value of map:
{"".join(attribute_values)}
    // 6. Return result.
    return result;
}}

"""
    )


def write_argument_count_check(out: TextIO, function_name: str, argument_count: int) -> None:
    if argument_count == 0:
        return

    if argument_count == 1:
        out.write(
            f"""    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{function_name}");

"""
        )
        return

    out.write(
        f"""    if (vm.argument_count() < {argument_count})
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountMany, "{function_name}", "{argument_count}");

"""
    )
