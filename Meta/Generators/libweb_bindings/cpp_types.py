# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from dataclasses import dataclass
from dataclasses import replace
from enum import Enum
from typing import Optional
from typing import Protocol
from typing import Union

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import snake_casify
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import Attribute
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import DictionaryMember
from Utils.webidl_parser import Enumeration
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import Interface
from Utils.webidl_parser import OperationParameter


class ContainedStorageType(Enum):
    Vector = "Vector"
    ConservativeVector = "GC::ConservativeVector"
    RootVector = "GC::RootVector"


class TypeOptionality(Enum):
    Required = "Required"
    OptionalArgument = "OptionalArgument"
    OptionalDictionaryMember = "OptionalDictionaryMember"


@dataclass
class CppType:
    name: str
    contained_storage_type: ContainedStorageType = ContainedStorageType.Vector
    is_nullable: bool = False
    is_optional_presence: bool = False
    gc_ref_target_type: str = ""


@dataclass
class InterfaceLikeType:
    name: str
    fully_qualified_name: str
    implementation_header: str


DictionaryMemberOrAttribute = Union[DictionaryMember, Attribute, OperationParameter]


class IDLNamed(Protocol):
    name: str
    extended_attributes: dict[str, str]


ARRAY_BUFFER_VIEW_TYPES = (
    "Int8Array",
    "Int16Array",
    "Int32Array",
    "Uint8Array",
    "Uint16Array",
    "Uint32Array",
    "Uint8ClampedArray",
    "BigInt64Array",
    "BigUint64Array",
    "Float16Array",
    "Float32Array",
    "Float64Array",
    "DataView",
)

BUFFER_SOURCE_TYPES = ("ArrayBuffer", *ARRAY_BUFFER_VIEW_TYPES)
TYPED_ARRAY_TYPES = tuple(type_name for type_name in ARRAY_BUFFER_VIEW_TYPES if type_name != "DataView")


def cpp_name(member: DictionaryMember) -> str:
    return make_name_acceptable_cpp(title_case_to_snake_case(member.name))


def idl_identifier_cpp_name(identifier: IDLNamed, suffix: Optional[Union[str, int]] = None) -> str:
    cpp_name = make_name_acceptable_cpp(snake_casify(title_case_to_snake_case(identifier.name)))
    if suffix is not None:
        cpp_name = f"{cpp_name}{suffix}"
    return cpp_name


def idl_implementation_cpp_name(identifier: IDLNamed) -> str:
    return identifier.extended_attributes.get("ImplementedAs", idl_identifier_cpp_name(identifier))


def is_optional_without_default(member: DictionaryMemberOrAttribute) -> bool:
    return isinstance(member, DictionaryMember) and not member.required and member.default_value is None


def is_numeric_type(type_name: str) -> bool:
    return type_name in (
        "byte",
        "octet",
        "short",
        "unsigned short",
        "long",
        "unsigned long",
        "long long",
        "unsigned long long",
        "float",
        "unrestricted float",
        "double",
        "unrestricted double",
    )


def is_string_type(type_name: str) -> bool:
    return type_name in ("DOMString", "USVString", "ByteString", "Utf16DOMString", "Utf16USVString")


def cpp_type_name_for_string(type_name: str, extended_attributes: Optional[dict[str, str]] = None) -> str:
    is_fly_string = extended_attributes is not None and "FlyString" in extended_attributes
    is_utf16_string = "Utf16" in type_name
    if is_utf16_string:
        return "Utf16FlyString" if is_fly_string else "Utf16String"
    return "FlyString" if is_fly_string else "String"


def add_include_for_string_cpp_type(cpp_type_name: str, includes: GeneratedIncludes) -> None:
    if cpp_type_name == "FlyString":
        includes.add("AK/FlyString.h")
    elif cpp_type_name == "Utf16FlyString":
        includes.add("AK/Utf16FlyString.h")
    elif cpp_type_name == "Utf16String":
        includes.add("AK/Utf16String.h")
    else:
        includes.add("AK/String.h")


