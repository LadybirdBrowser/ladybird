# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings.arguments import write_operation_parameter_conversions
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import add_header_includes_for_idl_type
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import libweb_include_path
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.overload_resolution import operation_callback_names
from Generators.libweb_bindings.overload_resolution import parameter_list_length
from Generators.libweb_bindings.to_idl_value import type_check_idl_value
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import Interface


def iterator_implementation_header_for_interface(interface: Interface) -> str:
    return libweb_include_path(interface.path.with_name(f"{interface.implemented_name}Iterator.h"))


def async_iterator_implementation_header_for_interface(interface: Interface) -> str:
    return libweb_include_path(interface.path.with_name(f"{interface.implemented_name}AsyncIterator.h"))


def write_iterator_prototype_declaration(out: TextIO, interface: Interface) -> None:
    if interface.iterable is None or interface.iterable.key_type is None:
        return

    out.write(
        f"""class {interface.name}IteratorPrototype : public JS::Object {{
    JS_OBJECT({interface.name}IteratorPrototype, JS::Object);
    GC_DECLARE_ALLOCATOR({interface.name}IteratorPrototype);

public:
    explicit {interface.name}IteratorPrototype(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~{interface.name}IteratorPrototype() override;

private:
    JS_DECLARE_NATIVE_FUNCTION(next);
}};

"""
    )


def write_async_iterator_prototype_declaration(out: TextIO, interface: Interface) -> None:
    if interface.async_iterable is None:
        return

    out.write(
        f"""class {interface.name}AsyncIteratorPrototype : public JS::Object {{
    JS_OBJECT({interface.name}AsyncIteratorPrototype, JS::Object);
    GC_DECLARE_ALLOCATOR({interface.name}AsyncIteratorPrototype);

public:
    explicit {interface.name}AsyncIteratorPrototype(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~{interface.name}AsyncIteratorPrototype() override;

private:
    JS_DECLARE_NATIVE_FUNCTION(next);
    {"JS_DECLARE_NATIVE_FUNCTION(return_);" if "DefinesAsyncIteratorReturn" in interface.extended_attributes else ""}
}};

"""
    )


