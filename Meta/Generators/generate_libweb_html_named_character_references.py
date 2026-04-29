#!/usr/bin/env python3

# Copyright (c) 2024, the SerenityOS developers.
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The goal is to encode the necessary data compactly while still allowing for fast matching of
# named character references, and taking full advantage of the note in the spec[1] that:
#
# > This list [of named character references] is static and will not be expanded or changed in the future.
#
# An overview of the approach taken (see [2] for more background/context):
#
# First, a deterministic acyclic finite state automaton (DAFSA) [3] is constructed from the set of
# named character references. The nodes in the DAFSA are populated with a "number" field that
# represents the count of all possible valid words from that node. This "number" field allows for
# minimal perfect hashing, where each word in the set corresponds to a unique index. The unique
# index of a word in the set is calculated during traversal/search of the DAFSA:
# - For any non-matching node that is iterated when searching a list of children, add their number
#   to the unique index
# - For nodes that match the current character, if the node is a valid end-of-word, add 1 to the
#   unique index
# Note that "searching a list of children" is assumed to use a linear scan, so, for example, if
# a list of children contained 'a', 'b', 'c', and 'd' (in that order), and the character 'c' was
# being searched for, then the "number" of both 'a' and 'b' would get added to the unique index,
# and then 1 would be added after matching 'c' (this minimal perfect hashing strategy comes from [4]).
#
# Something worth noting is that a DAFSA can be used with the set of named character references
# (with minimal perfect hashing) while keeping the nodes of the DAFSA <= 32-bits. This is a property
# that really matters, since any increase over 32-bits would immediately double the size of the data
# due to padding bits when storing the nodes in a contiguous array.
#
# There are also a few modifications made to the DAFSA to increase performance:
# - The 'first layer' of nodes is extracted out and replaced with a lookup table. This turns
#   the search for the first character from O(n) to O(1), and doesn't increase the data size because
#   all first characters in the set of named character references have the values 'a'-'z'/'A'-'Z',
#   so a lookup array of exactly 52 elements can be used. The lookup table stores the cumulative
#   "number" fields that would be calculated by a linear scan that matches a given node, thus allowing
#   the unique index to be built-up as normal with a O(1) search instead of a linear scan.
# - The 'second layer' of nodes is also extracted out and searches of the second layer are done
#   using a bit field of 52 bits (the set bits of the bit field depend on the first character's value),
#   where each set bit corresponds to one of 'a'-'z'/'A'-'Z' (similar to the first layer, the second
#   layer can only contain ASCII alphabetic characters). The bit field is then re-used (along with
#   an offset) to get the index into the array of second layer nodes. This technique ultimately
#   allows for storing the minimum number of nodes in the second layer, and therefore only increasing the
#   size of the data by the size of the 'first to second layer link' info which is 52 * 8 = 416 bytes.
# - After the second layer, the rest of the data is stored using a mostly-normal DAFSA, but there
#   are still a few differences:
#    - The "number" field is cumulative, in the same way that the first/second layer store a
#      cumulative "number" field. This cuts down slightly on the amount of work done during
#      the search of a list of children, and we can get away with it because the cumulative
#      "number" fields of the remaining nodes in the DAFSA (after the first and second layer
#      nodes were extracted out) happens to require few enough bits that we can store the
#      cumulative version while staying under our 32-bit budget.
#    - Instead of storing a 'last sibling' flag to denote the end of a list of children, the
#      length of each node's list of children is stored. Again, this is mostly done just because
#      there are enough bits available to do so while keeping the DAFSA node within 32 bits.
#    - Note: Together, these modifications open up the possibility of using a binary search instead
#      of a linear search over the children, but due to the consistently small lengths of the lists
#      of children in the remaining DAFSA, a linear search actually seems to be the better option.
#
# [1]: https://html.spec.whatwg.org/multipage/named-characters.html#named-character-references
# [2]: https://www.ryanliptak.com/blog/better-named-character-reference-tokenization/
# [3]: https://en.wikipedia.org/wiki/Deterministic_acyclic_finite_state_automaton
# [4]: Applications of finite automata representing large vocabularies (Cláudio L. Lucchesi,
#      Tomasz Kowaltowski, 1993) https://doi.org/10.1002/spe.4380230103

