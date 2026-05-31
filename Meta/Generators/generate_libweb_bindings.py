#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


import argparse
import subprocess
import sys

from io import StringIO
from pathlib import Path
from typing import Dict
from typing import List
from typing import Set
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Generators.libweb_bindings.intrinsics import collect_interface_sets
from Generators.libweb_bindings.intrinsics import write_exposed_interface_header
from Generators.libweb_bindings.intrinsics import write_exposed_interface_implementation
from Generators.libweb_bindings.intrinsics import write_intrinsic_definitions_header
from Generators.libweb_bindings.intrinsics import write_intrinsic_definitions_implementation
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
        "--depfile",
        type=Path,
        help="Path to write the per-IDL bindings dependency file to",
    )
    argument_parser.add_argument(
        "--cxx-generator",
        type=Path,
        help="Path to the existing C++ bindings generator",
    )
    argument_parser.add_argument(
        "-i",
        "--header-include-path",
        action="append",
        default=[],
        type=Path,
        help="Header search path to pass to the C++ bindings generator",
    )
    argument_parser.add_argument(
        "--python-generated-idl-list",
        type=Path,
        help="Path to a newline-separated list of IDL files opted in to the Python generator",
    )
    argument_parser.add_argument("paths", nargs="+", type=Path, help="Paths of every IDL file that could be Exposed")
    return argument_parser.parse_args()


def read_input_paths(paths: List[Path]) -> List[Path]:
    if len(paths) == 1 and str(paths[0]).startswith("@"):
        response_file_path = Path(str(paths[0])[1:])
        return [Path(path) for path in response_file_path.read_text().splitlines() if path]

    return paths


def read_path_list(path: Path) -> List[str]:
    if not path.exists():
        return []
    return [line for line in path.read_text(encoding="utf-8").splitlines() if line]


def cpp_namespace_for_module_path(path: Path) -> str:
    """A path of Libraries/LibWeb/<namespace>/... should have a namespace of Web::<namespace>."""
    parts = path.parts
    return parts[parts.index("LibWeb") + 1]


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


def run_cxx_generator(arguments: argparse.Namespace) -> int:
    if arguments.cxx_generator is None:
        return 0

    python_generated_idl_files: List[str] = []
    if arguments.python_generated_idl_list is not None:
        python_generated_idl_files = read_path_list(arguments.python_generated_idl_list)

    if python_generated_idl_files:
        sys.stderr.write(
            "The Python LibWeb bindings generator is not implemented yet, but these IDL files were opted in:\n"
        )
        for idl_file in python_generated_idl_files:
            sys.stderr.write(f"  {idl_file}\n")
        return 1

    command = [
        str(arguments.cxx_generator),
        "-o",
        str(arguments.output_path),
    ]
    for header_include_path in arguments.header_include_path:
        command.extend(["--header-include-path", str(header_include_path)])
    if arguments.depfile is not None:
        command.extend(["--depfile", str(arguments.depfile)])
    command.extend(str(path) for path in arguments.paths)

    return subprocess.run(command).returncode


def main() -> int:
    arguments = parse_arguments()
    output_directory = arguments.output_path
    output_directory.mkdir(parents=True, exist_ok=True)

    result = run_cxx_generator(arguments)
    if result != 0:
        return result

    modules: List[Module] = []

    for path in read_input_paths(arguments.paths):
        module = parse_module(path, path.read_text(encoding="utf-8"))
        modules.append(module)

    interface_sets = collect_interface_sets(modules)

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
    write_generated_file(output_directory / "Forward.h", write_forward_header, modules)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
