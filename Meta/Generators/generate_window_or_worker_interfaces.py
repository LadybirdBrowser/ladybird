#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


import argparse
import sys

from dataclasses import dataclass
from dataclasses import field
from pathlib import Path
from typing import List
from typing import Optional
from typing import Set
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.webidl_parser import Interface
from Utils.webidl_parser import parse_module

ALL_WORKERS_EXPOSURE = {
    "DedicatedWorker",
    "SharedWorker",
    "ServiceWorker",
    "AudioWorklet",
}

ALL_EXPOSURE = ALL_WORKERS_EXPOSURE | {
    "Window",
    "Worklet",
}


@dataclass
class InterfaceSets:
    intrinsics: List[Interface] = field(default_factory=list)
    window_exposed: List[Interface] = field(default_factory=list)
    dedicated_worker_exposed: List[Interface] = field(default_factory=list)
    shared_worker_exposed: List[Interface] = field(default_factory=list)

    def add_interface(self, interface: Interface) -> None:
        exposed_value = interface.extended_attributes.get("Exposed")
        if exposed_value is None:
            raise RuntimeError(f"Interface {interface.name} is missing extended attribute Exposed")

        exposures = parse_exposure_set(interface.name, exposed_value)

        self.intrinsics.append(interface)

        if "Window" in exposures:
            self.window_exposed.append(interface)

        if "DedicatedWorker" in exposures:
            self.dedicated_worker_exposed.append(interface)

        if "SharedWorker" in exposures:
            self.shared_worker_exposed.append(interface)


@dataclass
class LegacyConstructor:
    name: str
    constructor_class: str


def title_case_to_snake_case(value: str) -> str:
    parts = []
    for index, character in enumerate(value):
        if character.isupper() and index > 0:
            previous_character = value[index - 1]
            next_character = value[index + 1] if index + 1 < len(value) else ""
            if previous_character.islower() or (previous_character.isupper() and next_character.islower()):
                parts.append("_")
        parts.append(character.lower())
    return "".join(parts)


def parse_arguments() -> argparse.Namespace:
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument(
        "-o",
        "--output-path",
        required=True,
        type=Path,
        help="Path to output generated files into",
    )
    argument_parser.add_argument("paths", nargs="+", type=Path, help="Paths of every IDL file that could be Exposed")
    return argument_parser.parse_args()


def read_input_paths(paths: List[Path]) -> List[Path]:
    if len(paths) == 1 and str(paths[0]).startswith("@"):
        response_file_path = Path(str(paths[0])[1:])
        return [Path(path) for path in response_file_path.read_text().splitlines() if path]

    return paths


def parse_exposure_set(interface_name: str, exposed_value: str) -> Set[str]:
    exposed_value = exposed_value.strip()

    if exposed_value == "*":
        return set(ALL_EXPOSURE)
    if exposed_value == "Nobody":
        return set()

    def parse_single_candidate(candidate: str) -> Optional[Set[str]]:
        if candidate == "Window":
            return {"Window"}
        if candidate == "Worker":
            return set(ALL_WORKERS_EXPOSURE)
        if candidate == "DedicatedWorker":
            return {"DedicatedWorker"}
        if candidate == "SharedWorker":
            return {"SharedWorker"}
        if candidate == "ServiceWorker":
            return {"ServiceWorker"}
        if candidate == "AudioWorklet":
            return {"AudioWorklet"}
        if candidate == "LayoutWorklet":
            return {"LayoutWorklet"}
        if candidate == "PaintWorklet":
            return {"PaintWorklet"}
        if candidate == "Worklet":
            return {"Worklet"}
        return None

    if (parsed_candidate := parse_single_candidate(exposed_value)) is not None:
        return parsed_candidate

    if exposed_value.startswith("(") and exposed_value.endswith(")"):
        exposures: Set[str] = set()
        for candidate in exposed_value[1:-1].split(","):
            candidate_name = candidate.strip()
            if (parsed_candidate := parse_single_candidate(candidate_name)) is None:
                raise RuntimeError(
                    f"Unknown Exposed attribute candidate {candidate_name} in {exposed_value!r} in {interface_name}"
                )
            exposures.update(parsed_candidate)

        if exposures:
            return exposures

    raise RuntimeError(f"Unknown Exposed attribute {exposed_value!r} in {interface_name}")


def should_have_interface_object(interface: Interface) -> bool:
    if interface.is_callback_interface:
        return bool(interface.constants)
    return True