import argparse
import json
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))


SECOND_CODEPOINT_NAMES = {
    0x0338: "CombiningLongSolidusOverlay",
    0x20D2: "CombiningLongVerticalLineOverlay",
    0x200A: "HairSpace",
    0x0333: "CombiningDoubleLowLine",
    0x20E5: "CombiningReverseSolidusOverlay",
    0xFE00: "VariationSelector1",
    0x006A: "LatinSmallLetterJ",
    0x0331: "CombiningMacronBelow",
}


def get_second_codepoint_enum_name(codepoint: int) -> str:
    return SECOND_CODEPOINT_NAMES.get(codepoint, "None")


def is_ascii_alpha(c: int) -> bool:
    return (0x41 <= c <= 0x5A) or (0x61 <= c <= 0x7A)


def ascii_alphabetic_to_index(c: int) -> int:
    assert is_ascii_alpha(c)
    return (c - 0x41) if c <= 0x5A else (c - 0x61 + 26)


class Node:
    __slots__ = ("children", "is_terminal", "number")

    def __init__(self) -> None:
        self.children: list = [None] * 128
        self.is_terminal: bool = False
        self.number: int = 0

    def num_direct_children(self) -> int:
        return sum(1 for c in self.children if c is not None)

    def get_ascii_alphabetic_bit_mask(self) -> int:
        mask = 0
        for i, c in enumerate(self.children):
            if c is None:
                continue
            mask |= 1 << ascii_alphabetic_to_index(i)
        return mask


def calc_numbers(node: Node) -> None:
    node.number = 1 if node.is_terminal else 0
    for c in node.children:
        if c is None:
            continue
        calc_numbers(c)
        node.number += c.number


def node_key(n: Node) -> tuple:
    # Equality matches the C++ NodeTraits: identity of each child slot + terminal flag.
    return (tuple(id(c) if c is not None else 0 for c in n.children), n.is_terminal)


class DafsaBuilder:
    def __init__(self) -> None:
        self.root = Node()
        self.minimized_nodes: dict = {}  # node_key -> Node
        self.unchecked_nodes: list = []  # list of (parent, character_index, child)
        self.previous_word: str = ""

    def insert(self, s: str) -> None:
        assert s > self.previous_word, f"insertion order: {s!r} not > {self.previous_word!r}"

        common_prefix_len = 0
        for i in range(min(len(s), len(self.previous_word))):
            if s[i] != self.previous_word[i]:
                break
            common_prefix_len += 1

        self.minimize(common_prefix_len)

        if not self.unchecked_nodes:
            node = self.root
        else:
            node = self.unchecked_nodes[-1][2]

        for ch in s[common_prefix_len:]:
            c = ord(ch)
            assert node.children[c] is None
            child = Node()
            node.children[c] = child
            self.unchecked_nodes.append((node, c, child))
            node = child
        node.is_terminal = True

        self.previous_word = s

    def minimize(self, down_to: int) -> None:
        if not self.unchecked_nodes:
            return
        while len(self.unchecked_nodes) > down_to:
            parent, char_index, child = self.unchecked_nodes.pop()
            key = node_key(child)
            existing = self.minimized_nodes.get(key)
            if existing is not None:
                parent.children[char_index] = existing
            else:
                self.minimized_nodes[key] = child

    def calc_numbers(self) -> None:
        calc_numbers(self.root)

    def get_unique_index(self, s: str) -> int:
        index = 0
        node = self.root
        for ch in s:
            c = ord(ch)
            if node.children[c] is None:
                return -1
            for sibling_c in range(128):
                if node.children[sibling_c] is None:
                    continue
                if sibling_c < c:
                    index += node.children[sibling_c].number
            node = node.children[c]
            if node.is_terminal:
                index += 1
        return index


