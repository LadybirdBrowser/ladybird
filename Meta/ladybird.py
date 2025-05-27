#!/usr/bin/env python3

import argparse
import importlib.util
import multiprocessing
import os
import platform
import re
import shutil
import subprocess
import sys
import types

from enum import IntEnum
from pathlib import Path


def import_module(module_path: Path) -> types.ModuleType:
    spec = importlib.util.spec_from_file_location(module_path.stem, module_path)
    if not spec or not spec.loader:
        raise ModuleNotFoundError(f"Could not find module {module_path}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    return module


META_PATH = Path(__file__).parent
TOOLCHAIN_PATH = META_PATH.parent / "Toolchain"

BuildVcpkg = import_module(TOOLCHAIN_PATH / "BuildVcpkg.py")


class HostArchitecture(IntEnum):
    Unsupported = 0
    x86_64 = 1
    AArch64 = 2


class HostSystem(IntEnum):
    Unsupported = 0
    Linux = 1
    macOS = 2
    Windows = 3


class Platform:
    def __init__(self):
        self.system = platform.system()
        if self.system == "Windows":
            self.host_system = HostSystem.Windows
        elif self.system == "Darwin":
            self.host_system = HostSystem.macOS
        elif self.system == "Linux":
            self.host_system = HostSystem.Linux
        else:
            self.host_system = HostSystem.Unsupported

        self.architecture = platform.machine().lower()
        if self.architecture in ("x86_64", "amd64"):
            self.host_architecture = HostArchitecture.x86_64
        elif self.architecture in ("aarch64", "arm64"):
            self.host_architecture = HostArchitecture.AArch64
        else:
            self.host_architecture = HostArchitecture.Unsupported


def main():
    platform = Platform()

    if platform.host_system == HostSystem.Unsupported:
        print(f"Unsupported host system {platform.system}", file=sys.stderr)
        sys.exit(1)
    if platform.host_architecture == HostArchitecture.Unsupported:
        print(f"Unsupported host architecture {platform.architecture}", file=sys.stderr)
        sys.exit(1)

    parser = argparse.ArgumentParser(description="Ladybird")
    subparsers = parser.add_subparsers(dest="command")

    preset_parser = argparse.ArgumentParser(add_help=False)
    preset_parser.add_argument(
        "--preset",
        required=False,
        default=os.environ.get(
            "BUILD_PRESET", "windows_dev_ninja" if platform.host_system == HostSystem.Windows else "default"
        ),
    )

    # FIXME: Validate that the cc/cxx combination is compatible (e.g. don't allow CC=gcc and CXX=clang++)
    # FIXME: Migrate find_compiler.sh for more explicit compiler validation
    compiler_parser = argparse.ArgumentParser(add_help=False)
    compiler_parser.add_argument(
        "--cc",
        required=False,
        default=os.environ.get("CC", "clang-cl" if platform.host_system == HostSystem.Windows else "cc"),
    )
    compiler_parser.add_argument(
        "--cxx",
        required=False,
        default=os.environ.get("CXX", "clang-cl" if platform.host_system == HostSystem.Windows else "c++"),
    )

    target_parser = argparse.ArgumentParser(add_help=False)
    target_parser.add_argument("target", nargs=argparse.OPTIONAL, default="Ladybird")

    build_parser = subparsers.add_parser(
        "build", help="Compiles the target binaries", parents=[preset_parser, compiler_parser, target_parser]
    )
    build_parser.add_argument(
        "args", nargs=argparse.REMAINDER, help="Additional arguments passed through to the build system"
    )

    test_parser = subparsers.add_parser(
        "test", help="Runs the unit tests on the build host", parents=[preset_parser, compiler_parser]
    )
    test_parser.add_argument(
        "--pattern", required=False, help="Limits the tests that are ran to those that match the regex pattern"
    )

    run_parser = subparsers.add_parser(
        "run", help="Runs the application on the build host", parents=[preset_parser, compiler_parser, target_parser]
    )
    run_parser.add_argument(
        "args", nargs=argparse.REMAINDER, help="Additional arguments passed through to the application"
    )

    debug_parser = subparsers.add_parser(
        "debug",
        help="Launches the application on the build host in a gdb or lldb session",
        parents=[preset_parser, target_parser],
    )
    debug_parser.add_argument(
        "--debugger", required=False, default="gdb" if platform.host_system == HostSystem.Linux else "lldb"
    )
    debug_parser.add_argument(
        "-cmd", action="append", required=False, default=[], help="Additional commands passed through to the debugger"
    )

    subparsers.add_parser(
        "install", help="Installs the target binary", parents=[preset_parser, compiler_parser, target_parser]
    )

    subparsers.add_parser(
        "vcpkg", help="Ensure that dependencies are available", parents=[preset_parser, compiler_parser]
    )

    subparsers.add_parser("clean", help="Cleans the build environment", parents=[preset_parser, compiler_parser])

    rebuild_parser = subparsers.add_parser(
        "rebuild",
        help="Cleans the build environment and compiles the target binaries",
        parents=[preset_parser, compiler_parser, target_parser],
    )
    rebuild_parser.add_argument(
        "args", nargs=argparse.REMAINDER, help="Additional arguments passed through to the build system"
    )

    addr2line_parser = subparsers.add_parser(
        "addr2line",
        help="Resolves the addresses in the target binary to a file:line",
        parents=[preset_parser, compiler_parser, target_parser],
    )
    addr2line_parser.add_argument(
        "--program",
        required=False,
        default=(
            "llvm-symbolizer"
            if platform.host_system == HostSystem.Windows
            else "addr2line" if platform.host_system == HostSystem.Linux else "atos"
        ),
    )
    addr2line_parser.add_argument("addresses", nargs=argparse.REMAINDER)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    if platform.host_system != HostSystem.Windows and os.geteuid() == 0:
        print("Do not run ladybird.py as root, your Build directory will become root-owned", file=sys.stderr)
        sys.exit(1)
    elif platform.host_system == HostSystem.Windows and "VCINSTALLDIR" not in os.environ:
        print("ladybird.py must be run from a Visual Studio enabled environment", file=sys.stderr)
        sys.exit(1)

    if args.target == "ladybird":
        args.target = "Ladybird"

    if args.command == "build":
        build_dir = configure_main(platform, args.preset, args.cc, args.cxx)
        build_main(build_dir, args.target, args.args)
    elif args.command == "test":
        build_dir = configure_main(platform, args.preset, args.cc, args.cxx)
        build_main(build_dir)
        test_main(build_dir, args.preset, args.pattern)
    elif args.command == "run":
        if args.preset == "Sanitizer":
            # FIXME: Find some way to centralize these b/w CMakePresets.json, CI files, Documentation and here.
            os.environ["ASAN_OPTIONS"] = os.environ.get(
                "ASAN_OPTIONS",
                "strict_string_checks=1:check_initialization_order=1:strict_init_order=1:"
                "detect_stack_use_after_return=1:allocator_may_return_null=1",
            )
            os.environ["UBSAN_OPTIONS"] = os.environ.get(
                "UBSAN_OPTIONS", "print_stacktrace=1:print_summary=1:halt_on_error=1"
            )
        build_dir = configure_main(platform, args.preset, args.cc, args.cxx)
        build_main(build_dir, args.target)
        run_main(platform.host_system, build_dir, args.target, args.args)
    elif args.command == "debug":
        build_dir = configure_main(platform, args.preset, args.cc, args.cxx)
        build_main(build_dir, args.target, args.args)
        debug_main(platform.host_system, build_dir, args.target, args.debugger, args.cmd)
    elif args.command == "install":
        build_dir = configure_main(platform, args.preset, args.cc, args.cxx)
        build_main(build_dir, args.target, args.args)
        build_main(build_dir, "install", args.args)
    elif args.command == "vcpkg":
        configure_build_env(args.preset, args.cc, args.cxx)
        BuildVcpkg.build_vcpkg()
    elif args.command == "clean":
        clean_main(args.preset, args.cc, args.cxx)
    elif args.command == "rebuild":
        clean_main(args.preset, args.cc, args.cxx)
        build_dir = configure_main(platform, args.preset, args.cc, args.cxx)
        build_main(build_dir, args.target, args.args)
    elif args.command == "addr2line":
        build_dir = configure_main(platform, args.preset, args.cc, args.cxx)
        build_main(build_dir, args.target)
        addr2line_main(build_dir, args.target, args.program, args.addresses)


def configure_main(platform: Platform, preset: str, cc: str, cxx: str) -> Path:
    ladybird_source_dir, build_preset_dir, build_env_cmake_args = configure_build_env(preset, cc, cxx)
    BuildVcpkg.build_vcpkg()

    if build_preset_dir.joinpath("build.ninja").exists() or build_preset_dir.joinpath("ladybird.sln").exists():
        return build_preset_dir

    cmake_args = []

    host_system = platform.host_system
    if host_system == HostSystem.Linux and platform.host_architecture == HostArchitecture.AArch64:
        cmake_args.extend(configure_skia_jemalloc())

    validate_cmake_version()

    config_args = [
        "cmake",
        "--preset",
        preset,
        "-S",
        ladybird_source_dir,
        "-B",
        build_preset_dir,
    ]
    config_args.extend(build_env_cmake_args)
    config_args.extend(cmake_args)

    # FIXME: Improve error reporting for vcpkg install failures
    # https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/BuildInstructionsLadybird.md#unable-to-find-a-build-program-corresponding-to-ninja
    try:
        subprocess.check_call(config_args)
    except subprocess.CalledProcessError as e:
        print_process_stderr(e, "Unable to configure ladybird project")
        sys.exit(1)

    return build_preset_dir


def configure_skia_jemalloc() -> list[str]:
    # NOTE: The resource module is only available on Unix, see the "Availability" section at
    # https://docs.python.org/3/library/resource.html. Given Windows never calls this function, we import locally
    # instead.
    import resource

    page_size = resource.getpagesize()
    gn = shutil.which("gn") or None

    # https://github.com/LadybirdBrowser/ladybird/issues/261
    if page_size != 4096 and gn is None:
        print("GN not found! Please build GN from source and put it in $PATH", file=sys.stderr)
        sys.exit(1)

    cmake_args = []
    pkg_config = shutil.which("pkg-config") or None

    if pkg_config:
        cmake_args.append(f"-DPKG_CONFIG_EXECUTABLE={pkg_config}")

    user_vars_cmake_module = Path("Meta", "CMake", "vcpkg", "user-variables.cmake")
    user_vars_cmake_module.parent.mkdir(parents=True, exist_ok=True)

    with open(user_vars_cmake_module, "w") as f:
        f.writelines(
            [
                f"set(PKGCONFIG {pkg_config})",
                f"set(GN {gn})",
                "",
            ]
        )

    return cmake_args


def configure_build_env(preset: str, cc: str, cxx: str) -> tuple[Path, Path, list[str]]:
    cmake_args = [
        f"-DCMAKE_C_COMPILER={cc}",
        f"-DCMAKE_CXX_COMPILER={cxx}",
    ]

    ladybird_source_dir = ensure_ladybird_source_dir()
    build_root_dir = ladybird_source_dir / "Build"

    known_presets = {
        "default": build_root_dir / "release",
        "windows_ci_ninja": build_root_dir / "release",
        "windows_dev_ninja": build_root_dir / "debug",
        "windows_dev_msbuild": build_root_dir / "debug",
        "Debug": build_root_dir / "debug",
        "Sanitizer": build_root_dir / "sanitizers",
        "Distribution": build_root_dir / "distribution",
    }

    build_preset_dir = known_presets.get(preset, None)
    if not build_preset_dir:
        print(f'Unknown build preset "{preset}"', file=sys.stderr)
        sys.exit(1)

    vcpkg_root = str(build_root_dir / "vcpkg")
    os.environ["VCPKG_ROOT"] = vcpkg_root
    os.environ["PATH"] += os.pathsep + str(ladybird_source_dir.joinpath("Toolchain", "Local", "cmake", "bin"))
    os.environ["PATH"] += os.pathsep + str(vcpkg_root)

    return ladybird_source_dir, build_preset_dir, cmake_args


def validate_cmake_version():
    # FIXME: This 3.25+ CMake version check may not be needed anymore due to vcpkg downloading a newer version
    cmake_install_message = "Please install CMake version 3.25 or newer."

    try:
        cmake_version_output = subprocess.check_output(["cmake", "--version"], text=True).strip()

        version_match = re.search(r"version\s+(\d+)\.(\d+)\.(\d+)?", cmake_version_output)
        if version_match:
            major = int(version_match.group(1))
            minor = int(version_match.group(2))
            patch = int(version_match.group(3))
            if major < 3 or (major == 3 and minor < 25):
                print(f"CMake version {major}.{minor}.{patch} is too old. {cmake_install_message}", file=sys.stderr)
                sys.exit(1)
        else:
            print(f"Unable to determine CMake version. {cmake_install_message}", file=sys.stderr)
            sys.exit(1)
    except subprocess.CalledProcessError as e:
        print_process_stderr(e, f"CMake not found. {cmake_install_message}\n")
        sys.exit(1)


def ensure_ladybird_source_dir() -> Path:
    ladybird_source_dir = os.environ.get("LADYBIRD_SOURCE_DIR", None)
    ladybird_source_dir = Path(ladybird_source_dir) if ladybird_source_dir else None

    if not ladybird_source_dir or not ladybird_source_dir.is_dir():
        try:
            top_dir = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
            ladybird_source_dir = Path(top_dir)

            os.environ["LADYBIRD_SOURCE_DIR"] = str(ladybird_source_dir)
        except subprocess.CalledProcessError as e:
            print_process_stderr(e, "Unable to determine LADYBIRD_SOURCE_DIR:")
            sys.exit(1)

    return ladybird_source_dir


def build_main(build_dir: Path, target: str | None = None, args: list[str] = []):
    build_args = [
        "cmake",
        "--build",
        str(build_dir),
        "--parallel",
        os.environ.get("MAKEJOBS", str(multiprocessing.cpu_count())),
    ]
    if target:
        build_args.extend(["--target", target])

    if args:
        build_args.append("--")
        build_args.extend(args)

    try:
        subprocess.check_call(build_args)
    except subprocess.CalledProcessError as e:
        print_process_stderr(e, f"Unable to build Ladybird {f'target {target}' if target else 'project'}")
        sys.exit(1)


def test_main(build_dir: Path, preset: str, pattern: str | None):
    test_args = [
        "ctest",
        "--preset",
        preset,
        "--output-on-failure",
        "--test-dir",
        str(build_dir),
    ]

    if pattern:
        test_args.extend(["-R", pattern])

    try:
        subprocess.check_call(test_args)
    except subprocess.CalledProcessError as e:
        print_process_stderr(e, f"Unable to test Ladybird {f'pattern {pattern}' if pattern else 'project'}")
        sys.exit(1)


def run_main(host_system: HostSystem, build_dir: Path, target: str, args: list[str]):
    run_args = []

    if host_system == HostSystem.macOS and target in (
        "headless-browser",
        "ImageDecoder",
        "Ladybird",
        "RequestServer",
        "WebContent",
        "WebDriver",
        "WebWorker",
    ):
        run_args.append(str(build_dir.joinpath("bin", "Ladybird.app", "Contents", "MacOS", target)))
    else:
        run_args.append(str(build_dir.joinpath("bin", target)))

    run_args.extend(args)

    try:
        # FIXME: For Windows, set the working directory so DLLs are found
        subprocess.check_call(run_args)
    except subprocess.CalledProcessError as e:
        print_process_stderr(e, f'Unable to run ladybird target "{target}"')
        sys.exit(1)


def debug_main(host_system: HostSystem, build_dir: Path, target: str, debugger: str, debugger_commands: list[str]):
    debuggers = ["gdb", "lldb"]
    if debugger not in debuggers or not shutil.which(debugger):
        print("Please install gdb or lldb!", file=sys.stderr)
        sys.exit(1)

    gdb_args = [debugger]
    for cmd in debugger_commands:
        gdb_args.extend(["-ex" if debugger == "gdb" else "-o", cmd])

    if target == "Ladybird" and host_system == HostSystem.macOS:
        gdb_args.append(str(build_dir.joinpath("bin", "Ladybird.app")))
    else:
        gdb_args.append(str(build_dir.joinpath("bin", target)))

    try:
        # FIXME: For Windows, set the working directory so DLLs are found
        subprocess.check_call(gdb_args)
    except subprocess.CalledProcessError as e:
        print_process_stderr(e, f'Unable to run ladybird target "{target}" with "{debugger}" debugger')
        sys.exit(1)


def clean_main(preset: str, cc: str, cxx: str):
    ladybird_source_dir, build_preset_dir, _ = configure_build_env(preset, cc, cxx)
    shutil.rmtree(str(build_preset_dir), ignore_errors=True)

    user_vars_cmake_module = ladybird_source_dir.joinpath("Meta", "CMake", "vcpkg", "user-variables.cmake")
    user_vars_cmake_module.unlink(missing_ok=True)


def addr2line_main(build_dir, target: str, program: str, addresses: list[str]):
    if not shutil.which(program):
        print(f"Please install {program}!", file=sys.stderr)
        sys.exit(1)

    binary_file_path = None
    for root, _, files in os.walk(build_dir):
        if target in files:
            candidate = Path(root) / target
            if os.access(candidate, os.X_OK):
                binary_file_path = str(candidate)

    if not binary_file_path:
        print(f'Unable to find binary target "{target}" in build directory "{build_dir}"', file=sys.stderr)
        sys.exit(1)

    addr2line_args = [
        program,
        "-o" if program == "atos" else "-e",
        binary_file_path,
    ]
    addr2line_args.extend(addresses)

    try:
        subprocess.check_call(addr2line_args)
    except subprocess.CalledProcessError as e:
        print_process_stderr(e, f'Unable to find lines with "{program}" for binary target "{target}"')
        sys.exit(1)


def print_process_stderr(exception: subprocess.CalledProcessError, message: str):
    details = f": {exception.stderr}" if exception.stderr else ""
    print(f"{message}{details}", file=sys.stderr)


if __name__ == "__main__":
    main()
