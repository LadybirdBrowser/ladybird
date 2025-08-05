#!/usr/bin/env python3

import argparse
import os
import re

from collections import namedtuple
from dataclasses import dataclass
from enum import Enum
from html.parser import HTMLParser
from pathlib import Path
from posixpath import normpath
from urllib.parse import urljoin
from urllib.parse import urlparse
from urllib.parse import urlsplit
from urllib.parse import urlunsplit
from urllib.request import urlopen

download_exclude_list = {
    # This relies on a dynamic path that cannot be rewitten by the importer.
    "/resources/idlharness.js",
    # This file has been modified to improve the performance of our test runner.
    "/resources/testharness.js",
    # We have modified this file to output text files compatible with our test runner.
    "/resources/testharnessreport.js",
    # We have modified this file to use our internals object instead of relying on WebDriver.
    "/resources/testdriver-vendor.js",
    # Modified to use a relative path, which the importer can't rewrite.
    "/fonts/ahem.css",
}
visited_paths = set()


class TestType(Enum):
    TEXT = 1, "Tests/LibWeb/Text/input/wpt-import", "Tests/LibWeb/Text/expected/wpt-import"
    REF = 2, "Tests/LibWeb/Ref/input/wpt-import", "Tests/LibWeb/Ref/expected/wpt-import"
    CRASH = 3, "Tests/LibWeb/Crash/wpt-import", ""

    def __new__(cls, *args, **kwds):
        obj = object.__new__(cls)
        obj._value_ = args[0]
        return obj

    def __init__(self, _: str, input_path: str, expected_path: str):
        self.input_path = input_path
        self.expected_path = expected_path


PathMapping = namedtuple("PathMapping", ["source", "destination"])


class ResourceType(Enum):
    INPUT = 1
    EXPECTED = 2


@dataclass
class ResourceAndType:
    resource: str
    type: ResourceType


test_type = TestType.TEXT
raw_reference_path = None  # As specified in the test HTML
reference_path = None  # With parent directories


class LinkedResourceFinder(HTMLParser):
    def __init__(self):
        super().__init__()
        self._tag_stack_ = []
        self._match_css_url_ = re.compile(r"url\(['\"]?(?P<url>[^'\")]+)['\"]?\)")
        self._match_css_import_string_ = re.compile(r"@import\s+\"(?P<url>[^\")]+)\"")
        self._match_worker_import_path = re.compile(r"Worker\(\"(?P<url>.*)\"\)")
        self._resources = []

    @property
    def resources(self):
        return self._resources

    def handle_starttag(self, tag, attrs):
        self._tag_stack_.append(tag)
        if ":" in tag:
            tag = tag.split(":", 1)[-1]
        if tag in ["script", "img", "iframe"]:
            attr_dict = dict(attrs)
            if "src" in attr_dict:
                self._resources.append(attr_dict["src"])
        if tag == "link":
            attr_dict = dict(attrs)
            if "rel" in attr_dict and attr_dict["rel"] == "stylesheet":
                self._resources.append(attr_dict["href"])
        if tag == "form":
            attr_dict = dict(attrs)
            if "action" in attr_dict:
                self._resources.append(attr_dict["action"])

    def handle_endtag(self, tag):
        self._tag_stack_.pop()

    def handle_data(self, data):
        if self._tag_stack_ and self._tag_stack_[-1] == "style":
            # Look for uses of url()
            url_iterator = self._match_css_url_.finditer(data)
            for match in url_iterator:
                self._resources.append(match.group("url"))
            # Look for @imports that use plain strings - we already found the url() ones
            import_iterator = self._match_css_import_string_.finditer(data)
            for match in import_iterator:
                self._resources.append(match.group("url"))
        elif self._tag_stack_ and self._tag_stack_[-1] == "script":
            # Look for uses of Worker()
            filepath_iterator = self._match_worker_import_path.finditer(data)
            for match in filepath_iterator:
                self._resources.append(match.group("url"))


