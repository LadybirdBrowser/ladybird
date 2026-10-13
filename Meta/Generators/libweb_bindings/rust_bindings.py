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

from io import StringIO
from typing import Dict
from typing import List
from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.overload_resolution import operation_length
from Generators.libweb_bindings.overload_resolution import operation_overload_sets
from Generators.libweb_bindings.overload_resolution import parameter_list_length
from Generators.libweb_bindings.rust_names import rust_identifier
from Generators.libweb_bindings.rust_names import rust_local_name
from Generators.libweb_bindings.rust_to_idl_value import rust_default_value
from Generators.libweb_bindings.rust_to_idl_value import rust_to_idl_value
from Generators.libweb_bindings.rust_to_idl_value import write_dictionary_from_js
from Generators.libweb_bindings.rust_to_idl_value import write_union_from_js
from Generators.libweb_bindings.rust_to_js_value import rust_to_js_value
from Generators.libweb_bindings.rust_to_js_value import write_dictionary_to_js
from Generators.libweb_bindings.rust_to_js_value import write_union_to_js
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Generators.libweb_bindings.rust_types import rust_module_path
from Generators.libweb_bindings.rust_types import rust_parameter_type
from Generators.libweb_bindings.rust_types import rust_type
from Generators.libweb_bindings.rust_usage import DefinitionUsage
from Generators.libweb_bindings.rust_usage import is_implemented_in_rust
from Generators.libweb_bindings.wrappers import wrapper_name
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import Constructor
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Module
from Utils.webidl_parser import OperationParameter

# The extended attributes of an interface and of its members that the Rust bindings handle.
SUPPORTED_INTERFACE_EXTENDED_ATTRIBUTES = {"Exposed", "ImplementedIn", "SecureContext", "Experimental"}
SUPPORTED_MEMBER_EXTENDED_ATTRIBUTES = {"NewObject", "SameObject"}


def rust_module_name(interface: Interface) -> str:
    return title_case_to_snake_case(interface.name)


def rust_entry_point_name(interface: Interface, entry_point: str) -> str:
    return f"web_rust_{rust_module_name(interface)}_{entry_point}"


def wrapper_host_class_symbol(interface: Interface) -> str:
    return f"web_bindings_{wrapper_name(interface)}_host_class"


def implementation_path(interface: Interface) -> str:
    """The Rust path of the implementation: crate::encoding::text_encoder::TextEncoder for Encoding/TextEncoder.idl."""
    return f"{rust_module_path(interface.path)}::{interface.name}"


def interface_name_values(intrinsics: List[Interface]) -> Dict[str, int]:
    """The value of Web::Bindings::InterfaceName for each interface, which the Rust bindings pass the C++ bindings:
    the interfaces in the order of the intrinsics, from 1, as 0 is Unknown."""
    return {interface.name: index for index, interface in enumerate(intrinsics, start=1)}