def interface_like_type_for_idl_type(
    idl_type: IDLType,
    context: GenerationContext,
) -> Optional[InterfaceLikeType]:
    if idl_type.name == "WindowProxy":
        return InterfaceLikeType("WindowProxy", "HTML::WindowProxy", "LibWeb/HTML/WindowProxy.h")

    interface = context.interface(idl_type)
    if interface is None or interface.is_callback_interface:
        return None

    return InterfaceLikeType(
        interface.name,
        fully_qualified_name_for_interface(interface),
        implementation_header_for_interface(interface),
    )


def is_buffer_source_type(idl_type: IDLType) -> bool:
    return idl_type.name in BUFFER_SOURCE_TYPES


def is_typed_array_type(idl_type: IDLType) -> bool:
    return idl_type.name in TYPED_ARRAY_TYPES


def add_buffer_source_type_include(idl_type: IDLType, includes: GeneratedIncludes) -> None:
    if idl_type.name == "DataView":
        includes.add("LibJS/Runtime/DataView.h")
    elif idl_type.name == "ArrayBuffer":
        includes.add("LibJS/Runtime/ArrayBuffer.h")
    else:
        includes.add("LibJS/Runtime/TypedArray.h")


def add_include_for_contained_storage_type(
    contained_storage_type: ContainedStorageType,
    includes: GeneratedIncludes,
) -> None:
    if contained_storage_type is ContainedStorageType.ConservativeVector:
        includes.add("LibGC/ConservativeVector.h")
    elif contained_storage_type is ContainedStorageType.RootVector:
        includes.add("LibGC/RootVector.h")
    else:
        includes.add("AK/Vector.h")


def gc_ref_type(referent_type: str) -> CppType:
    return CppType(
        name=f"GC::Ref<{referent_type}>",
        contained_storage_type=ContainedStorageType.RootVector,
        gc_ref_target_type=referent_type,
    )


def gc_ptr_type(referent_type: str) -> CppType:
    return CppType(
        name=f"GC::Ptr<{referent_type}>",
        contained_storage_type=ContainedStorageType.RootVector,
        is_nullable=True,
        gc_ref_target_type=referent_type,
    )


def type_contains_gc_like_value(context: GenerationContext, idl_type: IDLType) -> bool:
    if isinstance(idl_type, IDLUnionType):
        return any(type_contains_gc_like_value(context, member_type) for member_type in idl_type.member_types)

    if isinstance(idl_type, IDLParameterizedType):
        return any(type_contains_gc_like_value(context, parameter) for parameter in idl_type.parameters)

    return (
        context.interface(idl_type) is not None
        or idl_type.name == "WindowProxy"
        or is_buffer_source_type(idl_type)
        or context.callback_function(idl_type) is not None
        or idl_type.name in ("any", "object", "Promise")
    )


def contained_storage_type_for_aggregate_type(context: GenerationContext, idl_type: IDLType) -> ContainedStorageType:
    if type_contains_gc_like_value(context, idl_type):
        return ContainedStorageType.ConservativeVector
    return ContainedStorageType.Vector


def union_type_to_variant(union_type: IDLUnionType, context: GenerationContext) -> str:
    cpp_types = [cpp_type_for_idl_type(member_type, context) for member_type in union_type.flattened_member_types()]

    if union_type.includes_undefined() or union_type.includes_nullable_type():
        cpp_types.append("Empty")

    return f"Variant<{', '.join(cpp_types)}>"


def cpp_value_type(member: DictionaryMemberOrAttribute, context: GenerationContext) -> str:
    optionality = TypeOptionality.OptionalArgument if is_optional_without_default(member) else TypeOptionality.Required
    return cpp_type_for_idl_type(
        member.type,
        context,
        optionality=optionality,
        extended_attributes=getattr(member, "extended_attributes", {}),
    )


