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
from Generators.libweb_bindings.glue_headers import bindings_glue_header_for_interface
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.realms import member_passes_realm_to_implementation
from Generators.libweb_bindings.realms import member_realm_expr
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


def bindings_wrapper_interface_name_for_operation(
    context: GenerationContext,
    interface: Interface,
    operation: Operation,
) -> str:
    for mixin in context.mixins.values():
        for mixin_operation in mixin.regular_operations:
            if (
                mixin_operation.name == operation.name
                and "ImplementedInBindings" in mixin_operation.extended_attributes
            ):
                return mixin.name

    return interface.name


def bindings_wrapper_header_for_operation(
    context: GenerationContext, interface: Interface, operation: Operation
) -> str:
    wrapper_interface_name = bindings_wrapper_interface_name_for_operation(context, interface, operation)
    return bindings_glue_header_for_interface(interface, wrapper_interface_name)


def bindings_operation_arguments(arguments: str, has_this_object: bool, realm_argument: str = "realm") -> str:
    bindings_arguments = [realm_argument]
    if has_this_object:
        bindings_arguments.append("*idl_object")
    if arguments:
        bindings_arguments.append(arguments)
    return ", ".join(bindings_arguments)


def bindings_operation_call(
    operation: Operation, arguments: str, has_this_object: bool, realm_argument: str = "realm"
) -> str:
    method_name = idl_implementation_cpp_name(operation)
    if has_this_object:
        bindings_arguments_without_realm = f"implementation, {arguments}" if arguments else "implementation"
        bindings_arguments_with_realm = (
            f"{realm_argument}, implementation, {arguments}" if arguments else f"{realm_argument}, implementation"
        )
        no_realm_call = f"Web::Bindings::{method_name}({bindings_arguments_without_realm})"
        realm_call = f"Web::Bindings::{method_name}({bindings_arguments_with_realm})"
        if member_passes_realm_to_implementation(operation):
            no_realm_call, realm_call = realm_call, no_realm_call
        return f"""Web::Bindings::invoke_first_available(*idl_object,
        [&](auto& implementation) -> decltype({no_realm_call}) {{ return {no_realm_call}; }},
        [&](auto& implementation) -> decltype({realm_call}) {{ return {realm_call}; }})"""

    bindings_arguments = bindings_operation_arguments(arguments, has_this_object, realm_argument)
    return f"Web::Bindings::{method_name}({bindings_arguments})"


def implementation_operation_call(
    interface: Interface,
    operation: Operation,
    arguments: str,
    realm_argument: str = "realm",
    extra_argument: Optional[str] = None,
) -> str:
    callee_arguments_with_realm = f"{realm_argument}, {arguments}" if arguments else realm_argument
    callee_arguments_with_vm = f"vm, {arguments}" if arguments else "vm"
    if extra_argument is not None:
        arguments = f"{arguments}, {extra_argument}" if arguments else extra_argument
        callee_arguments_with_realm = f"{callee_arguments_with_realm}, {extra_argument}"
        callee_arguments_with_vm = f"{callee_arguments_with_vm}, {extra_argument}"
    method_name = idl_implementation_cpp_name(operation)
    no_realm_call = f"implementation.{method_name}({arguments})"
    relevant_global_object_call = (
        f"implementation.{method_name}({realm_argument}.global_object(), {arguments})"
        if arguments
        else f"implementation.{method_name}({realm_argument}.global_object())"
    )
    realm_call = f"implementation.{method_name}({callee_arguments_with_realm})"
    vm_call = f"implementation.{method_name}({callee_arguments_with_vm})"
    if member_passes_realm_to_implementation(operation):
        no_realm_call, realm_call = realm_call, no_realm_call
    return f"""Web::Bindings::invoke_first_available(*idl_object,
        [&](auto& implementation) -> decltype({no_realm_call}) {{ return {no_realm_call}; }},
        [&](auto& implementation) -> decltype({relevant_global_object_call}) {{ return {relevant_global_object_call}; }},
        [&](auto& implementation) -> decltype({realm_call}) {{ return {realm_call}; }},
        [&](auto& implementation) -> decltype({vm_call}) {{ return {vm_call}; }})"""


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
                emit_as_static=True,
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
    stringifier_call = f"""[&]<typename Implementation = {fully_qualified_name_for_interface(interface)}> {{
        auto& implementation = static_cast<Implementation&>(*idl_object);
        return Web::Bindings::invoke_first_available(implementation,
            [&](auto& implementation) -> decltype(implementation.{stringifier_cpp_name}()) {{ return implementation.{stringifier_cpp_name}(); }},
            [&](auto& implementation) -> decltype(implementation.{stringifier_cpp_name}(realm)) {{ return implementation.{stringifier_cpp_name}(realm); }});
    }}()"""
    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({receiver_class}::to_string)
{{
    auto& realm = *vm.current_realm();
    auto this_value = vm.this_value();
    auto* idl_object = TRY(impl_from(vm, this_value));
    auto& this_object_realm = this_value_realm(realm, this_value);
    auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, this_object_realm, [&] {{ return {stringifier_call}; }}));
    return {to_javascript_value(stringifier_type, "R", includes, context, "this_object_realm")};
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

    return_type_is_promise = operation.return_type_is_promise()
    should_wrap_promise_rejections = return_type_is_promise and overload_index is None
    implemented_in_bindings = "ImplementedInBindings" in operation.extended_attributes
    if implemented_in_bindings:
        includes.add(bindings_wrapper_header_for_operation(context, interface, operation))

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
    [[maybe_unused]] auto& realm = *vm.current_realm();