def queue_children(node: Node, queue: list, child_indexes: dict, first_available_index: int) -> int:
    current_available_index = first_available_index
    for c in range(128):
        child = node.children[c]
        if child is None:
            continue
        if id(child) not in child_indexes:
            child_num_children = child.num_direct_children()
            if child_num_children > 0:
                child_indexes[id(child)] = current_available_index
                current_available_index += child_num_children
            queue.append(child)
    return current_available_index


def write_children_data(
    node: Node, node_data: list, queue: list, child_indexes: dict, first_available_index: int
) -> int:
    current_available_index = first_available_index
    unique_index_tally = 0
    for c in range(128):
        child = node.children[c]
        if child is None:
            continue
        child_num_children = child.num_direct_children()

        if id(child) not in child_indexes:
            if child_num_children > 0:
                child_indexes[id(child)] = current_available_index
                current_available_index += child_num_children
            queue.append(child)

        node_data.append(
            (
                c,
                unique_index_tally,
                child.is_terminal,
                child_indexes.get(id(child), 0),
                child_num_children,
            )
        )

        unique_index_tally += child.number
    return current_available_index


def write_node_data(dafsa_builder: DafsaBuilder, node_data: list, child_indexes: dict) -> None:
    queue: list = []

    first_available_index = 1
    first_available_index = queue_children(dafsa_builder.root, queue, child_indexes, first_available_index)

    child_indexes.clear()
    first_available_index = 1
    second_layer_length = len(queue)
    head = 0
    for _ in range(second_layer_length):
        node = queue[head]
        head += 1
        first_available_index = queue_children(node, queue, child_indexes, first_available_index)

    while head < len(queue):
        node = queue[head]
        head += 1
        first_available_index = write_children_data(node, node_data, queue, child_indexes, first_available_index)