def cpp_type_for_non_nullable_idl_type(
    idl_type: IDLType,
    context: GenerationContext,
    extended_attributes: Optional[dict[str, str]] = None,
) -> CppType:
    type_name = idl_type.name

    interface_like_type = interface_like_type_for_idl_type(idl_type, context)
    if interface_like_type is not None:
        return gc_ref_type(interface_like_type.fully_qualified_name)

    interface = context.interface(idl_type)
    if interface is not None:
        return gc_ref_type(fully_qualified_name_for_interface(interface))

    if context.callback_function(idl_type) is not None:
        return gc_ref_type("WebIDL::CallbackType")

    if is_buffer_source_type(idl_type):
        return gc_ref_type(f"JS::{type_name}")

    if type_name == "any":
        return CppType("JS::Value", ContainedStorageType.RootVector)
    if type_name == "boolean":
        return CppType("bool")
    if is_string_type(type_name):
        return CppType(cpp_type_name_for_string(type_name, extended_attributes))
    if type_name in ("double", "unrestricted double"):
        return CppType("double")
    if type_name in ("float", "unrestricted float"):
        return CppType("float")
    if type_name == "undefined":
        return CppType("Empty")
    if type_name == "object":
        return gc_ref_type("JS::Object")
    if type_name == "bigint":
        return gc_ref_type("JS::BigInt")
    if type_name == "byte":
        return CppType("WebIDL::Byte")
    if type_name == "octet":
        return CppType("WebIDL::Octet")
    if type_name == "short":
        return CppType("WebIDL::Short")
    if type_name == "unsigned short":
        return CppType("WebIDL::UnsignedShort")
    if type_name == "long":
        return CppType("WebIDL::Long")
    if type_name == "unsigned long":
        return CppType("WebIDL::UnsignedLong")
    if type_name == "long long":
        return CppType("WebIDL::LongLong")
    if type_name == "unsigned long long":
        return CppType("WebIDL::UnsignedLongLong")

    if isinstance(idl_type, IDLParameterizedType):
        if type_name == "Promise":
            return gc_ref_type("WebIDL::Promise")

        if type_name in ("sequence", "FrozenArray"):
            sequence_cpp_type = cpp_type_for_idl_type_details(idl_type.parameters[0], context)
            storage_type_name = sequence_cpp_type.contained_storage_type.value
            return CppType(
                f"{storage_type_name}<{sequence_cpp_type.name}>",
                sequence_cpp_type.contained_storage_type,
            )

        if type_name == "record":
            key_cpp_type = cpp_type_for_idl_type_details(idl_type.parameters[0], context)
            value_cpp_type = cpp_type_for_idl_type_details(idl_type.parameters[1], context)
            if (
                key_cpp_type.contained_storage_type == ContainedStorageType.ConservativeVector
                or value_cpp_type.contained_storage_type == ContainedStorageType.ConservativeVector
            ):
                return CppType(
                    f"GC::ConservativeHashMap<{key_cpp_type.name}, {value_cpp_type.name}>",
                    ContainedStorageType.ConservativeVector,
                )
            if (
                key_cpp_type.contained_storage_type == ContainedStorageType.RootVector
                or value_cpp_type.contained_storage_type == ContainedStorageType.RootVector
            ):
                return CppType(
                    f"GC::OrderedRootHashMap<{key_cpp_type.name}, {value_cpp_type.name}>",
                    ContainedStorageType.RootVector,
                )
            return CppType(f"OrderedHashMap<{key_cpp_type.name}, {value_cpp_type.name}>")

    if isinstance(idl_type, IDLUnionType):
        cpp_type = CppType(
            union_type_to_variant(idl_type, context), contained_storage_type_for_aggregate_type(context, idl_type)
        )
        cpp_type.is_nullable = idl_type.includes_undefined() or idl_type.includes_nullable_type()
        return cpp_type

    return CppType(type_name)


def with_nullable_cpp_type(cpp_type: CppType) -> CppType:
    if cpp_type.name == "JS::Value":
        return replace(cpp_type, is_nullable=True)

    if cpp_type.gc_ref_target_type:
        return gc_ptr_type(cpp_type.gc_ref_target_type)

    return replace(cpp_type, name=f"Optional<{cpp_type.name}>", is_nullable=True)


