# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import List
from typing import TextIO

from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Utils.webidl_parser import Interface

SUPPORTED_TRANSFER_TYPES = ("ImageBitmap", "MessagePort", "ReadableStream", "TransformStream", "WritableStream")


def write_structured_serialize_bindings_implementation(out: TextIO, interfaces: List[Interface]) -> None:
    serializable = [interface for interface in interfaces if "Serializable" in interface.extended_attributes]
    transferable = [
        interface
        for interface in interfaces
        if "Transferable" in interface.extended_attributes and interface.name in SUPPORTED_TRANSFER_TYPES
    ]
    included_interfaces = sorted(
        {interface.name: interface for interface in serializable + transferable}.values(),
        key=lambda interface: interface.name,
    )

    out.write(
        """#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
"""
    )
    for interface in included_interfaces:
        out.write(f"#include <LibWeb/Bindings/{interface.implemented_name}.h>\n")
        out.write(f"#include <{implementation_header_for_interface(interface)}>\n")

    out.write(
        """
namespace Web::Bindings {

bool is_platform_object(JS::Object const& object)
{
    return is<Bindings::PlatformObject>(object);
}

Transferable* transferable_from_object(JS::Object& object)
{
    auto* platform_object = as_if<Bindings::PlatformObject>(object);
    if (!platform_object)
        return nullptr;

    auto* wrappable = Bindings::wrappable_impl_from(platform_object);
    if (!wrappable)
        return nullptr;

    return as_if<Transferable>(*wrappable);
}

Optional<SerializablePlatformObject> serializable_from_object(JS::Object& object)
{
    auto* platform_object = as_if<Bindings::PlatformObject>(object);
    if (!platform_object)
        return {};

    auto* wrappable = Bindings::wrappable_impl_from(platform_object);
    if (!wrappable)
        return {};

    auto* serializable = as_if<Serializable>(*wrappable);
    if (!serializable)
        return {};

    return SerializablePlatformObject { serializable, platform_object->interface_name(), &platform_object->realm() };
}

GC::Ref<PlatformObject> create_serialized_platform_object(InterfaceName serialize_type, JS::Realm& realm)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);

    switch (serialize_type) {
"""
    )
    for interface in serializable:
        out.write(f"    case InterfaceName::{interface.name}:\n")
        out.write(
            f"        return Bindings::wrap(wrapper_world, realm, {fully_qualified_name_for_interface(interface)}::create());\n"
        )

    out.write(
        """    case InterfaceName::Unknown:
    default:
        VERIFY_NOT_REACHED();
    }
}

WebIDL::ExceptionOr<GC::Ref<PlatformObject>> create_transferred_platform_object(HTML::TransferType name, JS::Realm& target_realm, HTML::TransferDataDecoder& decoder)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(target_realm);

    switch (name) {
"""
    )
    for interface in transferable:
        out.write(f"    case HTML::TransferType::{interface.name}: {{\n")
        if interface.name == "MessagePort":
            out.write("""        auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(target_realm.global_object());
        VERIFY(global_scope);
        auto transferable = HTML::MessagePort::create(global_scope->this_impl());
""")
        elif interface.name in ("ReadableStream", "TransformStream", "WritableStream"):
            out.write(
                f"        auto transferable = GC::Heap::the().allocate<{fully_qualified_name_for_interface(interface)}>();\n"
            )
        else:
            out.write(f"        auto transferable = {fully_qualified_name_for_interface(interface)}::create();\n")
        out.write("""        TRY(transferable->transfer_receiving_steps(target_realm, decoder));
        return Bindings::wrap(wrapper_world, target_realm, transferable);
    }
""")

    out.write(
        """    case HTML::TransferType::ArrayBuffer:
    case HTML::TransferType::ResizableArrayBuffer:
    case HTML::TransferType::Unknown:
        break;
    }
    VERIFY_NOT_REACHED();
}

}
"""
    )
