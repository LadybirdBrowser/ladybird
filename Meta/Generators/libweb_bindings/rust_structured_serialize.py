# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Dict
from typing import List
from typing import TextIO

from Generators.libweb_bindings.rust_types import is_implemented_in_rust
from Generators.libweb_bindings.rust_types import rust_interface_path
from Utils.webidl_parser import Interface

# These strings are persisted in storage records. IDL decoration alone does not mean the wire format supports an interface.
STORAGE_IDENTIFIERS = (
    "Blob",
    "CryptoKey",
    "DOMException",
    "DOMMatrix",
    "DOMMatrixReadOnly",
    "DOMPoint",
    "DOMPointReadOnly",
    "DOMQuad",
    "DOMRect",
    "DOMRectReadOnly",
    "File",
    "FileList",
    "ImageBitmap",
    "ImageData",
    "Module",
    "QuotaExceededError",
)

# The serializable interfaces implemented in C++ whose steps run in Rust.
CPP_INTERFACES_WITH_RUST_STEPS = ("DOMException", "QuotaExceededError", "RTCEncodedAudioFrame")


def write_rust_serializable_dispatch(out: TextIO, interfaces: List[Interface], values: Dict[str, int]) -> None:
    serializable = sorted(
        (interface for interface in interfaces if "Serializable" in interface.extended_attributes),
        key=lambda interface: interface.name,
    )
    rust_interfaces = [interface for interface in serializable if is_implemented_in_rust(interface)]
    out.write(
        """
pub(crate) mod serializable_objects {
    use libjs_rust::api::{Gc, Object, Value};
    use crate::bindings::{CallContext, WebIdlResult};
    use crate::bindings::interface::is_exact_wrapper;
    use crate::bindings::structured_data::{self, SerializationReader, SerializationWriter};
    use crate::html::structured_serialize::serializable::{SerializableRegistryEntry, SerializableSteps};

    const CPP_INTERFACES: &[u16] = &["""
        + ", ".join(str(values[name]) for name in CPP_INTERFACES_WITH_RUST_STEPS)
        + """];

    pub(crate) fn rust_interface_name(object: Gc<Object>) -> Option<u16> {
"""
    )
    for interface in rust_interfaces:
        out.write(
            f"""        if is_exact_wrapper::<{rust_interface_path(interface)}>(object) {{
            return Some({values[interface.name]});
        }}
"""
        )
    out.write(
        """        None
    }

    pub(crate) fn is_cpp_serializable_interface(interface_name: u16) -> bool {
        CPP_INTERFACES.contains(&interface_name)
    }

    pub(crate) fn serialization_steps(
        interface_name: u16,
        cx: &CallContext,
        object: Gc<Object>,
        serialized: &mut SerializationWriter,
        for_storage: bool,
    ) -> WebIdlResult<()> {
        match interface_name {
"""
    )
    for interface in rust_interfaces:
        out.write(
            f"            {values[interface.name]} => structured_data::serialization_steps::<{rust_interface_path(interface)}>(cx, object, serialized, for_storage),\n"
        )
    out.write(
        """            _ => unreachable!("the interface is a Rust serializable interface"),
        }
    }

    pub(crate) fn deserialization_steps(
        interface_name: u16,
        cx: &CallContext,
        serialized: &mut SerializationReader,
    ) -> WebIdlResult<Value> {
        match interface_name {
"""
    )
    for interface in rust_interfaces:
        out.write(
            f"            {values[interface.name]} => structured_data::deserialization_steps::<{rust_interface_path(interface)}>(cx, serialized),\n"
        )
    out.write(
        """            _ => unreachable!("the interface is a Rust serializable interface"),
        }
    }

    pub(crate) const STORAGE_INTERFACES: &[SerializableRegistryEntry] = &[
"""
    )
    for name in STORAGE_IDENTIFIERS:
        out.write(f'        SerializableRegistryEntry {{ interface_name: {values[name]}, identifier: b"{name}" }},\n')
    out.write(
        f"""    ];

    pub(crate) fn serializable_interface_storage_identifier(interface_name: u16) -> Option<&'static [u8]> {{
        STORAGE_INTERFACES.iter().find(|entry| entry.interface_name == interface_name).map(|entry| entry.identifier)
    }}

    pub(crate) fn serializable_interface_name_from_storage_identifier(identifier: &[u8]) -> Option<u16> {{
        STORAGE_INTERFACES.iter().find(|entry| entry.identifier == identifier).map(|entry| entry.interface_name)
    }}

    pub(crate) fn is_ipc_interface(interface_name: u16) -> bool {{
        interface_name == {values["RTCEncodedAudioFrame"]} || serializable_interface_storage_identifier(interface_name).is_some()
    }}

    pub(crate) fn serializable_steps(interface_name: u16) -> SerializableSteps {{
        match interface_name {{
"""
    )
    for name in CPP_INTERFACES_WITH_RUST_STEPS:
        out.write(f"            {values[name]} => SerializableSteps::{name},\n")
    for interface in rust_interfaces:
        out.write(f"            {values[interface.name]} => SerializableSteps::Rust,\n")
    out.write(
        """            _ => unreachable!("the interface has serialization steps"),
        }
    }
}
"""
    )
