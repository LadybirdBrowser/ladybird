#!/usr/bin/env python3

import argparse
import os
import re
import sys
import shutil
import multiprocessing
from pathlib import Path
import subprocess
from enum import IntEnum


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
        import platform
        self.system = platform.system()
        if self.system == 'Windows':
            self.host_system = HostSystem.Windows
        elif self.system == 'Darwin':
            self.host_system = HostSystem.macOS
        elif self.system == 'Linux':
            self.host_system = HostSystem.Linux
        else:
            self.host_system = HostSystem.Unsupported

        self.architecture = platform.machine().lower()
        if self.architecture in ('x86_64', 'amd64'):
            self.host_architecture = HostArchitecture.x86_64
        elif self.architecture in ('aarch64', 'arm64'):
            self.host_architecture = HostArchitecture.AArch64
        else:
            self.host_architecture = HostArchitecture.Unsupported


def main(platform):
    if platform.host_system == HostSystem.Unsupported:
        print(f'Unsupported host system {platform.system}', file=sys.stderr)
        sys.exit(1)
    if platform.host_architecture == HostArchitecture.Unsupported:
        print(f'Unsupported host architecture {platform.architecture}', file=sys.stderr)
        sys.exit(1)

    parser = argparse.ArgumentParser(description='Ladybird')
    subparsers = parser.add_subparsers(dest='command')

    preset_parser = argparse.ArgumentParser(add_help=False)
    preset_parser.add_argument(
        '--preset',
        required=False,
        default=os.environ.get('BUILD_PRESET',
                               'windows_dev_ninja' if platform.host_system == HostSystem.Windows else 'default'),
    )

    # FIXME: Validate that the cc/cxx combination is compatible (e.g. don't allow CC=gcc and CXX=clang++)
    # FIXME: Migrate find_compiler.sh for more explicit compiler validation
    compiler_parser = argparse.ArgumentParser(add_help=False)
    compiler_parser.add_argument(
        '--cc',
        required=False,
        default=os.environ.get('CC', 'clang-cl' if platform.host_system == HostSystem.Windows else 'cc'))
    compiler_parser.add_argument(
        '--cxx',
        required=False,
        default=os.environ.get('CXX', 'clang-cl' if platform.host_system == HostSystem.Windows else 'c++')
    )

    target_parser = argparse.ArgumentParser(add_help=False)
    target_parser.add_argument('--target', required=False, default='Ladybird')

    build_parser = subparsers.add_parser('build', help='Compiles the target binaries',
                                         parents=[preset_parser, compiler_parser, target_parser])
    build_parser.add_argument('args', nargs=argparse.REMAINDER,
                              help='Additional arguments passed through to the build system')

    test_parser = subparsers.add_parser(
        'test', help='Runs the unit tests on the build host', parents=[preset_parser, compiler_parser])
    test_parser.add_argument('--pattern', required=False,
                             help='Limits the tests that are ran to those that match the regex pattern')

    args = parser.parse_args()
    kwargs = vars(args)
    command = kwargs.pop('command', None)

    if not command:
        parser.print_help()
        sys.exit(1)

    if platform.host_system != HostSystem.Windows and os.geteuid() == 0:
        print('Do not run ladybird.py as root, your Build directory will become root-owned', file=sys.stderr)
        sys.exit(1)
    elif platform.host_system == HostSystem.Windows and 'VCINSTALLDIR' not in os.environ:
        print('ladybird.py must be run from a Visual Studio enabled environment', file=sys.stderr)
        sys.exit(1)

    if command == 'build':
        build_dir = _configure_main(platform, **kwargs)
        _build_main(build_dir, **kwargs)
    elif command == 'test':
        build_dir = _configure_main(platform,  **kwargs)
        _build_main(build_dir)
        _test_main(build_dir, **kwargs)


def _configure_main(platform, **kwargs):
    cmake_args = []

    host_system = platform.host_system
    if host_system == HostSystem.Linux and platform.host_architecture == HostArchitecture.AArch64:
        cmake_args.extend(_configure_skia_jemalloc())

    lb_source_dir, build_preset_dir, build_env_cmake_args = _configure_build_env(**kwargs)
    if build_preset_dir.joinpath('build.ninja').exists() or build_preset_dir.joinpath('ladybird.sln').exists():
        return build_preset_dir

    _build_vcpkg()
    _validate_cmake_version()

    cmake_args.extend(build_env_cmake_args)
    config_args = [
        'cmake',
        '--preset',
        kwargs.get('preset'),
        '-S',
        lb_source_dir,
        '-B',
        build_preset_dir,
    ]
    config_args.extend(cmake_args)

    # FIXME: Improve error reporting for vcpkg install failures
    # https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/BuildInstructionsLadybird.md#unable-to-find-a-build-program-corresponding-to-ninja
    try:
        subprocess.check_call(config_args)
    except subprocess.CalledProcessError as e:
        _print_process_stderr(e, 'Unable to configure ladybird project')
        sys.exit(1)
    return build_preset_dir


def _configure_skia_jemalloc():
    import resource
    page_size = resource.getpagesize()
    gn = shutil.which('gn') or None
    # https://github.com/LadybirdBrowser/ladybird/issues/261
    if page_size != 4096 and gn is None:
        print('GN not found! Please build GN from source and put it in $PATH', file=sys.stderr)
        sys.exit(1)
    pkg_config = shutil.which('pkg-config') or None
    cmake_args = []
    if pkg_config:
        cmake_args.append(f'-DPKG_CONFIG_EXECUTABLE={pkg_config}')

    user_vars_cmake_module = Path('Meta/CMake/vcpkg/user-variables.cmake')
    user_vars_cmake_module.parent.mkdir(parents=True, exist_ok=True)

    with open(user_vars_cmake_module, 'w') as f:
        f.writelines([
            f'set(PKGCONFIG {pkg_config})',
            f'set(GN {gn})',
            '',
        ])

    return cmake_args