"""
    )
    if should_wrap_promise_rejections:
        includes.add("LibWeb/WebIDL/Promise.h")
        if operation_invokes_as_static or interface.is_namespace or "NeedsCallerRealm" in operation.extended_attributes:
            promise_realm_setup = "[[maybe_unused]] auto& promise_realm = realm;"
        else:
            promise_realm_setup = """auto promise_realm_this_value = vm.this_value();
    if (promise_realm_this_value.is_nullish())
        promise_realm_this_value = &realm.global_object();
    [[maybe_unused]] auto& promise_realm = this_value_realm(realm, promise_realm_this_value);"""
        out.write(
            f"""    {promise_realm_setup}

    auto steps = [&realm, &vm]() -> JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> {{
        (void)realm;
"""
        )
    if not operation_invokes_as_static:
        out.write(
            f"""    auto this_value = vm.this_value();
    if (this_value.is_nullish())
        this_value = &vm.current_realm()->global_object();
    [[maybe_unused]] {fully_qualified_name_for_interface(interface)}* idl_object = TRY(impl_from(vm, this_value));
    [[maybe_unused]] auto& this_object_realm = this_value_realm(realm, this_value);

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

    realm_argument = member_realm_expr(
        operation,
        interface=interface,
        is_static=operation_invokes_as_static,
    )
    if operation.parameters:
        out.write(
            f"""    JS::VM::TypeErrorRealmScope conversion_type_error_realm {{ vm, {realm_argument} }};

"""
        )
    write_operation_parameter_conversions(out, operation.parameters, includes, context, realm_argument)
    if operation.parameters:
        out.write(
            """    conversion_type_error_realm.restore();

"""
        )

    if operation_invokes_as_static:
        callee = fully_qualified_name_for_interface(interface)
        if interface.is_namespace:
            callee = fully_qualified_name_for_interface(interface).partition("::")[0]
        if implemented_in_bindings:
            out.write(
                f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {bindings_operation_call(operation, arguments, has_this_object=False)}; }}));
"""
            )
        elif "NeedsCallerRealm" in operation.extended_attributes or interface.is_namespace:
            first_argument = "realm" if "NeedsCallerRealm" in operation.extended_attributes else "vm"
            callee_arguments = f"{first_argument}, {arguments}" if arguments else first_argument
            if "DoesNotNeedVM" in operation.extended_attributes:
                callee_arguments = arguments
            out.write(
                f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {callee}::{idl_implementation_cpp_name(operation)}({callee_arguments}); }}));
"""
            )
        else:
            callee_arguments_with_vm = f"vm, {arguments}" if arguments else "vm"
            out.write(
                f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&]<typename Implementation = {callee}> {{
        return Web::Bindings::invoke_first_available_static<Implementation>(
            [&]<typename StaticImplementation>() -> decltype(StaticImplementation::{idl_implementation_cpp_name(operation)}({arguments})) {{ return StaticImplementation::{idl_implementation_cpp_name(operation)}({arguments}); }},
            [&]<typename StaticImplementation>() -> decltype(StaticImplementation::{idl_implementation_cpp_name(operation)}({callee_arguments_with_vm})) {{ return StaticImplementation::{idl_implementation_cpp_name(operation)}({callee_arguments_with_vm}); }});
    }}));
