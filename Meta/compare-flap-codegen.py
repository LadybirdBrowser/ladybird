#!/usr/bin/env python3

"""
Copyright (c) 2026-present, the Ladybird developers.

SPDX-License-Identifier: BSD-2-Clause
"""

import argparse
import difflib
import os
import re
import struct
import subprocess
import sys
import tempfile

from dataclasses import dataclass
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPOSITORY_ROOT / "Libraries/LibJS/Interpreter/interpreter.flap"
DEFAULT_LAYOUT = REPOSITORY_ROOT / "Build/release/Libraries/LibJS/Interpreter/layout.conf"
ARCHITECTURES = ("x86_64", "aarch64")
ELF_SECTION_TYPE_REL = 9
ELF_SECTION_TYPE_RELA = 4
ELF_SECTION_TYPE_SYMTAB = 2
ELF_SECTION_TYPE_DYNSYM = 11
ELF_SECTION_TYPE_NOBITS = 8
ELF_SECTION_FLAG_ALLOC = 0x2


@dataclass(frozen=True)
class ElfSection:
    name: str
    section_type: int
    flags: int
    size: int
    link: int
    info: int
    alignment: int
    entry_size: int
    contents: bytes

    def comparison_key(self):
        return (
            self.name,
            self.section_type,
            self.flags,
            self.size,
            self.link,
            self.info,
            self.alignment,
            self.entry_size,
            self.contents,
        )


