#!/usr/bin/env python3

import os
import sys

from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urljoin
from urllib.request import urlopen
from collections import namedtuple
from enum import Enum
import re

wpt_base_url = 'https://wpt.live/'


class TestType(Enum):
    TEXT = 1, 'Tests/LibWeb/Text/input/wpt-import', 'Tests/LibWeb/Text/expected/wpt-import'
    REF = 2, 'Tests/LibWeb/Ref/input/wpt-import', 'Tests/LibWeb/Ref/expected/wpt-import'

    def __new__(cls, *args, **kwds):
        obj = object.__new__(cls)
        obj._value_ = args[0]
        return obj

    def __init__(self, _: str, input_path: str, expected_path: str):
        self.input_path = input_path
        self.expected_path = expected_path


PathMapping = namedtuple('PathMapping', ['source', 'destination'])

test_type = TestType.TEXT
raw_reference_path = None  # As specified in the test HTML
reference_path = None  # With parent directories
src_values = []


class LinkedResourceFinder(HTMLParser):
    def __init__(self):
        super().__init__()
        self._tag_stack_ = []
        self._match_css_url_ = re.compile(r"url\(\"?(?P<url>[^\")]+)\"?\)")
        self._match_css_import_string_ = re.compile(r"@import\s+\"(?P<url>[^\")]+)\"")

    def handle_starttag(self, tag, attrs):
        self._tag_stack_.append(tag)
        if tag == "script":
            attr_dict = dict(attrs)
            if "src" in attr_dict:
                src_values.append(attr_dict["src"])
        if tag == "link":
            attr_dict = dict(attrs)
            if attr_dict["rel"] == "stylesheet":
                src_values.append(attr_dict["href"])

    def handle_endtag(self, tag):
        self._tag_stack_.pop()

    def handle_data(self, data):
        if self._tag_stack_ and self._tag_stack_[-1] == "style":
            # Look for uses of url()
            url_iterator = self._match_css_url_.finditer(data)
            for match in url_iterator:
                src_values.append(match.group("url"))
            # Look for @imports that use plain strings - we already found the url() ones
            import_iterator = self._match_css_import_string_.finditer(data)
            for match in import_iterator:
                src_values.append(match.group("url"))


class TestTypeIdentifier(HTMLParser):
    """Identifies what kind of test the page is, and stores it in self.test_type
    For reference tests, the URL of the reference page is saved as self.reference_path
    """

    def __init__(self, url):
        super().__init__()
        self.url = url
        self.test_type = TestType.TEXT
        self.reference_path = None

    def handle_starttag(self, tag, attrs):
        if tag == "link":
            attr_dict = dict(attrs)
            if attr_dict["rel"] == "match":
                self.test_type = TestType.REF
                self.reference_path = attr_dict["href"]


def map_to_path(sources, is_resource=True, resource_path=None):
    if is_resource:
        # Add it as a sibling path if it's a relative resource
        sibling_location = Path(resource_path).parent.__str__()
        sibling_import_path = test_type.input_path + '/' + sibling_location

        def remapper(x):
            if x.startswith('/'):
                return test_type.input_path + x
            return sibling_import_path + '/' + x

        filepaths = list(map(remapper, sources))
        filepaths = list(map(lambda x: Path(x), filepaths))
    else:
        # Add the test_type.input_path to the sources if root files
        def remapper(x):
            if x.startswith('/'):
                return test_type.input_path + x
            return test_type.input_path + '/' + x

        filepaths = list(map(lambda x: Path(remapper(x)), sources))

    # Map to source and destination
    def path_mapper(x):
        output_path = wpt_base_url + x.__str__().replace(test_type.input_path, '')
        return PathMapping(output_path, x.absolute())

    filepaths = list(map(path_mapper, filepaths))

    return filepaths


def modify_sources(files):
    for file in files:
        # Get the distance to the wpt-imports folder
        folder_index = str(file).find(test_type.input_path)
        if folder_index == -1:
            folder_index = str(file).find(test_type.expected_path)
            non_prefixed_path = str(file)[folder_index + len(test_type.expected_path):]
        else:
            non_prefixed_path = str(file)[folder_index + len(test_type.input_path):]

        parent_folder_count = len(Path(non_prefixed_path).parent.parts) - 1
        parent_folder_path = '../' * parent_folder_count

        with open(file, 'r') as f:
            page_source = f.read()

        # Iterate all scripts and overwrite the src attribute
        for i, src_value in enumerate(src_values):
            if src_value.startswith('/'):
                new_src_value = parent_folder_path + src_value[1::]
                page_source = page_source.replace(src_value, new_src_value)

        # Look for mentions of the reference page, and update their href
        if raw_reference_path is not None:
            new_reference_path = parent_folder_path + '../../expected/wpt-import/' + reference_path[::]
            page_source = page_source.replace(raw_reference_path, new_reference_path)

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
    # Ref tests don't have an expectation text file
    if test_type == TestType.REF:
        return

    for file in files:
        new_path = str(file.destination).replace(test_type.input_path, test_type.expected_path)
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

    with urlopen(url_to_import) as response:
        page = response.read().decode("utf-8")

    global test_type, reference_path, raw_reference_path
    identifier = TestTypeIdentifier(url_to_import)
    identifier.feed(page)
    test_type = identifier.test_type
    raw_reference_path = identifier.reference_path
    print(f"Identified {url_to_import} as type {test_type}, ref {raw_reference_path}")

    main_file = [resource_path]
    main_paths = map_to_path(main_file, False)

    if test_type == TestType.REF and raw_reference_path is None:
        raise RuntimeError('Failed to file reference path in ref test')

    if raw_reference_path is not None:
        if raw_reference_path.startswith('/'):
            reference_path = raw_reference_path
            main_paths.append(PathMapping(
                wpt_base_url + raw_reference_path,
                Path(test_type.expected_path + raw_reference_path).absolute()
            ))
        else:
            reference_path = Path(resource_path).parent.joinpath(raw_reference_path).__str__()
            main_paths.append(PathMapping(
                wpt_base_url + '/' + reference_path,
                Path(test_type.expected_path + '/' + reference_path).absolute()
            ))

    files_to_modify = download_files(main_paths)
    create_expectation_files(main_paths)

    parser = LinkedResourceFinder()
    parser.feed(page)

    modify_sources(files_to_modify)
    script_paths = map_to_path(src_values, True, resource_path)
    download_files(script_paths)


if __name__ == "__main__":
    main()
