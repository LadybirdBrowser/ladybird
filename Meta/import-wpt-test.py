#!/usr/bin/env python3

import os
import sys

from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urljoin
from urllib.request import urlopen
from collections import namedtuple

wpt_base_url = 'https://wpt.live/'
wpt_import_path = 'Tests/LibWeb/Text/input/wpt-import'
wpt_expected_path = 'Tests/LibWeb/Text/expected/wpt-import'
PathMapping = namedtuple('PathMapping', ['source', 'destination'])

src_values = []


class ScriptSrcValueFinder(HTMLParser):

    def handle_starttag(self, tag, attrs):
        if tag == "script":
            attr_dict = dict(attrs)
            if "src" in attr_dict:
                src_values.append(attr_dict["src"])


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
            page_source = f.read()

        page_source = page_source.replace('/fonts/ahem.css', '../' * parent_folder_count + 'fonts/ahem.css')

        # Iterate all scripts and overwrite the src attribute
        for i, src_value in enumerate(src_values):
            if src_value.startswith('/'):
                new_src_value = parent_folder_path + src_value[1::]
                page_source = page_source.replace(src_value, new_src_value)
                with open(file, 'w') as f:
                    f.write(str(page_source))


def download_files(filepaths):
    downloaded_files = []

    for file in filepaths:
        source = urljoin(file.source, "/".join(file.source.split('/')[3:]))
        destination = Path(os.path.normpath(file.destination))

        if destination.exists():
            print(f"Skipping {destination} as it already exists")
            continue

        print(f"Downloading {source} to {destination}")

        connection = urlopen(source)
        if connection.status != 200:
            print(f"Failed to download {file.source}")
            continue

        os.makedirs(destination.parent, exist_ok=True)

        with open(destination, 'wb') as f:
            f.write(connection.read())

            downloaded_files.append(destination)

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

    with urlopen(url_to_import) as response:
        page = response.read().decode("utf-8")

    parser = ScriptSrcValueFinder()
    parser.feed(page)

    modify_sources(files_to_modify)
    script_paths = map_to_path(src_values, True, resource_path)
    download_files(script_paths)


if __name__ == "__main__":
    main()