def check_interface_is_supported(interface: Interface, module: Module, context: GenerationContext) -> None:
    """Raises an error that names what the Rust bindings do not handle yet, for everything that the functions that
    write the bindings do not reject themselves."""

    def fail(message: str) -> None:
        raise UnsupportedByRustBindings(f"{interface.path}: {interface.name} is [ImplementedIn=Rust], but {message}")

    unsupported_attributes = set(interface.extended_attributes) - SUPPORTED_INTERFACE_EXTENDED_ATTRIBUTES
    if unsupported_attributes:
        fail(f"the Rust bindings do not handle its extended attributes {sorted(unsupported_attributes)}")
    if interface.is_namespace or interface.is_callback_interface:
        fail("it is not a regular interface")
    if interface.parent_name:
        fail("it inherits from another interface")
    special_operations = [
        interface.named_property_getter,
        interface.indexed_property_getter,
        interface.named_property_setter,
        interface.indexed_property_setter,
        interface.named_property_deleter,
    ]
    if any(special_operations) or interface.stringifier is not None:
        fail("it has special operations")
    if interface.iterable or interface.async_iterable or interface.maplike or interface.setlike:
        fail("it is iterable or a maplike or setlike")
    if interface.static_attributes or interface.static_operations or interface.constants:
        fail("it has static members or constants")
    if len(interface.constructors) > 1:
        fail("it has more than one constructor")
    for constructor in interface.constructors:
        if constructor.extended_attributes:
            fail(
                "the Rust bindings do not handle the extended attributes "
                f"{sorted(constructor.extended_attributes)} of its constructor"
            )
    if module.enumerations or module.callback_functions:
        fail("its module defines enumerations or callback functions")

    for other in context.interfaces.values():
        if other.parent_name == interface.name:
            fail(f"{other.name} inherits from it")

    def check_member_attributes(member_name: str, extended_attributes: dict) -> None:
        unsupported = set(extended_attributes) - SUPPORTED_MEMBER_EXTENDED_ATTRIBUTES
        if unsupported:
            fail(f"the Rust bindings do not handle the extended attributes {sorted(unsupported)} of {member_name}")

    # NB: operation_overload_sets() leaves out the operations marked [FIXME], which C++ defines as unimplemented.
    for operation in interface.regular_operations:
        check_member_attributes(operation.name, operation.extended_attributes)

    for attribute in interface.regular_attributes:
        check_member_attributes(attribute.name, attribute.extended_attributes)
        if not attribute.readonly:
            fail(f"its attribute {attribute.name} is not readonly")

    for name, operations in operation_overload_sets(interface).items():
        if len(operations) > 1:
            fail(f"its operation {name} is overloaded")


def check_cpp_does_not_name_rust_interfaces(context: GenerationContext, usage: DefinitionUsage) -> None:
    """An interface implemented in Rust has no C++ implementation object, so the C++ bindings cannot convert it."""
    rust_interfaces = {interface.name for interface in context.interfaces.values() if is_implemented_in_rust(interface)}
    if not rust_interfaces:
        return

    def check(path, types) -> None:
        for idl_type in types:
            for nested_type in idl_type.nested_types():
                if nested_type.name in rust_interfaces:
                    raise UnsupportedByRustBindings(
                        f"{path}: uses {nested_type.name}, which is implemented in Rust and has no C++ representation"
                    )

    for module in context.modules:
        if not is_implemented_in_rust(module.interface):
            check(module.path, usage.types_named_by_cpp_module(module))
        for dictionary in usage.cpp_dictionaries(module):
            check(module.path, [member.type for member in dictionary.members])


# C++ side