def lookup_legacy_constructor(interface: Interface) -> Optional[LegacyConstructor]:
    legacy_factory_function = interface.extended_attributes.get("LegacyFactoryFunction")
    if not legacy_factory_function:
        return None

    legacy_factory_function = legacy_factory_function.lstrip()
    name = []
    for character in legacy_factory_function:
        if character.isspace() or character == "(":
            break
        name.append(character)

    constructor_name = "".join(name)
    return LegacyConstructor(
        name=constructor_name,
        constructor_class=f"{constructor_name}Constructor",
    )


def write_intrinsic_definitions_header(out: TextIO, interface_sets: InterfaceSets) -> None:
    out.write(
        """#pragma once

#include <AK/Types.h>

namespace Web::Bindings {

enum class InterfaceName : u16 {
    Unknown = 0,
"""
    )

    for index, interface in enumerate(interface_sets.intrinsics, start=1):
        out.write(f"    {interface.name} = {index},\n")

    out.write(
        """};

bool is_exposed(InterfaceName, JS::Realm&);

}
"""
    )


def write_intrinsic_definitions_implementation(out: TextIO, interface_sets: InterfaceSets) -> None:
    out.write(
        """#include <LibGC/DeferGC.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
"""
    )

    for interface in interface_sets.intrinsics:
        out.write(f"#include <LibWeb/Bindings/{interface.implemented_name}.h>\n")

        legacy_constructor = lookup_legacy_constructor(interface)
        if legacy_constructor is not None:
            out.write(f"#include <LibWeb/Bindings/{legacy_constructor.constructor_class}.h>\n")

    out.write(
        """
namespace Web::Bindings {
"""
    )

    write_secure_context_switch(out, interface_sets.intrinsics)
    write_experimental_switch(out, interface_sets.intrinsics)
    write_global_exposed_switch(out, "window", interface_sets.window_exposed)
    write_global_exposed_switch(out, "dedicated_worker", interface_sets.dedicated_worker_exposed)
    write_global_exposed_switch(out, "shared_worker", interface_sets.shared_worker_exposed)

    out.write(
        """
// An interface, callback interface, namespace, or member construct is exposed in a given realm realm if the following steps return true:
bool is_exposed(InterfaceName name, JS::Realm& realm)
{
    auto const& global_object = realm.global_object();

    // 1. If construct’s exposure set is not *, and realm.[[GlobalObject]] does not implement an interface that is in construct’s exposure set, then return false.
    if (is<HTML::Window>(global_object)) {
       if (!is_window_exposed(name))
           return false;
    } else if (is<HTML::DedicatedWorkerGlobalScope>(global_object)) {
       if (!is_dedicated_worker_exposed(name))
           return false;
    } else if (is<HTML::SharedWorkerGlobalScope>(global_object)) {
        if (!is_shared_worker_exposed(name))
            return false;
    } else {
        TODO(); // FIXME: ServiceWorkerGlobalScope and WorkletGlobalScope.
    }

    // 2. If realm’s settings object is not a secure context, and construct is conditionally exposed on
    //    [SecureContext], then return false.
    if (is_secure_context_interface(name) && HTML::is_non_secure_context(principal_host_defined_environment_settings_object(realm)))
        return false;

    // AD-HOC: Do not expose experimental interfaces unless instructed to do so.
    if (!HTML::UniversalGlobalScopeMixin::expose_experimental_interfaces() && is_experimental_interface(name))
        return false;

    // FIXME: 3. If realm’s settings object’s cross-origin isolated capability is false, and construct is
    //           conditionally exposed on [CrossOriginIsolated], then return false.

    // 4. Return true.
    return true;
}

"""
    )

    for interface in interface_sets.intrinsics:
        if interface.is_namespace:
            write_namespace_creation(out, interface, interface_sets.intrinsics)
        else:
            write_interface_creation(out, interface)

    out.write(
        """}
"""
    )


def write_secure_context_switch(out: TextIO, interfaces: List[Interface]) -> None:
    out.write(
        """static constexpr bool is_secure_context_interface(InterfaceName name)
{
    switch (name) {
"""
    )
    for interface in interfaces:
        if "SecureContext" not in interface.extended_attributes:
            continue
        out.write(f"    case InterfaceName::{interface.name}:\n")

    out.write(
        """        return true;
    default:
        return false;
    }
}
"""
    )


