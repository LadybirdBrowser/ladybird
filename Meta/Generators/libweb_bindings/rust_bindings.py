# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The bindings of the interfaces that their IDL marks [ImplementedIn=Rust]. Their attributes, operations, constructor
# and interface objects are Rust code in libweb_bindings_rust, which this module writes to Bindings/RustBindings.rs,
# and the implementation of each interface is a Rust type in that crate, whose methods the bindings call: one per
# constructor, attribute and operation, with the signatures the generated bindings document. The bindings reach the
# runtime through the embedding API of the libjs_rust crate. LibWeb's C++
# bindings still create the interface object and the interface prototype object of every interface and expose them on
# the globals; for an interface implemented in Rust they call into the Rust bindings to initialize them and to
# construct objects, and they define the class of its wrappers.
#
# A wrapper is a platform object whose host data holds a GC cell of the implementation. Its wrappable slot stays
# empty, so the C++ bindings see a platform object without a C++ implementation object.

import textwrap

from io import StringIO
from typing import Callable
from typing import Dict
from typing import List
from typing import TextIO
from typing import Tuple

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.operations import idl_type_is_json_type
from Generators.libweb_bindings.operations import operation_is_default_to_json
from Generators.libweb_bindings.rust_members import RustAttribute
from Generators.libweb_bindings.rust_members import RustConstructor
from Generators.libweb_bindings.rust_members import RustMembers
from Generators.libweb_bindings.rust_members import RustOperation
from Generators.libweb_bindings.rust_members import RustOperationSet
from Generators.libweb_bindings.rust_members import RustPairIterator
from Generators.libweb_bindings.rust_members import rust_members
from Generators.libweb_bindings.rust_names import rust_identifier
from Generators.libweb_bindings.rust_names import rust_local_name
from Generators.libweb_bindings.rust_overloads import write_constructor_overload_resolution
from Generators.libweb_bindings.rust_overloads import write_operation_overload_resolution
from Generators.libweb_bindings.rust_to_idl_value import rust_default_value
from Generators.libweb_bindings.rust_to_idl_value import rust_to_idl_value
from Generators.libweb_bindings.rust_to_idl_value import write_dictionary_from_js
from Generators.libweb_bindings.rust_to_idl_value import write_enumeration_from_js
from Generators.libweb_bindings.rust_to_idl_value import write_union_from_js
from Generators.libweb_bindings.rust_to_js_value import rust_to_js_value
from Generators.libweb_bindings.rust_to_js_value import write_dictionary_to_js
from Generators.libweb_bindings.rust_to_js_value import write_enumeration_to_js
from Generators.libweb_bindings.rust_to_js_value import write_union_to_js
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Generators.libweb_bindings.rust_types import cpp_interface_name_constant
from Generators.libweb_bindings.rust_types import holds_cells
from Generators.libweb_bindings.rust_types import inheriting_interfaces
from Generators.libweb_bindings.rust_types import is_implemented_in_rust
from Generators.libweb_bindings.rust_types import is_promise_type
from Generators.libweb_bindings.rust_types import rust_interface_path
from Generators.libweb_bindings.rust_types import rust_parameter_type
from Generators.libweb_bindings.rust_types import rust_result_type
from Generators.libweb_bindings.rust_types import rust_type
from Generators.libweb_bindings.rust_usage import DefinitionUsage
from Generators.libweb_bindings.wrappers import wrapper_name
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Module
from Utils.webidl_parser import OperationParameter

# The extended attributes of an interface that the Rust bindings handle.
SUPPORTED_INTERFACE_EXTENDED_ATTRIBUTES = {
    "Experimental",
    "Exposed",
    "ImplementedIn",
    # NB: The C++ bindings expose the interface object under the aliases on the Window, whatever the language of the
    #     interface.
    "LegacyWindowAlias",
    # NB: The C++ bindings only leave the interface object of such an interface off the global object.
    "LegacyNoInterfaceObject",
    "NavigatorCompatibilityMode",
    "SecureContext",
    # NB: The implementation implements structured_data::Serializable or structured_data::Transferable, whose steps
    #     StructuredSerialize of LibWeb's C++ runs through the entry points that write_rust_structured_data_entry_points()
    #     writes.
    "Serializable",
    "Transferable",
}


def rust_module_name(interface: Interface) -> str:
    return title_case_to_snake_case(interface.name)


def rust_entry_point_name(interface: Interface, entry_point: str) -> str:
    return f"web_rust_{rust_module_name(interface)}_{entry_point}"


def wrapper_host_class_symbol(interface: Interface) -> str:
    return f"web_bindings_{wrapper_name(interface)}_host_class"


def iterator_host_class_symbol(interface: Interface) -> str:
    return f"web_bindings_{rust_module_name(interface)}_iterator_host_class"


def interface_name_values(intrinsics: List[Interface]) -> Dict[str, int]:
    """The value of Web::Bindings::InterfaceName for each interface, which the Rust bindings pass the C++ bindings:
    the interfaces in the order of the intrinsics, from 1, as 0 is Unknown."""
    return {interface.name: index for index, interface in enumerate(intrinsics, start=1)}


def check_interface_is_supported(interface: Interface, module: Module, context: GenerationContext) -> None:
    """Raises an error that names what the Rust bindings do not handle yet about the interface as a whole. Its members
    are the business of rust_members(), and its types that of the functions that write the bindings."""

    def fail(message: str) -> None:
        raise UnsupportedByRustBindings(f"{interface.path}: {interface.name} is [ImplementedIn=Rust], but {message}")

    unsupported_attributes = set(interface.extended_attributes) - SUPPORTED_INTERFACE_EXTENDED_ATTRIBUTES
    if unsupported_attributes:
        fail(f"the Rust bindings do not handle its extended attributes {sorted(unsupported_attributes)}")
    if interface.is_namespace or interface.is_callback_interface:
        fail("it is not a regular interface")
    if interface.parent_name and not is_implemented_in_rust(context.interfaces.get(interface.parent_name)):
        fail(f"it inherits from {interface.parent_name}, which is not implemented in Rust")
    if module.callback_functions:
        fail("its module defines callback functions")

    for other in inheriting_interfaces(interface, context):
        if not is_implemented_in_rust(other):
            fail(f"{other.name}, which is not implemented in Rust, inherits from it")

    rust_members(interface)


# C++ side


