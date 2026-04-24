# Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import signal
import subprocess
import sys

from pathlib import Path
from typing import Optional
from typing import Union


def run_command(
    command: list[str],
    input: Union[str, None] = None,
    return_output: bool = False,
    exit_on_failure: bool = False,
    cwd: Union[Path, None] = None,
) -> Optional[str]:
    stdin = subprocess.PIPE if type(input) is str else None
    stdout = subprocess.PIPE if return_output else None

    try:
        # FIXME: For Windows, set the working directory so DLLs are found.
        with subprocess.Popen(command, stdin=stdin, stdout=stdout, text=True, cwd=cwd) as process:
            (output, _) = process.communicate(input=input)

            if process.returncode != 0:
                if exit_on_failure:
                    sys.exit(process.returncode)
                return None

    except KeyboardInterrupt:
        process.send_signal(signal.SIGINT)
        process.wait()

        sys.exit(process.returncode)

    if return_output:
        return output.strip()

    return None


def string_hash(string: str) -> int:
    """Port of AK::string_hash that produces the same u32 value."""
    h = 0

    for ch in string:
        h = (h + ord(ch)) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6

    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF

    return h


def title_casify(dashy_name: str) -> str:
    return "".join(part[0].upper() + part[1:] for part in dashy_name.split("-") if part)


def camel_casify(dashy_name: str) -> str:
    parts = [part for part in dashy_name.split("-") if part]
    if not parts:
        return ""
    result = [parts[0]]
    for part in parts[1:]:
        result.append(part[0].upper() + part[1:])
    return "".join(result)


def snake_casify(dashy_name: str, trim_leading_underscores: bool = False) -> str:
    snake_case = dashy_name.replace("-", "_")
    if trim_leading_underscores:
        snake_case = snake_case.lstrip("_")
    return snake_case


def underlying_type_for_enum(member_count: int) -> str:
    if member_count <= 0xFF:
        return "u8"
    if member_count <= 0xFFFF:
        return "u16"
    if member_count <= 0xFFFFFFFF:
        return "u32"
    return "u64"


def make_name_acceptable_cpp(name: str) -> str:
    if name == "float":
        return "float_"
    return name
