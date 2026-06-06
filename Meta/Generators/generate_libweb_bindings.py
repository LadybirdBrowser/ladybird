#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


import argparse
import sys

from io import StringIO
from pathlib import Path
from typing import Dict
from typing import List
from typing import Set
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Generators.libweb_bindings import interfaces
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.intrinsics import collect_interface_sets
from Generators.libweb_bindings.intrinsics import write_exposed_interface_header
from Generators.libweb_bindings.intrinsics import write_exposed_interface_implementation
from Generators.libweb_bindings.intrinsics import write_intrinsic_definitions_header
from Generators.libweb_bindings.intrinsics import write_intrinsic_definitions_implementation
from Generators.libweb_bindings.to_idl_value import dictionaries_in_dependency_order
from Generators.libweb_bindings.to_idl_value import write_dictionary_conversion
from Generators.libweb_bindings.to_idl_value import write_dictionary_declaration
from Generators.libweb_bindings.to_idl_value import write_enumeration_conversion
from Generators.libweb_bindings.to_idl_value import write_enumeration_declaration
from Generators.libweb_bindings.to_js_value import write_dictionary_to_javascript_value_conversion
from Generators.libweb_bindings.to_js_value import write_dictionary_to_javascript_value_declaration
from Generators.libweb_bindings.to_js_value import write_enumeration_to_javascript_value_conversion
from Generators.libweb_bindings.to_js_value import write_enumeration_to_javascript_value_declaration
from Utils.webidl_parser import Module
from Utils.webidl_parser import parse_module


def parse_arguments() -> argparse.Namespace:
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument(
        "-o",
        "--output-path",
        required=True,
        type=Path,
        help="Path to output generated files into",
    )
    argument_parser.add_argument(
        "-d",
        "--depfile",
        type=Path,
        help="Path to write dependency file to",
    )
    argument_parser.add_argument("paths", nargs="+", type=Path, help="Paths of every IDL file that could be Exposed")
    return argument_parser.parse_args()


def read_input_paths(paths: List[Path]) -> List[Path]:
    if len(paths) == 1 and str(paths[0]).startswith("@"):
        response_file_path = Path(str(paths[0])[1:])
        return [Path(path) for path in response_file_path.read_text().splitlines() if path]

    return paths


def cpp_namespace_for_module_path(path: Path) -> str:
    """A path of Libraries/LibWeb/<namespace>/... should have a namespace of Web::<namespace>."""
    parts = path.parts
    return parts[parts.index("LibWeb") + 1]


def local_type_names(module: Module) -> set[str]:
    local_types = {enumeration.name for enumeration in module.enumerations}
    local_types.update(dictionary.name for dictionary in module.dictionaries)
    if module.interface is not None:
        local_types.add(module.interface.name)
    return local_types


def write_idl_header(out: TextIO, module: Module, context: GenerationContext) -> None:
    includes = GeneratedIncludes(local_type_names(module))
    body = StringIO()

    interfaces.write_declaration(body, includes, context, module.interface)

    for enumeration in module.enumerations:
        write_enumeration_declaration(body, enumeration, includes)
        write_enumeration_to_javascript_value_declaration(body, enumeration, includes)

    for dictionary in dictionaries_in_dependency_order(module.dictionaries, context):
        write_dictionary_declaration(body, dictionary, includes, context)
        write_dictionary_to_javascript_value_declaration(body, dictionary)

    out.write("#pragma once\n\n")
    includes.write(out)
    out.write("namespace Web::Bindings {\n\n")
    out.write(body.getvalue())
    out.write("} // namespace Web::Bindings\n")


def write_idl_implementation(out: TextIO, module: Module, context: GenerationContext) -> None:
    includes = GeneratedIncludes(local_type_names(module))
    includes.add_binding(module.path.stem)
    body = StringIO()

    interfaces.write_implementation(body, includes, context, module.interface)

    for enumeration in module.enumerations:
        write_enumeration_conversion(body, enumeration, includes)
        write_enumeration_to_javascript_value_conversion(body, enumeration)

    for dictionary in module.dictionaries:
        write_dictionary_conversion(body, dictionary, includes, context)
        write_dictionary_to_javascript_value_conversion(body, dictionary, includes, context)

    includes.write(out)
    out.write("namespace Web::Bindings {\n\n")
    out.write(body.getvalue())
    out.write("} // namespace Web::Bindings\n")