def write_cpp_declaration(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    includes.add("LibJS/HostObjectABI.h")
    includes.add("LibJS/Runtime/NativeFunction.h")
    includes.add("LibJS/Runtime/Object.h")
    includes.add("LibWeb/Bindings/InterfaceObject.h")
    includes.add("LibWeb/Export.h")
    out.write(
        f"""// The class of the wrappers of {interface.name}, whose implementation is in Rust, which code outside LibWeb names
// as well when it asks whether an object is a wrapper of the interface.
extern "C" WEB_API JSHostClass const {wrapper_host_class_symbol(interface)};

struct {interface.constructor_class} {{
public:
    static void initialize(JS::Realm&, JS::NativeFunction&);
    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(JS::HostFunction&, JS::FunctionObject&);
}};

struct {interface.prototype_class} {{
public:
    static void initialize(JS::Realm&, JS::Object&);
}};

"""
    )
    if rust_members(interface).pair_iterator is not None:
        out.write(
            f"""// The class of the default iterator objects of {interface.name}, whose cells the Rust bindings create.
extern "C" JSHostClass const {iterator_host_class_symbol(interface)};

struct {interface.name}IteratorPrototype {{
public:
    static void initialize(JS::Realm&, JS::Object&);
}};

"""
        )


def write_cpp_implementation(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Interface
) -> None:
    includes.add("LibJS/HostClassBuilder.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/HostFunction.h")
    includes.add("LibJS/Runtime/Realm.h")
    includes.add("LibWeb/Bindings/RustInterface.h")
    includes.add_binding(interface.implemented_name)
    parent = context.interfaces.get(interface.parent_name)
    if parent is not None:
        includes.add_binding(parent.implemented_name)
        parent_host_class = wrapper_host_class_symbol(parent)
    else:
        parent_host_class = "web_bindings_rust_interface_wrapper_host_class"

    initialize_prototype = rust_entry_point_name(interface, "initialize_prototype")
    initialize_constructor = rust_entry_point_name(interface, "initialize_constructor")
    construct = rust_entry_point_name(interface, "construct")
    supports_indexed_properties = rust_members(interface).indexed_property_getter is not None
    if supports_indexed_properties and inheriting_interfaces(interface, context):
        raise UnsupportedByRustBindings(
            f"{interface.path}: {interface.name} supports indexed properties and others inherit from it, which the "
            "Rust bindings do not handle yet"
        )
    out.write(
        f"""extern "C" {{

// The entry points of the Rust bindings of {interface.name}.
void {initialize_prototype}(JSVM*, void* realm, JSObject* prototype);
void {initialize_constructor}(JSVM*, void* realm, JSObject* constructor);
"""
    )
    if supports_indexed_properties:
        out.write(
            f"bool {item_value_entry_point(interface)}(JSVM*, void* realm, JSObject* wrapper, size_t index, "
            "JSValue* out_value);\n"
        )
    if interface.constructors:
        out.write(f"JSCompletion {construct}(JSVM*, JSObject* constructor, JSObject* new_target);\n")
    if not supports_indexed_properties:
        out.write(
            f"""
}}

// The wrappers are platform objects with ordinary internal methods: their implementation is a cell in their host data,
// not an implementation object in their wrappable slot, so they need none of the hooks and flags through which a
// wrapper keeps its implementation object alive, and the engine's own property caches apply to them.
constexpr JSHostClass {wrapper_host_class_symbol(interface)} = JS::make_host_class(JS_HOST_CLASS_OBJECT, "{interface.name}"sv,
    &{parent_host_class}, nullptr, nullptr, JS_HOST_CLASS_IS_PLATFORM_OBJECT);
"""
        )
    else:
        includes.add("LibWeb/Bindings/PlatformObject.h")
        legacy_info = f"{wrapper_name(interface)}_legacy_platform_object_info"
        out.write(
            f"""
}}

static Optional<JS::Value> {wrapper_name(interface)}_item_value(JS::HostObject const& wrapper, WrapperWorld&, JS::Realm& realm, size_t index)
{{
    JSValue value {{}};
    if (!{item_value_entry_point(interface)}(JS::HostABI::vm_to_abi(realm.vm()), &realm, JS::HostABI::object_to_abi(const_cast<JS::HostObject*>(&wrapper)), index, &value))
        return {{}};
    return JS::HostABI::value_from_abi(value);
}}

static constexpr LegacyPlatformObjectInfo {legacy_info} {{
    .supports_indexed_properties = true,
    .item_value = {wrapper_name(interface)}_item_value,
}};

// The wrappers are legacy platform objects, whose indexed properties the Rust bindings read: their implementation is a
// cell in their host data, not an implementation object in their wrappable slot.
constexpr JSHostClass {wrapper_host_class_symbol(interface)} = JS::make_host_class(JS_HOST_CLASS_OBJECT, "{interface.name}"sv,
    &{parent_host_class}, &indexed_legacy_platform_object_hooks, &{legacy_info},
    platform_object_host_class_flags | JS_HOST_CLASS_NOT_CACHEABLE_FOR_INDEXED_PROPERTY_ABSENCE);
"""
        )
    out.write(
        f"""
void {interface.prototype_class}::initialize(JS::Realm& realm, JS::Object& object)
{{
    {initialize_prototype}(JS::HostABI::vm_to_abi(realm.vm()), &realm, JS::HostABI::object_to_abi(&object));
}}

void {interface.constructor_class}::initialize(JS::Realm& realm, JS::NativeFunction& object)
{{
    {initialize_constructor}(JS::HostABI::vm_to_abi(realm.vm()), &realm, JS::HostABI::object_to_abi(&object));
}}

JS::ThrowCompletionOr<GC::Ref<JS::Object>> {interface.constructor_class}::construct([[maybe_unused]] JS::HostFunction& constructor, [[maybe_unused]] JS::FunctionObject& new_target)
{{
"""
    )
    if interface.constructors:
        out.write(
            f"""    auto object = TRY(JS::HostABI::completion_from_abi<JS::Value>({construct}(JS::HostABI::vm_to_abi(constructor.vm()), JS::HostABI::object_to_abi(&constructor), JS::HostABI::object_to_abi(&new_target))));
    return GC::Ref {{ object.as_object() }};
"""
        )
    else:
        out.write(
            f'    return constructor.vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, "{interface.name}");\n'
        )
    out.write("}\n\n")
    if rust_members(interface).pair_iterator is not None:
        initialize_iterator_prototype = rust_entry_point_name(interface, "initialize_iterator_prototype")
        out.write(
            f"""extern "C" void {initialize_iterator_prototype}(JSVM*, void* realm, JSObject* prototype);

constexpr JSHostClass {iterator_host_class_symbol(interface)} = JS::make_host_class(JS_HOST_CLASS_OBJECT, "{interface.name}Iterator"sv,
    nullptr, nullptr, nullptr, 0);

void {interface.name}IteratorPrototype::initialize(JS::Realm& realm, JS::Object& object)
{{
    {initialize_iterator_prototype}(JS::HostABI::vm_to_abi(realm.vm()), &realm, JS::HostABI::object_to_abi(&object));
}}

"""
        )


def write_ensure_web_prototype_declaration(out: TextIO) -> None:
    out.write(
        """
// The interface prototype object and the interface object of an interface implemented in Rust in the realm, for the
// Rust bindings.
extern "C" JSObject* web_bindings_ensure_web_prototype(void* realm, u16 interface_name);
extern "C" JSObject* web_bindings_ensure_web_constructor(void* realm, u16 interface_name);
extern "C" JSObject* web_bindings_ensure_web_iterator_prototype(void* realm, u16 interface_name);
"""
    )


def write_ensure_web_prototype_implementation(out: TextIO, intrinsics: List[Interface]) -> None:
    rust_interfaces = [interface for interface in intrinsics if is_implemented_in_rust(interface)]

    def write_switch(function: str, interfaces: List[Interface], object_of: Callable[[Interface], str]) -> None:
        out.write(
            f"""
JSObject* {function}(void* realm, u16 interface_name)
{{
    [[maybe_unused]] auto& js_realm = *static_cast<JS::Realm*>(realm);
    switch (static_cast<InterfaceName>(interface_name)) {{
"""
        )
        for interface in interfaces:
            out.write(
                f"""    case InterfaceName::{interface.name}:
        return JS::HostABI::object_to_abi(&{object_of(interface)});
"""
            )
        out.write(
            """    default:
        VERIFY_NOT_REACHED();
    }
}
"""
        )

    write_switch(
        "web_bindings_ensure_web_prototype",
        rust_interfaces,
        lambda interface: (
            f'ensure_web_prototype<{interface.prototype_class}>(js_realm, "{interface.namespaced_name}"_utf16_fly_string)'
        ),
    )
    write_switch(
        "web_bindings_ensure_web_constructor",
        rust_interfaces,
        lambda interface: (
            f'ensure_web_constructor<{interface.prototype_class}>(js_realm, "{interface.namespaced_name}"_utf16_fly_string)'
        ),
    )
    write_switch(
        "web_bindings_ensure_web_iterator_prototype",
        [interface for interface in rust_interfaces if rust_members(interface).pair_iterator is not None],
        lambda interface: (
            f'ensure_web_prototype<{interface.name}IteratorPrototype>(js_realm, "{interface.name}Iterator"_utf16_fly_string)'
        ),
    )


# Rust side


def constructor_description(interface: Interface) -> str:
    return f"the constructor of {interface.name}"


def write_argument_count_check(out: TextIO, function_name: str, required_argument_count: int) -> None:
    """Throws the errors of the C++ bindings for a call with too few arguments, which name the operation, or the
    interface for a constructor."""
    if required_argument_count == 1:
        out.write(
            f"""        if vm.argument_count() < 1 {{
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::BadArgCountOne, &[&"{function_name}"]);
        }}
"""
        )
    elif required_argument_count > 1:
        out.write(
            f"""        if vm.argument_count() < {required_argument_count} {{
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::BadArgCountMany,
                &[&"{function_name}", &{required_argument_count}],
            );
        }}
"""
        )


def write_parameter_conversions(
    out: TextIO, what: str, parameters: List[OperationParameter], context: GenerationContext
) -> None:
    for index, parameter in enumerate(parameters):
        parameter_what = f"the parameter {parameter.name} of {what}"
        if parameter.variadic:
            raise UnsupportedByRustBindings(f"{parameter_what} is variadic")
        unsupported = set(parameter.extended_attributes) - {
            "AllowShared",
            "AllowResizable",
            "Clamp",
            "EnforceRange",
            "LegacyNullToEmptyString",
        }
        if unsupported:
            raise UnsupportedByRustBindings(f"{parameter_what} has the extended attributes {sorted(unsupported)}")
        name = rust_local_name(parameter.name)
        conversion = rust_to_idl_value(
            parameter.type,
            parameter.extended_attributes,
            f"argument{index}",
            parameter.name,
            context,
            parameter_what,
            is_parameter=True,
        )
        out.write(f"        let argument{index} = vm.argument({index});\n")
        if parameter.optional and parameter.default_value is not None:
            out.write(
                f"""        let {name} = if argument{index}.is_undefined() {{
            {rust_default_value(parameter, context, parameter_what)}
        }} else {{
            {conversion}?
        }};
"""
            )
        elif parameter.optional:
            out.write(
                f"""        let {name} = if argument{index}.is_undefined() {{
            None
        }} else {{
            Some({conversion}?)
        }};
"""
            )
        else:
            out.write(f"        let {name} = {conversion}?;\n")


def implementation_method_signatures(
    context: GenerationContext, interface: Interface, members: RustMembers
) -> List[str]:
    """The methods the implementation of the interface defines, which the bindings call."""

    def parameter_list(what: str, parameters: List[OperationParameter]) -> str:
        return "".join(
            f", {rust_identifier(parameter.name)}: {rust_parameter_type(parameter, context, what)}"
            for parameter in parameters
        )

    signatures = []
    for constructor in members.constructors:
        parameters = parameter_list(constructor_description(interface), constructor.constructor.parameters)
        signatures.append(f"fn {constructor.method}(cx: &mut CallContext{parameters}) -> WebIdlResult<Self>")
    for attribute in members.attributes:
        what = f"{interface.name}.{attribute.attribute.name}"
        attribute_type = rust_type(attribute.attribute.type, context, what)
        if not attribute.attribute.inherit:
            signatures.append(
                f"fn {attribute.getter_method}(&self, cx: &mut CallContext) -> WebIdlResult<{attribute_type}>"
            )
        if attribute.setter_method is not None:
            signatures.append(
                f"fn {attribute.setter_method}(&self, cx: &mut CallContext, value: {attribute_type}) -> "
                "WebIdlResult<()>"
            )
    for operation_set in members.operation_sets:
        for overload in operation_set.overloads:
            if overload.is_default_to_json:
                continue
            what = f"{interface.name}.{operation_set.name}()"
            parameters = parameter_list(what, overload.operation.parameters)
            signatures.append(
                f"fn {overload.method}(&self, cx: &mut CallContext{parameters}) -> "
                f"WebIdlResult<{rust_result_type(overload.operation.return_type, context, what)}>"
            )
    if members.stringifier is not None and members.stringifier.defines_method:
        signatures.append(f"fn {members.stringifier.method}(&self, cx: &mut CallContext) -> WebIdlResult<Utf16String>")
    if members.pair_iterator is not None:
        key_type, value_type = pair_iterator_types(context, interface, members.pair_iterator)
        signatures.append(
            f"impl PairIterable with Key = {key_type} and Value = {value_type}: "
            "fn value_pairs_to_iterate_over(&self) -> impl Deref<Target = [(Self::Key, Self::Value)]> + '_"
        )
    if members.indexed_property_getter is not None:
        getter = members.indexed_property_getter.operation
        what = f"{interface.name}.{getter.operation.name}()"
        parameters = parameter_list(what, getter.operation.parameters)
        signatures.append(
            f"fn {getter.method}(&self, cx: &mut CallContext{parameters}) -> "
            f"WebIdlResult<{rust_result_type(getter.operation.return_type, context, what)}>"
        )
    for operation_set in members.static_operation_sets:
        for overload in operation_set.overloads:
            what = f"{interface.name}.{operation_set.name}()"
            parameters = parameter_list(what, overload.operation.parameters)
            signatures.append(
                f"fn {overload.method}(cx: &mut CallContext{parameters}) -> "
                f"WebIdlResult<{rust_result_type(overload.operation.return_type, context, what)}>"
            )
    return signatures


def pair_iterator_types(
    context: GenerationContext, interface: Interface, pair_iterator: RustPairIterator
) -> Tuple[str, str]:
    what = f"the pair iterator of {interface.name}"
    for idl_type in (pair_iterator.key_type, pair_iterator.value_type):
        if holds_cells(idl_type, context):
            raise UnsupportedByRustBindings(
                f"{interface.path}: {what} iterates over {idl_type}, a type whose values are or can hold cells, which "
                "the Rust bindings do not handle yet"
            )
    return (
        rust_type(pair_iterator.key_type, context, what, is_result=True),
        rust_type(pair_iterator.value_type, context, what, is_result=True),
    )


def write_rust_pair_iterator(out: TextIO, interface: Interface) -> None:
    """The classes of the default iterator objects of the interface, and the steps of its iterator prototype object,
    which the bindings runtime implements for every interface with a pair iterator,
    https://webidl.spec.whatwg.org/#es-iterator-prototype-object"""
    out.write(
        f"""
    /// The class of the cells of the default iterator objects of the interface.
    static ITERATOR_CELL_CLASS: EmbedderClass<DefaultIterator<Implementation>> = EmbedderClass::new("{interface.name} Iterator");

    unsafe extern "C" {{
        static {iterator_host_class_symbol(interface)}: JSHostClass;
    }}

    impl DefaultIteratorClasses for Implementation {{
        const ITERATOR_CELL_CLASS: &'static EmbedderClass<DefaultIterator<Self>> = &ITERATOR_CELL_CLASS;

        fn iterator_class() -> &'static JSHostClass {{
            // SAFETY: LibWeb defines the class of the default iterator objects as constant data.
            unsafe {{ &{iterator_host_class_symbol(interface)} }}
        }}
    }}

    /// The steps of {interface.name}IteratorPrototype::initialize() of LibWeb's C++ bindings.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a realm and the iterator prototype object of the realm.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "initialize_iterator_prototype")}(
        vm: *mut c_void,
        realm: *mut c_void,
        prototype: *mut c_void,
    ) {{
        // SAFETY: The caller passes live pointers.
        let (vm, realm, prototype) = unsafe {{ entry_point_arguments(vm, realm, prototype) }};
        define_operation(vm, realm, prototype, "next", raw_native!(iterator_next::<Implementation>), 0);
        define_to_string_tag(vm, prototype, "{interface.name} Iterator");
    }}
"""
    )


def write_rust_interface(
    out: TextIO, context: GenerationContext, module: Module, interface: Interface, interface_name_value: int
) -> None:
    implementation = rust_interface_path(interface)
    members = rust_members(interface)
    parent = context.interfaces.get(interface.parent_name)
    inherited_interface_name = "None" if parent is None else f"Some(super::{rust_module_name(parent)}::INTERFACE_NAME)"
    method_documentation = "".join(
        f"/// - `{signature}`\n" for signature in implementation_method_signatures(context, interface, members)
    )
    out.write(
        f"""
/// The bindings of {interface.name}, which call these methods of its implementation, {implementation}:
{method_documentation}pub(crate) mod {rust_module_name(interface)} {{
    #![allow(unused_imports)]

    use core::ffi::c_void;

    use ak::Utf16String;
    use libjs_rust::api::{{
        ArrayBuffer, DataView, EmbedderClass, ErrorKind, ErrorType, FunctionObject, Gc, JSHostClass, MarkedVec,
        Object, PropertyKey, RawNativeFunctionResult, ThrowCompletionOr, TypedArrayBase, TypedArrayKind, Value, Vm,
        is_host_instance_of, raw_native,
    }};

    use crate::bindings::conversions::{{
        ArrayBufferView, BufferSource, BufferSourceRules, CppInterface, EmptyDictionary, FromJs, ToJs, UsvString,
        implements_cpp_interface, record_from_js, record_to_js, rooted_sequence_to_js, sequence_from_js,
        sequence_to_js, to_array_buffer_view, to_boolean, to_buffer_source, to_cpp_interface,
        to_cpp_interface_in_union, to_dom_string,
        to_dom_string_treating_null_as_empty, to_double, to_float, to_long, to_long_enforcing_range, to_long_long,
        to_long_long_clamping, to_long_long_enforcing_range, to_typed_array, to_unrestricted_double,
        to_unrestricted_float, to_unsigned_long, to_unsigned_long_enforcing_range, to_unsigned_long_long,
        to_unsigned_long_long_enforcing_range, to_usv_string,
    }};
    use crate::bindings::interface::{{
        Inherits, Interface, Ref, Wrapped, cell_class, construct_arguments, construct_wrapper, create_dictionary_object,
        define_attribute, define_cached_attribute, define_constants, define_interface_object_properties,
        define_operation, define_to_string_tag, define_unimplemented_property, define_value_iterator_properties,
        entry_point_arguments, implementation_if, implementation_of, prototype_from_new_target, realm_is_window,
        rejected_promise, set_dictionary_member, set_interface_prototype_object_prototype, this_object, to_interface,
    }};
    use crate::bindings::iterable::{{
        DefaultIterator, DefaultIteratorClasses, define_pair_iterator_properties, entries, for_each, iterator_next, keys,
        values,
    }};
    use crate::bindings::structured_data;
    use crate::bindings::{{CallContext, WebIdlResult}};

    use {implementation} as Implementation;

    /// Web::Bindings::InterfaceName::{interface.name}.
    pub(crate) const INTERFACE_NAME: u16 = {interface_name_value};

    /// The class of the cells of the objects of the interface.
    static CELL_CLASS: EmbedderClass<Wrapped<Implementation>> = cell_class("{interface.name}");

    unsafe extern "C" {{
        static {wrapper_host_class_symbol(interface)}: JSHostClass;
    }}

    impl Interface for Implementation {{
        const CELL_CLASS: &'static EmbedderClass<Wrapped<Self>> = &CELL_CLASS;
        const INTERFACE_NAME: u16 = INTERFACE_NAME;
        const NAME: &'static str = "{interface.namespaced_name}";

        fn wrapper_class() -> &'static JSHostClass {{
            // SAFETY: LibWeb defines the class of the wrappers as constant data.
            unsafe {{ &{wrapper_host_class_symbol(interface)} }}
        }}
    }}

"""
    )
    write_this_implementation(out, context, interface)

    defined_property_names: List[str] = []
    recorded_unimplemented_names: List[str] = []

    if members.constants:
        constants = "".join(f'        ("{constant.name}", {constant.value}.0),\n' for constant in members.constants)
        out.write(
            f"""
    /// The constants of the interface, which its interface object and its interface prototype object both have.
    static CONSTANTS: [(&str, f64); {len(members.constants)}] = [
{constants}    ];
"""
        )

    for attribute in members.attributes:
        name = attribute.attribute.name
        conversion = rust_to_js_value(attribute.attribute.type, "result", "&cx", context, f"{interface.name}.{name}")
        call = f"Implementation::{attribute.getter_method}(&implementation, &mut cx)"
        if attribute.attribute.inherit:
            call = inherited_attribute_getter_call(context, interface, attribute)
        out.write(
            f"""
    pub(super) fn {attribute.getter_function}(vm: &Vm) -> ThrowCompletionOr<Value> {{
        let (implementation, mut cx) = this_implementation(vm)?;
        let result = {call}
            .map_err(|exception| exception.throw(&cx))?;
        {conversion}
    }}
"""
        )
        if attribute.setter_method is not None:
            write_rust_attribute_setter(out, context, interface, attribute)

    for operation_set in members.operation_sets:
        name = operation_set.name
        if len(operation_set.overloads) == 1 and operation_set.overloads[0].is_default_to_json:
            write_default_to_json(out, context, interface, operation_set.function)
            continue
        if len(operation_set.overloads) == 1:
            write_rust_operation(out, context, interface, name, operation_set.function, operation_set.overloads[0])
            continue
        for overload in operation_set.overloads:
            write_rust_operation(out, context, interface, name, overload.steps_function, overload)
        write_operation_overload_resolution(
            out,
            context,
            interface,
            name,
            operation_set.function,
            [overload.operation for overload in operation_set.overloads],
            [overload.steps_function for overload in operation_set.overloads],
        )

    if members.stringifier is not None:
        write_rust_stringifier(out, members.stringifier.function, members.stringifier.method)

    if members.indexed_property_getter is not None:
        getter = members.indexed_property_getter.operation
        getter_set = RustOperationSet(getter.operation.name, [getter])
        write_rust_operation(out, context, interface, getter_set.name, getter_set.function, getter)
        write_rust_item_value(out, context, interface, getter)

    if members.pair_iterator is not None:
        write_rust_pair_iterator(out, interface)

    for operation_set in members.static_operation_sets:
        if len(operation_set.overloads) > 1:
            raise UnsupportedByRustBindings(
                f"{interface.path}: the static operation {operation_set.name} of {interface.name} is overloaded, which "
                "the Rust bindings do not handle yet"
            )
        write_rust_static_operation(
            out, context, interface, operation_set.name, operation_set.function, operation_set.overloads[0]
        )

    out.write(
        f"""
    /// The steps of {interface.prototype_class}::initialize() of LibWeb's C++ bindings.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a realm and the interface prototype object of the realm.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "initialize_prototype")}(
        vm: *mut c_void,
        realm: *mut c_void,
        prototype: *mut c_void,
    ) {{
        // SAFETY: The caller passes live pointers.
        let (vm, realm, prototype) = unsafe {{ entry_point_arguments(vm, realm, prototype) }};
        set_interface_prototype_object_prototype(vm, realm, prototype, {inherited_interface_name});
"""
    )
    # NB: The properties are defined in the order of the C++ bindings: the attributes, the operations, the operations
    #     that are not implemented, then the constants.
    for attribute in members.attributes_in_order:
        if isinstance(attribute, str):
            out.write(f'        define_unimplemented_property(vm, prototype, "{attribute}");\n')
            recorded_unimplemented_names.append(attribute)
            continue
        name = attribute.attribute.name
        getter = f"raw_native!({attribute.getter_function})"
        if attribute.is_cached:
            out.write(f'        define_cached_attribute(vm, realm, prototype, "{name}", {getter});\n')
            defined_property_names.append(name)
            continue
        setter = "None" if attribute.setter_method is None else f"raw_native!({attribute.setter_function})"
        out.write(f'        define_attribute(vm, realm, prototype, "{name}", {getter}, {setter});\n')
        defined_property_names.append(name)
    for operation_set in members.operation_sets:
        name = operation_set.name
        function = operation_set.function
        length = operation_set.length
        definition = f'define_operation(vm, realm, prototype, "{name}", raw_native!({function}), {length});'
        out.write(window_only_definition(definition, operation_set.is_window_only))
        defined_property_names.append(name)
    for name in members.unimplemented_operation_names:
        out.write(f'        define_unimplemented_property(vm, prototype, "{name}");\n')
        recorded_unimplemented_names.append(name)
    if members.stringifier is not None:
        function = members.stringifier.function
        definition = f'define_operation(vm, realm, prototype, "toString", raw_native!({function}), 0);'
        out.write(window_only_definition(definition, members.stringifier.is_window_only))
        defined_property_names.append("toString")
    if members.indexed_property_getter is not None:
        getter = members.indexed_property_getter.operation
        getter_set = RustOperationSet(getter.operation.name, [getter])
        out.write(
            f'        define_operation(vm, realm, prototype, "{getter_set.name}", raw_native!({getter_set.function}), '
            f"{getter_set.length});\n"
        )
        defined_property_names.append(getter_set.name)
        if members.indexed_property_getter.has_value_iterator:
            out.write("        define_value_iterator_properties(vm, realm, prototype);\n")
    if members.pair_iterator is not None:
        out.write(
            """        define_pair_iterator_properties(
            vm,
            realm,
            prototype,
            raw_native!(entries::<Implementation>),
            raw_native!(for_each::<Implementation>),
            raw_native!(keys::<Implementation>),
            raw_native!(values::<Implementation>),
        );
"""
        )
        defined_property_names.extend(members.pair_iterator.function_names)
    if members.constants:
        out.write("        define_constants(vm, prototype, &CONSTANTS);\n")
        defined_property_names.extend(constant.name for constant in members.constants)
    # NB: A member that rust_members() accepts and nothing above defines would be missing from the prototype without
    #     any error, so the two are compared.
    assert defined_property_names == members.property_names(), (
        f"{interface.name}: the bindings define {defined_property_names}, its members are {members.property_names()}"
    )
    assert recorded_unimplemented_names == members.unimplemented_property_names()
    out.write(
        f"""        define_to_string_tag(vm, prototype, "{interface.namespaced_name}");
    }}

    /// The steps of {interface.constructor_class}::initialize() of LibWeb's C++ bindings.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a realm and the interface object of the realm.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "initialize_constructor")}(
        vm: *mut c_void,
        realm: *mut c_void,
        constructor: *mut c_void,
    ) {{
        // SAFETY: The caller passes live pointers.
        let (vm, realm, constructor) = unsafe {{ entry_point_arguments(vm, realm, constructor) }};
        define_interface_object_properties(vm, realm, constructor, "{interface.name}", {members.constructor_length}, INTERFACE_NAME, {inherited_interface_name});
"""
    )
    if members.constants:
        out.write("        define_constants(vm, constructor, &CONSTANTS);\n")
    for operation_set in members.static_operation_sets:
        name = operation_set.name
        definition = (
            f'define_operation(vm, realm, constructor, "{name}", raw_native!({operation_set.function}), '
            f"{operation_set.length});"
        )
        out.write(window_only_definition(definition, operation_set.is_window_only))
    out.write("    }\n")

    for constructor in members.constructors:
        write_rust_constructor(out, context, interface, constructor)
    if len(members.constructors) > 1:
        steps_functions = [constructor.steps_function for constructor in members.constructors]
        write_constructor_overload_resolution(out, context, interface, steps_functions)
    if members.constructors:
        out.write(
            f"""
    /// {interface.constructor_class}::construct() of LibWeb's C++ bindings.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, the interface object and the new target of a construction.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "construct")}(
        vm: *mut c_void,
        _constructor: *mut c_void,
        new_target: *mut c_void,
    ) -> RawNativeFunctionResult {{
        // SAFETY: The caller passes the VM and the new target.
        let (vm, new_target) = unsafe {{ construct_arguments(vm, new_target) }};
        RawNativeFunctionResult::from_completion(construct(vm, new_target))
    }}
"""
        )
    write_rust_structured_data_entry_points(out, interface)
    out.write("}\n")


def window_only_definition(definition: str, is_window_only: bool) -> str:
    """A statement of the steps that initialize an interface object or an interface prototype object, which defines a
    member that only a Window has only if the global object of the realm is a Window."""
    if not is_window_only:
        return f"        {definition}\n"
    return f"""        if realm_is_window(realm) {{
            {definition}
        }}
"""


def write_default_to_json(out: TextIO, context: GenerationContext, interface: Interface, function: str) -> None:
    """The native function of a [Default] toJSON operation, which runs the default toJSON steps, in the order of the C++
    bindings, https://webidl.spec.whatwg.org/#default-tojson-steps"""
    values = StringIO()
    # NB: The inheritance stack of the interface has the interface on top and the one it inherits from at the bottom,
    #     and collecting the attribute values of the stack pops it, https://webidl.spec.whatwg.org/#collect-attribute-values-of-an-inheritance-stack
    #     so the attributes of the interfaces that others inherit from are collected first.
    for interface_in_chain in context.inheritance_stack(interface):
        if not any(operation_is_default_to_json(operation) for operation in interface_in_chain.regular_operations):
            continue
        if not is_implemented_in_rust(interface_in_chain):
            raise UnsupportedByRustBindings(
                f"{interface.path}: {interface.name} inherits a [Default] toJSON() from {interface_in_chain.name}, "
                "which is not implemented in Rust"
            )
        module = "" if interface_in_chain.name == interface.name else f"super::{rust_module_name(interface_in_chain)}::"
        for attribute in rust_members(interface_in_chain).attributes:
            if not idl_type_is_json_type(attribute.attribute.type, context):
                continue
            values.write(
                f"""        let value = {module}{attribute.getter_function}(vm)?;
        set_dictionary_member(&cx, result, &PropertyKey::from_utf8("{attribute.attribute.name}"), value)?;
"""
            )
    out.write(
        f"""
    fn {function}(vm: &Vm) -> ThrowCompletionOr<Value> {{
        this_implementation(vm)?;
        let realm = vm.current_realm().expect("a native function runs in a realm");
        let cx = CallContext::new(vm, realm);
        // 4. Let result be OrdinaryObjectCreate(%Object.prototype%).
        let result = create_dictionary_object(&cx);
        // 5. For each key → value of map:
{values.getvalue()}        // 6. Return result.
        Ok(Value::from_object(result))
    }}
"""
    )


def item_value_entry_point(interface: Interface) -> str:
    return rust_entry_point_name(interface, "item_value")


def write_rust_item_value(out: TextIO, context: GenerationContext, interface: Interface, getter: RustOperation) -> None:
    """The entry point through which the internal methods of a legacy platform object that LibWeb's C++ defines read
    an indexed property of a wrapper of the interface, which has a value if the index is a supported property index,
    https://webidl.spec.whatwg.org/#legacy-platform-object-getownproperty"""
    return_type = getter.operation.return_type
    if not return_type.nullable:
        raise UnsupportedByRustBindings(
            f"{interface.path}: the indexed property getter of {interface.name} does not return a nullable type, by "
            "which the Rust bindings tell the indices that are not supported property indices"
        )
    what = f"the indexed property getter of {interface.name}"
    conversion = rust_to_js_value(return_type.without_nullable(), "value", "&cx", context, what)
    out.write(
        f"""
    /// The value of the indexed property of the wrapper, which LibWeb's C++ asks for, if the index is a supported
    /// property index.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a live realm and a live wrapper of the interface.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {item_value_entry_point(interface)}(
        vm: *mut c_void,
        realm: *mut c_void,
        wrapper: *mut c_void,
        index: usize,
        out_value: &mut u64,
    ) -> bool {{
        // SAFETY: The caller passes live pointers.
        let (vm, realm, wrapper) = unsafe {{ entry_point_arguments(vm, realm, wrapper) }};
        let Ok(index) = u32::try_from(index) else {{
            return false;
        }};
        let implementation = implementation_of::<Implementation>(wrapper);
        let mut cx = CallContext::new(vm, realm);
        let Ok(Some(value)) = Implementation::{getter.method}(&implementation, &mut cx, index) else {{
            return false;
        }};
        let value = {conversion}.expect("converting an object to a JS value does not throw");
        *out_value = value.0;
        true
    }}
"""
    )


def write_rust_stringifier(out: TextIO, function: str, method: str) -> None:
    """The native function of the stringifier, which calls the method of the implementation,
    https://webidl.spec.whatwg.org/#es-stringifier"""
    out.write(
        f"""
    fn {function}(vm: &Vm) -> ThrowCompletionOr<Value> {{
        let (implementation, mut cx) = this_implementation(vm)?;
        let result = Implementation::{method}(&implementation, &mut cx)
            .map_err(|exception| exception.throw(&cx))?;
        result.to_js(&cx)
    }}
"""
    )


def write_rust_static_operation(
    out: TextIO, context: GenerationContext, interface: Interface, name: str, function: str, overload: RustOperation
) -> None:
    """The native function of a static operation, which runs in the current realm, in the order of the C++ bindings:
    the arguments are counted, then converted, https://webidl.spec.whatwg.org/#es-operations"""
    operation = overload.operation
    what = f"{interface.name}.{name}()"
    if is_promise_type(operation.return_type):
        raise UnsupportedByRustBindings(
            f"{what} is static and returns a promise type, which the Rust bindings do not handle yet"
        )
    out.write(
        f"""
    fn {function}(vm: &Vm) -> ThrowCompletionOr<Value> {{
        let realm = vm.current_realm().expect("a native function runs in a realm");
"""
    )
    write_argument_count_check(out, name, overload.length)
    write_parameter_conversions(out, what, operation.parameters, context)
    arguments = "".join(f", {rust_local_name(parameter.name)}" for parameter in operation.parameters)
    conversion = rust_to_js_value(operation.return_type, "result", "&cx", context, what)
    out.write(
        f"""        let mut cx = CallContext::new(vm, realm);
        let result = Implementation::{overload.method}(&mut cx{arguments})
            .map_err(|exception| exception.throw(&cx))?;
        {conversion}
    }}
"""
    )


def write_rust_structured_data_entry_points(out: TextIO, interface: Interface) -> None:
    """The entry points through which StructuredSerialize of LibWeb's C++ runs the steps of an interface that is
    serializable or transferable, which Meta/Generators/libweb_bindings/structured_serialize.py declares."""
    if "Serializable" in interface.extended_attributes:
        out.write(
            f"""
    /// The serialization steps of the interface, for StructuredSerialize of LibWeb's C++.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a live realm, the live cell of an object of the interface, and a lent writer.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "serialization_steps")}(
        vm: *mut c_void,
        realm: *mut c_void,
        cell: *mut c_void,
        serialized: *mut c_void,
        for_storage: bool,
    ) -> RawNativeFunctionResult {{
        // SAFETY: The caller passes live pointers.
        unsafe {{ structured_data::serialization_steps::<Implementation>(vm, realm, cell, serialized, for_storage) }}
    }}

    /// The deserialization steps of the interface, which return the wrapper of the new object.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a live realm, and a lent reader.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "deserialization_steps")}(
        vm: *mut c_void,
        realm: *mut c_void,
        serialized: *mut c_void,
    ) -> RawNativeFunctionResult {{
        // SAFETY: The caller passes live pointers.
        unsafe {{ structured_data::deserialization_steps::<Implementation>(vm, realm, serialized) }}
    }}
"""
        )
    if "Transferable" in interface.extended_attributes:
        out.write(
            f"""
    /// The [[Detached]] internal slot of an object of the interface, for StructuredSerialize of LibWeb's C++.
    ///
    /// # Safety
    ///
    /// The cell must be the live cell of an object of the interface.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "is_detached")}(cell: *mut c_void) -> bool {{
        // SAFETY: The caller passes a live cell.
        unsafe {{ structured_data::is_detached::<Implementation>(cell) }}
    }}

    /// Sets the [[Detached]] internal slot of an object of the interface to true.
    ///
    /// # Safety
    ///
    /// The cell must be the live cell of an object of the interface.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "set_detached")}(cell: *mut c_void) {{
        // SAFETY: The caller passes a live cell.
        unsafe {{ structured_data::set_detached::<Implementation>(cell) }}
    }}

    /// The transfer steps of the interface.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a live realm, the live cell of an object of the interface, and a lent data holder.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "transfer_steps")}(
        vm: *mut c_void,
        realm: *mut c_void,
        cell: *mut c_void,
        data_holder: *mut c_void,
    ) -> RawNativeFunctionResult {{
        // SAFETY: The caller passes live pointers.
        unsafe {{ structured_data::transfer_steps::<Implementation>(vm, realm, cell, data_holder) }}
    }}

    /// The transfer-receiving steps of the interface, which return the wrapper of the new object.
    ///
    /// # Safety
    ///
    /// The pointers must be the VM, a live realm, and a lent data holder.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn {rust_entry_point_name(interface, "transfer_receiving_steps")}(
        vm: *mut c_void,
        realm: *mut c_void,
        data_holder: *mut c_void,
    ) -> RawNativeFunctionResult {{
        // SAFETY: The caller passes live pointers.
        unsafe {{ structured_data::transfer_receiving_steps::<Implementation>(vm, realm, data_holder) }}
    }}
"""
        )


def write_this_implementation(out: TextIO, context: GenerationContext, interface: Interface) -> None:
    """The function that finds what an attribute or an operation of the interface runs on. For an interface that
    others inherit from, that is an object of any of them, which the implementation of each reaches the inherited
    part of through Inherits."""
    inheriting = inheriting_interfaces(interface, context)
    if not inheriting:
        out.write(
            """
    /// The object that an attribute or an operation runs on, and the context it runs in.
    fn this_implementation(vm: &Vm) -> ThrowCompletionOr<(Ref<Implementation>, CallContext<'_>)> {
        let (object, realm) = this_object(vm, Implementation::wrapper_class(), Implementation::NAME)?;
        Ok((implementation_of(object), CallContext::new(vm, realm)))
    }
"""
        )
        return

    variants = f"        {interface.name}(Ref<Implementation>),\n"
    arms = f"""                Self::{interface.name}(object) => {{
                    let object: &Implementation = object;
                    object
                }}
"""
    choices = f"""        if let Some(object) = implementation_if::<Implementation>(object) {{
            return Ok((Receiver::{interface.name}(object), cx));
        }}
"""
    for other in inheriting:
        path = rust_interface_path(other)
        variants += f"        {other.name}(Ref<{path}>),\n"
        # NB: An interface reaches the part of an interface it does not inherit from directly through the ones between.
        steps = f"                    let object: &{path} = object;\n"
        ancestor = other
        while ancestor.name != interface.name:
            next_ancestor = context.interfaces[ancestor.parent_name]
            next_path = "Implementation" if next_ancestor.name == interface.name else rust_interface_path(next_ancestor)
            step = f"<{rust_interface_path(ancestor)} as Inherits<{next_path}>>::parent(object)"
            # NB: The last step is the value of the arm.
            if next_ancestor.name == interface.name:
                steps += f"                    {step}\n"
            else:
                steps += f"                    let object = {step};\n"
            ancestor = next_ancestor
        arms += f"""                Self::{other.name}(object) => {{
{steps}                }}
"""
        choices += f"""        if let Some(object) = implementation_if::<{path}>(object) {{
            return Ok((Receiver::{other.name}(object), cx));
        }}
"""
    out.write(
        f"""
    /// The object that an attribute or an operation of the interface runs on: an object of the interface, or of an
    /// interface that inherits from it.
    enum Receiver {{
{variants}    }}

    impl core::ops::Deref for Receiver {{
        type Target = Implementation;

        fn deref(&self) -> &Implementation {{
            match self {{
{arms}            }}
        }}
    }}

    /// The object that an attribute or an operation runs on, and the context it runs in.
    fn this_implementation(vm: &Vm) -> ThrowCompletionOr<(Receiver, CallContext<'_>)> {{
        let (object, realm) = this_object(vm, Implementation::wrapper_class(), Implementation::NAME)?;
        let cx = CallContext::new(vm, realm);
{choices}        unreachable!("a wrapper of the interface wraps an object of it or of an interface that inherits from it")
    }}
"""
    )


def inherited_attribute_getter_call(context: GenerationContext, interface: Interface, attribute: RustAttribute) -> str:
    """The call of the getter of an attribute that the interface redeclares with inherit, whose getter is that of the
    interface it inherits the attribute from, https://webidl.spec.whatwg.org/#dfn-inherit-getter. The implementation
    reaches the part that the getter runs on through Inherits."""
    name = attribute.attribute.name
    receiver = "&*implementation"
    path = rust_interface_path(interface)
    ancestor = context.interfaces.get(interface.parent_name)
    while ancestor is not None:
        ancestor_path = rust_interface_path(ancestor)
        receiver = f"<{path} as Inherits<{ancestor_path}>>::parent({receiver})"
        for ancestor_attribute in rust_members(ancestor).attributes:
            if ancestor_attribute.attribute.name == name and not ancestor_attribute.attribute.inherit:
                return f"{ancestor_path}::{ancestor_attribute.getter_method}({receiver}, &mut cx)"
        path = ancestor_path
        ancestor = context.interfaces.get(ancestor.parent_name)
    raise UnsupportedByRustBindings(
        f"{interface.path}: {interface.name} inherits the attribute {name}, which no interface it inherits from declares"
    )


def write_rust_attribute_setter(
    out: TextIO, context: GenerationContext, interface: Interface, attribute: RustAttribute
) -> None:
    """The native function of the setter of an attribute, in the order of the C++ bindings: the value is read before
    the this value is checked, and a missing value is undefined instead of an error."""
    name = attribute.attribute.name
    what = f"{interface.name}.{name}"
    conversion = rust_to_idl_value(
        attribute.attribute.type, attribute.attribute.extended_attributes, "value", name, context, what
    )
    if context.enumeration(attribute.attribute.type) is not None:
        # NB: The setter of an attribute of an enumeration type ignores a value that is not one of the enumeration's,
        #     https://webidl.spec.whatwg.org/#dfn-attribute-setter, and, as the C++ bindings do, a value whose
        #     conversion throws.
        conversion = f"""let Ok(idl_value) = {conversion} else {{
            return Ok(Value::UNDEFINED);
        }};"""
    else:
        conversion = f"let idl_value = {conversion}?;"
    out.write(
        f"""
    fn {attribute.setter_function}(vm: &Vm) -> ThrowCompletionOr<Value> {{
        let value = vm.argument(0);
        let (implementation, mut cx) = this_implementation(vm)?;
        let type_error_realm_scope = vm.type_error_realm_scope(cx.realm());
        {conversion}
        drop(type_error_realm_scope);
        Implementation::{attribute.setter_method}(&implementation, &mut cx, idl_value)
            .map_err(|exception| exception.throw(&cx))?;
        Ok(Value::UNDEFINED)
    }}
"""
    )


def write_rust_operation(
    out: TextIO, context: GenerationContext, interface: Interface, name: str, function: str, overload: RustOperation
) -> None:
    """The native function of the steps of an operation, in the order of the C++ bindings: the this value is checked,
    then the arguments are counted and converted, with the type errors of the conversions in the realm of the object."""
    operation = overload.operation
    what = f"{interface.name}.{name}()"
    steps = StringIO()
    steps.write("        let (implementation, mut cx) = this_implementation(vm)?;\n")
    write_argument_count_check(steps, name, overload.length)
    if operation.parameters:
        steps.write("        let type_error_realm_scope = vm.type_error_realm_scope(cx.realm());\n")
        write_parameter_conversions(steps, what, operation.parameters, context)
        steps.write("        drop(type_error_realm_scope);\n")
    arguments = "".join(f", {rust_local_name(parameter.name)}" for parameter in operation.parameters)
    returns_promise = is_promise_type(operation.return_type) and not operation.return_type.nullable
    call = f"Implementation::{overload.method}(&implementation, &mut cx{arguments})"
    if operation.return_type.name == "undefined" and not operation.return_type.nullable:
        # NB: An operation that returns nothing has no result to name, and the conversion of nothing is undefined.
        steps.write(
            f"""        {call}
            .map_err(|exception| exception.throw(&cx))?;
        Ok(Value::UNDEFINED)
"""
        )
    else:
        if returns_promise:
            conversion = "result.to_js(&cx)"
        else:
            conversion = rust_to_js_value(operation.return_type, "result", "&cx", context, what)
        steps.write(
            f"""        let result = {call}
            .map_err(|exception| exception.throw(&cx))?;
        {conversion}
"""
        )

    out.write(f"\n    fn {function}(vm: &Vm) -> ThrowCompletionOr<Value> {{\n")
    if returns_promise:
        # NB: The C++ bindings run everything, the check of the this value included, inside these steps.
        out.write(
            f"""        // An exception that the steps of an operation with a promise type throw rejects a new promise,
        // https://webidl.spec.whatwg.org/#dfn-create-operation-function
        let steps = || -> ThrowCompletionOr<Value> {{
{textwrap.indent(steps.getvalue(), "    ")}        }};
        steps().or_else(|throw| Ok(rejected_promise(vm, throw)))
"""
        )
    else:
        out.write(steps.getvalue())
    out.write("    }\n")


def write_rust_constructor(
    out: TextIO, context: GenerationContext, interface: Interface, constructor: RustConstructor
) -> None:
    """The steps of a constructor in the order of the C++ bindings: the prototype of the new object is read from the
    new target before the arguments are counted and converted, and the conversions throw in the current realm."""
    parameters = constructor.constructor.parameters
    out.write(
        f"""
    /// The constructor steps of the interface, which internally create a new object implementing the interface,
    /// https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
    fn {constructor.steps_function}(vm: &Vm, new_target: Gc<Object>) -> ThrowCompletionOr<Value> {{
        let realm = vm.current_realm().expect("a construction runs in a realm");
        let prototype = prototype_from_new_target(vm, new_target, INTERFACE_NAME)?;
"""
    )
    write_argument_count_check(out, interface.name, constructor.length)
    write_parameter_conversions(out, constructor_description(interface), parameters, context)
    arguments = "".join(f", {rust_local_name(parameter.name)}" for parameter in parameters)
    out.write(
        f"""        let mut cx = CallContext::new(vm, realm);
        let implementation =
            Implementation::{constructor.method}(&mut cx{arguments}).map_err(|exception| exception.throw(&cx))?;
        Ok(Value::from_object(construct_wrapper(vm, realm, prototype, implementation)))
    }}
"""
    )


def write_cpp_interface_names(out: TextIO, usage: DefinitionUsage, interface_name_values: Dict[str, int]) -> None:
    """The values by which the bindings ask LibWeb whether an object implements an interface that it implements in
    C++."""
    if not usage.cpp_interfaces_named_by_rust:
        return
    out.write(
        """
/// Web::Bindings::InterfaceName of the interfaces that LibWeb implements in C++ and that the interfaces implemented in
/// Rust name.
pub(crate) mod cpp_interface_names {
"""
    )
    for name, interface in sorted(usage.cpp_interfaces_named_by_rust.items()):
        if name not in interface_name_values:
            raise UnsupportedByRustBindings(
                f"An interface implemented in Rust names {name}, which has no Web::Bindings::InterfaceName"
            )
        constant = cpp_interface_name_constant(interface).rpartition("::")[2]
        out.write(f"    pub(crate) const {constant}: u16 = {interface_name_values[name]};\n")
    out.write("}\n")


def write_rust_definitions(out: TextIO, context: GenerationContext, usage: DefinitionUsage) -> None:
    """The conversions of the dictionaries and unions that the interfaces implemented in Rust name, each written once,
    whichever interfaces name it."""
    if not usage.rust_dictionaries and not usage.rust_unions and not usage.rust_enumerations:
        return
    out.write(
        """
/// The conversions of the definitions that the interfaces implemented in Rust name, whose types the modules of their
/// IDL files define next to the implementations.
pub(crate) mod definitions {
    #![allow(unused_imports)]

    use std::sync::LazyLock;

    use ak::Utf16String;
    use libjs_rust::api::{
        ErrorKind, ErrorType, Gc, Object, PropertyKey, ThrowCompletionOr, TypedArrayBase, TypedArrayKind, Value, Vm,
        is_host_instance_of,
    };

    use crate::bindings::CallContext;
    use crate::bindings::conversions::{
        ArrayBufferView, BufferSource, BufferSourceRules, CppInterface, EmptyDictionary, FromJs, ToJs, UsvString,
        buffer_source_if, dictionary_member, dictionary_object, enumeration_value_to_js, implements_cpp_interface,
        iterator_method, record_from_js, record_to_js, rooted_sequence_to_js, sequence_from_iterable, sequence_from_js,
        sequence_to_js, throw_no_union_types_matched, to_array_buffer_view, to_boolean, to_buffer_source,
        to_cpp_interface,
        to_dom_string, to_dom_string_treating_null_as_empty, to_double, to_enumeration_value, to_float, to_long,
        to_long_enforcing_range, to_long_long, to_long_long_enforcing_range, to_typed_array, to_unrestricted_double,
        to_unrestricted_float, to_unsigned_long, to_unsigned_long_enforcing_range, to_unsigned_long_long,
        to_unsigned_long_long_enforcing_range, to_usv_string,
    };
    use crate::bindings::interface::{
        Interface, Ref, create_dictionary_object, implementation_if, implementation_of, set_dictionary_member,
        to_interface,
    };
"""
    )
    for name, directions in usage.rust_dictionaries.items():
        dictionary = context.dictionaries[name]
        if dictionary.parent_name and directions.to_js:
            raise UnsupportedByRustBindings(
                f"The Rust bindings convert the dictionary {name}, which inherits from another dictionary, to a JS value"
            )
        if directions.from_js:
            write_dictionary_from_js(out, context, dictionary)
        if directions.to_js:
            write_dictionary_to_js(out, context, dictionary)
    for name, directions in usage.rust_enumerations.items():
        enumeration = context.enumerations[name]
        if directions.from_js:
            write_enumeration_from_js(out, context, enumeration)
        if directions.to_js:
            write_enumeration_to_js(out, context, enumeration)
    for union_type, directions in usage.rust_unions.items():
        if directions.from_js:
            write_union_from_js(out, context, union_type)
        if directions.to_js:
            write_union_to_js(out, context, union_type)
    out.write("}\n")


def write_rust_bindings(
    out: TextIO, context: GenerationContext, usage: DefinitionUsage, intrinsics: List[Interface]
) -> None:
    out.write(
        """// Generated by Meta/Generators/generate_libweb_bindings.py -- DO NOT EDIT
//
// The bindings of the interfaces that are implemented in Rust.
"""
    )
    values = interface_name_values(intrinsics)
    write_cpp_interface_names(out, usage, values)
    write_rust_definitions(out, context, usage)
    for module in context.modules:
        interface = module.interface
        if interface is None or not is_implemented_in_rust(interface):
            continue
        write_rust_interface(out, context, module, interface, values[interface.name])
    write_rust_wrap_for_cpp(out, context, usage)


def write_rust_wrap_for_cpp(out: TextIO, context: GenerationContext, usage: DefinitionUsage) -> None:
    """The entry point through which the C++ bindings of the interfaces that name an interface implemented in Rust
    wrap an object of it, which C++ code holds as the cell of the object."""
    documentation = """
/// The wrapper of an object of an interface implemented in Rust in the wrapper world of the realm, for the C++
/// bindings of the interfaces that name the interface.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and the live cell of an object of the interface.
#[unsafe(no_mangle)]
"""
    if not usage.rust_interfaces_named_by_cpp:
        out.write(
            f"""{documentation}pub unsafe extern "C" fn web_rust_bindings_wrap(
    _vm: *mut core::ffi::c_void,
    _realm: *mut core::ffi::c_void,
    _interface_name: u16,
    _cell: *mut core::ffi::c_void,
) -> *mut core::ffi::c_void {{
    unreachable!("LibWeb's C++ bindings name no interface that is implemented in Rust")
}}
"""
        )
        return

    def arm(interface: Interface) -> str:
        inheriting = inheriting_interfaces(interface, context)
        if not inheriting:
            return f"""        {rust_module_name(interface)}::INTERFACE_NAME => {{
            // SAFETY: The caller passes the cell of an object of the interface.
            wrap(&cx, unsafe {{ cell_from_abi::<{rust_interface_path(interface)}>(cell) }})
        }}
"""
        # NB: C++ names an object of an interface that others inherit from by that interface, whichever of them the
        #     object is.
        choices = "".join(
            f"""            // SAFETY: The caller passes a live cell.
            if let Some(object) = unsafe {{ cell_if_abi::<{rust_interface_path(candidate)}>(cell) }} {{
                break 'wrapper wrap(&cx, object);
            }}
"""
            for candidate in [interface] + inheriting
        )
        return f"""        {rust_module_name(interface)}::INTERFACE_NAME => 'wrapper: {{
{choices}            unreachable!("C++ passes the cell of an object of the interface or of one that inherits from it")
        }}
"""

    arms = "".join(arm(interface) for _, interface in sorted(usage.rust_interfaces_named_by_cpp.items()))
    out.write(
        f"""{documentation}pub unsafe extern "C" fn web_rust_bindings_wrap(
    vm: *mut core::ffi::c_void,
    realm: *mut core::ffi::c_void,
    interface_name: u16,
    cell: *mut core::ffi::c_void,
) -> *mut core::ffi::c_void {{
    use crate::bindings::CallContext;
    #[allow(unused_imports)]
    use crate::bindings::interface::{{cell_from_abi, cell_if_abi, realm_arguments, wrap}};

    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe {{ realm_arguments(vm, realm) }};
    let cx = CallContext::new(vm, realm);
    let wrapper = match interface_name {{
{arms}        _ => unreachable!("LibWeb's C++ bindings name no other interface that is implemented in Rust"),
    }};
    wrapper.as_ptr().cast()
}}
"""
    )


def check_rust_bindings(context: GenerationContext, usage: DefinitionUsage, intrinsics: List[Interface]) -> None:
    """Raises an error for every use of [ImplementedIn] that the bindings do not handle, before anything is written.
    The functions that write the Rust bindings reject the types and the definitions that they do not handle
    themselves, so the bindings are written once to find those."""
    for module in context.modules:
        interface = module.interface
        if interface is None:
            continue
        implemented_in = interface.extended_attributes.get("ImplementedIn")
        if implemented_in is not None and implemented_in != "Rust":
            raise UnsupportedByRustBindings(
                f"{interface.path}: {interface.name} is [ImplementedIn={implemented_in}], but the only value is Rust"
            )
        if is_implemented_in_rust(interface):
            check_interface_is_supported(interface, module, context)
    write_rust_bindings(StringIO(), context, usage, intrinsics)
