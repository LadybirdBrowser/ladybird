# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Dict
from typing import List
from typing import TextIO

from Generators.libweb_bindings.rust_types import is_implemented_in_rust
from Generators.libweb_bindings.rust_types import rust_interface_path
from Utils.webidl_parser import Interface


def write_rust_serializable_dispatch(out: TextIO, interfaces: List[Interface], values: Dict[str, int]) -> None:
    serializable = sorted(
        (interface for interface in interfaces if "Serializable" in interface.extended_attributes),
        key=lambda interface: interface.name,
    )
    rust_interfaces = [interface for interface in serializable if is_implemented_in_rust(interface)]
    cpp_interfaces = [interface for interface in serializable if not is_implemented_in_rust(interface)]
    out.write(
        """
pub(crate) mod serializable_objects {
    use libjs_rust::api::{Gc, Object, Value, Vm};
    use crate::bindings::{CallContext, WebIdlResult};
    use crate::bindings::interface::is_exact_wrapper;
    use crate::bindings::structured_data::{self, SerializationReader, SerializationWriter};

    const CPP_INTERFACES: &[u16] = &["""
        + ", ".join(str(values[interface.name]) for interface in cpp_interfaces)
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
        vm: &Vm,
        object: Gc<Object>,
        serialized: &mut SerializationWriter,
        for_storage: bool,
    ) -> WebIdlResult<()> {
        match interface_name {
"""
    )
    for interface in rust_interfaces:
        out.write(
            f"            {values[interface.name]} => structured_data::serialization_steps::<{rust_interface_path(interface)}>(vm, object, serialized, for_storage),\n"
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
}
"""
    )
