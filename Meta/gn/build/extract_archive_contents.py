#!/usr/bin/env python3
r"""Extracts files from an archive for use in the build

It's intended to be used for files that are cached between runs.

"""

import argparse
import pathlib
import tarfile
import zipfile
import sys


def extract_member(file, destination, path):
    """
    Extract a single file from a ZipFile or TarFile

    :param ZipFile|TarFile file: Archive object to extract from
    :param Path destination: Location to write the file
    :param str path: Filename to extract from archive.
    """
    destination_path = destination / path
    if destination_path.exists():
        return
    destination_path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(file, tarfile.TarFile):
        with file.extractfile(path) as member:
            destination_path.write_text(member.read().decode("utf-8"))
    else:
        assert isinstance(file, zipfile.ZipFile)
        with file.open(path) as member:
            destination_path.write_text(member.read().decode("utf-8"))


def extract_directory(file, destination, path):
    """
    Extract a directory from a ZipFile or TarFile

    :param ZipFile|TarFile file: Archive object to extract from
    :param Path destination: Location to write the files
    :param str path: Directory name to extract from archive.
    """
    destination_path = destination / path
    if destination_path.exists():
        return
    destination_path.mkdir(parents=True, exist_ok=True)
    if not isinstance(file, zipfile.ZipFile):
        raise NotImplementedError

    # FIXME: This loops over the entire archive len(args.paths) times. Decrease complexity
    for entry in file.namelist():
        if entry.startswith(path):
            file.extract(entry, destination)


def main():
    parser = argparse.ArgumentParser(epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive", help="input archive")
    parser.add_argument("paths", nargs="*", help="paths to extract from the archive")
    parser.add_argument("-s", "--stamp", required=False, help="stamp file name to create after operation is done")
    parser.add_argument("-d", "--destination", required=True, help="directory to write the extracted file to")
    args = parser.parse_args()

    archive = pathlib.Path(args.archive)
    destination = pathlib.Path(args.destination)

    def extract_paths(file, paths):
        for path in paths:
            if path.endswith("/"):
                extract_directory(file, destination, path)
            else:
                extract_member(file, destination, path)

    if tarfile.is_tarfile(archive):
        with tarfile.open(archive) as f:
            extract_paths(f, args.paths)
    elif zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as f:
            extract_paths(f, args.paths)
    else:
        print(f"Unknown file type for {archive}, unable to extract {args.path}")
        return 1

    if args.stamp:
        pathlib.Path(args.stamp).touch()

    return 0


if __name__ == "__main__":
    sys.exit(main())
