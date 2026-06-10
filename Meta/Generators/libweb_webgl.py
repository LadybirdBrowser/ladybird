# Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

# Shared model and CLI driver for the WebGL generators: loads GLFunctions.json and
# derives the generated method name and signature of each GL entry point.

import argparse
import json
import sys

from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.utils import title_case_to_snake_case as snake_case


def load_functions(path: str) -> list:
    with open(path, "r", encoding="utf-8") as input_file:
        return json.load(input_file)


# All WebGL generators take the same -h/-c/-j arguments and emit one header and one
# implementation file from the loaded function list.
def run_generator(description: str, write_header_file, write_implementation_file) -> None:
    parser = argparse.ArgumentParser(description=description, add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the header file to generate")
    parser.add_argument("-c", "--implementation", required=True, help="Path to the implementation file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    functions = load_functions(args.json)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, functions)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, functions)


def command_name(function: dict) -> str:
    assert function["name"].startswith("gl")
    return function["name"][2:]


def method_name(function: dict) -> str:
    return snake_case(command_name(function))


def method_signature(function: dict, qualifier: str = "") -> str:
    args = ", ".join(f"{arg['type']} {arg['name']}" for arg in function["args"])
    return f"{function['return']} {qualifier}{method_name(function)}({args})"