def define_the_pair_iterable_declaration(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.iterable is None or interface.iterable.key_type is None:
        return

    includes.add("LibJS/Runtime/ArrayPrototype.h")
    out.write(
        """    object.define_native_function(realm, vm.names.entries, entries, 0, default_attributes);
    object.define_native_function(realm, vm.names.forEach, for_each, 1, default_attributes);
    object.define_native_function(realm, vm.names.keys, keys, 0, default_attributes);
    object.define_native_function(realm, vm.names.values, values, 0, default_attributes);

    object.define_direct_property(vm.well_known_symbol_iterator(), object.get_without_side_effects(vm.names.entries), JS::Attribute::Configurable | JS::Attribute::Writable);

"""
    )


def define_the_async_iterable_declaration(
    out: TextIO,
    interface: Interface,
) -> None:
    if interface.async_iterable is None:
        return

    out.write(
        f"""    object.define_native_function(realm, vm.names.values, values, {parameter_list_length(interface.async_iterable.parameters)}, default_attributes);
    object.define_direct_property(vm.well_known_symbol_async_iterator(), object.get_without_side_effects(vm.names.values), JS::Attribute::Configurable | JS::Attribute::Writable);

"""
    )


def define_the_maplike_declaration(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.maplike is None:
        return

    includes.add("LibJS/Runtime/Map.h")
    out.write(
        """    object.define_native_accessor(realm, vm.names.size, get_size, nullptr, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    object.define_native_function(realm, vm.names.entries, entries, 0, default_attributes);
    object.define_direct_property(vm.well_known_symbol_iterator(), object.get_without_side_effects(vm.names.entries), JS::Attribute::Configurable | JS::Attribute::Writable);
    object.define_native_function(realm, vm.names.keys, keys, 0, default_attributes);
    object.define_native_function(realm, vm.names.values, values, 0, default_attributes);
    object.define_native_function(realm, vm.names.forEach, for_each, 1, default_attributes);
    object.define_native_function(realm, vm.names.get, get, 1, default_attributes);
    object.define_native_function(realm, vm.names.has, has, 1, default_attributes);
"""
    )
    if not interface.maplike.readonly:
        out.write(
            """    object.define_native_function(realm, vm.names.delete_, delete_, 1, default_attributes);
    object.define_native_function(realm, vm.names.clear, clear, 0, default_attributes);
"""
        )
    out.write("\n")


def define_the_setlike_declaration(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.setlike is None:
        return

    includes.add("LibJS/Runtime/Set.h")
    operation_callbacks = operation_callback_names(interface)
    out.write(
        """    object.define_native_accessor(realm, vm.names.size, get_size, nullptr, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    object.define_native_function(realm, vm.names.entries, entries, 0, default_attributes);
    object.define_native_function(realm, vm.names.keys, values, 0, default_attributes);
    object.define_native_function(realm, vm.names.values, values, 0, default_attributes);
    object.define_direct_property(vm.well_known_symbol_iterator(), object.get_without_side_effects(vm.names.values), JS::Attribute::Configurable | JS::Attribute::Writable);
    object.define_native_function(realm, vm.names.forEach, for_each, 1, default_attributes);
    object.define_native_function(realm, vm.names.has, has, 1, default_attributes);
"""
    )
    if not interface.setlike.readonly:
        if "add" not in operation_callbacks:
            out.write("    object.define_native_function(realm, vm.names.add, add, 1, default_attributes);\n")
        if "delete_" not in operation_callbacks:
            out.write("    object.define_native_function(realm, vm.names.delete_, delete_, 1, default_attributes);\n")
        if "clear" not in operation_callbacks:
            out.write("    object.define_native_function(realm, vm.names.clear, clear, 0, default_attributes);\n")
    out.write("\n")


# https://webidl.spec.whatwg.org/#js-iterable
def write_pair_iterable_declaration_functions(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.iterable is None or interface.iterable.key_type is None:
        return

    includes.add("LibJS/Runtime/AbstractOperations.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")
    includes.add(iterator_implementation_header_for_interface(interface))
    add_header_includes_for_idl_type(interface.iterable.key_type, includes, context)
    add_header_includes_for_idl_type(interface.iterable.value_type, includes, context)

    out.write(f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::entries)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::entries");

    // 1. Let jsValue be ? ToObject(this value).
    // 2. If jsValue is a platform object, then perform a security check, passing jsValue, "%Symbol.iterator%", and "method".
    // 3. If jsValue does not implement definition, then throw a TypeError.
    auto* this_impl = TRY(impl_from(vm));

    // 4. Return a newly created default iterator object for definition, with jsValue as its target, "key+value" as its kind, and index set to 0.
    return TRY(throw_dom_exception_if_needed(vm, [&] {{ return {fully_qualified_name_for_interface(interface)}Iterator::create(*this_impl, JS::Object::PropertyKind::KeyAndValue); }}));
}}

JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::keys)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::keys");

    // 1. Let jsValue be ? ToObject(this value).
    // 2. If jsValue is a platform object, then perform a security check, passing jsValue, "keys", and "method".
    // 3. If jsValue does not implement definition, then throw a TypeError.
    auto* this_impl = TRY(impl_from(vm));

    // 4. Return a newly created default iterator object for definition, with jsValue as its target, "key" as its kind, and index set to 0.
    return TRY(throw_dom_exception_if_needed(vm, [&] {{ return {fully_qualified_name_for_interface(interface)}Iterator::create(*this_impl, JS::Object::PropertyKind::Key); }}));
}}

JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::values)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::values");

    // 1. Let jsValue be ? ToObject(this value).
    // 2. If jsValue is a platform object, then perform a security check, passing jsValue, "values", and "method".
    // 3. If jsValue does not implement definition, then throw a TypeError.
    auto* this_impl = TRY(impl_from(vm));

    // 4. Return a newly created default iterator object for definition, with jsValue as its target, "value" as its kind, and index set to 0.
    return TRY(throw_dom_exception_if_needed(vm, [&] {{ return {fully_qualified_name_for_interface(interface)}Iterator::create(*this_impl, JS::Object::PropertyKind::Value); }}));
}}

JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::for_each)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::for_each");
    auto* this_impl = TRY(impl_from(vm));

    auto callback = vm.argument(0);
    if (!callback.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, callback);

    auto this_value = vm.this_value();
    TRY(this_impl->for_each([&](auto key, auto value) -> JS::ThrowCompletionOr<void> {{
        JS::Value wrapped_key = {to_javascript_value(interface.iterable.key_type, "key", includes, context)};
        JS::Value wrapped_value = {to_javascript_value(interface.iterable.value_type, "value", includes, context)};
        TRY(JS::call(vm, callback.as_function(), vm.argument(1), wrapped_value, wrapped_key, this_value));
        return {{}};
    }}));

    return JS::js_undefined();
}}

