#!/usr/bin/env python3

import os
import sys
from pathlib import Path
from bs4 import BeautifulSoup
from urllib.request import urlopen
from collections import namedtuple

wpt_base_url = 'https://wpt.live/'
wpt_import_path = 'Tests/LibWeb/Text/input/wpt-import'
wpt_expected_path = 'Tests/LibWeb/Text/expected/wpt-import'
PathMapping = namedtuple('PathMapping', ['source', 'destination'])


def get_script_sources(page_source):
    # Find all the <script> tags
    scripts = [script for script in page_source.findAll('script')]

    # Get the src attribute of each script tag
    sources = list(map(lambda x: x.get('src'), scripts))

    # Remove None values
    sources = list(filter(lambda x: x is not None, sources))

    return sources


def map_to_path(sources, is_resource=True, resource_path=None):
    if is_resource:
        # Add it as a sibling path if it's a relative resource
        sibling_location = Path(resource_path).parent.__str__()
        sibling_import_path = wpt_import_path + '/' + sibling_location

        def remapper(x):
            if x.startswith('/'):
                return wpt_import_path + x
            return sibling_import_path + '/' + x

        filepaths = list(map(remapper, sources))
        filepaths = list(map(lambda x: Path(x), filepaths))
    else:
        # Add the wpt_import_path to the sources if root files
        def remapper(x):
            return wpt_import_path + '/' + x

        filepaths = list(map(lambda x: Path(remapper(x)), sources))

    # Map to source and destination
    def path_mapper(x):
        output_path = wpt_base_url + x.__str__().replace(wpt_import_path, '')
        return PathMapping(output_path, x.absolute())
    filepaths = list(map(path_mapper, filepaths))

    return filepaths


def modify_sources(files):
    for file in files:
        # Get the distance to the wpt-imports folder
        folder_index = str(file).find(wpt_import_path)
        non_prefixed_path = str(file)[folder_index + len(wpt_import_path):]
        parent_folder_count = len(Path(non_prefixed_path).parent.parts) - 1
        parent_folder_path = '../' * parent_folder_count

        with open(file, 'r') as f:
            page_source = BeautifulSoup(f.read(), 'html.parser')

            # Iterate all scripts and overwrite the src attribute
            scripts = [script for script in page_source.findAll('script')]
            for script in scripts:
                if script.get('src') is not None:
                    if script['src'].startswith('/'):
                        script['src'] = parent_folder_path + script['src'][1::]

            with open(file, 'w') as f:
                f.write(str(page_source))


def download_files(filepaths):
    downloaded_files = []

    for file in filepaths:
        if (file.destination.exists()):
            print(f"Skipping {file.destination} as it already exists")
            continue

        print(f"Downloading {file.source} to {file.destination}")

        connection = urlopen(file.source)
        if connection.status != 200:
            print(f"Failed to download {file.source}")
            continue

        os.makedirs(file.destination.parent, exist_ok=True)

        with open(file.destination, 'wb') as f:
            f.write(connection.read())

            downloaded_files.append(file.destination)

    return downloaded_files


def create_expectation_files(files):
    for file in files:
        new_path = str(file.destination).replace(wpt_import_path, wpt_expected_path)
        new_path = new_path.rsplit(".", 1)[0] + '.txt'

        expected_file = Path(new_path)
        if expected_file.exists():
            print(f"Skipping {expected_file} as it already exists")
            continue

        os.makedirs(expected_file.parent, exist_ok=True)
        expected_file.touch()


def main():
    if len(sys.argv) != 2:
        print("Usage: import-wpt-test.py <url>")
        return

    url_to_import = sys.argv[1]
    resource_path = '/'.join(Path(url_to_import).parts[2::])

    main_file = [resource_path]
    main_paths = map_to_path(main_file, False)
    files_to_modify = download_files(main_paths)
    create_expectation_files(main_paths)
    modify_sources(files_to_modify)

    page = urlopen(url_to_import)
    page_source = BeautifulSoup(page, 'html.parser')

    scripts = get_script_sources(page_source)
    script_paths = map_to_path(scripts, True, resource_path)
    download_files(script_paths)


if __name__ == "__main__":
    main()