class TestTypeIdentifier(HTMLParser):
    """Identifies what kind of test the page is, and stores it in self.test_type
    For reference tests, the URL of the reference page is saved as self.reference_path
    """

    def __init__(self, url):
        super().__init__()
        self.url = url
        self.test_type = TestType.TEXT
        self.reference_path = None
        self.ref_test_link_found = False

    def handle_starttag(self, tag, attrs):
        if ":" in tag:
            tag = tag.split(":", 1)[-1]
        if tag == "link":
            attr_dict = dict(attrs)
            if "rel" in attr_dict and (attr_dict["rel"] == "match" or attr_dict["rel"] == "mismatch"):
                if self.ref_test_link_found:
                    raise RuntimeError("Ref tests with multiple match or mismatch links are not currently supported")
                self.test_type = TestType.REF
                self.reference_path = attr_dict["href"]
                self.ref_test_link_found = True


def map_to_path(
    sources: list[ResourceAndType], wpt_base_url: str, is_resource=True, resource_path=None
) -> list[PathMapping]:
    filepaths: list[PathMapping] = []

    for source in sources:
        base_directory = test_type.input_path if source.type == ResourceType.INPUT else test_type.expected_path

        if source.resource.startswith("/") or not is_resource:
            file_path = Path(base_directory, source.resource.lstrip("/"))
        else:
            # Add it as a sibling path if it's a relative resource
            sibling_location = Path(resource_path).parent
            parent_directory = Path(base_directory, sibling_location)

            file_path = Path(parent_directory, source.resource)
        # Map to source and destination
        output_path = wpt_base_url + str(file_path).replace(base_directory, "")

        filepaths.append(PathMapping(output_path, file_path.absolute()))

    return filepaths


def is_crash_test(url_string):
    # https://web-platform-tests.org/writing-tests/crashtest.html
    # A test file is treated as a crash test if they have -crash in their name before the file extension, or they are
    # located in a folder named crashtests
    parsed_url = urlparse(url_string)
    path_segments = parsed_url.path.strip("/").split("/")
    if len(path_segments) > 1 and "crashtests" in path_segments[::-1]:
        return True
    file_name = path_segments[-1]
    file_name_parts = file_name.split(".")
    if len(file_name_parts) > 1 and any([part.endswith("-crash") for part in file_name_parts[:-1]]):
        return True
    return False


def modify_sources(files, resources: list[ResourceAndType]) -> None:
    for file in files:
        # Get the distance to the wpt-imports folder
        folder_index = str(file).find(test_type.input_path)
        if folder_index == -1:
            folder_index = str(file).find(test_type.expected_path)
            non_prefixed_path = str(file)[folder_index + len(test_type.expected_path) :]
        else:
            non_prefixed_path = str(file)[folder_index + len(test_type.input_path) :]

        parent_folder_count = len(Path(non_prefixed_path).parent.parts) - 1
        parent_folder_path = "../" * parent_folder_count

        with open(file, "r") as f:
            page_source = f.read()

        # Iterate all scripts and overwrite the src attribute
        for resource in map(lambda r: r.resource, resources):
            if resource.startswith("/"):
                new_src_value = parent_folder_path + resource[1::]
                page_source = page_source.replace(resource, new_src_value)

        # Look for mentions of the reference page, and update their href
        if raw_reference_path is not None:
            new_reference_path = parent_folder_path + "../../expected/wpt-import/" + reference_path[::]
            page_source = page_source.replace(raw_reference_path, new_reference_path)

        with open(file, "w") as f:
            f.write(str(page_source))


def normalize_url(url):
    parts = urlsplit(url)
    normalized_path = normpath(parts.path)
    normalized_url = urlunsplit((parts.scheme, parts.netloc, normalized_path, parts.query, parts.fragment))
    return normalized_url


def remove_repeated_url_slashes(url):
    parsed = urlparse(url)
    return "/" + "/".join(segment for segment in parsed.path.split("/") if segment)