def run(command, *, cwd=None):
    try:
        subprocess.run(command, cwd=cwd, check=True)
    except FileNotFoundError as error:
        raise RuntimeError(f"required program not found: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"command failed with exit status {error.returncode}: {' '.join(command)}") from error


def compile_interpreter(flapc, architecture, output, arguments):
    command = [
        str(flapc),
        "--arch",
        architecture,
        "--object-format",
        "elf",
        "--constants",
        str(arguments.constants),
        "--input",
        str(arguments.input),
        "--output",
        str(output),
    ]
    if arguments.enable_assertions:
        command.append("--enable-assertions")
    if architecture == "aarch64" and arguments.has_jscvt:
        command.append("--has-jscvt")
    run(command)


def assemble(architecture, assembly, output, compiler):
    target = "x86_64-linux-gnu" if architecture == "x86_64" else "aarch64-linux-gnu"
    run([compiler, f"--target={target}", "-c", str(assembly), "-o", str(output)])


def parse_elf(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError(f"assembler did not produce an ELF object: {path}")
    if data[4] != 2 or data[5] != 1:
        raise RuntimeError(f"only little-endian ELF64 objects are supported: {path}")

    section_header_offset = struct.unpack_from("<Q", data, 40)[0]
    section_header_size = struct.unpack_from("<H", data, 58)[0]
    section_count = struct.unpack_from("<H", data, 60)[0]
    section_name_index = struct.unpack_from("<H", data, 62)[0]
    if section_header_size != 64 or section_count == 0 or section_name_index >= section_count:
        raise RuntimeError(f"unsupported ELF section table in {path}")

    headers = []
    for index in range(section_count):
        offset = section_header_offset + index * section_header_size
        if offset + section_header_size > len(data):
            raise RuntimeError(f"truncated ELF section table in {path}")
        headers.append(struct.unpack_from("<IIQQQQIIQQ", data, offset))

    string_header = headers[section_name_index]
    string_offset, string_size = string_header[4], string_header[5]
    section_names = data[string_offset : string_offset + string_size]

    def section_name(offset):
        end = section_names.find(b"\0", offset)
        if end == -1:
            raise RuntimeError(f"unterminated ELF section name in {path}")
        return section_names[offset:end].decode("utf-8", errors="replace")

    sections = []
    for header in headers:
        name_offset, section_type, flags, _, offset, size, link, info, alignment, entry_size = header
        contents = b"" if section_type == ELF_SECTION_TYPE_NOBITS else data[offset : offset + size]
        if len(contents) != (0 if section_type == ELF_SECTION_TYPE_NOBITS else size):
            raise RuntimeError(f"truncated ELF section contents in {path}")
        sections.append(
            ElfSection(
                name=section_name(name_offset),
                section_type=section_type,
                flags=flags,
                size=size,
                link=link,
                info=info,
                alignment=alignment,
                entry_size=entry_size,
                contents=contents,
            )
        )
    return sections


def relevant_sections(sections):
    result = []
    for section in sections:
        is_relocation = section.section_type in (ELF_SECTION_TYPE_REL, ELF_SECTION_TYPE_RELA)
        is_symbol_table = section.section_type in (ELF_SECTION_TYPE_SYMTAB, ELF_SECTION_TYPE_DYNSYM)
        is_unwind = section.name in (".eh_frame", ".eh_frame_hdr", ".ARM.exidx", ".ARM.extab")
        if section.flags & ELF_SECTION_FLAG_ALLOC or is_relocation or is_symbol_table or is_unwind:
            result.append(section)
    return result


def enclosing_handler(lines, line_number):
    handler = "program metadata"
    label_pattern = re.compile(r"^asm_handler_([A-Za-z0-9_]+?)(?:_(?:hot_end|cold|cold_end))?:$")
    for line in lines[:line_number]:
        match = label_pattern.match(line.strip())
        if match:
            suffix = line.strip()[len(f"asm_handler_{match.group(1)}") : -1]
            region = "cold" if suffix.startswith("_cold") else "hot"
            handler = f"{match.group(1)} ({region})"
    return handler


def report_assembly_difference(architecture, baseline, candidate):
    baseline_lines = baseline.read_text().splitlines()
    candidate_lines = candidate.read_text().splitlines()
    matcher = difflib.SequenceMatcher(a=baseline_lines, b=candidate_lines)
    regions = set()
    for tag, baseline_start, _, candidate_start, _ in matcher.get_opcodes():
        if tag == "equal":
            continue
        regions.add(enclosing_handler(baseline_lines, baseline_start))
        regions.add(enclosing_handler(candidate_lines, candidate_start))
    print(f"{architecture}: textual assembly differs in {', '.join(sorted(regions))}", file=sys.stderr)


def compare_architecture(architecture, baseline_dir, candidate_dir, arguments):
    baseline_assembly = baseline_dir / "interpreter.S"
    candidate_assembly = candidate_dir / "interpreter.S"
    baseline_object = baseline_dir / "interpreter.o"
    candidate_object = candidate_dir / "interpreter.o"

    compile_interpreter(arguments.baseline, architecture, baseline_assembly, arguments)
    compile_interpreter(arguments.candidate, architecture, candidate_assembly, arguments)
    assemble(architecture, baseline_assembly, baseline_object, arguments.compiler)
    assemble(architecture, candidate_assembly, candidate_object, arguments.compiler)

    assembly_equal = baseline_assembly.read_bytes() == candidate_assembly.read_bytes()
    if not assembly_equal:
        report_assembly_difference(architecture, baseline_assembly, candidate_assembly)

    baseline_sections = relevant_sections(parse_elf(baseline_object))
    candidate_sections = relevant_sections(parse_elf(candidate_object))
    sections_equal = [section.comparison_key() for section in baseline_sections] == [
        section.comparison_key() for section in candidate_sections
    ]
    if not sections_equal:
        baseline_by_name = {section.name: section for section in baseline_sections}
        candidate_by_name = {section.name: section for section in candidate_sections}
        differing = sorted(
            name
            for name in baseline_by_name.keys() | candidate_by_name.keys()
            if baseline_by_name.get(name) != candidate_by_name.get(name)
        )
        print(f"{architecture}: ELF codegen differs in sections: {', '.join(differing)}", file=sys.stderr)

    if sections_equal and (assembly_equal or not arguments.exact):
        print(f"{architecture}: codegen matches")
        return True
    return False


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Compare the native output of two flapc binaries for x86-64 and AArch64 ELF"
    )
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--input", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--constants", type=Path, default=DEFAULT_LAYOUT)
    parser.add_argument(
        "--compiler",
        default=os.environ.get("CC", "clang"),
        help="compiler used to assemble for both architectures; must accept Clang's --target",
    )
    parser.add_argument("--enable-assertions", action="store_true")
    parser.add_argument("--has-jscvt", action="store_true")
    parser.add_argument(
        "--semantic",
        dest="exact",
        action="store_false",
        help="allow textual assembly changes when compared ELF data is identical",
    )
    parser.set_defaults(exact=True)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    for path in (arguments.baseline, arguments.candidate, arguments.input, arguments.constants):
        if not path.is_file():
            print(f"missing input: {path}", file=sys.stderr)
            return 2

    try:
        with tempfile.TemporaryDirectory(prefix="flap-codegen-baseline-") as baseline_temp:
            with tempfile.TemporaryDirectory(prefix="flap-codegen-candidate-") as candidate_temp:
                matches = True
                for architecture in ARCHITECTURES:
                    baseline_dir = Path(baseline_temp) / architecture
                    candidate_dir = Path(candidate_temp) / architecture
                    baseline_dir.mkdir()
                    candidate_dir.mkdir()
                    matches &= compare_architecture(
                        architecture,
                        baseline_dir,
                        candidate_dir,
                        arguments,
                    )
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 2
    return 0 if matches else 1


if __name__ == "__main__":
    sys.exit(main())