def with_optional_cpp_type(cpp_type: CppType) -> CppType:
    if cpp_type.gc_ref_target_type and not cpp_type.is_nullable:
        return replace(
            cpp_type,
            name=f"GC::Ptr<{cpp_type.gc_ref_target_type}>",
            contained_storage_type=ContainedStorageType.RootVector,
            is_nullable=True,
            is_optional_presence=True,
        )

    return replace(
        cpp_type,
        name=f"Optional<{cpp_type.name}>",
        contained_storage_type=ContainedStorageType.Vector,
        is_optional_presence=True,
    )


def cpp_type_for_idl_type_details(
    idl_type: IDLType,
    context: GenerationContext,
    optionality: TypeOptionality = TypeOptionality.Required,
    extended_attributes: Optional[dict[str, str]] = None,
) -> CppType:
    if not idl_type.nullable or isinstance(idl_type, IDLUnionType):
        cpp_type = cpp_type_for_non_nullable_idl_type(idl_type, context, extended_attributes)
    else:
        cpp_type = with_nullable_cpp_type(
            cpp_type_for_non_nullable_idl_type(
                idl_type.clone_with_nullable(False),
                context,
                extended_attributes,
            )
        )

    if optionality is TypeOptionality.Required or (
        optionality is TypeOptionality.OptionalArgument and cpp_type.is_nullable
    ):
        return cpp_type

    return with_optional_cpp_type(cpp_type)


def cpp_type_for_idl_type(
    idl_type: IDLType,
    context: GenerationContext,
    optionality: TypeOptionality = TypeOptionality.Required,
    extended_attributes: Optional[dict[str, str]] = None,
) -> str:
    return cpp_type_for_idl_type_details(idl_type, context, optionality, extended_attributes).name


def cpp_type_details(member: DictionaryMemberOrAttribute, context: GenerationContext) -> CppType:
    optionality = (
        TypeOptionality.OptionalDictionaryMember if is_optional_without_default(member) else TypeOptionality.Required
    )
    return cpp_type_for_idl_type_details(
        member.type,
        context,
        optionality=optionality,
        extended_attributes=getattr(member, "extended_attributes", {}),
    )


def cpp_type(member: DictionaryMemberOrAttribute, context: GenerationContext) -> str:
    return cpp_type_details(member, context).name


def cpp_empty_value(member: DictionaryMember, context: GenerationContext) -> str:
    if cpp_type_details(member, context).gc_ref_target_type and not member.type.nullable:
        return "nullptr"
    return "OptionalNone {}"


def cpp_null_value(idl_type: IDLType, context: GenerationContext) -> str:
    if isinstance(idl_type, IDLUnionType):
        return "Empty {}"
    if cpp_type_for_idl_type_details(idl_type, context).gc_ref_target_type:
        return "nullptr"
    return "OptionalNone {}"


