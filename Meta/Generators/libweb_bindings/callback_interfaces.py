# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings.constants import define_the_constants
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.webidl_parser import Interface


def write_callback_interface_declaration(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Interface
) -> None:
    if not interface.constants:
        return

    includes.add("LibJS/Runtime/NativeFunction.h")
    includes.add("LibJS/Runtime/Object.h")

    out.write(f"""struct {interface.constructor_class} {{
public:
    static void initialize(JS::Realm&, JS::NativeFunction&);
}};

struct {interface.prototype_class} {{
public:
    static void initialize(JS::Realm&, JS::Object&);
}};

""")


def write_callback_interface_implementation(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    if not interface.constants:
        return

    includes.add("LibJS/Runtime/Realm.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibJS/Runtime/VM.h")
    includes.add_binding(interface.implemented_name)

    out.write(f"""void {interface.constructor_class}::initialize(JS::Realm& realm, JS::NativeFunction& object)
{{
    auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable;

    object.define_direct_property(vm.names.length, JS::Value(0), JS::Attribute::Configurable);
    object.define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, "{interface.name}"_string), JS::Attribute::Configurable);
""")
    define_the_constants(out, context, includes, interface)
    out.write(
        f"""}}

void {interface.prototype_class}::initialize(JS::Realm& realm, JS::Object& object)
{{
    object.set_prototype(realm.intrinsics().object_prototype());
}}

"""
    )
