# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import TextIO


class GeneratedIncludes:
    def __init__(self) -> None:
        self._includes: set[str] = set()

    def add(self, include: str) -> None:
        self._includes.add(include)

    def add_binding(self, name: str) -> None:
        self.add(f"LibWeb/Bindings/{name}.h")

    def write(self, out: TextIO) -> None:
        for include in sorted(self._includes):
            out.write(f"#include <{include}>\n")

        if self._includes:
            out.write("\n")