def write_experimental_switch(out: TextIO, interfaces: List[Interface]) -> None:
    out.write(
        """static constexpr bool is_experimental_interface(InterfaceName name)
{
    switch (name) {
"""
    )
    for interface in interfaces:
        if "Experimental" not in interface.extended_attributes:
            continue
        out.write(f"    case InterfaceName::{interface.name}:\n")

    out.write(
        """        return true;
    default:
        return false;
    }
}
"""
    )


def write_global_exposed_switch(out: TextIO, global_name: str, interfaces: List[Interface]) -> None:
    out.write(
        f"""static constexpr bool is_{global_name}_exposed(InterfaceName name)
{{
    switch (name) {{
"""
    )

    for interface in interfaces:
        out.write(f"    case InterfaceName::{interface.name}:\n")

    out.write(
        """        return true;
    default:
        return false;
    }
}
"""
    )


def write_namespace_creation(out: TextIO, interface: Interface, interfaces: List[Interface]) -> None:
    out.write(
        f"""template<>
void Intrinsics::create_web_namespace<{interface.namespace_class}>(JS::Realm& realm)
{{
    auto namespace_object = realm.create<{interface.namespace_class}>(realm);
    m_namespaces.set("{interface.name}"_fly_string, namespace_object);

    [[maybe_unused]] static constexpr u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;
"""
    )

    for owned_interface in interfaces:
        if owned_interface.extended_attributes.get("LegacyNamespace") != interface.name:
            continue

        out.write(
            f"""    namespace_object->define_intrinsic_accessor("{owned_interface.name}"_utf16_fly_string, attr, [](auto& realm) -> JS::Value {{ return &Bindings::ensure_web_constructor<{owned_interface.prototype_class}>(realm, "{interface.name}.{owned_interface.name}"_fly_string); }});
"""
        )

    out.write(
        """}
"""
    )


def write_interface_creation(out: TextIO, interface: Interface) -> None:
    named_properties_class = ""
    if "Global" in interface.extended_attributes and interface.supports_named_properties():
        named_properties_class = f"{interface.name}Properties"

    out.write(
        f"""template<>
WEB_API void Intrinsics::create_web_prototype_and_constructor<{interface.prototype_class}>(JS::Realm& realm)
{{
    auto& vm = realm.vm();

"""
    )

    if named_properties_class:
        out.write(
            f"""    auto named_properties_object = realm.create<{named_properties_class}>(realm);
    m_prototypes.set("{named_properties_class}"_fly_string, named_properties_object);

"""
        )

    out.write(
        f"""    auto prototype = realm.create<{interface.prototype_class}>(realm);
    m_prototypes.set("{interface.namespaced_name}"_fly_string, prototype);

    auto constructor = realm.create<{interface.constructor_class}>(realm);
    m_constructors.set("{interface.namespaced_name}"_fly_string, constructor);

    prototype->define_direct_property(vm.names.constructor, constructor.ptr(), JS::Attribute::Writable | JS::Attribute::Configurable);
"""
    )

    legacy_constructor = lookup_legacy_constructor(interface)
    if legacy_constructor is not None:
        out.write(
            f"""    auto legacy_constructor = realm.create<{legacy_constructor.constructor_class}>(realm);
    m_constructors.set("{legacy_constructor.name}"_fly_string, legacy_constructor.ptr());
"""
        )

    out.write(
        """}
"""
    )


def write_exposed_interface_header(out: TextIO, class_name: str) -> None:
    snake_name = title_case_to_snake_case(class_name)
    out.write(
        f"""#pragma once

#include <LibJS/Forward.h>

namespace Web::Bindings {{

void add_{snake_name}_exposed_interfaces(JS::Object&);

}}
"""
    )


def write_exposed_interface_implementation(out: TextIO, class_name: str, exposed_interfaces: List[Interface]) -> None:
    snake_name = title_case_to_snake_case(class_name)
    out.write(
        f"""#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/{class_name}ExposedInterfaces.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
"""
    )

    for interface in exposed_interfaces:
        out.write(f"#include <LibWeb/Bindings/{interface.implemented_name}.h>\n")

    out.write(
        f"""
namespace Web::Bindings {{

void add_{snake_name}_exposed_interfaces(JS::Object& global)
{{
    static constexpr u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;

    [[maybe_unused]] bool is_secure_context = HTML::is_secure_context(HTML::relevant_settings_object(global));
    [[maybe_unused]] bool expose_experimental_interfaces = HTML::UniversalGlobalScopeMixin::expose_experimental_interfaces();
"""
    )

    for interface in exposed_interfaces:
        if interface.is_namespace:
            write_namespace_global_accessor(out, interface)
            continue

        if "LegacyNamespace" in interface.extended_attributes:
            continue

        if "LegacyNoInterfaceObject" in interface.extended_attributes:
            continue

        if not should_have_interface_object(interface):
            continue

        write_interface_global_accessor(out, class_name, interface)

    out.write(
        """}

}
"""
    )