def write_header_file(out: TextIO) -> None:
    out.write("""
#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>

namespace Web::HTML {

// Uses u32 to match the `first` field of NamedCharacterReferenceCodepoints for bit-field packing purposes.
enum class NamedCharacterReferenceSecondCodepoint : u32 {
    None,
    CombiningLongSolidusOverlay, // U+0338
    CombiningLongVerticalLineOverlay, // U+20D2
    HairSpace, // U+200A
    CombiningDoubleLowLine, // U+0333
    CombiningReverseSolidusOverlay, // U+20E5
    VariationSelector1, // U+FE00
    LatinSmallLetterJ, // U+006A
    CombiningMacronBelow, // U+0331
};

inline Optional<u16> named_character_reference_second_codepoint_value(NamedCharacterReferenceSecondCodepoint codepoint)
{
    switch (codepoint) {
    case NamedCharacterReferenceSecondCodepoint::None:
        return {};
    case NamedCharacterReferenceSecondCodepoint::CombiningLongSolidusOverlay:
        return 0x0338;
    case NamedCharacterReferenceSecondCodepoint::CombiningLongVerticalLineOverlay:
        return 0x20D2;
    case NamedCharacterReferenceSecondCodepoint::HairSpace:
        return 0x200A;
    case NamedCharacterReferenceSecondCodepoint::CombiningDoubleLowLine:
        return 0x0333;
    case NamedCharacterReferenceSecondCodepoint::CombiningReverseSolidusOverlay:
        return 0x20E5;
    case NamedCharacterReferenceSecondCodepoint::VariationSelector1:
        return 0xFE00;
    case NamedCharacterReferenceSecondCodepoint::LatinSmallLetterJ:
        return 0x006A;
    case NamedCharacterReferenceSecondCodepoint::CombiningMacronBelow:
        return 0x0331;
    default:
        VERIFY_NOT_REACHED();
    }
}

// Note: The first codepoint could fit in 17 bits, and the second could fit in 4 (if unsigned).
// However, to get any benefit from minimizing the struct size, it would need to be accompanied by
// bit-packing the g_named_character_reference_codepoints_lookup array.
struct NamedCharacterReferenceCodepoints {
    u32 first : 24; // Largest value is U+1D56B
    NamedCharacterReferenceSecondCodepoint second : 8;
};
static_assert(sizeof(NamedCharacterReferenceCodepoints) == 4);

struct NamedCharacterReferenceFirstLayerNode {
    // Really only needs 12 bits.
    u16 number;
};
static_assert(sizeof(NamedCharacterReferenceFirstLayerNode) == 2);

struct NamedCharacterReferenceFirstToSecondLayerLink {
    u64 mask : 52;
    u64 second_layer_offset : 12;
};
static_assert(sizeof(NamedCharacterReferenceFirstToSecondLayerLink) == 8);

// Note: It is possible to fit this information within 24 bits, which could then allow for tightly
// bit-packing the second layer array. This would reduce the size of the array by 630 bytes.
struct NamedCharacterReferenceSecondLayerNode {
    // Could be 10 bits
    u16 child_index;
    u8 number;
    // Could be 4 bits
    u8 children_len : 7;
    bool end_of_word : 1;
};
static_assert(sizeof(NamedCharacterReferenceSecondLayerNode) == 4);

struct NamedCharacterReferenceNode {
    // The actual alphabet of characters used in the list of named character references only
    // includes 61 unique characters ('1'...'8', ';', 'a'...'z', 'A'...'Z').
    u8 character;
    // Typically, nodes are numbered with "an integer which gives the number of words that
    // would be accepted by the automaton starting from that state." This numbering
    // allows calculating "a one-to-one correspondence between the integers 1 to L
    // (L is the number of words accepted by the automaton) and the words themselves."
    //
    // This allows us to have a minimal perfect hashing scheme such that it's possible to store
    // and lookup the codepoint transformations of each named character reference using a separate
    // array.
    //
    // This uses that idea, but instead of storing a per-node number that gets built up while
    // searching a list of children, the cumulative number that would result from adding together
    // the numbers of all the previous sibling nodes is stored instead. This cuts down on a bit
    // of work done while searching while keeping the minimal perfect hashing strategy intact.
    //
    // Empirically, the largest number in our DAFSA is 51, so all number values could fit in a u6.
    u8 number : 7;
    bool end_of_word : 1;
    // Index of the first child of this node.
    // There are 3190 nodes in our DAFSA after the first and second layers were extracted out, so
    // all indexes can fit in a u12 (there would be 3872 nodes with the first/second layers
    // included, so still a u12).
    u16 child_index : 12;
    u16 children_len : 4;
};
static_assert(sizeof(NamedCharacterReferenceNode) == 4);

extern NamedCharacterReferenceNode g_named_character_reference_nodes[];
extern NamedCharacterReferenceFirstLayerNode g_named_character_reference_first_layer[];
extern NamedCharacterReferenceFirstToSecondLayerLink g_named_character_reference_first_to_second_layer[];
extern NamedCharacterReferenceSecondLayerNode g_named_character_reference_second_layer[];

Optional<NamedCharacterReferenceCodepoints> named_character_reference_codepoints_from_unique_index(u16 unique_index);

} // namespace Web::HTML

""")


def bool_str(b: bool) -> str:
    return "true" if b else "false"