""")


def write_iterator_prototype_implementation(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.iterable is None or interface.iterable.key_type is None:
        return

    includes.add("AK/TypeCasts.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/IteratorPrototype.h")
    includes.add("LibJS/Runtime/PrimitiveString.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")
    includes.add(iterator_implementation_header_for_interface(interface))

    iterator_interface_name = f"{interface.name}Iterator"
    out.write(f"""GC_DEFINE_ALLOCATOR({interface.name}IteratorPrototype);

{interface.name}IteratorPrototype::{interface.name}IteratorPrototype(JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().iterator_prototype())
{{
}}

{interface.name}IteratorPrototype::~{interface.name}IteratorPrototype()
{{
}}

void {interface.name}IteratorPrototype::initialize(JS::Realm& realm)
{{
    auto& vm = this->vm();
    Base::initialize(realm);
    define_native_function(realm, vm.names.next, next, 0, JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.name} Iterator"_utf16), JS::Attribute::Configurable);
}}

static JS::ThrowCompletionOr<{fully_qualified_name_for_interface(interface)}Iterator*> {make_name_acceptable_cpp(title_case_to_snake_case(iterator_interface_name))}_impl_from(JS::VM& vm)
{{
    auto this_object = TRY(vm.this_value().to_object(vm));
    if (!is<{fully_qualified_name_for_interface(interface)}Iterator>(*this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{iterator_interface_name}");
    return static_cast<{fully_qualified_name_for_interface(interface)}Iterator*>(this_object.ptr());
}}

JS_DEFINE_NATIVE_FUNCTION({interface.name}IteratorPrototype::next)
{{
    WebIDL::log_trace(vm, "{interface.name}IteratorPrototype::next");
    auto* impl = TRY({make_name_acceptable_cpp(title_case_to_snake_case(iterator_interface_name))}_impl_from(vm));
    return TRY(throw_dom_exception_if_needed(vm, [&] {{ return impl->next(); }}));
}}

""")


def write_async_iterable_declaration_functions(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.async_iterable is None:
        return
    if interface.async_iterable.key_type is not None:
        raise RuntimeError(f"Unsupported pair async iterable declaration on '{interface.name}'")

    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")
    includes.add(async_iterator_implementation_header_for_interface(interface))

    add_header_includes_for_idl_type(interface.async_iterable.value_type, includes, context)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::values)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::values");
    auto& realm = *vm.current_realm();
    auto* impl = TRY(impl_from(vm));

"""
    )
    write_operation_parameter_conversions(out, interface.async_iterable.parameters, includes, context)

    arguments = ", ".join(idl_identifier_cpp_name(parameter) for parameter in interface.async_iterable.parameters)
    if arguments:
        arguments = f", {arguments}"

    out.write(
        f"""    return TRY(throw_dom_exception_if_needed(vm, [&] {{ return {fully_qualified_name_for_interface(interface)}AsyncIterator::create(realm, JS::Object::PropertyKind::Value, *impl{arguments}); }}));
}}

"""
    )


def write_async_iterator_prototype_implementation(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.async_iterable is None:
        return

    includes.add("AK/StringView.h")
    includes.add("LibJS/Runtime/AsyncIteratorPrototype.h")
    includes.add("LibJS/Runtime/PrimitiveString.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")
    includes.add("LibWeb/WebIDL/AsyncIterator.h")
    includes.add(async_iterator_implementation_header_for_interface(interface))

    out.write(
        f"""GC_DEFINE_ALLOCATOR({interface.name}AsyncIteratorPrototype);

{interface.name}AsyncIteratorPrototype::{interface.name}AsyncIteratorPrototype(JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().async_iterator_prototype())
{{
}}

{interface.name}AsyncIteratorPrototype::~{interface.name}AsyncIteratorPrototype()
{{
}}

void {interface.name}AsyncIteratorPrototype::initialize(JS::Realm& realm)
{{
    auto& vm = this->vm();
    Base::initialize(realm);
    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.name} AsyncIterator"_utf16), JS::Attribute::Configurable);

    define_native_function(realm, vm.names.next, next, 0, JS::default_attributes);
    {"define_native_function(realm, vm.names.return_, return_, 1, JS::default_attributes);" if "DefinesAsyncIteratorReturn" in interface.extended_attributes else ""}
}}

JS_DEFINE_NATIVE_FUNCTION({interface.name}AsyncIteratorPrototype::next)
{{
    WebIDL::log_trace(vm, "{interface.name}AsyncIteratorPrototype::next");
    auto& realm = *vm.current_realm();

    return TRY(throw_dom_exception_if_needed(vm, [&] {{
        return WebIDL::AsyncIterator::next<{fully_qualified_name_for_interface(interface)}AsyncIterator>(realm, "{interface.name}AsyncIterator"sv);
    }}));
}}
"""
    )

    if "DefinesAsyncIteratorReturn" in interface.extended_attributes:
        out.write(f"""
JS_DEFINE_NATIVE_FUNCTION({interface.name}AsyncIteratorPrototype::return_)
{{
    WebIDL::log_trace(vm, "{interface.name}AsyncIteratorPrototype::return");
    auto& realm = *vm.current_realm();

    auto value = vm.argument(0);

    return TRY(throw_dom_exception_if_needed(vm, [&] {{
        return WebIDL::AsyncIterator::return_<{fully_qualified_name_for_interface(interface)}AsyncIterator>(realm, "{interface.name}AsyncIterator"sv, value);
    }}));
}}
""")


def write_maplike_declaration_functions(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.maplike is None:
        return

    includes.add("LibJS/Runtime/AbstractOperations.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Map.h")
    includes.add("LibJS/Runtime/MapIterator.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")

    out.write(f"""// https://webidl.spec.whatwg.org/#js-map-size
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::get_size)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::get_size");

    // 1. Let O be the this value, implementation-checked against A with identifier "size" and type "getter".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Return map’s size, converted to a JavaScript value.
    return map->map_size();
}}

// https://webidl.spec.whatwg.org/#js-map-entries
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::entries)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::entries");
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value, implementation-checked against A with identifier "entries" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Return the result of creating a map iterator from map with kind "key+value".
    return JS::MapIterator::create(realm, *map, PropertyKind::KeyAndValue);
}}

// https://webidl.spec.whatwg.org/#js-map-keys
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::keys)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::keys");
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value, implementation-checked against A with identifier "keys" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Return the result of creating a map iterator from map with kind "key".
    return JS::MapIterator::create(realm, *map, PropertyKind::Key);
}}

// https://webidl.spec.whatwg.org/#js-map-values
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::values)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::values");
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value, implementation-checked against A with identifier "values" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Return the result of creating a map iterator from map with kind "value".
    return JS::MapIterator::create(realm, *map, PropertyKind::Value);
}}

// https://webidl.spec.whatwg.org/#js-map-forEach
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::for_each)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::for_each");

    // 1. Let O be the this value, implementation-checked against A with identifier "forEach" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Let callbackFn be the first argument passed to the function, or undefined if not supplied.
    auto callback = vm.argument(0);

    // 4. If IsCallable(callbackFn) is false, throw a TypeError.
    if (!callback.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, callback);

    // 5. Let thisArg be the second argument passed to the function, or undefined if not supplied.
    auto this_arg = vm.argument(1);

    // 6. For each key → value of map:
    for (auto& [key, value] : *map) {{
        // 1. Let jsKey and jsValue be key and value converted to a JavaScript value.
        // 2. Perform ? Call(callbackFn, thisArg, « jsValue, jsKey, O »).
        TRY(JS::call(vm, callback.as_function(), this_arg, value, key, this_impl));
    }}

    // 7. Return undefined.
    return JS::js_undefined();
}}

// https://webidl.spec.whatwg.org/#js-map-get
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::get)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::get");

    // 1. Let O be the this value, implementation-checked against A with identifier "get" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Let keyType be the key type specified in the maplike declaration.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    auto key = vm.argument(0);
    {type_check_idl_value(interface.maplike.key_type, "key", includes, context, interface.name)}

    // FIXME: 6. If key is -0, set key to +0.

    // 7. If map[key] exists, then return map[key], converted to a JavaScript value.
    auto result = map->map_get(key);
    return result.value_or(JS::js_undefined());
}}

// https://webidl.spec.whatwg.org/#js-map-has
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::has)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::has");

    // 1. Let O be the this value, implementation-checked against A with identifier "has" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Let keyType be the key type specified in the maplike declaration.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    auto key = vm.argument(0);
    {type_check_idl_value(interface.maplike.key_type, "key", includes, context, interface.name)}

    // FIXME: 6. If key is -0, set key to +0.

    // 7. If map[key] exists, then return true; otherwise return false.
    return map->map_has(key);
}}
""")

    # If A does not declare a member with identifier "set", and A was declared with a read–write maplike declaration,
    # then there must exist a set data property on A’s interface prototype object with the following characteristics:
    if "set" not in operation_callback_names(interface) and not interface.maplike.readonly:
        out.write(f"""// https://webidl.spec.whatwg.org/#js-map-set
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::set)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::set");

    // 1. Let O be the this value, implementation-checked against A with identifier "set" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Let keyType be the key type specified in the maplike declaration, and valueType be the value type.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    auto key = vm.argument(0);
    {type_check_idl_value(interface.maplike.key_type, "key", includes, context, interface.name)}

    // FIXME: 6. If key is -0, set key to +0.

    // 7. Let valueArg be the second argument passed to this function, or undefined if not supplied.
    // 8. Let value be valueArg converted to an IDL value of type valueType.
    auto value = vm.argument(1);
    {type_check_idl_value(interface.maplike.value_type, "value", includes, context, interface.name)}

    // 9. Set map[key] to value.
    map->map_set(key, value);
    this_impl->on_map_modified_from_js({{}});

    // 10. Return O.
    return this_impl;
}}

""")

    # If A does not declare a member with identifier "delete", and A was declared with a read–write maplike declaration,
    # then there must exist a delete data property on A’s interface prototype object with the following characteristics:
    if "delete_" not in operation_callback_names(interface) and not interface.maplike.readonly:
        out.write(f"""// https://webidl.spec.whatwg.org/#js-map-delete
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::delete_)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::delete_");

    // 1. Let O be the this value, implementation-checked against A with identifier "delete" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Let keyType be the key type specified in the maplike declaration.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    auto key = vm.argument(0);
    {type_check_idl_value(interface.maplike.key_type, "key", includes, context, interface.name)}

    // FIXME: 6. If key is -0, set key to +0.

    // 7. Let retVal be true if map[key] exists, or else false.
    // 8. Remove map[key].
    auto result = map->map_remove(key);
    this_impl->on_map_modified_from_js({{}});

    // 9. Return retVal.
    return result;
}}

""")

    # If A does not declare a member with identifier "clear", and A was declared with a read–write maplike declaration,
    # then there must exist a clear data property on A’s interface prototype object with the following characteristics:
    if "clear" not in operation_callback_names(interface) and not interface.maplike.readonly:
        out.write(f"""// https://webidl.spec.whatwg.org/#js-map-delete
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::clear)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::clear");

    // 1. Let O be the this value, implementation-checked against A with identifier "delete" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Map> map = this_impl->map_entries();

    // 3. Clear map.
    // NOTE: The map is preserved because there may be existing iterators, currently suspended, iterating over it.
    map->map_clear();
    this_impl->on_map_modified_from_js({{}});

    // 4. Return undefined.
    return JS::js_undefined();
}}

""")


def write_setlike_declaration_functions(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.setlike is None:
        return

    includes.add("LibJS/Runtime/AbstractOperations.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Set.h")
    includes.add("LibJS/Runtime/SetIterator.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")

    out.write(f"""// https://webidl.spec.whatwg.org/#js-set-size
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::get_size)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::size");

    // 1. Let O be the this value, implementation-checked against A with identifier "size" and type "getter".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Return set’s size, converted to a JavaScript value.
    return set->set_size();
}}

// https://webidl.spec.whatwg.org/#js-set-entries
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::entries)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::values");
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value, implementation-checked against A with identifier "entries" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Return the result of creating a set iterator from set with kind "key+value".
    return JS::SetIterator::create(realm, *set, PropertyKind::KeyAndValue);
}}

// https://webidl.spec.whatwg.org/#js-set-values
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::values)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::values");
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value, implementation-checked against A with identifier "values" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Return the result of creating a set iterator from set with kind "value".
    return JS::SetIterator::create(realm, *set, PropertyKind::Value);
}}

// https://webidl.spec.whatwg.org/#js-set-forEach
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::for_each)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::for_each");

    // 1. Let O be the this value, implementation-checked against A with identifier "forEach" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Let callbackFn be the first argument passed to the function, or undefined if not supplied.
    // 4. If IsCallable(callbackFn) is false, throw a TypeError.
    auto callback = vm.argument(0);
    if (!callback.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, callback);

    // 5. Let thisArg be the second argument passed to the function, or undefined if not supplied.
    auto this_arg = vm.argument(1);

    // 6. For each value of set:
    for (auto& entry : *set) {{
        // 1. Let jsValue be value converted to a JavaScript value.
        auto value = entry.key;

        // 2. Perform ? Call(callbackFn, thisArg, « jsValue, jsValue, O»).
        TRY(JS::call(vm, callback.as_function(), this_arg, value, value, this_impl));
    }}

    // 7. Return undefined.
    return JS::js_undefined();
}}

// https://webidl.spec.whatwg.org/#js-set-has
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::has)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::has");

    // 1. Let O be the this value, implementation-checked against A with identifier "has" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Let valueType be the value type specified in the setlike declaration.
    // 4. Let valueArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let value be valueArg converted to an IDL value of type valueType.
    // FIXME: 6. If value is -0, set value to +0.
    auto value = vm.argument(0);
    {type_check_idl_value(interface.setlike.value_type, "value", includes, context, interface.name)}

    // 7. If set contains value, then return true, otherwise return false.
    return set->set_has(value);
}}

""")

    # If A does not declare a member with identifier "add", and A was declared with a read–write setlike declaration,
    # then there must exist an add data property on A’s interface prototype object with the following characteristics:
    if "add" not in operation_callback_names(interface) and not interface.setlike.readonly:
        out.write(f"""// https://webidl.spec.whatwg.org/#js-set-add
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::add)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::add");

    // 1. Let O be the this value, implementation-checked against A with identifier "add" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Let valueType be the value type specified in the setlike declaration.
    // 4. Let valueArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let value be valueArg converted to an IDL value of type valueType.
    // FIXME: 6. If value is -0, set value to +0.
    auto value = vm.argument(0);
    {type_check_idl_value(interface.setlike.value_type, "value", includes, context, interface.name)}

    // 6. Append value to set.
    set->set_add(value);
    this_impl->on_set_modified_from_js({{}});

    return this_impl;
}}

""")

    # If A does not declare a member with identifier "delete", and A was declared with a read–write setlike declaration,
    # then there must exist a delete data property on A’s interface prototype object with the following characteristics:
    if "delete_" not in operation_callback_names(interface) and not interface.setlike.readonly:
        out.write(f"""// https://webidl.spec.whatwg.org/#js-set-delete
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::delete_)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::delete_");

    // 1. Let O be the this value, implementation-checked against A with identifier "delete" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be O’s set entries.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Let valueType be the value type specified in the setlike declaration.
    // 4. Let valueArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let value be valueArg converted to an IDL value of type valueType.
    // 6. FIXME: If value is -0, set value to +0.
    auto value = vm.argument(0);
    {type_check_idl_value(interface.setlike.value_type, "value", includes, context, interface.name)}

    // 7. Let retVal be true if set contains value, or else false.
    // 8. Remove value from set.
    auto result = set->set_remove(value);
    this_impl->on_set_modified_from_js({{}});

    // 9. Return retVal.
    return result;
}}

""")

    # If A does not declare a member with identifier "clear", and A was declared with a read–write setlike declaration,
    # then there must exist a clear data property on A’s interface prototype object with the following characteristics:
    if "clear" not in operation_callback_names(interface) and not interface.setlike.readonly:
        out.write(f"""// https://webidl.spec.whatwg.org/#js-set-clear
JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::clear)
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::clear");

    // 1. Let O be the this value, implementation-checked against A with identifier "clear" and type "method".
    auto* this_impl = TRY(impl_from(vm));

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    GC::Ref<JS::Set> set = this_impl->set_entries();

    // 3. Empty set.
    // NOTE: Note: The set is preserved because there may be existing iterators, currently suspended, iterating over it.
    set->set_clear();
    this_impl->on_set_modified_from_js({{}});

    // 4. Return undefined.
    return JS::js_undefined();
}}

""")
