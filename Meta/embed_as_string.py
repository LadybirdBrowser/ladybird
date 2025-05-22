#!/usr/bin/env python3
r"""
Embeds a file into a String, a la #embed from C++23
"""

import argparse
import sys


def main():
    parser = argparse.ArgumentParser(epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="input file to stringify")
    parser.add_argument("-o", "--output", required=True, help="output file")
    parser.add_argument("-n", "--variable-name", required=True, help="name of the C++ variable")
    parser.add_argument("-s", "--namespace", required=False, help="C++ namespace to put the string into")
    args = parser.parse_args()

    with open(args.output, "w") as f:
        f.write("#include <AK/String.h>\n")
        if args.namespace:
            f.write(f"namespace {args.namespace} {{\n")
        f.write(f"extern String {args.variable_name};\n")
        f.write(f'String {args.variable_name} = R"~~~(')
        with open(args.input, "r") as input:
            for line in input.readlines():
                f.write(f"{line}")
        f.write(')~~~"_string;\n')
        if args.namespace:
            f.write("}\n")


if __name__ == "__main__":
    sys.exit(main())
