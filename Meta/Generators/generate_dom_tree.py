#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re

from html.parser import HTMLParser
from typing import Dict
from typing import List
from typing import Optional
from typing import Set
from typing import Tuple

# HTML void elements that cannot have children
HTML_VOID_ELEMENTS = frozenset(
    [
        "area",
        "base",
        "br",
        "col",
        "embed",
        "hr",
        "img",
        "input",
        "link",
        "meta",
        "param",
        "source",
        "track",
        "wbr",
    ]
)


def parse_html_tag_header(path: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    pattern = re.compile(r'__ENUMERATE_HTML_TAG\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)')
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                result[m.group(2)] = m.group(1)
    return result


def parse_html_attribute_header(path: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    pattern = re.compile(r'__ENUMERATE_HTML_ATTRIBUTE\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)')
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                result[m.group(2)] = m.group(1)
    return result


def parse_svg_tag_header(path: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    pattern = re.compile(r"__ENUMERATE_SVG_TAG\(\s*(\w+)\s*\)")
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                result[m.group(1).lower()] = m.group(1)
    return result


def parse_svg_attribute_header(path: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    pattern = re.compile(r'__ENUMERATE_SVG_ATTRIBUTE\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)')
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                result[m.group(2).lower()] = m.group(1)
    return result


non_identifier_character_regex = re.compile(r"\W+")


def to_snake_case(string: str) -> str:
    return non_identifier_character_regex.sub("_", string)


def to_pascal_case(string: str) -> str:
    return non_identifier_character_regex.sub(" ", string).title().replace(" ", "")


def resolve_tag(tag_name: str, in_svg: bool, html_tags: Dict[str, str], svg_tags: Dict[str, str]) -> Tuple[str, str]:
    if in_svg and tag_name in svg_tags:
        return "Namespace::SVG", f"SVG::TagNames::{svg_tags[tag_name]}"

    assert tag_name in html_tags
    return "Namespace::HTML", f"HTML::TagNames::{html_tags[tag_name]}"


def resolve_attribute(
    attribute_name: str, in_svg: bool, html_attributes: Dict[str, str], svg_attributes: Dict[str, str]
) -> str:
    if in_svg and attribute_name in svg_attributes:
        return f"SVG::AttributeNames::{svg_attributes[attribute_name]}"

    assert attribute_name in html_attributes
    return f"HTML::AttributeNames::{html_attributes[attribute_name]}"


def bits_for_count(count: int) -> int:
    bits = 8
    while bits < 64:
        if count < 2 ^ bits:
            return bits
        bits <<= 1
    return bits


class DOMElement:
    def __init__(self, var_name: str, tag: str, option: Optional[str]):
        self.var_name = var_name
        self.tag = tag
        self.option = option


class DOMTreeParser(HTMLParser):
    def __init__(
        self,
        struct_name: str,
        html_tags: Dict[str, str],
        html_attributes: Dict[str, str],
        svg_tags: Dict[str, str],
        svg_attributes: Dict[str, str],
        input_dir: str,
    ):
        super().__init__(convert_charrefs=False)
        self.struct_name = struct_name
        self.html_tags = html_tags
        self.html_attributes = html_attributes
        self.svg_tags = svg_tags
        self.svg_attributes = svg_attributes
        self.input_dir = input_dir

        self.lines: List[str] = []
        self.named_fields: List[str] = []
        self.unnamed_element_counter = 0
        self.indentation = 0
        self.svg_depth = 0
        self.element_stack: List[DOMElement] = []
        self.has_svg = False
        self.stylesheet_sources: List[Tuple[str, str]] = []  # (var_name, css_content)
        self.options: Set[str] = set()

    def _next_var(self) -> str:
        self.unnamed_element_counter += 1
        return f"element_{self.unnamed_element_counter}"

    def _current_parent(self) -> str:
        if self.element_stack:
            return self.element_stack[-1].var_name
        return "parent"

    def _deref_parent(self) -> str:
        current_parent = self._current_parent()
        if len(self.element_stack) > 0:
            return f"{current_parent}->"
        return f"{current_parent}."

    def _add_line(self, line: str) -> None:
        if not line.strip():
            self.lines.append("")
            return
        self.lines.append(("    " * self.indentation) + line)

    def handle_starttag(self, tag: str, attrs: List[Tuple[str, Optional[str]]]) -> None:
        self._handle_tag(tag, attrs, self_closing=False)

    def handle_startendtag(self, tag: str, attrs: List[Tuple[str, Optional[str]]]) -> None:
        self._handle_tag(tag, attrs, self_closing=True)

    def _handle_tag(self, tag: str, attrs: List[Tuple[str, Optional[str]]], self_closing: bool) -> None:
        attribute_dict = dict(attrs)

        # Wrap optional nodes in if statements
        option = attribute_dict.pop("data-option", None)
        if option:
            option = to_pascal_case(option)
            self._add_line(f"if (has_flag(options, {self.struct_name}::Options::{option})) {{")
            self.indentation += 1
            self.options.add(option)
        else:
            option = None

        # Handle SVG contexts
        in_svg = self.svg_depth > 0
        if tag == "svg":
            self.svg_depth += 1
            in_svg = True
            self.has_svg = True

        # Handle <link rel="stylesheet"> specially
        if tag == "link":
            assert attribute_dict.get("rel") is not None
            assert "href" in attribute_dict
            self._handle_stylesheet_link(attribute_dict)
            return

        # Determine variable name
        data_name = attribute_dict.pop("data-name", None)
        if data_name:
            var_name = to_snake_case(data_name)
            self.named_fields.append(var_name)
            is_named = True
        else:
            var_name = self._next_var()
            is_named = False

        namespace_expr, tag_expr = resolve_tag(tag, in_svg, self.html_tags, self.svg_tags)
        deref_parent = self._deref_parent()

        self._add_line(f"auto {var_name} = MUST(DOM::create_element(document, {tag_expr}, {namespace_expr}));")
        if is_named:
            self._add_line(f"this->{var_name} = {var_name};")

        # Set attributes (skip data-name)
        for attribute_name, attribute_value in attribute_dict.items():
            if attribute_value is None:
                attribute_value = ""
            attribute_expr = resolve_attribute(attribute_name, in_svg, self.html_attributes, self.svg_attributes)
            self._add_line(f'{var_name}->set_attribute_value({attribute_expr}, "{attribute_value}"_string);')

        self._add_line(f"MUST({deref_parent}append_child({var_name}));")

        self._add_line("")

        self.element_stack.append(DOMElement(var_name, tag, option))

        if self_closing or (tag in HTML_VOID_ELEMENTS and not in_svg):
            self.handle_endtag(tag)

    def handle_endtag(self, tag: str) -> None:
        if self.element_stack and self.element_stack[-1].tag == tag:
            node = self.element_stack.pop()

            if tag == "svg":
                self.svg_depth -= 1
            if node.option:
                self.indentation -= 1
                if not self.lines[-1]:
                    self.lines.pop()
                self._add_line(f"}} // Options::{node.option}")
                self._add_line("")

    def handle_data(self, data: str) -> None:
        if data.strip():
            deref_parent = self._deref_parent()
            self._add_line(f'MUST({deref_parent}set_text_content(R"~~~({data})~~~"_utf16));')
            self._add_line("")

    def handle_comment(self, data: str) -> None:
        pass

    def _handle_stylesheet_link(self, attribute_dict: Dict[str, Optional[str]]) -> None:
        href = attribute_dict["href"]
        assert href is not None
        css_path = os.path.join(self.input_dir, href)
        with open(css_path, "r", encoding="utf-8") as f:
            css_content = f.read()

        assert attribute_dict.get("data-name") is None
        var_name = self._next_var()

        source_var = f"s_stylesheet_source_{len(self.stylesheet_sources) + 1}"
        self.stylesheet_sources.append((source_var, css_content))

        deref_parent = self._deref_parent()

        self._add_line(
            f"auto {var_name} = MUST(DOM::create_element(document, HTML::TagNames::style, Namespace::HTML));"
        )
        self._add_line(f"MUST({var_name}->set_text_content(Utf16String::from_utf8({source_var})));")
        self._add_line(f"MUST({deref_parent}append_child({var_name}));")

        self._add_line("")


def generate(
    input_html: str,
    struct_name: str,
    namespace: str,
    header_path: str,
    html_tags_path: str,
    html_attributes_path: str,
    svg_tags_path: str,
    svg_attributes_path: str,
) -> Tuple[str, str]:
    html_tags = parse_html_tag_header(html_tags_path)
    html_attributes = parse_html_attribute_header(html_attributes_path)
    svg_tags = parse_svg_tag_header(svg_tags_path)
    svg_attributes = parse_svg_attribute_header(svg_attributes_path)

    input_dir = os.path.dirname(os.path.abspath(input_html))
    with open(input_html, "r", encoding="utf-8") as f:
        html_content = f.read()

    parser = DOMTreeParser(struct_name, html_tags, html_attributes, svg_tags, svg_attributes, input_dir)
    parser.feed(html_content)

    if not parser.lines[-1]:
        parser.lines.pop()

    # Generate header
    header_lines: List[str] = []
    header_lines.append("#pragma once")
    header_lines.append("")
    if parser.options:
        header_lines.append("#include <AK/EnumBits.h>")
    header_lines.append("#include <LibGC/Weak.h>")
    header_lines.append("#include <LibWeb/Forward.h>")
    header_lines.append("")
    header_lines.append(f"namespace {namespace} {{")
    header_lines.append("")
    header_lines.append(f"struct {struct_name} {{")

    if parser.options:
        header_lines.append(f"    enum class Options : u{bits_for_count(len(parser.options))} {{")
        header_lines.append("        None = 0,")
        option_bit = 0
        for option in parser.options:
            header_lines.append(f"        {option} = 1 << {option_bit},")
            option_bit += 1
        header_lines.append("    };")
        header_lines.append("")

    header_lines.append(
        f"    {struct_name}(DOM::Document&, DOM::Node& parent{(', Options = Options::None' if parser.options else '')});"
    )
    header_lines.append("")
    for field in parser.named_fields:
        header_lines.append(f"    GC::Weak<DOM::Element> {field};")
    header_lines.append("};")
    header_lines.append("")
    if parser.options:
        header_lines.append(f"AK_ENUM_BITWISE_OPERATORS({struct_name}::Options);")
        header_lines.append("")
    header_lines.append(f"}} // namespace {namespace}")
    header_lines.append("")

    # Generate implementation
    impl_lines: List[str] = []

    impl_lines.append("#include <LibWeb/DOM/Document.h>")
    impl_lines.append("#include <LibWeb/DOM/ElementFactory.h>")
    impl_lines.append("#include <LibWeb/HTML/AttributeNames.h>")
    impl_lines.append("#include <LibWeb/HTML/TagNames.h>")
    impl_lines.append("#include <LibWeb/Namespace.h>")
    if parser.has_svg:
        impl_lines.append("#include <LibWeb/SVG/AttributeNames.h>")
        impl_lines.append("#include <LibWeb/SVG/TagNames.h>")
    impl_lines.append("")

    impl_lines.append(f'#include "{header_path}"')
    impl_lines.append("")

    impl_lines.append(f"namespace {namespace} {{")
    impl_lines.append("")

    # Static stylesheet sources
    for source_var, css_content in parser.stylesheet_sources:
        impl_lines.append(f'static String {source_var} = R"~~~(')
        impl_lines.append(css_content.strip())
        impl_lines.append(')~~~"_string;')
        impl_lines.append("")

    # Constructor
    impl_lines.append(
        f"{struct_name}::{struct_name}(DOM::Document& document, DOM::Node& parent{', Options options' if parser.options else ''})"
    )
    impl_lines.append("{")
    for line in parser.lines:
        if line:
            impl_lines.append(f"    {line}")
        else:
            impl_lines.append("")
    impl_lines.append("}")
    impl_lines.append("")
    impl_lines.append(f"}} // namespace {namespace}")
    impl_lines.append("")

    return "\n".join(header_lines), "\n".join(impl_lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate C++ DOM tree from HTML template", add_help=False)
    parser.add_argument("-h", required=True, dest="header_output")
    parser.add_argument("-c", required=True, dest="impl_output")
    parser.add_argument("-i", required=True, dest="input_html")
    parser.add_argument("-s", required=True, dest="struct_name")
    parser.add_argument("-n", required=True, dest="namespace")
    parser.add_argument("--html-tags", required=True)
    parser.add_argument("--html-attributes", required=True)
    parser.add_argument("--svg-tags", required=True)
    parser.add_argument("--svg-attributes", required=True)
    args = parser.parse_args()

    header_path = os.path.relpath(args.header_output, os.path.dirname(str(args.impl_output)))
    if header_path.endswith(".tmp"):
        header_path = header_path[:-4]

    header, impl = generate(
        args.input_html,
        args.struct_name,
        args.namespace,
        header_path,
        args.html_tags,
        args.html_attributes,
        args.svg_tags,
        args.svg_attributes,
    )

    with open(args.header_output, "w", encoding="utf-8") as f:
        f.write(header)

    with open(args.impl_output, "w", encoding="utf-8") as f:
        f.write(impl)


if __name__ == "__main__":
    main()