def _configure_build_env(**kwargs):
    preset = kwargs.get('preset')
    cc = kwargs.get('cc')
    cxx = kwargs.get('cxx')
    cmake_args = []
    cmake_args.extend([
        f'-DCMAKE_C_COMPILER={cc}',
        f'-DCMAKE_CXX_COMPILER={cxx}',
    ])
    _ensure_ladybird_source_dir()
    lb_source_dir = Path(os.environ.get('LADYBIRD_SOURCE_DIR'))

    build_root_dir = lb_source_dir / 'Build'

    known_presets = {
        'default': build_root_dir / 'release',
        'windows_ci_ninja': build_root_dir / 'release',
        'windows_dev_ninja': build_root_dir / 'debug',
        'windows_dev_msbuild': build_root_dir / 'debug',
        'Debug': build_root_dir / 'debug',
        'Sanitizer': build_root_dir / 'sanitizers',
        'Distribution': build_root_dir / 'distribution',
    }
    if preset not in known_presets:
        print(f'Unknown build preset "{preset}"', file=sys.stderr)
        sys.exit(1)

    build_preset_dir = known_presets.get(preset)
    vcpkg_root = str(build_root_dir / 'vcpkg')
    os.environ['VCPKG_ROOT'] = vcpkg_root
    os.environ['PATH'] += os.pathsep + str(lb_source_dir.joinpath('Toolchain', 'Local', 'cmake', 'bin'))
    os.environ['PATH'] += os.pathsep + str(vcpkg_root)
    return lb_source_dir, build_preset_dir, cmake_args


def _build_vcpkg():
    sys.path.append(str(Path(__file__).parent.joinpath('..', 'Toolchain')))
    # FIXME: Rename main() in BuildVcpkg.py to build_vcpkg() and call that from the scripts __main__
    from BuildVcpkg import main as build_vcpkg
    build_vcpkg()


def _validate_cmake_version():
    # FIXME: This 3.25+ CMake version check may not be needed anymore due to vcpkg downloading a newer version
    cmake_pls_install_msg = 'Please install CMake version 3.25 or newer.'

    try:
        cmake_version_output = subprocess.check_output(
            [
                'cmake',
                '--version',
            ],
            text=True
        ).strip()

        version_match = re.search(r'version\s+(\d+)\.(\d+)\.(\d+)?', cmake_version_output)
        if version_match:
            major = int(version_match.group(1))
            minor = int(version_match.group(2))
            patch = int(version_match.group(3))
            if major < 3 or (major == 3 and minor < 25):
                print(f'CMake version {major}.{minor}.{patch} is too old. {cmake_pls_install_msg}', file=sys.stderr)
                sys.exit(1)
        else:
            print(f'Unable to determine CMake version. {cmake_pls_install_msg}', file=sys.stderr)
            sys.exit(1)
    except subprocess.CalledProcessError as e:
        _print_process_stderr(e, f'CMake not found. {cmake_pls_install_msg}\n')
        sys.exit(1)


def _ensure_ladybird_source_dir():
    ladybird_source_dir = os.environ.get('LADYBIRD_SOURCE_DIR', None)
    ladybird_source_dir = Path(ladybird_source_dir) if ladybird_source_dir else None

    if not ladybird_source_dir or not ladybird_source_dir.is_dir():
        try:
            top_dir = subprocess.check_output(
                [
                    'git',
                    'rev-parse',
                    '--show-toplevel',
                ],
                text=True
            ).strip()

            ladybird_source_dir = Path(top_dir)
            os.environ['LADYBIRD_SOURCE_DIR'] = str(ladybird_source_dir)
        except subprocess.CalledProcessError as e:
            _print_process_stderr(e, 'Unable to determine LADYBIRD_SOURCE_DIR:')
            sys.exit(1)
    return ladybird_source_dir


def _build_main(build_dir, **kwargs):
    build_args = [
        'cmake',
        '--build',
        str(build_dir),
        '--parallel',
        os.environ.get('MAKEJOBS', str(multiprocessing.cpu_count())),
    ]
    build_target = kwargs.get('target', '')
    if build_target:
        build_args.extend([
            '--target',
            build_target,
        ])
    build_system_args = kwargs.get('args', [])
    if build_system_args:
        build_args.append('--')
        build_args.extend(build_system_args)
    try:
        subprocess.check_call(build_args)
    except subprocess.CalledProcessError as e:
        msg = 'Unable to build ladybird '
        if build_target:
            msg += f'target "{build_target}"'
        else:
            msg += 'project'
        _print_process_stderr(e, msg)
        sys.exit(1)


def _test_main(build_dir, **kwargs):
    build_preset = kwargs.get('preset')
    test_args = [
        'ctest',
        '--preset',
        build_preset,
        '--output-on-failure',
        '--test-dir',
        str(build_dir),
    ]
    test_name_pattern = kwargs.get('pattern', None)
    if test_name_pattern:
        test_args.extend([
            '-R',
            test_name_pattern,
        ])
    try:
        subprocess.check_call(test_args)
    except subprocess.CalledProcessError as e:
        msg = 'Unable to test ladybird '
        if test_name_pattern:
            msg += f'name pattern "{test_name_pattern}"'
        else:
            msg += 'project'
        _print_process_stderr(e, msg)
        sys.exit(1)


def _print_process_stderr(e, msg):
    err_details = f': {e.stderr}' if e.stderr else ''
    print(f'{msg}{err_details}', file=sys.stderr)


if __name__ == '__main__':
    main(Platform())
