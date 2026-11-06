# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import List
from typing import TextIO

from Generators.libweb_bindings.rust_types import is_implemented_in_rust
from Utils.webidl_parser import Interface


def write_structured_serialize_bindings_implementation(out: TextIO, interfaces: List[Interface]) -> None:
    out.write("#include <LibWeb/HTML/StructuredSerialize.h>\n\nnamespace Web::Bindings {\n")
    out.write(
        "bool serializable_interface_is_implemented_in_rust(InterfaceName interface_name)\n{\n    switch (interface_name) {\n"
    )
    for interface in interfaces:
        if "Serializable" in interface.extended_attributes and is_implemented_in_rust(interface):
            out.write(f"    case InterfaceName::{interface.name}:\n")
    out.write("        return true;\n    default:\n        return false;\n    }\n}\n}\n")
