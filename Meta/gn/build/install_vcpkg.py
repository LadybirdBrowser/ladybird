#!/usr/bin/env python3

import argparse
import os
import pathlib
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description='Install vcpkg dependencies')
    parser.add_argument('--cc', type=str, required=True, help='The C compiler to use')
    parser.add_argument('--cxx', type=str, required=True, help='The C++ compiler to use')
    parser.add_argument('--manifest', type=str, required=True, help='The vcpkg manifest to install')
    parser.add_argument('--vcpkg', type=str, required=True, help='The path to the vcpkg executable')
    parser.add_argument('--vcpkg-root', type=str, required=True, help='The path to the vcpkg root directory')
    parser.add_argument('--vcpkg-triplet', type=str, required=True, help='The vcpkg triplet to use')
    parser.add_argument('--vcpkg-overlay-triplets', type=str, help='Path to a vcpkg overlay triplets directory')
    parser.add_argument('--vcpkg-binary-cache-dir', type=str, help='Path to a vcpkg binary cache directory')
    parser.add_argument('--stamp-file', type=str, help='Path to a file to touch after installation')
    parser.add_argument('install_directory', type=str, help='The directory to install vcpkg deps into')
    args = parser.parse_args()

    manifest_directory = pathlib.Path(args.manifest).parent

    env = os.environ.copy()
    env['CC'] = args.cc
    env['CXX'] = args.cxx

    vcpkg_arguments = [
        args.vcpkg,
        'install',
        '--no-print-usage',
        '--x-wait-for-lock',
        f'--triplet={args.vcpkg_triplet}',
        f'--vcpkg-root={args.vcpkg_root}',
        f'--x-manifest-root={manifest_directory}',
        f'--x-install-root={args.install_directory}'
    ]

    if args.vcpkg_overlay_triplets:
        vcpkg_arguments += [f'--overlay-triplets={args.vcpkg_overlay_triplets}']
    if args.vcpkg_binary_cache_dir:
        binary_cache_dir = pathlib.Path(args.vcpkg_binary_cache_dir).absolute()
        vcpkg_arguments += [f'--binarysource=clear;files,{binary_cache_dir},readwrite']

    subprocess.run(vcpkg_arguments, env=env, check=True)

    if args.stamp_file:
        pathlib.Path(args.stamp_file).touch()

    return 0


if __name__ == '__main__':
    sys.exit(main())