def write_cpp_declaration(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    includes.add("LibJS/HostObjectABI.h")
    includes.add("LibJS/Runtime/NativeFunction.h")
    includes.add("LibJS/Runtime/Object.h")
    includes.add("LibWeb/Bindings/InterfaceObject.h")
    out.write(
        f"""// The class of the wrappers of {interface.name}, whose implementation is in Rust.
extern "C" JSHostClass const {wrapper_host_class_symbol(interface)};

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


def write_cpp_implementation(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Interface
) -> None:
    includes.add("LibJS/HostClassBuilder.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/HostFunction.h")
    includes.add("LibJS/Runtime/Realm.h")
    includes.add_binding(interface.implemented_name)

    initialize_prototype = rust_entry_point_name(interface, "initialize_prototype")
    initialize_constructor = rust_entry_point_name(interface, "initialize_constructor")
    construct = rust_entry_point_name(interface, "construct")
    out.write(
        f"""extern "C" {{

// The entry points of the Rust bindings of {interface.name}.
void {initialize_prototype}(JSVM*, void* realm, JSObject* prototype);
void {initialize_constructor}(JSVM*, void* realm, JSObject* constructor);
"""
    )
    if interface.constructors:
        out.write(f"JSCompletion {construct}(JSVM*, JSObject* constructor, JSObject* new_target);\n")
    out.write(
        f"""
}}

// The wrappers are platform objects with ordinary internal methods: their implementation is a cell in their host data,
// not an implementation object in their wrappable slot, so they need none of the hooks and flags through which a
// wrapper keeps its implementation object alive, and the engine's own property caches apply to them.
constexpr JSHostClass {wrapper_host_class_symbol(interface)} = JS::make_host_class(JS_HOST_CLASS_OBJECT, "{interface.name}"sv,
    nullptr, nullptr, nullptr, JS_HOST_CLASS_IS_PLATFORM_OBJECT);

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


def write_ensure_web_prototype_declaration(out: TextIO) -> None:
    out.write(
        """
// The interface prototype object of an interface implemented in Rust in the realm, for the Rust bindings.
extern "C" JSObject* web_bindings_ensure_web_prototype(void* realm, u16 interface_name);
"""
    )


def write_ensure_web_prototype_implementation(out: TextIO, intrinsics: List[Interface]) -> None:
    out.write(
        """
JSObject* web_bindings_ensure_web_prototype(void* realm, u16 interface_name)
{
    [[maybe_unused]] auto& js_realm = *static_cast<JS::Realm*>(realm);
    switch (static_cast<InterfaceName>(interface_name)) {
"""
    )
    for interface in intrinsics:
        if not is_implemented_in_rust(interface):
            continue
        out.write(
            f"""    case InterfaceName::{interface.name}:
        return JS::HostABI::object_to_abi(&ensure_web_prototype<{interface.prototype_class}>(js_realm, "{interface.namespaced_name}"_utf16_fly_string));
"""
        )
    out.write(
        """    default:
        VERIFY_NOT_REACHED();
    }
}
"""
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
        unsupported = set(parameter.extended_attributes) - {"AllowShared", "AllowResizable"}
        if unsupported:
            raise UnsupportedByRustBindings(f"{parameter_what} has the extended attributes {sorted(unsupported)}")
        name = rust_local_name(parameter.name)
        conversion = rust_to_idl_value(
            parameter.type, parameter.extended_attributes, f"argument{index}", parameter.name, context, parameter_what
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


def implementation_method_signatures(context: GenerationContext, interface: Interface) -> List[str]:
    """The methods the implementation of the interface defines, which the bindings call."""

    def parameter_list(what: str, parameters: List[OperationParameter]) -> str:
        return "".join(
            f", {rust_identifier(parameter.name)}: {rust_parameter_type(parameter, context, what)}"
            for parameter in parameters
        )

    signatures = []
    for constructor in interface.constructors:
        parameters = parameter_list(constructor_description(interface), constructor.parameters)
        signatures.append(f"fn construct(cx: &mut CallContext{parameters}) -> WebIdlResult<Self>")
    for attribute in interface.regular_attributes:
        what = f"{interface.name}.{attribute.name}"
        signatures.append(
            f"fn {rust_identifier(attribute.name)}(&self, cx: &mut CallContext) -> "
            f"WebIdlResult<{rust_type(attribute.type, context, what)}>"
        )
    for name, operations in operation_overload_sets(interface).items():
        operation = operations[0]
        what = f"{interface.name}.{name}()"
        parameters = parameter_list(what, operation.parameters)
        signatures.append(
            f"fn {rust_identifier(name)}(&self, cx: &mut CallContext{parameters}) -> "
            f"WebIdlResult<{rust_type(operation.return_type, context, what)}>"
        )
    return signatures


def write_rust_interface(
    out: TextIO, context: GenerationContext, module: Module, interface: Interface, interface_name_value: int
) -> None:
    implementation = implementation_path(interface)
    method_documentation = "".join(
        f"/// - `{signature}`\n" for signature in implementation_method_signatures(context, interface)
    )
    out.write(
        f"""
/// The bindings of {interface.name}, which call these methods of its implementation, {implementation}:
{method_documentation}pub(crate) mod {rust_module_name(interface)} {{
    #![allow(unused_imports)]

    use core::ffi::c_void;

    use ak::Utf16String;
    use libjs_rust::api::{{
        EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType, Gc, JSHostClass, Object,
        RawNativeFunctionResult, ThrowCompletionOr, TypedArrayBase, TypedArrayKind, Value, Vm, raw_native,
    }};

    use crate::bindings::conversions::{{
        BufferSource, BufferSourceRules, EmptyDictionary, FromJs, ToJs, UsvString, record_to_js, sequence_to_js,
        to_boolean, to_buffer_source, to_dom_string, to_float, to_typed_array, to_usv_string,
    }};
    use crate::bindings::interface::{{
        construct_arguments, create_wrapper, define_attribute, define_interface_object_properties, define_operation,
        define_to_string_tag, entry_point_arguments, implementation_of, prototype_from_new_target,
        set_interface_prototype_object_prototype, this_object,
    }};
    use crate::bindings::{{CallContext, WebIdlResult}};

    use {implementation} as Implementation;

    /// Web::Bindings::InterfaceName::{interface.name}.
    pub(crate) const INTERFACE_NAME: u16 = {interface_name_value};

    /// The class of the cells that hold the implementations of the interface.
    static CELL_CLASS: EmbedderClass<Implementation> = EmbedderClass::new("{interface.name}");

    impl EmbedderValue for Implementation {{
        const CLASS: &'static EmbedderClass<Self> = &CELL_CLASS;
    }}

    unsafe extern "C" {{
        static {wrapper_host_class_symbol(interface)}: JSHostClass;
    }}

    fn wrapper_class() -> &'static JSHostClass {{
        // SAFETY: LibWeb defines the class of the wrappers as constant data.
        unsafe {{ &{wrapper_host_class_symbol(interface)} }}
    }}

    /// The implementation that an attribute or an operation runs on, and the context it runs in.
    fn this_implementation(vm: &Vm) -> ThrowCompletionOr<(Gc<EmbedderCell<Implementation>>, CallContext<'_>)> {{
        let (object, realm) = this_object(vm, wrapper_class(), "{interface.namespaced_name}")?;
        Ok((implementation_of(object, &CELL_CLASS), CallContext::new(vm, realm)))
    }}
"""
    )

    for attribute in interface.regular_attributes:
        getter = f"{rust_identifier(attribute.name).removeprefix('r#')}_getter"
        conversion = rust_to_js_value(attribute.type, "result", "&cx", context, f"{interface.name}.{attribute.name}")
        out.write(
            f"""
    fn {getter}(vm: &Vm) -> ThrowCompletionOr<Value> {{
        let (implementation, mut cx) = this_implementation(vm)?;
        let result = Implementation::{rust_identifier(attribute.name)}(&implementation, &mut cx)
            .map_err(|exception| exception.throw(vm))?;
        {conversion}
    }}
"""
        )

    for name, operations in operation_overload_sets(interface).items():
        operation = operations[0]
        function = rust_identifier(name)
        out.write(
            f"""
    fn {function.removeprefix("r#")}_function(vm: &Vm) -> ThrowCompletionOr<Value> {{
        let (implementation, mut cx) = this_implementation(vm)?;
"""
        )
        write_argument_count_check(out, name, operation_length(operation))
        if operation.parameters:
            out.write("        let type_error_realm_scope = vm.type_error_realm_scope(cx.realm());\n")
            write_parameter_conversions(out, f"{interface.name}.{name}()", operation.parameters, context)
            out.write("        drop(type_error_realm_scope);\n")
        arguments = "".join(f", {rust_local_name(parameter.name)}" for parameter in operation.parameters)
        conversion = rust_to_js_value(operation.return_type, "result", "&cx", context, f"{interface.name}.{name}()")
        out.write(
            f"""        let result = Implementation::{function}(&implementation, &mut cx{arguments})
            .map_err(|exception| exception.throw(vm))?;
        {conversion}
    }}
"""
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
        set_interface_prototype_object_prototype(vm, realm, prototype);
"""
    )
    for attribute in interface.regular_attributes:
        getter = f"{rust_identifier(attribute.name).removeprefix('r#')}_getter"
        out.write(f'        define_attribute(vm, realm, prototype, "{attribute.name}", raw_native!({getter}), None);\n')
    for name, operations in operation_overload_sets(interface).items():
        function = f"{rust_identifier(name).removeprefix('r#')}_function"
        length = operation_length(operations[0])
        out.write(f'        define_operation(vm, realm, prototype, "{name}", raw_native!({function}), {length});\n')
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
        define_interface_object_properties(vm, realm, constructor, "{interface.name}", {constructor_length(interface)}, INTERFACE_NAME);
    }}
"""
    )

    for constructor in interface.constructors:
        write_rust_constructor(out, context, interface, constructor)
    if interface.constructors:
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
    out.write("}\n")


def write_rust_constructor(
    out: TextIO, context: GenerationContext, interface: Interface, constructor: Constructor
) -> None:
    """The steps of a constructor in the order of the C++ bindings: the prototype of the new object is read from the
    new target before the arguments are counted and converted, and the conversions throw in the current realm."""
    out.write(
        """
    /// The constructor steps of the interface, which internally create a new object implementing the interface,
    /// https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
    fn construct(vm: &Vm, new_target: Gc<Object>) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a construction runs in a realm");
        let prototype = prototype_from_new_target(vm, new_target, INTERFACE_NAME)?;
"""
    )
    write_argument_count_check(out, interface.name, parameter_list_length(constructor.parameters))
    write_parameter_conversions(out, constructor_description(interface), constructor.parameters, context)
    arguments = "".join(f", {rust_local_name(parameter.name)}" for parameter in constructor.parameters)
    out.write(
        f"""        let mut cx = CallContext::new(vm, realm);
        let implementation =
            Implementation::construct(&mut cx{arguments}).map_err(|exception| exception.throw(vm))?;
        let implementation = CELL_CLASS.allocate(vm, implementation);
        Ok(Value::from_object(create_wrapper(vm, realm, wrapper_class(), prototype, implementation)))
    }}
"""
    )


def constructor_length(interface: Interface) -> int:
    if not interface.constructors:
        return 0
    return min(parameter_list_length(constructor.parameters) for constructor in interface.constructors)


def write_rust_definitions(out: TextIO, context: GenerationContext, usage: DefinitionUsage) -> None:
    """The conversions of the dictionaries and unions that the interfaces implemented in Rust name, each written once,
    whichever interfaces name it."""
    if not usage.rust_dictionaries and not usage.rust_unions:
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
    };

    use crate::bindings::CallContext;
    use crate::bindings::conversions::{
        BufferSource, BufferSourceRules, EmptyDictionary, FromJs, ToJs, UsvString, dictionary_member,
        dictionary_object, record_to_js, sequence_to_js, to_boolean, to_buffer_source, to_dom_string, to_float,
        to_typed_array, to_usv_string,
    };
    use crate::bindings::interface::{create_dictionary_object, set_dictionary_member};
"""
    )
    for name, directions in usage.rust_dictionaries.items():
        dictionary = context.dictionaries[name]
        if dictionary.parent_name:
            raise UnsupportedByRustBindings(
                f"The Rust bindings convert the dictionary {name}, which inherits from another dictionary"
            )
        if directions.from_js:
            write_dictionary_from_js(out, context, dictionary)
        if directions.to_js:
            write_dictionary_to_js(out, context, dictionary)
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
    write_rust_definitions(out, context, usage)
    values = interface_name_values(intrinsics)
    for module in context.modules:
        interface = module.interface
        if interface is None or not is_implemented_in_rust(interface):
            continue
        write_rust_interface(out, context, module, interface, values[interface.name])


def check_rust_bindings(context: GenerationContext, usage: DefinitionUsage, intrinsics: List[Interface]) -> None:
    """Raises an error for every use of [ImplementedIn] that the bindings do not handle, before anything is written.
    The functions that write the Rust bindings reject the types and the definitions that they do not handle
    themselves, so the bindings are written once to find those."""
    check_cpp_does_not_name_rust_interfaces(context, usage)
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