"""
            )
        if return_type_is_promise and should_wrap_promise_rejections:
            out.write(
                f"""        return R;
    }};

    auto maybe_R = steps();

    // And then, if an exception E was thrown:
    // 1. If op has a return type that is a promise type, then return ! Call(%Promise.reject%, %Promise%, «E»).
    // 2. Otherwise, end these steps and allow the exception to propagate.
    if (maybe_R.is_throw_completion())
        return WebIDL::create_rejected_promise(promise_realm, maybe_R.error_value())->promise();

    auto R = maybe_R.release_value();
    return {to_javascript_value(operation.return_type, "R", includes, context, "promise_realm")};
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
        operation_call = implementation_operation_call(interface, operation, arguments, realm_argument)
        if implemented_in_bindings:
            operation_call = bindings_operation_call(
                operation, arguments, has_this_object=True, realm_argument=realm_argument
            )
        ce_reactions_steps = wrap_with_ce_reactions(includes, "original_steps()", realm_argument)
        out.write(
            f"""    auto original_steps = [&] {{
        return WebIDL::throw_dom_exception_if_needed(vm, {realm_argument}, [&] {{ return {operation_call}; }});
    }};

    [[maybe_unused]] auto R = TRY({ce_reactions_steps});
    return {to_javascript_value(operation.return_type, "R", includes, context, realm_argument)};
}}

"""
        )
        return
    operation_call = implementation_operation_call(interface, operation, arguments, realm_argument)
    if implemented_in_bindings:
        operation_call = bindings_operation_call(
            operation, arguments, has_this_object=True, realm_argument=realm_argument
        )
    if return_type_is_promise:
        if "CreatesPromise" in operation.extended_attributes and should_wrap_promise_rejections:
            operation_call = implementation_operation_call(
                interface, operation, arguments, realm_argument, extra_argument="promise"
            )
            out.write(
                f"""    auto promise = WebIDL::create_promise({realm_argument});
    TRY(WebIDL::throw_dom_exception_if_needed(vm, {realm_argument}, [&] {{ return {operation_call}; }}));
        return promise;
    }};

    auto maybe_R = steps();

    // And then, if an exception E was thrown:
    // 1. If op has a return type that is a promise type, then return ! Call(%Promise.reject%, %Promise%, «E»).
    // 2. Otherwise, end these steps and allow the exception to propagate.
    if (maybe_R.is_throw_completion())
        return WebIDL::create_rejected_promise(promise_realm, maybe_R.error_value())->promise();

    return maybe_R.release_value()->promise();
}}

"""
            )
            return
        if should_wrap_promise_rejections:
            out.write(
                f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {realm_argument}, [&] {{ return {operation_call}; }}));
        return R;
    }};

    auto maybe_R = steps();

    // And then, if an exception E was thrown:
    // 1. If op has a return type that is a promise type, then return ! Call(%Promise.reject%, %Promise%, «E»).
    // 2. Otherwise, end these steps and allow the exception to propagate.
    if (maybe_R.is_throw_completion())
        return WebIDL::create_rejected_promise(promise_realm, maybe_R.error_value())->promise();

    return {to_javascript_value(operation.return_type, "maybe_R.release_value()", includes, context, "promise_realm")};
}}

"""
            )
            return
        if "CreatesPromise" in operation.extended_attributes:
            operation_call = implementation_operation_call(
                interface, operation, arguments, realm_argument, extra_argument="promise"
            )
            out.write(
                f"""    auto promise = WebIDL::create_promise({realm_argument});
    TRY(WebIDL::throw_dom_exception_if_needed(vm, {realm_argument}, [&] {{ return {operation_call}; }}));
    return promise->promise();
}}

"""
            )
        else:
            out.write(
                f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {realm_argument}, [&] {{ return {operation_call}; }}));
    return {to_javascript_value(operation.return_type, "R", includes, context, realm_argument)};
}}

"""
            )
        return
    out.write(
        f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {realm_argument}, [&] {{ return {operation_call}; }}));
    return {to_javascript_value(operation.return_type, "R", includes, context, realm_argument)};
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
        return (
            f'auto {value_name} = idl_object->has_attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);'
        )
    return f"auto {value_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, this_object_realm, [&] {{ return idl_object->{idl_implementation_cpp_name(attribute)}(); }}));"


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
    auto {js_value_name} = {to_javascript_value(attribute.type, value_name, includes, context, "this_object_realm")};

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

    # 1. Let map be a new ordered map.

    # 2. Let stack be the result of creating an inheritance stack for interface I.
    stack = context.inheritance_stack(interface)

    # 3. Invoke collect attribute values of an inheritance stack given this, stack, and map.
    attribute_values: list[str] = []
    collect_attribute_values_of_an_inheritance_stack(stack, context, includes, attribute_values)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    auto& realm = *vm.current_realm();

    auto this_value = vm.this_value();
    if (this_value.is_nullish())
        this_value = &realm.global_object();
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm, this_value));
    [[maybe_unused]] auto& this_object_realm = this_value_realm(realm, this_value);

    // 4. Let result be OrdinaryObjectCreate(%Object.prototype%).
    auto result = JS::Object::create(this_object_realm, this_object_realm.intrinsics().object_prototype());

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
