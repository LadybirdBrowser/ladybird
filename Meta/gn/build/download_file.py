#!/usr/bin/env python3
r"""Downloads a file as a build artifact.

The file is downloaded to the specified directory.

It's intended to be used for files that are cached between runs.

"""

import argparse
import hashlib
import os
import pathlib
import shutil
import sys
import tempfile
import urllib.request


def compute_sha256(path):
    sha256 = hashlib.sha256()

    with open(path, 'rb') as file:
        while True:
            data = file.read(256 << 10)
            if not data:
                break

            sha256.update(data)

    return sha256.hexdigest()


def main():
    parser = argparse.ArgumentParser(
        epilog=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('url', help='input url')
    parser.add_argument('-o', '--output', required=True,
                        help='output file')
    parser.add_argument('-v', '--version', required=True,
                        help='version of file to detect mismatches and redownload')
    parser.add_argument('-f', '--version-file', required=True,
                        help='filesystem location to cache version')
    parser.add_argument('-c', "--cache-path", required=False,
                        help='path for cached files to clear on version mismatch')
    parser.add_argument('-s', "--sha256", required=False,
                        help='expected SHA-256 hash of the downloaded file')
    args = parser.parse_args()

    version_from_file = ''
    version_file = pathlib.Path(args.version_file)
    if version_file.exists():
        with version_file.open('r') as f:
            version_from_file = f.readline().strip()

    if version_from_file == args.version:
        return 0

    # Fresh build or version mismatch, delete old cache
    if args.cache_path:
        cache_path = pathlib.Path(args.cache_path)
        shutil.rmtree(cache_path, ignore_errors=True)
        cache_path.mkdir(parents=True)

    output_file = pathlib.Path(args.output)
    print(f"Downloading file {output_file} from {args.url}")

    with urllib.request.urlopen(args.url) as f:
        try:
            with tempfile.NamedTemporaryFile(delete=False, dir=output_file.parent) as out:
                out.write(f.read())
            os.rename(out.name, output_file)
        except IOError:
            os.unlink(out.name)

    if args.sha256:
        actual_sha256 = compute_sha256(output_file)

        if args.sha256 != actual_sha256:
            print(f"SHA-256 mismatch for downloaded file {output_file}")
            print(f"Expected: {args.sha256}")
            print(f"Actual:   {actual_sha256}")
            return 1

    with open(version_file, 'w') as f:
        f.write(args.version)


if __name__ == '__main__':
    sys.exit(main())