def write_forward_header(out: TextIO, modules: List[Module]) -> None:
    out.write(
        """#pragma once

"""
    )

    interface_names_by_namespace: Dict[str, Set[str]] = {}

    for module in modules:
        interface = module.interface
        if interface is None or interface.is_namespace:
            continue

        namespace_name = cpp_namespace_for_module_path(interface.path)
        if not namespace_name:
            continue

        interface_names_by_namespace.setdefault(namespace_name, set()).add(interface.implemented_name)

    for namespace_name in sorted(interface_names_by_namespace):
        out.write(f"namespace Web::{namespace_name} {{\n\n")

        for class_name in sorted(interface_names_by_namespace[namespace_name]):
            out.write(f"class {class_name};\n")

        out.write(
            """
}

"""
        )

    dictionary_names = {dictionary.name for module in modules for dictionary in module.dictionaries}

    out.write(
        """namespace Web::Bindings {

"""
    )

    for dictionary_name in sorted(dictionary_names):
        out.write(f"struct {dictionary_name};\n")

    out.write(
        """
}
"""
    )


def write_generated_file(path: Path, writer, *args) -> None:
    output_file = StringIO()
    writer(output_file, *args)
    generated_contents = output_file.getvalue()

    if path.exists() and path.read_text(encoding="utf-8") == generated_contents:
        return

    with path.open("w", encoding="utf-8", newline="\n") as output_file:
        output_file.write(generated_contents)


def generate_depfile(depfile_path: Path, dependency_paths: List[Path], output_files: List[Path]) -> None:
    depfile_path.parent.mkdir(parents=True, exist_ok=True)

    depfile_contents = " ".join(str(output_file) for output_file in output_files)
    depfile_contents += ":"
    for dependency_path in dependency_paths:
        depfile_contents += f" \\\n {dependency_path}"
    depfile_contents += "\n"

    depfile_path.write_text(depfile_contents, encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    output_directory = arguments.output_path
    output_directory.mkdir(parents=True, exist_ok=True)

    dependency_paths: List[Path] = []
    modules: List[Module] = []

    for path in read_input_paths(arguments.paths):
        module = parse_module(path, path.read_text(encoding="utf-8"))
        modules.append(module)
        dependency_paths.append(path)

    context = GenerationContext(modules)
    modules = context.modules

    output_files: List[Path] = []

    for module in modules:
        path = module.path
        header_path = output_directory / f"{path.stem}.h"
        implementation_path = output_directory / f"{path.stem}.cpp"

        write_generated_file(header_path, write_idl_header, module, context)
        write_generated_file(implementation_path, write_idl_implementation, module, context)

        output_files.append(header_path)
        output_files.append(implementation_path)

    interface_sets = collect_interface_sets(modules)

    intrinsic_definitions_header_path = output_directory / "IntrinsicDefinitions.h"
    intrinsic_definitions_implementation_path = output_directory / "IntrinsicDefinitions.cpp"

    write_generated_file(intrinsic_definitions_header_path, write_intrinsic_definitions_header, interface_sets)
    write_generated_file(
        intrinsic_definitions_implementation_path, write_intrinsic_definitions_implementation, interface_sets
    )
    output_files.extend([intrinsic_definitions_header_path, intrinsic_definitions_implementation_path])

    for class_name in ("Window", "DedicatedWorker", "SharedWorker"):
        exposed_interface_header_path = output_directory / f"{class_name}ExposedInterfaces.h"
        write_generated_file(exposed_interface_header_path, write_exposed_interface_header, class_name)
        output_files.append(exposed_interface_header_path)

    exposed_interface_implementations = [
        ("Window", interface_sets.window_exposed),
        ("DedicatedWorker", interface_sets.dedicated_worker_exposed),
        ("SharedWorker", interface_sets.shared_worker_exposed),
    ]
    for class_name, exposed_interfaces in exposed_interface_implementations:
        exposed_interface_implementation_path = output_directory / f"{class_name}ExposedInterfaces.cpp"
        write_generated_file(
            exposed_interface_implementation_path,
            write_exposed_interface_implementation,
            class_name,
            exposed_interfaces,
        )
        output_files.append(exposed_interface_implementation_path)

    forward_header_path = output_directory / "Forward.h"
    write_generated_file(forward_header_path, write_forward_header, modules)
    output_files.append(forward_header_path)

    if arguments.depfile is not None:
        generate_depfile(arguments.depfile, dependency_paths, output_files)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