def add_header_includes_for_idl_type(
    idl_type: IDLType,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> None:
    if isinstance(idl_type, IDLUnionType):
        includes.add("AK/Variant.h")
        if idl_type.includes_undefined() or idl_type.includes_nullable_type():
            includes.add("AK/Types.h")
        for member_type in idl_type.flattened_member_types():
            add_header_includes_for_idl_type(member_type.clone_with_nullable(False), includes, context)
        return

    if isinstance(idl_type, IDLParameterizedType):
        if idl_type.name == "Promise":
            includes.add("LibGC/Ptr.h")
            includes.add("LibWeb/Forward.h")
            return
        if idl_type.name in ("sequence", "FrozenArray"):
            cpp_type = cpp_type_for_idl_type_details(idl_type, context)
            add_include_for_contained_storage_type(cpp_type.contained_storage_type, includes)
            for parameter in idl_type.parameters:
                add_header_includes_for_idl_type(parameter, includes, context)
            return
        if idl_type.name == "record":
            cpp_type = cpp_type_for_idl_type_details(idl_type, context)
            if cpp_type.contained_storage_type is ContainedStorageType.ConservativeVector:
                includes.add("LibGC/ConservativeHashMap.h")
            elif cpp_type.contained_storage_type is ContainedStorageType.RootVector:
                includes.add("LibGC/RootHashMap.h")
            else:
                includes.add("AK/HashMap.h")
            for parameter in idl_type.parameters:
                add_header_includes_for_idl_type(parameter, includes, context)
            return

    type_name = idl_type.name
    if type_name == "undefined":
        includes.add("AK/Types.h")
        return
    if type_name == "any":
        includes.add("LibJS/Runtime/Value.h")
        return
    if type_name == "boolean" or type_name in ("float", "unrestricted float", "double", "unrestricted double"):
        return
    if type_name in (
        "byte",
        "octet",
        "short",
        "unsigned short",
        "long",
        "unsigned long",
        "long long",
        "unsigned long long",
    ):
        includes.add("LibWeb/WebIDL/Types.h")
        return
    if is_string_type(type_name):
        add_include_for_string_cpp_type(cpp_type_name_for_string(type_name), includes)
        return

    if context.enumeration(idl_type) is not None:
        add_binding_include_for_type(idl_type, includes, context)
        return

    interface_like_type = interface_like_type_for_idl_type(idl_type, context)
    if interface_like_type is not None:
        includes.add("LibGC/Ptr.h")
        includes.add("LibWeb/Forward.h")
        return

    interface = context.interface(idl_type)
    if interface is not None:
        includes.add("LibGC/Ptr.h")
        includes.add("LibWeb/Forward.h")
        return

    if context.callback_function(idl_type) is not None:
        includes.add("LibGC/Ptr.h")
        includes.add("LibWeb/WebIDL/CallbackType.h")
        return

    if is_buffer_source_type(idl_type):
        includes.add("LibGC/Ptr.h")
        add_buffer_source_type_include(idl_type, includes)
        return

    if cpp_type_for_idl_type(idl_type, context) == type_name:
        add_binding_include_for_type(idl_type, includes, context)


def add_header_includes_for_type(
    member: DictionaryMember,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> None:
    member_cpp_type = cpp_type_details(member, context)
    if member_cpp_type.is_optional_presence or (member_cpp_type.is_nullable and not member_cpp_type.gc_ref_target_type):
        includes.add("AK/Optional.h")
    add_header_includes_for_idl_type(member.type, includes, context)


def libweb_include_path(path) -> str:
    parts = path.parts
    return "/".join(parts[parts.index("LibWeb") :])


def implementation_header_for_interface(interface: Interface) -> str:
    return libweb_include_path(interface.path.with_name(f"{interface.implemented_name}.h"))


def fully_qualified_name_for_interface(interface: Interface) -> str:
    parts = interface.path.parts
    namespace_name = parts[parts.index("LibWeb") + 1]
    return f"{namespace_name}::{interface.implemented_name}"


def converter_function_name(definition: Union[IDLType, Dictionary, Enumeration]) -> str:
    converter_name = make_name_acceptable_cpp(title_case_to_snake_case(definition.name))
    return f"convert_to_idl_value_for_{converter_name}"


def add_binding_include_for_type(idl_type: IDLType, includes: GeneratedIncludes, context: GenerationContext) -> None:
    if includes.is_local_type(idl_type.without_nullable().name):
        return

    dictionary = context.dictionary(idl_type)
    if dictionary is not None:
        includes.add_binding(dictionary.path.stem)
        return

    enumeration = context.enumeration(idl_type)
    if enumeration is not None:
        includes.add_binding(enumeration.path.stem)
        return

    callback_function = context.callback_function(idl_type)
    if callback_function is not None:
        includes.add_binding(callback_function.path.stem)
        return

    interface = context.interface(idl_type)
    if interface is not None:
        includes.add_binding(interface.implemented_name)
        return

    includes.add_binding(idl_type.name)