def download_files(filepaths, wpt_base_url, skip_existing):
    downloaded_files = []

    for file in filepaths:
        normalized_path = remove_repeated_url_slashes(file.source)
        print(f"Source {normalized_path}, Destination {file.destination}")
        if normalized_path in visited_paths:
            continue
        if normalized_path in download_exclude_list:
            print(f"Skipping {file.source} as it is in the exclude list")
            visited_paths.add(normalized_path)
            continue

        source = urljoin(wpt_base_url, normalized_path)
        destination = Path(os.path.normpath(file.destination))

        if skip_existing and destination.exists():
            print(f"Skipping {destination} as it already exists")
            visited_paths.add(normalized_path)
            continue

        visited_paths.add(normalized_path)

        print(f"Downloading {source} to {destination}")

        connection = urlopen(source)
        if connection.status != 200:
            print(f"Failed to download {file.source}")
            continue

        os.makedirs(destination.parent, exist_ok=True)

        with open(destination, "wb") as f:
            f.write(connection.read())

            downloaded_files.append(destination)

    return downloaded_files


def create_expectation_files(files, skip_existing):
    # Ref tests don't have an expectation text file
    if test_type in [TestType.REF, TestType.CRASH]:
        return

    for file in files:
        new_path = str(file.destination).replace(test_type.input_path, test_type.expected_path)
        new_path = new_path.rsplit(".", 1)[0] + ".txt"

        expected_file = Path(new_path)
        if skip_existing and expected_file.exists():
            print(f"Skipping {expected_file} as it already exists")
            continue

        os.makedirs(expected_file.parent, exist_ok=True)
        expected_file.touch()


def main():
    parser = argparse.ArgumentParser(description="Import a WPT test into LibWeb")
    parser.add_argument("--force", action="store_true", help="Force download of files even if they already exist")
    parser.add_argument("--wpt-base-url", type=urlparse, default="https://wpt.live/")
    parser.add_argument("url", type=str, help="The URL of the WPT test to import")
    args = parser.parse_args()

    skip_existing = not args.force
    url_to_import = args.url
    wpt_base_url = args.wpt_base_url.geturl()
    print(f"Importing WPT test from: {wpt_base_url}")
    resource_path = "/".join(Path(url_to_import).parts[2::])

    with urlopen(url_to_import) as response:
        page = response.read().decode("utf-8")

    global test_type, reference_path, raw_reference_path
    if is_crash_test(url_to_import):
        test_type = TestType.CRASH
    else:
        identifier = TestTypeIdentifier(url_to_import)
        identifier.feed(page)
        test_type = identifier.test_type
        raw_reference_path = identifier.reference_path

    print(f"Identified {url_to_import} as type {test_type}, ref {raw_reference_path}")

    main_file = [ResourceAndType(resource_path, ResourceType.INPUT)]
    main_paths = map_to_path(main_file, wpt_base_url, False)

    if test_type == TestType.REF and raw_reference_path is None:
        raise RuntimeError("Failed to file reference path in ref test")

    if raw_reference_path is not None:
        if raw_reference_path.startswith("/"):
            reference_path = raw_reference_path
            main_paths.append(
                PathMapping(
                    normalize_url(wpt_base_url + raw_reference_path),
                    Path(test_type.expected_path + raw_reference_path).absolute(),
                )
            )
        else:
            reference_path = Path(resource_path).parent.joinpath(raw_reference_path).__str__()
            main_paths.append(
                PathMapping(
                    normalize_url(wpt_base_url + "/" + reference_path),
                    Path(test_type.expected_path + "/" + reference_path).absolute(),
                )
            )

    files_to_modify = download_files(main_paths, wpt_base_url, skip_existing)
    create_expectation_files(main_paths, skip_existing)

    input_parser = LinkedResourceFinder()
    input_parser.feed(page)
    additional_resources = list(map(lambda s: ResourceAndType(s, ResourceType.INPUT), input_parser.resources))

    expected_parser = LinkedResourceFinder()
    for path in main_paths[1:]:
        with urlopen(path.source) as response:
            page = response.read().decode("utf-8")
            expected_parser.feed(page)
    additional_resources.extend(
        list(map(lambda s: ResourceAndType(s, ResourceType.EXPECTED), expected_parser.resources))
    )

    modify_sources(files_to_modify, additional_resources)
    script_paths = map_to_path(additional_resources, wpt_base_url, True, resource_path)
    download_files(script_paths, wpt_base_url, skip_existing)


if __name__ == "__main__":
    main()