def write_implementation_file(out: TextIO, named_character_reference_data: dict) -> None:
    dafsa_builder = DafsaBuilder()

    for key in named_character_reference_data.keys():
        dafsa_builder.insert(key[1:])
    dafsa_builder.minimize(0)
    dafsa_builder.calc_numbers()

    # Sanity check: the minimal perfect hashing must produce no collisions.
    index_set = set()
    for key in named_character_reference_data.keys():
        index = dafsa_builder.get_unique_index(key[1:])
        assert index not in index_set
        index_set.add(index)
    assert len(named_character_reference_data) == len(index_set)

    index_to_codepoints: list = [None] * len(named_character_reference_data)
    for key, value in named_character_reference_data.items():
        codepoints = value["codepoints"]
        unique_index = dafsa_builder.get_unique_index(key[1:])
        array_index = unique_index - 1
        second_codepoint = codepoints[1] if len(codepoints) == 2 else 0
        index_to_codepoints[array_index] = (codepoints[0], second_codepoint)

    out.write("""
#include <LibWeb/HTML/Parser/Entities.h>

namespace Web::HTML {

static NamedCharacterReferenceCodepoints g_named_character_reference_codepoints_lookup[] = {
""")

    for first, second in index_to_codepoints:
        out.write(
            f"    {{0x{first:X}, NamedCharacterReferenceSecondCodepoint::{get_second_codepoint_enum_name(second)}}},\n"
        )

    node_data: list = []
    child_indexes: dict = {}
    write_node_data(dafsa_builder, node_data, child_indexes)

    out.write("""};

NamedCharacterReferenceNode g_named_character_reference_nodes[] = {
    { 0, 0, false, 0, 0 },
""")

    for character, number, end_of_word, child_index, children_len in node_data:
        out.write(f"    {{ '{chr(character)}', {number}, {bool_str(end_of_word)}, {child_index}, {children_len} }},\n")

    out.write("""};

NamedCharacterReferenceFirstLayerNode g_named_character_reference_first_layer[] = {
""")

    num_children = dafsa_builder.root.num_direct_children()
    assert num_children == 52  # A-Z, a-z exactly
    unique_index_tally = 0
    for c in range(128):
        child = dafsa_builder.root.children[c]
        if child is None:
            continue
        assert is_ascii_alpha(c)
        out.write(f"    {{ {unique_index_tally} }},\n")
        unique_index_tally += child.number

    out.write("""};

NamedCharacterReferenceFirstToSecondLayerLink g_named_character_reference_first_to_second_layer[] = {
""")

    second_layer_offset = 0
    for c in range(128):
        child = dafsa_builder.root.children[c]
        if child is None:
            continue
        assert is_ascii_alpha(c)
        bit_mask = child.get_ascii_alphabetic_bit_mask()
        out.write(f"    {{ {bit_mask}ull, {second_layer_offset} }},\n")
        second_layer_offset += child.num_direct_children()

    out.write("""};

NamedCharacterReferenceSecondLayerNode g_named_character_reference_second_layer[] = {
""")

    for c in range(128):
        first_layer_node = dafsa_builder.root.children[c]
        if first_layer_node is None:
            continue
        assert is_ascii_alpha(c)

        local_unique_index_tally = 0
        for child_c in range(128):
            second_layer_node = first_layer_node.children[child_c]
            if second_layer_node is None:
                continue
            assert is_ascii_alpha(child_c)
            child_num_children = second_layer_node.num_direct_children()
            child_index = child_indexes.get(id(second_layer_node), 0)
            out.write(
                f"    {{ {child_index}, {local_unique_index_tally}, {child_num_children}, "
                f"{bool_str(second_layer_node.is_terminal)} }},\n"
            )
            local_unique_index_tally += second_layer_node.number

    out.write("""};

// Note: The unique index is 1-based.
Optional<NamedCharacterReferenceCodepoints> named_character_reference_codepoints_from_unique_index(u16 unique_index) {
    if (unique_index == 0) return {};
    return g_named_character_reference_codepoints_lookup[unique_index - 1];
}

} // namespace Web::HTML
""")


def main():
    parser = argparse.ArgumentParser(description="Generate Named Character References", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the Entities header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the Entities implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        named_character_reference_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, named_character_reference_data)


if __name__ == "__main__":
    main()
