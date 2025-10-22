# Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
# Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import enum
import os
import platform
import sys


class HostArchitecture(enum.IntEnum):
    x86_64 = enum.auto()
    riscv64 = enum.auto()
    AArch64 = enum.auto()


class HostSystem(enum.IntEnum):
    Linux = enum.auto()
    macOS = enum.auto()
    Windows = enum.auto()
    BSD = enum.auto()


class Platform:
    def __init__(self):
        self.system = platform.system()
        if self.system == "Windows":
            self.host_system = HostSystem.Windows
        elif self.system == "Darwin":
            self.host_system = HostSystem.macOS
        elif self.system == "Linux":
            self.host_system = HostSystem.Linux
        elif self.system in ("FreeBSD", "OpenBSD", "NetBSD", "DragonFly"):
            self.host_system = HostSystem.BSD
        else:
            print(f"Unsupported host system {self.system}", file=sys.stderr)
            sys.exit(1)

        self.architecture = platform.machine().lower()
        if self.architecture in ("x86_64", "amd64"):
            self.host_architecture = HostArchitecture.x86_64
        elif self.architecture == "riscv64":
            self.host_architecture = HostArchitecture.riscv64
        elif self.architecture in ("aarch64", "arm64"):
            self.host_architecture = HostArchitecture.AArch64
        else:
            print(f"Unsupported host architecture {self.architecture}", file=sys.stderr)
            sys.exit(1)

    def default_compiler(self) -> tuple[str, str]:
        cc = os.environ.get("CC", None)
        cxx = os.environ.get("CXX", None)

        if cc and cxx:
            return (cc, cxx)
        if self.host_system == HostSystem.Windows:
            return ("clang-cl", "clang-cl")
        return ("cc", "c++")

    def default_debugger(self) -> str:
        if self.host_system in (HostSystem.Linux, HostSystem.BSD):
            return "gdb"
        return "lldb"

    def default_symbolizer(self) -> str:
        if self.host_system == HostSystem.Windows:
            return "llvm-symbolizer"
        if self.host_system == HostSystem.macOS:
            return "atos"
        return "addr2line"
