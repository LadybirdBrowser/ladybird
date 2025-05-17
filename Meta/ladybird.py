#!/usr/bin/env python3

import argparse
import os
import sys
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

    build_parser = subparsers.add_parser('build', help='Compiles the target binaries')
    build_parser.add_argument('args', nargs=argparse.REMAINDER,
                              help='Additional arguments to pass to the build system')

    args = parser.parse_args()
    kwargs = vars(args)
    command = kwargs.pop('command', None)

    if not command:
        parser.print_help()
        sys.exit(1)

    if _is_admin(platform.host_system):
        print('Do not run ladybird.py as root, your Build directory will become root-owned', file=sys.stderr)
        sys.exit(1)

    build_dir = _configure_main(platform)

    if command == 'build':
        _build_main(platform.host_system, build_dir, **kwargs)


def _configure_main(platform):
    from pathlib import Path
    import subprocess
    cmake_args = []

    host_system = platform.host_system
    if host_system == HostSystem.Linux and platform.host_architecture == HostArchitecture.AArch64:
        cmake_args.extend(_configure_skia_jemalloc())

    _ensure_ladybird_source_dir()
    lb_source_dir = Path(os.environ.get('LADYBIRD_SOURCE_DIR'))

    build_root_dir = lb_source_dir / 'Build'

    known_presets = {
        'default': build_root_dir / 'release',
        'windows_ninja': build_root_dir / 'release',
        'windows_msbuild': build_root_dir / 'release',
        'Debug': build_root_dir / 'debug',
        'Sanitizer': build_root_dir / 'sanitizers',
        'Distribution': build_root_dir / 'distribution',
    }

    config_preset = os.environ.get('BUILD_PRESET', 'windows_ninja' if host_system == HostSystem.Windows else 'default')
    if config_preset not in known_presets:
        print(f'Unknown build preset "{config_preset}"', file=sys.stderr)
        sys.exit(1)

    build_preset_dir = known_presets.get(config_preset)

    cmake_args.append(f'-DCMAKE_INSTALL_PREFIX={ str(build_root_dir / f'ladybird-install-{config_preset}')}')
    vcpkg_root = str(build_root_dir / 'vcpkg')
    os.environ['VCPKG_ROOT'] = vcpkg_root
    os.environ['PATH'] += os.pathsep + str(lb_source_dir.joinpath('Toolchain', 'Local', 'cmake', 'bin'))
    os.environ['PATH'] += os.pathsep + str(vcpkg_root)

    from Toolchain.BuildVcpkg import main as build_vcpkg

    build_vcpkg()

    if build_preset_dir.joinpath('build.ninja').exists() or build_preset_dir.joinpath('ladybird.sln').exists():
        return build_preset_dir

    _validate_cmake_version()

    if host_system != HostSystem.Windows:
        cc = os.environ.get('CC', 'cc')
        cxx = os.environ.get('CXX', 'c++')
        cmake_args.extend([
            f'-DCMAKE_C_COMPILER={cc}',
            f'-DCMAKE_CXX_COMPILER={cxx}',
        ])

    config_args = [
        'cmake',
        '--preset',
        config_preset,
        '-S',
        lb_source_dir,
        '-B',
        build_preset_dir,
    ]
    config_args.extend(cmake_args)

    try:
        if host_system == HostSystem.Windows:
            subprocess.check_call(_vcvarsall_script(config_args))
        else:
            subprocess.check_call(config_args)
    except subprocess.CalledProcessError as e:
        _print_process_stderr(e, f'Unable to configure ladybird project')
        sys.exit(1)

    return build_preset_dir


def _configure_skia_jemalloc():
    import shutil
    import resource
    from pathlib import Path
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


def _validate_cmake_version():
    import re
    import subprocess
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
    from pathlib import Path
    import subprocess
    ladybird_source_dir = os.environ.get('LADYBIRD_SOURCE_DIR')

    if not ladybird_source_dir or not Path(ladybird_source_dir).is_dir():
        try:
            top_dir = subprocess.check_output(
                [
                    'git',
                    'rev-parse',
                    '--show-toplevel',
                ],
                text=True
            ).strip()

            ladybird_source_dir = str(Path(top_dir))
            os.environ['LADYBIRD_SOURCE_DIR'] = ladybird_source_dir
        except subprocess.CalledProcessError as e:
            _print_process_stderr(e, 'Unable to determine LADYBIRD_SOURCE_DIR:')
            sys.exit(1)
    return Path(ladybird_source_dir)


def _build_main(host_system, build_dir, **kwargs):
    import subprocess
    build_args = kwargs.get('args', [])
    # FIXME: Support run and install through cmake. For run, we should create
    #  run_* custom targets and then use `--target` to launch via cmake.
    #  For install, `--install` can be used
    build_args.insert(0, 'cmake')
    num_procs = _num_procs(host_system)
    build_args.extend([
        '--build',
        str(build_dir),
        '--parallel',
        num_procs,
    ])
    try:
        if host_system == HostSystem.Windows:
            build_cmd = _vcvarsall_script(build_args)
        else:
            build_cmd = ' '.join(build_args)
        subprocess.run(build_cmd, shell=True, check=True)
    except subprocess.CalledProcessError as e:
        _print_process_stderr(e, 'Unable to build ladybird project')
        sys.exit(1)


def _is_admin(host_system):
    return host_system != HostSystem.Windows and os.geteuid() == 0


def _num_procs(host_system):
    if host_system == HostSystem.Windows:
        return '%NUMBER_OF_PROCESSORS%'
    elif host_system == HostSystem.macOS:
        return '$(sysctl -n hw.ncpu)'
    elif host_system == HostSystem.Linux:
        return '$(nproc)'
    return None


def _find_vcvarsall():
    from pathlib import Path
    program_files = os.environ.get('ProgramFiles')
    vs_versions = [
        '2022',
    ]
    vs_editions = [
        'Enterprise',
        'Professional',
        'Community',
    ]

    vs_vcvarsall_paths = []
    for version in vs_versions:
        vcvarsall_paths = [
            f'{program_files}/Microsoft Visual Studio/{version}/{edition}/VC/Auxiliary/Build/vcvarsall.bat'
            for edition in vs_editions
        ]
        vs_vcvarsall_paths.extend(vcvarsall_paths)

    for vcvarsall in vs_vcvarsall_paths:
        if Path(vcvarsall).exists():
            return vcvarsall
    return None


def _vcvarsall_script(command_args):
    import tempfile
    vcvarsall_path = _find_vcvarsall()
    if not vcvarsall_path:
        print('Unable to find Visual Studio installation', file=sys.stderr)
        sys.exit(1)
    with tempfile.NamedTemporaryFile(suffix='.bat', delete=False, mode='w') as vcvarsall_batch:
        temp_script_path = vcvarsall_batch.name
        vcvarsall_batch.writelines([
            '@echo off\n',
            f'call "{vcvarsall_path}" x64\n',
            f'{' '.join(command_args)}\n',
        ])
        return temp_script_path


def _print_process_stderr(e, msg):
    err_details = f': {e.stderr}' if e.stderr else ''
    print(f'{msg}{err_details}', file=sys.stderr)


if __name__ == '__main__':
    main(Platform())