def write_interface_global_accessor(out: TextIO, class_name: str, interface: Interface) -> None:
    indentation = "    "

    def write_constructor_accessor(name: str, constructor_name: str) -> None:
        out.write(
            f'{indentation}global.define_intrinsic_accessor("{name}"_utf16_fly_string, attr, [](auto& realm) -> JS::Value {{ return &ensure_web_constructor<{interface.prototype_class}>(realm, "{constructor_name}"_fly_string); }});\n'
        )

    if "SecureContext" in interface.extended_attributes:
        out.write(
            """    if (is_secure_context) {
"""
        )
        indentation += "    "

    if "Experimental" in interface.extended_attributes:
        out.write(
            f"""{indentation}if (expose_experimental_interfaces) {{
"""
        )
        indentation += "    "

    write_constructor_accessor(interface.namespaced_name, interface.namespaced_name)

    if class_name == "Window":
        legacy_window_alias = interface.extended_attributes.get("LegacyWindowAlias")
        if legacy_window_alias:
            if legacy_window_alias.startswith("(") and legacy_window_alias.endswith(")"):
                aliases = [alias.strip() for alias in legacy_window_alias[1:-1].split(",")]
            else:
                aliases = [legacy_window_alias]

            for alias in aliases:
                write_constructor_accessor(alias, interface.namespaced_name)

    legacy_constructor = lookup_legacy_constructor(interface)
    if legacy_constructor is not None:
        write_constructor_accessor(legacy_constructor.name, legacy_constructor.name)

    if "Experimental" in interface.extended_attributes:
        indentation = indentation[:-4]
        out.write(
            f"""{indentation}}}
"""
        )

    if "SecureContext" in interface.extended_attributes:
        indentation = indentation[:-4]
        out.write(
            f"""{indentation}}}
"""
        )


def write_namespace_global_accessor(out: TextIO, interface: Interface) -> None:
    out.write(
        f"""    global.define_intrinsic_accessor("{interface.name}"_utf16_fly_string, attr, [](auto& realm) -> JS::Value {{ return &ensure_web_namespace<{interface.namespace_class}>(realm, "{interface.name}"_fly_string); }});
"""
    )


def write_generated_file(path: Path, writer, *args) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output_file:
        writer(output_file, *args)


def main() -> int:
    arguments = parse_arguments()
    output_directory = arguments.output_path
    output_directory.mkdir(parents=True, exist_ok=True)

    interface_sets = InterfaceSets()

    for path in read_input_paths(arguments.paths):
        interface = parse_module(path, path.read_text(encoding="utf-8")).interface
        if interface is None:
            raise RuntimeError(f"Interface for file {path} missing")
        interface_sets.add_interface(interface)

    write_generated_file(
        output_directory / "IntrinsicDefinitions.h", write_intrinsic_definitions_header, interface_sets
    )
    write_generated_file(
        output_directory / "IntrinsicDefinitions.cpp",
        write_intrinsic_definitions_implementation,
        interface_sets,
    )

    for class_name in ("Window", "DedicatedWorker", "SharedWorker"):
        write_generated_file(
            output_directory / f"{class_name}ExposedInterfaces.h",
            write_exposed_interface_header,
            class_name,
        )

    write_generated_file(
        output_directory / "WindowExposedInterfaces.cpp",
        write_exposed_interface_implementation,
        "Window",
        interface_sets.window_exposed,
    )
    write_generated_file(
        output_directory / "DedicatedWorkerExposedInterfaces.cpp",
        write_exposed_interface_implementation,
        "DedicatedWorker",
        interface_sets.dedicated_worker_exposed,
    )
    write_generated_file(
        output_directory / "SharedWorkerExposedInterfaces.cpp",
        write_exposed_interface_implementation,
        "SharedWorker",
        interface_sets.shared_worker_exposed,
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
